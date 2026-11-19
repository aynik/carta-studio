#include "LongImportCheck.h"
#include "Core/Session.h"
#include "Audio/Transport.h"
#include <algorithm>
#include <vector>
#if JUCE_MAC || JUCE_LINUX
#include <sys/resource.h>
#endif
#include <chrono>
#include <cmath>
#include <iostream>

int runLongImportCheck() {
    constexpr int rate = 44100, frames = 80 * 60 * rate, dataBytes = frames * 4;
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("carta-long-import", {}, false);
    if (root.createDirectory().failed()) {
        std::cerr << "Could not create temporary test directory.\n";
        return 1;
    }
    struct Cleanup {
        juce::File root;
        ~Cleanup() { root.deleteRecursively(); }
    } cleanup{root};
    const auto source = root.getChildFile("source.wav"), pcmDir = root.getChildFile("pcm");
    if (pcmDir.createDirectory().failed()) {
        std::cerr << "Could not create PCM directory.\n";
        return 1;
    }
    {
        auto out = source.createOutputStream();
        if (!out || !out->write("RIFF", 4) || !out->writeInt(36 + dataBytes) ||
            !out->write("WAVEfmt ", 8) || !out->writeInt(16) || !out->writeShort(1) ||
            !out->writeShort(2) || !out->writeInt(rate) || !out->writeInt(rate * 4) ||
            !out->writeShort(4) || !out->writeShort(16) || !out->write("data", 4) ||
            !out->writeInt(dataBytes) || !out->setPosition(44LL + dataBytes - 1) ||
            !out->writeByte(0)) {
            std::cerr << "Could not create sparse WAV fixture.\n";
            return 1;
        }
        for (const int frame : {0, 65535, 65536, frames / 2, frames - 1}) {
            if (!out->setPosition(44LL + static_cast<juce::int64>(frame) * 4) ||
                !out->writeShort(8192) || !out->writeShort(-16384)) {
                std::cerr << "Could not write test impulses.\n";
                return 1;
            }
        }
        out->flush();
        if (out->getStatus().failed()) {
            std::cerr << "Could not flush test fixture.\n";
            return 1;
        }
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    juce::String error;
    double last = -1;
    bool monotonic = true;
    const auto start = std::chrono::steady_clock::now();
    auto audio = carta::loadAudio(
        formats, source, error,
        [&](double progress) {
            if (progress < last)
                monotonic = false;
            last = progress;
            return true;
        },
        0, pcmDir);
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!audio || !audio->mapped || audio->numSamples() != frames || audio->numChannels() != 2 ||
        !monotonic || std::abs(last - 1) > 1e-12) {
        std::cerr << "Long import failed: " << error << '\n';
        return 1;
    }
#if JUCE_MAC || JUCE_LINUX
    if (!pcmDir.findChildFiles(juce::File::findFilesAndDirectories, false).isEmpty()) {
        std::cerr << "Mapped PCM still has a name.\n";
        return 1;
    }
#endif
    for (const int frame : {0, 65535, 65536, frames / 2, frames - 1}) {
        if (std::abs(audio->channelData(0)[frame] - 0.25f) > 1e-6f ||
            std::abs(audio->channelData(1)[frame] + 0.5f) > 1e-6f) {
            std::cerr << "Long import changed impulse at " << frame << ".\n";
            return 1;
        }
    }
    if (std::abs(carta::waveformRange(*audio, 0, 0, frames).getEnd() - 0.25f) > 1e-6f ||
        std::abs(carta::waveformRange(*audio, 1, 0, frames).getStart() + 0.5f) > 1e-6f) {
        std::cerr << "Long waveform lost an impulse.\n";
        return 1;
    }
    carta::Session session;
    session.tracks.push_back({"Long source", {{audio, 0, 0, static_cast<double>(frames) / rate}}});
    juce::AudioBuffer<float> output(2, 1);
    output.clear();
    session.render(output, 0, 1, static_cast<double>(frames / 2) / rate, rate, 2);
    if (std::abs(output.getSample(0, 0) - 0.25f) > 1e-6f ||
        std::abs(output.getSample(1, 0) + 0.5f) > 1e-6f) {
        std::cerr << "Long mapped render mismatch.\n";
        return 1;
    }
    std::cout << "Imported 80-minute sparse stereo PCM16 WAV in " << seconds
              << " s; decoded mapped PCM " << static_cast<juce::int64>(frames) * 8
              << " bytes. Impulses, waveform and rendered midpoint matched. This is a synthetic "
                 "mostly silent source; no audio device was used.\n";
    return 0;
}

