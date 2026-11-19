#include "Core/Session.h"
#include "WaveformBenchmark.h"
#include <chrono>
#include <iostream>
#include <vector>

int runWaveformBenchmark() {
    using Clock = std::chrono::steady_clock;
    constexpr int frames = 80 * 60 * 44100, columns = 1000, passes = 5;
    auto storage = std::make_shared<carta::MappedPcm>();
    storage->channels = 1;
    storage->frames = frames;
    auto output = storage->temporary.getFile().createOutputStream();
    const auto bytes = static_cast<juce::int64>(frames) * static_cast<juce::int64>(sizeof(float));
    if (!output || output->getStatus().failed() || !output->setPosition(bytes - 1) ||
        !output->writeByte(0)) {
        std::cerr << "Could not create temporary sparse waveform benchmark file.\n";
        return 1;
    }
    output.reset();
    storage->mapping = std::make_unique<juce::MemoryMappedFile>(storage->temporary.getFile(),
                                                                juce::MemoryMappedFile::readWrite);
    if (storage->mapping->getData() == nullptr) {
        std::cerr << "Could not map benchmark audio.\n";
        return 1;
    }
    auto* samples = static_cast<float*>(storage->mapping->getData());
    samples[0] = 0.5f;
    samples[4096] = -0.75f;
    samples[frames / 2] = 1.0f;
    samples[frames - 1] = -1.0f;
    carta::Audio audio;
    audio.sampleRate = 44100;
    audio.mapped = storage;
    const auto buildStart = Clock::now();
    carta::buildWaveformPeaks(audio);
    const auto buildEnd = Clock::now();
    size_t baseBytes = 0, extraBytes = 0;
    for (const auto& channel : audio.peakCache->peaks)
        baseBytes += channel.size() * sizeof(juce::Range<float>);
    for (const auto& channel : audio.peakCache->levels)
        for (const auto& level : channel)
            extraBytes += level.size() * sizeof(juce::Range<float>);
    std::vector<juce::Range<float>> hierarchical(static_cast<size_t>(columns));
    const auto run = [&](bool save) {
        const auto begin = Clock::now();
        for (int pass = 0; pass < passes; ++pass)
            for (int col = 0; col < columns; ++col) {
                const int left = static_cast<int>(static_cast<juce::int64>(col) * frames / columns);
                const int right =
                    static_cast<int>(static_cast<juce::int64>(col + 1) * frames / columns);
                const auto value = carta::waveformRange(audio, 0, left, right);
                if (save)
                    hierarchical[static_cast<size_t>(col)] = value;
                else if (value != hierarchical[static_cast<size_t>(col)])
                    return -1.0;
            }
        return std::chrono::duration<double, std::milli>(Clock::now() - begin).count() / passes;
    };
    const double hierarchicalMs = run(true);
    auto levels = std::move(audio.peakCache->levels);
    const double flatMs = run(false);
    if (hierarchicalMs < 0 || flatMs < 0) {
        std::cerr << "Hierarchical and flat waveform results differ.\n";
        return 1;
    }
    audio.peakCache->levels = std::move(levels);
    auto source = std::make_shared<carta::Audio>();
    source->sampleRate = audio.sampleRate;
    source->mapped = storage;
    source->peakCache = audio.peakCache;
    carta::Clip clip{source, 0, 0, source->length()};
    std::vector<carta::WaveformColumn> rms;
    const auto rmsStart = Clock::now();
    if (!carta::exactWaveformColumns(clip, 0, 0, source->length(), 0, columns, 0, columns, rms, {},
                                     true) ||
        rms.size() != columns || !rms.front().hasRms || rms.front().rms <= 0 ||
        !rms.back().hasRms || rms.back().rms <= 0) {
        std::cerr << "Could not calculate expected RMS columns.\n";
        return 1;
    }
    const auto rmsEnd = Clock::now();
    auto uncached = std::make_shared<carta::Audio>();
    uncached->sampleRate = audio.sampleRate;
    uncached->mapped = storage;
    carta::Clip uncachedClip{uncached, 0, 0, uncached->length()};
    std::vector<carta::WaveformColumn> referenceRms;
    const auto referenceStart = Clock::now();
    if (!carta::exactWaveformColumns(uncachedClip, 0, 0, uncached->length(), 0, columns, 0, columns,
                                     referenceRms, {}, true) ||
        referenceRms.size() != rms.size()) {
        std::cerr << "Could not calculate uncached RMS reference.\n";
        return 1;
    }
    const auto referenceEnd = Clock::now();
    for (size_t column = 0; column < rms.size(); ++column)
        if (rms[column].range != referenceRms[column].range ||
            !std::equal_to<float>{}(rms[column].rms, referenceRms[column].rms)) {
            std::cerr << "Cached and uncached RMS columns differ.\n";
            return 1;
        }
    std::cout << "80-minute 44.1 kHz mono, " << columns << " waveform columns\n"
              << "Cache build: "
              << std::chrono::duration<double, std::milli>(buildEnd - buildStart).count() << " ms\n"
              << "Base peak payload: " << baseBytes << " bytes; hierarchy payload: " << extraBytes
              << " bytes\n"
              << "Average per view: hierarchical " << hierarchicalMs << " ms; flat " << flatMs
              << " ms (" << passes << " passes)\n"
              << "RMS view scan: cached peaks "
              << std::chrono::duration<double, std::milli>(rmsEnd - rmsStart).count()
              << " ms; uncached reference "
              << std::chrono::duration<double, std::milli>(referenceEnd - referenceStart).count()
              << " ms\n"
              << "All column ranges matched. Source is a sparse, mostly silent mapped fixture; "
                 "this does not measure GUI, playback, or stereo.\n";
    return 0;
}