// Optional stress check: the sources span a full disc but only short regions
// are populated, keeping fixture creation and cleanup practical on a dev host.
int runLongMultitrackCheck() {
    using Clock = std::chrono::steady_clock;
    constexpr int rate = 44100, frames = 80 * 60 * rate, stripeFrames = 16384, period = 60 * rate,
                  block = 512, iterations = 320;
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("carta-long-multitrack", {}, false);
    if (root.createDirectory().failed()) {
        std::cerr << "Could not create temporary multitrack directory.\n";
        return 1;
    }
    struct Cleanup {
        juce::File root;
        ~Cleanup() { root.deleteRecursively(); }
    } cleanup{root};
    const auto pcmDir = root.getChildFile("pcm");
    if (pcmDir.createDirectory().failed())
        return 1;
    const auto sample = [rate, stripeFrames](int source, int channel, juce::int64 frame) -> int {
        const int sourceRate = source == 0 ? rate : 48000;
        if (frame < 0 || frame >= 80LL * 60 * sourceRate ||
            frame % (60LL * sourceRate) >= stripeFrames)
            return 0;
        return static_cast<int>((frame * 37 + source * 719 + channel * 1237) % 4097) - 2048;
    };
    std::vector<std::shared_ptr<carta::Audio>> sources;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    juce::String error;
    const auto beginImport = Clock::now();
    for (int sourceIndex = 0; sourceIndex < 2; ++sourceIndex) {
        const int sourceRate = sourceIndex == 0 ? rate : 48000;
        const int sourceFrames = 80 * 60 * sourceRate, dataBytes = sourceFrames * 4;
        const auto path = root.getChildFile("source-" + juce::String(sourceIndex) + ".wav");
        {
            auto out = path.createOutputStream();
            if (!out || !out->write("RIFF", 4) || !out->writeInt(36 + dataBytes) ||
                !out->write("WAVEfmt ", 8) || !out->writeInt(16) || !out->writeShort(1) ||
                !out->writeShort(2) || !out->writeInt(sourceRate) ||
                !out->writeInt(sourceRate * 4) || !out->writeShort(4) || !out->writeShort(16) ||
                !out->write("data", 4) || !out->writeInt(dataBytes) ||
                !out->setPosition(44LL + dataBytes - 1) || !out->writeByte(0)) {
                std::cerr << "Could not create sparse multitrack source.\n";
                return 1;
            }
            for (int minute = 0; minute < 80; ++minute) {
                const juce::int64 first = static_cast<juce::int64>(minute) * 60 * sourceRate;
                if (!out->setPosition(44 + first * 4))
                    return 1;
                for (int i = 0; i < stripeFrames; ++i)
                    for (int ch = 0; ch < 2; ++ch)
                        if (!out->writeShort(
                                static_cast<short>(sample(sourceIndex, ch, first + i)))) {
                            std::cerr << "Could not write multitrack signal.\n";
                            return 1;
                        }
            }
            out->flush();
            if (out->getStatus().failed())
                return 1;
        }
        auto audio = carta::loadAudio(formats, path, error, {}, 0, pcmDir);
        if (!audio || !audio->mapped || audio->numChannels() != 2 ||
            audio->numSamples() != sourceFrames ||
            std::abs(audio->sampleRate - sourceRate) > 1e-9) {
            std::cerr << "Multitrack import failed: " << error << '\n';
            return 1;
        }
        sources.push_back(std::move(audio));
    }
    const double importSeconds = std::chrono::duration<double>(Clock::now() - beginImport).count();
    for (int sourceIndex = 0; sourceIndex < 2; ++sourceIndex) {
        const auto& audio = *sources[static_cast<size_t>(sourceIndex)];
        const auto& cache = *audio.peakCache;
        if (cache.peaks.size() != 2 || cache.blockSize <= 0) {
            std::cerr << "Missing imported waveform peaks.\n";
            return 1;
        }
        for (int minute = 0; minute < 80; ++minute) {
            const auto sampleRate = sourceIndex == 0 ? rate : 48000;
            const juce::int64 peakIndex =
                (static_cast<juce::int64>(minute) * 60 * sampleRate) / cache.blockSize;
            const juce::int64 first = peakIndex * cache.blockSize,
                              last = std::min<juce::int64>(audio.numSamples(),
                                                           first + cache.blockSize);
            for (int channel = 0; channel < 2; ++channel) {
                if (peakIndex >=
                    static_cast<juce::int64>(cache.peaks[static_cast<size_t>(channel)].size()))
                    return 1;
                int low = 32767, high = -32768;
                for (auto frame = first; frame < last; ++frame) {
                    const int value = sample(sourceIndex, channel, frame);
                    low = std::min(low, value);
                    high = std::max(high, value);
                }
                const auto range =
                    cache.peaks[static_cast<size_t>(channel)][static_cast<size_t>(peakIndex)];
                if (std::abs(range.getStart() - static_cast<float>(low) / 32768.0f) > 1e-7f ||
                    std::abs(range.getEnd() - static_cast<float>(high) / 32768.0f) > 1e-7f) {
                    std::cerr << "Imported waveform peak mismatch at minute " << minute
                              << ", source " << sourceIndex << ", channel " << channel << ".\n";
                    return 1;
                }
            }
        }
    }
#if JUCE_MAC || JUCE_LINUX
    if (!pcmDir.findChildFiles(juce::File::findFilesAndDirectories, false).isEmpty()) {
        std::cerr << "Mapped PCM still has a name.\n";
        return 1;
    }
#endif
    carta::Session session;
    session.tracks.push_back({"A", {{sources[0], 0, 0, static_cast<double>(frames) / rate}}});
    session.tracks.push_back({"B", {{sources[1], 0, 0, static_cast<double>(frames) / rate}}});
    session.tracks.push_back(
        {"A delayed", {{sources[0], 4410.0 / rate, 0, static_cast<double>(frames - 4410) / rate}}});
    session.tracks.push_back(
        {"B advanced",
         {{sources[1], 0, 2205.0 / rate, static_cast<double>(frames - 2205) / rate}}});
    for (auto& track : session.tracks)
        track.gain = 0.25f;
    carta::ResamplingContext prepared;
    session.prepareResampling(prepared, rate);
    const carta::RenderClipIndex index(session);
    const auto playbackRange = carta::fullMasterPlaybackRange(session);
    carta::SincResampler referenceKernel(48000, rate);
    std::vector<double> timings;
    timings.reserve(iterations);
    float maximumError = 0;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const int minute = (iteration * 37) % 80;
        const int within = iteration % 4 == 0   ? 4000
                           : iteration % 4 == 1 ? stripeFrames - 256
                                                : 5000 + (iteration * 179) % (stripeFrames - 8000);
        const int start = minute * period + within;
        double position = static_cast<double>(start) / rate;
        carta::PlaybackClock clock;
        juce::AudioBuffer<float> output(2, block);
        output.clear();
        const auto begin = Clock::now();
        const bool playing = carta::renderPlayback(session, output, 0, block, position, rate, 2,
                                                   playbackRange, &prepared, &clock, &index);
        timings.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
        if (!playing) {
            std::cerr << "Multitrack playback ended unexpectedly.\n";
            return 1;
        }
        // Build a small independent PCM window for the 48 kHz source. The
        // reference reuses the interpolation kernel, but not mapped reads,
        // timeline traversal, track mixing, or long-position calculations.
        const juce::int64 sourceStart = std::max<juce::int64>(
            0,
            static_cast<juce::int64>(std::floor(static_cast<double>(start) * 48000 / rate)) - 512);
        const juce::int64 sourceEnd = std::min<juce::int64>(
            80LL * 60 * 48000,
            static_cast<juce::int64>(std::ceil(static_cast<double>(start + block) * 48000 / rate)) +
                2400 + 512);
        const int sourceCount = static_cast<int>(sourceEnd - sourceStart);
        std::vector<float> left(static_cast<size_t>(sourceCount)),
            right(static_cast<size_t>(sourceCount));
        for (int i = 0; i < sourceCount; ++i) {
            left[static_cast<size_t>(i)] =
                static_cast<float>(sample(1, 0, sourceStart + i)) / 32768.0f;
            right[static_cast<size_t>(i)] =
                static_cast<float>(sample(1, 1, sourceStart + i)) / 32768.0f;
        }
        for (int i = 0; i < block; ++i) {
            const juce::int64 frame = static_cast<juce::int64>(start) + i;
            const double sourcePosition =
                static_cast<double>(frame) * 48000 / rate - static_cast<double>(sourceStart);
            const auto b = referenceKernel.sampleStereo(left.data(), right.data(), sourceCount,
                                                        sourcePosition);
            const auto advanced = referenceKernel.sampleStereo(left.data(), right.data(),
                                                               sourceCount, sourcePosition + 2400);
            for (int channel = 0; channel < 2; ++channel) {
                float expected = static_cast<float>(sample(0, channel, frame)) / 32768.0f * 0.25f;
                expected += (channel == 0 ? b.first : b.second) * 0.25f;
                expected += static_cast<float>(sample(0, channel, frame - 4410)) / 32768.0f * 0.25f;
                expected += (channel == 0 ? advanced.first : advanced.second) * 0.25f;
                const float difference = std::abs(output.getSample(channel, i) - expected);
                maximumError = std::max(maximumError, difference);
                if (difference > 2e-5f) {
                    std::cerr << "Mapped multitrack sample mismatch at frame " << frame
                              << ", channel " << channel << ": " << difference << '\n';
                    return 1;
                }
            }
        }
    }
    std::sort(timings.begin(), timings.end());
    const double deadline = 1000.0 * block / rate;
    const auto late = std::count_if(timings.begin(), timings.end(),
                                    [deadline](double value) { return value > deadline; });
    std::cout << "Two 80-minute sparse stereo sources (44.1 and 48 kHz) imported in "
              << importSeconds << " s; decoded mapped PCM " << (80LL * 60 * (rate + 48000) * 8)
              << " bytes.\n"
              << "Four-track indexed 512-frame playback: p95 " << timings[iterations * 95 / 100]
              << " ms, p99 " << timings[iterations * 99 / 100] << " ms, maximum " << timings.back()
              << " ms; " << late << " of " << iterations << " renders exceeded " << deadline
              << " ms.\n"
              << "Waveform peaks and all output samples matched the generated PCM reference using "
                 "the same sinc kernel (maximum audio error "
              << maximumError
              << "). Sparse fixtures, no audio device, GUI activity or controlled cold-cache "
                 "conditions.\n";
#if JUCE_MAC || JUCE_LINUX
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if JUCE_MAC
        const double peakMiB = static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);
#else
        const double peakMiB = static_cast<double>(usage.ru_maxrss) / 1024.0;
#endif
        std::cout << "Process peak resident set " << peakMiB
                  << " MiB (OS accounting includes resident mapped pages; excludes system cache "
                     "and other processes).\n";
    }
#endif
    return 0;
}
