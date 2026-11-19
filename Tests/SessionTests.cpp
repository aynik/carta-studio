/**
 * @file SessionTests.cpp
 * @brief Core engine, DSP, editing, and persistence tests for Carta Studio.
 *
 * Exercises 44.1 kHz half-open `[start, end)` `Session` geometry and mixing,
 * `SincResampler`, `Transport` playback/looping, `Spectrogram` &
 * `StableSpectrumRaster`, `Mp3Gapless` / LAME CRC inspection, `EditHistory`
 * budgets, non-destructive `Editing` / `ClipSelection` / gap & crossfade
 * operations, `SessionIO` `.carta` packages, recovery, and WAV export.
 *
 * Also dispatches optional CLI diagnostic, benchmark, and crash/limit checks
 * (`--mp3-gapless-*`, `--software-paint-probe`, `--*-benchmark`,
 * `--long-*-check`, `--write-limit-test`, `--*-crash-test`).
 */
#include "Core/Session.h"
#include "IO/SessionIO.h"
#include "IO/CheckedOutput.h"
#include "IO/CheckedText.h"
#include "IO/FileObservation.h"
#include "Editing/Editing.h"
#include "Audio/Transport.h"
#include "Audio/Resampler.h"
#include "Audio/Spectrogram.h"
#include "Audio/DeviceSettings.h"
#include "IO/Mp3Gapless.h"
#if CARTA_STUDIO_MINIMP3
#include "IO/NativeMp3Format.h"
#endif
#include "WaveformBenchmark.h"
#include "RenderBenchmark.h"
#include "HistoryBenchmark.h"
#include "LongImportCheck.h"
#include "WriteLimitTest.h"
#include "MappedCrashTest.h"
#include "ExportCrashTest.h"
#include "SoftwarePaintProbe.h"
#include <cmath>
#include <array>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <limits>
#include <cstring>
#include <filesystem>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#endif

static void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
static bool near(float a, float b) {
    return std::abs(a - b) < 0.00001f;
}
static void confirmDefaultRouting(carta::Track& track) {
    const int count = carta::maxTrackChannels(track);
    track.routing.clear();
    for (int i = 0; i < count; ++i)
        track.routing.push_back(carta::defaultChannelRoute(i, count));
}
int main(int argc, char** argv) {
    if (argc == 3 && juce::String(argv[1]) == "--mp3-gapless-info") {
        auto input = juce::File(argv[2]).createInputStream();
        carta::Mp3GaplessInfo info;
        juce::String error;
        if (!input || !carta::inspectMp3Gapless(*input, info, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        std::cout << info.sampleRate << ' ' << info.channels << ' ' << info.rawFrames << ' '
                  << info.leadingSamples << ' ' << info.sampleCount << '\n';
        return 0;
    }
    if (argc == 4 && (juce::String(argv[1]) == "--mp3-gapless-decode" ||
                      juce::String(argv[1]) == "--mp3-gapless-decode-mapped")) {
        juce::AudioFormatManager formats;
#if CARTA_STUDIO_MINIMP3
        carta::registerNativeMp3Format(formats);
#endif
        formats.registerBasicFormats();
        juce::String error;
        const auto threshold = juce::String(argv[1]) == "--mp3-gapless-decode-mapped"
                                   ? 0
                                   : std::numeric_limits<juce::int64>::max();
        const auto audio = carta::loadAudio(formats, juce::File(argv[2]), error, {}, threshold);
        if (!audio) {
            std::cerr << error << '\n';
            return 1;
        }
#if CARTA_STUDIO_MINIMP3
        if (audio->sourceFormat != "Carta MP3") {
            std::cerr << "Experimental reader was not selected.\n";
            return 1;
        }
#endif
        auto output = juce::File(argv[3]).createOutputStream();
        if (!output || output->getStatus().failed())
            return 1;
        for (int i = 0; i < audio->numSamples(); ++i)
            for (int ch = 0; ch < audio->numChannels(); ++ch)
                if (!output->writeFloat(audio->channelData(ch)[i]))
                    return 1;
        output->flush();
        if (output->getStatus().failed())
            return 1;
        std::cout << audio->sampleRate << ' ' << audio->numChannels() << ' ' << audio->numSamples()
                  << '\n';
        return 0;
    }
    if (argc == 2 && juce::String(argv[1]) == "--software-paint-probe")
        return runSoftwarePaintProbe();
    if (argc == 2 && juce::String(argv[1]) == "--software-raster-benchmark")
        return runSoftwareRasterBenchmark();
    if (argc == 2 && juce::String(argv[1]) == "--waveform-benchmark")
        return runWaveformBenchmark();
    if (argc == 2 && juce::String(argv[1]) == "--render-benchmark")
        return runRenderBenchmark();
    if (argc == 2 && juce::String(argv[1]) == "--mixed-render-benchmark")
        return runMixedRateRenderBenchmark();
    if (argc == 2 && juce::String(argv[1]) == "--group-drag-benchmark")
        return runGroupDragBenchmark();
    if (argc == 2 && juce::String(argv[1]) == "--history-benchmark")
        return runHistoryBenchmark();
    if (argc == 2 && juce::String(argv[1]) == "--long-import-check")
        return runLongImportCheck();
    if (argc == 2 && juce::String(argv[1]) == "--long-multitrack-check")
        return runLongMultitrackCheck();
    if (argc == 2 && juce::String(argv[1]) == "--write-limit-test")
        return runWriteLimitTest();
    if (argc == 2 && juce::String(argv[1]) == "--mapped-crash-test")
        return runMappedCrashTest();
    if (argc == 2 && juce::String(argv[1]) == "--export-crash-test")
        return runExportCrashTest();
    if (argc == 2 && juce::String(argv[1]) == "--project-crash-test")
        return runProjectCrashTest();
    if (argc == 6 && juce::String(argv[1]) == "--project-crash-child")
        return runProjectCrashChild(juce::File(argv[2]), juce::File(argv[3]), argv[4], argv[5]);
    if (argc == 5 && juce::String(argv[1]) == "--export-crash-child")
        return runExportCrashChild(juce::File(argv[2]), juce::String(argv[3]) == "commit",
                                   juce::String(argv[4]) == "exclusive");
    if (argc == 4 && juce::String(argv[1]) == "--mapped-crash-child")
        return runMappedCrashChild(juce::File(argv[2]), juce::File(argv[3]));
    check(verifySoftwareRasterPrototype(false),
          "chunked software raster is stable across clipped paints and its cache budget");
    {
        constexpr int frameBytes = 417;
        std::vector<unsigned char> bytes(frameBytes * 4, 0);
        const unsigned char header[]{0xff, 0xfb, 0x90, 0x00};
        for (int i = 0; i < 4; ++i)
            std::memcpy(bytes.data() + i * frameBytes, header, 4);
        size_t cursor = 36;
        std::memcpy(bytes.data() + cursor, "Xing", 4);
        cursor += 4;
        const auto write32 = [&](std::uint32_t value) {
            bytes[cursor++] = static_cast<unsigned char>(value >> 24);
            bytes[cursor++] = static_cast<unsigned char>(value >> 16);
            bytes[cursor++] = static_cast<unsigned char>(value >> 8);
            bytes[cursor++] = static_cast<unsigned char>(value);
        };
        write32(15);
        const auto countOffset = cursor;
        write32(3);
        write32(static_cast<std::uint32_t>(bytes.size()));
        cursor += 100;
        write32(0);
        std::memcpy(bytes.data() + cursor, "LAME3.100", 9);
        const auto delayOffset = cursor + 21;
        bytes[delayOffset] = 0x24;
        bytes[delayOffset + 1] = 0x03;
        bytes[delayOffset + 2] = 0x84;
        auto inspect = [&](const std::vector<unsigned char>& data, carta::Mp3GaplessInfo& info,
                           juce::String& error, std::function<bool()> progress = {}) {
            juce::MemoryInputStream stream(data.data(), data.size(), false);
            stream.setPosition(7);
            const bool ok = carta::inspectMp3Gapless(stream, info, error, std::move(progress));
            check(stream.getPosition() == 7, "MP3 inspection restores the input position");
            return ok;
        };
        carta::Mp3GaplessInfo info;
        juce::String error;
        check(inspect(bytes, info, error) && info.sampleRate == 44100 && info.channels == 2 &&
                  info.rawFrames == 3456 && info.leadingSamples == 1105 && info.sampleCount == 1980,
              "inspect valid MPEG-1 Xing/LAME gapless timing");
        const auto unchanged = info;
        auto sealed = bytes;
        std::uint16_t tagCrc = 0;
        for (size_t i = 0; i < cursor + 34; ++i) {
            tagCrc = static_cast<std::uint16_t>(tagCrc ^ sealed[i]);
            for (int bit = 0; bit < 8; ++bit)
                tagCrc =
                    static_cast<std::uint16_t>((tagCrc >> 1) ^ ((tagCrc & 1u) != 0 ? 0xa001u : 0u));
        }
        sealed[cursor + 34] = static_cast<unsigned char>(tagCrc >> 8);
        sealed[cursor + 35] = static_cast<unsigned char>(tagCrc);
        check(inspect(sealed, info, error), "valid LAME timing tag checksum is accepted");
        auto badTag = sealed;
        badTag[delayOffset] ^= 1;
        check(!inspect(badTag, info, error) && error.contains("timing tag checksum") &&
                  info.sampleCount == unchanged.sampleCount,
              "damaged LAME timing is rejected without changing output");
        badTag = sealed;
        badTag[cursor + 34] ^= 1;
        check(!inspect(badTag, info, error) && error.contains("timing tag checksum"),
              "damaged LAME tag checksum is rejected");
        auto broken = bytes;
        broken[countOffset + 3] = 4;
        check(!inspect(broken, info, error) && info.sampleCount == unchanged.sampleCount,
              "inconsistent MP3 frame count preserves the prior result");
        broken = bytes;
        broken[frameBytes + 2] = 0xf0;
        check(!inspect(broken, info, error), "invalid MP3 audio frame is rejected");
        broken = bytes;
        broken.push_back(0);
        check(!inspect(broken, info, error), "unexpected MP3 trailing bytes are rejected");
        broken = bytes;
        broken.resize(bytes.size() + 128);
        std::memcpy(broken.data() + bytes.size(), "TAG", 3);
        check(inspect(broken, info, error), "a trailing ID3v1 tag is accepted");
        broken = bytes;
        broken.insert(broken.begin(), 15, 0);
        std::memcpy(broken.data(), "ID3", 3);
        broken[3] = 4;
        broken[9] = 5;
        check(inspect(broken, info, error) && info.sampleCount == 1980,
              "a bounded ID3v2 tag is skipped");
        broken[9] = 0x85;
        check(!inspect(broken, info, error), "invalid synchsafe ID3 size is rejected");
        broken = bytes;
        broken[delayOffset + 3] = 0;
        broken[frameBytes + 1] = 0xf3;
        check(!inspect(broken, info, error), "changed MPEG version in an audio frame is rejected");
        broken = bytes;
        broken.resize(bytes.size() - 1);
        check(!inspect(broken, info, error), "truncated MP3 frame is rejected");
        broken = bytes;
        broken[delayOffset + 1] = 0x00;
        broken[delayOffset + 2] = 0x01;
        check(!inspect(broken, info, error), "unsupported small MP3 padding is rejected");
        check(!inspect(bytes, info, error, [] { return false; }) && error.contains("cancelled"),
              "MP3 frame inspection is cancellable");
    }
    {
        const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getNonexistentChildFile("carta-loop-project", {});
        check(directory.createDirectory().wasOk(), "create loop project test directory");
        carta::Session project;
        project.minimumEndSample = 1000;
        juce::String error;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const auto file = directory.getChildFile("project.carta"),
                   copy = directory.getChildFile("copy.carta"),
                   recovery = directory.getChildFile("recovery.carta"),
                   legacy = directory.getChildFile("legacy.carta");
        const carta::WavRange loop{
            123, 1200}; // A shortened master can leave a repairable loop beyond its end.
        check(carta::saveSession(project, 2, file, error, {}, nullptr, loop) &&
                  carta::saveSessionCopy(project, 2, copy, error, {}, nullptr, loop) &&
                  carta::saveRecoverySession(project, 2, recovery, error, {}, nullptr, loop),
              "save loop metadata in project, copy and recovery");
        carta::Session opened;
        int channels = 0;
        bool recovered = false;
        carta::WavRange loaded;
        check(carta::loadSession(opened, channels, formats, file, error, {}, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, &loaded) &&
                  loaded.start == loop.start && loaded.end == loop.end &&
                  opened.minimumEndSample == 1000,
              "reopen exact loop even when it exceeds the current master");
        check(carta::loadSession(opened, channels, formats, copy, error, {}, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, &loaded) &&
                  loaded.start == loop.start && loaded.end == loop.end,
              "project copy preserves loop metadata");
        check(carta::loadSession(opened, channels, formats, recovery, error, {}, &recovered,
                                 nullptr, nullptr, nullptr, nullptr, &loaded) &&
                  recovered && loaded.start == loop.start && loaded.end == loop.end,
              "recovery preserves loop metadata");
        juce::File portable;
        check(carta::exportPortableSession(project, 2, directory, portable, error, {}, loop) &&
                  carta::loadSession(opened, channels, formats,
                                     portable.getChildFile("Project.carta"), error, {}, nullptr,
                                     nullptr, nullptr, nullptr, nullptr, &loaded) &&
                  loaded.start == loop.start && loaded.end == loop.end,
              "portable copy preserves loop metadata");
        check(carta::saveSession(project, 2, legacy, error) &&
                  carta::loadSession(opened, channels, formats, legacy, error, {}, nullptr, nullptr,
                                     nullptr, nullptr, nullptr, &loaded) &&
                  loaded.start == 0 && loaded.end == 0,
              "older projects open without a saved loop");
        const auto before = file.loadFileAsString();
        check(!carta::saveSession(project, 2, file, error, {}, nullptr, {7, 7}) &&
                  file.loadFileAsString() == before,
              "invalid loop cannot replace a project");
        auto malformed = juce::JSON::parse(before);
        malformed.getDynamicObject()->setProperty("loopEndSample", -1);
        const auto bad = directory.getChildFile("bad.carta");
        check(bad.replaceWithText(juce::JSON::toString(malformed)), "write malformed loop project");
        loaded = {99, 100};
        channels = 1;
        check(!carta::loadSession(opened, channels, formats, bad, error, {}, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, &loaded) &&
                  loaded.start == 99 && loaded.end == 100 && channels == 1,
              "malformed loop leaves the current session metadata unchanged");
        directory.deleteRecursively();
    }
    {
        const double rate = carta::sessionRate;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(1, 1000);
        audio->samples.clear();
        carta::Session session;
        session.tracks.push_back({"A",
                                  {{audio, 0, 0, 20 / rate},
                                   {audio, 50 / rate, 0, 20 / rate},
                                   {audio, 65 / rate, 0, 30 / rate}}});
        session.tracks.push_back(
            {"B", {{audio, 100 / rate, 0, 20 / rate}, {audio, 150 / rate, 0, 10 / rate}}});
        session.tracks.push_back({"C", {{audio, 110 / rate, 0, 15 / rate}}});
        const std::vector<carta::ClipReference> chosen{{0, 0}, {1, 0}};
        carta::ClipOverlapPreview preview;
        carta::ClipGroupMovePreview valid;
        juce::String error;
        check(preview.prepare(session, chosen, error) && valid.prepare(session, chosen, error),
              "prepare destination overlap index");
        const auto canonical = [&](double time) {
            const auto frame = time * rate, nearest = std::round(frame);
            return std::abs(frame - nearest) < 1.0e-7 ? nearest / rate : time;
        };
        for (juce::int64 delta = -10; delta <= 180; ++delta)
            for (juce::int64 trackDelta = 0; trackDelta <= 1; ++trackDelta) {
                if (!valid.validate(delta, trackDelta, error))
                    continue;
                bool expected = false;
                for (const auto& ref : chosen) {
                    const auto& clip = session.tracks[static_cast<size_t>(ref.track)]
                                           .clips[static_cast<size_t>(ref.clip)];
                    const double shifted = carta::shiftTimelineBySamples(clip.start, delta),
                                 start = canonical(shifted),
                                 end = canonical(shifted + clip.duration);
                    const int destination = static_cast<int>(ref.track + trackDelta);
                    const auto& others = session.tracks[static_cast<size_t>(destination)].clips;
                    for (size_t i = 0; i < others.size(); ++i) {
                        bool selected = false;
                        for (const auto& other : chosen)
                            selected |=
                                other.track == destination && other.clip == static_cast<int>(i);
                        if (!selected && start < canonical(others[i].start + others[i].duration) &&
                            end > canonical(others[i].start))
                            expected = true;
                    }
                }
                check(preview.overlaps(delta, trackDelta) == expected,
                      "indexed group overlap preview matches exhaustive destination intervals");
            }
        carta::ClipOverlapPreview single;
        check(single.prepare(session, {{0, 0}}, error), "prepare single-clip overlap index");
        check(!single.overlapsSingle(30 / rate, 0) && single.overlapsSingle(31 / rate, 0) &&
                  !single.overlapsSingle(0, 0),
              "touching clips remain clear and overlapping clips are reported");
        check(single.overlapsSingle(100 / rate, 1) && !single.overlapsSingle(0, 2),
              "single-clip overlap uses the destination track");
        check(!single.prepare(session, {{0, 0}, {0, 0}}, error) &&
                  single.overlapsSingle(31 / rate, 0),
              "invalid preview input preserves the previous index");
    }
    {
        carta::EditorState state;
        carta::EditHistory history;
        bool stable = true;
        check(!history.undo(state, &stable) && stable,
              "empty history leaves the geometry flag untouched");
        history.record(state, "Move", false);
        state.session.minimumEndSample = 100;
        history.record(state, "Marker", true);
        state.session.markers.push_back({20 / carta::sessionRate, "Track"});
        check(history.undo(state, &stable) && stable && state.session.markers.empty(),
              "undo reports a geometry-stable transition");
        check(history.undo(state, &stable) && !stable && state.session.minimumEndSample == 0,
              "undo reports a geometry-changing transition");
        check(history.redo(state, &stable) && !stable && state.session.minimumEndSample == 100,
              "redo preserves the geometry-changing flag");
        check(history.redo(state, &stable) && stable && state.session.markers.size() == 1,
              "redo preserves the geometry-stable flag");
    }
    {
        const std::string input = "partial-write fixture";
        std::string output;
        int calls = 0;
        check(carta::writeAllBytes(input.data(), input.size(),
                                   [&](const void* data, size_t size) {
                                       const auto count = std::min<size_t>(size, 3);
                                       output.append(static_cast<const char*>(data), count);
                                       ++calls;
                                       return static_cast<juce::int64>(count);
                                   }) &&
                  output == input && calls > 1,
              "checked output completes positive partial writes in order");
        calls = 0;
        check(!carta::writeAllBytes(input.data(), input.size(),
                                    [&](const void*, size_t) { return ++calls == 1 ? 2 : 0; }) &&
                  calls == 2,
              "checked output rejects a zero-length write before completion");
        int directorySyncCalls = 0, directoryCloseCalls = 0;
        check(!carta::syncAndCloseDirectory(
                  [&] {
                      ++directorySyncCalls;
                      return false;
                  },
                  [&] {
                      ++directoryCloseCalls;
                      return true;
                  }) &&
                  directorySyncCalls == 1 && directoryCloseCalls == 1,
              "directory sync failure still closes the descriptor");
        check(!carta::syncAndCloseDirectory([] { return true; }, [] { return false; }),
              "directory close failure is reported separately from commit success");
        check(carta::syncAndCloseDirectory([] { return true; }, [] { return true; }),
              "directory sync and close succeed together");
        int syncCalls = 0, closeCalls = 0;
        output.clear();
        check(carta::writeSyncAndClose(
                  input.data(), input.size(),
                  [&](const void* data, size_t size) {
                      const auto count = std::min<size_t>(size, 3);
                      output.append(static_cast<const char*>(data), count);
                      return static_cast<juce::int64>(count);
                  },
                  [&] {
                      ++syncCalls;
                      return true;
                  },
                  [&] {
                      ++closeCalls;
                      return true;
                  }) &&
                  output == input && syncCalls == 1 && closeCalls == 1,
              "checked text completes partial writes, syncs and closes once");
        syncCalls = closeCalls = 0;
        check(!carta::writeSyncAndClose(
                  input.data(), input.size(), [](const void*, size_t) { return -1; },
                  [&] {
                      ++syncCalls;
                      return true;
                  },
                  [&] {
                      ++closeCalls;
                      return true;
                  }) &&
                  syncCalls == 0 && closeCalls == 1,
              "failed text write skips sync but still closes");
        syncCalls = closeCalls = 0;
        check(!carta::writeSyncAndClose(
                  input.data(), input.size(),
                  [](const void*, size_t size) { return static_cast<juce::int64>(size); },
                  [&] {
                      ++syncCalls;
                      return false;
                  },
                  [&] {
                      ++closeCalls;
                      return true;
                  }) &&
                  syncCalls == 1 && closeCalls == 1,
              "failed text sync still closes and rejects the write");
        syncCalls = closeCalls = 0;
        check(!carta::writeSyncAndClose(
                  input.data(), input.size(),
                  [](const void*, size_t size) { return static_cast<juce::int64>(size); },
                  [&] {
                      ++syncCalls;
                      return true;
                  },
                  [&] {
                      ++closeCalls;
                      return false;
                  }) &&
                  syncCalls == 1 && closeCalls == 1,
              "failed text close rejects the write");
        check(!carta::writeAllBytes(
                  input.data(), input.size(),
                  [](const void*, size_t size) { return static_cast<juce::int64>(size) + 1; }),
              "checked output rejects invalid over-reported progress");
        struct FaultStream final : juce::OutputStream {
            juce::MemoryOutputStream storage;
            bool rejectWrites = false;
            void flush() override { storage.flush(); }
            juce::int64 getPosition() override { return storage.getPosition(); }
            bool setPosition(juce::int64 position) override {
                return storage.setPosition(position);
            }
            bool write(const void* data, size_t bytes) override {
                return !rejectWrites && storage.write(data, bytes);
            }
        } destination;
        bool failure = false;
        std::unique_ptr<juce::OutputStream> stream =
            std::make_unique<carta::CheckedOutputStream>(destination, failure);
        juce::WavAudioFormat format;
        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate(44100)
                                 .withNumChannels(1)
                                 .withBitsPerSample(16);
        auto writer = format.createWriterFor(stream, options);
        juce::AudioBuffer<float> samples(1, 16);
        samples.clear();
        check(writer && writer->writeFromAudioSampleBuffer(samples, 0, 16) && writer->flush() &&
                  !failure,
              "prepare WAV output with a successful explicit flush");
        destination.rejectWrites = true;
        writer.reset();
        check(failure,
              "checked output detects a failed WAV header rewrite in the writer destructor");
    }
    {
        carta::EditorState state;
        state.session.minimumEndSample = 100;
        state.session.markers = {{10.0 / carta::sessionRate, "One"},
                                 {20.0 / carta::sessionRate, "Two"},
                                 {20.0 / carta::sessionRate, "Duplicate"},
                                 {90.0 / carta::sessionRate, "Four"}};
        const auto before = state;
        juce::String error;
        carta::EditHistory history;
        check(!carta::deleteMarkers(state.session, {}, error) &&
                  !carta::deleteMarkers(state.session, {0, 0}, error) &&
                  !carta::deleteMarkers(state.session, {0, 4}, error) &&
                  state.session.markers.size() == 4,
              "invalid marker batches are rejected without changing any marker");
        check(carta::deleteMarkers(state.session, {3, 1}, error) &&
                  state.session.markers.size() == 2 && state.session.markers[0].name == "One" &&
                  state.session.markers[1].name == "Duplicate" &&
                  carta::projectEndSample(state.session) == 100,
              "deleting nonadjacent markers preserves surviving order and master duration");
        history.record(before, "Delete markers");
        check(history.undo(state) && state.session.markers.size() == 4 && history.redo(state) &&
                  state.session.markers.size() == 2,
              "batch marker deletion supports undo and redo");
        check(carta::deleteMarkers(state.session, {1, 0}, error) && state.session.markers.empty() &&
                  carta::projectEndSample(state.session) == 100,
              "all explicit markers can be removed without deleting the implicit first track");
    }
    {
        carta::EditorState state;
        state.session.minimumEndSample = 100;
        const double rate = carta::sessionRate;
        state.session.markers = {
            {10 / rate, "One"}, {20 / rate, "Two"}, {30 / rate, "Three"}, {40 / rate, "Four"}};
        const auto before = state;
        juce::String error;
        carta::EditHistory history;
        check(carta::shiftMarkerIndices(state.session, {2, 0}, 5, error) &&
                  std::abs(state.session.markers[0].position * rate - 15) < 1e-8 &&
                  std::abs(state.session.markers[1].position * rate - 20) < 1e-8 &&
                  std::abs(state.session.markers[2].position * rate - 35) < 1e-8 &&
                  std::abs(state.session.markers[3].position * rate - 40) < 1e-8,
              "nonadjacent marker shift preserves spacing and stationary markers");
        history.record(before, "Shift markers");
        check(history.undo(state) &&
                  std::abs(state.session.markers[0].position * rate - 10) < 1e-8 &&
                  history.redo(state) &&
                  std::abs(state.session.markers[2].position * rate - 35) < 1e-8,
              "nonadjacent marker shift is undoable and redoable");
        state = before;
        for (const auto delta : {10LL, 11LL, -11LL, std::numeric_limits<juce::int64>::max(),
                                 std::numeric_limits<juce::int64>::min()})
            check(!carta::shiftMarkerIndices(state.session, {0, 2}, delta, error) &&
                      std::abs(state.session.markers[0].position * rate - 10) < 1e-8 &&
                      std::abs(state.session.markers[2].position * rate - 30) < 1e-8,
                  "colliding, crossing and out-of-range shifts are rejected atomically");
        check(!carta::shiftMarkerIndices(state.session, {0, 0}, 1, error) &&
                  !carta::shiftMarkerIndices(state.session, {0, 4}, 1, error) &&
                  !carta::shiftMarkerIndices(state.session, {}, 1, error),
              "invalid marker selections are rejected");
        state.session.markers[1].position = 20.25 / rate;
        check(!carta::shiftMarkerIndices(state.session, {0, 2}, 1, error) &&
                  error.contains("off-grid") &&
                  std::abs(state.session.markers[0].position * rate - 10) < 1e-8,
              "off-grid stationary markers must be repaired before a batch shift");
    }
    {
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = 48000;
        source->samples.setSize(1, 100);
        for (int i = 0; i < 100; ++i)
            source->samples.setSample(0, i, 0.4f);
        carta::Session project;
        project.tracks.push_back({"Fractional end", {{source, 0, 0, source->length()}}});
        const auto range = carta::fullMasterPlaybackRange(project);
        check(
            carta::projectEndSample(project) == 92 && std::abs(range.end * 44100 - 92) < 1e-9 &&
                range.end > project.length(),
            "full playback rounds a fractional clip end to the exclusive project-sample boundary");
        for (double rate : {44100.0, 48000.0, 96000.0}) {
            const auto frames = static_cast<int>(std::ceil(92 * rate / 44100.0));
            juce::AudioBuffer<float> output(1, frames + 1);
            output.clear();
            carta::ResamplingContext context;
            project.prepareResampling(context, rate);
            double position = 0;
            bool playing = true;
            for (int offset = 0; offset < frames; offset += 17) {
                const int count = std::min(17, frames - offset);
                playing = carta::renderPlayback(project, output, offset, count, position, rate, 1,
                                                range, &context);
                check(playing == (offset + count < frames),
                      "fractional-end playback stops at the sample-aligned master boundary");
            }
            check(!playing && near(output.getSample(0, frames), 0),
                  "playback leaves samples past the master end untouched");
            if (std::abs(rate - 96000) < 1e-6)
                check(frames == 201 && near(output.getSample(0, 200), 0),
                      "device playback includes the final silent frame in the rounded project "
                      "interval");
        }
        project.minimumEndSample = 137;
        check(std::abs(carta::fullMasterPlaybackRange(project).end * 44100 - 137) < 1e-9,
              "full playback includes an explicit silent master tail");
    }
    {
        const double rate = carta::sessionRate;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(1, 64);
        for (int i = 0; i < 64; ++i)
            audio->samples.setSample(0, i, static_cast<float>(i) / 100.0f);
        carta::Session source;
        source.tracks.push_back({"Layer 1", {{audio, 0, 0, 64 / rate}}});
        source.mdDisc.tracks = {
            {"one", "First", "", {}, false, false, false, "", {{30, 35}, {10, 14}}},
            {"two", "Second", "", {}, false, false, false, "", {{30, 35}}}};
        carta::DiscPreviewPlan plan;
        juce::String error;
        check(carta::prepareDiscPreview(source, plan, error) && plan.samples == 14 &&
                  plan.spans.size() == 3,
              "prepare nonlinear repeated MdDisc preview");
        {
            auto longPrefix = source;
            longPrefix.mdDisc.tracks = {
                {"prefix",
                 "Prefix",
                 "",
                 {},
                 false,
                 false,
                 false,
                 "",
                 std::vector<carta::MdPart>(5000, carta::MdPart{10, 11})},
                {"tail", "Tail", "", {}, false, false, false, "", {{30, 35}}}};
            carta::DiscPreviewPlan complete, suffix;
            check(carta::prepareDiscPreview(longPrefix, complete, error) &&
                      carta::prepareDiscPreview(longPrefix, suffix, error, "tail", true),
                  "prepare a repeated prefix and its later-track preview");
            check(complete.spans.size() == 5001 && suffix.programStart == 5000 &&
                      suffix.samples == 5 && suffix.spans.size() <= 2049 &&
                      suffix.spans.front().logicalStart > 0,
                  "later preview retains bounded resampler context rather than every earlier use");
            auto reference = complete;
            reference.programStart = 5000;
            reference.samples = 5;
            for (double outputRate :
                 {std::nextafter(rate / carta::SincResampler::maxFullSupportRatio,
                                 std::numeric_limits<double>::infinity()),
                  22050.0, 44100.0, 48000.0, 96000.0}) {
                carta::DiscPreviewRenderer a, b;
                check(carta::prepareDiscPreviewRenderer(longPrefix, outputRate, 2, a, error) &&
                          carta::prepareDiscPreviewRenderer(longPrefix, outputRate, 2, b, error),
                      "prepare bounded-context preview comparison");
                const int frames = static_cast<int>(std::ceil(5 * outputRate / rate));
                juce::AudioBuffer<float> actual(2, frames), expected(2, frames);
                actual.clear();
                expected.clear();
                juce::int64 posA = 0, posB = 0;
                carta::renderDiscPreview(longPrefix, suffix, a, actual, 0, frames, posA, outputRate,
                                         2);
                carta::renderDiscPreview(longPrefix, reference, b, expected, 0, frames, posB,
                                         outputRate, 2);
                check(posA == frames && posB == frames,
                      "bounded and complete context finish together");
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < frames; ++i)
                        check(std::abs(actual.getSample(ch, i) - expected.getSample(ch, i)) < 1e-6f,
                              "bounded context preserves later-track preview samples");
            }
        }
        {
            auto late = source;
            constexpr juce::int64 base = 80LL * 60 * 44100;
            late.tracks[0].clips[0].start = static_cast<double>(base) / rate;
            late.mdDisc.tracks = {
                {"first", "First", "", {}, false, false, false, "", {{0, base + 32}}},
                {"last", "Last", "", {}, false, false, false, "", {{base + 32, base + 64}}}};
            check(carta::projectEndSample(late) == base + 64,
                  "late preview fixture has an exact 80-minute master position");
            carta::DiscPreviewPlan complete, suffix;
            check(carta::prepareDiscPreview(late, complete, error) &&
                      carta::prepareDiscPreview(late, suffix, error, "last", true),
                  "prepare late-position preview plans");
            check(suffix.programStart == base + 32 && suffix.samples == 32 &&
                      suffix.spans.size() == 1 && suffix.spans[0].logicalStart == base + 32 - 2048,
                  "later preview crops a continuous span at the bounded context");
            complete.programStart = base + 32;
            complete.samples = 32;
            for (double outputRate : {22050.0, 44100.0, 48000.0, 96000.0}) {
                carta::DiscPreviewRenderer a, b;
                check(carta::prepareDiscPreviewRenderer(late, outputRate, 2, a, error) &&
                          carta::prepareDiscPreviewRenderer(late, outputRate, 2, b, error),
                      "prepare late-position preview renderers");
                const int frames = static_cast<int>(std::ceil(32 * outputRate / rate));
                juce::AudioBuffer<float> actual(2, frames), expected(2, frames);
                actual.clear();
                expected.clear();
                juce::int64 posA = 0, posB = 0;
                for (int offset = 0; offset < frames; offset += 7) {
                    const int n = std::min(7, frames - offset);
                    carta::renderDiscPreview(late, suffix, a, actual, offset, n, posA, outputRate,
                                             2);
                    carta::renderDiscPreview(late, complete, b, expected, offset, n, posB,
                                             outputRate, 2);
                }
                check(posA == frames && posB == frames && actual.getMagnitude(0, 0, frames) > 0.1f,
                      "late preview renders audible audio and reaches its exact end");
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < frames; ++i)
                        check(std::abs(actual.getSample(ch, i) - expected.getSample(ch, i)) < 1e-6f,
                              "cropped context preserves preview audio at 80 minutes");
                if (std::equal_to<double>{}(outputRate, rate))
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < frames; ++i)
                            check(std::abs(actual.getSample(ch, i) -
                                           audio->samples.getSample(0, 32 + i)) < 1e-6f,
                                  "native-rate preview at 80 minutes preserves the independently "
                                  "indexed source samples");
            }
        }
        {
            auto contiguous = source;
            contiguous.mdDisc.tracks = {
                {"first", "First", "", {}, false, false, false, "", {{0, 4}, {4, 8}}},
                {"second", "Second", "", {}, false, false, false, "", {{8, 12}, {0, 4}}}};
            carta::DiscPreviewPlan combined;
            check(carta::prepareDiscPreview(contiguous, combined, error, "second", true) &&
                      combined.programStart == 8 && combined.samples == 8 &&
                      combined.spans.size() == 2 && combined.spans[0].logicalEnd == 12 &&
                      combined.spans[1].sourceStart == 0,
                  "contiguous ranges coalesce across track boundaries without merging a repeated "
                  "jump");
            carta::DiscPreviewPlan separate;
            separate.programStart = 8;
            separate.samples = 8;
            separate.spans = {{0, 4, 0}, {4, 8, 4}, {8, 12, 8}, {12, 16, 0}};
            for (double outputRate : {22050.0, 44100.0, 48000.0, 96000.0}) {
                carta::DiscPreviewRenderer a, b;
                check(carta::prepareDiscPreviewRenderer(contiguous, outputRate, 2, a, error) &&
                          carta::prepareDiscPreviewRenderer(contiguous, outputRate, 2, b, error),
                      "prepare contiguous-preview comparison");
                const int frames = static_cast<int>(std::ceil(8 * outputRate / rate));
                juce::AudioBuffer<float> actual(2, frames), expected(2, frames);
                actual.clear();
                expected.clear();
                juce::int64 posA = 0, posB = 0;
                for (int offset = 0; offset < frames; offset += 3) {
                    const int n = std::min(3, frames - offset);
                    carta::renderDiscPreview(contiguous, combined, a, actual, offset, n, posA,
                                             outputRate, 2);
                    carta::renderDiscPreview(contiguous, separate, b, expected, offset, n, posB,
                                             outputRate, 2);
                }
                check(posA == frames && posB == frames,
                      "coalesced and separate previews finish at the same frame");
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < frames; ++i)
                        check(std::abs(actual.getSample(ch, i) - expected.getSample(ch, i)) < 1e-6f,
                              "coalescing preserves audio including resampling context at the "
                              "selected track start");
            }
        }
        carta::DiscPreviewPlan trackPlan;
        check(carta::prepareDiscPreview(source, trackPlan, error, "one") &&
                  trackPlan.samples == 9 && trackPlan.spans.size() == 2 &&
                  trackPlan.spans[0].sourceStart == 30 && trackPlan.spans[1].sourceStart == 10,
              "selected track preview preserves its ordered source ranges");
        const auto savedPlan = trackPlan;
        check(!carta::prepareDiscPreview(source, trackPlan, error, "missing") &&
                  trackPlan.samples == savedPlan.samples &&
                  trackPlan.spans.size() == savedPlan.spans.size(),
              "missing track preview leaves previous plan unchanged");
        carta::DiscPreviewPlan fromTrack;
        check(carta::prepareDiscPreview(source, fromTrack, error, "two", true) &&
                  fromTrack.programStart == 9 && fromTrack.samples == 5 &&
                  fromTrack.spans.size() == 3 && fromTrack.spans[2].sourceStart == 30 &&
                  fromTrack.spans[2].logicalStart == 9,
              "preview from a later track uses program time and reuses its range");
        check(carta::prepareDiscPreview(source, fromTrack, error, "one", true) &&
                  fromTrack.programStart == 0 && fromTrack.samples == 14 &&
                  fromTrack.spans.size() == 3,
              "preview from first track includes subsequent repeated segments");
        const auto previousFrom = fromTrack;
        check(!carta::prepareDiscPreview(source, fromTrack, error, "missing", true) &&
                  fromTrack.samples == previousFrom.samples &&
                  fromTrack.programStart == previousFrom.programStart &&
                  !carta::prepareDiscPreview(source, fromTrack, error, {}, true),
              "invalid preview start leaves the existing plan untouched");
        juce::AudioBuffer<float> bank(2, 14);
        bank.clear();
        carta::ResamplingContext bankContext;
        source.prepareResampling(bankContext, rate);
        source.render(bank, 0, 5, 30 / rate, rate, 2, &bankContext);
        source.render(bank, 5, 4, 10 / rate, rate, 2, &bankContext);
        source.render(bank, 9, 5, 30 / rate, rate, 2, &bankContext);
        {
            carta::DiscPreviewPlan tail;
            check(carta::prepareDiscPreview(source, tail, error, "two", true),
                  "prepare later-track preview with preceding program context");
            for (double outputRate : {22050.0, 32000.0, 44100.0, 48000.0, 96000.0}) {
                const int frames = static_cast<int>(std::ceil(5 * outputRate / rate));
                juce::AudioBuffer<float> actual(2, frames + 3);
                actual.clear();
                carta::DiscPreviewRenderer renderer;
                check(carta::prepareDiscPreviewRenderer(source, outputRate, 2, renderer, error),
                      "prepare tail preview resampler");
                juce::int64 elapsed = 0;
                int offset = 0;
                bool playing = true;
                while (offset < frames + 3 && playing) {
                    const int count = std::min(3, frames + 3 - offset);
                    playing = carta::renderDiscPreview(source, tail, renderer, actual, offset,
                                                       count, elapsed, outputRate, 2);
                    offset += count;
                }
                carta::SincResampler kernel(rate, outputRate);
                check(!playing && elapsed == frames, "tail preview stops at the program end");
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < frames; ++i) {
                        const double at = 9 + static_cast<double>(i) * rate / outputRate;
                        const auto expected = std::abs(outputRate - rate) < 1e-9
                                                  ? bank.getSample(ch, 9 + i)
                                                  : kernel.sample(bank.getReadPointer(ch), 14, at);
                        check(
                            std::abs(actual.getSample(ch, i) - expected) < 1e-6f,
                            "tail preview includes preceding logical samples in resampler context");
                    }
            }
        }
        for (double outputRate : {22050.0, 44100.0, 48000.0, 96000.0}) {
            const int frames = static_cast<int>(std::ceil(14 * outputRate / rate));
            juce::AudioBuffer<float> actual(2, frames + 3), reference(2, frames + 3);
            actual.clear();
            reference.clear();
            carta::DiscPreviewRenderer renderer;
            check(carta::prepareDiscPreviewRenderer(source, outputRate, 2, renderer, error),
                  "prepare disc preview working storage");
            juce::int64 elapsed = 0;
            int offset = 0;
            bool playing = true;
            while (offset < frames + 3) {
                const int count = std::min(3, frames + 3 - offset);
                playing = carta::renderDiscPreview(source, plan, renderer, actual, offset, count,
                                                   elapsed, outputRate, 2);
                offset += count;
                if (!playing)
                    break;
            }
            check(!playing && elapsed == frames, "disc preview stops at its half-open logical end");
            carta::SincResampler kernel(rate, outputRate);
            for (int frame = 0; frame < frames; ++frame)
                for (int ch = 0; ch < 2; ++ch) {
                    const double position = static_cast<double>(frame) * rate / outputRate;
                    reference.setSample(ch, frame,
                                        std::abs(outputRate - rate) < 1e-9
                                            ? bank.getSample(ch, frame)
                                            : kernel.sample(bank.getReadPointer(ch), 14, position));
                }
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < frames + 3; ++i)
                    check(std::abs(actual.getSample(ch, i) - reference.getSample(ch, i)) < 1e-6f,
                          "disc preview resamples the continuous logical bank across joins");
        }
        {
            auto stereo = std::make_shared<carta::Audio>();
            stereo->sampleRate = 48000;
            stereo->samples.setSize(2, 3000);
            for (int i = 0; i < 3000; ++i) {
                stereo->samples.setSample(0, i, 0.3f * std::sin(static_cast<float>(i) * 0.071f));
                stereo->samples.setSample(1, i, 0.2f * std::cos(static_cast<float>(i) * 0.113f));
            }
            carta::Session mixed;
            mixed.tracks.push_back({"Layer 1", {{stereo, 0, 0, stereo->length()}}});
            mixed.mdDisc.tracks = {
                {"first", "First", "", {}, false, false, false, "", {{100, 900}, {1000, 1500}}},
                {"repeat", "Repeat", "", {}, false, false, false, "", {{100, 900}}}};
            carta::DiscPreviewPlan longPlan;
            check(carta::prepareDiscPreview(mixed, longPlan, error) && longPlan.samples == 2100,
                  "prepare long nonlinear preview");
            for (int channels : {1, 2}) {
                carta::ResamplingContext referenceContext;
                mixed.prepareResampling(referenceContext, rate);
                juce::AudioBuffer<float> whole(channels, 2100);
                whole.clear();
                mixed.render(whole, 0, 800, 100 / rate, rate, channels, &referenceContext);
                mixed.render(whole, 800, 500, 1000 / rate, rate, channels, &referenceContext);
                mixed.render(whole, 1300, 800, 100 / rate, rate, channels, &referenceContext);
                for (double outputRate : {32000.0, 48000.0}) {
                    const int frames = static_cast<int>(std::ceil(2100 * outputRate / rate));
                    juce::AudioBuffer<float> actual(channels, frames + 2);
                    actual.clear();
                    carta::DiscPreviewRenderer renderer;
                    check(carta::prepareDiscPreviewRenderer(mixed, outputRate, channels, renderer,
                                                            error),
                          "prepare long preview renderer");
                    juce::int64 position = 0;
                    int offset = 0;
                    bool playing = true;
                    while (offset < frames + 2 && playing) {
                        const int count = std::min(1 + (offset * 37) % 601, frames + 2 - offset);
                        playing =
                            carta::renderDiscPreview(mixed, longPlan, renderer, actual, offset,
                                                     count, position, outputRate, channels);
                        offset += count;
                    }
                    carta::SincResampler kernel(rate, outputRate);
                    check(!playing && position == frames,
                          "long preview ends exactly across variable callback sizes");
                    for (int ch = 0; ch < channels; ++ch)
                        for (int i = 0; i < frames; ++i) {
                            const auto expected =
                                kernel.sample(whole.getReadPointer(ch), 2100,
                                              static_cast<double>(i) * rate / outputRate);
                            check(std::abs(actual.getSample(ch, i) - expected) < 1e-5f,
                                  "long preview agrees with resampling the assembled PCM mix");
                        }
                }
            }
        }
        auto invalid = source;
        invalid.mdDisc.tracks.clear();
        const auto before = plan;
        check(!carta::prepareDiscPreview(invalid, plan, error) && plan.samples == before.samples &&
                  plan.spans.size() == before.spans.size(),
              "empty MdDisc preview is rejected without changing the previous plan");
        invalid = source;
        invalid.mdDisc.tracks[0].parts[0].endSample = 999999;
        check(!carta::prepareDiscPreview(invalid, plan, error),
              "preview rejects parts exceeding timeline master");
    }
    {
        const double rate = carta::sessionRate;
        double position = 10.4 / rate;
        check(carta::seekPlayhead(position, 100, carta::PlayheadSeek::nextSample) &&
                  std::abs(position * rate - 11) < 1e-9,
              "keyboard seek rounds the current position and advances one project sample");
        check(carta::seekPlayhead(position, 100, carta::PlayheadSeek::previousSample) &&
                  std::abs(position * rate - 10) < 1e-9,
              "keyboard seek moves one sample earlier");
        check(carta::seekPlayhead(position, 100, carta::PlayheadSeek::start) &&
                  std::abs(position) < 1e-12 &&
                  carta::seekPlayhead(position, 100, carta::PlayheadSeek::previousSample) &&
                  std::abs(position) < 1e-12,
              "keyboard seek clamps at the master start");
        check(carta::seekPlayhead(position, 100, carta::PlayheadSeek::end) &&
                  std::abs(position * rate - 100) < 1e-9 &&
                  carta::seekPlayhead(position, 100, carta::PlayheadSeek::nextSample) &&
                  std::abs(position * rate - 100) < 1e-9,
              "keyboard seek permits and clamps at the exclusive master end");
        position = 150 / rate;
        check(carta::seekPlayhead(position, 100, carta::PlayheadSeek::previousSample) &&
                  std::abs(position * rate - 99) < 1e-9,
              "keyboard seek clamps a cursor outside the master before stepping");
        const double before = position;
        check(!carta::seekPlayhead(position, 0, carta::PlayheadSeek::start) &&
                  !carta::seekPlayhead(position, 9007199254740992LL, carta::PlayheadSeek::end) &&
                  !carta::seekPlayhead(position, 100, static_cast<carta::PlayheadSeek>(99)) &&
                  std::memcmp(&position, &before, sizeof(double)) == 0,
              "invalid keyboard seeks preserve the playhead");
    }
    {
        carta::OutputPeakHold meter;
        auto peaks = meter.snapshot();
        check(peaks.channels == 0 && near(peaks.left, 0) && near(peaks.right, 0),
              "output peak hold starts unmeasured");
        juce::AudioBuffer<float> block(2, 8);
        block.clear();
        block.setSample(0, 0, 0.99f);
        block.setSample(0, 3, -0.75f);
        block.setSample(1, 4, 1.25f);
        meter.observe(block, 2, 3);
        peaks = meter.snapshot();
        check(peaks.channels == 2 && near(peaks.left, 0.75f) && near(peaks.right, 1.25f),
              "output peak hold measures both channels only within the requested block");
        const auto firstRecent = meter.takeRecent();
        const auto drained = meter.takeRecent();
        check(firstRecent.channels == 2 && near(firstRecent.left, 0.75f) &&
                  near(firstRecent.right, 1.25f) && drained.channels == 0 &&
                  near(drained.left, 0) && near(meter.snapshot().right, 1.25f),
              "polling recent peaks drains only the live interval and preserves the hold");
        block.clear();
        block.setSample(0, 2, 0.9f);
        block.setSample(1, 2, 0.3f);
        meter.observe(block, 2, 1);
        peaks = meter.snapshot();
        check(near(peaks.left, 0.9f) && near(peaks.right, 1.25f) &&
                  near(block.getSample(0, 2), 0.9f),
              "output peak hold retains maxima without changing audio");
        const auto current = meter.takeRecent();
        check(current.channels == 2 && near(current.left, 0.9f) && near(current.right, 0.3f),
              "live interval shows new levels independently of held maxima");
        meter.observe(block, -1, 2);
        meter.observe(block, 7, 2);
        meter.observe(block, 0, 0);
        check(near(meter.snapshot().right, 1.25f),
              "invalid peak measurement ranges leave the hold unchanged");
        meter.reset();
        peaks = meter.snapshot();
        check(peaks.channels == 0 && near(peaks.left, 0) && near(peaks.right, 0),
              "output peak hold resets");
        juce::AudioBuffer<float> mono(1, 3);
        mono.clear();
        mono.setSample(0, 1, -0.5f);
        meter.observe(mono, 0, 3);
        peaks = meter.snapshot();
        check(peaks.channels == 1 && near(peaks.left, 0.5f) && near(peaks.right, 0),
              "output peak hold handles mono devices");
        check(std::abs(carta::decayOutputPeak(1, 0, 1) - std::pow(10.0f, -1.2f)) < 1e-6f &&
                  near(carta::decayOutputPeak(1, 0.5f, 1), 0.5f),
              "live peak decays at 24 dB per second and responds immediately to higher peaks");
        check(near(carta::decayOutputPeak(0.5f, 0, -1), 0.5f) &&
                  near(carta::decayOutputPeak(std::numeric_limits<float>::infinity(), 0, 1), 0),
              "live peak decay handles invalid elapsed time and non-finite prior peaks");
    }
    {
        constexpr int count = 512;
        for (const double rate : {44100.0, 48000.0, 96000.0, 192000.0})
            for (const double origin :
                 {0.0, 1.23456789, 4800.0, 100000000.0, 9000000000000000.0 / 192000.0}) {
                for (const bool aligned : {false, true}) {
                    const double blockFrame = std::round(origin * rate),
                                 blockStart = aligned ? blockFrame / rate : origin;
                    for (int offset = -600; offset <= 900; offset += 37)
                        for (int length : {1, 17, 513}) {
                            const double left = blockStart +
                                                (static_cast<double>(offset) + 0.25) / rate,
                                         right = left + static_cast<double>(length) / rate;
                            const auto window =
                                carta::clipRenderWindow(left, right, blockStart, rate, count);
                            const bool maybe = carta::clipMayIntersectBlock(
                                left, right, blockStart,
                                blockStart + static_cast<double>(count - 1) / rate);
                            const auto canonical = [](double value) {
                                const auto frame = value * carta::sessionRate,
                                           nearest = std::round(frame);
                                return std::isfinite(frame) && std::abs(frame - nearest) < 1.0e-7
                                           ? nearest / carta::sessionRate
                                           : value;
                            };
                            const double canonicalLeft = canonical(left),
                                         canonicalRight = canonical(right);
                            check(window.getStart() >= 0 && window.getEnd() <= count,
                                  "clip render window stays inside the audio block");
                            for (int i = 0; i < count; ++i) {
                                const double time =
                                    aligned ? (blockFrame + static_cast<double>(i)) / rate
                                            : blockStart + static_cast<double>(i) / rate;
                                check(time < left || time >= right || window.contains(i),
                                      "clip render window never excludes an audible timestamp");
                                if (!maybe)
                                    check(time < canonicalLeft || time >= canonicalRight,
                                          "coarse clip rejection never excludes a canonicalized "
                                          "audible timestamp");
                            }
                        }
                }
            }
        check(carta::clipRenderWindow(10, 11, 0, 44100, 512).isEmpty(),
              "clip far beyond the block is skipped");
        check(!carta::clipMayIntersectBlock(10, 11, 0, 511.0 / 44100.0) &&
                  carta::clipMayIntersectBlock(std::numeric_limits<double>::quiet_NaN(), 1, 0, 1),
              "coarse clip test skips remote clips and retains invalid geometry for the detailed "
              "path");
        const auto unknown =
            carta::clipRenderWindow(std::numeric_limits<double>::quiet_NaN(), 1, 0, 44100, 512);
        check(unknown.getStart() == 0 && unknown.getEnd() == 512,
              "invalid window inputs conservatively retain the full block");
    }
    {
        carta::Session indexed;
        std::vector<std::shared_ptr<carta::Audio>> sources;
        for (double rate : {44100.0, 48000.0}) {
            auto source = std::make_shared<carta::Audio>();
            source->sampleRate = rate;
            source->samples.setSize(2, 512);
            for (int i = 0; i < 512; ++i) {
                source->samples.setSample(0, i, std::sin(i * 0.17f) * 0.2f);
                source->samples.setSample(1, i, std::cos(i * 0.13f) * 0.15f);
            }
            sources.push_back(source);
        }
        std::uint32_t seed = 0x739ab12du;
        for (int t = 0; t < 3; ++t) {
            carta::Track track;
            track.gain = 0.7f;
            track.pan = 0.2f;
            for (int i = 0; i < 450; ++i) {
                seed = seed * 1664525u + 1013904223u;
                const double start = 4790.0 + static_cast<double>(seed % 1800) / 44100.0 +
                                     (i % 3 == 0 ? 0.25 / 44100.0 : 0);
                track.clips.push_back(
                    {sources[static_cast<size_t>(i % 2)], start, 0, 180.0 / 48000.0});
                check(carta::setClipEnvelope(track.clips.back(), 0.3f, 20.0 / 44100.0,
                                             30.0 / 44100.0),
                      "prepare indexed-render fades");
            }
            indexed.tracks.push_back(std::move(track));
        }
        const carta::RenderClipIndex index(indexed);
        for (double rate : {44100.0, 48000.0, 96000.0}) {
            carta::ResamplingContext prepared;
            indexed.prepareResampling(prepared, rate);
            for (int block = 0; block < 60; ++block)
                for (const carta::TrackRange scope :
                     {carta::TrackRange{}, carta::TrackRange{1, 2}}) {
                    const double start = 4790.0 + (block * 43 - 100.25) / 44100.0;
                    juce::AudioBuffer<float> reference(2, 127), actual(2, 127);
                    reference.clear();
                    actual.clear();
                    indexed.render(reference, 0, 127, start, rate, 2, &prepared, scope);
                    indexed.render(actual, 0, 127, start, rate, 2, &prepared, scope, -1, &index);
                    for (int ch = 0; ch < 2; ++ch)
                        check(std::memcmp(reference.getReadPointer(ch), actual.getReadPointer(ch),
                                          127 * sizeof(float)) == 0,
                              "indexed and linear render match bit-for-bit for unsorted "
                              "overlapping mixed-rate clips");
                }
            carta::PlaybackClock linearClock, indexedClock;
            double linearPosition = 4790.0 + 300.0 / 44100.0, indexedPosition = linearPosition;
            const carta::PlaybackRange loop{4790.0 + 300.0 / 44100.0, 4790.0 + 457.0 / 44100.0,
                                            true};
            for (int block = 0; block < 40; ++block) {
                juce::AudioBuffer<float> reference(2, 127), actual(2, 127);
                reference.clear();
                actual.clear();
                check(carta::renderPlayback(indexed, reference, 0, 127, linearPosition, rate, 2,
                                            loop, &prepared, &linearClock) &&
                          carta::renderPlayback(indexed, actual, 0, 127, indexedPosition, rate, 2,
                                                loop, &prepared, &indexedClock, &index),
                      "indexed and linear loop playback continue");
                check(std::abs(linearPosition - indexedPosition) < 1e-12,
                      "indexed loop playback keeps the same transport position");
                for (int ch = 0; ch < 2; ++ch)
                    check(std::memcmp(reference.getReadPointer(ch), actual.getReadPointer(ch),
                                      127 * sizeof(float)) == 0,
                          "indexed loop playback matches linear playback bit-for-bit");
            }
        }
        carta::Session ordered;
        auto& track = ordered.tracks.emplace_back();
        for (int i = 0; i < 1024; ++i)
            track.clips.push_back({sources[0], static_cast<double>(i), 0, 0.005});
        const carta::RenderClipIndex orderedIndex(ordered);
        std::vector<int> visited;
        orderedIndex.forEach(0, 500.0, 500.001, [&](int clip) { visited.push_back(clip); });
        check(visited == std::vector<int>({500}),
              "render index skips remote clips in a long chronological track");
    }
    {
        // Exercise index boundaries independently of source rate, callback size,
        // fractional clip placement and the order in which clips were added.
        juce::Random random(0x6175636c);
        carta::Session arrangement;
        std::vector<std::shared_ptr<carta::Audio>> sources;
        for (double sourceRate : {32000.0, 44100.0, 96000.0}) {
            auto source = std::make_shared<carta::Audio>();
            source->sampleRate = sourceRate;
            source->samples.setSize(2, 4096);
            for (int i = 0; i < 4096; ++i) {
                source->samples.setSample(0, i, 0.1f * std::sin(i * 0.071f));
                source->samples.setSample(1, i, 0.1f * std::cos(i * 0.093f));
            }
            sources.push_back(source);
        }
        for (int row = 0; row < 3; ++row) {
            carta::Track track;
            track.gain = 0.5f + 0.1f * static_cast<float>(row);
            track.pan = 0.2f * static_cast<float>(row - 1);
            for (int i = 0; i < 120; ++i) {
                auto source = sources[static_cast<size_t>(random.nextInt(3))];
                const double offset = static_cast<double>(random.nextInt(500)) / source->sampleRate;
                const double duration =
                    static_cast<double>(100 + random.nextInt(900)) / source->sampleRate;
                const double start = 4790.0 + (static_cast<double>(random.nextInt(6000)) +
                                               0.25 * random.nextInt(4)) /
                                                  carta::sessionRate;
                track.clips.push_back({source, start, offset, duration});
                check(carta::setClipEnvelope(track.clips.back(), 0.3f, duration * 0.15,
                                             duration * 0.2),
                      "prepare varied indexed-render envelope");
            }
            arrangement.tracks.push_back(std::move(track));
        }
        const carta::RenderClipIndex index(arrangement);
        for (double outputRate : {22050.0, 44100.0, 48000.0, 96000.0, 192000.0}) {
            carta::ResamplingContext context;
            arrangement.prepareResampling(context, outputRate);
            for (int trial = 0; trial < 100; ++trial) {
                const auto& clip = arrangement.tracks[static_cast<size_t>(random.nextInt(3))]
                                       .clips[static_cast<size_t>(random.nextInt(120))];
                const int count = 1 + random.nextInt(256);
                const double edge = trial % 2 == 0 ? clip.start : clip.start + clip.duration;
                const double start = edge + static_cast<double>(random.nextInt(9) - 4) / outputRate;
                const carta::TrackRange scope =
                    trial % 3 == 0 ? carta::TrackRange{1, 2} : carta::TrackRange{};
                juce::AudioBuffer<float> expected(2, count), actual(2, count);
                expected.clear();
                actual.clear();
                arrangement.render(expected, 0, count, start, outputRate, 2, &context, scope);
                arrangement.render(actual, 0, count, start, outputRate, 2, &context, scope, -1,
                                   &index);
                for (int channel = 0; channel < 2; ++channel)
                    check(std::memcmp(expected.getReadPointer(channel),
                                      actual.getReadPointer(channel),
                                      static_cast<size_t>(count) * sizeof(float)) == 0,
                          "randomized index matches linear rendering at clip edges across rates "
                          "and callback sizes");
            }
        }
    }
    {
        carta::Session mix;
        for (int i = 0; i < 3; ++i) {
            auto source = std::make_shared<carta::Audio>();
            source->sampleRate = carta::sessionRate;
            source->samples.setSize(1, 2);
            source->samples.clear();
            source->samples.setSample(0, 0, static_cast<float>(i + 1) * 0.1f);
            mix.tracks.push_back(
                {"Track " + juce::String(i + 1), {{source, 0, 0, 2 / carta::sessionRate}}});
        }
        const auto sample = [&] {
            juce::AudioBuffer<float> out(2, 1);
            out.clear();
            mix.render(out, 0, 1, 0, carta::sessionRate, 2);
            return out.getSample(0, 0);
        };
        check(!carta::setTrackFlags(mix, {0, 0}, carta::TrackFlag::mute, true) &&
                  !mix.tracks[0].muted,
              "duplicate track indices reject atomically");
        check(!carta::setTrackFlags(mix, {0, 3}, carta::TrackFlag::mute, true) &&
                  !mix.tracks[0].muted,
              "invalid track index does not partially change the mix");
        check(carta::setTrackFlags(mix, {0, 2}, carta::TrackFlag::mute, true) &&
                  mix.tracks[0].muted && !mix.tracks[1].muted && mix.tracks[2].muted &&
                  near(sample(), 0.2f),
              "nonadjacent mute affects exactly the selected tracks and rendered mix");
        check(carta::setTrackFlags(mix, {0, 2}, carta::TrackFlag::mute, false) &&
                  carta::setTrackFlags(mix, {0, 2}, carta::TrackFlag::solo, true) &&
                  near(sample(), 0.4f),
              "nonadjacent solo preserves the unselected track and routes the selected mix");
        check(carta::setTrackFlags(mix, {2}, carta::TrackFlag::mute, true) && near(sample(), 0.1f),
              "mute retains precedence over solo for independently selected tracks");
        auto source = mix;
        source.tracks[0].muted = false;
        source.tracks[0].solo = false;
        source.tracks[2].muted = false;
        source.tracks[2].solo = false;
        source.tracks[1].solo = true;
        auto isolated = source;
        check(!carta::limitToTrackIndices(isolated, {0, 0}) && !isolated.tracks[0].muted &&
                  isolated.tracks[1].solo,
              "invalid independent mix scope leaves all flags intact");
        check(carta::limitToTrackIndices(isolated, {0, 2}) && isolated.tracks[1].muted &&
                  !isolated.tracks[1].solo && !isolated.tracks[0].muted &&
                  !isolated.tracks[2].muted,
              "independent mix scope excludes unselected tracks and their solos");
        carta::Session preview;
        carta::PlaybackRange previewRange;
        juce::String previewError;
        check(carta::prepareIndependentTrackAudition(source, {0, 2, 0, 2}, {0, 2}, false, preview,
                                                     previewRange, previewError) &&
                  preview.tracks[1].muted && !preview.tracks[1].solo && !source.tracks[1].muted,
              "independent audition prepares an isolated snapshot without changing the source");
        juce::AudioBuffer<float> audition(2, 2), expected(2, 2);
        audition.clear();
        expected.clear();
        double cursor = previewRange.start;
        carta::renderPlayback(preview, audition, 0, 2, cursor, carta::sessionRate, 2, previewRange);
        isolated.render(expected, 0, 2, 0, carta::sessionRate, 2);
        for (int ch = 0; ch < 2; ++ch)
            check(std::memcmp(audition.getReadPointer(ch), expected.getReadPointer(ch),
                              2 * sizeof(float)) == 0,
                  "independent selected-track audition matches its isolated mix");
    }
    {
        const double rate = carta::sessionRate;
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = rate;
        source->samples.setSize(1, 200);
        source->samples.clear();
        carta::Session original;
        for (int i = 0; i < 3; ++i)
            original.tracks.push_back(
                {"Track " + juce::String(i + 1), {{source, 0, 0, 100 / rate}}});
        juce::String error;
        check(carta::groupClips(original, {{0, 0}, {2, 0}}, error),
              "prepare persistent group for scoped ripple validation");
        const carta::TimeSelection range{20, 40, 0, 0};
        auto rejected = original;
        carta::Clipboard clipboard;
        clipboard.lengthSamples = 7;
        clipboard.tracks.resize(1);
        check(!carta::rippleCutRange(rejected, range, false, clipboard, error) &&
                  error.contains("clip group") && clipboard.lengthSamples == 7 &&
                  rejected.tracks[0].clips.size() == 1 && rejected.tracks[2].clips.size() == 1,
              "scoped ripple cut rejects a partial group without replacing the clipboard");
        check(!carta::insertGap(rejected, range, false, error) && error.contains("clip group") &&
                  rejected.tracks[0].clips.size() == 1,
              "scoped gap insertion rejects a partial group atomically");
        check(!carta::editIndependentTracks(rejected, range, {0, 1},
                                            carta::IndependentRangeEdit::cutClose, clipboard,
                                            error) &&
                  error.contains("clip group") && clipboard.lengthSamples == 7,
              "independent ripple rejects a missing group track without replacing the clipboard");
        check(!carta::editIndependentTracks(
                  rejected, range, {0}, carta::IndependentRangeEdit::insertGap, clipboard, error) &&
                  error.contains("clip group"),
              "independent gap insertion rejects a partial group");
        carta::Session preview;
        carta::PlaybackRange previewRange;
        check(!carta::prepareIndependentRipplePreview(original, range, {0}, 10, 10, preview,
                                                      previewRange, error) &&
                  error.contains("clip group"),
              "ripple preview applies the same group validation as the edit");
        check(!carta::pasteRangeInserting(rejected, clipboard, 0, 20, false, error) &&
                  error.contains("clip group") && rejected.tracks[0].clips.size() == 1,
              "insert paste cannot shift part of a group");
        auto allowed = original;
        check(carta::editIndependentTracks(allowed, range, {0, 2},
                                           carta::IndependentRangeEdit::insertGap, clipboard,
                                           error) &&
                  allowed.tracks[0].clips.size() == 2 && allowed.tracks[2].clips.size() == 2 &&
                  allowed.tracks[1].clips.size() == 1,
              "independent gap insertion accepts all grouped tracks");
        auto all = original;
        check(carta::rippleRemoveRange(all, range, true, &error) &&
                  all.tracks[0].clips.size() == 2 && all.tracks[2].clips.size() == 2,
              "all-track ripple accepts the full group");
        auto prior = original;
        for (int track : {0, 2})
            prior.tracks[static_cast<size_t>(track)].clips[0].duration = 10 / rate;
        prior.tracks[0].clips.push_back({source, 30 / rate, 30 / rate, 30 / rate});
        check(carta::insertGap(prior, range, false, error) && prior.tracks[0].clips.size() == 2,
              "groups wholly before the edit do not block unrelated scoped insertion");
    }
    {
        const double rate = carta::sessionRate;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(1, 100);
        for (int i = 0; i < 100; ++i)
            audio->samples.setSample(0, i, static_cast<float>(i + 1) / 100.0f);
        carta::Session initial;
        for (int i = 0; i < 3; ++i)
            initial.tracks.push_back({"Track " + juce::String(i + 1), {{audio, 0, 0, 100 / rate}}});
        initial.markers = {{90 / rate, "Tail"}};
        const carta::TimeSelection range{20, 40, 0, 2};
        carta::Clipboard clipboard;
        clipboard.lengthSamples = 7;
        clipboard.tracks.resize(1);
        juce::String error;
        carta::Clipboard independentCopy = clipboard;
        check(!carta::copyIndependentTracks(initial, range, {0, 0}, independentCopy, error) &&
                  independentCopy.lengthSamples == 7,
              "invalid independent copy preserves the previous clipboard");
        check(carta::copyIndependentTracks(initial, range, {2, 0}, independentCopy, error) &&
                  independentCopy.lengthSamples == 20 && independentCopy.tracks.size() == 3 &&
                  independentCopy.tracks[1].empty() && initial.tracks[0].clips.size() == 1,
              "independent copy leaves the project intact and retains silent intervening rows");
        auto silent = initial;
        silent.tracks[0].clips.clear();
        silent.tracks[2].clips.clear();
        check(carta::copyIndependentTracks(silent, range, {0, 2}, independentCopy, error) &&
                  !independentCopy.hasAudio() && independentCopy.lengthSamples == 20 &&
                  independentCopy.tracks.size() == 3,
              "independent copy retains an entirely silent time range");
        auto rejected = initial;
        check(!carta::editIndependentTracks(
                  rejected, range, {0, 0}, carta::IndependentRangeEdit::cutGap, clipboard, error) &&
                  rejected.tracks[0].clips.size() == 1 && clipboard.lengthSamples == 7,
              "duplicate independent tracks reject atomically without replacing the clipboard");
        check(!carta::editIndependentTracks(rejected, range, {0, 1, 2},
                                            carta::IndependentRangeEdit::cutClose, clipboard,
                                            error) &&
                  error.contains("marker") && rejected.tracks[0].clips.size() == 1 &&
                  clipboard.lengthSamples == 7,
              "independent ripple cut preserves audio and clipboard when the marker would be "
              "orphaned");
        auto cut = initial;
        check(carta::editIndependentTracks(cut, range, {0, 2}, carta::IndependentRangeEdit::cutGap,
                                           clipboard, error) &&
                  clipboard.lengthSamples == 20 && clipboard.tracks.size() == 3 &&
                  clipboard.tracks[1].empty() && clipboard.tracks[0].size() == 1 &&
                  clipboard.tracks[2].size() == 1,
              "nonadjacent cut keeps the intervening clipboard row silent");
        check(cut.tracks[0].clips.size() == 2 && cut.tracks[1].clips.size() == 1 &&
                  cut.tracks[2].clips.size() == 2 &&
                  std::abs(cut.markers[0].position * rate - 90) < 1e-8,
              "nonadjacent cut leaves the intervening track and markers unchanged");
        carta::Session paste;
        for (int i = 0; i < 3; ++i) {
            carta::Track track;
            track.name = "Destination " + juce::String(i + 1);
            paste.tracks.push_back(std::move(track));
        }
        check(carta::pasteRange(paste, clipboard, 0, 0, &error) &&
                  paste.tracks[0].clips.size() == 1 && paste.tracks[1].clips.empty() &&
                  paste.tracks[2].clips.size() == 1,
              "pasting nonadjacent cut keeps audio on the corresponding destination rows");
        auto ripple = initial;
        carta::Clipboard rippleCopy;
        carta::Session preview;
        carta::PlaybackRange previewRange;
        check(carta::prepareIndependentRipplePreview(initial, range, {0, 2}, 10, 15, preview,
                                                     previewRange, error) &&
                  std::abs(previewRange.start * rate - 10) < 1e-8 &&
                  std::abs(previewRange.end * rate - 35) < 1e-8 &&
                  initial.tracks[0].clips.size() == 1,
              "independent ripple preview includes the requested context without editing the "
              "session");
        auto retained = preview;
        const auto oldRange = previewRange;
        check(!carta::prepareIndependentRipplePreview(initial, range, {0, 1, 2}, 10, 15, preview,
                                                      previewRange, error) &&
                  error.contains("marker") &&
                  preview.tracks[0].clips.size() == retained.tracks[0].clips.size() &&
                  std::abs(previewRange.start - oldRange.start) < 1e-12,
              "marker-invalid independent preview leaves prior outputs intact");
        check(carta::editIndependentTracks(
                  ripple, range, {2, 0}, carta::IndependentRangeEdit::cutClose, rippleCopy, error),
              "nonadjacent ripple cut succeeds when an unselected track keeps the master end");
        juce::AudioBuffer<float> previewMix(2, 25), appliedMix(2, 25);
        previewMix.clear();
        appliedMix.clear();
        preview.render(previewMix, 0, 25, previewRange.start, rate, 2);
        ripple.render(appliedMix, 0, 25, previewRange.start, rate, 2);
        for (int ch = 0; ch < 2; ++ch)
            check(std::memcmp(previewMix.getReadPointer(ch), appliedMix.getReadPointer(ch),
                              25 * sizeof(float)) == 0,
                  "independent ripple preview matches the actual full mix bit-for-bit");
        juce::AudioBuffer<float> rendered(2, 100);
        rendered.clear();
        for (int track : {0, 1, 2}) {
            rendered.clear();
            ripple.render(rendered, 0, 100, 0, rate, 2, nullptr, {track, track});
            for (int i = 0; i < 80; ++i) {
                const auto sourceIndex = (track == 1 || i < 20) ? i : i + 20;
                check(near(rendered.getSample(0, i), audio->samples.getSample(0, sourceIndex)),
                      "independent ripple retains the expected audio on selected and untouched "
                      "tracks");
            }
        }
        check(rippleCopy.tracks.size() == 3 && rippleCopy.tracks[1].empty() &&
                  std::abs(ripple.markers[0].position * rate - 90) < 1e-8,
              "independent ripple cut preserves clipboard row spacing and marker positions");
        auto inserted = initial;
        carta::Clipboard untouched = clipboard;
        check(carta::editIndependentTracks(inserted, range, {0, 2},
                                           carta::IndependentRangeEdit::insertGap, untouched,
                                           error) &&
                  inserted.tracks[0].clips.size() == 2 && inserted.tracks[1].clips.size() == 1 &&
                  inserted.tracks[2].clips.size() == 2 &&
                  untouched.lengthSamples == clipboard.lengthSamples,
              "independent insertion changes only selected rows and leaves the clipboard intact");
        rendered.clear();
        inserted.render(rendered, 0, 100, 0, rate, 2, nullptr, {0, 0});
        for (int i = 0; i < 100; ++i)
            check(near(rendered.getSample(0, i), i < 20   ? audio->samples.getSample(0, i)
                                                 : i < 40 ? 0.0f
                                                          : audio->samples.getSample(0, i - 20)),
                  "independent gap insertion retains the expected source samples");
        auto split = initial;
        carta::Clipboard unused;
        check(carta::editIndependentTracks(split, range, {0, 2},
                                           carta::IndependentRangeEdit::splitBoundaries, unused,
                                           error) &&
                  split.tracks[0].clips.size() == 3 && split.tracks[1].clips.size() == 1 &&
                  split.tracks[2].clips.size() == 3,
              "independent split affects only selected tracks");
        auto trimmed = initial;
        check(carta::editIndependentTracks(trimmed, range, {0, 2},
                                           carta::IndependentRangeEdit::trimOutside, unused,
                                           error) &&
                  trimmed.tracks[0].clips.size() == 1 &&
                  trimmed.tracks[1].clips[0].duration > trimmed.tracks[0].clips[0].duration &&
                  trimmed.tracks[2].clips.size() == 1,
              "independent trim preserves the unselected row");
        auto orphan = initial;
        check(!carta::editIndependentTracks(orphan, range, {0, 1, 2},
                                            carta::IndependentRangeEdit::trimOutside, unused,
                                            error) &&
                  error.contains("marker") && orphan.tracks[0].clips.size() == 1 &&
                  std::abs(orphan.tracks[0].clips[0].duration * rate - 100) < 1e-8,
              "independent trim rejects orphaned markers atomically");
        auto duplicated = initial;
        check(carta::editIndependentTracks(duplicated, range, {0, 2},
                                           carta::IndependentRangeEdit::duplicate, unused, error) &&
                  duplicated.tracks[0].clips.size() == 2 &&
                  duplicated.tracks[1].clips.size() == 1 && duplicated.tracks[2].clips.size() == 2,
              "independent duplication leaves the middle track untouched");
        auto newTracks = initial;
        int first = -1;
        check(carta::splitIndependentRangeToNewTracks(newTracks, range, {2, 0}, first) &&
                  first == 3 && newTracks.tracks.size() == 5 &&
                  newTracks.tracks[1].clips.size() == 1 && newTracks.tracks[3].clips.size() == 1 &&
                  newTracks.tracks[4].clips.size() == 1,
              "moving an independent range to new tracks skips unselected rows");
        {
            auto detailed = std::make_shared<carta::Audio>();
            detailed->sampleRate = 48000;
            detailed->samples.setSize(2, 1024);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 1024; ++i)
                    detailed->samples.setSample(
                        ch, i, std::sin(static_cast<float>(i) * (ch == 0 ? 0.13f : 0.19f)) * 0.2f);
            const juce::int64 base = 44100 * 4790;
            carta::Session multitrack;
            for (int t = 0; t < 24; ++t) {
                carta::Track track;
                track.name = "Source " + juce::String(t);
                track.gain = 0.25f + 0.01f * static_cast<float>(t);
                track.pan = (t % 3 - 1) * 0.1f;
                if (t != 8) {
                    track.clips.push_back({detailed, (base + 0.25) / rate, 0, 500 / rate});
                    carta::setClipEnvelope(track.clips[0], 0.7f, 30 / rate, 40 / rate);
                }
                multitrack.tracks.push_back(std::move(track));
            }
            const carta::TimeSelection selected{base + 100, base + 300, 0, 23};
            std::vector<int> ordered, reverse;
            for (int i = 0; i < 24; i += 2)
                ordered.push_back(i);
            reverse = ordered;
            std::reverse(reverse.begin(), reverse.end());
            auto sequential = multitrack;
            int referenceFirst = -1;
            for (int t : ordered) {
                int added = -1;
                if (carta::splitRangeToNewTracks(sequential, {selected.start, selected.end, t, t},
                                                 added) &&
                    referenceFirst < 0)
                    referenceFirst = added;
            }
            auto batched = multitrack;
            int batchedFirst = -1;
            check(
                carta::splitIndependentRangeToNewTracks(batched, selected, reverse, batchedFirst) &&
                    batchedFirst == referenceFirst &&
                    batched.tracks.size() == sequential.tracks.size(),
                "batched split keeps sequential row order, skipping selected rows with no clips");
            bool identical = batched.tracks.size() == sequential.tracks.size();
            if (identical)
                for (size_t t = 0; t < batched.tracks.size(); ++t) {
                    const auto& a = batched.tracks[t];
                    const auto& b = sequential.tracks[t];
                    if (a.name != b.name || a.clips.size() != b.clips.size() ||
                        !std::equal_to<float>{}(a.gain, b.gain) ||
                        !std::equal_to<float>{}(a.pan, b.pan)) {
                        identical = false;
                        break;
                    }
                    for (size_t c = 0; c < a.clips.size(); ++c)
                        if (std::memcmp(&a.clips[c].start, &b.clips[c].start, sizeof(double)) ||
                            std::memcmp(&a.clips[c].offset, &b.clips[c].offset, sizeof(double)) ||
                            std::memcmp(&a.clips[c].duration, &b.clips[c].duration,
                                        sizeof(double)) ||
                            a.clips[c].audio != b.clips[c].audio) {
                            identical = false;
                            break;
                        }
                }
            check(identical, "batched split matches sequential source geometry and mix settings at "
                             "a fractional full-disc position");
            juce::AudioBuffer<float> sequentialAudio(2, 500), batchedAudio(2, 500);
            sequentialAudio.clear();
            batchedAudio.clear();
            sequential.render(sequentialAudio, 0, 500, static_cast<double>(base) / rate, rate, 2);
            batched.render(batchedAudio, 0, 500, static_cast<double>(base) / rate, rate, 2);
            for (int ch = 0; ch < 2; ++ch)
                check(std::memcmp(sequentialAudio.getReadPointer(ch),
                                  batchedAudio.getReadPointer(ch), 500 * sizeof(float)) == 0,
                      "batched split renders identically to sequential splits with mixed-rate "
                      "faded audio");
            int untouchedFirst = 99;
            auto unchanged = multitrack;
            check(!carta::splitIndependentRangeToNewTracks(unchanged, selected, {0, 0},
                                                           untouchedFirst) &&
                      untouchedFirst == 99 && unchanged.tracks.size() == multitrack.tracks.size(),
                  "duplicate split rows reject without changing the arrangement or output index");
        }
        {
            const juce::int64 base = 44100 * 4790;
            constexpr int frames = 1000;
            for (const double sourceRate : {rate, 48000.0}) {
                auto source = std::make_shared<carta::Audio>();
                source->sampleRate = sourceRate;
                source->samples.setSize(2, 1200);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 1200; ++i)
                        source->samples.setSample(
                            ch, i,
                            static_cast<float>((i * (ch == 0 ? 173 : 263) + 71) % 997 - 498) /
                                1100.0f);
                carta::Session original;
                original.tracks.push_back(
                    {"Long source",
                     {{source, static_cast<double>(base) / rate, 0, frames / rate}}});
                check(carta::setClipEnvelope(original.tracks[0].clips[0], 0.7f, 200 / rate,
                                             150 / rate),
                      "prepare faded source for repeated split-to-track integrity check");
                juce::AudioBuffer<float> expected(2, frames), actual(2, frames);
                expected.clear();
                original.render(expected, 0, frames, static_cast<double>(base) / rate, rate, 2);
                auto edited = original;
                bool editsSucceeded = true;
                for (int i = 0; i < 40; ++i) {
                    int added = -1;
                    const auto start = base + 10 + i * 20;
                    if (!carta::splitRangeToNewTracks(edited, {start, start + 10, 0, 0}, added)) {
                        editsSucceeded = false;
                        break;
                    }
                }
                actual.clear();
                edited.render(actual, 0, frames, static_cast<double>(base) / rate, rate, 2);
                float maxError = 0;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < frames; ++i)
                        maxError = std::max(maxError, std::abs(expected.getSample(ch, i) -
                                                               actual.getSample(ch, i)));
                check(editsSucceeded && maxError < 1e-6f,
                      "repeated split-to-track edits preserve original native and resampled faded "
                      "audio at a full-disc position");
            }
        }
        auto moved = initial;
        check(!carta::moveIndependentRange(moved, range, {0, 2}, 1, 40, error) &&
                  moved.tracks[0].clips.size() == 1,
              "independent move rejects an undersized destination atomically");
        check(carta::moveIndependentRange(moved, range, {0, 2}, 0, 40, error) &&
                  moved.tracks[0].clips.size() == 3 && moved.tracks[1].clips.size() == 1 &&
                  moved.tracks[2].clips.size() == 3,
              "independent range move leaves the intervening track untouched and sums destination "
              "overlaps");
        auto stereo = std::make_shared<carta::Audio>();
        stereo->sampleRate = 48000;
        stereo->samples.setSize(2, 256);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 256; ++i)
                stereo->samples.setSample(
                    ch, i, std::sin(static_cast<float>(i) * (ch == 0 ? 0.31f : 0.47f)) * 0.5f);
        const juce::int64 base = 44100 * 4799;
        carta::Session mixed;
        for (int i = 0; i < 3; ++i)
            mixed.tracks.push_back({"Mixed " + juce::String(i + 1),
                                    {{stereo, static_cast<double>(base) / rate, 0, 200 / rate}}});
        auto changed = mixed;
        carta::Clipboard noClipboard;
        check(carta::editIndependentTracks(changed, {base + 50, base + 100, 0, 2}, {0, 2},
                                           carta::IndependentRangeEdit::closeGap, noClipboard,
                                           error),
              "nonadjacent mixed-rate ripple at a long timeline position");
        juce::AudioBuffer<float> reference(2, 200), actual(2, 150);
        for (int track : {0, 1, 2}) {
            reference.clear();
            actual.clear();
            mixed.render(reference, 0, 200, static_cast<double>(base) / rate, rate, 2, nullptr,
                         {track, track});
            changed.render(actual, 0, 150, static_cast<double>(base) / rate, rate, 2, nullptr,
                           {track, track});
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 150; ++i) {
                    const int expected = track == 1 || i < 50 ? i : i + 50;
                    check(std::abs(actual.getSample(ch, i) - reference.getSample(ch, expected)) <
                              1e-6f,
                          "nonadjacent mixed-rate ripple preserves source phase and untouched "
                          "tracks");
                }
        }
    }
    {
        carta::EditorState state;
        for (int i = 0; i < 3; ++i) {
            auto source = std::make_shared<carta::Audio>();
            source->sampleRate = carta::sessionRate;
            source->samples.setSize(1, 4);
            source->samples.clear();
            source->samples.setSample(0, 0, 0.1f * static_cast<float>(1 << i));
            state.session.tracks.push_back(
                {"Track " + juce::String(i + 1), {{source, 0, 0, 4 / carta::sessionRate}}});
        }
        const auto render = [&] {
            juce::AudioBuffer<float> out(2, 1);
            out.clear();
            state.session.render(out, 0, 1, 0, carta::sessionRate, 2);
            return out.getSample(0, 0);
        };
        check(near(render(), 0.7f), "all track rows initially contribute to mix");
        carta::EditHistory history;
        auto before = state;
        check(carta::setTrackFlag(state.session, {0, 1}, carta::TrackFlag::mute, true) &&
                  near(render(), 0.4f) && !state.session.tracks[2].muted,
              "bulk mute affects only selected rows and the rendered mix");
        check(!carta::setTrackFlag(state.session, {0, 1}, carta::TrackFlag::mute, true) &&
                  !carta::setTrackFlag(state.session, {0, 3}, carta::TrackFlag::solo, true),
              "bulk flag no-ops and invalid scopes are rejected");
        history.record(before);
        check(history.undo(state) && near(render(), 0.7f) && history.redo(state) &&
                  near(render(), 0.4f),
              "bulk mute supports undo and redo");
        check(carta::setTrackFlag(state.session, {0, 1}, carta::TrackFlag::mute, false) &&
                  carta::setTrackFlag(state.session, {1, 2}, carta::TrackFlag::solo, true) &&
                  near(render(), 0.6f),
              "bulk solo includes only soloed audible tracks");
        check(carta::setTrackFlag(state.session, {1, 1}, carta::TrackFlag::mute, true) &&
                  near(render(), 0.4f),
              "mute takes precedence within a bulk solo");
        check(carta::setTrackFlag(state.session, {1, 1}, carta::TrackFlag::solo, false) &&
                  state.session.tracks[2].solo && !state.session.tracks[1].solo,
              "bulk unsolo preserves solo outside the selected rows");
    }
    {
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = carta::sessionRate;
        source->samples.setSize(4, 100);
        source->samples.clear();
        carta::EditorState state;
        state.session.tracks.push_back(
            {"Source",
             {{source, 10 / carta::sessionRate, 4 / carta::sessionRate, 50 / carta::sessionRate}}});
        state.session.tracks.push_back(
            {"Destination", {{source, 70 / carta::sessionRate, 0, 20 / carta::sessionRate}}});
        state.session.tracks[1].gain = 0.5f;
        state.selectedTrack = 0;
        state.selectedClip = 0;
        const auto before = state;
        carta::EditHistory history;
        check(!carta::moveClipToTrack(state.session, 0, state.selectedClip, 0) &&
                  !carta::moveClipToTrack(state.session, 0, state.selectedClip, 2) &&
                  state.session.tracks[0].clips.size() == 1 && state.selectedClip == 0,
              "invalid clip transfer leaves source and selection intact");
        check(carta::moveClipToTrack(state.session, 0, state.selectedClip, 1) &&
                  state.selectedClip == 1 && state.session.tracks[0].clips.empty() &&
                  state.session.tracks[1].clips.size() == 2,
              "clip transfer appends to the selected destination");
        state.selectedTrack = 1;
        const auto& moved = state.session.tracks[1].clips[1];
        check(moved.audio == source && std::abs(moved.start * carta::sessionRate - 10) < 1e-8 &&
                  std::abs(moved.offset * carta::sessionRate - 4) < 1e-8 &&
                  std::abs(moved.duration * carta::sessionRate - 50) < 1e-8 &&
                  near(state.session.tracks[1].gain, 0.5f),
              "clip transfer preserves geometry and uses destination mix settings");
        check(!carta::clipRoutingConfirmed(state.session.tracks[1],
                                           carta::maxTrackChannels(state.session.tracks[1])),
              "unconfirmed multichannel destination remains visible to export preflight");
        history.record(before);
        check(history.undo(state) && state.selectedTrack == 0 && state.selectedClip == 0 &&
                  state.session.tracks[0].clips.size() == 1 && history.redo(state) &&
                  state.selectedTrack == 1 && state.selectedClip == 1 &&
                  state.session.tracks[1].clips.size() == 2,
              "clip track transfer supports undo and redo");
    }
    {
        const double rate = carta::sessionRate;
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = rate;
        source->samples.setSize(1, 100);
        for (int i = 0; i < 100; ++i)
            source->samples.setSample(0, i, static_cast<float>(i) / 100.0f);
        carta::EditorState state;
        state.session.tracks.push_back({"A", {{source, 0, 0, 60 / rate}}});
        state.session.tracks.push_back({"B", {{source, 20 / rate, 5 / rate, 20 / rate}}});
        state.session.tracks.push_back({"C", {{source, 0, 0, 60 / rate}}});
        state.session.markers = {{5 / rate, "One"}, {40 / rate, "Two"}};
        state.selection = {10, 30, 0, 1};
        const auto before = state;
        carta::EditHistory history;
        juce::String error;
        check(carta::duplicateRange(state.session, state.selection, error) &&
                  state.session.tracks[0].clips.size() == 2 &&
                  state.session.tracks[1].clips.size() == 2 &&
                  state.session.tracks[2].clips.size() == 1,
              "duplicate selection copies only selected rows and retains other tracks");
        check(state.session.markers.size() == 2 &&
                  std::abs(state.session.markers[1].position * rate - 40) < 1e-8 &&
                  state.selection.start == 10 && state.selection.end == 30,
              "duplicate selection keeps markers and original selection");
        juce::AudioBuffer<float> output(2, 1);
        output.clear();
        state.session.render(output, 0, 1, 32 / rate, rate, 2, nullptr, {0, 0});
        check(near(output.getSample(0, 0), 0.44f),
              "duplicate selection sums with material already at the destination");
        output.clear();
        state.session.render(output, 0, 1, 42 / rate, rate, 2, nullptr, {1, 1});
        check(near(output.getSample(0, 0), 0.07f),
              "duplicate selection preserves relative silence and source offsets");
        history.record(before);
        check(history.undo(state) && state.session.tracks[0].clips.size() == 1 &&
                  state.selection.start == 10 && history.redo(state) &&
                  state.session.tracks[0].clips.size() == 2,
              "duplicate selection supports undo and redo");
        const auto count = state.session.tracks[0].clips.size();
        check(carta::duplicateRange(state.session, {70, 80, 0, 1}, error) &&
                  state.session.tracks[0].clips.size() == count &&
                  carta::projectEndSample(state.session) == 90,
              "duplicating silence extends the master without creating clips");
        check(!carta::duplicateRange(state.session, {9007199254740971LL, 9007199254740987LL, 0, 1},
                                     error) &&
                  state.session.tracks[0].clips.size() == count,
              "duplicating beyond the precise timeline is rejected atomically");
    }
    {
        const double rate = carta::sessionRate;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(1, 100);
        for (int i = 0; i < 100; ++i)
            audio->samples.setSample(0, i, static_cast<float>(i) / 100.0f);
        carta::EditorState state;
        state.session.tracks.push_back({"A", {{audio, 0, 0, 80 / rate}}});
        state.session.tracks.push_back({"B", {{audio, 0, 0, 80 / rate}}});
        state.session.tracks.push_back({"C", {{audio, 0, 0, 80 / rate}}});
        state.session.markers = {{70 / rate, "End"}};
        state.selection = {20, 40, 0, 1};
        carta::Clipboard copied;
        copied.lengthSamples = 20;
        copied.tracks = {{{audio, 5 / rate, 10 / rate, 10 / rate}}, {}};
        const auto before = state;
        carta::EditHistory history;
        juce::String error;
        check(carta::pasteRangeReplacing(state.session, copied, 0, 20, error),
              "replace paste succeeds across rows");
        juce::AudioBuffer<float> out(2, 1);
        auto sample = [&](int track, int frame) {
            out.clear();
            state.session.render(out, 0, 1, frame / rate, rate, 2, nullptr, {track, track});
            return out.getSample(0, 0);
        };
        check(near(sample(0, 19), 0.19f) && near(sample(0, 22), 0.0f) &&
                  near(sample(0, 27), 0.12f) && near(sample(0, 36), 0.0f) &&
                  near(sample(0, 40), 0.40f),
              "replace paste preserves destination outside the span and copied internal silence");
        check(near(sample(1, 27), 0.0f) && near(sample(1, 40), 0.40f) && near(sample(2, 27), 0.27f),
              "replace paste clears silent clipboard rows but leaves other rows unchanged");
        check(state.session.markers.size() == 1 &&
                  std::abs(state.session.markers[0].position * rate - 70) < 1e-8,
              "replace paste retains markers");
        history.record(before);
        check(history.undo(state) && near(sample(0, 27), 0.27f) && history.redo(state) &&
                  near(sample(0, 27), 0.12f),
              "replace paste supports undo and redo");
        const auto count = state.session.tracks[0].clips.size();
        check(!carta::pasteRangeReplacing(state.session, copied, 2, 20, error) &&
                  state.session.tracks[0].clips.size() == count,
              "replace paste rejects insufficient destination rows atomically");
        check(!carta::pasteRangeReplacing(state.session, copied, 0, 9007199254740980LL, error) &&
                  state.session.tracks[0].clips.size() == count,
              "replace paste rejects timeline overflow atomically");
        carta::Session shortMaster;
        shortMaster.tracks.push_back({"A", {{audio, 0, 0, 80 / rate}}});
        shortMaster.markers = {{75 / rate, "Last"}};
        carta::Clipboard shortCopy;
        shortCopy.lengthSamples = 30;
        shortCopy.tracks = {{{audio, 0, 0, 5 / rate}}};
        check(carta::pasteRangeReplacing(shortMaster, shortCopy, 0, 60, error) &&
                  carta::projectEndSample(shortMaster) == 90 &&
                  std::abs(shortMaster.markers[0].position * rate - 75) < 1e-8 &&
                  shortMaster.tracks[0].clips.size() == 2,
              "replace paste retains the full span and keeps markers in its silent tail");
    }
    {
        const double rate = carta::sessionRate;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(1, 100);
        for (int i = 0; i < 100; ++i)
            audio->samples.setSample(0, i, static_cast<float>(i) / 100.0f);
        carta::EditorState state;
        for (const char* name : {"A", "B", "C"})
            state.session.tracks.push_back({name, {{audio, 0, 0, 60 / rate}}});
        state.session.markers = {{10 / rate, "Before"}, {30 / rate, "After"}};
        const auto before = state;
        carta::Clipboard copied;
        copied.lengthSamples = 20;
        copied.tracks = {{{audio, 5 / rate, 10 / rate, 10 / rate}}, {}};
        carta::EditHistory history;
        juce::String error;
        juce::AudioBuffer<float> out(2, 1);
        auto sample = [&](int track, int frame) {
            out.clear();
            state.session.render(out, 0, 1, frame / rate, rate, 2, nullptr, {track, track});
            return out.getSample(0, 0);
        };
        check(carta::pasteRangeInserting(state.session, copied, 0, 20, false, error),
              "insert paste on destination rows succeeds");
        check(near(sample(0, 19), 0.19f) && near(sample(0, 22), 0.0f) &&
                  near(sample(0, 27), 0.12f) && near(sample(0, 40), 0.20f),
              "insert paste splits destination and preserves source alignment and copied silence");
        check(near(sample(1, 27), 0.0f) && near(sample(1, 40), 0.20f) &&
                  near(sample(2, 40), 0.40f) &&
                  std::abs(state.session.markers[1].position * rate - 30) < 1e-8,
              "destination-only insertion shifts silent row, preserving other tracks and markers");
        history.record(before);
        check(history.undo(state) && near(sample(0, 40), 0.40f) && history.redo(state) &&
                  near(sample(0, 40), 0.20f),
              "insert paste is undoable and redoable");
        state = before;
        check(carta::pasteRangeInserting(state.session, copied, 0, 20, true, error) &&
                  near(sample(2, 27), 0.0f) && near(sample(2, 40), 0.20f),
              "all-track insert paste shifts tracks outside clipboard rows");
        check(std::abs(state.session.markers[0].position * rate - 10) < 1e-8 &&
                  std::abs(state.session.markers[1].position * rate - 50) < 1e-8,
              "all-track insert paste moves only later markers");
        auto invalid = before.session;
        invalid.markers.push_back(invalid.markers.back());
        check(!carta::pasteRangeInserting(invalid, copied, 0, 20, true, error) &&
                  error.isNotEmpty() && invalid.tracks[0].clips.size() == 1 &&
                  std::abs(invalid.markers[1].position * rate - 30) < 1e-8,
              "invalid all-track markers reject insert paste atomically");
        auto empty = before.session;
        for (auto& track : empty.tracks)
            track.clips.clear();
        empty.markers.clear();
        check(carta::pasteRangeInserting(empty, copied, 0, 0, true, error) &&
                  empty.tracks[0].clips.size() == 1,
              "insert paste works on an empty arrangement");
        auto beyond = before.session;
        check(carta::pasteRangeInserting(beyond, copied, 0, 80, false, error) &&
                  beyond.tracks[0].clips.size() == 2,
              "insert paste works beyond existing material");
        check(!carta::pasteRangeInserting(beyond, copied, 2, 20, false, error) &&
                  beyond.tracks[0].clips.size() == 2,
              "insert paste rejects incomplete destination rows atomically");
        check(!carta::pasteRangeInserting(beyond, copied, 0, 9007199254740980LL, false, error) &&
                  beyond.tracks[0].clips.size() == 2,
              "insert paste rejects timeline overflow atomically");
    }
    {
        const double rate = carta::sessionRate;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(1, 100);
        for (int i = 0; i < 100; ++i)
            audio->samples.setSample(0, i, static_cast<float>(i) / 100.0f);
        carta::EditorState state;
        state.session.tracks.push_back({"A", {{audio, 0, 0, 80 / rate}}});
        state.session.tracks.push_back({"B", {{audio, 0, 10 / rate, 70 / rate}}});
        state.session.tracks.push_back({"Empty", {}});
        state.session.markers = {{10 / rate, "Before"}, {50 / rate, "After"}};
        state.selection = {20, 40, 1, 1};
        state.selectedTrack = 2;
        state.selectedClip = 0;
        state.selectedMarker = 0;
        state.position = 45 / rate;
        const auto before = state;
        carta::Clipboard copied;
        juce::String error;
        carta::EditHistory history;
        check(carta::rippleCutRange(state.session, state.selection, false, copied, error) &&
                  copied.tracks.size() == 1 && copied.lengthSamples == 20 &&
                  copied.tracks[0].size() == 1,
              "selected-track ripple cut copies only selected audio");
        carta::finishRangeCut(state.selection, state.selectedTrack, state.selectedClip,
                              state.selectedMarker, state.position, false,
                              static_cast<int>(state.session.tracks.size()));
        check(state.selectedTrack == 1 && state.selectedClip == -1 && state.selectedMarker == -1 &&
                  state.selection.start == 20 && state.selection.end == 20 &&
                  std::abs(state.position * rate - 20) < 1e-8,
              "selected-track cut focuses paste at the first copied row and cut start");
        juce::AudioBuffer<float> out(2, 1);
        auto sample = [&](int track, int frame) {
            out.clear();
            state.session.render(out, 0, 1, frame / rate, rate, 2, nullptr, {track, track});
            return out.getSample(0, 0);
        };
        check(near(sample(0, 20), 0.20f) && near(sample(1, 20), 0.50f) &&
                  std::abs(state.session.markers[1].position * rate - 50) < 1e-8,
              "selected-track ripple cut closes one row and preserves markers");
        history.record(before);
        check(history.undo(state) && near(sample(1, 20), 0.30f) && state.selectedTrack == 2 &&
                  state.selection.end == 40 && history.redo(state) && near(sample(1, 20), 0.50f) &&
                  state.selectedTrack == 1 && state.selection.end == 20,
              "ripple cut restores focus and selection through undo and redo");
        state = before;
        check(carta::rippleCutRange(state.session, state.selection, true, copied, error) &&
                  copied.tracks.size() == 3 && copied.tracks[0].size() == 1 &&
                  copied.tracks[1].size() == 1 && copied.tracks[2].empty(),
              "all-track ripple cut copies all affected rows including silent rows");
        carta::finishRangeCut(state.selection, state.selectedTrack, state.selectedClip,
                              state.selectedMarker, state.position, true,
                              static_cast<int>(state.session.tracks.size()));
        check(state.selectedTrack == 0 && state.selection.firstTrack == 0 &&
                  state.selection.lastTrack == 2 && state.selection.end == 20 &&
                  std::abs(state.position * rate - 20) < 1e-8,
              "all-track cut focuses the first clipboard row for immediate paste");
        check(near(sample(0, 20), 0.40f) && near(sample(1, 20), 0.50f) &&
                  std::abs(state.session.markers[1].position * rate - 30) < 1e-8,
              "all-track ripple cut closes every row and moves later markers");
        check(carta::pasteRangeInserting(
                  state.session, copied, state.selectedTrack,
                  static_cast<juce::int64>(std::llround(state.position * rate)), true, error) &&
                  near(sample(0, 20), 0.20f) && near(sample(1, 20), 0.30f) &&
                  near(sample(0, 40), 0.40f) &&
                  std::abs(state.session.markers[1].position * rate - 50) < 1e-8,
              "all-track cut and immediate insert paste restore audio and later markers");
        auto collision = before.session;
        collision.markers = {{20 / rate, "One"}, {30 / rate, "Two"}};
        const auto savedClipboard = copied;
        check(!carta::rippleCutRange(collision, before.selection, true, copied, error) &&
                  error.contains("same sample") && collision.tracks[0].clips.size() == 1 &&
                  copied.tracks.size() == savedClipboard.tracks.size() &&
                  copied.tracks[0][0].audio == savedClipboard.tracks[0][0].audio,
              "rejected ripple cut preserves session and prior clipboard");
        check(!carta::rippleCutRange(collision, {90, 99, 0, 1}, false, copied, error) &&
                  copied.tracks.size() == savedClipboard.tracks.size(),
              "cutting silence preserves prior clipboard");
    }
    {
        const double rate = carta::sessionRate;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(1, 100);
        for (int i = 0; i < 100; ++i)
            audio->samples.setSample(0, i, static_cast<float>(i + 1) / 100.0f);
        carta::EditorState state;
        state.session.tracks.push_back({"A", {{audio, 0, 0, 60 / rate}}});
        state.session.tracks.push_back({"B", {{audio, 0, 0, 60 / rate}}});
        state.session.markers = {{50 / rate, "Later"}};
        const auto before = state;
        juce::String error;
        carta::EditHistory history;
        auto silent = carta::copyRange(state.session, {70, 80, 0, 1});
        check(!silent.empty() && !silent.hasAudio() && silent.lengthSamples == 10 &&
                  silent.tracks.size() == 2,
              "copy represents a fully silent range with its duration and rows");
        check(!carta::pasteRange(state.session, silent, 0, 10, &error) && error.contains("silent"),
              "additive paste reports that silence has no effect");
        check(carta::pasteRangeReplacing(state.session, silent, 0, 10, error),
              "replace paste applies copied silence");
        juce::AudioBuffer<float> out(2, 1);
        auto sample = [&](int track, int frame) {
            out.clear();
            state.session.render(out, 0, 1, frame / rate, rate, 2, nullptr, {track, track});
            return out.getSample(0, 0);
        };
        check(near(sample(0, 9), 0.10f) && near(sample(0, 10), 0.0f) && near(sample(1, 15), 0.0f) &&
                  near(sample(1, 20), 0.21f),
              "silent replacement clears only the target range on every row");
        history.record(before);
        check(history.undo(state) && near(sample(0, 10), 0.11f) && history.redo(state) &&
                  near(sample(0, 10), 0.0f),
              "silent replacement supports undo and redo");
        state = before;
        check(carta::pasteRangeInserting(state.session, silent, 0, 10, true, error) &&
                  near(sample(0, 10), 0.0f) && near(sample(1, 20), 0.11f) &&
                  std::abs(state.session.markers[0].position * rate - 60) < 1e-8,
              "inserting silence shifts all audio and later markers");
        auto beyond = before.session;
        check(carta::pasteRangeInserting(beyond, silent, 0, 70, true, error) &&
                  carta::projectEndSample(beyond) == 80 && beyond.tracks[0].clips.size() == 1,
              "silent insertion beyond audio extends the master");
        auto shortMaster = before.session;
        shortMaster.markers = {{59 / rate, "End"}};
        check(carta::pasteRangeReplacing(shortMaster, silent, 0, 50, error) &&
                  carta::projectEndSample(shortMaster) == 60 &&
                  std::abs(shortMaster.markers[0].position * rate - 59) < 1e-8,
              "silent replacement preserves the master end and its markers");
        {
            auto mixed = before;
            const auto mixedBefore = mixed;
            carta::EditHistory mixedHistory;
            const auto withTail = carta::copyRange(mixed.session, {40, 80, 0, 0});
            check(withTail.hasAudio() && withTail.lengthSamples == 40 &&
                      carta::pasteRange(mixed.session, withTail, 0, 60, &error) &&
                      carta::projectEndSample(mixed.session) == 100,
                  "overlap paste retains the copied audio and its trailing silence");
            juce::AudioBuffer<float> rendered(1, 40);
            rendered.clear();
            mixed.session.render(rendered, 0, 40, 60 / rate, rate, 1, nullptr, {0, 0});
            for (int i = 0; i < 20; ++i)
                check(near(rendered.getSample(0, i), static_cast<float>(i + 41) / 100.0f),
                      "pasted audio preserves source samples");
            for (int i = 20; i < 40; ++i)
                check(near(rendered.getSample(0, i), 0.0f), "pasted tail remains silent");
            mixedHistory.record(mixedBefore, "Paste");
            check(mixedHistory.undo(mixed) && carta::projectEndSample(mixed.session) == 60 &&
                      mixedHistory.redo(mixed) && carta::projectEndSample(mixed.session) == 100,
                  "undo and redo restore the complete pasted span");
            const auto oldEnd = carta::projectEndSample(mixed.session);
            check(!carta::pasteRange(mixed.session, silent, 0, 9007199254740988LL, &error) &&
                      carta::projectEndSample(mixed.session) == oldEnd,
                  "silent paste past the precise timeline limit is rejected atomically");
            auto noChange = before.session;
            check(!carta::pasteRange(noChange, silent, 0, 10, &error) &&
                      carta::projectEndSample(noChange) == 60,
                  "overlapping silence already inside the master is a no-op");
        }
        auto gap = before.session;
        gap.tracks[0].clips = {{audio, 20 / rate, 0, 40 / rate}};
        gap.markers.clear();
        carta::Clipboard copied;
        check(carta::rippleCutRange(gap, {0, 10, 0, 0}, false, copied, error) && !copied.empty() &&
                  !copied.hasAudio() && std::abs(gap.tracks[0].clips[0].start * rate - 10) < 1e-8,
              "ripple cut can copy silence while closing time before later audio");
    }
    {
        const double rate = carta::sessionRate;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(2, 100);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 100; ++i)
                audio->samples.setSample(ch, i, static_cast<float>(i + ch * 10) / 100.0f);
        carta::EditorState state;
        state.session.tracks.push_back({"A", {{audio, 0, 0, 60 / rate}}});
        state.session.tracks.push_back({"B", {{audio, 0, 10 / rate, 60 / rate}}});
        state.session.tracks.push_back({"C", {{audio, 0, 0, 60 / rate}}});
        state.session.markers = {{50 / rate, "Track"}};
        state.selection = {10, 30, 0, 1};
        const auto before = state;
        carta::EditHistory history;
        juce::String error;
        check(carta::moveRange(state.session, state.selection, 1, 20, error),
              "move time selection across overlapping destination rows");
        juce::AudioBuffer<float> output(2, 1);
        auto sample = [&](int track, int channel, int frame) {
            output.clear();
            state.session.render(output, 0, 1, frame / rate, rate, 2, nullptr, {track, track});
            return output.getSample(channel, 0);
        };
        check(near(sample(0, 0, 15), 0.0f) && near(sample(0, 0, 30), 0.30f),
              "move leaves a gap only in selected source time");
        check(near(sample(1, 0, 22), 0.12f) && near(sample(1, 1, 22), 0.22f) &&
                  near(sample(1, 0, 35), 0.70f),
              "move preserves stereo alignment and sums destination overlaps");
        check(near(sample(2, 0, 22), 0.44f) && near(sample(2, 1, 22), 0.64f) &&
                  std::abs(state.session.markers[0].position * rate - 50) < 1e-8,
              "move keeps second selected row aligned and markers fixed");
        history.record(before);
        check(history.undo(state) && near(sample(0, 0, 15), 0.15f) && history.redo(state) &&
                  near(sample(0, 0, 15), 0.0f),
              "range move supports undo and redo");
        auto invalid = before.session;
        check(!carta::moveRange(invalid, before.selection, 2, 20, error) &&
                  invalid.tracks[0].clips.size() == 1,
              "range move rejects insufficient rows without mutation");
        check(!carta::moveRange(invalid, before.selection, 0, 9007199254740980LL, error) &&
                  invalid.tracks[0].clips.size() == 1,
              "range move rejects timeline overflow without mutation");
        check(!carta::moveRange(invalid, before.selection, 0, 10, error) &&
                  invalid.tracks[0].clips.size() == 1,
              "range move rejects unchanged destination");
        carta::Session shortMaster;
        shortMaster.tracks.push_back({"A", {{audio, 0, 0, 60 / rate}}});
        shortMaster.markers = {{55 / rate, "Late"}};
        check(!carta::moveRange(shortMaster, {40, 60, 0, 0}, 0, 10, error) &&
                  error.contains("marker") && shortMaster.tracks[0].clips.size() == 1,
              "range move rejects orphaned markers without changing audio");
    }
    {
        const double rate = carta::sessionRate;
        auto stereo = std::make_shared<carta::Audio>();
        stereo->sampleRate = rate;
        stereo->samples.setSize(2, 8);
        for (int i = 0; i < 8; ++i) {
            stereo->samples.setSample(0, i, 0.2f);
            stereo->samples.setSample(1, i, 0.7f);
        }
        carta::EditorState state;
        state.session.tracks.push_back({"Stereo", {{stereo, 0, 0, 8 / rate}}});
        const auto before = state;
        carta::EditHistory history;
        juce::String error;
        check(carta::swapStereoSourceChannels(state.session, 0, error),
              "swap stereo source channels");
        juce::AudioBuffer<float> out(2, 1);
        auto render = [&](int channels) {
            out.clear();
            state.session.render(out, 0, 1, 0, rate, channels);
        };
        render(2);
        check(near(out.getSample(0, 0), 0.7f) && near(out.getSample(1, 0), 0.2f),
              "stereo routing swaps left and right source signals");
        render(1);
        check(near(out.getSample(0, 0), 0.45f), "stereo source swap retains mono fold-down");
        history.record(before);
        check(history.undo(state) && state.session.tracks[0].routing.empty() &&
                  history.redo(state) && state.session.tracks[0].routing.size() == 2,
              "stereo source swap supports undo and redo");
        state.session.tracks[0].routing = {{0.2f, 0.3f}, {0.5f, 0.7f}};
        check(carta::swapStereoSourceChannels(state.session, 0, error) &&
                  near(state.session.tracks[0].routing[0].left, 0.5f) &&
                  near(state.session.tracks[0].routing[1].right, 0.3f),
              "source swap preserves custom routing coefficients");
        state.session.tracks[0].routing = {{0.5f, 0.5f}, {0.5f, 0.5f}};
        check(!carta::swapStereoSourceChannels(state.session, 0, error),
              "identical routing rows are a no-op");
        state = before;
        const auto unfolded = state;
        carta::EditHistory monoHistory;
        check(carta::foldStereoTrackToMono(state.session, 0, error),
              "fold stereo track routing to mono");
        render(2);
        check(near(out.getSample(0, 0), 0.45f) && near(out.getSample(1, 0), 0.45f),
              "mono track fold averages stereo channels to both outputs");
        render(1);
        check(near(out.getSample(0, 0), 0.45f),
              "mono track fold preserves centered master mono mix");
        monoHistory.record(unfolded);
        check(monoHistory.undo(state) && state.session.tracks[0].routing.empty() &&
                  monoHistory.redo(state) && state.session.tracks[0].routing.size() == 2,
              "mono track fold supports undo and redo");
        check(!carta::foldStereoTrackToMono(state.session, 0, error) && error.contains("already"),
              "repeated mono fold is a no-op");
        state.session.tracks[0].routing = {{0.2f, 0.3f}, {0.5f, 0.7f}};
        check(carta::foldStereoTrackToMono(state.session, 0, error) &&
                  near(state.session.tracks[0].routing[0].left, 0.25f) &&
                  near(state.session.tracks[0].routing[0].right, 0.25f) &&
                  near(state.session.tracks[0].routing[1].left, 0.6f) &&
                  near(state.session.tracks[0].routing[1].right, 0.6f),
              "mono track fold averages existing custom routing coefficients");
        auto mono = std::make_shared<carta::Audio>();
        mono->sampleRate = rate;
        mono->samples.setSize(1, 8);
        state.session.tracks[0].clips.push_back({mono, 0, 0, 8 / rate});
        check(!carta::swapStereoSourceChannels(state.session, 0, error) && error.isNotEmpty() &&
                  near(state.session.tracks[0].routing[0].left, 0.25f),
              "mixed channel layouts reject swapping without mutation");
        check(!carta::foldStereoTrackToMono(state.session, 0, error) && error.isNotEmpty() &&
                  near(state.session.tracks[0].routing[0].left, 0.25f),
              "mixed channel layouts reject mono fold without mutation");
    }
    {
        const double rate = carta::sessionRate;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(2, 8);
        for (int i = 0; i < 8; ++i) {
            audio->samples.setSample(0, i, 0.2f);
            audio->samples.setSample(1, i, -0.6f);
        }
        carta::Session mix;
        mix.tracks.push_back({"Stereo", {{audio, 0, 0, 8 / rate}}});
        carta::MixLevels levels;
        juce::String error;
        double previous = -1;
        bool monotonic = true;
        check(carta::scanMixLevels(mix, 2, {0, 16}, levels, error,
                                   [&](double p) {
                                       if (p < previous)
                                           monotonic = false;
                                       previous = p;
                                       return true;
                                   }) &&
                  monotonic && std::abs(previous - 1) < 1e-12,
              "mix-level scan reports monotonic completion");
        check(levels.channels == 2 && near(levels.peak[0], 0.2f) && near(levels.peak[1], 0.6f) &&
                  std::abs(levels.rms[0] - 0.2 / std::sqrt(2.0)) < 1e-6 &&
                  std::abs(levels.rms[1] - 0.6 / std::sqrt(2.0)) < 1e-6,
              "level scan measures per-channel peak and RMS including trailing silence");
        check(carta::scanMixLevels(mix, 1, {0, 8}, levels, error) && levels.channels == 1 &&
                  near(levels.peak[0], 0.2f) && std::abs(levels.rms[0] - 0.2) < 1e-6,
              "level scan uses the mono master fold-down");
        auto outsideSolo = std::make_shared<carta::Audio>();
        outsideSolo->sampleRate = rate;
        outsideSolo->samples.setSize(2, 8);
        outsideSolo->samples.clear();
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 8; ++i)
                outsideSolo->samples.setSample(ch, i, 0.9f);
        mix.tracks.push_back({"Other", {{outsideSolo, 0, 0, 8 / rate}}});
        mix.tracks[1].solo = true;
        auto scoped = mix;
        check(carta::limitToTracks(scoped, 0, 0) &&
                  carta::scanMixLevels(scoped, 2, {0, 8}, levels, error) &&
                  near(levels.peak[0], 0.2f) && near(levels.peak[1], 0.6f),
              "selected-track level scan ignores solo outside its scope");
        check(carta::scanMixLevels(mix, 2, {0, 8}, levels, error) && near(levels.peak[0], 0.9f),
              "full-mix analysis retains ordinary solo behavior");
        check(carta::validateMixRange(mix, 2, {0, 2000000000LL}, error) &&
                  !carta::validateWavRange(mix, 2, {0, 2000000000LL}, error,
                                           {carta::WavEncoding::pcm16, false}),
              "read-only analysis accepts ranges longer than classic WAV allows");
        mix.tracks.pop_back();
        const auto before = levels;
        check(
            !carta::scanMixLevels(mix, 2, {0, 8}, levels, error, [](double p) { return p < 1; }) &&
                error.contains("cancelled") && levels.channels == before.channels &&
                near(levels.peak[0], before.peak[0]),
            "cancelled level scan preserves previous result");
        audio->samples.setSample(0, 3, std::numeric_limits<float>::quiet_NaN());
        check(!carta::scanMixLevels(mix, 2, {0, 8}, levels, error) &&
                  error.contains("non-finite") && levels.channels == before.channels,
              "invalid mix preserves previous level result");
    }
    {
        const double rate = carta::sessionRate;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(2, 6000);
        audio->samples.clear();
        audio->samples.setSample(0, 10, 1.01f);
        audio->samples.setSample(0, 4100, -1.2f);
        audio->samples.setSample(0, 5000, 1.0f);
        audio->samples.setSample(0, 4101, 1.2f);
        audio->samples.setSample(1, 4098, -1.1f);
        audio->samples.setSample(1, 4099, 1.5f);
        carta::Session mix;
        mix.tracks.push_back({"Stereo", {{audio, 5 / rate, 0, 6000 / rate}}});
        carta::MixLevels levels;
        juce::String error;
        check(carta::scanMixLevels(mix, 2, {8, 5010}, levels, error) &&
                  levels.aboveFullScale[0] == 3 && levels.firstAboveFullScale[0] == 15 &&
                  levels.aboveFullScale[1] == 2 && levels.firstAboveFullScale[1] == 4103 &&
                  levels.peakSample[0] == 4105 && levels.peakSample[1] == 4104,
              "level analysis reports absolute overload and first maximum positions across blocks");
        check(carta::scanMixLevels(mix, 2, {16, 4105}, levels, error) &&
                  levels.aboveFullScale[0] == 0 && levels.firstAboveFullScale[0] == -1 &&
                  levels.aboveFullScale[1] == 2 && levels.firstAboveFullScale[1] == 4103,
              "level analysis respects exclusive selection boundaries");
        check(carta::scanMixLevels(mix, 2, {16, 4106}, levels, error) &&
                  levels.aboveFullScale[0] == 1 && levels.firstAboveFullScale[0] == 4105,
              "extending the range includes the boundary sample");
        check(carta::scanMixLevels(mix, 2, {0, 5}, levels, error) && levels.peakSample[0] == -1 &&
                  levels.peakSample[1] == -1,
              "silent output has no nonzero peak position");
    }
    {
        const double rate = carta::sessionRate;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(2, 100);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 100; ++i)
                audio->samples.setSample(ch, i, static_cast<float>(i + ch * 10) / 100.0f);
        carta::EditorState state;
        state.session.tracks.push_back(
            {"One", {{audio, 0, 0, 20 / rate}, {audio, 30 / rate, 5 / rate, 20 / rate}}});
        state.session.tracks.push_back(
            {"Two",
             {{audio, 5 / rate, 10 / rate, 20 / rate}, {audio, 50 / rate, 20 / rate, 30 / rate}}});
        state.session.tracks.push_back({"Three", {}});
        state.session.markers = {{70 / rate, "Late"}};
        state.session.tracks[0].clips[1].name = "Moved";
        state.session.tracks[0].clips[1].gain = 0.5f;
        const auto before = state;
        carta::EditHistory history;
        juce::String error;
        const std::vector<carta::ClipReference> group{{0, 1}, {1, 0}};
        {
            carta::Session units;
            units.tracks.push_back({"A",
                                    {{audio, 10 / rate, 0, 10 / rate},
                                     {audio, 35 / rate, 0, 5 / rate},
                                     {audio, 90 / rate, 0, 5 / rate}}});
            units.tracks.push_back(
                {"B", {{audio, 12.25 / rate, 0, 7 / rate}, {audio, 60 / rate, 0, 8 / rate}}});
            units.tracks.push_back({"C", {{audio, 63 / rate, 0, 4 / rate}}});
            check(carta::groupClips(units, {{0, 0}, {1, 0}}, error) &&
                      carta::groupClips(units, {{1, 1}, {2, 0}}, error),
                  "prepare two persistent groups for sequencing");
            const std::vector<carta::ClipReference> partial{{0, 0}, {0, 1}, {1, 1}, {0, 2}};
            carta::Session preparedArrangement;
            check(carta::prepareSequenceClipGroup(units, preparedArrangement, partial, error) &&
                      std::abs(units.tracks[0].clips[1].start * rate - 35) < 1e-8 &&
                      std::abs(preparedArrangement.tracks[0].clips[1].start * rate - 20) < 1e-8,
                  "sequence preparation leaves the source arrangement unchanged");
            check(!carta::prepareDistributeClipGroup(units, preparedArrangement, {{0, 0}}, error) &&
                      std::abs(preparedArrangement.tracks[0].clips[1].start * rate - 20) < 1e-8,
                  "failed arrangement preparation preserves the previous output");
            check(carta::prepareDistributeClipGroup(units, preparedArrangement, partial, error) &&
                      std::abs(units.tracks[0].clips[1].start * rate - 35) < 1e-8 &&
                      std::abs(preparedArrangement.tracks[0].clips[1].start * rate - 39) < 1e-8,
                  "distribution preparation leaves the source arrangement unchanged");
            auto sequenced = units;
            check(carta::sequenceClipGroup(sequenced, partial, error),
                  "sequence persistent groups and standalone clips as units");
            check(std::abs(sequenced.tracks[0].clips[0].start * rate - 10) < 1e-8 &&
                      std::abs(sequenced.tracks[1].clips[0].start * rate - 12.25) < 1e-8 &&
                      std::abs(sequenced.tracks[0].clips[1].start * rate - 20) < 1e-8 &&
                      std::abs(sequenced.tracks[1].clips[1].start * rate - 25) < 1e-8 &&
                      std::abs(sequenced.tracks[2].clips[0].start * rate - 28) < 1e-8 &&
                      std::abs(sequenced.tracks[0].clips[2].start * rate - 33) < 1e-8,
                  "sequencing preserves internal overlaps and offsets while closing gaps between "
                  "units");
            check(sequenced.tracks[1].clips[1].groupId == units.tracks[1].clips[1].groupId &&
                      std::memcmp(&sequenced.tracks[2].clips[0].offset,
                                  &units.tracks[2].clips[0].offset, sizeof(double)) == 0,
                  "sequencing preserves group identity and source geometry");
            auto spaced = units;
            check(carta::distributeClipGroup(spaced, partial, error),
                  "distribute persistent groups and standalone clips as units");
            check(std::abs(spaced.tracks[0].clips[0].start * rate - 10) < 1e-8 &&
                      std::abs(spaced.tracks[1].clips[0].start * rate - 12.25) < 1e-8 &&
                      std::abs(spaced.tracks[0].clips[1].start * rate - 39) < 1e-8 &&
                      std::abs(spaced.tracks[1].clips[1].start * rate - 63) < 1e-8 &&
                      std::abs(spaced.tracks[2].clips[0].start * rate - 66) < 1e-8 &&
                      std::abs(spaced.tracks[0].clips[2].start * rate - 90) < 1e-8,
                  "distribution fixes endpoint units and spaces their bounding spans equally");
            auto singleton = units;
            check(!carta::sequenceClipGroup(singleton, {{0, 0}}, error) &&
                      !carta::distributeClipGroup(singleton, {{0, 0}}, error) &&
                      std::abs(singleton.tracks[1].clips[0].start * rate - 12.25) < 1e-8,
                  "a single persistent group is never sequenced or distributed internally");
            auto tooTight = units;
            tooTight.tracks[1].clips[0].duration = 70 / rate;
            check(!carta::distributeClipGroup(tooTight, {{0, 0}, {0, 1}, {1, 1}}, error) &&
                      error.contains("overlap") &&
                      std::abs(tooTight.tracks[1].clips[1].start * rate - 60) < 1e-8,
                  "overlapping unit spans reject distribution atomically");
            auto protectedEnd = units;
            protectedEnd.markers.push_back({89 / rate, "End"});
            check(!carta::sequenceClipGroup(protectedEnd, partial, error) &&
                      error.contains("marker") &&
                      std::abs(protectedEnd.tracks[0].clips[2].start * rate - 90) < 1e-8,
                  "sequencing groups preserves valid markers atomically");
        }
        {
            carta::EditorState arrangement;
            arrangement.session.tracks.push_back({"A",
                                                  {{audio, 0, 5 / rate, 10 / rate},
                                                   {audio, 50 / rate, 20 / rate, 8 / rate},
                                                   {audio, 70 / rate, 0, 10 / rate}}});
            arrangement.session.tracks.push_back({"B", {{audio, 5 / rate, 10 / rate, 5.5 / rate}}});
            arrangement.session.tracks[1].clips[0].gain = 0.6f;
            arrangement.session.markers = {{15 / rate, "inside"}};
            const auto original = arrangement;
            const std::vector<carta::ClipReference> sequence{{0, 1}, {1, 0}, {0, 0}};
            carta::EditHistory sequenceHistory;
            sequenceHistory.record(arrangement);
            check(carta::sequenceClipGroup(arrangement.session, sequence, error),
                  "sequence selected clips in timeline order across tracks");
            const auto& a = arrangement.session.tracks[0].clips;
            const auto& b = arrangement.session.tracks[1].clips;
            check(std::abs(b[0].start * rate - 10) < 1e-8 &&
                      std::abs(a[1].start * rate - 15.5) < 1e-8 &&
                      std::abs(a[2].start * rate - 70) < 1e-8,
                  "sequencing closes selected gaps and overlaps, preserves fractional lengths and "
                  "leaves unselected clips in place");
            check(near(b[0].gain, 0.6f) &&
                      std::memcmp(&b[0].offset, &original.session.tracks[1].clips[0].offset,
                                  sizeof(double)) == 0 &&
                      arrangement.session.markers.size() == 1,
                  "sequencing retains source, gain and markers");
            check(!carta::sequenceClipGroup(arrangement.session, sequence, error) &&
                      error.contains("already"),
                  "already consecutive group creates no edit");
            check(sequenceHistory.undo(arrangement) &&
                      std::abs(arrangement.session.tracks[0].clips[1].start * rate - 50) < 1e-8 &&
                      sequenceHistory.redo(arrangement) &&
                      std::abs(arrangement.session.tracks[0].clips[1].start * rate - 15.5) < 1e-8,
                  "sequencing supports undo and redo");
            auto invalid = original.session;
            check(!carta::sequenceClipGroup(invalid, {{0, 0}, {0, 0}}, error) &&
                      std::abs(invalid.tracks[0].clips[1].start * rate - 50) < 1e-8,
                  "duplicate sequence references fail atomically");
            auto protectedEnd = original.session;
            protectedEnd.tracks[0].clips.pop_back();
            protectedEnd.markers = {{55 / rate, "end"}};
            check(!carta::sequenceClipGroup(protectedEnd, sequence, error) &&
                      error.contains("marker") &&
                      std::abs(protectedEnd.tracks[0].clips[1].start * rate - 50) < 1e-8,
                  "sequencing refuses to orphan a marker and leaves clips intact");
            auto distributed = original;
            carta::EditHistory distributeHistory;
            distributeHistory.record(distributed, "Distribute clips");
            check(carta::distributeClipGroup(distributed.session, sequence, error),
                  "distribute nonadjacent clips across tracks");
            check(std::abs(distributed.session.tracks[1].clips[0].start * rate - 27.25) < 1e-8 &&
                      std::abs(distributed.session.tracks[0].clips[0].start * rate) < 1e-8 &&
                      std::abs(distributed.session.tracks[0].clips[1].start * rate - 50) < 1e-8 &&
                      std::abs(distributed.session.tracks[0].clips[2].start * rate - 70) < 1e-8,
                  "distribution fixes endpoint clips and places the middle clip at equal "
                  "fractional gaps without moving unselected audio");
            check(near(distributed.session.tracks[1].clips[0].gain, 0.6f) &&
                      std::memcmp(&distributed.session.tracks[1].clips[0].offset,
                                  &original.session.tracks[1].clips[0].offset,
                                  sizeof(double)) == 0 &&
                      distributed.session.markers.size() == 1,
                  "distribution preserves source offset, gain and markers");
            check(!carta::distributeClipGroup(distributed.session, sequence, error) &&
                      error.contains("equal gaps"),
                  "repeating equal distribution does not create an edit");
            check(distributeHistory.undo(distributed) &&
                      std::abs(distributed.session.tracks[1].clips[0].start * rate - 5) < 1e-8 &&
                      distributeHistory.redo(distributed) &&
                      std::abs(distributed.session.tracks[1].clips[0].start * rate - 27.25) < 1e-8,
                  "distribution supports undo and redo");
            auto tooTight = original.session;
            tooTight.tracks[0].clips[1].start = 12 / rate;
            check(!carta::distributeClipGroup(tooTight, sequence, error) &&
                      error.contains("overlap") &&
                      std::abs(tooTight.tracks[1].clips[0].start * rate - 5) < 1e-8,
                  "distribution rejects an insufficient span atomically");
            check(!carta::distributeClipGroup(tooTight, {{0, 0}, {0, 0}, {1, 0}}, error) &&
                      !carta::distributeClipGroup(tooTight, {{0, 0}, {1, 0}}, error),
                  "distribution rejects duplicate references and too few clips");
        }
        check(carta::validateClipGroupMove(state.session, group, 10, 1, error),
              "group drag preview accepts valid time and track offsets");
        check(!carta::validateClipGroupMove(state.session, group, -100, 0, error) &&
                  !carta::validateClipGroupMove(state.session, group, 0, 2, error),
              "group drag preview rejects negative time and nonexistent destinations");
        check(!carta::validateClipGroupMove(state.session, {{0, 1}, {0, 1}}, 0, 0, error),
              "group drag preview rejects duplicate clip references");
        carta::ClipGroupMovePreview preview;
        check(preview.prepare(state.session, group, error), "prepare immutable group drag preview");
        for (const juce::int64 delta :
             {-100LL, -30LL, -5LL, 0LL, 10LL, 100LL, std::numeric_limits<juce::int64>::max(),
              std::numeric_limits<juce::int64>::min()})
            for (const juce::int64 rows : {-2LL, -1LL, 0LL, 1LL, 2LL}) {
                juce::String expectedError, previewError;
                const bool expected = carta::validateClipGroupMove(state.session, group, delta,
                                                                   rows, expectedError),
                           actual = preview.validate(delta, rows, previewError);
                check(expected == actual && expectedError == previewError,
                      "prepared group preview agrees with full validation across bounds and "
                      "extreme offsets");
            }
        check(!preview.prepare(state.session, {{0, 1}, {0, 1}}, error) &&
                  preview.validate(10, 1, error),
              "invalid preview preparation retains the previous valid plan");
        const auto snapTargets = carta::clipGroupSnapTargets(state.session, {}, 12 / rate, group);
        const auto hasTarget = [&](double target) {
            return std::any_of(snapTargets.begin(), snapTargets.end(), [&](double value) {
                return std::abs(value - target / rate) < 1e-10;
            });
        };
        check(!hasTarget(30) && !hasTarget(5) && hasTarget(50) && hasTarget(70) && hasTarget(12),
              "group drag excludes selected edges but retains other clip edges, markers and "
              "playhead");
        check(std::is_sorted(snapTargets.begin(), snapTargets.end()) &&
                  std::adjacent_find(snapTargets.begin(), snapTargets.end()) == snapTargets.end(),
              "group snap targets are sorted and unique");
        const auto snapped =
            carta::snapClipGroupMove(30 / rate, 20 / rate, 9, 2 / rate, {60 / rate});
        const auto unsnapped =
            carta::snapClipGroupMove(30 / rate, 20 / rate, 9, 0.1 / rate, {60 / rate});
        check(
            snapped.hasGuide && snapped.delta == 10 && std::abs(snapped.guide * rate - 60) < 1e-8 &&
                !unsnapped.hasGuide && unsnapped.delta == 9,
            "group drag snaps the anchor edge by a shared integer-sample offset within tolerance");
        const std::vector<double> targets{0 / rate,    11 / rate, 33.3 / rate,
                                          59.8 / rate, 81 / rate, 120 / rate};
        for (juce::int64 request = -60; request <= 120; ++request) {
            const double anchor = 30.25 / rate, length = 19.5 / rate, tolerance = 2.4 / rate;
            double best = tolerance;
            bool found = false;
            for (const double target : targets)
                for (const double edge : {0.0, length}) {
                    const auto delta =
                        static_cast<juce::int64>(std::llround((target - anchor - edge) * rate));
                    const double distance =
                        std::abs(anchor + edge + static_cast<double>(request) / rate - target);
                    const double residual =
                        std::abs(anchor + edge + static_cast<double>(delta) / rate - target);
                    if (distance <= best && residual <= 0.5 / rate + 1e-10) {
                        best = distance;
                        found = true;
                    }
                }
            const auto result =
                carta::snapClipGroupMove(anchor, length, request, tolerance, targets);
            check(result.hasGuide == found,
                  "binary-search group snap agrees with exhaustive target search");
            if (found) {
                const double a = std::abs(anchor + static_cast<double>(request) / rate -
                                          result.guide),
                             b = std::abs(anchor + length + static_cast<double>(request) / rate -
                                          result.guide);
                check(std::abs(std::min(a, b) - best) < 1e-10,
                      "binary-search group snap chooses an equally near guide");
            }
        }
        carta::ClipSelection timelineGroup;
        check(timelineGroup.toggle(state.session, group[0], 10) &&
                  timelineGroup.toggle(state.session, group[1], 10) &&
                  timelineGroup.get(10).size() == 2 && timelineGroup.contains(0, 1, 10) &&
                  timelineGroup.contains(1, 0, 10),
              "timeline clip selection toggles clips on independent tracks");
        check(
            carta::describeClipSelection(timelineGroup, 10, 0, 1)
                    .contains("2 clips selected. Focused clip is selected") &&
                carta::describeClipSelection(timelineGroup, 10, 0, 0)
                    .contains("Focused clip is not selected") &&
                carta::describeClipSelection(timelineGroup, 10, 1, -1).contains("No clip focused"),
            "accessible group summary distinguishes focused and selected clips");
        check(!timelineGroup.toggle(state.session, {9, 0}, 10) && timelineGroup.get(10).size() == 2,
              "invalid timeline clip selection preserves existing selection");
        check(timelineGroup.toggle(state.session, group[0], 10) &&
                  timelineGroup.get(10).size() == 1 && !timelineGroup.contains(0, 1, 10),
              "timeline clip toggle removes only the chosen clip");
        check(timelineGroup.get(11).empty() && !timelineGroup.contains(1, 0, 11),
              "project revision invalidates old clip indices");
        check(carta::describeClipSelection(timelineGroup, 11, 1, 0) == "No clip group selected.",
              "accessible group summary omits stale selections");
        check(timelineGroup.toggle(state.session, group[0], 11) &&
                  timelineGroup.get(11).size() == 1 && !timelineGroup.contains(1, 0, 11),
              "selection after an edit starts fresh");
        timelineGroup.clear();
        check(timelineGroup.get(11).empty(), "timeline clip selection can be cleared");
        {
            carta::ClipSelection selectedClips;
            carta::TimeSelection current{5, 15, 0, 0};
            check(selectedClips.toggle(state.session, group[0], 20) &&
                      selectedClips.toggle(state.session, group[1], 20),
                  "prepare overlapping clip-group and range focus");
            check(!carta::activateTimeSelection(current, selectedClips, {10, 10, 0, 0}, 3) &&
                      current.start == 5 && current.end == 15 && selectedClips.get(20).size() == 2,
                  "failed empty range selection preserves both previous selections");
            check(!carta::activateTimeSelection(current, selectedClips, {10, 20, 0, 3}, 3) &&
                      selectedClips.get(20).size() == 2,
                  "failed out-of-bounds track range preserves clip group");
            check(carta::activateTimeSelection(current, selectedClips, {10, 20, 0, 1}, 3) &&
                      current.start == 10 && current.end == 20 && current.firstTrack == 0 &&
                      current.lastTrack == 1 && selectedClips.get(20).empty(),
                  "successful explicit range selection clears whole-clip editing focus");
            check(carta::clipGroupShortcut(0, true, true, false, !selectedClips.get(20).empty()) ==
                          carta::ClipGroupShortcut::none &&
                      carta::clipGroupShortcut('C', false, false, true,
                                               !selectedClips.get(20).empty()) ==
                          carta::ClipGroupShortcut::none,
                  "delete and copy no longer target the former clip group");
            check(selectedClips.toggle(state.session, group[0], 20) && current.start == 10 &&
                      current.end == 20 &&
                      carta::clipGroupShortcut(0, true, true, false,
                                               !selectedClips.get(20).empty()) ==
                          carta::ClipGroupShortcut::remove,
                  "selecting a group afterward preserves the range and restores clip-group "
                  "shortcuts");
        }

        {
            carta::ClipSelection bulk;
            check(bulk.selectTracks(state.session, {0, 2}, nullptr, 30) &&
                      bulk.get(30).size() == 4 && bulk.contains(0, 0, 30) &&
                      bulk.contains(1, 1, 30),
                  "bulk selection includes every clip across tracks and skips empty tracks");
            check(bulk.selectTracks(state.session, {0, 0}, nullptr, 30) &&
                      bulk.get(30).size() == 2 && !bulk.contains(1, 0, 30),
                  "track-scoped bulk selection replaces the prior group");
            const carta::TimeSelection range{20, 30, 0, 1};
            check(bulk.selectTracks(state.session, {0, 1}, &range, 30) &&
                      bulk.get(30).size() == 1 && bulk.contains(1, 0, 30),
                  "bulk time selection uses half-open intersections and excludes touching edges");
            const carta::TimeSelection empty{80, 90, 0, 1};
            check(!bulk.selectTracks(state.session, {0, 1}, &empty, 30) &&
                      bulk.get(30).size() == 1 && bulk.contains(1, 0, 30),
                  "empty bulk result preserves the prior group");
            check(!bulk.selectTracks(state.session, {0, 3}, nullptr, 30) && bulk.contains(1, 0, 30),
                  "invalid track scope preserves the prior group");
            check(bulk.toggle(state.session, {1, 0}, 30) && bulk.get(30).empty() &&
                      !bulk.contains(1, 0, 30),
                  "toggling a bulk-selected clip updates membership");
            check(bulk.selectTracks(state.session, {1, 1}, nullptr, 31) && bulk.get(30).empty() &&
                      bulk.get(31).size() == 2 && bulk.contains(1, 1, 31),
                  "bulk selection replaces stale revisions and maintains membership");
        }
        carta::ClipSelection nudgeSelection;
        check(nudgeSelection.toggle(before.session, group[0], 20) &&
                  nudgeSelection.toggle(before.session, group[1], 20),
              "prepare persistent group nudge selection");
        auto nudged = before;
        carta::EditHistory nudgeHistory;
        for (juce::uint64 revision = 20; revision < 22; ++revision) {
            auto old = nudged;
            check(carta::moveClipGroup(nudged.session, nudgeSelection.get(revision), 1, 0, error),
                  "nudge selected clips one project sample");
            nudgeHistory.record(std::move(old));
            check(nudgeSelection.retainAfterStableEdit(revision, revision + 1) &&
                      nudgeSelection.get(revision + 1).size() == 2,
                  "stable horizontal nudge keeps the group for the next command");
        }
        check(std::abs(nudged.session.tracks[0].clips[1].start * rate - 32) < 1e-8 &&
                  std::abs(nudged.session.tracks[1].clips[0].start * rate - 7) < 1e-8 &&
                  std::abs(nudged.session.markers[0].position * rate - 70) < 1e-8,
              "repeated group nudges preserve clip offsets and markers");
        check(!nudgeSelection.retainAfterStableEdit(20, 23) && nudgeSelection.get(22).size() == 2,
              "a stale revision cannot restore selected indices");
        check(nudgeHistory.undo(nudged) &&
                  std::abs(nudged.session.tracks[0].clips[1].start * rate - 31) < 1e-8 &&
                  nudgeHistory.undo(nudged) &&
                  std::abs(nudged.session.tracks[0].clips[1].start * rate - 30) < 1e-8,
              "repeated group nudges are individually undoable");
        using Shortcut = carta::ClipGroupShortcut;
        check(carta::clipGroupShortcut('C', false, false, true, true) == Shortcut::copy &&
                  carta::clipGroupShortcut('X', false, false, true, true) == Shortcut::cut &&
                  carta::clipGroupShortcut('D', false, false, true, true) == Shortcut::duplicate &&
                  carta::clipGroupShortcut('B', false, false, true, true) == Shortcut::split,
              "standard command shortcuts target the active clip group");
        check(carta::clipGroupShortcut(0, true, true, false, true) == Shortcut::remove,
              "unmodified delete or backspace targets the active clip group");
        check(carta::clipGroupShortcut('c', false, false, true, false) == Shortcut::none &&
                  carta::clipGroupShortcut('d', false, false, false, true) == Shortcut::none &&
                  carta::clipGroupShortcut(0, true, false, false, true) == Shortcut::none &&
                  carta::clipGroupShortcut('v', false, false, true, true) == Shortcut::none,
              "inactive groups, modified commands and paste retain their existing behavior");
        check(carta::moveClipGroup(state.session, group, 10, 1, error),
              "move nonadjacent whole clips across rows");
        check(state.session.tracks[0].clips.size() == 1 &&
                  state.session.tracks[1].clips.size() == 2 &&
                  state.session.tracks[2].clips.size() == 1,
              "group move removes only selected clips and retains other material");
        const auto& first = state.session.tracks[1].clips.back();
        const auto& second = state.session.tracks[2].clips.back();
        check(std::abs(first.start * rate - 40) < 1e-8 &&
                  std::abs(second.start * rate - 15) < 1e-8 &&
                  std::abs(first.offset * rate - 5) < 1e-8 && first.name == "Moved" &&
                  near(first.gain, 0.5f),
              "group move preserves relative timing, source offsets and envelopes");
        juce::AudioBuffer<float> output(2, 1);
        output.clear();
        state.session.render(output, 0, 1, 55 / rate, rate, 2, nullptr, {1, 1});
        check(near(output.getSample(0, 0), 0.35f) && near(output.getSample(1, 0), 0.50f),
              "group move mixes destination overlap while preserving stereo channels and gain");
        check(std::abs(state.session.markers[0].position * rate - 70) < 1e-8,
              "group move leaves markers fixed");
        history.record(before);
        check(history.undo(state) && state.session.tracks[0].clips.size() == 2 &&
                  history.redo(state) && state.session.tracks[2].clips.size() == 1,
              "group move supports undo and redo");
        auto rejected = before.session;
        check(!carta::moveClipGroup(rejected, {{0, 1}, {0, 1}}, 10, 0, error) &&
                  rejected.tracks[0].clips.size() == 2,
              "duplicate group references fail atomically");
        check(!carta::moveClipGroup(rejected, group, -100, 0, error) &&
                  rejected.tracks[0].clips.size() == 2,
              "group move rejects negative time atomically");
        check(!carta::moveClipGroup(rejected, group, 0, 2, error) &&
                  rejected.tracks[0].clips.size() == 2,
              "group move rejects missing destination tracks atomically");
        check(!carta::moveClipGroup(rejected, group, std::numeric_limits<juce::int64>::max(), 0,
                                    error) &&
                  rejected.tracks[0].clips.size() == 2,
              "group move rejects oversized offsets atomically");
        carta::Session end;
        end.tracks.push_back({"One", {{audio, 0, 0, 80 / rate}}});
        end.markers = {{75 / rate, "End"}};
        check(!carta::moveClipGroup(end, {{0, 0}}, -1, 0, error) && error.isNotEmpty() &&
                  end.tracks[0].clips.size() == 1,
              "group move rejects an invalid start without changing audio");
        end.tracks[0].clips[0].start = 20 / rate;
        end.tracks[0].clips[0].duration = 60 / rate;
        check(!carta::moveClipGroup(end, {{0, 0}}, -20, 0, error) && error.contains("marker") &&
                  std::abs(end.tracks[0].clips[0].start * rate - 20) < 1e-8,
              "group move rejects newly orphaned markers atomically");
        carta::Session stacking;
        stacking.tracks.push_back(
            {"Stack",
             {{audio, 0, 0, 20 / rate}, {audio, 0, 0, 20 / rate}, {audio, 0, 0, 20 / rate}}});
        stacking.tracks.push_back({"Destination", {}});
        stacking.tracks[0].clips[0].name = "First";
        stacking.tracks[0].clips[1].name = "Middle";
        stacking.tracks[0].clips[2].name = "Last";
        const std::vector<carta::ClipReference> reverseOrder{{0, 2}, {0, 0}};
        carta::Session prepared;
        std::vector<carta::ClipReference> preparedRefs{{8, 9}};
        check(
            carta::prepareClipGroupMove(stacking, prepared, reverseOrder, 0, 1, error,
                                        &preparedRefs) &&
                stacking.tracks[0].clips.size() == 3 && stacking.tracks[1].clips.empty() &&
                prepared.tracks[1].clips.size() == 2 && preparedRefs[0].clip == 1 &&
                preparedRefs[1].clip == 0,
            "preparing a group move leaves its source untouched and returns ordered destinations");
        check(!carta::prepareClipGroupMove(stacking, prepared, {{0, 0}, {0, 0}}, 1, 0, error,
                                           &preparedRefs) &&
                  prepared.tracks[1].clips.size() == 2 && preparedRefs.size() == 2 &&
                  preparedRefs[0].clip == 1,
              "failed group move preparation preserves its output and mapping");
        carta::Session copyCandidate;
        std::vector<carta::ClipReference> dragCopyRefs;
        check(carta::prepareClipGroupCopy(stacking, copyCandidate, reverseOrder, 10, 1, error,
                                          &dragCopyRefs) &&
                  stacking.tracks[0].clips.size() == 3 && stacking.tracks[1].clips.empty() &&
                  copyCandidate.tracks[0].clips.size() == 3 &&
                  copyCandidate.tracks[1].clips.size() == 2 && dragCopyRefs[0].clip == 1 &&
                  dragCopyRefs[1].clip == 0 &&
                  copyCandidate.tracks[1].clips[0].audio == stacking.tracks[0].clips[0].audio,
              "prepare copies preserves originals, shared audio and stacking order");
        check(!carta::prepareClipGroupCopy(stacking, copyCandidate, reverseOrder, 0, 0, error,
                                           &dragCopyRefs) &&
                  copyCandidate.tracks[1].clips.size() == 2 && dragCopyRefs.size() == 2,
              "no-op copy leaves candidate and mapping unchanged");
        check(!carta::prepareClipGroupCopy(stacking, copyCandidate, reverseOrder, 0, 2, error,
                                           &dragCopyRefs) &&
                  copyCandidate.tracks[1].clips.size() == 2 && dragCopyRefs.size() == 2,
              "invalid destination leaves copy candidate unchanged");
        auto horizontal = stacking;
        check(carta::moveClipGroup(horizontal, reverseOrder, 1, 0, error) &&
                  horizontal.tracks[0].clips[0].name == "First" &&
                  horizontal.tracks[0].clips[1].name == "Middle" &&
                  horizontal.tracks[0].clips[2].name == "Last",
              "horizontal group move keeps original stacking order including unselected overlaps");
        auto crossTrack = stacking;
        std::vector<carta::ClipReference> mapped{{8, 9}};
        check(carta::moveClipGroup(crossTrack, reverseOrder, 0, 1, error, &mapped) &&
                  crossTrack.tracks[1].clips.size() == 2 &&
                  crossTrack.tracks[1].clips[0].name == "First" &&
                  crossTrack.tracks[1].clips[1].name == "Last",
              "cross-track group move preserves source stacking rather than selection order");
        check(mapped.size() == 2 && mapped[0].track == 1 && mapped[0].clip == 1 &&
                  mapped[1].track == 1 && mapped[1].clip == 0,
              "move results identify each relocated clip in original selection order");
        check(!carta::moveClipGroup(crossTrack, {{1, 0}, {1, 0}}, 1, 0, error, &mapped) &&
                  mapped.size() == 2 && mapped[0].clip == 1 && mapped[1].clip == 0,
              "rejected group move preserves the caller's previous reference mapping");
        auto stackedCopies = stacking;
        std::vector<carta::ClipReference> duplicateRefs{{8, 9}};
        check(carta::duplicateClipGroup(stackedCopies, reverseOrder, error, &duplicateRefs) &&
                  stackedCopies.tracks[0].clips[3].name == "First" &&
                  stackedCopies.tracks[0].clips[4].name == "Last",
              "group duplication preserves source stacking despite reversed selection order");
        check(duplicateRefs.size() == 2 && duplicateRefs[0].track == 0 &&
                  duplicateRefs[0].clip == 4 && duplicateRefs[1].clip == 3,
              "duplicate mapping identifies copies in input selection order");
        check(!carta::duplicateClipGroup(stackedCopies, {{0, 0}, {0, 0}}, error, &duplicateRefs) &&
                  duplicateRefs.size() == 2 && duplicateRefs[0].clip == 4,
              "invalid duplication preserves the caller's output mapping");
        auto duplicate = before;
        check(carta::duplicateClipGroup(duplicate.session, group, error),
              "duplicate nonadjacent clips as one group");
        check(duplicate.session.tracks[0].clips.size() == 3 &&
                  duplicate.session.tracks[1].clips.size() == 3 &&
                  std::abs(duplicate.session.tracks[0].clips.back().start * rate - 75) < 1e-8 &&
                  std::abs(duplicate.session.tracks[1].clips.back().start * rate - 50) < 1e-8 &&
                  duplicate.session.tracks[0].clips.back().name == "Moved" &&
                  near(duplicate.session.tracks[0].clips.back().gain, 0.5f),
              "duplicate preserves relative positions, source and envelope after sample span");
        check(std::abs(duplicate.session.markers[0].position - before.session.markers[0].position) <
                  1e-12,
              "group duplicate leaves markers fixed");
        output.clear();
        duplicate.session.render(output, 0, 1, 55 / rate, rate, 2, nullptr, {1, 1});
        check(near(output.getSample(0, 0), 0.4f) && near(output.getSample(1, 0), 0.6f),
              "group duplicate sums existing destination audio with the copied stereo clip");
        carta::EditHistory duplicateHistory;
        duplicateHistory.record(before);
        check(duplicateHistory.undo(duplicate) && duplicate.session.tracks[0].clips.size() == 2 &&
                  duplicateHistory.redo(duplicate) && duplicate.session.tracks[0].clips.size() == 3,
              "group duplicate supports undo and redo");
        auto deleted = before;
        check(carta::deleteClipGroup(deleted.session, group, error),
              "delete nonadjacent clips across tracks");
        check(deleted.session.tracks[0].clips.size() == 1 &&
                  deleted.session.tracks[1].clips.size() == 1 &&
                  std::abs(deleted.session.tracks[1].clips[0].start * rate - 50) < 1e-8,
              "group delete retains unselected clips and timeline positions");
        carta::EditHistory deleteHistory;
        deleteHistory.record(before);
        check(deleteHistory.undo(deleted) && deleted.session.tracks[0].clips.size() == 2 &&
                  deleteHistory.redo(deleted) && deleted.session.tracks[1].clips.size() == 1,
              "group delete supports undo and redo");
        auto invalid = before.session;
        check(!carta::deleteClipGroup(invalid, {{0, 0}, {0, 0}}, error) &&
                  invalid.tracks[0].clips.size() == 2,
              "group delete rejects duplicates atomically");
        check(!carta::duplicateClipGroup(invalid, {{0, 0}, {3, 0}}, error) &&
                  invalid.tracks[0].clips.size() == 2,
              "group duplicate rejects invalid references atomically");
        auto deletedEnd = end;
        check(carta::deleteClipGroup(deletedEnd, {{0, 0}}, error) &&
                  deletedEnd.tracks[0].clips.empty() && carta::projectEndSample(deletedEnd) == 80 &&
                  std::abs(deletedEnd.markers[0].position * rate - 75) < 1e-8,
              "group delete leaves a silent tail and keeps markers in place");
        carta::EditorState oneClip;
        oneClip.session.tracks.push_back({"Clip", {{audio, 0, 0, 100 / rate}}});
        oneClip.session.tracks[0].clips[0].name = "Named";
        check(
            carta::setClipEnvelope(oneClip.session.tracks[0].clips[0], 0.5f, 20 / rate, 20 / rate),
            "prepare clip split envelope");
        oneClip.session.markers = {{75 / rate, "Marker"}};
        oneClip.selection = {10, 90, 0, 0};
        const auto beforeOneClip = oneClip;
        for (const auto sample : {juce::int64(-1), juce::int64(0), juce::int64(100),
                                  std::numeric_limits<juce::int64>::max()})
            check(!carta::splitClipAtSample(oneClip.session, 0, 0, sample) &&
                      oneClip.session.tracks[0].clips.size() == 1,
                  "clip split rejects out-of-range samples without changing the session");
        check(!carta::splitClipAtSample(oneClip.session, 1, 0, 50) &&
                  !carta::splitClipAtSample(oneClip.session, 0, 1, 50),
              "clip split rejects invalid references");
        check(carta::splitClipAtSample(oneClip.session, 0, 0, 50) &&
                  oneClip.session.tracks[0].clips.size() == 2 &&
                  std::abs(oneClip.session.tracks[0].clips[1].offset * rate - 50) < 1e-8 &&
                  oneClip.session.tracks[0].clips[1].name == "Named",
              "clip split keeps source offset, name and timing");
        check(oneClip.selection.start == 10 && oneClip.selection.end == 90 &&
                  std::abs(oneClip.session.markers[0].position * rate - 75) < 1e-8,
              "clip split retains selection and markers");
        juce::AudioBuffer<float> whole(2, 100), pieces(2, 100);
        whole.clear();
        pieces.clear();
        beforeOneClip.session.render(whole, 0, 100, 0, rate, 2);
        oneClip.session.render(pieces, 0, 100, 0, rate, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 100; ++i)
                check(near(whole.getSample(ch, i), pieces.getSample(ch, i)),
                      "clip split preserves faded audio");
        carta::EditHistory splitHistory;
        splitHistory.record(beforeOneClip);
        check(splitHistory.undo(oneClip) && oneClip.session.tracks[0].clips.size() == 1 &&
                  splitHistory.redo(oneClip) && oneClip.session.tracks[0].clips.size() == 2,
              "clip split supports undo and redo");
        auto groupSplit = before;
        groupSplit.session.tracks[0].clips[1].start = 10 / rate;
        groupSplit.selection = {5, 65, 0, 1};
        groupSplit.position = 15 / rate;
        const auto beforeGroupSplit = groupSplit;
        const std::vector<carta::ClipReference> splitGroup{{1, 1}, {0, 0}, {1, 0}};
        std::vector<std::vector<carta::ClipReference>> splitMappings;
        check(carta::splitClipGroupAtSample(groupSplit.session, splitGroup, 15, error,
                                            &splitMappings),
              "split nonadjacent selected clips at one sample");
        check(splitMappings.size() == 3 && splitMappings[0].size() == 1 &&
                  splitMappings[0][0].track == 1 && splitMappings[0][0].clip == 2 &&
                  splitMappings[1].size() == 2 && splitMappings[1][0].track == 0 &&
                  splitMappings[1][0].clip == 0 && splitMappings[1][1].clip == 1 &&
                  splitMappings[2].size() == 2 && splitMappings[2][0].track == 1 &&
                  splitMappings[2][1].clip == 1,
              "split mapping keeps unsplit selected clips and both pieces despite shifted indices");
        check(groupSplit.session.tracks[0].clips.size() == 3 &&
                  groupSplit.session.tracks[1].clips.size() == 3 &&
                  std::abs(groupSplit.session.tracks[0].clips[2].start * rate - 10) < 1e-8 &&
                  std::abs(groupSplit.session.tracks[1].clips[2].start * rate - 50) < 1e-8,
              "group split skips unselected overlaps and selected clips outside the split");
        check(groupSplit.selection.start == 5 && groupSplit.selection.end == 65 &&
                  std::abs(groupSplit.position * rate - 15) < 1e-8 &&
                  std::abs(groupSplit.session.markers[0].position * rate - 70) < 1e-8,
              "group split leaves playhead, selection and marker positions alone");
        juce::AudioBuffer<float> originalGroup(2, 80), splitGroupAudio(2, 80);
        originalGroup.clear();
        splitGroupAudio.clear();
        beforeGroupSplit.session.render(originalGroup, 0, 80, 0, rate, 2);
        groupSplit.session.render(splitGroupAudio, 0, 80, 0, rate, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 80; ++i)
                check(near(originalGroup.getSample(ch, i), splitGroupAudio.getSample(ch, i)),
                      "group split preserves overlapping audio");
        carta::EditHistory groupSplitHistory;
        groupSplitHistory.record(beforeGroupSplit);
        check(groupSplitHistory.undo(groupSplit) &&
                  groupSplit.session.tracks[1].clips.size() == 2 &&
                  groupSplitHistory.redo(groupSplit) &&
                  groupSplit.session.tracks[1].clips.size() == 3,
              "group split supports undo and redo");
        auto badSplit = beforeGroupSplit.session;
        check(!carta::splitClipGroupAtSample(badSplit, {{0, 0}, {0, 0}}, 15, error) &&
                  badSplit.tracks[0].clips.size() == 2,
              "duplicate group split targets fail atomically");
        check(!carta::splitClipGroupAtSample(badSplit, {{0, 0}, {3, 0}}, 15, error) &&
                  badSplit.tracks[0].clips.size() == 2,
              "invalid group split targets fail atomically");
        check(!carta::splitClipGroupAtSample(badSplit, splitGroup, 90, error, &splitMappings) &&
                  badSplit.tracks[1].clips.size() == 2 && splitMappings.size() == 3 &&
                  splitMappings[0][0].clip == 2,
              "group split with no eligible clips preserves the previous mapping");
        auto copySource = before.session;
        copySource.tracks[0].clips[1].start = 30.25 / rate;
        copySource.tracks[0].clips[1].duration = 20.5 / rate;
        copySource.tracks[2].clips.push_back({audio, 50.5 / rate, 3 / rate, 20 / rate});
        const std::vector<carta::ClipReference> copyRefs{{2, 0}, {0, 1}};
        carta::Clipboard clipCopy;
        check(carta::copyClipGroup(copySource, copyRefs, clipCopy, error) &&
                  clipCopy.lengthSamples == 41 && clipCopy.tracks.size() == 3 &&
                  clipCopy.tracks[0].size() == 1 && clipCopy.tracks[1].empty() &&
                  clipCopy.tracks[2].size() == 1 && clipCopy.wholeClips,
              "clip group copy retains only selected clips and the intervening silent row");
        check(std::abs(clipCopy.tracks[0][0].start * rate - 0.25) < 1e-8 &&
                  std::abs(clipCopy.tracks[2][0].start * rate - 20.5) < 1e-8 &&
                  clipCopy.tracks[0][0].name == "Moved" && near(clipCopy.tracks[0][0].gain, 0.5f),
              "clip copy retains fractional positions, names and envelopes");
        carta::Session pasted;
        pasted.tracks.resize(3);
        check(carta::pasteRange(pasted, clipCopy, 0, 0, &error) &&
                  pasted.tracks[0].clips.size() == 1 && pasted.tracks[1].clips.empty() &&
                  pasted.tracks[2].clips.size() == 1,
              "fractional clip group pastes at sample zero without unrelated overlaps");
        carta::Session isolated;
        isolated.tracks.resize(3);
        isolated.tracks[0].clips.push_back(copySource.tracks[0].clips[1]);
        isolated.tracks[2].clips.push_back(copySource.tracks[2].clips[0]);
        juce::AudioBuffer<float> sourceGroup(2, 41), pastedGroup(2, 41);
        sourceGroup.clear();
        pastedGroup.clear();
        isolated.render(sourceGroup, 0, 41, 30 / rate, rate, 2);
        pasted.render(pastedGroup, 0, 41, 0, rate, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 41; ++i)
                check(near(sourceGroup.getSample(ch, i), pastedGroup.getSample(ch, i)),
                      "clip group paste preserves mixed audio including fractional starts");
        carta::Session destinations;
        for (int t = 0; t < 3; ++t)
            destinations.tracks.push_back({"Destination", {{audio, 0, 0, 100 / rate}}});
        std::vector<carta::ClipReference> pastedRefs;
        auto replaced = destinations;
        check(carta::pasteRangeReplacing(replaced, clipCopy, 0, 0, error, &pastedRefs) &&
                  replaced.tracks[1].clips.size() == 1 &&
                  std::abs(replaced.tracks[1].clips[0].start * rate - 41) < 1e-8,
              "replacing a copied clip group clears the full span even on its silent middle row");
        check(pastedRefs.size() == 2 && pastedRefs[0].track == 0 && pastedRefs[1].track == 2 &&
                  replaced.tracks[0].clips[static_cast<size_t>(pastedRefs[0].clip)].name == "Moved",
              "replace paste reports inserted references after removing overlapping material");
        auto inserted = destinations;
        check(carta::pasteRangeInserting(inserted, clipCopy, 0, 0, false, error, &pastedRefs) &&
                  inserted.tracks[1].clips.size() == 1 &&
                  std::abs(inserted.tracks[1].clips[0].start * rate - 41) < 1e-8,
              "inserting a copied clip group shifts audio on its silent middle row");
        check(pastedRefs.size() == 2 && pastedRefs[0].track == 0 && pastedRefs[1].track == 2 &&
                  inserted.tracks[0].clips[static_cast<size_t>(pastedRefs[0].clip)].name == "Moved",
              "insert paste reports inserted references after shifting destination clips");
        check(!carta::pasteRange(inserted, clipCopy, 2, 0, &error, &pastedRefs) &&
                  pastedRefs.size() == 2 && pastedRefs[1].track == 2,
              "invalid paste preserves the previous reference output");
        const auto oldCopy = clipCopy;
        check(!carta::copyClipGroup(copySource, {{0, 1}, {0, 1}}, clipCopy, error) &&
                  clipCopy.lengthSamples == oldCopy.lengthSamples && clipCopy.tracks[2].size() == 1,
              "rejected group copy preserves the clipboard");
        check(!carta::copyClipGroup(copySource, {{0, 1}, {9, 0}}, clipCopy, error) &&
                  clipCopy.tracks.size() == 3,
              "invalid group copy leaves the clipboard intact");
        carta::EditorState cutState;
        cutState.session = copySource;
        const auto beforeCut = cutState;
        carta::Clipboard cutClipboard;
        check(carta::cutClipGroup(cutState.session, copyRefs, cutClipboard, error) &&
                  cutState.session.tracks[0].clips.size() == 1 &&
                  cutState.session.tracks[2].clips.empty() && cutClipboard.tracks.size() == 3,
              "cut copies and removes only selected clips");
        carta::EditHistory cutHistory;
        cutHistory.record(beforeCut);
        check(cutHistory.undo(cutState) && cutState.session.tracks[2].clips.size() == 1 &&
                  cutHistory.redo(cutState) && cutState.session.tracks[2].clips.empty() &&
                  cutClipboard.lengthSamples == 41,
              "undo and redo group cut restore clips while retaining the clipboard");
        check(carta::cutClipGroup(end, {{0, 0}}, cutClipboard, error) &&
                  end.tracks[0].clips.empty() && carta::projectEndSample(end) == 80 &&
                  cutClipboard.lengthSamples == 60 &&
                  std::abs(end.markers[0].position * rate - 75) < 1e-8,
              "group cut copies the clip and preserves the silent master and marker");
        auto invalidCut = beforeCut.session;
        check(!carta::cutClipGroup(invalidCut, {{0, 0}, {0, 0}}, cutClipboard, error) &&
                  invalidCut.tracks[0].clips.size() == beforeCut.session.tracks[0].clips.size() &&
                  cutClipboard.lengthSamples == 60,
              "invalid group cut preserves both audio and the previous clipboard");
    }
    {
        const auto rate = carta::sessionRate;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(4, 100);
        carta::Session accessible;
        accessible.tracks.push_back({"Music", {{audio, 25 / rate, 0, 50 / rate}}});
        accessible.tracks[0].clips[0].name = "Intro";
        accessible.tracks[0].muted = true;
        accessible.tracks[0].solo = true;
        accessible.tracks[0].collapsed = true;
        accessible.markers = {{50 / rate, "Track two"}};
        const auto description =
            carta::describeTimeline(accessible, {30, 60, 0, 0}, 0, 0, 0, 40, false);
        const auto independentDescription =
            carta::describeTimeline(accessible, {30, 60, 0, 0}, 0, 0, 0, 40, false, nullptr, true);
        check(independentDescription.contains("end exclusive, on independent layers") &&
                  !independentDescription.contains("end exclusive, layers 1 to 1"),
              "accessible time selection does not announce a contiguous row span for independent "
              "layers");
        check(description.contains("Master end sample 75, end exclusive"),
              "accessible timeline announces the exact master end");
        auto extendedAccessible = accessible;
        extendedAccessible.minimumEndSample = 100;
        check(carta::describeTimeline(extendedAccessible, {}, 0, 0, -1, 0, false)
                  .contains(
                      "Master end sample 100, end exclusive. Minimum master duration 100 samples"),
              "accessible timeline distinguishes the master end and its explicit minimum");
        check(description.contains("Playhead 00:00.000, sample 40") &&
                  description.contains("Layer 1: Music") && description.contains("Clip: Intro") &&
                  description.contains("Clip samples 25 to 75, end exclusive"),
              "accessible timeline reports focused clip, layer and exact playhead");
        check(description.contains("Muted") && description.contains("Solo") &&
                  description.contains("Collapsed") &&
                  description.contains("routing needs confirmation"),
              "accessible timeline reports track state and routing warning");
        check(description.contains("Marker: Track two") &&
                  description.contains("Marker sample 50") &&
                  description.contains("Selection samples 30 to 60, end exclusive, layers 1 to 1"),
              "accessible timeline reports marker and time selection");
        {
            auto repair = accessible;
            repair.markers = {{50.25 / rate, "Fractional"},
                              {50.4 / rate, "Collision"},
                              {75 / rate, "Outside"},
                              {std::numeric_limits<double>::quiet_NaN(), "Invalid"}};
            const auto fractional = carta::describeTimeline(repair, {}, -1, -1, 0, 0, false);
            check(fractional.contains("off the sample grid") &&
                      fractional.contains("nearest sample 50") &&
                      fractional.contains("Another marker rounds to this sample") &&
                      !fractional.contains("Marker sample 50."),
                  "accessible marker description distinguishes a fractional position and rounded "
                  "collision from an exact sample");
            const auto outside = carta::describeTimeline(repair, {}, -1, -1, 2, 0, false);
            check(outside.contains("Marker sample 75") &&
                      outside.contains("Outside the master duration"),
                  "accessible marker description reports the exclusive master boundary");
            check(carta::describeTimeline(repair, {}, -1, -1, 3, 0, false)
                      .contains("Invalid marker position"),
                  "accessible marker description rejects a non-finite position");
            repair.markers = {{(50.0 + 1e-8) / rate, "Tolerance"}, {-1 / rate, "Negative"}};
            const auto aligned = carta::describeMarkerPosition(repair, 0, 75);
            check(aligned.contains("Marker sample 50") &&
                      !aligned.contains("off the sample grid") &&
                      !aligned.contains("Another marker"),
                  "marker feedback uses the same grid tolerance and ignores invalid collision "
                  "candidates");
        }
        const auto navigation = carta::describeNavigationTarget(accessible, 0, 0);
        check(navigation.contains("Layer 1 of 1: Music") && navigation.contains("Muted") &&
                  navigation.contains("Solo") && navigation.contains("Collapsed") &&
                  navigation.contains("routing needs confirmation") &&
                  navigation.contains("Clip: Intro") &&
                  navigation.contains("Samples 25 to 75, end exclusive"),
              "keyboard navigation description includes focus, state, routing and clip boundaries");
        check(description.contains("4 source channels, 44100.00 Hz") &&
                  navigation.contains("4 source channels, 44100.00 Hz") &&
                  carta::makeNavigationRows(accessible)[1].description.contains(
                      "4 source channels, 44100.00 Hz"),
              "focused clip and browser descriptions report actual source channel count and sample "
              "rate");
        check(carta::describeNavigationTarget(accessible, -1, -1) == "No layer selected." &&
                  !carta::describeNavigationTarget(accessible, 0, -1).contains("Clip:"),
              "keyboard navigation description handles missing focus");
        {
            carta::Track overlaps;
            overlaps.clips = {{audio, 0, 0, 10},
                              {audio, 2, 0, 3},
                              {audio, 5, 0, 3},
                              {audio, 9, 0, 3},
                              {audio, 12, 0, 1}};
            const auto ranges = carta::clipOverlapRanges(overlaps, 0, 20);
            check(
                ranges.size() == 2 && near(static_cast<float>(ranges[0].getStart()), 2) &&
                    near(static_cast<float>(ranges[0].getEnd()), 8) &&
                    near(static_cast<float>(ranges[1].getStart()), 9) &&
                    near(static_cast<float>(ranges[1].getEnd()), 10),
                "overlap spans merge continuous nested and touching changes without merging gaps");
            const auto viewport = carta::clipOverlapRanges(overlaps, 4, 9.5);
            check(viewport.size() == 2 && near(static_cast<float>(viewport[0].getStart()), 4) &&
                      near(static_cast<float>(viewport[0].getEnd()), 8) &&
                      near(static_cast<float>(viewport[1].getStart()), 9) &&
                      near(static_cast<float>(viewport[1].getEnd()), 9.5f),
                  "overlap spans clip to the viewport");
            check(carta::overlappingClipCount(overlaps, 0) == 3 &&
                      carta::overlappingClipCount(overlaps, 1) == 1 &&
                      carta::overlappingClipCount(overlaps, 4) == 0,
                  "clip overlap counts exclude clips that only touch");
            check(carta::clipOverlapRanges(overlaps, 10, 12).empty() &&
                      carta::clipOverlapRanges(overlaps, 3, 3).empty() &&
                      carta::overlappingClipCount(overlaps, -1) == 0,
                  "nonoverlap and invalid viewports are safe");
            overlaps.clips.push_back({audio, std::numeric_limits<double>::quiet_NaN(), 0, 1});
            check(carta::clipOverlapRanges(overlaps, 0, 20).size() == 2 &&
                      carta::overlappingClipCount(overlaps, 0) == 3,
                  "invalid geometry does not create spurious overlaps");
            const auto counts = carta::overlappingClipCounts(overlaps);
            check(counts == std::vector<int>({3, 1, 1, 1, 0, 0}),
                  "batch overlap counts exclude touching and invalid clips");
            carta::Session overlapBrowser;
            overlapBrowser.tracks.push_back(overlaps);
            const auto overlapRows = carta::makeNavigationRows(overlapBrowser);
            check(overlapRows.size() == 6 &&
                      overlapRows[1].description.contains(
                          "Overlaps 3 other clips; signals sum when audible") &&
                      overlapRows[2].description.contains(
                          "Overlaps 1 other clip; signals sum when audible") &&
                      !overlapRows[5].description.contains("Overlaps"),
                  "browser descriptions announce overlaps and do not count touching clips");
            juce::Random randomOverlap(0x519a);
            for (int trial = 0; trial < 500; ++trial) {
                carta::Track randomTrack;
                const int count = randomOverlap.nextInt(100);
                for (int i = 0; i < count; ++i) {
                    const double start = static_cast<double>(randomOverlap.nextInt(120) - 20) / 4;
                    const double duration = static_cast<double>(randomOverlap.nextInt(30)) / 4;
                    randomTrack.clips.push_back(
                        {i % 29 == 0 ? std::shared_ptr<carta::Audio>{} : audio, start, 0,
                         duration});
                }
                if (!randomTrack.clips.empty() && trial % 3 == 0)
                    randomTrack.clips.back().start = std::numeric_limits<double>::quiet_NaN();
                const auto batch = carta::overlappingClipCounts(randomTrack);
                check(batch.size() == randomTrack.clips.size(),
                      "batch overlap counts preserve clip indices");
                for (size_t i = 0; i < batch.size(); ++i)
                    check(batch[i] == carta::overlappingClipCount(randomTrack, static_cast<int>(i)),
                          "batch overlap count matches pairwise reference");
            }
            carta::Session indexed;
            indexed.tracks.push_back({"A",
                                      {{audio, 0, 0, 80 / rate},
                                       {audio, 10.2 / rate, 0, 20 / rate},
                                       {audio, 50 / rate, 0, 10 / rate}}});
            indexed.tracks.push_back({"B",
                                      {{audio, 0, 0, 80 / rate},
                                       {audio, 10.2 / rate, 0, 20 / rate},
                                       {audio, 10.3 / rate, 0, 0.1 / rate}}});
            indexed.tracks.push_back(
                {"Touching", {{audio, 0, 0, 20 / rate}, {audio, 20 / rate, 0, 20 / rate}}});
            const auto indexedRegions = carta::overlapRegions(indexed);
            check(indexedRegions.size() == 3 && indexedRegions[0].track == 0 &&
                      indexedRegions[0].start == 11 && indexedRegions[0].end == 31 &&
                      indexedRegions[1].track == 1 && indexedRegions[1].start == 11 &&
                      indexedRegions[1].end == 31 && indexedRegions[2].track == 0 &&
                      indexedRegions[2].start == 50 && indexedRegions[2].end == 60,
                  "overlap browser regions are sample aligned, ordered, and retain simultaneous "
                  "tracks");
            carta::Session subSample;
            subSample.tracks.push_back(
                {"short", {{audio, 0, 0, 20 / rate}, {audio, 10.1 / rate, 0, 0.2 / rate}}});
            check(carta::overlapRegions(subSample).empty() &&
                      carta::overlapRegions(carta::Session{}).empty(),
                  "overlap browser excludes intervals containing no project sample and empty "
                  "sessions");
            carta::Session described;
            described.tracks.push_back(overlaps);
            check(carta::describeTimeline(described, {}, 0, 0, -1, 0, false)
                          .contains("Overlaps 3 other clips; signals sum when audible") &&
                      carta::describeNavigationTarget(described, 0, 1)
                          .contains("Overlaps 1 other clip; signals sum when audible"),
                  "timeline and keyboard descriptions announce overlap and summing");
        }
        auto browser = accessible;
        browser.tracks[0].clips.push_back({audio, 10 / rate, 0, 20 / rate});
        browser.tracks[0].clips.back().name = "First";
        browser.tracks[0].clips.push_back({audio, 10 / rate, 5 / rate, 20 / rate});
        browser.tracks[0].clips.back().name = "First";
        browser.tracks.push_back({"Empty", {}});
        const auto rows = carta::makeNavigationRows(browser);
        check(rows.size() == 5 && rows[0].track == 0 && rows[0].clip == -1 && rows[1].clip == 1 &&
                  rows[2].clip == 2 && rows[3].clip == 0 && rows[4].track == 1 &&
                  rows[4].clip == -1,
              "track and clip browser orders coincident and later clips deterministically and "
              "includes empty tracks");
        check(rows[1].description.contains("Muted") &&
                  rows[1].description.contains("Clip: First") &&
                  rows[1].description.contains("samples 10 to 30, end exclusive") &&
                  rows[4].description.contains("Empty"),
              "browser rows have individually descriptive accessible names");
        check(rows[1].description.contains("1 of 3") && rows[2].description.contains("2 of 3") &&
                  rows[1].description != rows[2].description && rows[1].hasSampleRange &&
                  !rows[0].hasSampleRange && !rows[4].hasSampleRange,
              "identically named coincident clips have distinct row descriptions and selectable "
              "time");
        juce::SparseSet<int> selection;
        selection.addRange({0, 1});
        check(carta::updateBrowserClipSelection(rows, 1, carta::BrowserSelectionAction::toggle,
                                                selection) &&
                  selection.size() == 1 && selection.contains(1),
              "keyboard clip selection removes track headers and selects a clip");
        check(carta::updateBrowserClipSelection(rows, 3, carta::BrowserSelectionAction::toggle,
                                                selection) &&
                  selection.size() == 2 && selection.contains(1) && selection.contains(3),
              "keyboard clip selection retains nonadjacent clips");
        check(!carta::updateBrowserClipSelection(rows, 4, carta::BrowserSelectionAction::toggle,
                                                 selection) &&
                  selection.size() == 2,
              "track headers cannot be toggled as clips");
        check(carta::updateBrowserClipSelection(rows, 0, carta::BrowserSelectionAction::track,
                                                selection) &&
                  selection.size() == 3 && selection.contains(2),
              "track selection adds all clips on the cursor's track");
        check(carta::updateBrowserClipSelection(rows, 4, carta::BrowserSelectionAction::track,
                                                selection) &&
                  selection.size() == 3,
              "selecting an empty track preserves the clip selection");
        check(carta::updateBrowserClipSelection(rows, 0, carta::BrowserSelectionAction::clear,
                                                selection) &&
                  selection.isEmpty(),
              "clear removes selected clips");
        check(carta::updateBrowserClipSelection(rows, 4, carta::BrowserSelectionAction::all,
                                                selection) &&
                  selection.size() == 3 && !selection.contains(0) && !selection.contains(4),
              "select all selects clips but not track headers");
        check(!carta::updateBrowserClipSelection(rows, -1, carta::BrowserSelectionAction::track,
                                                 selection) &&
                  selection.size() == 3,
              "invalid browser cursor preserves selection");
        check(carta::makeNavigationRows(carta::Session{}).empty(),
              "empty project has no browser rows");
        accessible.markers[0].position = std::numeric_limits<double>::quiet_NaN();
        const auto missing = carta::describeTimeline(accessible, {}, -1, -1, 0, 0, true);
        check(missing.contains("Playing") && missing.contains("No layer selected") &&
                  missing.contains("Invalid marker position") &&
                  missing.contains("No time selection"),
              "accessible summary handles empty focus and invalid marker positions");
    }
    {
        const double rate = carta::sessionRate;
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = rate;
        source->samples.setSize(1, 16);
        for (int i = 0; i < 16; ++i)
            source->samples.setSample(0, i, static_cast<float>(i + 1) / 16.0f);
        for (const juce::int64 base :
             {juce::int64(0), juce::int64(44100 * 1200), juce::int64(44100 * 4799)}) {
            carta::Session original;
            original.tracks.push_back(
                {"Tiny", {{source, static_cast<double>(base) / rate, 0, 2 / rate}}});
            auto split = original;
            check(carta::canSplitClipAtSample(split.tracks[0].clips[0], base + 1) &&
                      carta::splitClipAtSample(split, 0, 0, base + 1),
                  "two-sample clips split into one-sample clips at long timeline positions");
            juce::int64 first = 0, last = 0;
            check(carta::clipSampleRange(split.tracks[0].clips[0], first, last) && first == base &&
                      last == base + 1 &&
                      carta::clipSampleRange(split.tracks[0].clips[1], first, last) &&
                      first == base + 1 && last == base + 2,
                  "both split pieces contain exactly one intended project sample");
            juce::AudioBuffer<float> before(2, 4), after(2, 4);
            before.clear();
            after.clear();
            original.render(before, 0, 4, static_cast<double>(base) / rate, rate, 2);
            split.render(after, 0, 4, static_cast<double>(base) / rate, rate, 2);
            for (int ch = 0; ch < 2; ++ch)
                check(std::memcmp(before.getReadPointer(ch), after.getReadPointer(ch),
                                  4 * sizeof(float)) == 0,
                      "one-sample split preserves the rendered audio exactly");
            check(!carta::canSplitClipAtSample(split.tracks[0].clips[0], base + 1),
                  "a one-sample piece cannot be split again");
        }
        const juce::int64 base = 44100 * 4799;
        carta::Session fractional;
        fractional.tracks.push_back(
            {"Fractional", {{source, (static_cast<double>(base) + 0.25) / rate, 0, 1.9 / rate}}});
        auto split = fractional;
        check(carta::canSplitClipAtSample(split.tracks[0].clips[0], base + 2) &&
                  carta::splitClipAtSample(split, 0, 0, base + 2),
              "fractional clips can split when each piece contains a project sample");
        juce::AudioBuffer<float> before(2, 4), after(2, 4);
        before.clear();
        after.clear();
        fractional.render(before, 0, 4, static_cast<double>(base) / rate, rate, 2);
        split.render(after, 0, 4, static_cast<double>(base) / rate, rate, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 4; ++i)
                check(std::abs(before.getSample(ch, i) - after.getSample(ch, i)) < 1e-5f,
                      "fractional split retains the source phase within resampler tolerance");
    }
    {
        const double rate = carta::sessionRate;
        const juce::int64 base = 44100 * 4799;
        constexpr int frames = 600, splits = 300;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(1, frames);
        for (int i = 0; i < frames; ++i)
            audio->samples.setSample(0, i, static_cast<float>((i * 137) % 1999 - 999) / 1000.0f);
        carta::Session original;
        original.tracks.push_back(
            {"Long split", {{audio, static_cast<double>(base) / rate, 0, frames / rate}}});
        check(carta::setClipEnvelope(original.tracks[0].clips[0], 0.8f, 100 / rate, 100 / rate),
              "prepare repeated split fade fixture");
        for (int mode = 0; mode < 4; ++mode) {
            auto edited = original;
            juce::String error;
            for (int i = 1; i <= splits; ++i) {
                bool changed = false;
                if (mode == 0)
                    changed = carta::splitClipAtSample(edited, 0, i - 1, base + i);
                else if (mode == 1)
                    changed = carta::splitClipGroupAtSample(edited, {{0, i - 1}}, base + i, error);
                else if (mode == 2)
                    changed = carta::splitTracksAtSample(edited, {0, 0}, base + i);
                else if ((i & 1) != 0)
                    changed = carta::splitRange(edited, {base + i, base + i + 1, 0, 0});
                else
                    continue;
                check(changed, "repeated sample split succeeds through every editing path");
            }
            check(edited.tracks[0].clips.size() == splits + 1,
                  "repeated splits retain every piece");
            for (int i = 0; i <= splits; ++i) {
                const auto& clip = edited.tracks[0].clips[static_cast<size_t>(i)];
                check(std::abs(clip.offset * rate - i) < 2e-9,
                      "repeated long-timeline splits preserve source offsets on the sample grid");
                const int expected = i < splits ? 1 : frames - splits;
                check(std::abs(clip.duration * rate - expected) < 2e-9,
                      "repeated long-timeline splits preserve sample durations");
            }
            juce::AudioBuffer<float> before(2, frames), after(2, frames);
            before.clear();
            after.clear();
            original.render(before, 0, frames, static_cast<double>(base) / rate, rate, 2);
            edited.render(after, 0, frames, static_cast<double>(base) / rate, rate, 2);
            for (int ch = 0; ch < 2; ++ch)
                check(std::memcmp(before.getReadPointer(ch), after.getReadPointer(ch),
                                  frames * sizeof(float)) == 0,
                      "repeated split paths preserve faded rendered samples bit-for-bit");
            if (mode == 0) {
                std::vector<carta::ClipReference> pieces;
                for (int i = 0; i <= splits; ++i)
                    pieces.push_back({0, i});
                check(carta::joinSelectedClipChains(edited, pieces, error) &&
                          edited.tracks[0].clips.size() == 1,
                      "rejoin all faded sample-grid pieces near the end of a disc");
                after.clear();
                edited.render(after, 0, frames, static_cast<double>(base) / rate, rate, 2);
                for (int ch = 0; ch < 2; ++ch)
                    check(std::memcmp(before.getReadPointer(ch), after.getReadPointer(ch),
                                      frames * sizeof(float)) == 0,
                          "rejoining faded native-rate pieces restores the original samples "
                          "bit-for-bit");
            }
        }
        auto mixed = std::make_shared<carta::Audio>();
        mixed->sampleRate = 48000;
        mixed->samples.setSize(2, 1024);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 1024; ++i)
                mixed->samples.setSample(
                    ch, i, std::sin(static_cast<float>(i) * (ch == 0 ? 0.39f : 0.57f)) * 0.5f);
        carta::Session mixedOriginal;
        mixedOriginal.tracks.push_back(
            {"Mixed rate", {{mixed, static_cast<double>(base) / rate, 0, frames / rate}}});
        auto mixedEdited = mixedOriginal;
        for (int i = 1; i <= splits; ++i)
            check(carta::splitClipAtSample(mixedEdited, 0, i - 1, base + i),
                  "split mixed-rate source at long timeline position");
        juce::AudioBuffer<float> mixedBefore(2, frames), mixedAfter(2, frames);
        mixedBefore.clear();
        mixedAfter.clear();
        mixedOriginal.render(mixedBefore, 0, frames, static_cast<double>(base) / rate, rate, 2);
        mixedEdited.render(mixedAfter, 0, frames, static_cast<double>(base) / rate, rate, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < frames; ++i)
                check(std::abs(mixedBefore.getSample(ch, i) - mixedAfter.getSample(ch, i)) < 1e-6f,
                      "repeated mixed-rate splits preserve resampled audio within one millionth");
        std::vector<carta::ClipReference> allPieces;
        for (int i = 0; i <= splits; ++i)
            allPieces.push_back({0, i});
        juce::String joinError;
        const bool rejoined = carta::joinSelectedClipChains(mixedEdited, allPieces, joinError);
        if (!rejoined)
            std::cerr << "Long split join: " << joinError << '\n';
        check(rejoined && mixedEdited.tracks[0].clips.size() == 1,
              "rejoin all mixed-rate split pieces at a long timeline position");
        juce::AudioBuffer<float> joinedAudio(2, frames);
        joinedAudio.clear();
        mixedEdited.render(joinedAudio, 0, frames, static_cast<double>(base) / rate, rate, 2);
        for (int ch = 0; ch < 2; ++ch)
            check(std::memcmp(mixedBefore.getReadPointer(ch), joinedAudio.getReadPointer(ch),
                              frames * sizeof(float)) == 0,
                  "rejoining restores the original mixed-rate samples bit-for-bit");
    }
    {
        const double rate = carta::sessionRate;
        const juce::int64 base = 44100 * 4799LL;
        constexpr int outputFrames = 8192;
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = 48000;
        source->samples.setSize(2, 1024);
        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < 1024; ++i)
                source->samples.setSample(
                    channel, i,
                    static_cast<float>((i * (channel == 0 ? 193 : 347)) % 997 - 498) / 1000.0f);
        carta::Session longLoop;
        longLoop.tracks.push_back(
            {"Long", {{source, static_cast<double>(base) / rate, 0, 800 / rate}}});
        check(carta::setClipEnvelope(longLoop.tracks[0].clips[0], 0.8f, 40 / rate, 60 / rate) &&
                  carta::splitClipAtSample(longLoop, 0, 0, base + 150),
              "prepare faded mixed-rate edit near disc end");
        for (const double outputRate : {32000.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
            carta::ResamplingContext context;
            longLoop.prepareResampling(context, outputRate);
            for (int length : {1, 19, 157}) {
                const carta::PlaybackRange range{static_cast<double>(base + 100) / rate,
                                                 static_cast<double>(base + 100 + length) / rate,
                                                 true};
                juce::AudioBuffer<float> actual(2, outputFrames), reference(2, outputFrames);
                actual.clear();
                reference.clear();
                double cursor = range.start;
                carta::PlaybackClock clock;
                for (int offset = 0; offset < outputFrames;) {
                    const int count = std::min(1 + (offset * 37) % 251, outputFrames - offset);
                    check(carta::renderPlayback(longLoop, actual, offset, count, cursor, outputRate,
                                                2, range, &context, &clock),
                          "long-position loop continues across variable callbacks");
                    offset += count;
                }
                for (int i = 0; i < outputFrames; ++i) {
                    const long double phase =
                        std::fmod(static_cast<long double>(i) * static_cast<long double>(rate) /
                                      static_cast<long double>(outputRate),
                                  static_cast<long double>(length));
                    const double at =
                        static_cast<double>((static_cast<long double>(base + 100) + phase) /
                                            static_cast<long double>(rate));
                    longLoop.render(reference, i, 1, at, outputRate, 2, &context);
                }
                float maximum = 0;
                for (int channel = 0; channel < 2; ++channel)
                    for (int i = 0; i < outputFrames; ++i)
                        maximum = std::max(maximum, std::abs(actual.getSample(channel, i) -
                                                             reference.getSample(channel, i)));
                if (maximum > 2e-5f)
                    std::cerr << "Long-position loop mismatch at " << outputRate << " Hz, "
                              << length << " project samples: " << maximum << '\n';
                check(maximum <= 2e-5f,
                      "short loop near 80 minutes matches independent high-precision phase");
                juce::AudioBuffer<float> stateless(2, outputFrames);
                stateless.clear();
                double statelessPosition = range.start;
                check(carta::renderPlayback(longLoop, stateless, 0, outputFrames, statelessPosition,
                                            outputRate, 2, range, &context),
                      "stateless long-position loop continues in one buffer");
                float statelessMaximum = 0;
                for (int channel = 0; channel < 2; ++channel)
                    for (int i = 0; i < outputFrames; ++i)
                        statelessMaximum =
                            std::max(statelessMaximum, std::abs(stateless.getSample(channel, i) -
                                                                reference.getSample(channel, i)));
                if (statelessMaximum > 2e-5f)
                    std::cerr << "Stateless long-position loop mismatch at " << outputRate
                              << " Hz, " << length << " project samples: " << statelessMaximum
                              << '\n';
                check(statelessMaximum <= 2e-5f,
                      "stateless long-position loop matches independent phase in one buffer");
                juce::AudioBuffer<float> whole(2, outputFrames);
                whole.clear();
                double wholeCursor = range.start;
                carta::PlaybackClock wholeClock;
                check(carta::renderPlayback(longLoop, whole, 0, outputFrames, wholeCursor,
                                            outputRate, 2, range, &context, &wholeClock),
                      "long-position loop renders in one callback");
                for (int channel = 0; channel < 2; ++channel)
                    for (int i = 0; i < outputFrames; ++i)
                        check(std::abs(whole.getSample(channel, i) -
                                       actual.getSample(channel, i)) <= 2e-5f,
                              "long-position loop phase is independent of callback sizes");
                if (length == 19) {
                    cursor = static_cast<double>(base + 110) / rate;
                    juce::AudioBuffer<float> afterSeek(2, 1), expectedSeek(2, 1);
                    afterSeek.clear();
                    expectedSeek.clear();
                    check(carta::renderPlayback(longLoop, afterSeek, 0, 1, cursor, outputRate, 2,
                                                range, &context, &clock),
                          "loop accepts an external seek");
                    longLoop.render(expectedSeek, 0, 1, static_cast<double>(base + 110) / rate,
                                    outputRate, 2, &context);
                    for (int channel = 0; channel < 2; ++channel)
                        check(std::abs(afterSeek.getSample(channel, 0) -
                                       expectedSeek.getSample(channel, 0)) < 2e-5f,
                              "external seek resets retained loop phase");
                    const carta::PlaybackRange changed{static_cast<double>(base + 120) / rate,
                                                       static_cast<double>(base + 130) / rate,
                                                       true};
                    cursor = changed.start;
                    afterSeek.clear();
                    expectedSeek.clear();
                    check(carta::renderPlayback(longLoop, afterSeek, 0, 1, cursor, outputRate, 2,
                                                changed, &context, &clock),
                          "loop accepts changed boundaries");
                    longLoop.render(expectedSeek, 0, 1, changed.start, outputRate, 2, &context);
                    for (int channel = 0; channel < 2; ++channel)
                        check(std::abs(afterSeek.getSample(channel, 0) -
                                       expectedSeek.getSample(channel, 0)) < 2e-5f,
                              "changed loop boundaries reset retained phase");
                }
            }
        }
    }
    {
        const double rate = carta::sessionRate;
        const juce::int64 base = 44100 * 4799LL;
        constexpr int projectFrames = 40000;
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = 48000;
        source->samples.setSize(2, 50000);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 50000; ++i)
                source->samples.setSample(
                    ch, i, static_cast<float>((i * (ch == 0 ? 193 : 347)) % 997 - 498) / 1000.0f);
        carta::Session mix;
        mix.tracks.push_back(
            {"Long", {{source, static_cast<double>(base) / rate, 0, 44000 / rate}}});
        for (const double deviceRate : {32000.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
            const carta::PlaybackRange range{static_cast<double>(base + 100) / rate,
                                             static_cast<double>(base + 100 + projectFrames) / rate,
                                             false};
            const int frames = static_cast<int>(std::ceil(projectFrames * deviceRate / rate));
            carta::ResamplingContext prepared;
            mix.prepareResampling(prepared, deviceRate);
            carta::PlaybackClock clock;
            juce::AudioBuffer<float> actual(2, frames), reference(2, frames);
            actual.clear();
            reference.clear();
            double position = range.start;
            for (int offset = 0; offset < frames;) {
                const int count = 1;
                const bool playing = carta::renderPlayback(mix, actual, offset, count, position,
                                                           deviceRate, 2, range, &prepared, &clock);
                check(playing == (offset + count < frames),
                      "continuous long-position playback stops at its exclusive end");
                offset += count;
            }
            for (int i = 0; i < frames; ++i)
                mix.render(reference, i, 1, range.start + static_cast<double>(i) / deviceRate,
                           deviceRate, 2, &prepared);
            float maximum = 0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < frames; ++i)
                    maximum = std::max(
                        maximum, std::abs(actual.getSample(ch, i) - reference.getSample(ch, i)));
            if (maximum > 1e-6f)
                std::cerr << "Long-position continuous playback mismatch at " << deviceRate
                          << " Hz: " << maximum << '\n';
            check(maximum <= 1e-6f,
                  "continuous playback near 80 minutes matches the independent sample clock");
        }
    }
    {
        const double rate = carta::sessionRate;
        const juce::int64 base = 44100 * 4799;
        constexpr int frames = 800, cutStart = 101, cutEnd = 203;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(1, frames);
        for (int i = 0; i < frames; ++i)
            audio->samples.setSample(0, i, static_cast<float>((i * 127) % 997 - 498) / 1000.0f);
        carta::Session original;
        original.tracks.push_back(
            {"Range", {{audio, static_cast<double>(base) / rate, 0, frames / rate}}});
        const carta::TimeSelection range{base + cutStart, base + cutEnd, 0, 0};
        juce::String error;
        auto copied = carta::copyRange(original, range);
        check(copied.tracks.size() == 1 && copied.tracks[0].size() == 1 &&
                  std::abs(copied.tracks[0][0].offset * rate - cutStart) < 1e-9 &&
                  std::abs(copied.tracks[0][0].duration * rate - (cutEnd - cutStart)) < 1e-9,
              "copy computes exact source offset and duration near the end of a disc");
        carta::Session pasted;
        pasted.tracks.push_back({"Paste", {}});
        check(carta::pasteRange(pasted, copied, 0, base + 300, &error),
              "paste long-timeline range");
        juce::AudioBuffer<float> audioOut(2, frames + cutEnd - cutStart);
        audioOut.clear();
        pasted.render(audioOut, 0, cutEnd - cutStart, static_cast<double>(base + 300) / rate, rate,
                      2);
        for (int i = 0; i < cutEnd - cutStart; ++i)
            check(std::abs(audioOut.getSample(0, i) - audio->samples.getSample(0, i + cutStart)) <
                      1e-7f,
                  "copied and pasted range retains source samples");
        auto removed = original;
        check(carta::removeRange(removed, range, &error), "remove long-timeline range");
        check(removed.tracks[0].clips.size() == 2 &&
                  std::abs(removed.tracks[0].clips[0].duration * rate - cutStart) < 1e-9 &&
                  std::abs(removed.tracks[0].clips[1].offset * rate - cutEnd) < 1e-9 &&
                  std::abs(removed.tracks[0].clips[1].duration * rate - (frames - cutEnd)) < 1e-9,
              "gap deletion keeps exact retained source geometry");
        audioOut.clear();
        removed.render(audioOut, 0, frames, static_cast<double>(base) / rate, rate, 2);
        for (int i = 0; i < frames; ++i)
            check(std::abs(audioOut.getSample(0, i) -
                           (i >= cutStart && i < cutEnd ? 0.0f : audio->samples.getSample(0, i))) <
                      1e-7f,
                  "gap deletion preserves the independent sample map");
        auto ripple = original;
        check(carta::rippleRemoveRange(ripple, range, false, &error), "ripple long-timeline range");
        check(std::abs(ripple.tracks[0].clips[1].offset * rate - cutEnd) < 1e-9 &&
                  std::abs(ripple.tracks[0].clips[1].duration * rate - (frames - cutEnd)) < 1e-9,
              "ripple keeps exact source offset and duration");
        audioOut.clear();
        ripple.render(audioOut, 0, frames - (cutEnd - cutStart), static_cast<double>(base) / rate,
                      rate, 2);
        for (int i = 0; i < frames - (cutEnd - cutStart); ++i)
            check(std::abs(audioOut.getSample(0, i) -
                           audio->samples.getSample(0, i < cutStart ? i : i + cutEnd - cutStart)) <
                      1e-7f,
                  "ripple deletion preserves the independent sample map");
        auto inserted = original;
        check(carta::insertGap(inserted, range, false, error), "insert long-timeline gap");
        check(std::abs(inserted.tracks[0].clips[1].offset * rate - cutStart) < 1e-9 &&
                  std::abs(inserted.tracks[0].clips[1].duration * rate - (frames - cutStart)) <
                      1e-9,
              "gap insertion preserves exact retained source geometry");
        audioOut.clear();
        inserted.render(audioOut, 0, frames + cutEnd - cutStart, static_cast<double>(base) / rate,
                        rate, 2);
        for (int i = 0; i < frames + cutEnd - cutStart; ++i) {
            const float expected = i < cutStart ? audio->samples.getSample(0, i)
                                   : i < cutEnd
                                       ? 0.0f
                                       : audio->samples.getSample(0, i - (cutEnd - cutStart));
            check(std::abs(audioOut.getSample(0, i) - expected) < 1e-7f,
                  "gap insertion preserves the independent sample map");
        }
        auto trimmed = original;
        check(carta::trimTracksToRange(trimmed, range, error), "trim long-timeline range");
        check(std::abs(trimmed.tracks[0].clips[0].offset * rate - cutStart) < 1e-9 &&
                  std::abs(trimmed.tracks[0].clips[0].duration * rate - (cutEnd - cutStart)) < 1e-9,
              "range trim preserves exact source geometry");
        auto moved = original;
        int firstTrack = -1;
        check(carta::splitRangeToNewTracks(moved, range, firstTrack) && firstTrack == 1,
              "move long-timeline range to new track");
        check(std::abs(moved.tracks[1].clips[0].offset * rate - cutStart) < 1e-9 &&
                  std::abs(moved.tracks[1].clips[0].duration * rate - (cutEnd - cutStart)) < 1e-9,
              "moving a range preserves exact source geometry");
        auto staggered = original;
        staggered.tracks[0].clips.push_back(
            {audio, static_cast<double>(base + 150) / rate, 0, 50 / rate});
        const auto staggeredCopy = carta::copyRange(staggered, range);
        check(staggeredCopy.tracks[0].size() == 2 &&
                  std::abs(staggeredCopy.tracks[0][1].start * rate - 49) < 1e-9,
              "copied on-grid clip retains its exact relative insertion offset");
        carta::Session rounded;
        const double edge = static_cast<double>(base + cutStart) / rate;
        rounded.tracks.push_back(
            {"Near boundary",
             {{audio, std::nextafter(edge, -std::numeric_limits<double>::infinity()), 0,
               3 / rate}}});
        check(carta::removeRange(rounded, {base + cutStart, base + cutStart + 1, 0, 0}, &error) &&
                  rounded.tracks[0].clips.size() == 1 && rounded.tracks[0].clips[0].duration > 0,
              "roundoff at a range boundary does not create an empty clip");
    }
    {
        const double rate = carta::sessionRate;
        auto boundaryAudio = std::make_shared<carta::Audio>();
        boundaryAudio->sampleRate = rate;
        boundaryAudio->samples.setSize(1, 5000);
        for (int i = 0; i < 5000; ++i)
            boundaryAudio->samples.setSample(0, i,
                                             static_cast<float>((i * 37) % 199 - 99) / 100.0f);
        for (const auto& point : std::array<std::pair<juce::int64, int>, 4>{
                 {{5, 10}, {1000, 4410}, {2646001, 1}, {209034002, 10}}}) {
            const auto first = point.first, last = first + point.second;
            carta::Session boundary;
            boundary.tracks.push_back(
                {"Boundary",
                 {{boundaryAudio, static_cast<double>(first) / rate, 0, point.second / rate}}});
            boundary.tracks[0].clips[0].groupId = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
            const auto originalClip = boundary.tracks[0].clips[0];
            juce::String error;
            check(carta::copyRange(boundary, {last, last + 1, 0, 0}).tracks[0].empty(),
                  "copy after a sample-aligned clip has no phantom fragment");
            check(carta::rangeMoveLinkedGroups(boundary, {last, last + 1, 0, 0}, {}) == 0,
                  "adjacent time does not warn about a non-overlapping group");
            const carta::TimeSelection adjacent{last, last + 1, 0, 0};
            carta::ClipSelection clipSelection;
            check(clipSelection.restore(boundary, {{0, 0}}, 7) &&
                      !clipSelection.selectTracks(boundary, {0, 0}, &adjacent, 7) &&
                      clipSelection.contains(0, 0, 7),
                  "a touching clip is not selected and the previous selection is retained");
            auto nextClip = boundary;
            nextClip.tracks[0].clips.push_back(
                {boundaryAudio, static_cast<double>(last) / rate, 0, 1 / rate});
            check(clipSelection.selectTracks(nextClip, {0, 0}, &adjacent, 7) &&
                      clipSelection.get(7).size() == 1 && clipSelection.contains(0, 1, 7),
                  "time-scoped selection chooses the overlapping clip without its adjacent "
                  "predecessor");
            carta::ClipOverlapPreview overlapPreview;
            check(carta::clipOverlapRanges(nextClip.tracks[0], static_cast<double>(first) / rate,
                                           static_cast<double>(last + 2) / rate)
                          .empty() &&
                      carta::overlappingClipCounts(nextClip.tracks[0]) ==
                          std::vector<int>({0, 0}) &&
                      carta::overlappingClipCount(nextClip.tracks[0], 0) == 0 &&
                      overlapPreview.prepare(nextClip, {{0, 1}}, error) &&
                      !overlapPreview.overlapsSingle(static_cast<double>(last) / rate, 0),
                  "touching sample-aligned clips agree across overlap display, accessibility "
                  "counts and drag preview");
            check(carta::overlapRegions(nextClip).empty() &&
                      !carta::makeNavigationRows(nextClip)[1].description.contains("Overlaps"),
                  "touching clips do not appear in the overlap browser or accessible clip "
                  "description");
            juce::AudioBuffer<float> boundaryRender(2, 1);
            boundaryRender.clear();
            nextClip.render(boundaryRender, 0, 1, static_cast<double>(last) / rate, rate, 2);
            check(near(boundaryRender.getSample(0, 0), boundaryAudio->samples.getSample(0, 0)),
                  "touching clips do not sum at their shared boundary");
            auto realOverlap = nextClip;
            realOverlap.tracks[0].clips[0].duration = (point.second + 0.25) / rate;
            check(carta::clipOverlapRanges(realOverlap.tracks[0], static_cast<double>(first) / rate,
                                           static_cast<double>(last + 2) / rate)
                              .size() == 1 &&
                      carta::overlappingClipCounts(realOverlap.tracks[0]) ==
                          std::vector<int>({1, 1}) &&
                      overlapPreview.prepare(realOverlap, {{0, 1}}, error) &&
                      overlapPreview.overlapsSingle(static_cast<double>(last) / rate, 0),
                  "a real fractional overlap appears in every overlap indicator");
            check(carta::overlapRegions(realOverlap).size() == 1 &&
                      carta::makeNavigationRows(realOverlap)[1].description.contains(
                          "Overlaps 1 other clip"),
                  "fractional overlap appears in the browser and accessible description");
            boundaryRender.clear();
            realOverlap.render(boundaryRender, 0, 1, static_cast<double>(last) / rate, rate, 2);
            check(near(boundaryRender.getSample(0, 0),
                       boundaryAudio->samples.getSample(0, point.second) +
                           boundaryAudio->samples.getSample(0, 0)),
                  "fractional overlap really sums at the shared project sample");
            auto removed = boundary;
            check(!carta::removeRange(removed, {last, last + 1, 0, 0}, &error) &&
                      std::equal_to<double>{}(removed.tracks[0].clips[0].duration,
                                              originalClip.duration),
                  "deleting after an aligned clip is a true no-op");
            auto trimmed = boundary;
            check(!carta::trimTracksToRange(trimmed, {first, last, 0, 0}, error) &&
                      std::equal_to<double>{}(trimmed.tracks[0].clips[0].duration,
                                              originalClip.duration),
                  "trimming to exact clip bounds preserves the original geometry");
            auto rippled = boundary;
            check(!carta::rippleRemoveRange(rippled, {last, last + 1, 0, 0}, false, &error) &&
                      std::equal_to<double>{}(rippled.tracks[0].clips[0].duration,
                                              originalClip.duration),
                  "ripple after an aligned clip is a true no-op");
            auto inserted = boundary;
            check(!carta::insertGap(inserted, {last, last + 1, 0, 0}, false, error) &&
                      std::equal_to<double>{}(inserted.tracks[0].clips[0].duration,
                                              originalClip.duration),
                  "insertion after an aligned clip does not split it");
            int target = -1;
            auto split = boundary;
            check(!carta::splitRangeToNewTracks(split, {last, last + 1, 0, 0}, target) &&
                      split.tracks.size() == 1,
                  "adjacent selection does not create a new track");
            auto fractional = boundary;
            fractional.tracks[0].clips[0].duration = (point.second + 0.25) / rate;
            const auto copied = carta::copyRange(fractional, {last, last + 1, 0, 0});
            check(copied.tracks[0].size() == 1 && copied.tracks[0][0].duration > 0 &&
                      carta::rangeMoveLinkedGroups(fractional, {last, last + 1, 0, 0}, {}) == 1,
                  "a genuine quarter-sample overlap remains selectable and reports its group");
            check(clipSelection.selectTrackIndices(fractional, {0}, &adjacent, 7) &&
                      clipSelection.contains(0, 0, 7),
                  "a genuine fractional overlap still selects the clip");
            check(carta::removeRange(fractional, {first, last, 0, 0}, &error) &&
                      fractional.tracks[0].clips.size() == 1 &&
                      fractional.tracks[0].clips[0].duration > 0,
                  "range deletion preserves a genuine fractional tail");
        }
    }
    {
        const double rate = carta::sessionRate;
        const juce::int64 base = 44100 * 4799;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(1, 256);
        for (int i = 0; i < 256; ++i)
            audio->samples.setSample(0, i, static_cast<float>((i * 31) % 251 - 125) / 250.0f);
        carta::Session initial;
        initial.tracks.push_back({"A", {{audio, static_cast<double>(base) / rate, 0, 128 / rate}}});
        initial.tracks.push_back(
            {"B", {{audio, static_cast<double>(base + 300) / rate, 0, 128 / rate}}});
        auto moved = initial;
        juce::String error;
        const std::vector<carta::ClipReference> clips{{0, 0}, {1, 0}};
        for (int i = 1; i <= 1000; ++i) {
            check(carta::moveClipGroup(moved, clips, 1, 0, error),
                  "nudge a group forward at a long timeline position");
            for (int t = 0; t < 2; ++t) {
                const auto expected = static_cast<double>(base + i + (t == 0 ? 0 : 300)) / rate;
                check(std::memcmp(&moved.tracks[static_cast<size_t>(t)].clips[0].start, &expected,
                                  sizeof(double)) == 0,
                      "group nudge remains on the exact project-sample grid");
            }
        }
        for (int i = 0; i < 1000; ++i)
            check(carta::moveClipGroup(moved, clips, -1, 0, error),
                  "nudge a group back at a long timeline position");
        for (int t = 0; t < 2; ++t)
            check(std::memcmp(&moved.tracks[static_cast<size_t>(t)].clips[0].start,
                              &initial.tracks[static_cast<size_t>(t)].clips[0].start,
                              sizeof(double)) == 0,
                  "reversing group nudges restores exact clip positions");
        juce::AudioBuffer<float> before(2, 500), after(2, 500);
        before.clear();
        after.clear();
        initial.render(before, 0, 500, static_cast<double>(base) / rate, rate, 2);
        moved.render(after, 0, 500, static_cast<double>(base) / rate, rate, 2);
        for (int ch = 0; ch < 2; ++ch)
            check(std::memcmp(before.getReadPointer(ch), after.getReadPointer(ch),
                              500 * sizeof(float)) == 0,
                  "reversed group nudges restore the original audio bit-for-bit");
        auto fractional = initial;
        fractional.tracks[0].clips[0].start = (static_cast<double>(base) + 0.25) / rate;
        const double start = fractional.tracks[0].clips[0].start;
        check(carta::moveClipGroup(fractional, {{0, 0}}, 1, 0, error) &&
                  std::abs((fractional.tracks[0].clips[0].start - start) * rate - 1) < 1e-4 &&
                  std::abs((fractional.tracks[0].clips[0].start * rate -
                            std::floor(fractional.tracks[0].clips[0].start * rate)) -
                           0.25) < 1e-4,
              "nudging a fractional clip preserves its fractional placement");
        auto transfer = initial;
        const auto preserved = transfer.tracks[0].clips[0].start;
        check(carta::moveClipGroup(transfer, {{0, 0}}, 0, 1, error) &&
                  std::memcmp(&transfer.tracks[1].clips.back().start, &preserved, sizeof(double)) ==
                      0,
              "vertical-only group moves do not change time");
    }
    {
        const double rate = carta::sessionRate;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(1, 1000);
        audio->samples.clear();
        carta::EditorState state;
        state.session.tracks.push_back({"Audio", {{audio, 0, 0, 1000 / rate}}});
        state.session.markers = {{200 / rate, "Outro"}, {0, ""}, {100 / rate, "Intro"}};
        const auto before = state;
        carta::EditHistory history;
        juce::String error;
        check(carta::nameMarkersByTrackNumber(state.session, error) &&
                  state.session.markers[0].name == "Track 3" &&
                  state.session.markers[1].name == "Track 1" &&
                  state.session.markers[2].name == "Track 2",
              "number unsorted markers in master-track order and repair an empty name");
        std::vector<carta::MasterTrack> numbered;
        check(carta::getMasterTracks(state.session, numbered, error) && numbered.size() == 3 &&
                  numbered[0].name == "Track 1" && numbered[2].name == "Track 3",
              "numbered marker names match the exported master-track order");
        check(!carta::nameMarkersByTrackNumber(state.session, error) && error.isEmpty(),
              "numbering already-numbered markers is a no-op");
        history.record(before);
        check(history.undo(state) && state.session.markers[0].name == "Outro" &&
                  history.redo(state) && state.session.markers[0].name == "Track 3",
              "automatic marker naming supports undo and redo");
        auto implicit = before.session;
        implicit.markers = {{200 / rate, "B"}, {100.3 / rate, "A"}};
        check(carta::nameMarkersByTrackNumber(implicit, error) &&
                  implicit.markers[1].name == "Track 2" && implicit.markers[0].name == "Track 3",
              "implicit track one and legacy fractional positions use master-track numbering");
        check(!carta::getMasterTracks(implicit, numbered, error) && error.contains("off-grid"),
              "fractional master-track boundaries require explicit repair");
        check(carta::snapMarkersToSamples(implicit, error),
              "repair fractional boundary before using the track list");
        check(carta::getMasterTracks(implicit, numbered, error) && numbered[0].name == "Track 1" &&
                  numbered[1].name == "Track 2",
              "implicit track numbering matches exported track list");
        auto invalid = before.session;
        invalid.markers[0].position = invalid.markers[2].position;
        check(!carta::nameMarkersByTrackNumber(invalid, error) && error.isNotEmpty() &&
                  invalid.markers[0].name == "Outro" && invalid.markers[2].name == "Intro",
              "duplicate marker positions reject numbering atomically");
        invalid = before.session;
        invalid.markers[0].position = 1000 / rate;
        check(!carta::nameMarkersByTrackNumber(invalid, error) &&
                  invalid.markers[0].name == "Outro",
              "out-of-range markers reject numbering atomically");
        auto renamed = before.session;
        renamed.markers[0].position = 100.25 / rate;
        const auto positionBefore = renamed.markers[0].position;
        check(!carta::renameMarkerIndices(renamed, {0, 0}, {"One", "Two"}, error) &&
                  renamed.markers[0].name == "Outro",
              "duplicate batch rename indices reject atomically");
        check(!carta::renameMarkerIndices(renamed, {0, 2}, {"One", "Bad\nname"}, error) &&
                  renamed.markers[0].name == "Outro",
              "multiline names reject without partial updates");
        check(carta::renameMarkerIndices(
                  renamed, {0, 2}, {juce::String::fromUTF8("終曲"), "Intro revised"}, error) &&
                  renamed.markers[0].name == juce::String::fromUTF8("終曲") &&
                  renamed.markers[1].name.isEmpty() &&
                  std::memcmp(&renamed.markers[0].position, &positionBefore, sizeof(double)) == 0,
              "batch rename preserves unselected and fractional marker positions exactly");
    }
    {
        juce::int64 sample = -1;
        check(carta::parseSampleOrTime(" 44100 ", sample) && sample == 44100,
              "bare integers parse as project samples");
        check(carta::parseSampleOrTime("44100 smp", sample) && sample == 44100 &&
                  carta::parseSampleOrTime("1.000023 s", sample) && sample == 44101 &&
                  carta::parseSampleOrTime("62.5s", sample) && sample == 2756250,
              "formatted sample and seconds values parse for editing");
        check(!carta::parseSampleOrTime("1:02 s", sample) &&
                  !carta::parseSampleOrTime("1.2 smp", sample) &&
                  !carta::parseSampleOrTime("1e3 s", sample),
              "invalid unit suffixes and numeric syntax are rejected");
        check(carta::parseSampleOrTime("1:02.500", sample) && sample == 2756250,
              "minutes and fractional seconds parse to exact samples");
        check(carta::parseSampleOrTime("1:02:03.004", sample) && sample == 3723 * 44100LL + 176,
              "hours and decimal seconds round to a project sample");
        check(carta::parseSampleOrTime("0:00.005", sample) && sample == 221 &&
                  carta::parseSampleOrTime("0:59.999999999", sample) && sample == 60 * 44100,
              "time conversion rounds ties and carries into the next minute");
        check(carta::parseSampleOrTime("9007199254740991", sample) && sample == 9007199254740991LL,
              "largest precise project sample parses");
        for (const auto* invalid :
             {"", "-1", "+1", "1.5", "0:60", "1:60:00", "0:", ":01", "0:01.", "0:01.1234567890",
              "0:01.2.3", "1:2:3:4", "9007199254740992", "99999999999999999999:00", "0:０１"}) {
            sample = 123;
            check(!carta::parseSampleOrTime(juce::String::fromUTF8(invalid), sample) &&
                      sample == 123,
                  "invalid sample or time input is rejected without changing the result");
        }
    }
    check(!carta::outputSupportsMaster(0, 1) && carta::outputSupportsMaster(1, 1) &&
              carta::outputSupportsMaster(2, 1) && !carta::outputSupportsMaster(1, 2) &&
              carta::outputSupportsMaster(2, 2) && !carta::outputSupportsMaster(2, 3),
          "output channel guard preserves stereo and permits mono output");
    {
        carta::Session imported;
        auto first = std::make_shared<carta::Audio>();
        first->file = juce::File::getCurrentWorkingDirectory().getChildFile("First.wav");
        first->sampleRate = carta::sessionRate;
        first->samples.setSize(2, 100);
        auto second = std::make_shared<carta::Audio>();
        second->file = juce::File::getCurrentWorkingDirectory().getChildFile("Second.wav");
        second->sampleRate = carta::sessionRate;
        second->samples.setSize(4, 100);
        first->samples.clear();
        second->samples.clear();
        first->samples.setSample(0, 0, 0.5f);
        second->samples.setSample(0, 0, 0.25f);
        check(carta::addImportedAudioTrack(imported, first, 50) &&
                  carta::addImportedAudioTrack(imported, second, 50),
              "batch import places each source on its own track");
        check(imported.tracks.size() == 2 && imported.tracks[0].name == "Layer 1" &&
                  imported.tracks[1].name == "Layer 2" &&
                  imported.tracks[1].clips[0].audio == second,
              "import keeps source and order with generic layer names");
        check(std::abs(imported.tracks[0].clips[0].start * carta::sessionRate - 50) < 1e-8 &&
                  std::abs(imported.tracks[1].clips[0].offset) < 1e-8 &&
                  carta::maxTrackChannels(imported.tracks[1]) == 4 &&
                  !carta::clipRoutingConfirmed(imported.tracks[1], 4),
              "import preserves exact drop position and multichannel routing requirement");
        juce::AudioBuffer<float> out(2, 2);
        out.clear();
        imported.render(out, 0, 2, 49 / carta::sessionRate, carta::sessionRate, 2);
        check(near(out.getSample(0, 0), 0) && out.getSample(0, 1) > 0.5f,
              "imported audio starts at drop sample");
        check(!carta::addImportedAudioTrack(imported, first, -1) &&
                  !carta::addImportedAudioTrack(imported, {}, 0) && imported.tracks.size() == 2,
              "invalid imported tracks leave arrangement unchanged");
        check(carta::addImportedAudioToTrack(imported, 0, second, 200) &&
                  imported.tracks.size() == 2 && imported.tracks[0].clips.size() == 2 &&
                  imported.tracks[0].clips.back().audio == second &&
                  std::abs(imported.tracks[0].clips.back().start * carta::sessionRate - 200) < 1e-8,
              "import inserts the source at the requested time in an existing track");
        check(!carta::addImportedAudioToTrack(imported, 2, first, 0) &&
                  !carta::addImportedAudioToTrack(imported, 0, first, -1) &&
                  imported.tracks[0].clips.size() == 2,
              "invalid destination or time does not alter the arrangement");
    }
    {
        const carta::TimeSelection original{100, 200, 1, 2};
        carta::TimeSelection fromLength{5, 6, 0, 0};
        check(carta::selectionFromLength(100, 100, 1, 2, 500, 3, fromLength) &&
                  fromLength.start == 100 && fromLength.end == 200 && fromLength.firstTrack == 1 &&
                  fromLength.lastTrack == 2,
              "construct a half-open multi-track selection from length");
        check(carta::selectionFromLength(499, 1, 0, 0, 500, 3, fromLength) && fromLength.end == 500,
              "selection length can end exactly at the timeline limit");
        check(!carta::selectionFromLength(499, 2, 0, 0, 500, 3, fromLength) &&
                  !carta::selectionFromLength(1, 0, 0, 0, 500, 3, fromLength) &&
                  !carta::selectionFromLength(-1, 1, 0, 0, 500, 3, fromLength) &&
                  !carta::selectionFromLength(1, std::numeric_limits<juce::int64>::max(), 0, 0, 500,
                                              3, fromLength) &&
                  fromLength.start == 499 && fromLength.end == 500,
              "invalid and overflowing lengths leave selection unchanged");
        check(!carta::selectionFromLength(1, 2, 1, 3, 500, 3, fromLength) &&
                  !carta::selectionFromLength(1, 2, 2, 1, 500, 3, fromLength),
              "selection length rejects invalid track ranges");
        auto left = carta::resizeTimeSelection(original, true, 150, 1);
        check(left.start == 150 && left.end == 200 && left.firstTrack == 1 && left.lastTrack == 2,
              "resize left selection edge without changing tracks");
        auto reversed = carta::resizeTimeSelection(original, true, 250, 4);
        check(reversed.start == 200 && reversed.end == 250 && reversed.firstTrack == 1 &&
                  reversed.lastTrack == 4,
              "selection endpoint crossing reverses range and extends tracks");
        auto right = carta::resizeTimeSelection(original, false, 50, 0);
        check(right.start == 50 && right.end == 100 && right.firstTrack == 0 &&
                  right.lastTrack == 2,
              "reverse right edge while extending to earlier track");
        auto invalid = carta::resizeTimeSelection(original, false, -1, 0);
        check(invalid.start == 100 && invalid.end == 200 && invalid.firstTrack == 1,
              "invalid selection resize leaves range unchanged");
        auto nudged = original;
        check(carta::nudgeTimeSelection(nudged, carta::SelectionPart::start, 1, 500) &&
                  nudged.start == 101 && nudged.end == 200,
              "nudge selection start by one sample");
        check(carta::nudgeTimeSelection(nudged, carta::SelectionPart::end, -1, 500) &&
                  nudged.start == 101 && nudged.end == 199,
              "nudge exclusive selection end by one sample");
        check(carta::nudgeTimeSelection(nudged, carta::SelectionPart::whole, 7, 500) &&
                  nudged.start == 108 && nudged.end == 206 && nudged.firstTrack == 1 &&
                  nudged.lastTrack == 2,
              "translate selection without changing its length or tracks");
        check(!carta::nudgeTimeSelection(nudged, carta::SelectionPart::start, 98, 500) &&
                  !carta::nudgeTimeSelection(nudged, carta::SelectionPart::end, -98, 500) &&
                  nudged.start == 108 && nudged.end == 206,
              "selection cannot collapse or cross");
        check(!carta::nudgeTimeSelection(nudged, carta::SelectionPart::whole,
                                         std::numeric_limits<juce::int64>::min(), 500) &&
                  !carta::nudgeTimeSelection(nudged, carta::SelectionPart::whole,
                                             std::numeric_limits<juce::int64>::max(), 500) &&
                  nudged.start == 108 && nudged.end == 206,
              "oversized nudges cannot overflow or alter selection");
        check(carta::nudgeTimeSelection(nudged, carta::SelectionPart::whole, -108, 500) &&
                  nudged.start == 0 && nudged.end == 98 &&
                  !carta::nudgeTimeSelection(nudged, carta::SelectionPart::whole, -1, 500),
              "selection stops at timeline start");
        check(carta::nudgeTimeSelection(nudged, carta::SelectionPart::end, 402, 500) &&
                  nudged.end == 500 &&
                  !carta::nudgeTimeSelection(nudged, carta::SelectionPart::end, 1, 500),
              "selection stops at timeline limit");
        carta::TimeSelection extended;
        carta::KeyboardRangeExtension keyboard;
        double cursor = 5 / carta::sessionRate;
        check(carta::extendTimeSelection(extended, cursor, 1, 3, 1, 500, keyboard) &&
                  extended.start == 5 && extended.end == 6 && extended.firstTrack == 1 &&
                  extended.lastTrack == 1,
              "keyboard creates a one-sample selection on the selected track");
        check(carta::extendTimeSelection(extended, cursor, 1, 3, -1, 500, keyboard) &&
                  !extended.hasRange() && keyboard.focus == 5,
              "keyboard selection can collapse to its anchor");
        check(carta::extendTimeSelection(extended, cursor, 1, 3, -1, 500, keyboard) &&
                  extended.start == 4 && extended.end == 5 && keyboard.focus == 4,
              "keyboard selection reverses across the anchor");
        check(carta::extendTimeSelection(extended, cursor, 1, 3, 1, 500, keyboard) &&
                  carta::extendTimeSelection(extended, cursor, 1, 3, 1, 500, keyboard) &&
                  extended.start == 5 && extended.end == 6,
              "keyboard selection can reverse back");
        extended = {10, 20, 1, 2};
        cursor = 10 / carta::sessionRate;
        check(carta::extendTimeSelection(extended, cursor, 1, 3, -1, 500, keyboard) &&
                  extended.start == 9 && extended.end == 20 && extended.lastTrack == 2,
              "keyboard extends existing selection at playhead edge across its tracks");
        extended = {10, 20, 1, 2};
        cursor = 15 / carta::sessionRate;
        check(carta::extendTimeSelection(extended, cursor, 1, 3, -1, 500, keyboard) &&
                  extended.start == 10 && extended.end == 19,
              "changed playhead restarts gesture at existing selection end");
        extended = {};
        cursor = 0;
        keyboard = {};
        check(!carta::extendTimeSelection(extended, cursor, -1, 3, 1, 500, keyboard) &&
                  !keyboard.active,
              "keyboard requires a selected track when creating a range");
        check(!carta::extendTimeSelection(extended, cursor, 0, 3, -1, 500, keyboard) &&
                  !keyboard.active,
              "keyboard selection stops at zero without changing state");
        cursor = 500 / carta::sessionRate;
        check(!carta::extendTimeSelection(extended, cursor, 0, 3, 1, 500, keyboard) &&
                  !keyboard.active,
              "keyboard selection stops at the timeline limit");
        extended = {100, 100, 2, 2};
        cursor = 5 / carta::sessionRate;
        keyboard = {};
        check(carta::extendTimeSelection(extended, cursor, 0, 3, 1, 500, keyboard) &&
                  extended.start == 100 && extended.end == 101 && extended.firstTrack == 2 &&
                  keyboard.independent,
              "keyboard selection starts at the edit point on its track instead of the playhead");
        cursor = 17 / carta::sessionRate;
        check(carta::extendTimeSelection(extended, cursor, 0, 3, 1, 500, keyboard) &&
                  extended.start == 100 && extended.end == 102,
              "independent keyboard extension keeps its anchor while playback advances");
        cursor = 18 / carta::sessionRate;
        check(carta::extendTimeSelection(extended, cursor, 0, 3, -1, 500, keyboard) &&
                  carta::extendTimeSelection(extended, cursor, 0, 3, -1, 500, keyboard) &&
                  extended.hasPoint() && extended.start == 100 &&
                  carta::extendTimeSelection(extended, cursor, 0, 3, -1, 500, keyboard) &&
                  extended.start == 99 && extended.end == 100,
              "independent selection can collapse and reverse at the original point");
        extended = {10, 20, 1, 2};
        cursor = 15 / carta::sessionRate;
        keyboard = {};
        check(carta::extendTimeSelection(extended, cursor, 1, 3, 1, 500, keyboard, true) &&
                  extended.end == 21,
              "playing range extension starts at the current selection edge");
        cursor = 16 / carta::sessionRate;
        check(carta::extendTimeSelection(extended, cursor, 1, 3, 1, 500, keyboard, true) &&
                  extended.end == 22,
              "playing range extension continues even as the playhead moves");
        carta::TimeSelection tracks{10, 20, 1, 1};
        carta::KeyboardTrackExtension vertical;
        check(carta::extendSelectionTracks(tracks, 1, 4, 1, vertical) && tracks.firstTrack == 1 &&
                  tracks.lastTrack == 2 && tracks.start == 10 && tracks.end == 20,
              "keyboard extends the time selection to the next track without changing its time");
        check(carta::extendSelectionTracks(tracks, 2, 4, -1, vertical) && tracks.firstTrack == 1 &&
                  tracks.lastTrack == 1,
              "keyboard contracts back to the anchored track");
        check(carta::extendSelectionTracks(tracks, 1, 4, -1, vertical) && tracks.firstTrack == 0 &&
                  tracks.lastTrack == 1 && vertical.focus == 0,
              "keyboard reverses past the track anchor");
        check(!carta::extendSelectionTracks(tracks, 0, 4, -1, vertical) && vertical.focus == 0 &&
                  tracks.firstTrack == 0,
              "keyboard track selection stops at the first track");
        check(carta::extendSelectionTracks(tracks, 0, 4, 1, vertical) &&
                  carta::extendSelectionTracks(tracks, 1, 4, 1, vertical) &&
                  carta::extendSelectionTracks(tracks, 2, 4, 1, vertical) &&
                  tracks.lastTrack == 3 && !carta::extendSelectionTracks(tracks, 3, 4, 1, vertical),
              "keyboard track selection reverses and stops at the last track");
        tracks = {30, 40, 0, 3};
        vertical = {};
        check(carta::extendSelectionTracks(tracks, 0, 4, -1, vertical) && tracks.firstTrack == 0 &&
                  tracks.lastTrack == 2,
              "existing all-track selection contracts from the opposite edge");
        tracks = {50, 60, 1, 2};
        check(carta::extendSelectionTracks(tracks, 2, 4, -1, vertical) && tracks.firstTrack == 0 &&
                  tracks.lastTrack == 2,
              "independently changed selection restarts the vertical anchor");
        tracks = {};
        vertical = {};
        check(!carta::extendSelectionTracks(tracks, 0, 4, 1, vertical) && !vertical.active,
              "keyboard track extension requires a time range");
        carta::SelectionMemory memory;
        carta::TimeSelection remembered{10, 250, 1, 2}, recalled{0, 5, 0, 0};
        check(!memory.canRestore(3, 7) && !memory.remember({}, 3, 7) &&
                  memory.remember(remembered, 3, 7),
              "selection memory requires a valid range");
        check(memory.restore(recalled, 3, 7) && recalled.start == 10 && recalled.end == 250 &&
                  recalled.firstTrack == 1 && recalled.lastTrack == 2,
              "selection memory restores exact half-open samples and rows");
        recalled = {1, 2, 0, 0};
        check(!memory.restore(recalled, 3, 8) && !memory.restore(recalled, 2, 7) &&
                  recalled.start == 1 && recalled.end == 2 && recalled.firstTrack == 0,
              "selection memory rejects stale project generations and missing tracks without "
              "mutation");
        check(!memory.remember({10, 20, 0, 3}, 3, 7) && memory.restore(recalled, 3, 7) &&
                  recalled.start == 10,
              "invalid new bookmark does not replace the saved selection");
        memory.retainAfterGeometryStableEdit(7, 8);
        check(memory.restore(recalled, 3, 8) && recalled.start == 10 && !memory.canRestore(3, 7),
              "selection memory advances through a geometry-stable edit");
        memory.retainAfterGeometryStableEdit(7, 9);
        check(memory.canRestore(3, 8) && !memory.canRestore(3, 9),
              "stale bookmark generation cannot be revived by a later stable edit");
        {
            carta::ZoomMemory zoomMemory;
            carta::TimelineViewport view{30, 5};
            check(!zoomMemory.toggle(view, 100) && !zoomMemory.available(),
                  "zoom toggle requires a prior view");
            check(zoomMemory.remember({10, 20}) && !zoomMemory.remember({-1, 1}) &&
                      zoomMemory.available(),
                  "invalid zoom memory does not replace a previous view");
            check(zoomMemory.toggle(view, 100) && std::abs(view.start - 10) < 1e-12 &&
                      std::abs(view.span - 20) < 1e-12,
                  "toggle restores prior start and span");
            check(zoomMemory.toggle(view, 100) && std::abs(view.start - 30) < 1e-12 &&
                      std::abs(view.span - 5) < 1e-12,
                  "repeated toggle returns to the detailed view");
            check(zoomMemory.toggle(view, 12) && std::abs(view.start) < 1e-12 &&
                      std::abs(view.span - 12) < 1e-12,
                  "zoom memory clamps to a shortened project");
            auto unchanged = view;
            check(!zoomMemory.toggle(view, 0) && std::memcmp(&view, &unchanged, sizeof(view)) == 0,
                  "invalid timeline duration preserves the view");
            check(zoomMemory.remember({0, 1e-8}) && zoomMemory.toggle(view, 12) &&
                      std::abs(view.span - 16.0 / carta::sessionRate) < 1e-12,
                  "zoom restore respects the minimum visible span");
            zoomMemory.clear();
            check(!zoomMemory.available() && !zoomMemory.toggle(view, 12),
                  "new project can clear the zoom memory");
        }
        carta::Session timeline;
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = carta::sessionRate;
        source->samples.setSize(1, 500);
        timeline.tracks.push_back(
            {"A",
             {{source, 100 / carta::sessionRate, 0, 30 / carta::sessionRate},
              {source, 300.2 / carta::sessionRate, 0, 30 / carta::sessionRate}}});
        timeline.markers.push_back({200 / carta::sessionRate, "marker"});
        const auto targets = carta::timelineSnapTargets(
            timeline, original, 400 / carta::sessionRate, 0, 0, true, true, true);
        const auto contains = [&](double sample) {
            return std::any_of(targets.begin(), targets.end(), [&](double value) {
                return std::abs(value * carta::sessionRate - sample) < 1e-8;
            });
        };
        check(contains(0) && contains(200) && contains(400) && contains(300) && contains(330),
              "timeline targets include marker, selection, playhead and sample-rounded clip edges");
        check(!contains(130), "dragged clip boundaries are excluded from snapping");
        const auto noMarkers = carta::timelineSnapTargets(timeline, {}, 400 / carta::sessionRate,
                                                          -1, -1, false, false, true);
        check(std::none_of(
                  noMarkers.begin(), noMarkers.end(),
                  [](double value) { return std::abs(value * carta::sessionRate - 200) < 1e-8; }),
              "marker gesture can exclude duplicate marker targets");
        const auto playheadTargets = carta::timelineSnapTargets(
            timeline, original, std::numeric_limits<double>::quiet_NaN(), -1, -1, true, true, true);
        check(std::none_of(playheadTargets.begin(), playheadTargets.end(),
                           [](double value) {
                               return !std::isfinite(value) ||
                                      std::abs(value * carta::sessionRate - 400) < 1e-8;
                           }) &&
                  std::any_of(playheadTargets.begin(), playheadTargets.end(),
                              [](double value) {
                                  return std::abs(value * carta::sessionRate - 200) < 1e-8;
                              }),
              "playhead dragging snaps to markers without snapping to its old position");
        {
            const carta::TimeSelection scope{350, 360, 0, 0};
            for (int mask = 0; mask < 32; ++mask) {
                carta::SnapTargetOptions options{(mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0,
                                                 (mask & 8) != 0, (mask & 16) != 0};
                auto one = carta::timelineSnapTargets(timeline, scope, 400 / carta::sessionRate, 0,
                                                      0, true, true, false, options);
                const auto groupTargets = carta::clipGroupSnapTargets(
                    timeline, scope, 400 / carta::sessionRate, {{0, 0}}, options);
                std::sort(one.begin(), one.end());
                one.erase(std::unique(one.begin(), one.end()), one.end());
                check(one == groupTargets,
                      "single and group drag target filters agree for every option combination");
                const auto has = [&](double frame) {
                    return std::any_of(one.begin(), one.end(), [&](double time) {
                        return std::abs(time * carta::sessionRate - frame) < 1e-7;
                    });
                };
                check(has(300.2) == options.clipEdges && has(330.2) == options.clipEdges &&
                          has(200) == options.markers && has(350) == options.selection &&
                          has(360) == options.selection && has(400) == options.playhead &&
                          has(0) == options.timelineStart && !has(100) && !has(130),
                      "target options independently select boundaries while excluding the dragged "
                      "clip");
                const auto markerDrag = carta::timelineSnapTargets(
                    timeline, scope, 400 / carta::sessionRate, -1, -1, false, false, true, options);
                const auto safe = [&](double frame) {
                    return std::any_of(markerDrag.begin(), markerDrag.end(), [&](double time) {
                        return std::abs(time * carta::sessionRate - frame) < 1e-7;
                    });
                };
                check(!safe(200) && !safe(350) && !safe(360),
                      "gesture exclusions override enabled marker and selection options");
            }
        }
        juce::int64 start = -1, end = -1, boundary = -1;
        check(carta::clipSampleRange(timeline.tracks[0].clips[1], start, end) && start == 301 &&
                  end == 331,
              "fractional clip bounds select precisely the contained project sample timestamps");
        const auto& fractionalClip = timeline.tracks[0].clips[1];
        check(carta::clipSampleRangeWithin(fractionalClip, 305.1 / carta::sessionRate,
                                           310.2 / carta::sessionRate, start, end) &&
                  start == 306 && end == 311,
              "clip portion selects project sample timestamps in a fractional half-open interval");
        check(
            carta::clipSampleRangeWithin(fractionalClip, 0, 310 / carta::sessionRate, start, end) &&
                start == 301 && end == 310,
            "clip portion before playhead excludes the playhead sample");
        check(carta::clipSampleRangeWithin(fractionalClip, 310 / carta::sessionRate,
                                           400 / carta::sessionRate, start, end) &&
                  start == 310 && end == 331,
              "clip portion after playhead includes the playhead sample");
        check(
            carta::clipSampleRangeWithin(fractionalClip, 0, 400 / carta::sessionRate, start, end) &&
                start == 301 && end == 331,
            "clip portion clamps to both clip edges");
        start = 17;
        end = 23;
        check(!carta::clipSampleRangeWithin(fractionalClip, 331 / carta::sessionRate,
                                            400 / carta::sessionRate, start, end) &&
                  start == 17 && end == 23,
              "disjoint clip portion preserves previous bounds");
        check(!carta::clipSampleRangeWithin(fractionalClip, 305.1 / carta::sessionRate,
                                            305.2 / carta::sessionRate, start, end) &&
                  start == 17 && end == 23,
              "subsample interval without project sample timestamps is empty");
        check(!carta::clipSampleRangeWithin(fractionalClip, 0,
                                            std::numeric_limits<double>::infinity(), start, end) &&
                  !carta::clipSampleRangeWithin(fractionalClip, -1, 1, start, end) &&
                  !carta::clipSampleRangeWithin(fractionalClip, 2, 1, start, end) && start == 17 &&
                  end == 23,
              "invalid interval rejects without mutating bounds");
        check(carta::adjacentClipBoundary(timeline, 0, 100 / carta::sessionRate, true, boundary) &&
                  boundary == 130,
              "next boundary skips the current edge");
        check(
            carta::adjacentClipBoundary(timeline, 0, 130.25 / carta::sessionRate, true, boundary) &&
                boundary == 301,
            "navigation handles fractional playhead positions");
        check(carta::adjacentClipBoundary(timeline, 0, 301 / carta::sessionRate, false, boundary) &&
                  boundary == 130,
              "previous boundary skips the current edge");
        timeline.tracks.push_back(
            {"B", {{source, 200 / carta::sessionRate, 0, 100 / carta::sessionRate}}});
        check(carta::adjacentClipBoundary(timeline, -1, 130 / carta::sessionRate, true, boundary) &&
                  boundary == 200,
              "all-track navigation includes other rows");
        check(carta::adjacentClipBoundary(timeline, 0, 130 / carta::sessionRate, true, boundary) &&
                  boundary == 301,
              "track-scoped navigation excludes other rows");
        boundary = 42;
        check(!carta::adjacentClipBoundary(timeline, 0, 0, false, boundary) && boundary == 42 &&
                  !carta::adjacentClipBoundary(timeline, 5, 0, true, boundary),
              "missing or invalid boundary leaves output unchanged");
        auto empty = timeline.tracks[0].clips[0];
        empty.start = 1.1 / carta::sessionRate;
        empty.duration = 0.1 / carta::sessionRate;
        check(!carta::clipSampleRange(empty, start, end),
              "clip with no project sample timestamps has no selectable sample range");
        carta::Track order;
        order.clips = {{source, 3, 0, 1}, {source, 1, 0, 2}, {source, 1, 0, 1}, {source, 1, 0, 1}};
        int hit = -1;
        check(carta::adjacentClipIndex(order, -1, true, hit) && hit == 2,
              "keyboard clip navigation starts in timeline order");
        check(carta::adjacentClipIndex(order, hit, true, hit) && hit == 3,
              "coincident clips remain individually selectable");
        check(carta::adjacentClipIndex(order, hit, true, hit) && hit == 1 &&
                  carta::adjacentClipIndex(order, hit, true, hit) && hit == 0,
              "clip navigation sorts by start then end");
        check(!carta::adjacentClipIndex(order, hit, true, hit) && hit == 0,
              "next clip does not wrap or change selection");
        check(carta::adjacentClipIndex(order, -1, false, hit) && hit == 0 &&
                  carta::adjacentClipIndex(order, hit, false, hit) && hit == 1,
              "reverse navigation begins at last clip");
        order.clips[1].start = std::numeric_limits<double>::quiet_NaN();
        check(!carta::adjacentClipIndex(order, 1, true, hit) &&
                  carta::adjacentClipIndex(order, 3, true, hit) && hit == 0,
              "invalid geometry cannot corrupt traversal");
        check(!carta::adjacentClipIndex(order, 100, true, hit) &&
                  !carta::adjacentClipIndex(carta::Track{}, -1, true, hit),
              "invalid selection and empty track have no next clip");
        carta::EditorState renameState;
        renameState.session = timeline;
        carta::EditHistory renameHistory;
        renameHistory.record(renameState);
        renameState.session.tracks[0].clips[0].name = "Alternate take";
        check(renameHistory.undo(renameState) &&
                  renameState.session.tracks[0].clips[0].name.isEmpty() &&
                  renameHistory.redo(renameState) &&
                  renameState.session.tracks[0].clips[0].name == "Alternate take",
              "clip rename is undoable and redoable");
    }
    {
        const double rate = carta::sessionRate;
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = rate;
        source->samples.setSize(1, 1000);
        for (int i = 0; i < 1000; ++i)
            source->samples.setSample(0, i, 0.1f + static_cast<float>(i) / 5000);
        carta::Session original;
        original.tracks.push_back(
            {"selected", {{source, 0, 0, 1000 / rate}, {source, 100 / rate, 0, 700 / rate}}});
        original.tracks.push_back({"other", {{source, 0, 0, 1000 / rate}}});
        original.markers = {{900 / rate, "late"}};
        check(carta::setClipEnvelope(original.tracks[0].clips[0], 0.7f, 100 / rate, 200 / rate),
              "prepare trim-to-selection fade");
        carta::EditorState state;
        state.session = original;
        auto previous = state;
        carta::EditHistory history;
        juce::String error;
        check(carta::trimTracksToRange(state.session, {200, 800, 0, 0}, error) &&
                  state.session.tracks[0].clips.size() == 2 &&
                  state.session.tracks[1].clips.size() == 1,
              "trim selected track retains overlapping pieces and other tracks");
        juce::AudioBuffer<float> before(2, 1000), after(2, 1000);
        before.clear();
        after.clear();
        original.render(before, 0, 1000, 0, rate, 2, nullptr, {0, 0});
        state.session.render(after, 0, 1000, 0, rate, 2, nullptr, {0, 0});
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 1000; ++i)
                check(
                    near(after.getSample(ch, i), i >= 200 && i < 800 ? before.getSample(ch, i) : 0),
                    "trim preserves selected audio and source-anchored fades inside range");
        check(state.session.markers.size() == 1 && carta::projectEndSample(state.session) == 1000,
              "other track preserves master and markers during trim");
        history.record(previous);
        check(history.undo(state) && state.session.tracks[0].clips[0].start < 1e-12 &&
                  history.redo(state) &&
                  std::abs(state.session.tracks[0].clips[0].start * rate - 200) < 1e-7,
              "trim to selection is undoable and redoable");
        auto blocked = original;
        check(!carta::trimTracksToRange(blocked, {200, 800, 0, 1}, error) && error.isNotEmpty() &&
                  carta::projectEndSample(blocked) == 1000 && blocked.tracks[0].clips.size() == 2,
              "trim rejects orphaned marker atomically");
        blocked.markers.clear();
        check(carta::trimTracksToRange(blocked, {200, 800, 0, 1}, error) &&
                  carta::projectEndSample(blocked) == 800,
              "trim all selected tracks after marker repair");
        check(!carta::trimTracksToRange(blocked, {200, 800, 0, 1}, error) && error.isNotEmpty(),
              "repeated trim is a no-op");
    }
    {
        const double rate = carta::sessionRate;
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = rate;
        source->samples.setSize(1, 1000);
        carta::Session master;
        master.tracks.push_back({"whole", {{source, 0, 0, 1000 / rate}}});
        master.tracks.push_back({"inner", {{source, 200 / rate, 0, 600 / rate}}});
        master.markers = {{700 / rate, "later"},
                          {300 / rate, "earlier"},
                          {300 / rate, "duplicate"},
                          {1200 / rate, "outside"},
                          {std::numeric_limits<double>::quiet_NaN(), "invalid"}};
        carta::TimeSelection selected{3, 5, 0, 0};
        check(
            carta::selectToBoundary(master, 500, carta::SelectionBoundary::masterStart, selected) &&
                selected.start == 0 && selected.end == 500 && selected.lastTrack == 1,
            "select playhead to master start across tracks");
        check(carta::selectToBoundary(master, 500, carta::SelectionBoundary::masterEnd, selected) &&
                  selected.start == 500 && selected.end == 1000,
              "select playhead to exclusive master end");
        check(carta::selectToBoundary(master, 500, carta::SelectionBoundary::previousMarker,
                                      selected) &&
                  selected.start == 300 && selected.end == 500,
              "select to previous valid marker");
        check(
            carta::selectToBoundary(master, 500, carta::SelectionBoundary::nextMarker, selected) &&
                selected.start == 500 && selected.end == 700,
            "select to next valid marker despite invalid entries");
        check(carta::selectToBoundary(master, 500, carta::SelectionBoundary::previousClip,
                                      selected) &&
                  selected.start == 200 && selected.end == 500,
              "select to previous clip edge across tracks");
        check(carta::selectToBoundary(master, 500, carta::SelectionBoundary::nextClip, selected) &&
                  selected.start == 500 && selected.end == 800,
              "select to next clip edge across tracks");
        check(
            !carta::selectToBoundary(master, 700, carta::SelectionBoundary::nextMarker, selected) &&
                !carta::selectToBoundary(master, 0, carta::SelectionBoundary::masterStart,
                                         selected) &&
                !carta::selectToBoundary(master, 1001, carta::SelectionBoundary::masterEnd,
                                         selected) &&
                selected.start == 500 && selected.end == 800,
            "missing or empty boundaries preserve existing selection");
        check(carta::selectToBoundary(master, 500, carta::SelectionBoundary::previousClip, selected,
                                      {0, 0}) &&
                  selected.start == 0 && selected.end == 500 && selected.firstTrack == 0 &&
                  selected.lastTrack == 0,
              "scoped previous edge ignores other tracks");
        check(carta::selectToBoundary(master, 500, carta::SelectionBoundary::nextClip, selected,
                                      {0, 0}) &&
                  selected.start == 500 && selected.end == 1000,
              "scoped next edge ignores closer edges on excluded tracks");
        check(carta::selectToBoundary(master, 500, carta::SelectionBoundary::nextClip, selected,
                                      {1, 1}) &&
                  selected.end == 800 && selected.firstTrack == 1 && selected.lastTrack == 1,
              "scoped next edge uses the chosen track");
        check(carta::selectToBoundary(master, 500, carta::SelectionBoundary::previousMarker,
                                      selected, {1, 1}) &&
                  selected.start == 300 && selected.end == 500 && selected.firstTrack == 1 &&
                  selected.lastTrack == 1,
              "scoped marker selection retains project-wide marker boundaries");
        check(carta::selectToBoundary(master, 500, carta::SelectionBoundary::nextClip, selected,
                                      {0, 1}) &&
                  selected.end == 800 && selected.lastTrack == 1,
              "multi-track scope chooses the nearest included edge");
        check(!carta::selectToBoundary(master, 500, carta::SelectionBoundary::masterStart, selected,
                                       {-1, 0}) &&
                  !carta::selectToBoundary(master, 500, carta::SelectionBoundary::masterStart,
                                           selected, {1, 2}) &&
                  selected.start == 500 && selected.end == 800,
              "invalid track scopes leave the selection unchanged");
        master.tracks.push_back({"outer", {{source, 100 / rate, 0, 800 / rate}}});
        check(carta::selectToBoundaryOnTracks(master, 500, carta::SelectionBoundary::nextClip,
                                              {0, 2}, selected) &&
                  selected.end == 900 && selected.firstTrack == 0 && selected.lastTrack == 2,
              "nonadjacent next-boundary selection ignores an intervening track");
        check(carta::selectToBoundaryOnTracks(master, 500, carta::SelectionBoundary::previousClip,
                                              {2, 0}, selected) &&
                  selected.start == 100,
              "nonadjacent previous-boundary selection uses closest included edge regardless of "
              "order");
        check(!carta::selectToBoundaryOnTracks(master, 500, carta::SelectionBoundary::nextClip,
                                               {0, 0}, selected) &&
                  !carta::selectToBoundaryOnTracks(master, 500, carta::SelectionBoundary::nextClip,
                                                   {0, 3}, selected) &&
                  selected.start == 100,
              "invalid independent track lists preserve the previous boundary selection");
    }
    {
        const double rate = carta::sessionRate;
        auto sourceGap = std::make_shared<carta::Audio>();
        sourceGap->sampleRate = rate;
        sourceGap->samples.setSize(1, 1000);
        sourceGap->samples.clear();
        carta::Session gaps;
        gaps.minimumEndSample = 1000;
        gaps.tracks.push_back({"Gaps",
                               {{sourceGap, 100 / rate, 0, 100 / rate},
                                {sourceGap, 180 / rate, 0, 70 / rate},
                                {sourceGap, 400 / rate, 0, 100 / rate}}});
        gaps.tracks.push_back({"Other", {{sourceGap, 250 / rate, 0, 150 / rate}}});
        gaps.tracks[0].muted = true;
        carta::TimeSelection selectedGap{7, 8, 1, 1};
        check(carta::selectClipGap(gaps, 0, 0, selectedGap) && selectedGap.start == 0 &&
                  selectedGap.end == 100 && selectedGap.firstTrack == 0,
              "select leading gap on muted track");
        check(carta::selectClipGap(gaps, 0, 250, selectedGap) && selectedGap.start == 250 &&
                  selectedGap.end == 400,
              "select internal gap at exclusive clip end despite audio on another track");
        check(carta::selectClipGap(gaps, 0, 999, selectedGap) && selectedGap.start == 500 &&
                  selectedGap.end == 1000,
              "select silent tail bounded by explicit master end");
        check(!carta::selectClipGap(gaps, 0, 100, selectedGap) &&
                  !carta::selectClipGap(gaps, 0, 249, selectedGap) &&
                  !carta::selectClipGap(gaps, 0, 400, selectedGap) &&
                  !carta::selectClipGap(gaps, 0, 1000, selectedGap) &&
                  !carta::selectClipGap(gaps, 2, 50, selectedGap) && selectedGap.start == 500 &&
                  selectedGap.end == 1000,
              "occupied, end and invalid points preserve the existing selection");
        gaps.tracks.push_back({"Empty", {}});
        check(carta::selectClipGap(gaps, 2, 50, selectedGap) && selectedGap.start == 0 &&
                  selectedGap.end == 1000 && selectedGap.firstTrack == 2,
              "an empty track selects its entire master span");
        auto fractional = std::make_shared<carta::Audio>();
        fractional->sampleRate = 48000;
        fractional->samples.setSize(1, 100);
        fractional->samples.clear();
        gaps.tracks[2].clips = {{fractional, 100.2 / rate, 0, 50 / rate}};
        check(carta::selectClipGap(gaps, 2, 100, selectedGap) && selectedGap.end == 101 &&
                  !carta::selectClipGap(gaps, 2, 101, selectedGap) &&
                  carta::selectClipGap(gaps, 2, 151, selectedGap) && selectedGap.start == 151,
              "fractional clip edges use half-open project-sample occupancy");
        gaps.tracks[2].clips = {{sourceGap, 0, 0, 270 / rate},
                                {sourceGap, 350 / rate, 0, 250 / rate}};
        check(carta::selectClipGapOnTracks(gaps, {2, 0}, 300, selectedGap) &&
                  selectedGap.start == 270 && selectedGap.end == 350 &&
                  selectedGap.firstTrack == 0 && selectedGap.lastTrack == 2,
              "common gap intersects independent rows and ignores intervening audio");
        check(!carta::selectClipGapOnTracks(gaps, {0, 1, 2}, 300, selectedGap) &&
                  !carta::selectClipGapOnTracks(gaps, {0, 0}, 300, selectedGap) &&
                  !carta::selectClipGapOnTracks(gaps, {}, 300, selectedGap) &&
                  !carta::selectClipGapOnTracks(gaps, {0, 8}, 300, selectedGap) &&
                  selectedGap.start == 270 && selectedGap.end == 350,
              "occupied, duplicate, empty and invalid track scopes preserve the previous result");
    }
    {
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = 44100;
        audio->samples.setSize(2, 1024 * 1024);
        audio->samples.clear();
        audio->samples.setSample(0, 543210, 1.0f);
        audio->samples.setSample(1, 543211, 0.5f);
        auto overview = carta::makeSpectrogramTile(*audio, 0, 8192, 128);
        check(overview.isValid() && overview.getWidth() == 128 && overview.getHeight() == 192,
              "spectrogram tile covers requested source range and channels");
        const int column = 543210 / 8192;
        bool transientLeft = false, transientRight = false;
        for (int y = 0; y < 96; ++y) {
            transientLeft |= overview.getPixelAt(column, y).getBrightness() > 0;
            transientRight |= overview.getPixelAt(column, y + 96).getBrightness() > 0;
        }
        check(transientLeft && transientRight,
              "wide spectrogram column captures brief transients in both channels");
        auto detail = carta::makeSpectrogramTile(*audio, 543200, 1, 32);
        check(detail.isValid() && detail.getWidth() == 32,
              "spectrogram tile resolves a short source window");
        auto full = carta::makeSpectrogramTile(*audio, 500000, 512, 64),
             a = carta::makeSpectrogramTile(*audio, 500000, 512, 32),
             b = carta::makeSpectrogramTile(*audio, 516384, 512, 32);
        check(full.isValid() && a.isValid() && b.isValid(), "adjacent spectrogram tiles compute");
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 64; ++x)
                check(full.getPixelAt(x, y) ==
                          (x < 32 ? a.getPixelAt(x, y) : b.getPixelAt(x - 32, y)),
                      "tile boundaries do not change spectral values");
        auto tones = std::make_shared<carta::Audio>();
        tones->sampleRate = 44100;
        tones->samples.setSize(2, 44100);
        for (int i = 0; i < 44100; ++i) {
            tones->samples.setSample(
                0, i,
                0.8f * std::sin(static_cast<float>(2.0 * juce::MathConstants<double>::pi * 1000 *
                                                   i / 44100)));
            tones->samples.setSample(
                1, i,
                0.8f * std::sin(static_cast<float>(2.0 * juce::MathConstants<double>::pi * 8000 *
                                                   i / 44100)));
        }
        const auto toneTile = carta::makeSpectrogramTile(*tones, 10000, 256, 32);
        check(toneTile.isValid(), "spectrogram frequency fixture computes");
        for (int ch = 0; ch < 2; ++ch) {
            int brightest = 0;
            float maximum = -1;
            for (int y = 0; y < 96; ++y) {
                const auto light = toneTile.getPixelAt(16, ch * 96 + y).getBrightness();
                if (light > maximum) {
                    maximum = light;
                    brightest = y;
                }
            }
            const double frequency = ch == 0 ? 1000.0 : 8000.0;
            const int expected = static_cast<int>(
                std::round(95.0 * (1.0 - std::log(frequency / 20.0) / std::log(22050.0 / 20.0))));
            check(maximum > 0.5f && std::abs(brightest - expected) <= 4,
                  "log-frequency rows align with source channel tone");
        }
        carta::Clip spectralClip{tones, 0, 11000 / 44100.0, 7000 / 44100.0};
        check(carta::setClipEnvelope(spectralClip, 0.5f, 3000 / 44100.0, 2000 / 44100.0),
              "prepare trimmed faded spectral clip");
        carta::Audio processed;
        processed.sampleRate = tones->sampleRate;
        processed.samples.setSize(2, 44100);
        processed.samples.clear();
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 44100; ++i) {
                const double time = static_cast<double>(i) / processed.sampleRate;
                if (time >= spectralClip.offset &&
                    time < spectralClip.offset + spectralClip.duration)
                    processed.samples.setSample(ch, i,
                                                tones->samples.getSample(ch, i) *
                                                    carta::clipLevel(spectralClip, time));
            }
        const auto clipTile = carta::makeClipSpectrogramTile(spectralClip, 9000, 256, 48);
        const auto processedTile = carta::makeSpectrogramTile(processed, 9000, 256, 48);
        const auto rawTile = carta::makeSpectrogramTile(*tones, 9000, 256, 48);
        check(clipTile.isValid() && processedTile.isValid() && rawTile.isValid(),
              "clip spectrum and reference tiles compute");
        bool changed = false;
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 48; ++x) {
                check(clipTile.getPixelAt(x, y) == processedTile.getPixelAt(x, y),
                      "clip spectrum matches independently enveloped and trimmed source");
                changed |= clipTile.getPixelAt(x, y) != rawTile.getPixelAt(x, y);
            }
        check(changed, "clip gain, fades and trims visibly change spectral data");
        for (int order = 10; order <= 13; ++order) {
            const auto clipView =
                carta::makeClipSpectrogramTile(spectralClip, 10000, 1024, 8, {}, order);
            const auto referenceView =
                carta::makeSpectrogramTile(processed, 10000, 1024, 8, {}, order);
            const auto firstHalf =
                carta::makeClipSpectrogramTile(spectralClip, 10000, 1024, 4, {}, order);
            const auto secondHalf =
                carta::makeClipSpectrogramTile(spectralClip, 14096, 1024, 4, {}, order);
            check(clipView.isValid() && referenceView.isValid() && firstHalf.isValid() &&
                      secondHalf.isValid(),
                  "each selectable FFT size generates clip and source spectrum tiles");
            if (clipView.isValid() && referenceView.isValid() && firstHalf.isValid() &&
                secondHalf.isValid())
                for (int y = 0; y < 192; ++y)
                    for (int x = 0; x < 8; ++x) {
                        check(clipView.getPixelAt(x, y) == referenceView.getPixelAt(x, y),
                              "all FFT sizes preserve clip trim and envelope");
                        check(clipView.getPixelAt(x, y) == (x < 4
                                                                ? firstHalf.getPixelAt(x, y)
                                                                : secondHalf.getPixelAt(x - 4, y)),
                              "all FFT sizes remain continuous at tile boundaries");
                    }
            const auto tone = carta::makeSpectrogramTile(*tones, 15000, 256, 4, {}, order);
            for (int ch = 0; ch < 2; ++ch) {
                int brightest = 0;
                float maximum = -1;
                for (int y = 0; y < 96; ++y) {
                    const auto light = tone.getPixelAt(2, ch * 96 + y).getBrightness();
                    if (light > maximum) {
                        maximum = light;
                        brightest = y;
                    }
                }
                const double frequency = ch == 0 ? 1000.0 : 8000.0;
                const int expected = static_cast<int>(std::round(
                    95.0 * (1.0 - std::log(frequency / 20.0) / std::log(22050.0 / 20.0))));
                check(maximum > 0.5f && std::abs(brightest - expected) <= 5,
                      "selectable FFT sizes place tones near the labelled frequencies");
            }
        }
        check(!carta::makeSpectrogramTile(*tones, 0, 1, 1, {}, 9).isValid() &&
                  !carta::makeSpectrogramTile(*tones, 0, 1, 1, {}, 14).isValid(),
              "unsupported FFT sizes are rejected");
        {
            using Window = carta::SpectrumWindow;
            carta::Audio binTone;
            binTone.sampleRate = 44100;
            binTone.samples.setSize(1, 8192);
            for (int i = 0; i < 8192; ++i)
                binTone.samples.setSample(
                    0, i,
                    static_cast<float>(
                        0.25 * std::sin(2.0 * juce::MathConstants<double>::pi * 64 * i / 2048)));
            int reference = -1;
            std::vector<juce::Image> images;
            for (const auto window : {Window::hann, Window::hamming, Window::blackman}) {
                auto image = carta::makeSpectrogramTile(
                    binTone, 4096, 1, 1, {}, 11, carta::SpectrumBand::custom, 1000, 1700, window);
                check(image.isValid(), "each spectrum window generates a tile");
                int maximum = 0;
                for (int y = 0; y < 96; ++y)
                    maximum = std::max(maximum, static_cast<int>(image.getPixelAt(0, y).getRed()));
                if (reference < 0)
                    reference = maximum;
                check(std::abs(maximum - reference) <= 1,
                      "window coherent-gain calibration preserves steady bin-tone brightness");
                images.push_back(image);
            }
            bool differs = false;
            for (int y = 0; y < 96; ++y)
                differs |= images[0].getPixelAt(0, y) != images[2].getPixelAt(0, y);
            check(differs, "different windows produce distinct spectral leakage");
            check(!carta::makeSpectrogramTile(binTone, 4096, 1, 1, {}, 11,
                                              carta::SpectrumBand::full, 20, 22050,
                                              static_cast<Window>(99))
                       .isValid(),
                  "unknown spectrum windows are rejected");
            for (int i = 0; i < 8192; ++i)
                binTone.samples.setSample(
                    0, i,
                    static_cast<float>(
                        1e-5 * std::sin(2.0 * juce::MathConstants<double>::pi * 64 * i / 2048)));
            const auto shallow =
                carta::makeSpectrogramTile(binTone, 4096, 1, 1, {}, 11, carta::SpectrumBand::custom,
                                           1000, 1700, Window::hann, -80);
            const auto deep =
                carta::makeSpectrogramTile(binTone, 4096, 1, 1, {}, 11, carta::SpectrumBand::custom,
                                           1000, 1700, Window::hann, -120);
            int shallowPeak = 0, deepPeak = 0;
            for (int y = 0; y < 96; ++y) {
                shallowPeak =
                    std::max(shallowPeak, static_cast<int>(shallow.getPixelAt(0, y).getRed()));
                deepPeak = std::max(deepPeak, static_cast<int>(deep.getPixelAt(0, y).getRed()));
            }
            check(shallowPeak == 0 && deepPeak >= 40 && deepPeak <= 45,
                  "deeper spectrum floor reveals a -100 dB tone at the expected normalized "
                  "brightness");
            check(!carta::makeSpectrogramTile(binTone, 4096, 1, 1, {}, 11,
                                              carta::SpectrumBand::full, 20, 22050, Window::hann,
                                              -121)
                          .isValid() &&
                      !carta::makeSpectrogramTile(binTone, 4096, 1, 1, {}, 11,
                                                  carta::SpectrumBand::full, 20, 22050,
                                                  Window::hann, -39)
                           .isValid(),
                  "out-of-range spectrum floors are rejected");
            binTone.samples.clear();
            const auto silent =
                carta::makeSpectrogramTile(binTone, 4096, 1, 1, {}, 11, carta::SpectrumBand::full,
                                           20, 22050, Window::hann, -120);
            for (int y = 0; y < 96; ++y)
                check(silent.getPixelAt(0, y).getRed() == 0,
                      "silence remains black at the deepest floor");
        }
        {
            using Band = carta::SpectrumBand;
            double low = 0, high = 0;
            check(carta::spectrogramFrequencyRange(44100, Band::full, low, high) &&
                      std::abs(low - 20) < 1e-9 && std::abs(high - 22050) < 1e-9,
                  "full spectrogram covers 20 Hz to Nyquist");
            check(carta::spectrogramFrequencyRange(44100, Band::bass, low, high) &&
                      std::abs(low - 20) < 1e-9 && std::abs(high - 1000) < 1e-9,
                  "bass spectrogram exposes the requested range");
            check(carta::spectrogramFrequencyRange(44100, Band::voice, low, high) &&
                      std::abs(low - 100) < 1e-9 && std::abs(high - 8000) < 1e-9,
                  "voice spectrogram exposes the requested range");
            check(carta::spectrogramFrequencyRange(8000, Band::voice, low, high) &&
                      std::abs(low - 100) < 1e-9 && std::abs(high - 4000) < 1e-9,
                  "frequency presets clamp to source Nyquist");
            check(carta::spectrogramFrequencyRange(1000, Band::treble, low, high) &&
                      std::abs(low - 250) < 1e-9 && std::abs(high - 500) < 1e-9,
                  "treble display remains meaningful below the nominal lower edge");
            const auto oldLow = low, oldHigh = high;
            check(!carta::spectrogramFrequencyRange(0, Band::voice, low, high) &&
                      !carta::spectrogramFrequencyRange(44100, static_cast<Band>(99), low, high) &&
                      std::memcmp(&low, &oldLow, sizeof(double)) == 0 &&
                      std::memcmp(&high, &oldHigh, sizeof(double)) == 0,
                  "invalid frequency settings preserve bounds");
            for (const auto& range :
                 {std::pair<double, double>{20, 22050}, {400, 500}, {1, 192000}}) {
                const auto ticks = carta::spectrogramFrequencyTicks(range.first, range.second, 180);
                check(ticks.size() >= 3 &&
                          std::abs(ticks.front().frequency - range.second) < 1e-9 &&
                          std::abs(ticks.back().frequency - range.first) < 1e-9,
                      "frequency ticks include endpoints and intermediate labels");
                for (size_t i = 0; i < ticks.size(); ++i) {
                    const auto expected = (std::log(range.second) - std::log(ticks[i].frequency)) /
                                          (std::log(range.second) - std::log(range.first));
                    check(std::abs(ticks[i].position - expected) < 1e-9 &&
                              !carta::formatSpectrumFrequency(ticks[i].frequency).isEmpty(),
                          "frequency ticks align with the logarithmic spectrogram scale");
                    if (i > 0) {
                        const auto labelTop = [](double position) {
                            return std::clamp(static_cast<int>(std::lround(position * 179)) - 6, 0,
                                              168);
                        };
                        check(ticks[i].frequency < ticks[i - 1].frequency &&
                                  labelTop(ticks[i].position) - labelTop(ticks[i - 1].position) >=
                                      12,
                              "clamped frequency labels do not overlap");
                    }
                }
            }
            check(carta::spectrogramFrequencyTicks(400, 500, 8).size() == 1 &&
                      carta::spectrogramFrequencyTicks(500, 400, 100).empty() &&
                      carta::spectrogramFrequencyTicks(0, 100, 100).empty(),
                  "frequency ticks handle short lanes and invalid bounds");
            check(carta::formatSpectrumFrequency(22050) == "22.05k" &&
                      carta::formatSpectrumFrequency(1000) == "1k" &&
                      carta::formatSpectrumFrequency(455) == "455" &&
                      carta::formatSpectrumFrequency(0).isEmpty(),
                  "frequency labels retain useful precision and kHz units");
            for (const auto band : {Band::full, Band::bass, Band::voice, Band::treble}) {
                check(carta::spectrogramFrequencyRange(tones->sampleRate, band, low, high),
                      "get spectrum frequency bounds");
                const auto image = carta::makeSpectrogramTile(*tones, 15000, 256, 4, {}, 11, band);
                check(image.isValid(), "generate each spectrogram frequency range");
                if (!image.isValid())
                    continue;
                for (int ch = 0; ch < 2; ++ch) {
                    const double hz = ch == 0 ? 1000.0 : 8000.0;
                    if (hz < low || hz > high)
                        continue;
                    int brightest = 0;
                    float maximum = -1;
                    for (int y = 0; y < 96; ++y) {
                        const auto brightness = image.getPixelAt(2, ch * 96 + y).getBrightness();
                        if (brightness > maximum) {
                            maximum = brightness;
                            brightest = y;
                        }
                    }
                    const int expected = static_cast<int>(
                        std::round(95.0 * (1.0 - std::log(hz / low) / std::log(high / low))));
                    check(maximum > 0.5f && std::abs(brightest - expected) <= 5,
                          "frequency range and tone display share the gutter scale");
                }
            }
        }
        {
            auto lowRate = std::make_shared<carta::Audio>();
            lowRate->sampleRate = 8000;
            lowRate->samples.setSize(1, 8000);
            auto highRate = std::make_shared<carta::Audio>();
            highRate->sampleRate = 48000;
            highRate->samples.setSize(1, 48000);
            for (int i = 0; i < 8000; ++i)
                lowRate->samples.setSample(
                    0, i,
                    static_cast<float>(
                        0.8 * std::sin(2.0 * juce::MathConstants<double>::pi * 1000 * i / 8000)));
            for (int i = 0; i < 48000; ++i)
                highRate->samples.setSample(
                    0, i,
                    static_cast<float>(
                        0.8 * std::sin(2.0 * juce::MathConstants<double>::pi * 1000 * i / 48000)));
            carta::Clip lowClip{lowRate, 0, 0, 1}, highClip{highRate, 1, 0, 1};
            carta::Track track{"Mixed", {lowClip, highClip}};
            const double displayRate = carta::spectrogramTrackRate(track);
            check(std::equal_to<double>{}(displayRate, 48000),
                  "mixed-rate track uses highest source Nyquist for its spectrum scale");
            for (auto band : {carta::SpectrumBand::full, carta::SpectrumBand::bass,
                              carta::SpectrumBand::voice, carta::SpectrumBand::treble}) {
                const auto lowImage = carta::makeClipSpectrogramTile(lowClip, 4000, 256, 1, {}, 11,
                                                                     band, displayRate);
                const auto highImage = carta::makeClipSpectrogramTile(highClip, 24000, 256, 1, {},
                                                                      11, band, displayRate);
                check(lowImage.isValid() && highImage.isValid(),
                      "mixed-rate sources render on common scale");
                double low = 0, high = 0;
                check(carta::spectrogramFrequencyRange(displayRate, band, low, high),
                      "get common frequency bounds");
                int lowBrightest = 0, highBrightest = 0;
                float lowMaximum = -1, highMaximum = -1;
                for (int y = 0; y < 96; ++y) {
                    const float l = lowImage.getPixelAt(0, y).getBrightness(),
                                h = highImage.getPixelAt(0, y).getBrightness();
                    if (l > lowMaximum) {
                        lowMaximum = l;
                        lowBrightest = y;
                    }
                    if (h > highMaximum) {
                        highMaximum = h;
                        highBrightest = y;
                    }
                    const double lower =
                        low * std::pow(high / low, juce::jlimit(0.0, 1.0, (95.0 - y - 0.5) / 95.0));
                    if (lower > 4000)
                        check(std::equal_to<float>{}(l, 0),
                              "source frequencies above Nyquist are blank on the shared scale");
                }
                const int expected = static_cast<int>(
                    std::round(95.0 * (1.0 - std::log(1000.0 / low) / std::log(high / low))));
                check(lowMaximum > 0.5f && highMaximum > 0.5f &&
                          std::abs(lowBrightest - expected) <= 5 &&
                          std::abs(highBrightest - expected) <= 5,
                      "equal tones at different sample rates align with the shared gutter");
            }
            double customLow = 0, customHigh = 0;
            check(carta::spectrogramFrequencyRange(8000, carta::SpectrumBand::custom, customLow,
                                                   customHigh, 100, 12000) &&
                      std::abs(customLow - 100) < 1e-9 && std::abs(customHigh - 12000) < 1e-9,
                  "custom spectrum bounds remain fixed above source Nyquist");
            const auto lowImage = carta::makeClipSpectrogramTile(lowClip, 4000, 256, 1, {}, 11,
                                                                 carta::SpectrumBand::custom,
                                                                 displayRate, 100, 12000);
            const auto highImage = carta::makeClipSpectrogramTile(highClip, 24000, 256, 1, {}, 11,
                                                                  carta::SpectrumBand::custom,
                                                                  displayRate, 100, 12000);
            check(lowImage.isValid() && highImage.isValid(),
                  "custom mixed-rate spectrum tiles render");
            int lowBrightest = 0, highBrightest = 0;
            float lowPeak = -1, highPeak = -1;
            for (int y = 0; y < 96; ++y) {
                const float l = lowImage.getPixelAt(0, y).getBrightness(),
                            h = highImage.getPixelAt(0, y).getBrightness();
                if (l > lowPeak) {
                    lowPeak = l;
                    lowBrightest = y;
                }
                if (h > highPeak) {
                    highPeak = h;
                    highBrightest = y;
                }
                const double lower =
                    100.0 * std::pow(120.0, juce::jlimit(0.0, 1.0, (94.5 - y) / 95.0));
                if (lower > 4000)
                    check(std::equal_to<float>{}(l, 0),
                          "custom spectrum remains blank above each source Nyquist");
            }
            const int expected =
                static_cast<int>(std::round(95.0 * (1.0 - std::log(10.0) / std::log(120.0))));
            check(lowPeak > 0.5f && highPeak > 0.5f && std::abs(lowBrightest - expected) <= 5 &&
                      std::abs(highBrightest - expected) <= 5,
                  "custom spectrum aligns tones across sample rates");
            const auto beforeLow = customLow, beforeHigh = customHigh;
            check(!carta::spectrogramFrequencyRange(44100, carta::SpectrumBand::custom, customLow,
                                                    customHigh, 0, 12000) &&
                      !carta::spectrogramFrequencyRange(44100, carta::SpectrumBand::custom,
                                                        customLow, customHigh, 100, 100) &&
                      !carta::spectrogramFrequencyRange(44100, carta::SpectrumBand::custom,
                                                        customLow, customHigh, 100, 192001) &&
                      std::equal_to<double>{}(customLow, beforeLow) &&
                      std::equal_to<double>{}(customHigh, beforeHigh),
                  "invalid custom spectrum bounds are rejected without changing outputs");
        }
        check(!carta::makeClipSpectrogramTile(spectralClip, 9000, 256, 48, [] { return true; })
                   .isValid(),
              "clip spectral analysis supports cancellation");
        auto invalidClip = spectralClip;
        invalidClip.gain = std::numeric_limits<float>::quiet_NaN();
        check(!carta::makeClipSpectrogramTile(invalidClip, 9000, 256, 48).isValid(),
              "invalid spectral envelope is rejected");
        int checks = 0;
        check(!carta::makeSpectrogramTile(*audio, 0, 8192, 128, [&] { return ++checks > 5; })
                   .isValid(),
              "spectrogram tile cancellation drops partial analysis");
        check(!carta::makeSpectrogramTile(*audio, 0, 0, 10).isValid() &&
                  !carta::makeSpectrogramTile(*audio, audio->numSamples(), 1, 1).isValid(),
              "invalid spectral tile geometry rejected");
    }
    {
        const double sample = 1.0 / carta::sessionRate;
        const auto raw = carta::snapTimelinePosition(0.234567, 0, 1, false, 0, {});
        check(std::abs(raw.time - 0.234567) < 1e-12 && !raw.hasGuide,
              "disabled snapping leaves pointer position unchanged");
        const auto grid = carta::snapTimelinePosition(100.4 * sample, 0, 1, true, 0, {});
        check(std::abs(grid.time / sample - 100) < 1e-9 && !grid.hasGuide,
              "sample grid rounds dragged edge");
        const auto target = carta::snapTimelinePosition(0.501, 0, 1, true, 0.01, {0.5, 0.8});
        check(target.hasGuide && std::abs(target.time - 0.5) < 1e-12 &&
                  std::abs(target.guide - 0.5) < 1e-12,
              "nearby boundary wins over sample grid");
        const auto end = carta::snapTimelinePosition(0.301, 0, 1, true, 0.01, {0.5}, 0.2);
        check(end.hasGuide && std::abs(end.time - 0.3) < 1e-12 && std::abs(end.guide - 0.5) < 1e-12,
              "moving clip snaps trailing edge without changing duration");
        const auto outside = carta::snapTimelinePosition(0.19, 0.2, 0.9, true, 0.1, {0.1});
        check(!outside.hasGuide && outside.time >= 0.2,
              "invalid boundary cannot pull trim outside its bounds");
        const auto fallback = carta::snapTimelinePosition(0.7, 0, 1, true, 0.01, {0.2});
        check(!fallback.hasGuide && std::abs(fallback.time * carta::sessionRate -
                                             std::round(0.7 * carta::sessionRate)) < 1e-8,
              "distant boundary falls back to sample grid");
        const auto noGrid =
            carta::snapTimelinePosition(0.5 * sample, 0.25 * sample, 0.75 * sample, true, 0, {});
        check(noGrid.time >= 0.25 * sample && noGrid.time <= 0.75 * sample,
              "subsample bounds without a grid point stay valid");
        const auto nearest =
            carta::snapTimelinePosition(0.501, 0, 1, false, 0.01, {0.494, 0.502, 0.505});
        {
            const auto tenth = carta::snapTimelinePosition(0.16, 0, 2, true, 0, {}, 0, 4410);
            const auto second = carta::snapTimelinePosition(1.6, 0, 3, true, 0, {}, 0, 44100);
            check(std::abs(tenth.time - 0.2) < 1e-12 && tenth.hasGuide &&
                      std::abs(second.time - 2) < 1e-12 && second.hasGuide,
                  "time grids snap to exact project-sample-aligned seconds");
            const auto boundary =
                carta::snapTimelinePosition(0.148, 0, 2, true, 0.01, {0.15}, 0, 4410);
            check(boundary.hasGuide && std::abs(boundary.time - 0.15) < 1e-12,
                  "enabled boundary snapping takes precedence over time grid");
            const auto noTick = carta::snapTimelinePosition(0.25, 0.21, 0.29, true, 0, {}, 0, 4410);
            check(std::abs(noTick.time - 0.25) < 1e-12 && !noTick.hasGuide,
                  "bounded trim without an available grid tick remains valid");
            const auto bypass = carta::snapTimelinePosition(0.16, 0, 2, false, 0, {}, 0, 4410);
            check(std::abs(bypass.time - 0.16) < 1e-12 && !bypass.hasGuide,
                  "disabling grid snapping preserves pointer position");
            juce::int64 delta = 0;
            check(carta::snapClipGroupToGrid(1102.25 / carta::sessionRate, 4300, 4410, delta) &&
                      delta == 3308 &&
                      std::abs((1102.25 + static_cast<double>(delta)) - 4410.25) < 1e-9,
                  "group grid snaps anchor using a shared integer-sample delta while preserving "
                  "fractional timing");
            delta = 42;
            check(!carta::snapClipGroupToGrid(1, 0, 0, delta) &&
                      !carta::snapClipGroupToGrid(0, -1, 4410, delta) && delta == 42,
                  "invalid group grid snaps preserve the output delta");
        }
        check(nearest.hasGuide && std::abs(nearest.guide - 0.502) < 1e-12,
              "nearest eligible target selected deterministically");
    }
    {
        const double rate = carta::sessionRate;
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = rate;
        source->samples.setSize(1, 1000);
        for (int i = 0; i < 1000; ++i)
            source->samples.setSample(0, i, static_cast<float>(i + 1) / 2000);
        carta::Session original;
        original.tracks.push_back(
            {"one", {{source, 0, 0, 1000 / rate}, {source, 200 / rate, 0, 500 / rate}}});
        original.tracks.push_back({"two", {{source, 0, 0, 1000 / rate}}});
        original.markers = {{100 / rate, "before"}, {400 / rate, "at"}, {800 / rate, "after"}};
        const carta::TimeSelection range{400, 500, 0, 0};
        juce::String error;
        auto local = original;
        check(carta::insertGap(local, range, false, error) && local.tracks[0].clips.size() == 4 &&
                  local.tracks[1].clips.size() == 1,
              "insert gap splits all crossing overlaps only on selected tracks");
        check(std::abs(local.markers[1].position * rate - 400) < 1e-7 &&
                  carta::projectEndSample(local) == 1100,
              "selected-track insertion leaves markers in place");
        carta::EditorState state;
        state.session = original;
        auto before = state;
        carta::EditHistory history;
        check(carta::insertGap(state.session, range, true, error) &&
                  state.session.tracks[1].clips.size() == 2,
              "insert gap on all tracks");
        check(std::abs(state.session.markers[0].position * rate - 100) < 1e-7 &&
                  std::abs(state.session.markers[1].position * rate - 500) < 1e-7 &&
                  std::abs(state.session.markers[2].position * rate - 900) < 1e-7,
              "all-track insertion shifts markers at and after the boundary");
        juce::AudioBuffer<float> a(2, 1000), b(2, 1100);
        a.clear();
        b.clear();
        original.render(a, 0, 1000, 0, rate, 2);
        state.session.render(b, 0, 1100, 0, rate, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 1100; ++i)
                check(near(b.getSample(ch, i), i < 400   ? a.getSample(ch, i)
                                               : i < 500 ? 0
                                                         : a.getSample(ch, i - 100)),
                      "insert gap preserves audio around silent sample range");
        history.record(before);
        check(history.undo(state) && carta::projectEndSample(state.session) == 1000 &&
                  history.redo(state) && carta::projectEndSample(state.session) == 1100,
              "gap insertion is undoable and redoable");
        auto bad = original;
        bad.markers.push_back({400 / rate, "duplicate"});
        check(!carta::insertGap(bad, range, true, error) && bad.tracks[0].clips.size() == 2 &&
                  carta::projectEndSample(bad) == 1000 && error.isNotEmpty(),
              "all-track insertion rejects duplicate markers atomically");
        auto offGrid = original;
        offGrid.markers[2].position = 800.25 / rate;
        const double fractionalMarker = offGrid.markers[2].position;
        check(!carta::insertGap(offGrid, range, true, error) && error.contains("off-grid") &&
                  offGrid.tracks[0].clips.size() == 2 &&
                  std::memcmp(&offGrid.markers[2].position, &fractionalMarker, sizeof(double)) == 0,
              "all-track insertion rejects fractional markers without modifying audio or markers");
        carta::Clipboard copied;
        copied.lengthSamples = 100;
        copied.tracks = {{{source, 0, 0, 100 / rate}}};
        check(!carta::pasteRangeInserting(offGrid, copied, 0, 400, true, error) &&
                  error.contains("off-grid") && offGrid.tracks[0].clips.size() == 2 &&
                  std::memcmp(&offGrid.markers[2].position, &fractionalMarker, sizeof(double)) == 0,
              "all-track insert paste also rejects fractional markers atomically");
        check(carta::insertGap(offGrid, range, false, error) &&
                  std::memcmp(&offGrid.markers[2].position, &fractionalMarker, sizeof(double)) == 0,
              "destination-only insertion preserves fractional markers without moving them");
        auto noAudio = original;
        check(!carta::insertGap(noAudio, {1200, 1300, 0, 0}, false, error) &&
                  carta::projectEndSample(noAudio) == 1000,
              "insertion beyond audio is a no-op");
    }
    {
        const double rate = carta::sessionRate;
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = rate;
        source->samples.setSize(1, 1000);
        carta::Session original;
        original.tracks.push_back({"master", {{source, 0, 0, 1000 / rate}}});
        original.markers = {{500 / rate, "middle"}, {900 / rate, "end"}, {1200 / rate, "legacy"}};
        juce::String error;
        auto shortened = original;
        shortened.tracks[0].clips[0].duration = 900 / rate;
        check(!carta::preservesMarkerBounds(original, shortened, error) && error.isNotEmpty(),
              "shortening rejects a formerly valid marker at the exclusive end");
        shortened = original;
        shortened.tracks[0].clips[0].duration = 950 / rate;
        error.clear();
        check(carta::preservesMarkerBounds(original, shortened, error),
              "an existing out-of-range marker does not block a safe edit");
        auto moved = shortened;
        moved.markers[1].position = 800 / rate;
        moved.tracks[0].clips[0].duration = 850 / rate;
        check(carta::preservesMarkerBounds(original, moved, error),
              "moving a marker alongside audio does not reject its old position");
        auto edgeSession = original;
        edgeSession.tracks.push_back(
            {"fractional", {{source, 10.25 / rate, 10 / rate, 500 / rate}}});
        const std::vector<carta::ClipReference> edgeRefs{{1, 0}, {0, 0}};
        carta::ClipGroupEdgePreview edgePlan;
        check(edgePlan.prepare(edgeSession, edgeRefs, error), "prepare group edge geometry once");
        for (bool endEdge : {false, true})
            for (int delta : {-100, -1, 1, 100, 1000}) {
                std::vector<carta::Clip> expected, actual;
                juce::String expectedError, actualError;
                const bool expectedOk = carta::previewClipGroupEdges(
                    edgeSession, edgeRefs, endEdge, delta, expected, expectedError);
                const bool actualOk = edgePlan.calculate(endEdge, delta, actual, actualError);
                check(actualOk == expectedOk && actualError == expectedError,
                      "prepared edge preview agrees with complete validation");
                if (actualOk && expectedOk)
                    for (size_t i = 0; i < actual.size(); ++i) {
                        check(std::memcmp(&actual[i].start, &expected[i].start, sizeof(double)) ==
                                      0 &&
                                  std::memcmp(&actual[i].offset, &expected[i].offset,
                                              sizeof(double)) == 0 &&
                                  std::memcmp(&actual[i].duration, &expected[i].duration,
                                              sizeof(double)) == 0,
                              "prepared edge preview preserves exact on-grid and fractional "
                              "geometry");
                    }
            }
        std::vector<carta::Clip> preserved{edgeSession.tracks[0].clips[0]};
        check(!edgePlan.calculate(false, -100, preserved, error) && preserved.size() == 1 &&
                  std::abs(preserved[0].start) < 1e-12,
              "invalid prepared edge preview leaves the caller's output untouched");
        edgePlan.clear();
        check(!edgePlan.calculate(false, 1, preserved, error),
              "cleared edge preview cannot reuse stale clip geometry");
        for (int minimum : {0, 920})
            for (int otherEnd : {0, 925}) {
                auto previewSession = original;
                previewSession.minimumEndSample = minimum;
                if (otherEnd)
                    previewSession.tracks.push_back({"other", {{source, 0, 0, otherEnd / rate}}});
                carta::ClipGroupEdgeMarkerPreview preview;
                check(preview.prepare(previewSession, {{0, 0}}, error),
                      "prepare marker-safe group edge preview");
                for (int delta : {-1, -99, -100, -101, -300}) {
                    std::vector<carta::Clip> adjusted;
                    auto candidate = previewSession;
                    juce::String expectedError, actualError;
                    check(carta::previewClipGroupEdges(previewSession, {{0, 0}}, true, delta,
                                                       adjusted, actualError),
                          "prepare valid group trim geometry");
                    candidate.tracks[0].clips[0] = adjusted[0];
                    const bool expected =
                        carta::preservesMarkerBounds(previewSession, candidate, expectedError);
                    check(preview.validate(adjusted, actualError) == expected,
                          "prepared group marker check agrees with full commit validation, "
                          "including retained invalid markers and master floor");
                }
            }
        auto edit = original;
        check(carta::removeRange(edit, {900, 1000, 0, 0}, &error) &&
                  carta::projectEndSample(edit) == 1000 && edit.minimumEndSample == 1000 &&
                  std::abs(edit.markers[1].position * rate - 900) < 1e-8,
              "range removal leaves a silent tail and preserves the marker at its start");
        edit = original;
        check(carta::removeRange(edit, {950, 1000, 0, 0}, &error) &&
                  carta::projectEndSample(edit) == 1000,
              "range removal keeps the former master end even without affected markers");
        edit = original;
        edit.tracks.push_back({"other", {{source, 0, 0, 1000 / rate}}});
        check(carta::removeRange(edit, {900, 1000, 0, 0}, &error) &&
                  carta::projectEndSample(edit) == 1000,
              "another track can preserve master duration");
    }
    {
        const double rate = carta::sessionRate;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = rate;
        audio->samples.setSize(1, 100);
        for (int i = 0; i < 100; ++i)
            audio->samples.setSample(0, i, static_cast<float>(i) / 100.0f);
        carta::Clip clip{audio, 100 / rate, 20 / rate, 60 / rate};
        juce::String error;
        clip.gain = 0.75f;
        clip.fadeIn = 10 / rate;
        clip.fadeInStart = 20 / rate;
        check(carta::trimClipToSamples(clip, 110, 150, error),
              "trim clip to exact project samples");
        check(std::abs(clip.start * rate - 110) < 1e-9 &&
                  std::abs(clip.offset * rate - 30) < 1e-9 &&
                  std::abs(clip.duration * rate - 40) < 1e-9,
              "trim preserves source alignment and exclusive end");
        check(near(clip.gain, 0.75f) && std::abs(clip.fadeInStart * rate - 20) < 1e-9,
              "trim preserves source anchored envelope");
        check(carta::trimClipToSamples(clip, 80, 160, error), "untrim back into source");
        check(std::abs(clip.offset * rate) < 1e-8 && std::abs(clip.duration * rate - 80) < 1e-8,
              "untrim restores source start and duration");
        const auto before = clip;
        check(!carta::trimClipToSamples(clip, 79, 160, error) &&
                  !carta::trimClipToSamples(clip, 80, 181, error) &&
                  !carta::trimClipToSamples(clip, 100, 100, error),
              "reject source overruns and empty range");
        check(std::memcmp(&before.start, &clip.start, sizeof(double)) == 0 &&
                  std::memcmp(&before.offset, &clip.offset, sizeof(double)) == 0 &&
                  std::memcmp(&before.duration, &clip.duration, sizeof(double)) == 0,
              "invalid trim leaves clip untouched");
        check(carta::trimClipToSamples(clip, 100, 101, error) &&
                  std::abs(clip.duration * rate - 1) < 1e-9,
              "allow one sample clip");
        check(carta::moveClipToSample(clip, 44101, error) &&
                  std::abs(clip.start * rate - 44101) < 1e-9 &&
                  std::abs(clip.offset * rate - 20) < 1e-8,
              "move clip to exact sample without changing source");
        check(!carta::moveClipToSample(clip, -1, error) &&
                  !carta::moveClipToSample(clip, 9007199254740992LL, error),
              "reject invalid and imprecise positions");
        auto longClip = clip;
        longClip.duration = 80 / rate;
        const auto oldStart = longClip.start;
        check(!carta::moveClipToSample(longClip, 9007199254740990LL, error) &&
                  std::abs(longClip.start - oldStart) < 1e-12,
              "move rejects an end beyond the precise timeline without mutation");
        carta::Clip fractional{audio, 10 / rate, 20 / rate, 10.5 / rate};
        fractional.gain = 0.7f;
        fractional.fadeIn = 5 / rate;
        const auto originalFractional = fractional;
        check(carta::alignClipEdgeToSample(fractional, true, 100, error) &&
                  std::abs((fractional.start + fractional.duration) * rate - 100) < 1e-8 &&
                  std::abs(fractional.start * rate - 89.5) < 1e-8,
              "end alignment preserves fractional clip duration");
        check(std::memcmp(&fractional.offset, &originalFractional.offset, sizeof(double)) == 0 &&
                  std::memcmp(&fractional.duration, &originalFractional.duration, sizeof(double)) ==
                      0 &&
                  near(fractional.gain, originalFractional.gain) &&
                  std::memcmp(&fractional.fadeIn, &originalFractional.fadeIn, sizeof(double)) == 0,
              "alignment retains source and envelope");
        const auto savedStart = fractional.start;
        check(!carta::alignClipEdgeToSample(fractional, true, 10, error) &&
                  std::memcmp(&fractional.start, &savedStart, sizeof(double)) == 0,
              "end alignment before timeline start is rejected atomically");
        check(carta::alignClipEdgeToSample(fractional, false, 42, error) &&
                  std::abs(fractional.start * rate - 42) < 1e-9,
              "start alignment lands on the selected sample");
        check(!carta::alignClipEdgeToSample(fractional, false, -1, error) &&
                  !carta::alignClipEdgeToSample(fractional, false, 9007199254740991LL, error),
              "alignment rejects invalid and overflowing positions");
        {
            carta::Session edges;
            edges.tracks.push_back(
                {"source",
                 {{audio, 100 / rate, 0, 20 / rate}, {audio, 70 / rate, 0, 10.25 / rate}}});
            edges.tracks.push_back(
                {"other", {{audio, 135.5 / rate, 0, 10 / rate}, {audio, 50 / rate, 0, 10 / rate}}});
            double target = -1;
            const double previous =
                edges.tracks[0].clips[1].start + edges.tracks[0].clips[1].duration;
            check(carta::adjacentClipEdge(edges, {0, 0}, false, false, target) &&
                      std::memcmp(&target, &previous, sizeof(double)) == 0,
                  "previous edge finds nearest other clip end");
            check(carta::adjacentClipEdge(edges, {0, 0}, true, true, target) &&
                      std::memcmp(&target, &edges.tracks[1].clips[0].start, sizeof(double)) == 0,
                  "next edge finds fractional boundary on another track");
            auto moving = edges.tracks[0].clips[0];
            check(carta::alignClipEdgeToTime(moving, true, target, error) &&
                      std::abs((moving.start + moving.duration - target) * rate) < 1e-8 &&
                      std::memcmp(&moving.offset, &edges.tracks[0].clips[0].offset,
                                  sizeof(double)) == 0 &&
                      std::memcmp(&moving.duration, &edges.tracks[0].clips[0].duration,
                                  sizeof(double)) == 0,
                  "alignment to another track preserves fractional edge and source geometry");
            check(!carta::adjacentClipEdge(edges, {1, 1}, false, false, target) &&
                      !carta::adjacentClipEdge(edges, {9, 0}, false, true, target),
                  "missing and invalid neighbor searches fail");
            const auto beforeMoving = moving;
            check(!carta::alignClipEdgeToTime(moving, true, 1 / rate, error) &&
                      !carta::alignClipEdgeToTime(
                          moving, false, std::numeric_limits<double>::quiet_NaN(), error) &&
                      std::memcmp(&moving.start, &beforeMoving.start, sizeof(double)) == 0,
                  "invalid exact alignment leaves clip untouched");
            edges.tracks[1].clips.push_back({audio, 150.25 / rate, 0, 10 / rate});
            const std::vector<carta::ClipReference> group{{0, 0}, {1, 0}};
            check(carta::adjacentClipEdgeExcluding(edges, {0, 0}, false, true, group, target) &&
                      std::abs(target * rate - 150.25) < 1e-8,
                  "group alignment skips the other selected group's edges");
            auto alignedGroup = edges;
            check(carta::alignClipGroupEdgeToTime(alignedGroup, group, {0, 0}, false, target,
                                                  error) &&
                      std::abs(alignedGroup.tracks[0].clips[0].start * rate - 150.25) < 1e-8 &&
                      std::abs((alignedGroup.tracks[1].clips[0].start -
                                alignedGroup.tracks[0].clips[0].start) *
                                   rate -
                               35.5) < 1e-8 &&
                      std::memcmp(&alignedGroup.tracks[1].clips[0].offset,
                                  &edges.tracks[1].clips[0].offset, sizeof(double)) == 0,
                  "fractional alignment translates every group member while preserving relative "
                  "timing and source offset");
            auto rejectedGroup = edges;
            const std::vector<carta::ClipReference> negative{{0, 0}, {0, 1}};
            check(!carta::alignClipGroupEdgeToTime(rejectedGroup, negative, {0, 0}, false,
                                                   10 / rate, error) &&
                      std::memcmp(&rejectedGroup.tracks[0].clips[0].start,
                                  &edges.tracks[0].clips[0].start, sizeof(double)) == 0 &&
                      std::memcmp(&rejectedGroup.tracks[0].clips[1].start,
                                  &edges.tracks[0].clips[1].start, sizeof(double)) == 0,
                  "alignment that would move another member before zero is atomic");
            carta::Session markerGroup;
            markerGroup.tracks.push_back({"A", {{audio, 100 / rate, 0, 20 / rate}}});
            markerGroup.tracks.push_back({"B", {{audio, 110 / rate, 0, 20 / rate}}});
            markerGroup.markers.push_back({129 / rate, "End"});
            check(!carta::alignClipGroupEdgeToTime(markerGroup, {{0, 0}, {1, 0}}, {0, 0}, false, 0,
                                                   error) &&
                      std::abs(markerGroup.tracks[0].clips[0].start * rate - 100) < 1e-8,
                  "group alignment cannot leave a marker beyond the master");
        }
        const double fractionalEnd = fractional.start + fractional.duration;
        check(carta::nudgeClipEdge(fractional, false, 1, error) &&
                  std::abs((fractional.start + fractional.duration - fractionalEnd) * rate) <
                      1e-7 &&
                  std::abs(fractional.start * rate - 43) < 1e-8,
              "one-sample start trim preserves a fractional opposite edge");
        carta::Clip remote{audio, 9007199254740000.0 / rate, 20 / rate, 60 / rate};
        check(!carta::nudgeClipEdge(remote, false, 1, error),
              "one-sample trim rejects positions without sufficient floating-point precision");
        carta::Clip edge{audio, 30 / rate, 20 / rate, 60 / rate};
        edge.gain = 0.75f;
        edge.fadeIn = 10 / rate;
        edge.fadeInStart = 20 / rate;
        check(carta::nudgeClipEdge(edge, false, 1, error) &&
                  std::abs(edge.start * rate - 31) < 1e-8 &&
                  std::abs(edge.offset * rate - 21) < 1e-8 &&
                  std::abs((edge.start + edge.duration) * rate - 90) < 1e-8,
              "trimming the start by one sample preserves timeline end and source alignment");
        check(carta::nudgeClipEdge(edge, false, -1, error) &&
                  std::abs(edge.start * rate - 30) < 1e-8 &&
                  std::abs(edge.offset * rate - 20) < 1e-8 &&
                  std::abs(edge.duration * rate - 60) < 1e-8 && near(edge.gain, 0.75f) &&
                  std::abs(edge.fadeInStart * rate - 20) < 1e-8,
              "untrimming the start restores geometry and leaves the envelope anchored");
        check(carta::nudgeClipEdge(edge, true, 1, error) &&
                  std::abs((edge.start + edge.duration) * rate - 91) < 1e-8 &&
                  std::abs(edge.offset * rate - 20) < 1e-8,
              "untrimming the end keeps start and source offset");
        carta::Clip atSourceEnd{audio, 10 / rate, 40 / rate, 60 / rate};
        const auto originalEnd = atSourceEnd.duration;
        check(!carta::nudgeClipEdge(atSourceEnd, true, 1, error) &&
                  std::memcmp(&atSourceEnd.duration, &originalEnd, sizeof(double)) == 0,
              "end untrim cannot exceed source");
        carta::Clip atSourceStart{audio, 10 / rate, 0, 20 / rate};
        const auto originalStart = atSourceStart.start;
        check(!carta::nudgeClipEdge(atSourceStart, false, -1, error) &&
                  std::memcmp(&atSourceStart.start, &originalStart, sizeof(double)) == 0,
              "start untrim cannot exceed source");
        carta::Clip oneSample{audio, 10 / rate, 0, 1 / rate};
        check(!carta::nudgeClipEdge(oneSample, true, -1, error),
              "keyboard trim cannot remove the entire clip");
        carta::EditorState edgeState;
        edgeState.session.tracks.push_back({"test", {{audio, 30 / rate, 20 / rate, 60 / rate}}});
        edgeState.session.markers = {{89 / rate, "Last"}};
        const auto edgeBefore = edgeState;
        carta::EditHistory edgeHistory;
        check(carta::nudgeClipEdge(edgeState.session.tracks[0].clips[0], true, -1, error) &&
                  !carta::preservesMarkerBounds(edgeBefore.session, edgeState.session, error),
              "edge trim can be rejected when it would orphan a marker");
        edgeState = edgeBefore;
        edgeState.session.markers.clear();
        const auto withoutMarker = edgeState;
        check(carta::nudgeClipEdge(edgeState.session.tracks[0].clips[0], true, -1, error),
              "end trim succeeds without orphaned marker");
        edgeHistory.record(withoutMarker);
        check(edgeHistory.undo(edgeState) &&
                  std::abs(edgeState.session.tracks[0].clips[0].duration * rate - 60) < 1e-8 &&
                  edgeHistory.redo(edgeState) &&
                  std::abs(edgeState.session.tracks[0].clips[0].duration * rate - 59) < 1e-8,
              "one-sample trim supports undo and redo");
        clip.fadeIn = 0;
        carta::Session session;
        session.tracks.push_back({"test", {{clip}}});
        juce::AudioBuffer<float> output(2, 1);
        output.clear();
        session.render(output, 0, 1, 44101 / rate, rate, 2);
        check(near(output.getSample(0, 0), audio->samples.getSample(0, 20) * 0.75f),
              "sample-precise clip renders correct source frame");
    }
    {
        const double rate = carta::sessionRate;
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = rate;
        source->samples.setSize(1, 1000);
        carta::Session edit;
        edit.tracks.push_back({"A", {}});
        edit.tracks.push_back({"B", {}});
        carta::Clipboard copied;
        copied.lengthSamples = 200;
        copied.tracks = {{{source, 0, 0, 20 / rate}}, {{source, 0, 0, 200 / rate}}};
        juce::String error;
        const juce::int64 nearEnd = 9007199254740991LL - 100;
        check(!carta::pasteRange(edit, copied, 0, nearEnd, &error) && error.isNotEmpty() &&
                  edit.tracks[0].clips.empty() && edit.tracks[1].clips.empty(),
              "paste preflights every track before mutating any");
        check(carta::pasteRange(edit, copied, 0, 10, &error) && edit.tracks[0].clips.size() == 1 &&
                  edit.tracks[1].clips.size() == 1,
              "valid multitrack paste still succeeds");
        int index = 0;
        check(carta::duplicateClipAfter(edit, 0, index, error) && index == 1 &&
                  edit.tracks[0].clips.size() == 2,
              "valid clip duplication succeeds");
        auto nearLimit = edit;
        nearLimit.tracks[0].clips[0].start = static_cast<double>(nearEnd) / rate;
        nearLimit.tracks[0].clips[0].duration = 80 / rate;
        index = 0;
        check(carta::validTimelineGeometry(nearLimit.tracks[0].clips[0].start, 0,
                                           nearLimit.tracks[0].clips[0].duration),
              "near-limit duplication fixture starts valid");
        check(!carta::duplicateClipAfter(nearLimit, 0, index, error) && index == 0 &&
                  nearLimit.tracks[0].clips.size() == 2 && error.isNotEmpty(),
              "duplication refuses an invalid endpoint atomically");
        check(!carta::validTimelineGeometry(1e12, 0, 1 / rate) &&
                  !carta::validTimelineGeometry(0, 0, 0),
              "geometry validation rejects unrepresentable and empty clips");
    }
    {
        carta::EditHistory named;
        carta::EditorState state;
        state.session.tracks.push_back({"before", {}});
        named.record(state, "Move clip");
        state.session.tracks[0].name = "moved";
        const auto movedRevision = named.revision();
        named.record(state, "Add marker");
        state.session.markers.push_back({0, "marker"});
        check(named.undoAction() == "Add marker" && named.redoAction().isEmpty(),
              "history exposes the latest action name");
        const auto undoTargets = named.targets(false);
        check(undoTargets.size() == 2 && undoTargets[0].action == "Add marker" &&
                  undoTargets[0].revision == movedRevision &&
                  undoTargets[1].action == "Move clip" && undoTargets[1].revision == 0 &&
                  named.targets(true).empty(),
              "history targets list undo destinations nearest first");
        check(named.undo(state) && named.undoAction() == "Move clip" &&
                  named.redoAction() == "Add marker",
              "undo transfers the correct name to redo");
        check(named.undo(state) && named.undoAction().isEmpty() &&
                  named.redoAction() == "Move clip",
              "multiple undo operations retain action order");
        const auto redoTargets = named.targets(true);
        check(redoTargets.size() == 2 && redoTargets[0].action == "Move clip" &&
                  redoTargets[0].revision == movedRevision && redoTargets[1].action == "Add marker",
              "history targets list redo destinations nearest first");
        check(named.redo(state) && named.undoAction() == "Move clip" &&
                  named.redoAction() == "Add marker",
              "redo restores action names in the correct order");
        named.record(state, "Sequence clips");
        state.session.tracks[0].name = "sequenced";
        check(named.undoAction() == "Sequence clips" && named.redoAction().isEmpty(),
              "branching discards stale redo names");
        named.record(state, {});
        check(named.undoAction() == "Edit", "unnamed actions get a safe fallback label");
        named.reset();
        check(named.undoAction().isEmpty() && named.redoAction().isEmpty(),
              "reset removes action names");
    }
    {
        carta::EditHistory history;
        carta::EditorState state;
        state.session.tracks.push_back({"Original", {}});
        state.session.markers.push_back({0.25, "one"});
        state.selection = {10, 30, 0, 0};
        state.selectedTrack = 0;
        state.selectedMarker = 0;
        state.position = 0.25;
        history.record(state);
        state.session.tracks[0].name = "Edited";
        state.selection = {};
        state.selectedMarker = -1;
        state.masterChannels = 1;
        state.position = 1;
        check(history.isDirty() && history.canUndo() && !history.canRedo(),
              "edit creates dirty undo state");
        history.markSaved();
        check(!history.isDirty(), "saving marks current revision clean");
        check(history.undo(state) && history.isDirty() &&
                  state.session.tracks[0].name == "Original",
              "undo restores project and leaves saved revision");
        check(state.selection.start == 10 && state.selection.end == 30 &&
                  state.selectedTrack == 0 && state.selectedMarker == 0 &&
                  state.masterChannels == 2 && std::abs(state.position - 0.25) < 1e-12,
              "undo restores selection marker channels and cursor");
        check(history.redo(state) && !history.isDirty() &&
                  state.session.tracks[0].name == "Edited" && state.masterChannels == 1 &&
                  !state.selection.hasRange(),
              "redo restores saved project and editing context");
        check(history.undo(state), "undo saved revision before branching");
        history.record(state);
        state.session.tracks[0].name = "Different edit";
        check(history.isDirty() && !history.canRedo(),
              "branch clears redo and does not reuse saved revision");
        check(history.undo(state) && history.isDirty(),
              "undo after branch does not confuse saved state");
        history.reset();
        history.record(state);
        state.session.tracks[0].name = "save snapshot";
        const auto savingRevision = history.revision();
        history.record(state);
        state.session.tracks[0].name = "newer edit";
        history.markSaved(savingRevision);
        check(history.isDirty(), "completing an older save does not mark newer edits clean");
        check(history.undo(state) && !history.isDirty() &&
                  state.session.tracks[0].name == "save snapshot",
              "undo reaches asynchronously saved revision");
        check(history.redo(state) && history.isDirty(),
              "redo leaves asynchronously saved revision");
        history.reset();
        check(!history.isDirty() && !history.canUndo() && !history.canRedo(),
              "new project resets history and saved revision");
        history.markRecovered();
        check(history.isDirty() && !history.canUndo(),
              "recovered project starts dirty without fabricated undo history");
        history.markSaved();
        check(!history.isDirty(), "saving a recovered project marks it clean");
        history.reset();
        for (int n = 0; n < 45; ++n) {
            history.record(state);
            state.session.tracks[0].name = juce::String(n);
        }
        int undone = 0;
        while (history.undo(state))
            ++undone;
        check(undone == 40 && history.isDirty(),
              "history limit retains forty edits and cannot falsely reach discarded saved state");
        int redone = 0;
        while (history.redo(state))
            ++redone;
        check(redone == 40 && history.isDirty(), "redo retains all available revisions");
    }
    {
        carta::EditorState state;
        state.session.tracks.push_back({"A", {}});
        carta::EditHistory probe;
        probe.record(state);
        const auto bytes = probe.estimatedMetadataBytes();
        check(bytes > 0 && !probe.takeBudgetPruned(), "history estimates snapshot metadata");
        auto selectedState = state;
        selectedState.selectedClips.resize(1000);
        selectedState.selectedTracks.resize(1000);
        carta::EditHistory selectionProbe;
        selectionProbe.record(selectedState);
        check(selectionProbe.estimatedMetadataBytes() >=
                  bytes + 1000 * (sizeof(carta::ClipReference) + sizeof(int)),
              "history metadata budget accounts for stored clip and track selections");
        carta::EditHistory limited(bytes * 2);
        limited.record(state);
        state.session.tracks[0].name = "B";
        limited.record(state);
        state.session.tracks[0].name = "C";
        limited.record(state);
        state.session.tracks[0].name = "D";
        check(limited.takeBudgetPruned() && !limited.takeBudgetPruned() &&
                  limited.estimatedMetadataBytes() <= bytes * 2,
              "metadata budget prunes and reports old history");
        check(limited.undo(state) && state.session.tracks[0].name == "C" && limited.undo(state) &&
                  state.session.tracks[0].name == "B" && !limited.undo(state),
              "budget retains nearest undo entries");
        check(limited.redo(state) && state.session.tracks[0].name == "C" && limited.redo(state) &&
                  state.session.tracks[0].name == "D",
              "retained history redoes in order");
        carta::EditHistory tiny(1);
        tiny.record(state);
        check(tiny.isDirty() && !tiny.canUndo() && tiny.estimatedMetadataBytes() == 0 &&
                  tiny.takeBudgetPruned(),
              "oversized snapshot is dropped and edit stays dirty");
        tiny.reset();
        check(!tiny.isDirty() && !tiny.takeBudgetPruned(), "history reset clears budget warning");
        carta::EditorState growing;
        growing.session.tracks.push_back({"A", {}});
        carta::EditHistory variable(bytes * 2);
        variable.record(growing);
        growing.session.tracks[0].name =
            juce::String::repeatedString("x", static_cast<int>(bytes * 3));
        check(variable.undo(growing) && growing.session.tracks[0].name == "A" &&
                  !variable.canRedo() && variable.takeBudgetPruned(),
              "undo reports when an oversized redo snapshot cannot be retained");
        check(variable.estimatedMetadataBytes() <= bytes * 2 && !variable.isDirty(),
              "pruned redo cannot confuse the saved revision or exceed the metadata budget");
    }
    {
        const double rate = carta::sessionRate;
        auto a = std::make_shared<carta::Audio>(), b = std::make_shared<carta::Audio>();
        for (auto audio : {a, b}) {
            audio->sampleRate = rate;
            audio->samples.setSize(2, 4096);
            audio->samples.clear();
            carta::buildWaveformPeaks(*audio);
        }
        carta::EditorState first;
        first.session.tracks.push_back({"A", {{a, 0, 0, 4096 / rate}, {a, 0, 0, 4096 / rate}}});
        carta::EditorState second;
        second.session.tracks.push_back({"B", {{b, 0, 0, 4096 / rate}}});
        carta::Clipboard clipboard;
        carta::EditHistory history;
        history.record(first);
        history.record(first);
        const auto retained = history.estimatedRetainedMediaBytes(second.session, clipboard);
        check(retained >= 2u * 4096u * sizeof(float) && retained < 3u * 4096u * sizeof(float),
              "history counts each retained decoded source and peak payload once");
        clipboard.lengthSamples = 1;
        clipboard.tracks = {{{a, 0, 0, 1 / rate}}};
        check(history.estimatedRetainedMediaBytes(second.session, clipboard) == 0,
              "clipboard-held sources do not count as history-only media");
        history.enforceRetainedMediaBudget(second.session, clipboard, 0);
        check(history.canUndo() && !history.takeBudgetPruned(),
              "clipboard-held media preserves history under the media budget");
        clipboard = {};
        history.enforceRetainedMediaBudget(second.session, clipboard, retained - 1);
        check(!history.canUndo() && history.takeBudgetPruned() && history.isDirty(),
              "history prunes all snapshots retaining over-budget media and remains dirty");
        carta::EditHistory shared;
        shared.record(first);
        check(shared.estimatedRetainedMediaBytes(first.session, clipboard) == 0,
              "current-session sources do not count as history-only media");
        shared.enforceRetainedMediaBudget(first.session, clipboard, 0);
        check(shared.canUndo(), "current-session sources do not force history pruning");
        carta::EditHistory redo;
        redo.record(first);
        auto current = second;
        check(redo.undo(current) && current.session.tracks[0].clips[0].audio == a &&
                  redo.estimatedRetainedMediaBytes(current.session, clipboard) > 0,
              "redo-only media is counted after undo");
        redo.enforceRetainedMediaBudget(current.session, clipboard, 0);
        check(!redo.canRedo() && redo.takeBudgetPruned() && !redo.isDirty(),
              "redo-only media is pruned without changing the restored revision");
    }
    {
        const double rate = carta::sessionRate;
        auto storage = std::make_shared<carta::MappedPcm>();
        storage->channels = 2;
        storage->frames = 1000000;
        auto first = std::make_shared<carta::Audio>(), alias = std::make_shared<carta::Audio>();
        first->sampleRate = alias->sampleRate = rate;
        first->mapped = alias->mapped = storage;
        carta::EditorState state;
        state.session.tracks.push_back({"Mapped", {{first, 0, 0, 1}, {alias, 0, 0, 1}}});
        carta::EditHistory history;
        history.record(state);
        history.record(state);
        carta::Session empty;
        carta::Clipboard clipboard;
        const auto mappedBytes = history.estimatedRetainedMappedBytes(empty, clipboard);
        check(mappedBytes == 8000000 && history.estimatedRetainedMediaBytes(empty, clipboard) == 0,
              "history counts a shared mapped PCM file once and excludes it from heap payload");
        clipboard.lengthSamples = 1;
        clipboard.tracks = {{{alias, 0, 0, 1 / rate}}};
        check(history.estimatedRetainedMappedBytes(empty, clipboard) == 0,
              "clipboard-held mapped storage is not counted as history-only");
        history.enforceRetainedMediaBudget(empty, clipboard, std::numeric_limits<size_t>::max(), 0);
        check(history.canUndo() && !history.takeBudgetPruned(),
              "clipboard-held mapped audio does not prune history");
        clipboard = {};
        history.enforceRetainedMediaBudget(empty, clipboard, std::numeric_limits<size_t>::max(),
                                           mappedBytes - 1);
        check(!history.canUndo() && history.takeBudgetPruned() && history.isDirty(),
              "mapped PCM budget prunes retaining snapshots and keeps revision dirty");
        carta::EditHistory currentShared;
        currentShared.record(state);
        carta::Session current;
        current.tracks.push_back({"Current", {{alias, 0, 0, 1}}});
        check(currentShared.estimatedRetainedMappedBytes(current, clipboard) == 0,
              "a current source sharing the mapped file protects it from history accounting");
        carta::EditHistory actual;
        juce::File temporary;
        {
            auto mapped = std::make_shared<carta::MappedPcm>();
            mapped->channels = 1;
            mapped->frames = 8;
            temporary = mapped->temporary.getFile();
            const float data[8]{};
            check(temporary.replaceWithData(data, sizeof(data)),
                  "create mapped history PCM fixture");
            mapped->mapping = std::make_unique<juce::MemoryMappedFile>(
                temporary, juce::MemoryMappedFile::readOnly);
            check(mapped->mapping->getData() != nullptr, "map retained PCM fixture");
            auto audio = std::make_shared<carta::Audio>();
            audio->sampleRate = rate;
            audio->mapped = mapped;
            carta::EditorState prior;
            prior.session.tracks.push_back({"Temporary", {{audio, 0, 0, 8 / rate}}});
            actual.record(std::move(prior));
        }
        check(temporary.existsAsFile() &&
                  actual.estimatedRetainedMappedBytes(empty, clipboard) == 32,
              "history retains a real mapped PCM file");
        actual.enforceRetainedMediaBudget(empty, clipboard, std::numeric_limits<size_t>::max(), 0);
        check(!actual.canUndo() && !temporary.exists() && actual.takeBudgetPruned(),
              "pruning the last history reference removes its temporary PCM file");
    }
    {
        carta::SincResampler converter(96000, 44100);
        check(converter.valid() && !carta::SincResampler(0, 44100).valid(),
              "resampler validates rates");
        check(carta::SincResampler::hasFullSupport(1536000, 44100) &&
                  !carta::SincResampler::hasFullSupport(3072000, 44100) &&
                  !carta::SincResampler::hasFullSupport(0, 44100),
              "resampler identifies ratios beyond its bounded support");
        {
            auto high = std::make_shared<carta::Audio>(), normal = std::make_shared<carta::Audio>();
            high->sampleRate = 3072000;
            high->samples.setSize(1, 32);
            high->samples.clear();
            normal->sampleRate = carta::sessionRate;
            normal->samples.setSize(1, 44100);
            normal->samples.clear();
            carta::Session mix;
            mix.tracks.push_back({"Extreme", {{high, 0, 0, 32 / high->sampleRate}}});
            mix.tracks.push_back({"Normal", {{normal, 0, 0, 1}}});
            juce::String error;
            carta::ExportPreflight result;
            result.channels = 17;
            check(!carta::validateMixRange(mix, 2, {0, 1}, error) && error.contains("Extreme") &&
                      error.contains("sample rate"),
                  "audible extreme ratio rejects analysis and reports the affected track");
            check(!carta::scanExportPreflight(mix, 2, {0, 1}, result, error) &&
                      result.channels == 17,
                  "extreme ratio rejects preflight without changing its result");
            auto target = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getNonexistentChildFile("carta-rate-guard", ".wav");
            check(target.replaceWithText("preserve"),
                  "create export destination for rate validation");
            bool clipped = false;
            check(!carta::exportWavRange(mix, 2, {0, 1}, target, clipped, error) &&
                      target.loadFileAsString() == "preserve",
                  "extreme ratio rejects export without changing the destination");
            target.deleteFile();
            check(carta::validateMixRange(mix, 2, {1, 2}, error),
                  "extreme source outside a selected range does not block it");
            mix.tracks[0].muted = true;
            check(carta::validateMixRange(mix, 2, {0, 1}, error),
                  "muted extreme source does not block rendering");
            mix.tracks[0].muted = false;
            mix.tracks[1].solo = true;
            check(carta::validateMixRange(mix, 2, {0, 1}, error),
                  "non-solo extreme source does not block rendering");
            mix.tracks[1].solo = false;
            high->sampleRate = 1536000;
            check(carta::validateMixRange(mix, 2, {0, 1}, error),
                  "the full-support ratio remains renderable");
        }
        {
            auto invalidAudio = std::make_shared<carta::Audio>();
            invalidAudio->sampleRate = carta::sessionRate;
            invalidAudio->samples.setSize(1, 4);
            invalidAudio->samples.clear();
            invalidAudio->samples.setSample(0, 0, 0.5f);
            carta::Session invalid;
            invalid.tracks.push_back({"Out-of-range source", {{invalidAudio, 0, 1.0e100, 1}}});
            carta::ResamplingContext context;
            invalid.prepareResampling(context, carta::sessionRate);
            juce::AudioBuffer<float> output(2, 2);
            output.clear();
            invalid.render(output, 0, 2, 0, carta::sessionRate, 2, &context);
            check(
                near(output.getSample(0, 0), 0) && near(output.getSample(1, 0), 0),
                "render skips an unrepresentable source index before converting it to an integer");
        }
        carta::ResamplingContext prepared;
        const auto prebuilt = std::make_shared<carta::SincResampler>(96000, 44100);
        prepared.registerPrepared(96000, 44100, prebuilt);
        check(prepared.find(96000, 44100) == prebuilt.get(),
              "prepared resampler is installed without rebuilding");
        prepared.registerPrepared(48000, 44100, prebuilt);
        check(prepared.find(48000, 44100) && prepared.find(48000, 44100)->matches(48000, 44100),
              "stale resampler is replaced for correct rates");
        prepared.prepareOutput(48000);
        check(prepared.find(96000, 48000) && prepared.find(48000, 48000),
              "device-rate changes prepare registered sources");
        constexpr int n = 12000;
        std::vector<float> dc(n, 0.25f), pass(n), upper(n), high(n), impulse(n, 0.0f);
        for (int i = 0; i < n; ++i) {
            pass[static_cast<size_t>(i)] = static_cast<float>(
                std::sin(2.0 * juce::MathConstants<double>::pi * 1000 * i / 96000));
            upper[static_cast<size_t>(i)] = static_cast<float>(
                std::sin(2.0 * juce::MathConstants<double>::pi * 20000 * i / 96000));
            high[static_cast<size_t>(i)] = static_cast<float>(
                std::sin(2.0 * juce::MathConstants<double>::pi * 24000 * i / 96000));
        }
        impulse[4000] = 1.0f;
        for (double p : {0.0, 0.33, 20.75, 5000.12, 11999.0})
            check(near(converter.sample(dc.data(), n, p), 0.25f),
                  "resampler preserves DC including boundaries");
#if defined(__unix__) || defined(__APPLE__)
        // Reserve address space only: this exercises the real maximum-length
        // indexing path without allocating eight gigabytes of physical PCM.
        if (sizeof(size_t) >= 8) {
            const auto pageValue = sysconf(_SC_PAGESIZE);
            check(pageValue > 0, "query memory page size for long-source resampling");
            const auto page = static_cast<size_t>(pageValue),
                       count = static_cast<size_t>(std::numeric_limits<int>::max());
            const auto bytes = count * sizeof(float),
                       mappingBytes = ((bytes + page - 1) / page) * page;
            void* mapping = mmap(nullptr, mappingBytes, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (mapping != MAP_FAILED) {
                constexpr size_t tailCount = 8192;
                const auto tailPage = ((bytes - tailCount * sizeof(float)) / page) * page;
                check(mprotect(mapping, page, PROT_READ | PROT_WRITE) == 0 &&
                          mprotect(static_cast<char*>(mapping) + tailPage, mappingBytes - tailPage,
                                   PROT_READ | PROT_WRITE) == 0,
                      "make long-source boundary pages accessible");
                auto* data = static_cast<float*>(mapping);
                std::vector<float> tail(tailCount);
                for (size_t i = 0; i < tailCount; ++i)
                    data[count - tailCount + i] = tail[i] =
                        static_cast<float>(0.2 + 0.15 * std::sin(static_cast<double>(i) * 0.17));
                for (double fromEnd : {0.25, 0.5, 1.0, 20.75, 127.5}) {
                    const auto actual = converter.sample(data, static_cast<int>(count),
                                                         static_cast<double>(count) - fromEnd);
                    const auto expected =
                        converter.sample(tail.data(), static_cast<int>(tail.size()),
                                         static_cast<double>(tail.size()) - fromEnd);
                    const auto pair = converter.sampleStereo(data, data, static_cast<int>(count),
                                                             static_cast<double>(count) - fromEnd);
                    check(std::memcmp(&actual, &expected, sizeof(float)) == 0 &&
                              std::memcmp(&pair.first, &expected, sizeof(float)) == 0 &&
                              std::memcmp(&pair.second, &expected, sizeof(float)) == 0,
                          "mono and stereo resampling clamp correctly near the maximum source "
                          "length");
                }
                check(munmap(mapping, mappingBytes) == 0,
                      "release long-source address reservation");
            } else
                std::cerr
                    << "SKIP: could not reserve address space for maximum-length resampler test\n";
        }
#endif
        for (int i = 0; i < 2000; ++i) {
            const double p =
                static_cast<double>((i * 2791) % n) + static_cast<double>(i % 997) / 997.0;
            if (p >= n)
                continue;
            const auto pair = converter.sampleStereo(pass.data(), high.data(), n, p);
            const float a = converter.sample(pass.data(), n, p),
                        b = converter.sample(high.data(), n, p);
            check(std::memcmp(&pair.first, &a, sizeof(float)) == 0 &&
                      std::memcmp(&pair.second, &b, sizeof(float)) == 0,
                  "stereo resampling is bit-identical to independent channels including source "
                  "boundaries");
        }
        for (const double sourceRate : {48000.0, 192000.0}) {
            carta::SincResampler stereoConverter(sourceRate, 44100);
            for (const double p : {0.0, 0.125, 20.75, 4000.125, 11999.0, 11999.875}) {
                const auto pair = stereoConverter.sampleStereo(pass.data(), high.data(), n, p);
                const float a = stereoConverter.sample(pass.data(), n, p),
                            b = stereoConverter.sample(high.data(), n, p);
                check(std::memcmp(&pair.first, &a, sizeof(float)) == 0 &&
                          std::memcmp(&pair.second, &b, sizeof(float)) == 0,
                      "stereo resampling matches independent channels at different conversion "
                      "ratios");
            }
        }
        auto stereo = std::make_shared<carta::Audio>(),
             fourChannel = std::make_shared<carta::Audio>();
        stereo->sampleRate = fourChannel->sampleRate = 96000;
        stereo->samples.setSize(2, n);
        fourChannel->samples.setSize(4, n);
        fourChannel->samples.clear();
        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch) {
                const auto value =
                    ch == 0 ? pass[static_cast<size_t>(i)] : high[static_cast<size_t>(i)];
                stereo->samples.setSample(ch, i, value);
                fourChannel->samples.setSample(ch, i, value);
            }
        carta::Session optimized, scalar;
        optimized.tracks.push_back(
            {"Stereo", {{stereo, 2 / carta::sessionRate, 7.25 / 96000, 100 / carta::sessionRate}}});
        scalar.tracks.push_back(
            {"Four",
             {{fourChannel, 2 / carta::sessionRate, 7.25 / 96000, 100 / carta::sessionRate}}});
        optimized.tracks[0].routing = {{0.6f, 0.2f}, {0.1f, 0.7f}};
        scalar.tracks[0].routing = {{0.6f, 0.2f}, {0.1f, 0.7f}, {0, 0}, {0, 0}};
        check(carta::setClipEnvelope(optimized.tracks[0].clips[0], 0.7f, 20 / carta::sessionRate,
                                     20 / carta::sessionRate) &&
                  carta::setClipEnvelope(scalar.tracks[0].clips[0], 0.7f, 20 / carta::sessionRate,
                                         20 / carta::sessionRate),
              "prepare routed faded resampler fixtures");
        carta::ResamplingContext renderContext;
        optimized.prepareResampling(renderContext, carta::sessionRate);
        for (int masterChannels : {1, 2}) {
            juce::AudioBuffer<float> fast(2, 120), reference(2, 120);
            fast.clear();
            reference.clear();
            optimized.render(fast, 0, 120, 0, carta::sessionRate, masterChannels, &renderContext);
            scalar.render(reference, 0, 120, 0, carta::sessionRate, masterChannels, &renderContext);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 120; ++i) {
                    const float a = fast.getSample(ch, i), b = reference.getSample(ch, i);
                    check(std::memcmp(&a, &b, sizeof(float)) == 0,
                          "optimized stereo mix matches independent-channel rendering with custom "
                          "routes and fades");
                }
        }
        double passError = 0, upperError = 0, aliasEnergy = 0;
        constexpr int measurements = 3500;
        for (int i = 0; i < measurements; ++i) {
            const double sourcePosition = 128.0 + static_cast<double>(i) * 96000 / 44100;
            const double expected =
                std::sin(2.0 * juce::MathConstants<double>::pi * 1000 * sourcePosition / 96000);
            const auto actual = converter.sample(pass.data(), n, sourcePosition);
            const auto unwanted = converter.sample(high.data(), n, sourcePosition);
            const auto upperActual = converter.sample(upper.data(), n, sourcePosition);
            const auto upperExpected =
                std::sin(2.0 * juce::MathConstants<double>::pi * 20000 * sourcePosition / 96000);
            passError += (actual - expected) * (actual - expected);
            upperError += (upperActual - upperExpected) * (upperActual - upperExpected);
            aliasEnergy += unwanted * unwanted;
        }
        check(std::sqrt(passError / measurements) < 0.0001,
              "resampler preserves passband without delay");
        check(std::sqrt(upperError / measurements) < 0.005,
              "resampler preserves the upper audible passband");
        check(std::sqrt(aliasEnergy / measurements) < 0.0002,
              "resampler rejects out-of-band alias");
        carta::SincResampler higherRate(192000, 44100);
        std::vector<float> upper192(24000), reject192(24000);
        for (int i = 0; i < 24000; ++i) {
            upper192[static_cast<size_t>(i)] = static_cast<float>(
                std::sin(2.0 * juce::MathConstants<double>::pi * 20000 * i / 192000));
            reject192[static_cast<size_t>(i)] = static_cast<float>(
                std::sin(2.0 * juce::MathConstants<double>::pi * 24000 * i / 192000));
        }
        double error192 = 0, alias192 = 0;
        for (int i = 0; i < measurements; ++i) {
            const double at = 256.0 + static_cast<double>(i) * 192000 / 44100;
            const auto expected192 =
                std::sin(2.0 * juce::MathConstants<double>::pi * 20000 * at / 192000);
            const auto value = higherRate.sample(upper192.data(), 24000, at);
            const auto alias = higherRate.sample(reject192.data(), 24000, at);
            error192 += (value - expected192) * (value - expected192);
            alias192 += alias * alias;
        }
        check(std::sqrt(error192 / measurements) < 0.005 &&
                  std::sqrt(alias192 / measurements) < 0.0002,
              "192 kHz conversion preserves passband and rejects aliasing");
        for (const double sourceRate : {384000.0, 768000.0}) {
            carta::SincResampler highRate(sourceRate, 44100);
            const int length = static_cast<int>(sourceRate * 0.025);
            std::vector<float> low(static_cast<size_t>(length)), upperBand(low.size()),
                rejected(low.size());
            for (int i = 0; i < length; ++i) {
                low[static_cast<size_t>(i)] = static_cast<float>(
                    std::sin(2.0 * juce::MathConstants<double>::pi * 1000 * i / sourceRate));
                upperBand[static_cast<size_t>(i)] = static_cast<float>(
                    std::sin(2.0 * juce::MathConstants<double>::pi * 20000 * i / sourceRate));
                rejected[static_cast<size_t>(i)] = static_cast<float>(
                    std::sin(2.0 * juce::MathConstants<double>::pi * 24000 * i / sourceRate));
            }
            double lowError = 0, highError = 0, alias = 0;
            constexpr int points = 500;
            for (int i = 0; i < points; ++i) {
                const double at = static_cast<double>(highRate.supportTaps(sourceRate, 44100)) / 2 +
                                  16 + i * sourceRate / 44100;
                const double lowExpected = std::sin(2.0 * juce::MathConstants<double>::pi * 1000 *
                                                    at / sourceRate),
                             highExpected = std::sin(2.0 * juce::MathConstants<double>::pi * 20000 *
                                                     at / sourceRate);
                const double lowDifference = highRate.sample(low.data(), length, at) - lowExpected,
                             highDifference =
                                 highRate.sample(upperBand.data(), length, at) - highExpected,
                             unwanted = highRate.sample(rejected.data(), length, at);
                lowError += lowDifference * lowDifference;
                highError += highDifference * highDifference;
                alias += unwanted * unwanted;
            }
            check(std::sqrt(lowError / points) < 0.0001 && std::sqrt(highError / points) < 0.005 &&
                      std::sqrt(alias / points) < 0.0002,
                  "high-rate conversion preserves passband and rejects aliasing");
        }
        check(converter.sample(impulse.data(), n, 4000) > 0.4f &&
                  std::abs(converter.sample(impulse.data(), n, 4200)) < 0.00001f,
              "resampler impulse centered and bounded");
    }
    carta::Audio tone;
    tone.sampleRate = 8192;
    tone.samples.setSize(2, 4096);
    tone.samples.clear();
    for (int i = 0; i < 4096; ++i)
        tone.samples.setSample(0, i,
                               0.5f * std::sin(2.0f * juce::MathConstants<float>::pi * 1024.0f *
                                               static_cast<float>(i) / 8192.0f));
    const auto spectrum = carta::makeSpectrogramTile(tone, 0, 512, 8);
    check(spectrum.isValid() && spectrum.getHeight() == 192, "spectrum channel layout");
    const int toneRow =
        static_cast<int>(std::round(95 * (1 - std::log(1024.0 / 20.0) / std::log(4096.0 / 20.0))));
    check(spectrum.getPixelAt(spectrum.getWidth() / 2, toneRow).getBrightness() > 0.5f,
          "spectrum detects tone frequency");
    check(spectrum.getPixelAt(spectrum.getWidth() / 2, 96 + toneRow).getBrightness() < 0.01f,
          "spectrum separates silent channel");
    check(!carta::makeSpectrogramTile(tone, 0, 512, 8, [] { return true; }).isValid(),
          "spectrum cancellation");
    {
        carta::Audio waveform;
        waveform.sampleRate = 44100;
        waveform.samples.setSize(2, 1031);
        waveform.samples.clear();
        waveform.samples.setSample(0, 255, 1.0f);
        waveform.samples.setSample(0, 768, -0.75f);
        for (int i = 0; i < 1031; ++i)
            waveform.samples.setSample(1, i, static_cast<float>((i * 37) % 101 - 50) / 100.0f);
        carta::buildWaveformPeaks(waveform);
        check(near(carta::waveformRange(waveform, 0, 256, 768).getEnd(), 0),
              "waveform does not include transient outside range");
        check(near(carta::waveformRange(waveform, 0, 254, 257).getEnd(), 1),
              "waveform includes transient within range");
        for (int ch = 0; ch < 2; ++ch)
            for (int begin = 0; begin < 1031; begin += 31)
                for (int end = begin + 1; end <= 1031; end += 47) {
                    const auto expected = waveform.samples.findMinMax(ch, begin, end - begin);
                    const auto actual = carta::waveformRange(waveform, ch, begin, end);
                    check(near(expected.getStart(), actual.getStart()) &&
                              near(expected.getEnd(), actual.getEnd()),
                          "cached waveform range matches source samples");
                }
        waveform.peakCache->peaks.clear();
        check(near(carta::waveformRange(waveform, 0, 254, 257).getEnd(), 1),
              "waveform falls back when cache is unavailable");
    }
    {
        auto budget = std::make_shared<carta::WaveformPeakBudget>(4096);
        std::vector<std::shared_ptr<carta::Audio>> sources;
        for (int i = 0; i < 20; ++i) {
            auto audio = std::make_shared<carta::Audio>();
            audio->sampleRate = 44100;
            audio->samples.setSize(1, 131072);
            audio->samples.clear();
            audio->samples.setSample(0, 50000, 0.75f);
            check(carta::buildWaveformPeaks(*audio, 16u * 1024u * 1024u, {}, budget),
                  "build a waveform under the shared budget");
            check(budget->allocated() <= budget->limit &&
                      near(carta::waveformRange(*audio, 0, 49999, 50002).getEnd(), 0.75f),
                  "shared cache stays within its budget and fallback remains accurate");
            sources.push_back(std::move(audio));
        }
        size_t total = 0;
        bool degraded = false, unavailable = false;
        for (const auto& audio : sources) {
            total += audio->peakCache->reserved;
            degraded |= audio->peakCache->blockSize > 256;
            unavailable |= audio->peakCache->peaks.empty();
        }
        check(total == budget->allocated() && total <= budget->limit && degraded && unavailable,
              "multiple sources share a strict payload cap and eventually use uncached fallback");
        auto copy = std::make_shared<carta::Audio>(*sources.front());
        auto firstCache = copy->peakCache;
        const auto held = budget->allocated();
        check(copy->peakCache == sources.front()->peakCache && budget->allocated() == held,
              "copying an audio object shares its peak payload and reservation");
        check(
            !carta::buildWaveformPeaks(*sources.front(), 4096, [](double) { return false; }) &&
                sources.front()->peakCache == firstCache && budget->allocated() == held,
            "cancelled managed rebuild keeps the old cache and releases its temporary reservation");
        sources.clear();
        check(budget->allocated() == firstCache->reserved,
              "reservations release when all other source caches are destroyed");
        copy.reset();
        check(budget->allocated() == firstCache->reserved,
              "shared cache stays reserved while another reference exists");
        const auto sharedBytes = firstCache->reserved;
        check(sharedBytes > 0, "shared cache retained nonzero payload");
        firstCache.reset();
        check(budget->allocated() == 0, "last cache reference releases the remaining reservation");
    }
    {
        constexpr int frames = 2100123;
        carta::Audio large;
        large.sampleRate = 44100;
        large.samples.setSize(2, frames);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < frames; ++i)
                large.samples.setSample(
                    ch, i,
                    static_cast<float>((static_cast<juce::int64>(i) * 37 + ch * 19) % 2001 - 1000) /
                        1000.0f);
        large.samples.setSample(0, 4095, 2.0f);
        large.samples.setSample(0, 4096, -2.0f);
        carta::buildWaveformPeaks(large);
        check(large.peakCache->levels.size() == 2 && large.peakCache->levels[0].size() >= 3,
              "large waveform builds multiple cache levels");
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 100; ++i) {
                const int begin = static_cast<int>((static_cast<juce::int64>(i) * 17261) % frames);
                const int length =
                    static_cast<int>((static_cast<juce::int64>(i) * 7919 + 1) % (frames - begin)) +
                    1;
                const int end = begin + length;
                const auto expected = large.samples.findMinMax(ch, begin, length);
                const auto actual = carta::waveformRange(large, ch, begin, end);
                check(near(expected.getStart(), actual.getStart()) &&
                          near(expected.getEnd(), actual.getEnd()),
                      "hierarchical waveform query matches raw sample ranges");
            }
        check(near(carta::waveformRange(large, 0, 0, 4096).getEnd(), 2) &&
                  near(carta::waveformRange(large, 0, 4096, 8192).getStart(), -2),
              "hierarchical waveform keeps boundary impulses on the correct side");
        const auto cached = carta::waveformRange(large, 1, 1024, frames - 17);
        large.peakCache->levels.clear();
        const auto flat = carta::waveformRange(large, 1, 1024, frames - 17);
        check(near(cached.getStart(), flat.getStart()) && near(cached.getEnd(), flat.getEnd()),
              "missing higher cache levels fall back to base peaks");
        const auto previousBlock = large.peakCache->blockSize;
        const auto previousCount = large.peakCache->peaks[0].size();
        double lastProgress = -1;
        bool monotonic = true;
        int progressCalls = 0;
        check(!carta::buildWaveformPeaks(large, 64,
                                         [&](double value) {
                                             if (value < lastProgress)
                                                 monotonic = false;
                                             lastProgress = value;
                                             ++progressCalls;
                                             return value < 0.05;
                                         }) &&
                  monotonic && progressCalls > 1 && lastProgress < 0.2,
              "large adaptive waveform blocks can be cancelled during a block");
        check(large.peakCache->blockSize == previousBlock &&
                  large.peakCache->peaks[0].size() == previousCount &&
                  carta::waveformRange(large, 1, 1024, frames - 17) == cached,
              "cancelled waveform rebuild preserves the previous cache");
        lastProgress = -1;
        monotonic = true;
        check(carta::buildWaveformPeaks(large, 64,
                                        [&](double value) {
                                            if (value < lastProgress)
                                                monotonic = false;
                                            lastProgress = value;
                                            return true;
                                        }) &&
                  monotonic && std::abs(lastProgress - 1) < 1e-12,
              "adaptive waveform rebuild reports monotonic complete progress");
        for (size_t budget : {size_t{1024}, size_t{64}, size_t{0}}) {
            carta::buildWaveformPeaks(large, budget);
            size_t payload = 0;
            for (const auto& channel : large.peakCache->peaks)
                payload += channel.size() * sizeof(juce::Range<float>);
            for (const auto& channel : large.peakCache->levels)
                for (const auto& level : channel)
                    payload += level.size() * sizeof(juce::Range<float>);
            check(payload <= budget && (budget == 0 || large.peakCache->blockSize > 256),
                  "waveform cache payload respects a small per-source budget");
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 20; ++i) {
                    const int begin =
                        static_cast<int>((static_cast<juce::int64>(i) * 29317) % frames);
                    const int length = static_cast<int>((static_cast<juce::int64>(i) * 13117 + 37) %
                                                        (frames - begin)) +
                                       1;
                    const auto expected = large.samples.findMinMax(ch, begin, length),
                               actual = carta::waveformRange(large, ch, begin, begin + length);
                    check(near(expected.getStart(), actual.getStart()) &&
                              near(expected.getEnd(), actual.getEnd()),
                          "adaptive and uncached waveform ranges match raw samples");
                }
            auto shared = std::make_shared<carta::Audio>();
            shared->sampleRate = large.sampleRate;
            shared->samples.makeCopyOf(large.samples);
            shared->peakCache->peaks = large.peakCache->peaks;
            shared->peakCache->levels = large.peakCache->levels;
            shared->peakCache->blockSize = large.peakCache->blockSize;
            carta::Clip clip{shared, 0, 0, shared->length()};
            clip.gain = 0.7f;
            clip.fadeIn = clip.duration;
            clip.fadeInStart = 0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 20; ++i) {
                    const int begin = (i * 29317) % frames, end = std::min(frames, begin + 13117);
                    juce::Range<float> bound{9, 10};
                    const bool available = carta::cachedWaveformBounds(clip, ch, begin, end, bound);
                    check(available == (budget != 0),
                          "provisional waveform bounds require a complete cache");
                    if (available)
                        for (int sample = begin; sample < end; ++sample) {
                            const float value = shared->samples.getSample(ch, sample) *
                                                carta::clipLevel(clip, static_cast<double>(sample) /
                                                                           shared->sampleRate);
                            check(value >= bound.getStart() - 1e-6f &&
                                      value <= bound.getEnd() + 1e-6f,
                                  "provisional waveform bounds contain every faded sample");
                        }
                    else
                        check(bound == juce::Range<float>(9, 10),
                              "missing cache leaves provisional bounds unchanged");
                }
        }
    }
    {
        for (const int zoom : {1, 2, 4, 8}) {
            const float bound = 1.0f / static_cast<float>(zoom);
            check(near(carta::waveformDisplayValue(bound, zoom), 1.0f) &&
                      near(carta::waveformDisplayValue(-bound, zoom), -1.0f) &&
                      near(carta::waveformDisplayValue(bound * 0.5f, zoom), 0.5f),
                  "waveform zoom maps linear amplitude consistently to lane height");
            check(near(carta::waveformDisplayValue(2.0f, zoom), 1.0f) &&
                      near(carta::waveformDisplayValue(-2.0f, zoom), -1.0f),
                  "waveform zoom clips display geometry but not audio");
            check(std::abs(carta::waveformDisplayDb(1, zoom) -
                           20 * std::log10(static_cast<double>(bound))) < 1e-10 &&
                      std::abs(carta::waveformDisplayDb(0.5, zoom) -
                               20 * std::log10(static_cast<double>(bound) * 0.5)) < 1e-10,
                  "waveform gutter labels match the vertical zoom");
            for (int db = -59; db <= 0; ++db) {
                const auto sample =
                    static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0) / zoom);
                const auto lane =
                    carta::waveformDisplayValue(sample, zoom, carta::WaveformScale::decibels);
                check(std::abs(lane - (1.0f + static_cast<float>(db) / 60.0f)) < 1e-5f &&
                          near(carta::waveformDisplayValue(-sample, zoom,
                                                           carta::WaveformScale::decibels),
                               -lane),
                      "logarithmic waveform maps signed dB levels monotonically");
                check(
                    std::abs(carta::waveformDisplayDb(lane, zoom, carta::WaveformScale::decibels) -
                             20 * std::log10(static_cast<double>(sample))) < 1e-4,
                    "logarithmic gutter and waveform mapping agree");
            }
            check(near(carta::waveformDisplayValue(0.0001f / static_cast<float>(zoom), zoom,
                                                   carta::WaveformScale::decibels),
                       0) &&
                      near(carta::waveformDisplayValue(2.0f / static_cast<float>(zoom), zoom,
                                                       carta::WaveformScale::decibels),
                           1),
                  "logarithmic waveform clamps below its floor and above its visible range");
        }
        check(std::isinf(carta::waveformDisplayDb(0, 1)) && carta::waveformDisplayDb(0, 1) < 0 &&
                  near(carta::waveformDisplayValue(std::numeric_limits<float>::quiet_NaN(), 1), 0),
              "waveform scale handles silence and non-finite values safely");
        check(carta::waveformRmsDisplayRange(0.5f, 0.5f, 0.5f) == juce::Range<float>(0.0f, 0.5f) &&
                  carta::waveformRmsDisplayRange(-0.5f, -0.5f, 0.5f) ==
                      juce::Range<float>(-0.5f, 0.0f) &&
                  carta::waveformRmsDisplayRange(-0.1f, 0.9f, 0.5f) ==
                      juce::Range<float>(-0.1f, 0.5f),
              "RMS geometry never introduces a polarity absent from the peak waveform");
    }
    {
        constexpr int frames = 200000;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = 44100;
        audio->samples.setSize(2, frames);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < frames; ++i)
                audio->samples.setSample(
                    ch, i, static_cast<float>((i * 37 + ch * 101) % 2001 - 1000) / 900.0f);
        check(carta::buildWaveformPeaks(*audio), "prepare cached peaks for exact waveform columns");
        carta::Clip clip{audio, 0.4, 0.3, 3.8};
        clip.gain = 0.8f;
        clip.fadeIn = 2.0;
        clip.fadeInStart = 0.15;
        clip.fadeOut = 2.4;
        clip.fadeOutEnd = 4.3;
        constexpr int left = 120, width = 20, columns = 20;
        constexpr double view = 0, span = 5.5;
        for (int ch = 0; ch < 2; ++ch) {
            std::vector<carta::WaveformColumn> result;
            check(carta::exactWaveformColumns(clip, ch, view, span, left, width, left, columns,
                                              result) &&
                      result.size() == columns,
                  "calculate exact wide fading waveform columns");
            std::vector<carta::WaveformColumn> withRms;
            check(carta::exactWaveformColumns(clip, ch, view, span, left, width, left, columns,
                                              withRms, {}, true) &&
                      withRms.size() == columns,
                  "calculate source-channel RMS along with exact waveform peaks");
            for (int x = 0; x < columns; ++x) {
                const double start = static_cast<double>(x) * span / width,
                             end = static_cast<double>(x + 1) * span / width;
                int lo = 0, hi = 0;
                const bool exists = carta::waveformSampleRange(clip, start, end, lo, hi);
                check(result[static_cast<size_t>(x)].hasSamples == exists,
                      "exact waveform column has the same clipped sample range as the display");
                if (!exists)
                    continue;
                float low = std::numeric_limits<float>::infinity(), high = -low;
                double energy = 0;
                for (int i = lo; i < hi; ++i) {
                    const float value = audio->samples.getSample(ch, i) *
                                        carta::clipLevel(clip, i / audio->sampleRate);
                    low = std::min(low, value);
                    high = std::max(high, value);
                    energy += static_cast<double>(value) * value;
                }
                check(near(result[static_cast<size_t>(x)].range.getStart(), low) &&
                          near(result[static_cast<size_t>(x)].range.getEnd(), high),
                      "exact wide faded waveform peaks match raw envelope-adjusted samples");
                check(!result[static_cast<size_t>(x)].hasRms &&
                          withRms[static_cast<size_t>(x)].hasRms &&
                          std::abs(withRms[static_cast<size_t>(x)].rms -
                                   std::sqrt(energy / (hi - lo))) < 1e-6 &&
                          near(withRms[static_cast<size_t>(x)].range.getStart(), low) &&
                          near(withRms[static_cast<size_t>(x)].range.getEnd(), high),
                      "RMS and peaks match a direct per-channel, trimmed and faded sample scan");
            }
        }
        std::vector<carta::WaveformColumn> retained{{{-3, 4}, true}};
        int checks = 0;
        check(!carta::exactWaveformColumns(clip, 0, view, span, left, width, left, columns,
                                           retained, [&] { return ++checks > 3; }) &&
                  checks > 3 && retained.size() == 1 && near(retained[0].range.getStart(), -3),
              "cancelling a wide fade scan leaves prior results intact");
        check(!carta::exactWaveformColumns(clip, 2, view, span, left, width, left, columns,
                                           retained) &&
                  retained.size() == 1,
              "invalid waveform requests leave results intact");
        checks = 0;
        check(!carta::exactWaveformColumns(
                  clip, 0, view, span, left, width, left, columns, retained,
                  [&] { return ++checks > 3; }, true) &&
                  retained.size() == 1 && near(retained[0].range.getStart(), -3),
              "cancelling RMS calculation preserves the previous columns");
        const auto originalSample = audio->samples.getSample(0, 20000);
        audio->samples.setSample(0, 20000, std::numeric_limits<float>::quiet_NaN());
        check(!carta::exactWaveformColumns(clip, 0, view, span, left, width, left, columns,
                                           retained, {}, true) &&
                  retained.size() == 1 && near(retained[0].range.getStart(), -3),
              "non-finite source data cannot publish RMS columns");
        audio->samples.setSample(0, 20000, originalSample);
        clip.fadeIn = 0.5;
        clip.fadeInStart = 0.3;
        clip.fadeOut = 0;
        std::vector<carta::WaveformColumn> flatColumns;
        check(carta::exactWaveformColumns(clip, 0, view, span, left, width, left, columns,
                                          flatColumns),
              "calculate columns with flat envelope sections and cached peaks");
        std::vector<carta::WaveformColumn> flatRms;
        check(carta::exactWaveformColumns(clip, 0, view, span, left, width, left, columns, flatRms,
                                          {}, true),
              "calculate RMS using flat-envelope cached peaks");
        for (int x = 5; x < 14; ++x) {
            int lo = 0, hi = 0;
            const double start = static_cast<double>(x) * span / width,
                         end = static_cast<double>(x + 1) * span / width;
            if (!carta::waveformSampleRange(clip, start, end, lo, hi))
                continue;
            float low = std::numeric_limits<float>::infinity(), high = -low;
            double energy = 0;
            for (int i = lo; i < hi; ++i) {
                const float value =
                    audio->samples.getSample(0, i) * carta::clipLevel(clip, i / audio->sampleRate);
                low = std::min(low, value);
                high = std::max(high, value);
                energy += static_cast<double>(value) * value;
            }
            check(flatColumns[static_cast<size_t>(x)].hasSamples &&
                      near(flatColumns[static_cast<size_t>(x)].range.getStart(), low) &&
                      near(flatColumns[static_cast<size_t>(x)].range.getEnd(), high),
                  "flat cached columns match direct envelope-adjusted extrema");
            check(flatRms[static_cast<size_t>(x)].hasRms &&
                      std::abs(flatRms[static_cast<size_t>(x)].rms -
                               std::sqrt(energy / (hi - lo))) < 1e-6,
                  "flat-envelope RMS matches direct energy despite using cached peaks");
        }
    }
    {
        constexpr int frames = 12000;
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = 44100;
        audio->samples.setSize(2, frames);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < frames; ++i)
                audio->samples.setSample(
                    ch, i, static_cast<float>((i * 37 + ch * 17) % 201 - 100) / 100.0f);
        audio->samples.setSample(0, 11000, 2.0f);
        audio->samples.setSample(1, 100, -2.0f);
        carta::buildWaveformPeaks(*audio);
        carta::Clip clip{audio, 0, 0, frames / audio->sampleRate};
        clip.gain = 0.8f;
        clip.fadeIn = 10000 / audio->sampleRate;
        clip.fadeInStart = 0;
        clip.fadeOut = 8000 / audio->sampleRate;
        clip.fadeOutEnd = frames / audio->sampleRate;
        for (int ch = 0; ch < 2; ++ch)
            for (const auto range : {juce::Range<int>{0, 2000}, juce::Range<int>{3000, 7096},
                                     juce::Range<int>{0, frames}, juce::Range<int>{3500, 11001}}) {
                float low = std::numeric_limits<float>::infinity(), high = -low;
                for (int i = range.getStart(); i < range.getEnd(); ++i) {
                    const auto value = audio->samples.getSample(ch, i) *
                                       carta::clipLevel(clip, i / audio->sampleRate);
                    low = std::min(low, value);
                    high = std::max(high, value);
                }
                const auto displayed =
                    carta::clipWaveformRange(clip, ch, range.getStart(), range.getEnd());
                check(displayed.getStart() <= low + 1e-6f && displayed.getEnd() >= high - 1e-6f,
                      "faded waveform range never hides rendered source peaks");
                if (range.getLength() <= 4096)
                    check(near(displayed.getStart(), low) && near(displayed.getEnd(), high),
                          "short faded waveform range matches every envelope-adjusted sample");
            }
        clip.fadeIn = clip.fadeOut = 0;
        const auto scaled = carta::clipWaveformRange(clip, 0, 0, frames),
                   raw = carta::waveformRange(*audio, 0, 0, frames);
        check(near(scaled.getStart(), raw.getStart() * clip.gain) &&
                  near(scaled.getEnd(), raw.getEnd() * clip.gain),
              "constant-gain waveform uses exact cached extrema");
        check(carta::clipWaveformRange(clip, 2, 0, frames).isEmpty(),
              "invalid waveform channel is harmless");
        {
            auto faded = std::make_shared<carta::Audio>();
            faded->sampleRate = 44100;
            faded->samples.setSize(1, 5000);
            for (int i = 0; i < 5000; ++i)
                faded->samples.setSample(0, i, 0.5f);
            faded->samples.setSample(0, 0, 2.0f);
            carta::buildWaveformPeaks(*faded);
            carta::Clip fading{faded, 0, 0, 5000.0 / 44100};
            fading.fadeIn = 5000.0 / 44100;
            bool exact = true;
            const auto estimate = carta::clipWaveformRange(fading, 0, 0, 5000, &exact);
            check(!exact && estimate.getEnd() > 1,
                  "wide fade peak warning is identified as a conservative estimate");
            const auto shortRange = carta::clipWaveformRange(fading, 0, 0, 4096, &exact);
            check(exact && shortRange.getEnd() < 1,
                  "short fade peak warning uses exact envelope-adjusted samples");
            fading.fadeIn = 0;
            const auto constantRange = carta::clipWaveformRange(fading, 0, 0, 5000, &exact);
            check(exact && constantRange.getEnd() > 1,
                  "constant gain uses exact cached clipping extrema at wide zoom");
            fading.gain = 0.5f;
            const auto atFullScale = carta::clipWaveformRange(fading, 0, 0, 5000, &exact);
            check(exact && near(atFullScale.getEnd(), 1.0f),
                  "exactly 0 dBFS is distinguishable from an over-range peak");
        }
    }
    {
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = 48000;
        source->samples.setSize(1, 1000);
        source->samples.clear();
        source->samples.setSample(0, 255, 1.0f);
        source->samples.setSample(0, 264, 0.8f);
        source->samples.setSample(0, 555, -1.0f);
        carta::buildWaveformPeaks(*source);
        carta::Clip clip{source, 10, 255.5 / 48000.0, 299.5 / 48000.0};
        int first = -1, last = -1;
        check(carta::waveformSampleRange(clip, 10, 10 + 8.5 / 48000.0, first, last) &&
                  first == 256 && last == 264 &&
                  near(carta::waveformRange(*source, 0, first, last).getEnd(), 0),
              "trimmed waveform excludes samples before the clip and at the next pixel boundary");
        check(carta::waveformSampleRange(clip, 10 + 8.5 / 48000.0, 10 + 16.5 / 48000.0, first,
                                         last) &&
                  first == 264 && last == 272 &&
                  near(carta::waveformRange(*source, 0, first, last).getEnd(), 0.8f),
              "transient on a pixel boundary belongs to the following column");
        check(carta::waveformSampleRange(clip, 10 + 290 / 48000.0, 11, first, last) &&
                  first == 546 && last == 555 &&
                  near(carta::waveformRange(*source, 0, first, last).getStart(), 0),
              "trimmed waveform excludes samples at the exclusive source end");
        first = 12;
        last = 13;
        check(!carta::waveformSampleRange(clip, 9, 10, first, last) && first == 12 && last == 13,
              "empty waveform intersection leaves output unchanged");
        check(!carta::waveformSampleRange(clip, 10, 10 + 0.4 / 48000.0, first, last),
              "a partial pixel without source sample timestamps stays empty");
        check(
            carta::waveformPointRange(clip, 10 + 8.5 / 48000.0, 10 + 16.5 / 48000.0, first, last) &&
                first <= 264 && first >= 262 && last >= 272 && last <= 274,
            "sample-point view includes neighboring source points for line continuity");
        check(carta::waveformPointRange(clip, -1e20, 1e20, first, last) && first == 256 &&
                  last == 555,
              "extreme view bounds are safely clamped to the half-open clip trim");
        first = 12;
        last = 13;
        check(!carta::waveformPointRange(clip, -1e20, 9, first, last) && first == 12 &&
                  last == 13 && !carta::waveformPointRange(clip, 11, 1e20, first, last),
              "offscreen clip point queries preserve their output bounds");
        check(!carta::waveformPointRange(clip, 10, std::numeric_limits<double>::infinity(), first,
                                         last),
              "sample-point view rejects non-finite bounds");
        clip.audio.reset();
        check(!carta::waveformSampleRange(clip, 10, 11, first, last),
              "waveform mapping rejects missing media");
    }
    auto audio = std::make_shared<carta::Audio>();
    audio->sampleRate = 4;
    audio->samples.setSize(4, 4);
    for (int i = 0; i < 4; ++i)
        for (int ch = 0; ch < 4; ++ch)
            audio->samples.setSample(ch, i, static_cast<float>(i + ch));
    carta::Session session;
    session.tracks.push_back({"Four channels", {{audio, 1, 0, 1}}});
    juce::AudioBuffer<float> stereo(2, 8);
    stereo.clear();
    session.render(stereo, 0, 8, 0, 4, 2);
    check(near(stereo.getSample(0, 3), 0) && near(stereo.getSample(0, 4), 1) &&
              near(stereo.getSample(1, 4), 2) && near(stereo.getSample(0, 7), 4),
          "stereo timing and multichannel fold");
    juce::AudioBuffer<float> mono(1, 8);
    mono.clear();
    session.render(mono, 0, 8, 0, 4, 1);
    check(near(mono.getSample(0, 4), 1.5f) && near(mono.getSample(0, 7), 4.5f), "mono fold");
    {
        carta::Session mixed;
        mixed.tracks.push_back({"Left", {{audio, 0, 0, 1}}});
        mixed.tracks.push_back({"Right", {{audio, 0, 0, 1}}});
        mixed.tracks[0].gain = 0.5f;
        mixed.tracks[0].pan = -1.0f;
        mixed.tracks[1].pan = 1.0f;
        juce::AudioBuffer<float> out(2, 1);
        out.clear();
        mixed.render(out, 0, 1, 0, 4, 2);
        check(near(out.getSample(0, 0), 0.5f) && near(out.getSample(1, 0), 2.0f),
              "track gain and stereo balance");
        mixed.tracks[1].muted = true;
        out.clear();
        mixed.render(out, 0, 1, 0, 4, 2);
        check(near(out.getSample(0, 0), 0.5f) && near(out.getSample(1, 0), 0),
              "muted track excluded");
        mixed.tracks[1].muted = false;
        mixed.tracks[1].solo = true;
        out.clear();
        mixed.render(out, 0, 1, 0, 4, 2);
        check(near(out.getSample(0, 0), 0) && near(out.getSample(1, 0), 2),
              "solo excludes other tracks");
        mixed.tracks[1].muted = true;
        out.clear();
        mixed.render(out, 0, 1, 0, 4, 2);
        check(near(out.getSample(0, 0), 0) && near(out.getSample(1, 0), 0),
              "mute takes precedence over solo");
        mixed.tracks[1].muted = false;
        juce::AudioBuffer<float> soloMono(1, 1);
        soloMono.clear();
        mixed.render(soloMono, 0, 1, 0, 4, 1);
        check(near(soloMono.getSample(0, 0), 1), "mono output folds the same pan-adjusted mix");
    }
    {
        juce::AudioBuffer<float> audition(2, 8);
        audition.clear();
        carta::PlaybackRange preview;
        const auto sampleIs = [](double seconds, double sample) {
            return std::abs(seconds * carta::sessionRate - sample) < 1.0e-8;
        };
        check(carta::selectionPreviewRange(10, 15, 4, 3, 20, preview) &&
                  sampleIs(preview.start, 6) && sampleIs(preview.end, 18) && !preview.loop,
              "selection context includes the full range and both margins");
        check(carta::selectionPreviewRange(1, 19, 100, std::numeric_limits<juce::int64>::max(), 20,
                                           preview) &&
                  sampleIs(preview.start, 0) && sampleIs(preview.end, 20),
              "selection context clamps at master ends without overflow");
        check(!carta::selectionPreviewRange(10, 10, 1, 1, 20, preview) &&
                  !carta::selectionPreviewRange(10, 21, 1, 1, 20, preview) &&
                  !carta::selectionPreviewRange(10, 15, -1, 1, 20, preview),
              "selection context rejects invalid selections and margins");
        check(carta::boundaryPreviewRange(10, 4, 6, 20, preview) && sampleIs(preview.start, 6) &&
                  sampleIs(preview.end, 16) && !preview.loop,
              "boundary preview uses half-open sample positions");
        check(carta::boundaryPreviewRange(0, 100, 3, 20, preview) && sampleIs(preview.start, 0) &&
                  sampleIs(preview.end, 3),
              "boundary preview clamps at project start");
        check(carta::boundaryPreviewRange(20, 2, std::numeric_limits<juce::int64>::max(), 20,
                                          preview) &&
                  sampleIs(preview.start, 18) && sampleIs(preview.end, 20),
              "boundary preview clamps at project end without overflow");
        check(!carta::boundaryPreviewRange(0, 1, 0, 20, preview) &&
                  !carta::boundaryPreviewRange(21, 1, 1, 20, preview) &&
                  !carta::boundaryPreviewRange(2, -1, 1, 20, preview),
              "boundary preview rejects empty or invalid ranges");
        double position = 1.25;
        check(!carta::renderPlayback(session, audition, 1, 6, position, 4, 2, {1.25, 1.75, false}),
              "selection audition stops at end");
        check(near(audition.getSample(0, 0), 0) && near(audition.getSample(0, 1), 2) &&
                  near(audition.getSample(0, 2), 3) && near(audition.getSample(0, 3), 0) &&
                  near(static_cast<float>(position), 1.75f),
              "selection audition respects half-open bounds and buffer offset");
        audition.clear();
        position = 1.25;
        check(carta::renderPlayback(session, audition, 0, 6, position, 4, 2, {1.25, 1.75, true}),
              "loop remains active");
        for (int i = 0; i < 6; ++i)
            check(near(audition.getSample(0, i), i % 2 == 0 ? 2.0f : 3.0f),
                  "loop wraps without extra boundary sample");
        check(near(static_cast<float>(position), 1.25f), "loop wraps at block boundary");
        audition.clear();
        position = 1.25;
        check(carta::renderPlayback(session, audition, 0, 3, position, 6, 2, {1.25, 1.75, true}),
              "loop supports differing output rate");
        check(carta::renderPlayback(session, audition, 3, 3, position, 6, 2, {1.25, 1.75, true}),
              "loop continues across buffers");
        for (int i = 0; i < 3; ++i)
            check(near(audition.getSample(0, i), audition.getSample(0, i + 3)),
                  "loop is consistent across callback boundaries");
        juce::AudioBuffer<float> continuous(2, 31), chunked(2, 31);
        continuous.clear();
        chunked.clear();
        double firstPosition = 1.1, secondPosition = 1.1;
        check(carta::renderPlayback(session, continuous, 0, 31, firstPosition, 7, 2,
                                    {1.1, 1.5, true}),
              "non-integral loop renders");
        for (int i = 0; i < 31; i += 3)
            check(carta::renderPlayback(session, chunked, i, std::min(3, 31 - i), secondPosition, 7,
                                        2, {1.1, 1.5, true}),
                  "non-integral loop spans callbacks");
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 31; ++i)
                check(near(continuous.getSample(ch, i), chunked.getSample(ch, i)),
                      "loop output is independent of callback sizes");
        position = 0;
        check(!carta::renderPlayback(session, audition, 0, 1, position, 4, 2, {1, 1, true}),
              "invalid loop range stops");
        auto tiny = std::make_shared<carta::Audio>();
        tiny->sampleRate = carta::sessionRate;
        tiny->samples.setSize(1, 16);
        for (int i = 0; i < 16; ++i)
            tiny->samples.setSample(0, i, 0.25f);
        carta::Session nearEnd;
        nearEnd.tracks.push_back({"end", {{tiny, 0, 0, 10 / carta::sessionRate}}});
        juce::AudioBuffer<float> finalSample(1, 1);
        finalSample.clear();
        double justBefore = (10.0 - 0.001) / carta::sessionRate;
        check(!carta::renderPlayback(nearEnd, finalSample, 0, 1, justBefore, carta::sessionRate, 1,
                                     {0, 10 / carta::sessionRate, false}) &&
                  finalSample.getSample(0, 0) > 0.2f,
              "genuine fractional frame before endpoint is rendered");
        carta::PlaybackClock edgeClock;
        finalSample.clear();
        justBefore = (10.0 - 0.001) / carta::sessionRate;
        check(!carta::renderPlayback(nearEnd, finalSample, 0, 1, justBefore, carta::sessionRate, 1,
                                     {0, 10 / carta::sessionRate, false}, nullptr, &edgeClock) &&
                  finalSample.getSample(0, 0) > 0.2f,
              "retained clock renders a genuine fractional frame before the exclusive endpoint");
        audition.clear();
        position = 1.25;
        carta::PlaybackClock boundedClock;
        check(!carta::renderPlayback(session, audition, 1, 6, position, 4, 2, {1.25, 1.75, false},
                                     nullptr, &boundedClock) &&
                  near(audition.getSample(0, 0), 0) && near(audition.getSample(0, 1), 2) &&
                  near(audition.getSample(0, 2), 3) && near(audition.getSample(0, 3), 0) &&
                  near(static_cast<float>(position), 1.75f),
              "retained clock respects bounded playback and leaves remaining output untouched");

        juce::AudioBuffer<float> tinyLoop(1, 3);
        tinyLoop.clear();
        double tinyPosition = 0;
        check(carta::renderPlayback(nearEnd, tinyLoop, 0, 3, tinyPosition, carta::sessionRate, 1,
                                    {0, 1e-15, true}),
              "sub-frame loop remains finite and does not stall at its start");
    }
    {
        auto a = std::make_shared<carta::Audio>(), b = std::make_shared<carta::Audio>();
        a->sampleRate = b->sampleRate = carta::sessionRate;
        a->samples.setSize(1, 128);
        b->samples.setSize(1, 128);
        for (int i = 0; i < 128; ++i) {
            a->samples.setSample(0, i, 0.4f);
            b->samples.setSample(0, i, 0.2f);
        }
        carta::Session mixed;
        mixed.tracks.push_back(
            {"Overlap",
             {{a, 0, 0, 128 / carta::sessionRate}, {b, 0, 0, 128 / carta::sessionRate}}});
        mixed.tracks[0].gain = 0.75f;
        mixed.tracks[0].pan = 0.25f;
        check(carta::setClipEnvelope(mixed.tracks[0].clips[1], 0.5f, 20 / carta::sessionRate,
                                     20 / carta::sessionRate),
              "prepare isolated clip envelope");
        mixed.tracks.push_back({"Outside", {{a, 0, 0, 128 / carta::sessionRate}}});
        mixed.tracks[1].solo = true;
        auto reference = mixed;
        reference.tracks[0].clips.erase(reference.tracks[0].clips.begin());
        check(carta::limitToTracks(reference, 0, 0), "prepare isolated clip reference");
        for (int channels : {1, 2}) {
            juce::AudioBuffer<float> actual(2, 128), expected(2, 128);
            actual.clear();
            expected.clear();
            double position = 0;
            check(!carta::renderPlayback(mixed, actual, 0, 128, position, carta::sessionRate,
                                         channels, {0, 128 / carta::sessionRate, false, {0, 0}, 1}),
                  "isolated clip playback stops at clip end");
            reference.render(expected, 0, 128, 0, carta::sessionRate, channels);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 128; ++i)
                    check(near(actual.getSample(ch, i), expected.getSample(ch, i)),
                          "isolated clip excludes overlaps and other tracks while preserving gain, "
                          "pan and fades");
            check(actual.getSample(0, 50) > 0 && actual.getSample(0, 50) < 0.15f,
                  "isolated clip remains audible despite outside solo");
        }
        juce::AudioBuffer<float> looped(2, 256);
        looped.clear();
        double position = 0;
        check(carta::renderPlayback(mixed, looped, 0, 256, position, carta::sessionRate, 2,
                                    {0, 128 / carta::sessionRate, true, {0, 0}, 1}),
              "isolated clip loops");
        for (int i = 0; i < 128; ++i)
            check(near(looped.getSample(0, i), looped.getSample(0, i + 128)),
                  "isolated clip loop repeats the same samples");
        mixed.tracks[0].muted = true;
        looped.clear();
        position = 0;
        carta::renderPlayback(mixed, looped, 0, 128, position, carta::sessionRate, 2,
                              {0, 128 / carta::sessionRate, false, {0, 0}, 1});
        check(near(looped.getSample(0, 50), 0), "isolated clip respects track mute");
        looped.clear();
        position = 0;
        check(!carta::renderPlayback(mixed, looped, 0, 1, position, carta::sessionRate, 2,
                                     {0, 1, false, {0, 0}, 3}) &&
                  near(looped.getSample(0, 0), 0),
              "invalid clip index cannot fall back to the full mix");
        check(!carta::renderPlayback(mixed, looped, 0, 1, position, carta::sessionRate, 2,
                                     {0, 1, false, {}, 0}),
              "clip filter requires a single track");
    }
    {
        auto a = std::make_shared<carta::Audio>(), b = std::make_shared<carta::Audio>();
        a->sampleRate = b->sampleRate = carta::sessionRate;
        a->samples.setSize(1, 1000);
        b->samples.setSize(1, 1000);
        for (int i = 0; i < 1000; ++i) {
            a->samples.setSample(0, i, static_cast<float>(i) * 0.0001f);
            b->samples.setSample(0, i, static_cast<float>(i) * 0.0002f);
        }
        carta::Session original;
        original.tracks.push_back({"A", {{a, 0, 0, 1000 / carta::sessionRate}}});
        original.tracks.push_back({"B", {{b, 0, 0, 1000 / carta::sessionRate}}});
        const carta::TimeSelection selection{300, 500, 0, 0};
        for (const bool all : {false, true}) {
            carta::Session preview, applied = original;
            carta::PlaybackRange range;
            juce::String error;
            check(carta::prepareRipplePreview(original, selection, all, 100, 150, preview, range,
                                              error),
                  "prepare ripple preview");
            check(carta::rippleRemoveRange(applied, selection, all, &error),
                  "apply matching ripple edit");
            check(std::abs(range.start * carta::sessionRate - 200) < 1e-8 &&
                      std::abs(range.end * carta::sessionRate - 450) < 1e-8,
                  "preview uses configured sample context");
            juce::AudioBuffer<float> actual(2, 250), expected(2, 250);
            actual.clear();
            expected.clear();
            double pos = range.start;
            check(
                !carta::renderPlayback(preview, actual, 0, 250, pos, carta::sessionRate, 2, range),
                "preview stops at end");
            applied.render(expected, 0, 250, range.start, carta::sessionRate, 2);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 250; ++i)
                    check(near(actual.getSample(ch, i), expected.getSample(ch, i)),
                          "preview matches actual ripple rendering");
            check(original.tracks[0].clips.size() == 1 && original.tracks[1].clips.size() == 1 &&
                      std::abs(original.tracks[0].clips[0].duration * carta::sessionRate - 1000) <
                          1e-8,
                  "preview leaves original session unchanged");
            juce::AudioBuffer<float> joined(2, 1);
            joined.clear();
            preview.render(joined, 0, 1, 350 / carta::sessionRate, carta::sessionRate, 2);
            const float wanted = 0.055f + (all ? 0.11f : 0.07f);
            check(near(joined.getSample(0, 0), wanted),
                  "preview respects selected versus all-track ripple scope");
        }
        carta::Session conflicting = original;
        conflicting.markers = {{300 / carta::sessionRate, "A"}, {400 / carta::sessionRate, "B"}};
        carta::Session output = original;
        carta::PlaybackRange range{1, 2, true};
        juce::String error;
        check(!carta::prepareRipplePreview(conflicting, selection, true, 100, 100, output, range,
                                           error) &&
                  error.contains("same sample"),
              "preview reports marker collision");
        check(output.tracks[0].clips.size() == 1 && std::abs(range.start - 1) < 1e-8 && range.loop,
              "failed preview preserves both outputs");
        check(!carta::prepareRipplePreview(original, {0, 1000, 0, 1}, true, 100, 100, output, range,
                                           error) &&
                  error.contains("No audio time"),
              "preview reports empty resulting arrangement");
    }
    {
        check(carta::formatProjectTime(0) == "00:00.000" &&
                  carta::formatProjectTime(44) == "00:00.000" &&
                  carta::formatProjectTime(441) == "00:00.010",
              "sample time truncates milliseconds consistently");
        check(carta::formatProjectTime(44100 * 61LL + 22050) == "01:01.500" &&
                  carta::formatProjectTime(44100 * 3600LL) == "01:00:00.000",
              "sample time handles minutes and hours");
        check(carta::formatProjectTime(-1) == "00:00.000" &&
                  carta::formatProjectTime(44100 * 360000LL) == "100:00:00.000",
              "sample time handles invalid negative and long positions");
        using TimeFormat = carta::TimelineTimeFormat;
        check(carta::formatTimelineTime(44101, TimeFormat::samples) == "44101 smp" &&
                  carta::formatTimelineTime(44101, TimeFormat::seconds) == "1.000023 s" &&
                  carta::formatTimelineTime(44101, TimeFormat::clock) == "00:01.000023",
              "timeline units describe one project sample with sufficient precision");
        check(carta::formatTimelineTime(44100 * 61LL + 22050, TimeFormat::clock) ==
                      "01:01.500000" &&
                  carta::formatTimelineTime(44100 * 360000LL, TimeFormat::seconds) ==
                      "360000.000000 s",
              "timeline time formats handle minutes and long durations");
        check(carta::formatRulerTick(44101, 1, TimeFormat::clock) == "00:01.000023" &&
                  carta::formatRulerTick(44101, 1, TimeFormat::seconds) == "1.000023 s" &&
                  carta::formatRulerTick(44101, 1, TimeFormat::samples) == "44101 smp",
              "high-zoom ruler uses distinct sample, clock and seconds labels");
        check(carta::formatRulerTick(441, 441, TimeFormat::seconds) == "0.010 s" &&
                  carta::formatRulerTick(88200, 44100, TimeFormat::seconds) == "2 s" &&
                  carta::formatRulerTick(88200, 44100, TimeFormat::clock) == "00:02",
              "wide ruler labels use appropriate precision");
        for (juce::int64 n = 0; n < 44100; ++n) {
            const auto clockText = carta::formatTimelineTime(n, TimeFormat::clock),
                       secondsText = carta::formatTimelineTime(n, TimeFormat::seconds),
                       samplesText = carta::formatTimelineTime(n, TimeFormat::samples);
            juce::int64 parsed = -1;
            check(clockText != carta::formatTimelineTime(n + 1, TimeFormat::clock) &&
                      secondsText != carta::formatTimelineTime(n + 1, TimeFormat::seconds),
                  "time readouts distinguish adjacent project samples");
            check(carta::parseSampleOrTime(clockText, parsed) && parsed == n &&
                      carta::parseSampleOrTime(secondsText, parsed) && parsed == n &&
                      carta::parseSampleOrTime(samplesText, parsed) && parsed == n,
                  "all time readout formats round-trip to the original sample");
        }
        const auto secondsTicks = carta::makeRulerTicks(0, 10, 1000);
        check(secondsTicks.interval == 88200 && secondsTicks.samples.size() == 6 &&
                  secondsTicks.samples[1] == 88200 &&
                  carta::formatRulerTick(secondsTicks.samples[1], secondsTicks.interval) == "00:02",
              "ruler uses spaced whole-second ticks");
        const auto sampleTicks = carta::makeRulerTicks(0.5, 0.001, 1000);
        check(sampleTicks.interval == 5 && !sampleTicks.samples.empty() &&
                  sampleTicks.samples.front() % 5 == 0 &&
                  carta::formatRulerTick(sampleTicks.samples.front(), sampleTicks.interval)
                      .endsWith(" smp"),
              "ruler uses exact sample ticks at high zoom");
        const auto longTicks = carta::makeRulerTicks(360000, 600, 1000);
        check(!longTicks.samples.empty() &&
                  carta::formatRulerTick(longTicks.samples.front(), longTicks.interval)
                      .startsWith("100:"),
              "ruler labels long sessions without integer overflow");
        check(carta::makeRulerTicks(0, 0, 100).samples.empty() &&
                  carta::makeRulerTicks(0, 1, 0).samples.empty() &&
                  carta::makeRulerTicks(std::numeric_limits<double>::infinity(), 1, 100)
                      .samples.empty(),
              "ruler rejects invalid viewports");
        check(std::abs(carta::followPlayheadViewport(10, 10, 100, 18) - 10) < 1e-9,
              "follow keeps the viewport still inside its safe area");
        check(std::abs(carta::followPlayheadViewport(10, 10, 100, 19) - 18) < 1e-9,
              "follow pages at the right edge");
        check(std::abs(carta::followPlayheadViewport(10, 10, 100, 2) - 1) < 1e-9,
              "follow reveals a backward seek or loop");
        check(std::abs(carta::followPlayheadViewport(10, 10, 100, 18, true) - 13) < 1e-9 &&
                  std::abs(carta::followPlayheadViewport(13, 10, 100, 18.25, true) - 13.25) < 1e-9,
              "pinned follow moves continuously with the playhead");
        check(std::abs(carta::followPlayheadViewport(13, 10, 100, 2, true)) < 1e-9 &&
                  std::abs(carta::followPlayheadViewport(13, 10, 100, 99, true) - 90) < 1e-9,
              "pinned follow clamps backward loops and timeline ends");
        check(std::abs(carta::followPlayheadViewport(
                           10, 10, 100, std::numeric_limits<double>::quiet_NaN(), true) -
                       10) < 1e-9,
              "pinned follow ignores invalid positions");
        check(std::abs(carta::followPlayheadViewport(80, 10, 100, 99) - 90) < 1e-9 &&
                  std::abs(carta::followPlayheadViewport(10, 10, 100, 0)) < 1e-9,
              "follow clamps at timeline edges");
        check(std::abs(carta::followPlayheadViewport(10, 0, 100, 20) - 10) < 1e-9 &&
                  std::abs(carta::followPlayheadViewport(10, 10, 100,
                                                         std::numeric_limits<double>::quiet_NaN()) -
                           10) < 1e-9,
              "follow ignores invalid viewport input");
        check(std::abs(carta::selectionEdgeScroll(10, 10, 100, 100, 900, 500, 0.05) - 10) < 1e-9,
              "selection edge scroll stays still in viewport center");
        check(
            std::abs(carta::selectionEdgeScroll(10, 10, 100, 100, 900, 900, 0.05) - 10.3) < 1e-9 &&
                std::abs(carta::selectionEdgeScroll(10, 10, 100, 100, 900, 100, 0.05) - 9.7) < 1e-9,
            "selection edge scroll advances in either direction");
        check(std::abs(carta::selectionEdgeScroll(10, 10, 100, 100, 900, 888, 0.05) - 10.15) < 1e-9,
              "selection edge scroll slows near the edge zone");
        check(std::abs(carta::selectionEdgeScroll(0, 10, 100, 100, 900, -100, 0.05)) < 1e-9 &&
                  std::abs(carta::selectionEdgeScroll(90, 10, 100, 100, 900, 1000, 0.05) - 90) <
                      1e-9,
              "selection edge scroll clamps to timeline bounds");
        check(std::abs(carta::selectionEdgeScroll(10, 10, 100, 100, 900, 900, 2) - 10.6) < 1e-9 &&
                  std::abs(carta::selectionEdgeScroll(10, 10, 100, 100, 900, 900, 0) - 10) < 1e-9,
              "selection edge scroll bounds timer delays and ignores zero time");
        check(std::abs(carta::clipDragDelta(500, 500, 100, 900, 10, 10, 10)) < 1e-9,
              "clip drag has no displacement at its anchor");
        check(std::abs(carta::clipDragDelta(900, 500, 100, 900, 15, 10, 10) - 10) < 1e-9 &&
                  std::abs(carta::clipDragDelta(10000, 500, 100, 900, 15, 10, 10) - 10) < 1e-9,
              "clip drag includes viewport scroll and clamps outside pointer positions");
        check(std::abs(carta::clipDragDelta(100, 500, 100, 900, 5, 10, 10) + 10) < 1e-9,
              "clip drag remains anchored while scrolling left");
        check(std::abs(carta::clipDragDelta(500, 500, 100, 100, 10, 10, 10)) < 1e-9,
              "clip drag ignores an empty viewport");
        check(carta::selectionTrackScrollDirection(100, 500, 99, 2, 6, 700) == -1 &&
                  carta::selectionTrackScrollDirection(100, 500, 501, 2, 6, 700) == 1,
              "selection track scrolling follows either vertical edge");
        check(carta::selectionTrackScrollDirection(100, 500, 300, 2, 6, 700) == 0 &&
                  carta::selectionTrackScrollDirection(100, 500, 99, 0, 6, 700) == 0,
              "selection track scrolling rests in the center and at the first row");
        check(carta::selectionTrackScrollDirection(100, 500, 501, 2, 6, 500) == 0 &&
                  carta::selectionTrackScrollDirection(100, 500, 501, 5, 6, 700) == 0,
              "selection track scrolling stops when no further row can be revealed");
        check(carta::selectionTrackScrollDirection(100, 100, 100, 0, 1, 100) == 0 &&
                  carta::selectionTrackScrollDirection(100, 500, 200, 0, 0, 100) == 0,
              "selection track scrolling rejects an empty viewport or session");
    }
    {
        carta::Session layout;
        layout.tracks.resize(3);
        bool allVisible = false;
        layout.tracks[0].height = 100;
        layout.tracks[1].height = 200;
        layout.tracks[2].height = 150;
        layout.tracks[1].collapsed = true;
        check(carta::visibleTrackRows(layout, 0, 0).isEmpty() &&
                  carta::visibleTrackRows(layout, -1, 100).isEmpty() &&
                  carta::visibleTrackRows(layout, 3, 100).isEmpty(),
              "visible track range rejects empty or invalid viewports");
        check(carta::visibleTrackRows(layout, 0, 100) == juce::Range<int>(0, 1) &&
                  carta::visibleTrackRows(layout, 0, 101) == juce::Range<int>(0, 2),
              "visible track range includes partial rows but not rows beyond the viewport");
        check(carta::visibleTrackRows(layout, 1, 40) == juce::Range<int>(1, 2) &&
                  carta::visibleTrackRows(layout, 1, 41) == juce::Range<int>(1, 3),
              "visible track range uses collapsed heights and scrolled start");
        layout.tracks.clear();
        layout.tracks.resize(3);
        carta::EditorState state;
        state.session = layout;
        auto before = state;
        carta::EditHistory history;
        check(carta::fitTrackHeights(state.session, 301, allVisible) && allVisible &&
                  state.session.tracks[0].height == 101 && state.session.tracks[1].height == 100 &&
                  state.session.tracks[2].height == 100,
              "fit distributes available pixels across tracks");
        check(!carta::fitTrackHeights(state.session, 301, allVisible) && allVisible,
              "fit with unchanged heights does not create an edit");
        history.record(before);
        check(history.undo(state) && state.session.tracks[0].height == 116 && history.redo(state) &&
                  state.session.tracks[0].height == 101,
              "fit track heights is undoable");
        check(carta::fitTrackHeights(state.session, 100, allVisible) && !allVisible &&
                  state.session.tracks[2].height == 72,
              "fit reports when minimum heights exceed viewport");
        check(carta::fitTrackHeights(state.session, 10000, allVisible) && allVisible &&
                  state.session.tracks[0].height == 800,
              "fit respects maximum height");
        state.session.tracks[1].collapsed = true;
        state.session.tracks[1].height = 200;
        check(carta::fitTrackHeights(state.session, 340, allVisible) && allVisible &&
                  state.session.tracks[0].height == 150 && state.session.tracks[2].height == 150 &&
                  state.session.tracks[1].height == 200 &&
                  carta::trackDisplayHeight(state.session.tracks[1]) == 40,
              "fit preserves collapsed height and divides remaining space");
        check(carta::fitTrackHeights(state.session, 160, allVisible) && !allVisible,
              "fit accounts for collapsed rows when reporting overflow");
        for (auto& track : state.session.tracks)
            track.collapsed = true;
        check(!carta::fitTrackHeights(state.session, 120, allVisible) && allVisible &&
                  state.session.tracks[1].height == 200,
              "fitting all collapsed rows preserves expansion heights");
        auto expanded = state;
        expanded.session.tracks[1].collapsed = false;
        carta::EditHistory collapseHistory;
        collapseHistory.record(expanded);
        check(collapseHistory.undo(state) && !state.session.tracks[1].collapsed &&
                  state.session.tracks[1].height == 200 && collapseHistory.redo(state) &&
                  state.session.tracks[1].collapsed && state.session.tracks[1].height == 200,
              "collapse is undoable and retains the expanded height");
        layout.tracks.clear();
        check(!carta::fitTrackHeights(layout, 0, allVisible) && allVisible,
              "fitting an empty session is harmless");
    }
    {
        const double start = 4800.0, duration = 16.0 / carta::sessionRate;
        check(carta::timelineTimeToPixel(start, start, duration, 200, 800) == 200 &&
                  carta::timelineTimeToPixel(start + duration, start, duration, 200, 800) == 1000,
              "deep zoom maps viewport endpoints consistently");
        const int before = carta::timelineTimeToPixel(0, start, duration, 200, 800),
                  after = carta::timelineTimeToPixel(9600, start, duration, 200, 800);
        check(before == -99999800 && after == 100000200 &&
                  static_cast<juce::int64>(after) - before == 200000000,
              "remote clip edges saturate without integer overflow at full-disc positions");
        check(carta::timelineTimeToPixel(std::numeric_limits<double>::quiet_NaN(), start, duration,
                                         200, 800) == 200 &&
                  carta::timelineTimeToPixel(start, start, 0, 200, 800) == 200 &&
                  carta::timelineTimeToPixel(start, start, duration, 200, 0) == 200,
              "invalid timeline coordinates and empty viewports safely return the left edge");
        check(carta::timelineTimeToPixel(std::numeric_limits<double>::max(), 0, duration,
                                         std::numeric_limits<int>::max() - 5,
                                         800) == std::numeric_limits<int>::max(),
              "pixel origin addition saturates at integer bounds");
    }
    {
        const double rate = carta::sessionRate;
        auto markerAudio = std::make_shared<carta::Audio>();
        markerAudio->sampleRate = rate;
        markerAudio->samples.setSize(1, 500);
        markerAudio->samples.clear();
        carta::EditorState state;
        state.session.tracks.push_back(
            {"First", {{markerAudio, 50.25 / rate, 0, 20 / rate}, {markerAudio, 0, 0, 20 / rate}}});
        state.session.tracks.push_back({"Second", {{markerAudio, 100.1 / rate, 0, 20 / rate}}});
        state.session.tracks[0].clips[0].name = "Middle";
        state.session.tracks[0].clips[1].name = "Start";
        state.session.tracks[1].clips[0].name = juce::String::fromUTF8(u8"終わり");
        state.session.markers = {{30 / rate, "Existing"}};
        const auto original = state;
        juce::String error;
        const std::vector<carta::ClipReference> refs{{1, 0}, {0, 0}, {0, 1}};
        carta::EditHistory markerHistory;
        markerHistory.record(state, "Add clip-start markers");
        check(carta::addMarkersAtClipStarts(state.session, refs, error) &&
                  state.session.markers.size() == 4,
              "add markers for selected clips across tracks");
        check(state.session.markers[0].name == "Existing" &&
                  state.session.markers[1].name == "Start" &&
                  state.session.markers[2].name == "Middle" &&
                  state.session.markers[3].name == juce::String::fromUTF8(u8"終わり") &&
                  std::llround(state.session.markers[1].position * rate) == 0 &&
                  std::llround(state.session.markers[2].position * rate) == 51 &&
                  std::llround(state.session.markers[3].position * rate) == 101,
              "clip markers retain existing entries, sort additions and use the first contained "
              "project sample and clip name");
        check(markerHistory.undo(state) && state.session.markers.size() == 1 &&
                  markerHistory.redo(state) && state.session.markers.size() == 4,
              "clip-start marker edit supports undo and redo");
        const auto beforeFailure = state.session.markers;
        check(!carta::addMarkersAtClipStarts(state.session, refs, error) &&
                  error.contains("coincides") &&
                  state.session.markers.size() == beforeFailure.size(),
              "existing marker collisions leave all markers unchanged");
        auto duplicate = original.session;
        duplicate.tracks[1].clips[0].start = 50.25 / rate;
        duplicate.minimumEndSample = 100;
        check(carta::addMarkersAtClipStarts(duplicate, refs, error) &&
                  duplicate.markers.size() == 3 && duplicate.markers[2].name == "Middle" &&
                  std::llround(duplicate.markers[2].position * rate) == 51,
              "coincident selected clip starts create one marker named after the topmost track "
              "regardless of selection order");
        auto duplicateEnd = original.session;
        duplicateEnd.tracks[1].clips[0].start = 50.25 / rate;
        duplicateEnd.minimumEndSample = 100;
        check(carta::addMarkersAtClipEnds(duplicateEnd, {{1, 0}, {0, 0}}, error) &&
                  duplicateEnd.markers.size() == 2 &&
                  duplicateEnd.markers[1].name == "After Middle" &&
                  std::llround(duplicateEnd.markers[1].position * rate) == 71,
              "coincident selected clip ends create one deterministic marker");
        auto tiny = original.session;
        tiny.tracks[0].clips[0].start = 50.1 / rate;
        tiny.tracks[0].clips[0].duration = 0.2 / rate;
        check(!carta::addMarkersAtClipStarts(tiny, {{0, 0}}, error) && tiny.markers.size() == 1,
              "clip with no project sample cannot create a marker");
        check(!carta::addMarkersAtClipStarts(tiny, {{0, 1}, {0, 1}}, error) &&
                  tiny.markers.size() == 1,
              "duplicate clip references reject the edit atomically");
        carta::EditorState ending = original;
        carta::EditHistory endingHistory;
        endingHistory.record(ending, "Add clip-end markers");
        check(carta::addMarkersAtClipEnds(ending.session, {{0, 0}, {0, 1}}, error) &&
                  ending.session.markers.size() == 3 &&
                  std::llround(ending.session.markers[1].position * rate) == 20 &&
                  std::llround(ending.session.markers[2].position * rate) == 71 &&
                  ending.session.markers[1].name == "After Start" &&
                  ending.session.markers[2].name == "After Middle",
              "clip-end markers use exclusive sample boundaries, retain names and sort additions");
        check(endingHistory.undo(ending) && ending.session.markers.size() == 1 &&
                  endingHistory.redo(ending) && ending.session.markers.size() == 3,
              "clip-end markers support undo and redo");
        auto atMaster = original.session;
        check(!carta::addMarkersAtClipEnds(atMaster, refs, error) && error.contains("master end") &&
                  atMaster.markers.size() == 1,
              "a clip ending at the master end rejects the entire marker batch");
        auto collision = original.session;
        collision.markers.push_back({71 / rate, "occupied"});
        check(!carta::addMarkersAtClipEnds(collision, {{0, 1}, {0, 0}}, error) &&
                  error.contains("coincides") && collision.markers.size() == 2,
              "a clip-end marker collision rejects the entire batch");
    }
    {
        const double rate = carta::sessionRate;
        const juce::int64 base = 4799LL * 44100;
        auto alignAudio = std::make_shared<carta::Audio>();
        alignAudio->sampleRate = 48000;
        alignAudio->samples.setSize(1, 48000);
        for (int i = 0; i < 48000; ++i)
            alignAudio->samples.setSample(0, i, 0.2f * std::sin(static_cast<float>(i) * 0.071f));
        carta::Session original;
        original.tracks.push_back(
            {"A", {{alignAudio, static_cast<double>(base) / rate, 0.1, 0.6}}});
        original.tracks.push_back(
            {"B", {{alignAudio, (static_cast<double>(base) + 0.25) / rate, 0.1, 0.5}}});
        const std::vector<carta::ClipReference> refs{{0, 0}, {1, 0}};
        juce::String error;
        auto shifted = original;
        const double shiftTarget = (static_cast<double>(base) + 1000.5) / rate;
        check(carta::alignClipGroupEdgeToTime(shifted, refs, {0, 0}, false, shiftTarget, error) &&
                  std::abs((shifted.tracks[1].clips[0].start - shifted.tracks[0].clips[0].start) *
                               rate -
                           0.25) < 1e-4,
              "fractional group alignment retains relative positioning near 80 minutes");
        juce::AudioBuffer<float> beforeShift(1, 512), afterShift(1, 512);
        beforeShift.clear();
        afterShift.clear();
        original.render(beforeShift, 0, 512, (static_cast<double>(base) + 2000) / rate, rate, 1);
        shifted.render(afterShift, 0, 512, (static_cast<double>(base) + 3000.5) / rate, rate, 1);
        for (int i = 0; i < 512; ++i)
            check(std::abs(beforeShift.getSample(0, i) - afterShift.getSample(0, i)) < 1e-6f,
                  "fractional group alignment preserves the shifted mixed-rate audio");
        auto startTrim = original;
        const auto start = base + 8820;
        check(carta::trimClipGroupEdgesToSample(startTrim, refs, false, start, error),
              "align mixed-grid group starts to an exact sample");
        check(std::abs(startTrim.tracks[0].clips[0].start * rate - start) < 1e-6 &&
                  std::abs(startTrim.tracks[1].clips[0].start * rate - start) < 1e-6 &&
                  std::abs(
                      (startTrim.tracks[1].clips[0].start + startTrim.tracks[1].clips[0].duration) -
                      (original.tracks[1].clips[0].start + original.tracks[1].clips[0].duration)) <
                      1e-10,
              "group start alignment preserves both exclusive ends");
        juce::AudioBuffer<float> before(1, 1024), after(1, 1024);
        before.clear();
        after.clear();
        original.render(before, 0, 1024, static_cast<double>(base + 9000) / rate, rate, 1);
        startTrim.render(after, 0, 1024, static_cast<double>(base + 9000) / rate, rate, 1);
        for (int i = 0; i < 1024; ++i)
            check(std::abs(before.getSample(0, i) - after.getSample(0, i)) < 1e-6f,
                  "start alignment preserves the mixed-rate retained audio");
        auto endTrim = original;
        const auto end = base + 13230;
        check(
            carta::trimClipGroupEdgesToSample(endTrim, refs, true, end, error) &&
                std::abs((endTrim.tracks[0].clips[0].start + endTrim.tracks[0].clips[0].duration) *
                             rate -
                         end) < 1e-6 &&
                std::abs((endTrim.tracks[1].clips[0].start + endTrim.tracks[1].clips[0].duration) *
                             rate -
                         end) < 1e-6,
            "align mixed-grid group ends to an exact sample");
        before.clear();
        after.clear();
        original.render(before, 0, 1024, static_cast<double>(base + 1000) / rate, rate, 1);
        endTrim.render(after, 0, 1024, static_cast<double>(base + 1000) / rate, rate, 1);
        for (int i = 0; i < 1024; ++i)
            check(std::abs(before.getSample(0, i) - after.getSample(0, i)) < 1e-6f,
                  "end alignment preserves the retained mixed-rate audio");
        auto rejected = original;
        check(!carta::trimClipGroupEdgesToSample(rejected, refs, false, base + 40000, error) &&
                  std::memcmp(&rejected.tracks[0].clips[0].duration,
                              &original.tracks[0].clips[0].duration, sizeof(double)) == 0 &&
                  std::memcmp(&rejected.tracks[1].clips[0].start,
                              &original.tracks[1].clips[0].start, sizeof(double)) == 0,
              "invalid group alignment leaves every clip unchanged");
        check(!carta::trimClipGroupEdgesToSample(startTrim, refs, false, start, error) &&
                  error.contains("already"),
              "already aligned group is a no-op");
        auto relative = original;
        check(carta::trimClipGroupToAnchorRange(relative, refs, {0, 0}, true, base + 8820, true,
                                                base + 22050, error),
              "trim a mixed-grid group relative to an exact anchor range near the end of a disc");
        const auto& first = relative.tracks[0].clips[0];
        const auto& second = relative.tracks[1].clips[0];
        check(std::abs(first.start * rate - (base + 8820)) < 1e-6 &&
                  std::abs((first.start + first.duration) * rate - (base + 22050)) < 1e-6 &&
                  std::abs((second.start - first.start) * rate - 0.25) < 1e-4 &&
                  std::abs((second.duration - first.duration + 0.1) * rate) < 1e-4,
              "anchor trim keeps fractional relative positions and applies shared edge movements");
        before.clear();
        after.clear();
        original.render(before, 0, 1024, static_cast<double>(base + 9000) / rate, rate, 1);
        relative.render(after, 0, 1024, static_cast<double>(base + 9000) / rate, rate, 1);
        for (int i = 0; i < 1024; ++i)
            check(std::abs(before.getSample(0, i) - after.getSample(0, i)) < 1e-6f,
                  "relative group trim retains mixed-rate audio");
        auto keepStart = original;
        check(carta::trimClipGroupToAnchorRange(keepStart, refs, {0, 0}, false, 0, true,
                                                base + 22050, error) &&
                  std::memcmp(&keepStart.tracks[1].clips[0].start,
                              &original.tracks[1].clips[0].start, sizeof(double)) == 0 &&
                  std::memcmp(&keepStart.tracks[1].clips[0].offset,
                              &original.tracks[1].clips[0].offset, sizeof(double)) == 0,
              "an unchanged start field preserves a fractional edge and source offset exactly");
        auto nearGrid = original;
        nearGrid.tracks[0].clips[0].start = std::nextafter(nearGrid.tracks[0].clips[0].start,
                                                           std::numeric_limits<double>::infinity());
        const double exactStart = nearGrid.tracks[0].clips[0].start,
                     exactOffset = nearGrid.tracks[0].clips[0].offset;
        check(
            carta::trimClipGroupToAnchorRange(nearGrid, refs, {0, 0}, false, 0, true, base + 22050,
                                              error) &&
                std::memcmp(&nearGrid.tracks[0].clips[0].start, &exactStart, sizeof(double)) == 0 &&
                std::memcmp(&nearGrid.tracks[0].clips[0].offset, &exactOffset, sizeof(double)) == 0,
            "an unchanged near-grid start is not rounded by a group end trim");
        auto markerBound = original;
        markerBound.markers.push_back({static_cast<double>(base + 24000) / rate, "End marker"});
        check(!carta::trimClipGroupToAnchorRange(markerBound, refs, {0, 0}, true, base + 8820, true,
                                                 base + 22050, error) &&
                  std::memcmp(&markerBound.tracks[0].clips[0].duration,
                              &original.tracks[0].clips[0].duration, sizeof(double)) == 0,
              "group range trim rejects orphaned markers atomically");
        check(!carta::trimClipGroupToAnchorRange(rejected, refs, {0, 0}, true, base + 40000, true,
                                                 base + 50000, error) &&
                  std::memcmp(&rejected.tracks[1].clips[0].start,
                              &original.tracks[1].clips[0].start, sizeof(double)) == 0,
              "out-of-source group range trim is atomic");
        check(!carta::trimClipGroupToAnchorRange(rejected, refs, {0, 1}, true, base + 1000, false,
                                                 0, error),
              "group trim rejects an invalid anchor");
    }
    {
        const double rate = carta::sessionRate;
        auto trimAudio = std::make_shared<carta::Audio>();
        trimAudio->sampleRate = rate;
        trimAudio->samples.setSize(1, 20 * 44100);
        trimAudio->samples.clear();
        carta::Session original;
        original.tracks.push_back({"A", {{trimAudio, 2, 2, 6}}});
        original.tracks.push_back({"B", {{trimAudio, 3, 3, 6}}});
        original.tracks.push_back({"Standalone", {{trimAudio, 4, 4, 6}}});
        juce::String error;
        check(carta::groupClips(original, {{0, 0}, {1, 0}}, error),
              "prepare grouped and standalone playhead trims");
        const std::vector<carta::ClipReference> partial{{0, 0}, {2, 0}};
        auto focused = original;
        check(carta::trimClipUnitsToSample(focused, partial, {1, 0}, false, 5 * 44100, error) &&
                  std::abs(focused.tracks[0].clips[0].start - 4) < 1e-12 &&
                  std::abs(focused.tracks[1].clips[0].start - 5) < 1e-12 &&
                  std::abs(focused.tracks[2].clips[0].start - 5) < 1e-12,
              "playhead start trim uses the focused group member and independently trims "
              "standalone clips");
        check(
            std::abs(focused.tracks[0].clips[0].offset - 4) < 1e-12 &&
                std::abs((focused.tracks[1].clips[0].start + focused.tracks[1].clips[0].duration) -
                         9) < 1e-12,
            "playhead group trim preserves source alignment and the opposite edges");
        auto outer = original;
        check(carta::trimClipUnitsToSample(outer, partial, {2, 0}, false, 5 * 44100, error) &&
                  std::abs(outer.tracks[0].clips[0].start - 5) < 1e-12 &&
                  std::abs(outer.tracks[1].clips[0].start - 6) < 1e-12 &&
                  std::abs(outer.tracks[2].clips[0].start - 5) < 1e-12,
              "a nonfocused group uses its earliest start as the playhead anchor");
        auto ends = original;
        check(carta::trimClipUnitsToSample(ends, partial, {0, 0}, true, 7 * 44100, error) &&
                  std::abs((ends.tracks[0].clips[0].start + ends.tracks[0].clips[0].duration) - 7) <
                      1e-12 &&
                  std::abs((ends.tracks[1].clips[0].start + ends.tracks[1].clips[0].duration) - 8) <
                      1e-12 &&
                  std::abs((ends.tracks[2].clips[0].start + ends.tracks[2].clips[0].duration) - 7) <
                      1e-12,
              "playhead end trim preserves the relative ends within the focused group");
        auto outerEnd = original;
        check(carta::trimClipUnitsToSample(outerEnd, partial, {2, 0}, true, 7 * 44100, error) &&
                  std::abs(
                      (outerEnd.tracks[0].clips[0].start + outerEnd.tracks[0].clips[0].duration) -
                      6) < 1e-12 &&
                  std::abs(
                      (outerEnd.tracks[1].clips[0].start + outerEnd.tracks[1].clips[0].duration) -
                      7) < 1e-12,
              "a nonfocused group uses its latest end as the playhead anchor");
        auto unchanged = original;
        check(!carta::trimClipUnitsToSample(unchanged, {{0, 0}}, {0, 0}, false, 2 * 44100, error) &&
                  std::abs(unchanged.tracks[1].clips[0].start - 3) < 1e-12,
              "trimming a group to its existing anchor is a no-op");
        auto invalid = original;
        check(!carta::trimClipUnitsToSample(invalid, partial, {1, 0}, false, 10 * 44100, error) &&
                  std::abs(invalid.tracks[0].clips[0].start - 2) < 1e-12 &&
                  std::abs(invalid.tracks[2].clips[0].start - 4) < 1e-12,
              "invalid playhead trim rejects all units without partial changes");
        auto marker = original;
        marker.markers.push_back({9.5, "End"});
        check(!carta::trimClipUnitsToSample(marker, partial, {0, 0}, true, 7 * 44100, error) &&
                  error.contains("marker") &&
                  std::abs(marker.tracks[2].clips[0].duration - 6) < 1e-12,
              "playhead trim rejects an orphaned marker atomically");
        carta::Session combined;
        combined.tracks.push_back({"Standalone", {{trimAudio, 0, 0, 10}}});
        combined.tracks.push_back({"Anchor", {{trimAudio, 0, 0, 8}}});
        combined.tracks.push_back({"Partner", {{trimAudio, 0, 0, 9.5}}});
        combined.markers.push_back({9.75, "End"});
        check(carta::groupClips(combined, {{1, 0}, {2, 0}}, error) &&
                  carta::trimClipUnitsToSample(combined, {{0, 0}, {1, 0}}, {1, 0}, true, 9 * 44100,
                                               error) &&
                  std::abs(combined.tracks[2].clips[0].duration - 10.5) < 1e-12,
              "marker bounds are checked against the complete combined trim");
    }
    {
        auto rateSource = std::make_shared<carta::Audio>();
        rateSource->sampleRate = 48000;
        rateSource->samples.setSize(1, 200);
        rateSource->samples.clear();
        rateSource->sourceCues = {
            {0, "outside"}, {48, "trim start"}, {96, "inside"}, {128, "trim end"}};
        carta::Session cueSession;
        cueSession.tracks.push_back(
            {"rate", {{rateSource, 10.25 / carta::sessionRate, 48.0 / 48000, 80.0 / 48000}}});
        juce::String error;
        check(carta::addMarkersFromClipCues(cueSession, {0, 0}, error) &&
                  cueSession.markers.size() == 2 &&
                  std::llround(cueSession.markers[0].position * carta::sessionRate) == 10 &&
                  std::llround(cueSession.markers[1].position * carta::sessionRate) == 54 &&
                  cueSession.markers[0].name == "trim start" &&
                  cueSession.markers[1].name == "inside",
              "mixed-rate cues map from a half-open trim to nearest project samples");
        auto dense = std::make_shared<carta::Audio>();
        dense->sampleRate = 96000;
        dense->samples.setSize(1, 100);
        dense->samples.clear();
        dense->sourceCues = {{0, "one"}, {1, "two"}};
        carta::Session collision;
        collision.tracks.push_back({"dense", {{dense, 0, 0, 100.0 / 96000}}});
        check(!carta::addMarkersFromClipCues(collision, {0, 0}, error) &&
                  error.contains("collide") && collision.markers.empty(),
              "different source-rate cues that round to one project sample fail atomically");
        dense->sourceCueError = "Unsupported cue data.";
        check(!carta::addMarkersFromClipCues(collision, {0, 0}, error) &&
                  error == "Unsupported cue data." && collision.markers.empty(),
              "unsupported captured cue data cannot create partial markers");
    }
    {
        auto marked = session;
        marked.markers = {{1, "One"}, {1.25, "Two"}};
        juce::String markerError;
        check(!carta::setMarkerSample(marked, 0, 55125, markerError) &&
                  near(static_cast<float>(marked.markers[0].position), 1),
              "marker cannot move onto another boundary");
        check(!carta::setMarkerSample(marked, 0, 88200, markerError) &&
                  !carta::setMarkerSample(marked, 0, -1, markerError),
              "marker rejects out-of-bounds samples");
        check(carta::setMarkerSample(marked, 0, 66150, markerError) &&
                  near(static_cast<float>(marked.markers[0].position), 1.5f),
              "marker moves to exact sample");
    }
    {
        const double rate = carta::sessionRate;
        carta::EditorState state;
        state.session = session;
        state.session.markers = {{300 / rate, "Later"}, {10 / rate, "First"}};
        auto before = state;
        carta::EditHistory history;
        juce::String error;
        check(carta::shiftMarkers(state.session, 15, error), "shift all markers forward");
        auto rows = carta::inspectMarkers(state.session);
        check(rows[0].sample == 25 && rows[1].sample == 315 &&
                  state.session.markers[0].name == "Later" &&
                  state.session.markers[1].name == "First",
              "marker shift preserves names, order and relative spacing");
        history.record(before);
        check(history.undo(state) && carta::inspectMarkers(state.session)[0].sample == 10 &&
                  history.redo(state) && carta::inspectMarkers(state.session)[0].sample == 25,
              "bulk marker shift supports undo and redo");
        const auto unchanged = state.session.markers;
        check(
            !carta::shiftMarkers(state.session, -26, error) &&
                !carta::shiftMarkers(state.session, std::numeric_limits<juce::int64>::max(),
                                     error) &&
                !carta::shiftMarkers(state.session, std::numeric_limits<juce::int64>::min(), error),
            "out-of-bounds and extreme shifts are rejected");
        check(std::memcmp(&state.session.markers[0].position, &unchanged[0].position,
                          sizeof(double)) == 0 &&
                  std::memcmp(&state.session.markers[1].position, &unchanged[1].position,
                              sizeof(double)) == 0,
              "rejected marker shifts are atomic");
        state.session.markers[1].position = state.session.markers[0].position;
        check(!carta::shiftMarkers(state.session, 1, error),
              "duplicate markers must be repaired before bulk shift");
        state.session.markers[1].position = 25.4 / rate;
        check(!carta::shiftMarkers(state.session, 1, error),
              "fractional markers must be repaired before bulk shift");
    }
    {
        const double rate = carta::sessionRate;
        carta::EditorState state;
        state.session = session;
        state.session.markers = {
            {100 / rate, "B"}, {10 / rate, "A"}, {200 / rate, "C"}, {300 / rate, "D"}};
        const auto original = state;
        juce::String error;
        carta::EditHistory history;
        check(carta::shiftMarkersInRange(state.session, 100, 300, 20, error),
              "shift only markers in half-open range");
        auto rows = carta::inspectMarkers(state.session);
        check(rows[0].sample == 10 && rows[1].sample == 120 && rows[2].sample == 220 &&
                  rows[3].sample == 300 && state.session.markers[0].name == "B",
              "scoped shift includes start, excludes end, and keeps names and other markers");
        history.record(original);
        check(history.undo(state) && carta::inspectMarkers(state.session)[1].sample == 100 &&
                  history.redo(state) && carta::inspectMarkers(state.session)[1].sample == 120,
              "scoped marker shift is undoable and redoable");
        const auto unchanged = state.session.markers;
        check(!carta::shiftMarkersInRange(state.session, 120, 221, -110, error) &&
                  error.contains("collide") &&
                  !carta::shiftMarkersInRange(state.session, 120, 221, -111, error) &&
                  error.contains("cross"),
              "scoped shift rejects collisions and crossings with stationary markers");
        check(!carta::shiftMarkersInRange(state.session, 120, 221, 80, error) &&
                  error.contains("collide"),
              "scoped shift rejects collision with later marker");
        check(!carta::shiftMarkersInRange(state.session, 301, 400, 1, error) &&
                  error.contains("no markers"),
              "empty scope does not create an edit");
        check(!carta::shiftMarkersInRange(state.session, 120, 221,
                                          std::numeric_limits<juce::int64>::max(), error) &&
                  std::memcmp(&unchanged[0].position, &state.session.markers[0].position,
                              sizeof(double)) == 0 &&
                  std::memcmp(&unchanged[2].position, &state.session.markers[2].position,
                              sizeof(double)) == 0,
              "failed scoped shifts are atomic even at extreme offsets");
    }
    {
        const double rate = carta::sessionRate;
        carta::EditorState state;
        state.session = session;
        state.session.markers = {
            {300.4 / rate, "Later"}, {10.6 / rate, "First"}, {500 / rate, "Exact"}};
        const auto before = state;
        carta::EditHistory history;
        juce::String error;
        check(carta::snapMarkersToSamples(state.session, error) &&
                  std::llround(state.session.markers[0].position * rate) == 300 &&
                  std::llround(state.session.markers[1].position * rate) == 11 &&
                  state.session.markers[0].name == "Later",
              "bulk marker snap rounds fractional positions and preserves order and names");
        check(std::memcmp(&state.session.markers[2].position, &before.session.markers[2].position,
                          sizeof(double)) == 0,
              "bulk snap leaves aligned markers bit-identical");
        history.record(before);
        check(history.undo(state) &&
                  std::memcmp(&state.session.markers[0].position,
                              &before.session.markers[0].position, sizeof(double)) == 0 &&
                  history.redo(state) && !carta::inspectMarkers(state.session)[0].offGrid,
              "bulk marker snap supports undo and redo");
        check(!carta::snapMarkersToSamples(state.session, error),
              "bulk marker snap detects no changes");
        auto collision = before.session;
        collision.markers.push_back({300 / rate, "Collision"});
        check(!carta::snapMarkersToSamples(collision, error) && error.isNotEmpty() &&
                  std::memcmp(&collision.markers[0].position, &before.session.markers[0].position,
                              sizeof(double)) == 0,
              "bulk marker snap rejects collisions atomically");
        auto outside = before.session;
        outside.markers.push_back({88200 / rate, "Outside"});
        check(!carta::snapMarkersToSamples(outside, error) &&
                  std::memcmp(&outside.markers[0].position, &before.session.markers[0].position,
                              sizeof(double)) == 0,
              "bulk marker snap rejects out-of-bounds markers atomically");
    }
    {
        auto marked = session;
        const double rate = carta::sessionRate;
        marked.markers = {{300 / rate, "Later"},
                          {10 / rate, "First"},
                          {10.4 / rate, "First"},
                          {88200 / rate, "Past end"},
                          {std::numeric_limits<double>::quiet_NaN(), "Invalid"},
                          {20 / rate, " "}};
        const auto rows = carta::inspectMarkers(marked);
        check(rows.size() == 6 && rows[0].index == 1 && rows[1].index == 2 && rows[2].index == 5 &&
                  rows[3].index == 0 && rows[4].index == 3 && rows[5].index == 4,
              "marker inspection orders valid samples and preserves duplicate identity");
        check(rows[0].duplicate && rows[1].duplicate && rows[1].offGrid && !rows[0].offGrid,
              "marker inspection flags duplicate and fractional positions");
        check(rows[2].emptyName && rows[4].outside && !rows[5].validPosition,
              "marker inspection exposes empty names, out-of-range and invalid positions");
        juce::String error;
        check(carta::setMarkerSample(marked, 2, 11, error) &&
                  carta::setMarkerSample(marked, 3, 88199, error) &&
                  carta::setMarkerSample(marked, 4, 12, error),
              "inspected markers can be repaired individually");
        marked.markers[5].name = "Named";
        std::vector<carta::MasterTrack> tracks;
        check(carta::getMasterTracks(marked, tracks, error) && tracks.size() == 7,
              "repairing all listed issues restores valid master tracks");
        const auto repaired = carta::inspectMarkers(marked);
        check(std::none_of(repaired.begin(), repaired.end(),
                           [](const auto& r) {
                               return !r.validPosition || r.outside || r.duplicate || r.emptyName ||
                                      r.offGrid;
                           }),
              "repaired marker list has no reported issues");
    }
    {
        auto faded = session;
        auto& clip = faded.tracks[0].clips[0];
        clip.name = juce::String::fromUTF8("Intro 日本語");
        check(!carta::setClipEnvelope(clip, 1, 1.1, 0), "fade cannot exceed clip duration");
        check(carta::setClipEnvelope(clip, 0.5f, 0.5, 0.5),
              "set nondestructive clip gain and fades");
        juce::AudioBuffer<float> beforeFadeSplit(2, 4), afterFadeSplit(2, 4);
        beforeFadeSplit.clear();
        afterFadeSplit.clear();
        faded.render(beforeFadeSplit, 0, 4, 1, 4, 2);
        check(near(beforeFadeSplit.getSample(0, 0), 0) &&
                  near(beforeFadeSplit.getSample(0, 1), 0.5f) &&
                  near(beforeFadeSplit.getSample(0, 2), 1.5f) &&
                  near(beforeFadeSplit.getSample(0, 3), 1),
              "clip fade and gain envelope");
        check(faded.split(0, 0, 1.5), "split faded clip");
        faded.render(afterFadeSplit, 0, 4, 1, 4, 2);
        check(faded.tracks[0].clips[0].name == juce::String::fromUTF8("Intro 日本語") &&
                  faded.tracks[0].clips[1].name == juce::String::fromUTF8("Intro 日本語"),
              "split preserves independent clip name");
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 4; ++i)
                check(near(beforeFadeSplit.getSample(ch, i), afterFadeSplit.getSample(ch, i)),
                      "splitting preserves clip envelope");
        auto copy = carta::copyRange(faded, {55125, 77175, 0, 0});
        carta::Session pasted;
        pasted.tracks.push_back({"Destination", {}});
        check(carta::pasteRange(pasted, copy, 0, 0), "paste faded range");
        check(!pasted.tracks[0].clips.empty() &&
                  pasted.tracks[0].clips[0].name == juce::String::fromUTF8("Intro 日本語"),
              "copy and paste preserve clip name");
        juce::AudioBuffer<float> pasteAudio(2, 2), originalRange(2, 2);
        pasteAudio.clear();
        originalRange.clear();
        pasted.render(pasteAudio, 0, 2, 0, 4, 2);
        faded.render(originalRange, 0, 2, 1.25, 4, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 2; ++i)
                check(near(pasteAudio.getSample(ch, i), originalRange.getSample(ch, i)),
                      "copy and paste preserve fades");
    }
    {
        carta::Clip clip{session.tracks[0].clips[0].audio, 4, 0.25, 1};
        clip.gain = 0.7f;
        clip.fadeIn = 0.5;
        clip.fadeInStart = 0;
        clip.fadeOut = 0.75;
        clip.fadeOutEnd = 1.5;
        const auto equal = [](double a, double b) { return std::abs(a - b) < 1e-12; };
        check(carta::setClipFade(clip, true, 0.4) && equal(clip.fadeIn, 0.4) &&
                  equal(clip.fadeInStart, clip.offset) && equal(clip.fadeOut, 0.75) &&
                  equal(clip.fadeOutEnd, 1.5) && near(clip.gain, 0.7f),
              "changing a trimmed fade-in anchors only that fade to the current source edge");
        check(near(carta::clipLevel(clip, clip.offset), 0),
              "new fade-in starts silently at the trimmed edge");
        check(carta::setClipFade(clip, false, 0.2) && equal(clip.fadeOut, 0.2) &&
                  equal(clip.fadeOutEnd, clip.offset + clip.duration) && equal(clip.fadeIn, 0.4) &&
                  equal(clip.fadeInStart, clip.offset),
              "changing fade-out leaves fade-in intact");
        const auto before = clip;
        check(!carta::setClipFade(clip, true, std::numeric_limits<double>::quiet_NaN()) &&
                  !carta::setClipFade(clip, false, 1.1) && equal(clip.fadeIn, before.fadeIn) &&
                  equal(clip.fadeOut, before.fadeOut) &&
                  equal(clip.fadeInStart, before.fadeInStart) &&
                  equal(clip.fadeOutEnd, before.fadeOutEnd),
              "invalid fade changes leave both envelopes unchanged");
        check(carta::setClipFade(clip, true, 0) && carta::setClipFade(clip, false, clip.duration),
              "fade endpoints allow zero and full clip duration");
    }
    {
        auto constant = std::make_shared<carta::Audio>();
        constant->sampleRate = 4;
        constant->samples.setSize(1, 8);
        for (int i = 0; i < 8; ++i)
            constant->samples.setSample(0, i, 1);
        carta::Session joins;
        joins.tracks.push_back({"Join", {{constant, 0, 0, 1}, {constant, 0.5, 1, 1}}});
        juce::String joinError;
        carta::Session preview;
        carta::PlaybackRange previewRange;
        check(carta::prepareCrossfadePreview(joins, 0, 1, false, 4410, 4410, preview, previewRange,
                                             joinError) &&
                  std::abs(previewRange.start - 0.4) < 1e-10 &&
                  std::abs(previewRange.end - 1.1) < 1e-10 &&
                  std::abs(joins.tracks[0].clips[0].fadeOut) < 1e-12,
              "same-track crossfade preview includes the full overlap and context without editing "
              "the project");
        check(carta::crossfadeOverlap(joins, 0, 1, joinError),
              "create linear crossfade with either clip selected");
        juce::AudioBuffer<float> joined(2, 6);
        joined.clear();
        joins.render(joined, 0, 6, 0, 4, 2);
        juce::AudioBuffer<float> previewAudio(2, 6);
        previewAudio.clear();
        preview.render(previewAudio, 0, 6, 0, 4, 2);
        for (int ch = 0; ch < 2; ++ch)
            check(std::memcmp(previewAudio.getReadPointer(ch), joined.getReadPointer(ch),
                              6 * sizeof(float)) == 0,
                  "crossfade preview renders the exact applied edit");
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 6; ++i)
                check(near(joined.getSample(ch, i), 1),
                      "linear crossfade preserves level for identical signals");
        auto nested = joins;
        nested.tracks[0].clips[1].start = 0.25;
        nested.tracks[0].clips[1].duration = 0.25;
        const auto oldFade = nested.tracks[0].clips[0].fadeOut;
        check(!carta::crossfadeOverlap(nested, 0, 0, joinError) &&
                  std::abs(nested.tracks[0].clips[0].fadeOut - oldFade) < 1e-12,
              "nested crossfade leaves clips unchanged");
        auto ambiguous = joins;
        ambiguous.tracks[0].clips.push_back({constant, 0.25, 0, 1});
        check(!carta::crossfadeOverlap(ambiguous, 0, 0, joinError),
              "multiple overlapping clips require disambiguation");
        carta::EditorState cross;
        cross.session.tracks.push_back({"Outgoing", {{constant, 0, 0, 1}}});
        cross.session.tracks.push_back({"Incoming", {{constant, 0.5, 1, 1}}});
        const auto before = cross;
        carta::EditHistory history;
        carta::Session crossPreview;
        carta::PlaybackRange crossRange;
        check(carta::prepareCrossfadePreview(before.session, 1, 0, true, 441000, 441000,
                                             crossPreview, crossRange, joinError) &&
                  std::abs(crossRange.start) < 1e-12 && std::abs(crossRange.end - 1.5) < 1e-12 &&
                  std::abs(before.session.tracks[1].clips[0].fadeIn) < 1e-12,
              "cross-track preview clamps context at master boundaries without changing the "
              "original");
        check(carta::crossfadeAcrossTracks(cross.session, 1, 0, joinError),
              "cross-track linear crossfade works from the incoming clip");
        joined.clear();
        cross.session.render(joined, 0, 6, 0, 4, 2);
        previewAudio.clear();
        crossPreview.render(previewAudio, 0, 6, 0, 4, 2);
        for (int ch = 0; ch < 2; ++ch)
            check(std::memcmp(previewAudio.getReadPointer(ch), joined.getReadPointer(ch),
                              6 * sizeof(float)) == 0,
                  "cross-track preview matches the applied edit");
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 6; ++i)
                check(near(joined.getSample(ch, i), 1),
                      "cross-track complementary fades retain level for identical signals");
        check(std::abs(cross.session.tracks[0].clips[0].fadeOut - 0.5) < 1e-12 &&
                  std::abs(cross.session.tracks[1].clips[0].fadeIn - 0.5) < 1e-12,
              "cross-track fades span the overlap");
        history.record(before);
        check(history.undo(cross) && std::abs(cross.session.tracks[0].clips[0].fadeOut) < 1e-12 &&
                  history.redo(cross) &&
                  std::abs(cross.session.tracks[1].clips[0].fadeIn - 0.5) < 1e-12,
              "cross-track crossfade supports undo and redo");
        auto third = before.session;
        third.tracks[0].clips.push_back({constant, 0.75, 0, 0.5});
        check(!carta::crossfadeAcrossTracks(third, 0, 0, joinError) && joinError.isNotEmpty() &&
                  std::abs(third.tracks[0].clips[0].fadeOut) < 1e-12 &&
                  std::abs(third.tracks[1].clips[0].fadeIn) < 1e-12,
              "additional clip on either participating row rejects crossfade atomically");
        auto many = before.session;
        many.tracks.push_back({"Third", {{constant, 0.25, 0, 1}}});
        const auto retainedRange = crossRange;
        const auto retainedFade = crossPreview.tracks[0].clips[0].fadeOut;
        check(!carta::prepareCrossfadePreview(many, 0, 0, true, 10, 10, crossPreview, crossRange,
                                              joinError) &&
                  std::abs(crossRange.start - retainedRange.start) < 1e-12 &&
                  std::abs(crossPreview.tracks[0].clips[0].fadeOut - retainedFade) < 1e-12,
              "ambiguous crossfade preview preserves its previous outputs");
        auto subSample = before.session;
        subSample.tracks[1].clips[0].start = 1.0 - 0.5 / carta::sessionRate;
        check(!carta::prepareCrossfadePreview(subSample, 0, 0, true, 10, 10, crossPreview,
                                              crossRange, joinError) &&
                  joinError.contains("no project samples"),
              "preview rejects an overlap with no project sample positions");
        auto noSamples = subSample;
        const auto originalOutgoing = noSamples.tracks[0].clips[0];
        const auto originalIncoming = noSamples.tracks[1].clips[0];
        check(!carta::crossfadeClipPair(noSamples, {0, 0}, {1, 0}, joinError) &&
                  joinError.contains("no project samples") &&
                  std::abs(noSamples.tracks[0].clips[0].fadeOut - originalOutgoing.fadeOut) <
                      1e-12 &&
                  std::abs(noSamples.tracks[1].clips[0].fadeIn - originalIncoming.fadeIn) < 1e-12,
              "selected crossfade rejects a sample-free overlap without changing either envelope");
        check(!carta::crossfadeAcrossTracks(noSamples, 0, 0, joinError) &&
                  joinError.contains("no project samples"),
              "cross-track crossfade uses the same sample boundary validation as its preview");
        auto sameTrack = noSamples;
        sameTrack.tracks[0].clips.push_back(sameTrack.tracks[1].clips[0]);
        sameTrack.tracks.pop_back();
        check(!carta::crossfadeOverlap(sameTrack, 0, 0, joinError) &&
                  joinError.contains("no project samples") &&
                  std::abs(sameTrack.tracks[0].clips[0].fadeOut) < 1e-12,
              "same-track crossfade rejects a sample-free overlap atomically");
        auto oneSample = before.session;
        oneSample.tracks[1].clips[0].start = 1.0 - 1.0 / carta::sessionRate;
        check(carta::crossfadeAcrossTracks(oneSample, 0, 0, joinError),
              "an overlap containing one project sample remains eligible for crossfade");
        auto late = before.session;
        const double discEnd = 4800.0;
        late.tracks[0].clips[0].start = discEnd;
        late.tracks[1].clips[0].start = discEnd + 1.0 - 0.5 / carta::sessionRate;
        check(!carta::crossfadeAcrossTracks(late, 0, 0, joinError) &&
                  joinError.contains("no project samples"),
              "sample-free crossfades near the end of an 80-minute timeline reject consistently");
        late.tracks[1].clips[0].start = discEnd + 1.0 - 1.0 / carta::sessionRate;
        check(carta::crossfadeAcrossTracks(late, 0, 0, joinError),
              "one-sample overlaps near the end of an 80-minute timeline remain eligible");
        check(!carta::crossfadeAcrossTracks(many, 0, 0, joinError) && joinError.isNotEmpty() &&
                  std::abs(many.tracks[0].clips[0].fadeOut) < 1e-12,
              "multiple overlapping other-track clips reject crossfade atomically");
        auto contained = before.session;
        contained.tracks[1].clips[0].duration = 0.25;
        check(!carta::crossfadeAcrossTracks(contained, 0, 0, joinError) &&
                  std::abs(contained.tracks[0].clips[0].fadeOut) < 1e-12,
              "cross-track crossfade rejects contained clips");
    }
    {
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = 44100;
        source->samples.setSize(1, 1000);
        for (int i = 0; i < 1000; ++i)
            source->samples.setSample(0, i, std::sin(static_cast<float>(i) * 0.03f));
        carta::Session original;
        original.tracks.push_back({"Join", {{source, 0, 0, 1000 / carta::sessionRate}}});
        check(carta::setClipEnvelope(original.tracks[0].clips[0], 0.8f, 100 / carta::sessionRate,
                                     100 / carta::sessionRate),
              "prepare joined fade fixture");
        auto pieces = original;
        const double boundary = 500 / carta::sessionRate;
        check(pieces.split(0, 0, boundary), "split before joining");
        std::swap(pieces.tracks[0].clips[0], pieces.tracks[0].clips[1]);
        int selected = 1;
        juce::String error;
        carta::EditorState state;
        state.session = pieces;
        carta::EditHistory history;
        auto before = state;
        check(carta::joinNextClip(state.session, 0, selected, error) && selected == 0 &&
                  state.session.tracks[0].clips.size() == 1,
              "join consecutive split clips regardless of storage order");
        juce::AudioBuffer<float> a(2, 1000), b(2, 1000);
        a.clear();
        b.clear();
        pieces.render(a, 0, 1000, 0, carta::sessionRate, 2);
        state.session.render(b, 0, 1000, 0, carta::sessionRate, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 1000; ++i)
                check(near(a.getSample(ch, i), b.getSample(ch, i)),
                      "join preserves rendered source and fade");
        history.record(before);
        check(history.undo(state) && state.session.tracks[0].clips.size() == 2 &&
                  history.redo(state) && state.session.tracks[0].clips.size() == 1,
              "join supports undo and redo");
        auto rejected = pieces;
        selected = 1;
        rejected.tracks[0].clips[0].start += 1 / carta::sessionRate;
        check(!carta::joinNextClip(rejected, 0, selected, error) &&
                  rejected.tracks[0].clips.size() == 2,
              "join rejects gaps without mutation");
        rejected = pieces;
        rejected.tracks[0].clips[0].start -= 1 / carta::sessionRate;
        check(!carta::joinNextClip(rejected, 0, selected, error), "join rejects overlaps");
        rejected = pieces;
        rejected.tracks[0].clips[0].gain = 0.5f;
        check(!carta::joinNextClip(rejected, 0, selected, error),
              "join rejects differing envelopes");
        rejected = pieces;
        rejected.tracks[0].clips.push_back(rejected.tracks[0].clips[0]);
        check(!carta::joinNextClip(rejected, 0, selected, error),
              "join rejects ambiguous successors");
        rejected = pieces;
        rejected.tracks[0].clips[0].offset += 1 / carta::sessionRate;
        check(!carta::joinNextClip(rejected, 0, selected, error),
              "join rejects discontinuous source offsets");
        rejected = pieces;
        rejected.tracks[0].clips[0].offset += 1e-6 / carta::sessionRate;
        check(!carta::joinNextClip(rejected, 0, selected, error),
              "join rejects a source discontinuity larger than numerical grid tolerance");
    }
    {
        auto joinAudio = std::make_shared<carta::Audio>();
        joinAudio->sampleRate = 44100;
        joinAudio->samples.setSize(1, 1000);
        for (int i = 0; i < 1000; ++i)
            joinAudio->samples.setSample(0, i, 0.2f * std::sin(static_cast<float>(i) * 0.07f));
        carta::Session original;
        original.tracks.push_back({"One", {{joinAudio, 0, 0, 1000 / carta::sessionRate}}});
        original.tracks.push_back({"Two", {{joinAudio, 0, 0, 1000 / carta::sessionRate}}});
        for (auto& track : original.tracks)
            check(carta::setClipEnvelope(track.clips[0], 0.8f, 100 / carta::sessionRate,
                                         100 / carta::sessionRate),
                  "prepare group join envelopes");
        auto pieces = original;
        for (int track : {0, 1}) {
            check(pieces.split(track, 0, 500 / carta::sessionRate) &&
                      pieces.split(track, 0, 250 / carta::sessionRate),
                  "create group join pieces");
            std::swap(pieces.tracks[static_cast<size_t>(track)].clips[0],
                      pieces.tracks[static_cast<size_t>(track)].clips[2]);
        }
        const std::vector<carta::ClipReference> refs{{1, 2}, {0, 1}, {1, 0},
                                                     {0, 2}, {1, 1}, {0, 0}};
        auto joined = pieces;
        std::vector<carta::ClipReference> mapping;
        juce::String error;
        check(carta::joinSelectedClipChains(joined, refs, error, &mapping) &&
                  joined.tracks[0].clips.size() == 1 && joined.tracks[1].clips.size() == 1 &&
                  mapping.size() == refs.size(),
              "join selected contiguous chains on two tracks despite storage and selection order");
        for (size_t i = 0; i < refs.size(); ++i)
            check(mapping[i].track == refs[i].track && mapping[i].clip == 0,
                  "joined reference mapping identifies each original clip's result");
        juce::AudioBuffer<float> before(1, 1000), after(1, 1000);
        before.clear();
        after.clear();
        original.render(before, 0, 1000, 0, carta::sessionRate, 1);
        joined.render(after, 0, 1000, 0, carta::sessionRate, 1);
        check(std::memcmp(before.getReadPointer(0), after.getReadPointer(0),
                          1000 * sizeof(float)) == 0,
              "group join preserves all faded audio bit-for-bit");
        auto incompatible = pieces;
        incompatible.tracks[1].clips[1].gain = 0.5f;
        std::vector<carta::ClipReference> untouched{{7, 8}};
        check(!carta::joinSelectedClipChains(incompatible, refs, error, &untouched) &&
                  incompatible.tracks[0].clips.size() == 3 &&
                  incompatible.tracks[1].clips.size() == 3 && untouched.size() == 1 &&
                  untouched[0].track == 7,
              "a later incompatible track rejects the whole join and preserves output mapping");
        auto ambiguous = pieces;
        ambiguous.tracks[0].clips.push_back(ambiguous.tracks[0].clips[1]);
        check(!carta::joinSelectedClipChains(ambiguous, refs, error) &&
                  ambiguous.tracks[0].clips.size() == 4 && ambiguous.tracks[1].clips.size() == 3,
              "an unselected ambiguous successor rejects all joins");
        auto singleton = pieces;
        std::vector<carta::ClipReference> singletonMap;
        check(carta::joinSelectedClipChains(singleton, {{0, 2}, {0, 1}, {0, 0}, {1, 1}}, error,
                                            &singletonMap) &&
                  singleton.tracks[0].clips.size() == 1 && singleton.tracks[1].clips.size() == 3 &&
                  singletonMap[3].track == 1 && singletonMap[3].clip == 1,
              "unjoined singleton track remains unchanged and mapped");
        auto gap = pieces;
        check(!carta::joinSelectedClipChains(gap, {{0, 2}, {0, 0}}, error) &&
                  gap.tracks[0].clips.size() == 3,
              "an unselected intermediate clip cannot be joined implicitly");
    }
    {
        auto chainAudio = std::make_shared<carta::Audio>();
        chainAudio->sampleRate = 44100;
        chainAudio->samples.setSize(1, 44100);
        for (int i = 0; i < 44100; ++i)
            chainAudio->samples.setSample(0, i, 0.1f * std::sin(i * 0.013f));
        carta::Session pieces;
        pieces.tracks.push_back(
            {"Chains", {{chainAudio, 0, 0, 1}, {chainAudio, 2, 0, 1}, {chainAudio, 4, 0, 1}}});
        check(pieces.split(0, 0, 0.5) && pieces.split(0, 0, 0.25) && pieces.split(0, 3, 2.5),
              "prepare two separate selected chains");
        auto& clips = pieces.tracks[0].clips;
        std::reverse(clips.begin(), clips.end());
        const std::vector<carta::ClipReference> refs{{0, 0}, {0, 2}, {0, 5},
                                                     {0, 1}, {0, 3}, {0, 4}};
        auto joined = pieces;
        std::vector<carta::ClipReference> mapped;
        juce::String error;
        check(carta::joinSelectedClipChains(joined, refs, error, &mapped) &&
                  joined.tracks[0].clips.size() == 3 && mapped.size() == 6,
              "join two separated chains and retain a singleton despite reversed storage");
        if (mapped.size() == 6 && joined.tracks[0].clips.size() == 3) {
            check(mapped[1].clip == mapped[3].clip && mapped[2].clip == mapped[4].clip &&
                      mapped[4].clip == mapped[5].clip && mapped[0].clip != mapped[1].clip &&
                      mapped[1].clip != mapped[2].clip,
                  "mapping retains both chains and singleton after indices shift");
            juce::AudioBuffer<float> before(1, 220500), after(1, 220500);
            before.clear();
            after.clear();
            pieces.render(before, 0, 220500, 0, carta::sessionRate, 1);
            joined.render(after, 0, 220500, 0, carta::sessionRate, 1);
            check(std::memcmp(before.getReadPointer(0), after.getReadPointer(0),
                              220500 * sizeof(float)) == 0,
                  "separate chain joins preserve the complete mix bit-for-bit");
        }
        auto incompatible = pieces;
        incompatible.tracks[0].clips[1].gain = 0.5f;
        check(!carta::joinSelectedClipChains(incompatible, refs, error) &&
                  incompatible.tracks[0].clips.size() == 6,
              "incompatible later chain rejects earlier joins atomically");
    }
    session.tracks[0].clips.push_back({audio, 1, 0, 1});
    const auto regions = carta::layoutClipTitles({{0, 100}, {0, 100}}, {0, 100});
    check(regions.size() == 2 && regions[0].bounds.getEnd() == regions[1].bounds.getStart() &&
              regions[0].clip != regions[1].clip,
          "overlapping titles share one non-overlapping strip");
    const auto cropped = carta::layoutClipTitles({{-20, 50}, {25, 90}}, {0, 60});
    for (const auto& region : cropped)
        check(region.bounds.getStart() >= 0 && region.bounds.getEnd() <= 60,
              "title layout stays within viewport");
    {
        const auto reference = [](const std::vector<juce::Range<int>>& clips,
                                  juce::Range<int> viewport) {
            std::vector<int> edges;
            std::vector<carta::ClipTitleRegion> out;
            for (const auto& clip : clips) {
                const auto visible = clip.getIntersectionWith(viewport);
                if (!visible.isEmpty()) {
                    edges.push_back(visible.getStart());
                    edges.push_back(visible.getEnd());
                }
            }
            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
            for (size_t e = 1; e < edges.size(); ++e) {
                const int start = edges[e - 1], end = edges[e];
                std::vector<int> active;
                for (size_t clip = 0; clip < clips.size(); ++clip)
                    if (!clips[clip].isEmpty() && clips[clip].getStart() < end &&
                        clips[clip].getEnd() > start)
                        active.push_back(static_cast<int>(clip));
                for (size_t j = 0; j < active.size(); ++j) {
                    const int left =
                        start + static_cast<int>((static_cast<juce::int64>(end) - start) *
                                                 static_cast<juce::int64>(j) /
                                                 static_cast<juce::int64>(active.size()));
                    const int right =
                        start + static_cast<int>((static_cast<juce::int64>(end) - start) *
                                                 static_cast<juce::int64>(j + 1) /
                                                 static_cast<juce::int64>(active.size()));
                    if (left == right)
                        continue;
                    if (!out.empty() && out.back().clip == active[j] &&
                        out.back().bounds.getEnd() == left)
                        out.back().bounds = out.back().bounds.withEnd(right);
                    else
                        out.push_back({active[j], {left, right}});
                }
            }
            return out;
        };
        juce::Random random(0x7a11e);
        for (int trial = 0; trial < 3000; ++trial) {
            std::vector<juce::Range<int>> clips;
            const int count = random.nextInt(90);
            for (int i = 0; i < count; ++i) {
                const int start = random.nextInt(301) - 150;
                clips.emplace_back(start, start + random.nextInt(160));
            }
            const int viewStart = random.nextInt(101) - 50;
            const juce::Range<int> viewport{viewStart, viewStart + 1 + random.nextInt(150)};
            const auto expected = reference(clips, viewport),
                       actual = carta::layoutClipTitles(clips, viewport);
            check(actual.size() == expected.size(),
                  "swept title layout matches reference region count");
            for (size_t i = 0; i < actual.size(); ++i)
                check(actual[i].clip == expected[i].clip && actual[i].bounds == expected[i].bounds,
                      "swept title layout preserves reference clip and pixel allocation");
        }
        const auto emptyClip = carta::layoutClipTitles({{0, 10}, {5, 5}}, {0, 10});
        check(emptyClip.size() == 1 && emptyClip[0].clip == 0 &&
                  emptyClip[0].bounds == juce::Range<int>(0, 10),
              "an empty clip interval cannot claim title pixels");
        std::vector<juce::Range<int>> dense(5000, {0, 1});
        const auto narrow = carta::layoutClipTitles(dense, {0, 1});
        check(narrow.size() == 1 && narrow[0].clip == 4999 &&
                  narrow[0].bounds == juce::Range<int>(0, 1),
              "one-pixel overlap retains the original title priority among thousands of clips");
        const int lo = std::numeric_limits<int>::min() + 10,
                  hi = std::numeric_limits<int>::max() - 10;
        const auto wide = carta::layoutClipTitles({{lo, hi}, {lo, hi}}, {lo, hi});
        check(wide.size() == 2 && wide[0].bounds.getStart() == lo &&
                  wide[0].bounds.getEnd() == -1 && wide[1].bounds.getStart() == -1 &&
                  wide[1].bounds.getEnd() == hi,
              "title allocation handles a full-width integer viewport without overflow");
    }
    const auto markerLabels = carta::layoutMarkerLabels({10, 25, 40, 160}, {0, 300}, 2);
    check(markerLabels.size() == 2 && markerLabels[0].marker == 2 && markerLabels[1].marker == 3 &&
              markerLabels[0].bounds.getEnd() + 2 <= markerLabels[1].bounds.getStart(),
          "dense marker labels prioritize the selected marker without overlap");
    const auto unselectedLabels = carta::layoutMarkerLabels({10, 25, 40, 160}, {0, 300}, -1);
    check(unselectedLabels.size() == 2 && unselectedLabels[0].marker == 0 &&
              unselectedLabels[1].marker == 3,
          "dense marker labels use chronological order when none is selected");
    const auto coincidentLabels = carta::layoutMarkerLabels({10, 10, 10}, {0, 150}, 2);
    check(coincidentLabels.size() == 1 && coincidentLabels[0].marker == 2,
          "selected coincident marker keeps its label");
    check(carta::layoutMarkerLabels({98, 100, -10}, {0, 110}, -1).empty(),
          "offscreen or too-narrow marker labels are omitted");
    for (int selected = -1; selected < 12; ++selected) {
        const std::vector<int> pixels{300, -5, 12, 50, 52, 100, 101, 190, 200, 201, 202, 275};
        const auto labels = carta::layoutMarkerLabels(pixels, {0, 300}, selected);
        for (size_t i = 0; i < labels.size(); ++i) {
            const auto& label = labels[i];
            check(label.marker >= 0 && static_cast<size_t>(label.marker) < pixels.size() &&
                      label.bounds.getStart() >= pixels[static_cast<size_t>(label.marker)] + 3 &&
                      label.bounds.getEnd() <= 300 && label.bounds.getLength() >= 32,
                  "marker label geometry stays within the viewport and its own marker");
            if (i > 0)
                check(labels[i - 1].bounds.getEnd() + 2 <= label.bounds.getStart(),
                      "marker labels remain separated for every selected marker");
        }
    }
    juce::AudioBuffer<float> overlap(2, 8);
    overlap.clear();
    session.render(overlap, 0, 8, 0, 4, 2);
    check(near(overlap.getSample(0, 4), 2) && near(overlap.getSample(1, 4), 4),
          "overlapping clips both contribute to the mix");
    session.tracks[0].clips.pop_back();
    {
        carta::Session edit;
        edit.tracks.push_back({"A", {{audio, 1, 0, 1}, {audio, 1.25, 0, 0.5}}});
        edit.tracks.push_back({"B", {{audio, 1, 0, 1}}});
        const carta::TimeSelection range{55125, 77175, 0, 1};
        const auto copied = carta::copyRange(edit, range);
        check(copied.lengthSamples == 22050 && copied.tracks.size() == 2 &&
                  copied.tracks[0].size() == 2 && copied.tracks[1].size() == 1,
              "copy preserves overlapping clips and tracks");
        check(near(static_cast<float>(copied.tracks[0][0].offset), 0.25f) &&
                  near(static_cast<float>(copied.tracks[0][0].duration), 0.5f),
              "copy clips source range");
        edit.markers = {{1.5, "inside"}, {1.9, "after"}};
        const auto original = edit;
        auto splitEdit = original;
        check(carta::splitRange(splitEdit, range) && splitEdit.tracks[0].clips.size() == 4 &&
                  splitEdit.tracks[1].clips.size() == 3,
              "split both range boundaries across overlapping tracks");
        juce::AudioBuffer<float> unsplit(2, 4), splitAudio(2, 4);
        unsplit.clear();
        splitAudio.clear();
        original.render(unsplit, 0, 4, 1, 4, 2);
        splitEdit.render(splitAudio, 0, 4, 1, 4, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 4; ++i)
                check(near(unsplit.getSample(ch, i), splitAudio.getSample(ch, i)),
                      "range split preserves reconstruction");
        carta::EditorState splitAt;
        splitAt.session = original;
        check(carta::setClipEnvelope(splitAt.session.tracks[0].clips[0], 0.8f, 0.25, 0.25),
              "prepare faded split-at-playhead fixture");
        const auto beforeSplitAt = splitAt;
        carta::EditHistory splitHistory;
        check(carta::splitTracksAtSample(splitAt.session, {0, 0}, 66150) &&
                  splitAt.session.tracks[0].clips.size() == 4 &&
                  splitAt.session.tracks[1].clips.size() == 1,
              "split at playhead cuts every crossing overlap only on the scoped track");
        check(splitAt.session.markers.size() == 2 &&
                  std::abs(splitAt.session.markers[0].position - 1.5) < 1e-12,
              "split at playhead preserves markers");
        juce::AudioBuffer<float> originalSamples(2, 44100), cutSamples(2, 44100);
        originalSamples.clear();
        cutSamples.clear();
        beforeSplitAt.session.render(originalSamples, 0, 44100, 1, carta::sessionRate, 2);
        splitAt.session.render(cutSamples, 0, 44100, 1, carta::sessionRate, 2);
        float splitError = 0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 44100; ++i)
                splitError = std::max(splitError, std::abs(originalSamples.getSample(ch, i) -
                                                           cutSamples.getSample(ch, i)));
        check(splitError < 2e-5f,
              "split at playhead preserves mixed faded audio across the boundary");
        splitHistory.record(beforeSplitAt);
        check(splitHistory.undo(splitAt) && splitAt.session.tracks[0].clips.size() == 2 &&
                  splitHistory.redo(splitAt) && splitAt.session.tracks[0].clips.size() == 4,
              "split at playhead supports undo and redo");
        auto allSplit = original;
        check(carta::splitTracksAtSample(allSplit, {0, 1}, 66150) &&
                  allSplit.tracks[0].clips.size() == 4 && allSplit.tracks[1].clips.size() == 2,
              "split at playhead cuts crossing clips on all scoped tracks");
        check(!carta::splitTracksAtSample(allSplit, {0, 1}, 66150) &&
                  !carta::splitTracksAtSample(allSplit, {0, 1}, 44100),
              "splitting at existing edges is a no-op");
        check(!carta::splitTracksAtSample(allSplit, {0, 2}, 66150) &&
                  !carta::splitTracksAtSample(allSplit, {0, 1}, -1) &&
                  allSplit.tracks[0].clips.size() == 4,
              "invalid scope and negative position leave the arrangement intact");
        auto moved = original;
        moved.tracks[0].gain = 0.7f;
        moved.tracks[0].pan = -0.3f;
        moved.tracks[0].solo = true;
        moved.tracks[0].routing = {{0.7f, 0.2f}, {0.1f, 0.8f}};
        check(carta::setClipEnvelope(moved.tracks[0].clips[0], 0.8f, 0.25, 0.25),
              "prepare faded split-to-track fixture");
        const auto beforeMove = moved;
        carta::EditorState state;
        state.session = moved;
        carta::EditHistory history;
        auto previous = state;
        int first = -1;
        check(carta::splitRangeToNewTracks(state.session, range, first) && first == 2 &&
                  state.session.tracks.size() == 4,
              "split selection creates one track per nonempty source row");
        check(state.session.tracks[2].clips.size() == 2 &&
                  state.session.tracks[3].clips.size() == 1 && state.session.tracks[2].solo &&
                  state.session.tracks[2].routing.size() == 2 &&
                  near(state.session.tracks[2].gain, 0.7f) &&
                  near(state.session.tracks[2].pan, -0.3f),
              "new tracks preserve overlaps and mix settings");
        check(state.session.markers.size() == 2 &&
                  carta::projectEndSample(state.session) == carta::projectEndSample(beforeMove),
              "split to tracks preserves markers and master duration");
        juce::AudioBuffer<float> beforeMoveAudio(2, 8), afterMoveAudio(2, 8);
        beforeMoveAudio.clear();
        afterMoveAudio.clear();
        beforeMove.render(beforeMoveAudio, 0, 8, 0.5, 4, 2);
        state.session.render(afterMoveAudio, 0, 8, 0.5, 4, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 8; ++i)
                check(near(beforeMoveAudio.getSample(ch, i), afterMoveAudio.getSample(ch, i)),
                      "split to tracks preserves audible mix across boundaries");
        history.record(previous);
        check(history.undo(state) && state.session.tracks.size() == 2 && history.redo(state) &&
                  state.session.tracks.size() == 4,
              "split to tracks supports undo and redo");
        int untouched = 99;
        auto noAudio = original;
        check(!carta::splitRangeToNewTracks(noAudio, {0, 100, 0, 1}, untouched) &&
                  untouched == 99 && noAudio.tracks.size() == 2,
              "split of silent range changes nothing");
        auto localRipple = original;
        check(carta::rippleRemoveRange(localRipple, {55125, 77175, 0, 0}, false),
              "ripple selected track");
        check(localRipple.tracks[1].clips.size() == 1 &&
                  near(static_cast<float>(localRipple.tracks[1].clips[0].start), 1.0f) &&
                  near(static_cast<float>(localRipple.markers[1].position), 1.9f),
              "local ripple leaves other track and markers unchanged");
        check(near(static_cast<float>(localRipple.tracks[0].clips[1].start), 1.25f) &&
                  near(static_cast<float>(localRipple.tracks[0].clips[1].offset), 0.75f),
              "ripple closes gap with correct right source");
        auto globalRipple = original;
        check(carta::rippleRemoveRange(globalRipple, {55125, 77175, 0, 0}, true),
              "ripple all tracks");
        check(globalRipple.tracks[1].clips.size() == 2 &&
                  near(static_cast<float>(globalRipple.markers[0].position), 1.25f) &&
                  near(static_cast<float>(globalRipple.markers[1].position), 1.4f),
              "global ripple aligns tracks and markers");
        check(!carta::pasteRange(edit, copied, 1, 0) &&
                  edit.tracks[0].clips.size() == original.tracks[0].clips.size(),
              "paste rejects incomplete track destinations without mutation");
        check(carta::removeRange(edit, range), "remove selected range");
        check(edit.tracks[0].clips.size() == 2 && edit.tracks[1].clips.size() == 2,
              "remove leaves both sides and removes overlaps");
        check(near(static_cast<float>(edit.tracks[0].clips[1].start), 1.75f) &&
                  near(static_cast<float>(edit.tracks[0].clips[1].offset), 0.75f),
              "gap deletion preserves right source offset");
        check(carta::pasteRange(edit, copied, 0, 132300), "paste on multiple tracks");
        check(near(static_cast<float>(edit.tracks[0].clips[2].start), 3.0f) &&
                  near(static_cast<float>(edit.tracks[1].clips[2].start), 3.0f),
              "paste uses sample-accurate destination");
        juce::AudioBuffer<float> before(2, 2), afterPaste(2, 2);
        before.clear();
        afterPaste.clear();
        original.render(before, 0, 2, 1.25, 4, 2);
        edit.render(afterPaste, 0, 2, 3.0, 4, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 2; ++i)
                check(near(before.getSample(ch, i), afterPaste.getSample(ch, i)),
                      "copied range renders identically");
        check(!carta::removeRange(edit, {0, 100, 0, 5}) &&
                  carta::copyRange(edit, {0, 100, 0, 5}).empty(),
              "invalid ranges are rejected");
    }
    {
        const double rate = carta::sessionRate;
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = rate;
        source->samples.setSize(1, 1000);
        for (int i = 0; i < 1000; ++i)
            source->samples.setSample(0, i, static_cast<float>(i) / 2000.0f);
        carta::Session base;
        base.tracks.push_back({"master", {{source, 0, 0, 1000 / rate}}});
        const carta::TimeSelection range{100, 200, 0, 0};
        juce::String error;
        for (const auto& markers : std::vector<std::vector<carta::Marker>>{
                 {{100 / rate, "start"}, {150 / rate, "inside"}},
                 {{150 / rate, "inside"}, {200 / rate, "end"}},
                 {{110 / rate, "one"}, {190 / rate, "two"}}}) {
            auto conflicting = base;
            conflicting.markers = markers;
            const auto before = conflicting;
            check(!carta::rippleRemoveRange(conflicting, range, true, &error) &&
                      error.contains("same sample"),
                  "ripple rejects boundary collisions");
            check(conflicting.tracks[0].clips.size() == 1 &&
                      std::abs(conflicting.tracks[0].clips[0].duration -
                               before.tracks[0].clips[0].duration) < 1e-12 &&
                      conflicting.markers[0].name == before.markers[0].name &&
                      std::abs(conflicting.markers[1].position - before.markers[1].position) <
                          1e-12,
                  "rejected ripple leaves all clips and markers unchanged");
        }
        auto fractional = base;
        fractional.tracks[0].clips[0].duration = 900.25 / rate;
        check(carta::projectEndSample(fractional) == 901,
              "fractional source end includes the last partial output sample");
        fractional.tracks[0].clips[0].duration = 0.25 / rate;
        check(carta::projectEndSample(fractional) == 1,
              "nonempty sub-sample duration is not discarded");
        fractional.tracks[0].clips[0].duration = (900.0 + 0.00000005) / rate;
        check(carta::projectEndSample(fractional) == 900,
              "near-integer floating-point noise does not add a sample");
        auto valid = base;
        valid.markers = {{50 / rate, "before"}, {150 / rate, "inside"}, {500 / rate, "after"}};
        check(carta::rippleRemoveRange(valid, range, true, &error),
              "ripple with distinct markers succeeds");
        check(std::llround(valid.markers[0].position * rate) == 50 &&
                  std::llround(valid.markers[1].position * rate) == 100 &&
                  std::llround(valid.markers[2].position * rate) == 400 &&
                  valid.markers[1].name == "inside",
              "ripple maps markers in integer samples and preserves names");
        auto fractionalMarker = base;
        fractionalMarker.markers = {{500.25 / rate, "legacy"}};
        const double oldPosition = fractionalMarker.markers[0].position;
        check(!carta::rippleRemoveRange(fractionalMarker, range, true, &error) &&
                  error.contains("off-grid") && fractionalMarker.tracks[0].clips.size() == 1 &&
                  std::memcmp(&fractionalMarker.markers[0].position, &oldPosition,
                              sizeof(double)) == 0,
              "all-track ripple rejects fractional marker movement atomically");
        check(carta::rippleRemoveRange(fractionalMarker, range, false, &error) &&
                  std::memcmp(&fractionalMarker.markers[0].position, &oldPosition,
                              sizeof(double)) == 0,
              "selected-track ripple leaves fractional markers unchanged");
        std::vector<carta::MasterTrack> masterTracks;
        check(carta::getMasterTracks(valid, masterTracks, error) && masterTracks.size() == 4 &&
                  masterTracks.back().range.end == 900,
              "successful ripple leaves valid ordered track boundaries");
        juce::AudioBuffer<float> rendered(2, 900);
        rendered.clear();
        valid.render(rendered, 0, 900, 0, rate, 2);
        for (int i = 0; i < 900; ++i)
            check(
                near(rendered.getSample(0, i), source->samples.getSample(0, i < 100 ? i : i + 100)),
                "ripple reconstruction removes exactly the selected source samples");
        auto local = base;
        local.markers = {{900 / rate, "late"}};
        check(!carta::rippleRemoveRange(local, {800, 1000, 0, 0}, false, &error) &&
                  error.contains("beyond the master") && local.tracks[0].clips.size() == 1,
              "local ripple cannot strand a marker after shortened master");
        auto empty = base;
        empty.markers = {{0, "first"}};
        check(!carta::rippleRemoveRange(empty, {0, 1000, 0, 0}, true, &error) &&
                  error.contains("beyond the master") && empty.tracks[0].clips.size() == 1,
              "deleting all audio cannot silently orphan a marker");
        empty.markers.clear();
        check(carta::rippleRemoveRange(empty, {0, 1000, 0, 0}, true, &error) &&
                  empty.tracks[0].clips.empty(),
              "ripple can empty a marker-free arrangement");
    }
    check(!session.split(0, 0, 1) && session.split(0, 0, 1.5), "split bounds");
    check(session.tracks[0].clips.size() == 2 &&
              near(static_cast<float>(session.tracks[0].clips[1].offset), 0.5f),
          "split source offset");
    juce::AudioBuffer<float> after(2, 8);
    after.clear();
    session.render(after, 0, 8, 0, 4, 2);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 8; ++i)
            check(near(stereo.getSample(ch, i), after.getSample(ch, i)), "split preserves render");
    juce::AudioBuffer<float> resampled(2, 2);
    resampled.clear();
    session.render(resampled, 0, 2, 1.125, 4, 2);
    carta::SincResampler reference(4, 4);
    const float expectedLeft = (reference.sample(audio->samples.getReadPointer(0), 4, 0.5) +
                                reference.sample(audio->samples.getReadPointer(2), 4, 0.5)) *
                               0.5f;
    const float expectedRight = (reference.sample(audio->samples.getReadPointer(1), 4, 0.5) +
                                 reference.sample(audio->samples.getReadPointer(3), 4, 0.5)) *
                                0.5f;
    check(near(resampled.getSample(0, 0), expectedLeft) &&
              near(resampled.getSample(1, 0), expectedRight),
          "fractional position uses the band-limited converter");
    {
        auto highRate = std::make_shared<carta::Audio>();
        highRate->sampleRate = 96000;
        highRate->samples.setSize(2, 9600);
        for (int i = 0; i < 9600; ++i) {
            highRate->samples.setSample(
                0, i, 0.4f * std::sin(2.0f * juce::MathConstants<float>::pi * 1000 * i / 96000));
            highRate->samples.setSample(
                1, i, 0.3f * std::sin(2.0f * juce::MathConstants<float>::pi * 2000 * i / 96000));
        }
        carta::Session multirate;
        multirate.tracks.push_back({"96 kHz", {{highRate, 0, 0, 0.1}}});
        carta::ResamplingContext context;
        multirate.prepareResampling(context, 44100);
        juce::AudioBuffer<float> whole(2, 4410), chunks(2, 4410), audition(2, 4410);
        whole.clear();
        chunks.clear();
        audition.clear();
        multirate.render(whole, 0, 4410, 0, 44100, 2, &context);
        for (int p = 0; p < 4410; p += 257)
            multirate.render(chunks, p, std::min(257, 4410 - p), static_cast<double>(p) / 44100,
                             44100, 2, &context);
        double cursor = 0;
        for (int p = 0; p < 4410; p += 257)
            carta::renderPlayback(multirate, audition, p, std::min(257, 4410 - p), cursor, 44100, 2,
                                  {0, 0.1, false}, &context);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 4410; ++i) {
                check(near(whole.getSample(ch, i), chunks.getSample(ch, i)),
                      "resampling is independent of render block size");
                check(near(whole.getSample(ch, i), audition.getSample(ch, i)),
                      "playback and master rendering use the same converter");
            }
    }
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getNonexistentChildFile("carta-session-test", {}, false);
    check(dir.createDirectory().wasOk(), "create temp directory");
    {
        const auto target = dir.getChildFile("checked-text.json");
        const auto text = juce::String::fromUTF8("{\"name\":\"日本語\"}\n");
        check(target.replaceWithText("Original"), "prepare checked-text destination");
        juce::File temporary;
        {
            auto prepared = carta::prepareCheckedText(target, text);
            check(prepared != nullptr, "prepare checked UTF-8 text");
            temporary = prepared->getFile();
            check(temporary.existsAsFile() && temporary.loadFileAsString() == text &&
                      target.loadFileAsString() == "Original",
                  "prepared UTF-8 file is complete without touching the destination");
        }
        check(!temporary.exists() && target.loadFileAsString() == "Original",
              "abandoning prepared text removes only its temporary file");
        auto prepared = carta::prepareCheckedText(target, text);
        check(prepared && carta::replaceTemporaryAtomically(*prepared, target) &&
                  target.loadFileAsString() == text,
              "committing checked UTF-8 text replaces the target after preparation");
        check(!carta::prepareCheckedText(dir.getChildFile("missing-parent").getChildFile("out"),
                                         text),
              "text preparation fails for a missing parent directory");
#if JUCE_MAC || JUCE_LINUX
        const auto directoryTarget = dir.getChildFile("existing-directory");
        check(directoryTarget.createDirectory().wasOk(),
              "create a destination that cannot be replaced");
        const auto retained = directoryTarget.getChildFile("retain.txt");
        check(retained.replaceWithText("Retain"), "prepare destination directory fixture");
#if JUCE_MAC || JUCE_LINUX
        check(carta::syncParentDirectory(retained) == carta::DirectorySyncStatus::confirmed,
              "parent directory sync succeeds for a local fixture");
        check(carta::syncParentDirectory(retained.getParentDirectory()
                                             .getChildFile("missing-directory")
                                             .getChildFile("file")) ==
                  carta::DirectorySyncStatus::unconfirmed,
              "missing parent directory reports unconfirmed synchronization");
#endif
        auto rejected = carta::prepareCheckedText(directoryTarget, text);
        check(rejected && !carta::replaceTemporaryAtomically(*rejected, directoryTarget) &&
                  retained.loadFileAsString() == "Retain" &&
                  rejected->getFile().loadFileAsString() == text,
              "failed atomic replacement keeps the destination and prepared temporary intact");
#endif
    }
    {
        juce::String settingsError;
        std::unique_ptr<juce::XmlElement> saved = std::make_unique<juce::XmlElement>("old");
        const auto file = dir.getChildFile("device-settings").getChildFile("audio.xml");
        check(carta::loadDeviceSettings(file, saved, settingsError) && !saved,
              "missing audio settings use defaults");
        juce::XmlElement device("DEVICESETUP");
        device.setAttribute("audioOutputDeviceName", juce::String::fromUTF8("Output 日本語"));
        device.setAttribute("audioDeviceRate", 48000);
        device.setAttribute("audioDeviceBufferSize", 256);
        device.setAttribute("audioDeviceOutChans", "1100");
        check(carta::saveDeviceSettings(file, device, settingsError) &&
                  carta::loadDeviceSettings(file, saved, settingsError) && saved &&
                  saved->getStringAttribute("audioOutputDeviceName") ==
                      device.getStringAttribute("audioOutputDeviceName") &&
                  saved->getIntAttribute("audioDeviceRate") == 48000 &&
                  saved->getIntAttribute("audioDeviceBufferSize") == 256 &&
                  saved->getStringAttribute("audioDeviceOutChans") == "1100",
              "audio settings persist device, rate, buffer, channel mask and Unicode");
        carta::AudioOutputSnapshot actual;
        actual.open = true;
        actual.name = device.getStringAttribute("audioOutputDeviceName");
        actual.sampleRate = 48000;
        actual.bufferSize = 256;
        actual.channels.parseString("1100", 2);
        check(carta::describeDeviceRestoreDifference(device, actual).isEmpty(),
              "matching restored audio settings have no warning");
        actual.name = "Different output";
        actual.sampleRate = 44100;
        actual.bufferSize = 512;
        actual.channels.parseString("11", 2);
        const auto difference = carta::describeDeviceRestoreDifference(device, actual);
        check(difference.contains("output device") && difference.contains("sample rate") &&
                  difference.contains("buffer size") && difference.contains("output channels"),
              "restore warning identifies changed device, rate, buffer and channel mapping");
        actual.open = false;
        check(carta::describeDeviceRestoreDifference(device, actual).contains("No audio output"),
              "restore reports unavailable output");
        actual.open = true;
        actual.channels.clear();
        check(carta::describeDeviceRestoreDifference(device, actual).contains("No audio output"),
              "restore reports an open device with no output channels");
        juce::XmlElement defaults("DEVICESETUP");
        actual.name = "Any";
        actual.channels.setBit(0);
        check(carta::describeDeviceRestoreDifference(defaults, actual).isEmpty(),
              "unspecified device properties do not report mismatches");
        const auto contents = file.loadFileAsString();
        juce::XmlElement invalid("OTHER");
        check(!carta::saveDeviceSettings(file, invalid, settingsError) &&
                  file.loadFileAsString() == contents,
              "invalid audio state cannot replace previous settings");
        const auto bad = dir.getChildFile("bad-device.xml");
        check(bad.replaceWithText("<DEVICESETUP>"), "write malformed audio settings");
        check(!carta::loadDeviceSettings(bad, saved, settingsError) && saved &&
                  saved->hasTagName("DEVICESETUP"),
              "malformed audio settings preserve the caller's existing state");
        const auto oversized = dir.getChildFile("oversized-device.xml");
        auto stream = oversized.createOutputStream();
        check(stream && stream->setPosition(64 * 1024) && stream->writeByte(0),
              "write oversized audio settings");
        stream.reset();
        check(!carta::loadDeviceSettings(oversized, saved, settingsError) && saved &&
                  saved->hasTagName("DEVICESETUP"),
              "oversized audio settings are rejected without replacing state");
        const auto parent = dir.getChildFile("not-a-directory");
        check(parent.replaceWithText("x"), "write audio settings directory blocker");
        check(
            !carta::saveDeviceSettings(parent.getChildFile("settings.xml"), device, settingsError),
            "audio settings save reports an unusable directory");
    }
    const auto wavfile = dir.getChildFile("source.wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> output = wavfile.createOutputStream();
        auto opts = juce::AudioFormatWriterOptions{}
                        .withSampleRate(44100)
                        .withNumChannels(4)
                        .withBitsPerSample(24);
        auto writer = wav.createWriterFor(output, opts);
        check(writer != nullptr, "create four-channel fixture");
        juce::AudioBuffer<float> fixture(4, 128);
        for (int ch = 0; ch < 4; ++ch)
            for (int i = 0; i < 128; ++i)
                fixture.setSample(ch, i, 0.1f * static_cast<float>(ch + 1));
        check(writer->writeFromAudioSampleBuffer(fixture, 0, 128), "write fixture");
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    juce::String error;
    {
        const auto sourceFile = dir.getChildFile("mapped-bank-source.wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> output = sourceFile.createOutputStream();
            auto writer = wav.createWriterFor(output, juce::AudioFormatWriterOptions{}
                                                          .withSampleRate(44100)
                                                          .withNumChannels(2)
                                                          .withBitsPerSample(24));
            check(writer != nullptr, "create distinct mapped-bank audio fixture");
            juce::AudioBuffer<float> fixture(2, 128);
            for (int i = 0; i < 128; ++i) {
                fixture.setSample(0, i, static_cast<float>(i - 64) / 256.0f);
                fixture.setSample(1, i, static_cast<float>(63 - i) / 512.0f);
            }
            check(writer->writeFromAudioSampleBuffer(fixture, 0, 128),
                  "write distinct mapped-bank audio fixture");
        }
        auto bankAudio = carta::loadAudio(formats, sourceFile, error);
        check(static_cast<bool>(bankAudio), "load mapped-bank audio fixture");
        carta::Session source;
        source.tracks.push_back({"Layer 1", {{bankAudio, 0, 0, bankAudio->length()}}});
        source.mdDisc.tracks = {
            {"one", "Opening", "", {}, false, false, false, "", {{0, 32}, {32, 96}}},
            {"two", "Repeat", "", {}, false, false, false, "", {{32, 96}}}};
        source.mdDisc.groups = {{1, 2, "Main Set", ""}};
        carta::DiscPreviewPlan originalPlan;
        carta::DiscPreviewRenderer originalRenderer;
        check(carta::prepareDiscPreview(source, originalPlan, error) &&
                  originalPlan.samples == 160 &&
                  carta::prepareDiscPreviewRenderer(source, 44100, 2, originalRenderer, error),
              "prepare MdDisc preview");
        juce::AudioBuffer<float> expected(2, 160);
        expected.clear();
        juce::int64 originalPosition = 0;
        carta::renderDiscPreview(source, originalPlan, originalRenderer, expected, 0, 160,
                                 originalPosition, 44100, 2);
        check(originalPosition == 160, "render full MdDisc preview");
        const auto package = dir.getChildFile("md-disc-portable.carta");
        check(carta::saveNativePackage(source, 2, package, error) && sourceFile.deleteFile(),
              "save portable MdDisc package and remove the original source");
        carta::Session reopened;
        int channels = 0;
        check(carta::loadNativePackage(reopened, channels, formats, package, error) &&
                  reopened.mdDisc.tracks.size() == 2 && reopened.mdDisc.groups.size() == 1 &&
                  reopened.mdDisc.tracks[0].parts[1] == reopened.mdDisc.tracks[1].parts[0],
              "reopened package retains MdDisc structure and shared parts");
        carta::DiscPreviewPlan reopenedPlan;
        carta::DiscPreviewRenderer reopenedRenderer;
        check(carta::prepareDiscPreview(reopened, reopenedPlan, error) &&
                  carta::prepareDiscPreviewRenderer(reopened, 44100, 2, reopenedRenderer, error),
              "prepare reopened MdDisc preview");
        juce::AudioBuffer<float> actual(2, 160);
        actual.clear();
        juce::int64 reopenedPosition = 0;
        carta::renderDiscPreview(reopened, reopenedPlan, reopenedRenderer, actual, 0, 160,
                                 reopenedPosition, 44100, 2);
        check(reopenedPosition == 160, "reopened preview retains its logical duration");
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 160; ++i)
                check(std::equal_to<float>{}(actual.getSample(ch, i), expected.getSample(ch, i)),
                      "reopened MdDisc preview preserves every rendered sample");

        auto repeated = reopened;
        repeated.mdDisc.tracks.clear();
        repeated.mdDisc.groups.clear();
        for (int i = 0; i < 240; ++i)
            repeated.mdDisc.tracks.push_back({"md-" + std::to_string(i),
                                              i == 2 ? "" : ("Track " + std::to_string(i + 1)),
                                              "",
                                              {},
                                              false,
                                              false,
                                              false,
                                              "",
                                              i % 2 == 0
                                                  ? std::vector<carta::MdPart>{{0, 32}, {32, 96}}
                                                  : std::vector<carta::MdPart>{{32, 96}}});
        const auto repeatedPkg = dir.getChildFile("repeated-240-portable.carta");
        check(carta::saveNativePackage(repeated, 2, repeatedPkg, error),
              "save 240-track repeated MdDisc package");
        carta::Session repeatedReopened;
        int repeatedChannels = 0;
        check(carta::loadNativePackage(repeatedReopened, repeatedChannels, formats, repeatedPkg,
                                       error) &&
                  repeatedChannels == 2 && repeatedReopened.mdDisc.tracks.size() == 240,
              "reopen 240-track MdDisc package");
        carta::DiscPreviewPlan repeatedPlan;
        carta::DiscPreviewRenderer repeatedRenderer;
        check(carta::prepareDiscPreview(repeatedReopened, repeatedPlan, error) &&
                  repeatedPlan.samples == 160 * 120 &&
                  carta::prepareDiscPreviewRenderer(repeatedReopened, 44100, 2, repeatedRenderer,
                                                    error),
              "prepare all 240 occurrences after reopening");
        juce::AudioBuffer<float> clockBlock(2, 257);
        juce::int64 clockPosition = 0;
        while (clockPosition < repeatedPlan.samples) {
            const auto first = clockPosition;
            const int count =
                static_cast<int>(std::min<juce::int64>(257, repeatedPlan.samples - first));
            clockBlock.clear();
            carta::renderDiscPreview(repeatedReopened, repeatedPlan, repeatedRenderer, clockBlock,
                                     0, count, clockPosition, 44100, 2);
            check(clockPosition == first + count, "preview advances through every repeated part");
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < count; ++i)
                    check(std::equal_to<float>{}(
                              clockBlock.getSample(ch, i),
                              expected.getSample(ch, static_cast<int>((first + i) % 160))),
                          "preview preserves sample order across repeated part references");
        }
        const auto clockWav = dir.getChildFile("clock-review.wav");
        check(carta::exportDiscProgramWav(repeatedReopened, 2, clockWav, error),
              "export logical MdDisc program as a review WAV");
        std::unique_ptr<juce::AudioFormatReader> review(formats.createReaderFor(clockWav));
        check(review && review->lengthInSamples == repeatedPlan.samples && review->numChannels == 2,
              "review WAV has the logical playback length and channel count");
        check(review->metadataValues["NumCuePoints"] == "240" &&
                  review->metadataValues["NumCueLabels"] == "239" &&
                  review->metadataValues["Cue0Offset"] == "0" &&
                  review->metadataValues["Cue1Offset"] == "96" &&
                  review->metadataValues["Cue2Offset"] == "160" &&
                  review->metadataValues["CueLabel0Text"] == "Track 1" &&
                  review->metadataValues["CueLabel2Identifier"] == "4" &&
                  review->metadataValues["CueLabel2Text"] == "Track 4",
              "review WAV cues mark logical occurrences and preserve empty titles without "
              "inventing labels");
        juce::AudioBuffer<float> reviewBlock(2, 257);
        for (juce::int64 first = 0; first < repeatedPlan.samples; first += 257) {
            const int count =
                static_cast<int>(std::min<juce::int64>(257, repeatedPlan.samples - first));
            check(review->read(&reviewBlock, 0, count, first, true, true), "read review WAV");
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < count; ++i)
                    check(std::equal_to<float>{}(
                              reviewBlock.getSample(ch, i),
                              expected.getSample(ch, static_cast<int>((first + i) % 160))),
                          "review WAV preserves the authored playback order");
        }
        review.reset();
        const auto originalSize = clockWav.getSize();
        juce::String beforeHash, afterHash;
        check(carta::hashSourceFile(clockWav, beforeHash, error),
              "hash review WAV before cancellation");
        check(!carta::exportDiscProgramWav(repeatedReopened, 2, clockWav, error, {},
                                           [](double progress) { return progress < 0.2; }) &&
                  clockWav.getSize() == originalSize &&
                  carta::hashSourceFile(clockWav, afterHash, error) && beforeHash == afterHash,
              "cancelled review export preserves an existing destination");
        check(!carta::exportDiscProgramWav(repeatedReopened, 2, clockWav, error, {}, {}, true) &&
                  carta::hashSourceFile(clockWav, afterHash, error) && beforeHash == afterHash,
              "no-replace review export preserves an existing destination");
        carta::WavRange reviewedRange{7, 9};
        auto invalidReview = repeatedReopened;
        invalidReview.mdDisc.tracks[0].halfWidthTitle = std::string("bad\0title", 9);
        check(!carta::validateDiscProgramWav(invalidReview, 2, reviewedRange, error) &&
                  error.contains("titles") && reviewedRange.start == 7 && reviewedRange.end == 9,
              "review preflight rejects embedded NUL without changing its output range");
        check(!carta::exportDiscProgramWav(invalidReview, 2, clockWav, error) &&
                  carta::hashSourceFile(clockWav, afterHash, error) && beforeHash == afterHash,
              "invalid review cue metadata cannot replace an existing destination");
        invalidReview = repeatedReopened;
        invalidReview.mdDisc.tracks.clear();
        for (int i = 0; i < 256; ++i)
            invalidReview.mdDisc.tracks.push_back({"too-many-" + std::to_string(i),
                                                   "T",
                                                   "",
                                                   {},
                                                   false,
                                                   false,
                                                   false,
                                                   "",
                                                   {{32, 96}}});
        check(!carta::validateDiscProgramWav(invalidReview, 2, reviewedRange, error) &&
                  error.contains("255") && reviewedRange.start == 7 && reviewedRange.end == 9,
              "review preflight reports the 255-track UTOC limit before requesting a destination");
    }
    {
        constexpr int frames = 64;
        carta::Session source;
        for (int t = 0; t < 3; ++t) {
            auto trackAudio = std::make_shared<carta::Audio>();
            trackAudio->sampleRate = carta::sessionRate;
            trackAudio->samples.setSize(1, frames);
            for (int i = 0; i < frames; ++i)
                trackAudio->samples.setSample(0, i,
                                              static_cast<float>((i * (t + 3)) % 19 - 9) * 0.01f);
            source.tracks.push_back({"Track " + juce::String(t + 1),
                                     {{trackAudio, 0, 0, frames / carta::sessionRate}}});
        }
        source.tracks[1].solo = true;
        auto exportMix = source;
        check(carta::limitToTrackIndices(exportMix, {0, 2}),
              "isolate nonadjacent selected tracks for export");
        carta::Session audition;
        carta::PlaybackRange range;
        juce::String auditionError;
        check(carta::prepareIndependentTrackAudition(source, {0, frames, 0, 2}, {0, 2}, false,
                                                     audition, range, auditionError),
              "prepare nonadjacent audition for export comparison");
        const auto output = dir.getChildFile("independent-track-mix.wav");
        bool clipped = false;
        check(carta::exportWavRange(exportMix, 2, {0, frames}, output, clipped, error, {}, {},
                                    {carta::WavEncoding::float32, false}),
              "export nonadjacent track mix as float WAV");
        const auto decoded = carta::loadAudio(formats, output, error);
        check(decoded && decoded->numSamples() == frames, "read nonadjacent track WAV");
        juce::AudioBuffer<float> played(2, frames);
        played.clear();
        double position = range.start;
        carta::renderPlayback(audition, played, 0, frames, position, carta::sessionRate, 2, range);
        for (int ch = 0; ch < 2; ++ch)
            check(std::memcmp(played.getReadPointer(ch), decoded->channelData(ch),
                              frames * sizeof(float)) == 0,
                  "independent-track playback and float WAV match bit-for-bit despite an "
                  "unselected solo");
    }
    {
        constexpr int frames = 32768;
        const double rate = carta::sessionRate;
        const auto sourceFile = dir.getChildFile("edit-stability-source.wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream = sourceFile.createOutputStream();
            const auto options = juce::AudioFormatWriterOptions{}
                                     .withSampleRate(rate)
                                     .withNumChannels(2)
                                     .withBitsPerSample(16);
            auto writer = wav.createWriterFor(stream, options);
            check(writer != nullptr, "create edit-stability source");
            juce::AudioBuffer<float> input(2, frames);
            for (int channel = 0; channel < 2; ++channel)
                for (int i = 0; i < frames; ++i)
                    input.setSample(
                        channel, i,
                        static_cast<float>(
                            (i * (channel == 0 ? 137 : 271) + channel * 331) % 16001 - 8000) /
                            32768.0f);
            check(writer->writeFromAudioSampleBuffer(input, 0, frames),
                  "write edit-stability source");
        }
        auto source = carta::loadAudio(formats, sourceFile, error);
        check(source != nullptr, "load edit-stability source");
        carta::Session edited;
        edited.tracks.push_back({"Samples", {{source, 0, 0, frames / rate}}});
        for (int i = 0; i < 80; ++i) {
            const double at = static_cast<double>(((i * 37) % 80 + 1) * 397) / rate;
            auto& clips = edited.tracks[0].clips;
            int hit = -1;
            for (int c = 0; c < static_cast<int>(clips.size()); ++c)
                if (at > clips[static_cast<size_t>(c)].start &&
                    at < clips[static_cast<size_t>(c)].start +
                             clips[static_cast<size_t>(c)].duration) {
                    hit = c;
                    break;
                }
            check(hit >= 0 && edited.split(0, hit, at), "split exact sample in varied order");
        }
        const auto verify = [&] {
            juce::AudioBuffer<float> rendered(2, frames), chunked(2, frames);
            rendered.clear();
            chunked.clear();
            edited.render(rendered, 0, frames, 0, rate, 2);
            carta::ResamplingContext kernels;
            edited.prepareResampling(kernels, rate);
            for (int at = 0; at < frames; at += 257)
                edited.render(chunked, at, std::min(257, frames - at),
                              static_cast<double>(at) / rate, rate, 2, &kernels);
            for (int channel = 0; channel < 2; ++channel)
                for (int i = 0; i < frames; ++i) {
                    const auto actual = rendered.getSample(channel, i),
                               expected = source->channelData(channel)[i];
                    if (std::memcmp(&actual, &expected, sizeof(float)) != 0) {
                        std::cerr << "Edit stability mismatch at channel " << channel << ", sample "
                                  << i << ": " << actual << " vs " << expected << '\n';
                        for (const auto& clip : edited.tracks[0].clips)
                            if (std::abs(clip.start * rate - i) < 2 ||
                                std::abs((clip.start + clip.duration) * rate - i) < 2)
                                std::cerr << std::setprecision(17) << "  clip start "
                                          << clip.start * rate << " end "
                                          << (clip.start + clip.duration) * rate << " source "
                                          << clip.offset * rate << '\n';
                        return false;
                    }
                    const auto partial = chunked.getSample(channel, i);
                    if (std::memcmp(&partial, &expected, sizeof(float)) != 0) {
                        std::cerr << "Chunked edit mismatch at channel " << channel << ", sample "
                                  << i << '\n';
                        return false;
                    }
                }
            return true;
        };
        check(verify(), "sample-exact render after varied splits");
        const auto path = dir.getChildFile("edit-stability.carta");
        for (int pass = 0; pass < 4; ++pass) {
            for (size_t c = 1; c < edited.tracks[0].clips.size(); c += 7) {
                auto& clip = edited.tracks[0].clips[c];
                check(carta::nudgeClipEdge(clip, false, 1, error) &&
                          carta::nudgeClipEdge(clip, false, -1, error),
                      "reversible one-sample trim cycle");
            }
            check(verify(), "sample-exact render after trim cycles");
            check(carta::saveSession(edited, 2, path, error), "save repeated edit project");
            carta::Session reopened;
            int channels = 0;
            check(carta::loadSession(reopened, channels, formats, path, error) && channels == 2,
                  "reopen repeated edit project");
            edited = std::move(reopened);
            check(verify(), "sample-exact render after repeated project round trips");
        }
        const auto exported = dir.getChildFile("edit-stability-export.wav");
        bool clipped = false;
        check(carta::exportWavRange(edited, 2, {0, frames}, exported, clipped, error, {}, {},
                                    {carta::WavEncoding::float32, false}) &&
                  !clipped,
              "export edited arrangement as float WAV");
        const auto renderedFile = carta::loadAudio(formats, exported, error);
        check(renderedFile && renderedFile->numSamples() == frames, "reload edited float WAV");
        for (int channel = 0; channel < 2; ++channel)
            check(std::memcmp(renderedFile->channelData(channel), source->channelData(channel),
                              static_cast<size_t>(frames) * sizeof(float)) == 0,
                  "export preserves every source sample after repeated edits and reloads");
        carta::Session rippled;
        rippled.tracks.push_back({"Ripple", {{source, 0, 0, frames / rate}}});
        std::vector<int> sampleMap(frames);
        std::iota(sampleMap.begin(), sampleMap.end(), 0);
        std::vector<juce::int64> markers{0, 8000, 16000, 24000};
        for (size_t i = 0; i < markers.size(); ++i)
            rippled.markers.push_back({static_cast<double>(markers[i]) / rate,
                                       "Marker " + juce::String(static_cast<int>(i + 1))});
        const auto verifyRipple = [&] {
            if (carta::projectEndSample(rippled) != static_cast<juce::int64>(sampleMap.size()) ||
                rippled.markers.size() != markers.size())
                return false;
            for (size_t i = 0; i < markers.size(); ++i)
                if (std::llround(rippled.markers[i].position * rate) != markers[i])
                    return false;
            juce::AudioBuffer<float> output(2, static_cast<int>(sampleMap.size()));
            output.clear();
            rippled.render(output, 0, static_cast<int>(sampleMap.size()), 0, rate, 2);
            for (int channel = 0; channel < 2; ++channel)
                for (size_t i = 0; i < sampleMap.size(); ++i) {
                    const float actual = output.getSample(channel, static_cast<int>(i));
                    const float expected =
                        sampleMap[i] < 0 ? 0.0f : source->channelData(channel)[sampleMap[i]];
                    if (std::memcmp(&actual, &expected, sizeof(float)) != 0) {
                        std::cerr << "Ripple mismatch at channel " << channel << ", sample " << i
                                  << ": " << actual << " vs " << expected << '\n';
                        return false;
                    }
                }
            return true;
        };
        const auto rippleProject = dir.getChildFile("ripple-stability.carta");
        for (int pass = 0; pass < 20; ++pass) {
            const int start = 150 + pass * 251, length = 1 + (pass * 17) % 63, end = start + length;
            const carta::TimeSelection range{start, end, 0, 0};
            check(carta::insertGap(rippled, range, true, error),
                  "insert gap during repeated edit sequence");
            sampleMap.insert(sampleMap.begin() + start, static_cast<size_t>(length), -1);
            for (auto& marker : markers)
                if (marker >= start)
                    marker += length;
            check(verifyRipple(), "inserted gap audio and markers match independent reference");
            check(carta::rippleRemoveRange(rippled, range, true, &error), "close the inserted gap");
            sampleMap.erase(sampleMap.begin() + start, sampleMap.begin() + end);
            for (auto& marker : markers)
                if (marker >= end)
                    marker -= length;
            check(verifyRipple(), "closing gap restores reference audio and markers");
            check(carta::rippleRemoveRange(rippled, range, true, &error),
                  "ripple-delete selected source samples");
            sampleMap.erase(sampleMap.begin() + start, sampleMap.begin() + end);
            for (auto& marker : markers)
                if (marker >= end)
                    marker -= length;
            check(verifyRipple(),
                  "ripple deletion matches independent sample and marker reference");
            if (pass % 4 == 3) {
                check(carta::saveSession(rippled, 2, rippleProject, error),
                      "save repeated ripple project");
                carta::Session reopened;
                int channels = 0;
                check(carta::loadSession(reopened, channels, formats, rippleProject, error) &&
                          channels == 2,
                      "reopen repeated ripple project");
                rippled = std::move(reopened);
                check(verifyRipple(), "ripple audio and markers survive project round trip");
            }
        }
        carta::Session multi;
        multi.tracks.push_back({"First", {{source, 0, 0, (frames - 1000) / rate}}});
        multi.tracks.push_back({"Second", {{source, 5 / rate, 0, (frames - 5) / rate}}});
        multi.tracks[1].gain = 0.5f;
        std::vector<std::vector<int>> tracks(2, std::vector<int>(frames, -1));
        for (int i = 0; i < frames - 1000; ++i)
            tracks[0][static_cast<size_t>(i)] = i;
        for (int i = 5; i < frames; ++i)
            tracks[1][static_cast<size_t>(i)] = i - 5;
        std::vector<juce::int64> multiMarkers{0, 8000, 16000, 24000};
        for (size_t i = 0; i < multiMarkers.size(); ++i)
            multi.markers.push_back({static_cast<double>(multiMarkers[i]) / rate,
                                     "Track " + juce::String(static_cast<int>(i + 1))});
        const auto verifyMulti = [&] {
            if (carta::projectEndSample(multi) != static_cast<juce::int64>(tracks[0].size()) ||
                multi.markers.size() != multiMarkers.size())
                return false;
            for (size_t i = 0; i < multiMarkers.size(); ++i)
                if (std::llround(multi.markers[i].position * rate) != multiMarkers[i])
                    return false;
            juce::AudioBuffer<float> output(2, static_cast<int>(tracks[0].size()));
            output.clear();
            multi.render(output, 0, static_cast<int>(tracks[0].size()), 0, rate, 2);
            for (int channel = 0; channel < 2; ++channel)
                for (size_t i = 0; i < tracks[0].size(); ++i) {
                    float expected = 0.0f;
                    if (tracks[0][i] >= 0)
                        expected += source->channelData(channel)[tracks[0][i]];
                    if (tracks[1][i] >= 0)
                        expected += source->channelData(channel)[tracks[1][i]] * 0.5f;
                    const float actual = output.getSample(channel, static_cast<int>(i));
                    if (std::memcmp(&actual, &expected, sizeof(float)) != 0) {
                        std::cerr << "Multitrack ripple mismatch at channel " << channel
                                  << ", sample " << i << ": " << actual << " vs " << expected
                                  << '\n';
                        return false;
                    }
                }
            return true;
        };
        check(verifyMulti(), "initial multitrack mix matches independent reference");
        const auto multiProject = dir.getChildFile("multitrack-ripple-stability.carta");
        for (int pass = 0; pass < 12; ++pass) {
            const int start = 200 + pass * 233, length = 2 + (pass * 13) % 37, end = start + length;
            const carta::TimeSelection range{start, end, 0, 0};
            check(carta::insertGap(multi, range, false, error), "insert gap only on selected row");
            tracks[0].insert(tracks[0].begin() + start, static_cast<size_t>(length), -1);
            tracks[0].resize(tracks[1].size());
            check(verifyMulti(), "selected-row gap leaves other track and markers unchanged");
            check(carta::rippleRemoveRange(multi, range, false, &error),
                  "close gap on selected row");
            tracks[0].erase(tracks[0].begin() + start, tracks[0].begin() + end);
            tracks[0].resize(tracks[1].size(), -1);
            check(verifyMulti(), "selected-row gap closure restores the mix");
            check(carta::rippleRemoveRange(multi, range, false, &error),
                  "ripple selected row only");
            tracks[0].erase(tracks[0].begin() + start, tracks[0].begin() + end);
            tracks[0].resize(tracks[1].size(), -1);
            check(verifyMulti(), "selected-row ripple preserves the other track and markers");
            if (pass % 3 == 2) {
                const int allStart = start + 100, allEnd = allStart + 7;
                const carta::TimeSelection allRange{allStart, allEnd, 0, 0};
                check(carta::rippleRemoveRange(multi, allRange, true, &error), "ripple all rows");
                for (auto& track : tracks)
                    track.erase(track.begin() + allStart, track.begin() + allEnd);
                for (auto& marker : multiMarkers)
                    if (marker >= allEnd)
                        marker -= 7;
                check(verifyMulti(), "all-row ripple moves both tracks and markers consistently");
                check(carta::saveSession(multi, 2, multiProject, error),
                      "save multitrack ripple project");
                carta::Session reopened;
                int channels = 0;
                check(carta::loadSession(reopened, channels, formats, multiProject, error) &&
                          channels == 2,
                      "reopen multitrack ripple project");
                multi = std::move(reopened);
                check(verifyMulti(), "multitrack ripple mix and markers survive reopening");
            }
        }
        const auto collisionStart = multiMarkers[1] - 1, collisionEnd = multiMarkers[2] + 1;
        check(
            !carta::rippleRemoveRange(multi, {collisionStart, collisionEnd, 0, 0}, true, &error) &&
                error.contains("two track markers") && verifyMulti(),
            "multitrack ripple rejects marker collisions without changing audio or markers");
        const auto multiExport = dir.getChildFile("multitrack-ripple-export.wav");
        check(carta::exportWavRange(multi, 2, {0, static_cast<juce::int64>(tracks[0].size())},
                                    multiExport, clipped, error, {}, {},
                                    {carta::WavEncoding::float32, false}) &&
                  !clipped,
              "export multitrack ripple mix");
        const auto multiOutput = carta::loadAudio(formats, multiExport, error);
        check(multiOutput && multiOutput->numSamples() == static_cast<int>(tracks[0].size()),
              "reload multitrack ripple export");
        for (int channel = 0; channel < 2; ++channel)
            for (size_t i = 0; i < tracks[0].size(); ++i) {
                float expected = 0.0f;
                if (tracks[0][i] >= 0)
                    expected += source->channelData(channel)[tracks[0][i]];
                if (tracks[1][i] >= 0)
                    expected += source->channelData(channel)[tracks[1][i]] * 0.5f;
                const float actual = multiOutput->channelData(channel)[i];
                check(std::memcmp(&actual, &expected, sizeof(float)) == 0,
                      "multitrack ripple WAV matches independent mix bit-for-bit");
            }
        constexpr int nativeFrames = 24000, projectFrames = 20000;
        const auto highFile = dir.getChildFile("edit-stability-48k.wav");
        {
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream = highFile.createOutputStream();
            const auto options = juce::AudioFormatWriterOptions{}
                                     .withSampleRate(48000)
                                     .withNumChannels(2)
                                     .withBitsPerSample(16);
            auto writer = wav.createWriterFor(stream, options);
            check(writer != nullptr, "create mixed-rate edit fixture");
            juce::AudioBuffer<float> input(2, nativeFrames);
            for (int channel = 0; channel < 2; ++channel)
                for (int i = 0; i < nativeFrames; ++i)
                    input.setSample(
                        channel, i,
                        static_cast<float>(
                            (i * (channel == 0 ? 193 : 347) + channel * 571) % 14001 - 7000) /
                            32768.0f);
            check(writer->writeFromAudioSampleBuffer(input, 0, nativeFrames),
                  "write mixed-rate edit fixture");
        }
        auto high = carta::loadAudio(formats, highFile, error);
        check(high != nullptr, "load mixed-rate edit fixture");
        carta::Session mixed;
        mixed.tracks.push_back({"Project rate", {{source, 0, 0, projectFrames / rate}}});
        mixed.tracks.push_back({"Source rate", {{high, 137 / rate, 17 / 48000.0, 18000 / rate}}});
        mixed.tracks[0].gain = 0.5f;
        mixed.tracks[1].gain = 0.5f;
        mixed.markers = {{0, "Start"}, {10000 / rate, "Middle"}};
        const auto makeMix = [&](const carta::Session& project, double outputRate) {
            const int count = static_cast<int>(std::ceil(projectFrames * outputRate / rate));
            juce::AudioBuffer<float> output(2, count);
            output.clear();
            project.render(output, 0, count, 0, outputRate, 2);
            return output;
        };
        const auto expectedProject = makeMix(mixed, rate), expectedDevice = makeMix(mixed, 48000);
        {
            // Independent reference: render unmoved clips and render only the
            // selected original clips at a shifted time, then sum the results.
            std::uint32_t randomState = 0x85ac731du;
            const auto next = [&]() {
                randomState = randomState * 1664525u + 1013904223u;
                return randomState;
            };
            carta::ResamplingContext projectContext, deviceContext;
            mixed.prepareResampling(projectContext, rate);
            mixed.prepareResampling(deviceContext, 48000);
            constexpr int groupFrames = 512;
            for (int trial = 0; trial < 16; ++trial) {
                carta::Session original;
                for (int track = 0; track < 4; ++track) {
                    carta::Track row;
                    row.name = "Random track " + juce::String(track + 1);
                    row.gain = 0.2f;
                    for (int index = 0; index < 3; ++index) {
                        const auto media = ((next() >> 16) & 1u) ? source : high;
                        const double start = (10.0 + static_cast<double>(next() % 180u) +
                                              static_cast<double>(next() % 4u) * 0.25) /
                                             rate;
                        const double duration = (70.0 + static_cast<double>(next() % 80u) +
                                                 static_cast<double>(next() % 4u) * 0.25) /
                                                rate;
                        const double offset =
                            (10.0 + static_cast<double>(next() % 500u) + 0.25) / media->sampleRate;
                        row.clips.push_back({media, start, offset, duration});
                        check(carta::setClipEnvelope(row.clips.back(), 0.6f, 5 / rate, 9 / rate),
                              "prepare randomized group-move envelope");
                    }
                    original.tracks.push_back(std::move(row));
                }
                original.tracks[3].clips.push_back({source, 450 / rate, 0, 30 / rate});
                original.markers = {{200.25 / rate, "Fixed"}};
                auto selectedOnly = original, remaining = original;
                for (int track = 0; track < 4; ++track) {
                    selectedOnly.tracks[static_cast<size_t>(track)].clips.clear();
                    remaining.tracks[static_cast<size_t>(track)].clips.clear();
                    for (int index = 0;
                         index <
                         static_cast<int>(original.tracks[static_cast<size_t>(track)].clips.size());
                         ++index) {
                        const bool picked = track < 2 && (index == 0 || index == 2);
                        (picked ? selectedOnly : remaining)
                            .tracks[static_cast<size_t>(track)]
                            .clips.push_back(original.tracks[static_cast<size_t>(track)]
                                                 .clips[static_cast<size_t>(index)]);
                    }
                }
                const juce::int64 delta = 5 + static_cast<juce::int64>(next() % 45u);
                const std::vector<carta::ClipReference> refs{{1, 2}, {0, 2}, {1, 0}, {0, 0}};
                auto moved = original;
                check(carta::moveClipGroup(moved, refs, delta, 1, error),
                      "move randomized nonadjacent mixed-rate clips");
                check(std::abs(moved.markers[0].position * rate - 200.25) < 1e-8,
                      "randomized group move keeps marker position");
                for (const double outputRate : {rate, 48000.0}) {
                    const int count = static_cast<int>(std::ceil(groupFrames * outputRate / rate));
                    juce::AudioBuffer<float> actual(2, count), base(2, count), shifted(2, count);
                    actual.clear();
                    base.clear();
                    shifted.clear();
                    const auto* context = outputRate < 46000 ? &projectContext : &deviceContext;
                    moved.render(actual, 0, count, 0, outputRate, 2, context);
                    remaining.render(base, 0, count, 0, outputRate, 2, context);
                    selectedOnly.render(shifted, 0, count, -static_cast<double>(delta) / rate,
                                        outputRate, 2, context);
                    for (int channel = 0; channel < 2; ++channel)
                        for (int i = 0; i < count; ++i)
                            check(std::abs(actual.getSample(channel, i) -
                                           (base.getSample(channel, i) +
                                            shifted.getSample(channel, i))) < 2e-5f,
                                  "randomized group move matches independent shifted mix");
                    if (trial % 5 == 0) {
                        const auto groupProject = dir.getChildFile("random-group-move.carta");
                        check(carta::saveSession(moved, 2, groupProject, error),
                              "save randomized group move");
                        carta::Session reopened;
                        int reopenedChannels = 0;
                        check(carta::loadSession(reopened, reopenedChannels, formats, groupProject,
                                                 error) &&
                                  reopenedChannels == 2,
                              "reopen randomized group move");
                        check(std::memcmp(&moved.markers[0].position, &reopened.markers[0].position,
                                          sizeof(double)) == 0,
                              "fractional marker position round trips exactly");
                        for (size_t track = 0; track < moved.tracks.size(); ++track)
                            for (size_t clip = 0; clip < moved.tracks[track].clips.size(); ++clip) {
                                const auto& a = moved.tracks[track].clips[clip];
                                const auto& b = reopened.tracks[track].clips[clip];
                                for (const auto& pair :
                                     {std::pair{a.start, b.start}, std::pair{a.offset, b.offset},
                                      std::pair{a.duration, b.duration},
                                      std::pair{a.fadeIn, b.fadeIn},
                                      std::pair{a.fadeOut, b.fadeOut},
                                      std::pair{a.fadeInStart, b.fadeInStart},
                                      std::pair{a.fadeOutEnd, b.fadeOutEnd}})
                                    check(std::memcmp(&pair.first, &pair.second, sizeof(double)) ==
                                              0,
                                          "clip geometry and envelope anchors round trip exactly");
                            }
                        juce::AudioBuffer<float> roundTrip(2, count);
                        roundTrip.clear();
                        reopened.render(roundTrip, 0, count, 0, outputRate, 2, context);
                        for (int channel = 0; channel < 2; ++channel)
                            for (int sample = 0; sample < count; ++sample) {
                                const float a = actual.getSample(channel, sample),
                                            b = roundTrip.getSample(channel, sample);
                                if (std::memcmp(&a, &b, sizeof(float)) != 0) {
                                    std::cerr << "Group round-trip mismatch trial " << trial
                                              << " rate " << outputRate << " channel " << channel
                                              << " sample " << sample << ": "
                                              << std::setprecision(12) << a << " vs " << b << '\n';
                                    for (size_t t = 0; t < moved.tracks.size(); ++t)
                                        for (size_t c = 0; c < moved.tracks[t].clips.size(); ++c) {
                                            const auto& x = moved.tracks[t].clips[c];
                                            const auto& y = reopened.tracks[t].clips[c];
                                            if (std::memcmp(&x.start, &y.start, sizeof(double)) ||
                                                std::memcmp(&x.offset, &y.offset, sizeof(double)) ||
                                                std::memcmp(&x.duration, &y.duration,
                                                            sizeof(double)))
                                                std::cerr << std::setprecision(17) << " track " << t
                                                          << " clip " << c << " start " << x.start
                                                          << "/" << y.start << " offset "
                                                          << x.offset << "/" << y.offset
                                                          << " duration " << x.duration << "/"
                                                          << y.duration << '\n';
                                        }
                                    check(false, "randomized group move survives project round "
                                                 "trip bit-for-bit");
                                }
                            }
                        if (outputRate < 46000) {
                            if (trial == 0) {
                                const auto recoveryPath =
                                    dir.getChildFile("random-group.recovery.carta");
                                check(carta::saveRecoverySession(moved, 2, recoveryPath, error),
                                      "save fractional group recovery snapshot");
                                carta::Session recovered;
                                int recoveryChannels = 0;
                                bool recoveryFlag = false;
                                check(carta::loadSession(recovered, recoveryChannels, formats,
                                                         recoveryPath, error, {}, &recoveryFlag) &&
                                          recoveryFlag && recoveryChannels == 2,
                                      "reopen fractional group recovery snapshot");
                                check(std::memcmp(&moved.tracks[1].clips[1].offset,
                                                  &recovered.tracks[1].clips[1].offset,
                                                  sizeof(double)) == 0 &&
                                          std::memcmp(&moved.markers[0].position,
                                                      &recovered.markers[0].position,
                                                      sizeof(double)) == 0,
                                      "recovery preserves exact fractional clip and marker "
                                      "positions");
                                juce::AudioBuffer<float> recoveryMix(2, count);
                                recoveryMix.clear();
                                recovered.render(recoveryMix, 0, count, 0, outputRate, 2, context);
                                for (int channel = 0; channel < 2; ++channel)
                                    check(std::memcmp(actual.getReadPointer(channel),
                                                      recoveryMix.getReadPointer(channel),
                                                      static_cast<size_t>(count) * sizeof(float)) ==
                                              0,
                                          "recovered fractional group renders bit-for-bit");
                            }
                            const auto file = dir.getChildFile("random-group-move.wav");
                            bool groupClipped = false;
                            check(carta::exportWavRange(reopened, 2, {0, groupFrames}, file,
                                                        groupClipped, error, {}, {},
                                                        {carta::WavEncoding::float32, false}) &&
                                      !groupClipped,
                                  "export randomized group move");
                            const auto groupExport = carta::loadAudio(formats, file, error);
                            check(groupExport && groupExport->numSamples() == groupFrames,
                                  "reopen randomized group WAV");
                            for (int channel = 0; channel < 2; ++channel)
                                check(std::memcmp(actual.getReadPointer(channel),
                                                  groupExport->channelData(channel),
                                                  static_cast<size_t>(groupFrames) *
                                                      sizeof(float)) == 0,
                                      "randomized group WAV matches rendered mix bit-for-bit");
                        }
                    }
                }
            }
        }
        {
            auto swapped = mixed;
            check(carta::swapStereoSourceChannels(swapped, 1, error),
                  "swap stereo routing on imported mixed-rate source");
            const auto expected = makeMix(swapped, rate);
            const auto swapProject = dir.getChildFile("stereo-channel-swap.carta");
            check(carta::saveSession(swapped, 2, swapProject, error),
                  "save swapped source-channel routing");
            carta::Session reopened;
            int channels = 0;
            check(carta::loadSession(reopened, channels, formats, swapProject, error) &&
                      channels == 2 && reopened.tracks[1].routing.size() == 2,
                  "reopen swapped source-channel routing");
            const auto actual = makeMix(reopened, rate);
            for (int channel = 0; channel < 2; ++channel)
                check(std::memcmp(actual.getReadPointer(channel), expected.getReadPointer(channel),
                                  static_cast<size_t>(projectFrames) * sizeof(float)) == 0,
                      "swapped routing survives reopen bit-for-bit");
            const auto outputFile = dir.getChildFile("stereo-channel-swap.wav");
            check(carta::exportWavRange(reopened, 2, {0, projectFrames}, outputFile, clipped, error,
                                        {}, {}, {carta::WavEncoding::float32, false}),
                  "export swapped source-channel routing");
            const auto output = carta::loadAudio(formats, outputFile, error);
            check(output && output->numSamples() == projectFrames, "read swapped routing export");
            for (int channel = 0; channel < 2; ++channel)
                check(std::memcmp(output->channelData(channel), expected.getReadPointer(channel),
                                  static_cast<size_t>(projectFrames) * sizeof(float)) == 0,
                      "swapped routing WAV matches playback mix bit-for-bit");
            check(carta::foldStereoTrackToMono(swapped, 1, error), "fold mixed-rate stereo track");
            const auto folded = makeMix(swapped, rate);
            check(carta::saveSession(swapped, 2, swapProject, error), "save folded routing");
            check(carta::loadSession(reopened, channels, formats, swapProject, error),
                  "reopen folded routing");
            const auto foldedAgain = makeMix(reopened, rate);
            for (int channel = 0; channel < 2; ++channel)
                check(std::memcmp(foldedAgain.getReadPointer(channel),
                                  folded.getReadPointer(channel),
                                  static_cast<size_t>(projectFrames) * sizeof(float)) == 0,
                      "folded routing survives reopen bit-for-bit");
        }
        const auto verifyMixed = [&] {
            if (mixed.markers.size() != 2 || std::llround(mixed.markers[0].position * rate) != 0 ||
                std::llround(mixed.markers[1].position * rate) != 10000)
                return false;
            for (const auto outputRate : {rate, 48000.0}) {
                const auto output = makeMix(mixed, outputRate);
                const auto& expected = outputRate < 46000 ? expectedProject : expectedDevice;
                for (int channel = 0; channel < 2; ++channel)
                    for (int i = 0; i < output.getNumSamples(); ++i) {
                        const auto actual = output.getSample(channel, i),
                                   original = expected.getSample(channel, i);
                        if (std::memcmp(&actual, &original, sizeof(float)) != 0) {
                            std::cerr << "Mixed-rate edit mismatch at " << outputRate
                                      << " Hz, channel " << channel << ", sample " << i << ": "
                                      << actual << " vs " << original << '\n';
                            return false;
                        }
                    }
            }
            return true;
        };
        for (int track = 0; track < 2; ++track)
            for (int i = 0; i < 60; ++i) {
                const double at = static_cast<double>(((i * 37) % 60 + 1) * 239) / rate;
                auto& clips = mixed.tracks[static_cast<size_t>(track)].clips;
                int hit = -1;
                for (int c = 0; c < static_cast<int>(clips.size()); ++c)
                    if (at > clips[static_cast<size_t>(c)].start &&
                        at < clips[static_cast<size_t>(c)].start +
                                 clips[static_cast<size_t>(c)].duration) {
                        hit = c;
                        break;
                    }
                check(hit >= 0 && mixed.split(track, hit, at),
                      "split mixed-rate clips on project sample boundaries");
            }
        check(verifyMixed(), "mixed-rate audio is unchanged by varied splits");
        const auto mixedProject = dir.getChildFile("mixed-rate-edit-stability.carta");
        for (int pass = 0; pass < 4; ++pass) {
            const carta::TimeSelection gap{7001 + pass * 71, 7020 + pass * 71, 0, 1};
            check(carta::insertGap(mixed, gap, true, error) &&
                      carta::rippleRemoveRange(mixed, gap, true, &error),
                  "insert and close mixed-rate gap");
            check(verifyMixed(), "mixed-rate audio is unchanged after gap closure");
            check(carta::saveSession(mixed, 2, mixedProject, error), "save mixed-rate edits");
            carta::Session reopened;
            int channels = 0;
            check(carta::loadSession(reopened, channels, formats, mixedProject, error) &&
                      channels == 2,
                  "reopen mixed-rate edits");
            mixed = std::move(reopened);
            check(verifyMixed(), "mixed-rate audio is unchanged after project round trip");
        }
        {
            auto moved = mixed;
            const carta::TimeSelection selection{3000, 5000, 0, 1};
            check(carta::moveRange(moved, selection, 0, 9000, error),
                  "move split mixed-rate stereo selection together");
            const auto movedProject = makeMix(moved, rate), movedDevice = makeMix(moved, 48000);
            for (int channel = 0; channel < 2; ++channel)
                for (int i = 0; i < projectFrames; ++i) {
                    const float expected = i >= 3000 && i < 5000 ? 0.0f
                                           : i >= 9000 && i < 11000
                                               ? expectedProject.getSample(channel, i) +
                                                     expectedProject.getSample(channel, i - 6000)
                                               : expectedProject.getSample(channel, i);
                    const float actual = movedProject.getSample(channel, i);
                    check(std::abs(actual - expected) < 2e-5f,
                          "mixed-rate range move preserves channels, timing, silence and additive "
                          "overlap");
                }
            check(moved.markers.size() == 2 &&
                      std::llround(moved.markers[1].position * rate) == 10000,
                  "mixed-rate range move leaves track markers fixed");
            const auto moveProject = dir.getChildFile("mixed-rate-move.carta");
            check(carta::saveSession(moved, 2, moveProject, error), "save mixed-rate range move");
            carta::Session reopened;
            int channels = 0;
            check(carta::loadSession(reopened, channels, formats, moveProject, error) &&
                      channels == 2,
                  "reopen mixed-rate range move");
            for (double outputRate : {rate, 48000.0}) {
                const auto actual = makeMix(reopened, outputRate);
                const auto& expected = outputRate < 46000 ? movedProject : movedDevice;
                for (int channel = 0; channel < 2; ++channel)
                    check(std::memcmp(
                              actual.getReadPointer(channel), expected.getReadPointer(channel),
                              static_cast<size_t>(actual.getNumSamples()) * sizeof(float)) == 0,
                          "moved mix survives reopen bit-for-bit at project and device rates");
            }
            const auto moveExport = dir.getChildFile("mixed-rate-move.wav");
            check(carta::exportWavRange(reopened, 2, {0, projectFrames}, moveExport, clipped, error,
                                        {}, {}, {carta::WavEncoding::float32, false}),
                  "export moved mixed-rate master");
            const auto output = carta::loadAudio(formats, moveExport, error);
            check(output && output->numSamples() == projectFrames, "reload moved master");
            for (int channel = 0; channel < 2; ++channel)
                check(std::memcmp(output->channelData(channel),
                                  movedProject.getReadPointer(channel),
                                  static_cast<size_t>(projectFrames) * sizeof(float)) == 0,
                      "moved float WAV matches rendered master bit-for-bit");
        }
        const auto mixedOutputFile = dir.getChildFile("mixed-rate-edit-export.wav");
        check(carta::exportWavRange(mixed, 2, {0, projectFrames}, mixedOutputFile, clipped, error,
                                    {}, {}, {carta::WavEncoding::float32, false}) &&
                  !clipped,
              "export mixed-rate edited arrangement");
        const auto mixedOutput = carta::loadAudio(formats, mixedOutputFile, error);
        check(mixedOutput && mixedOutput->numSamples() == projectFrames,
              "reload mixed-rate edited WAV");
        for (int channel = 0; channel < 2; ++channel)
            check(std::memcmp(mixedOutput->channelData(channel),
                              expectedProject.getReadPointer(channel),
                              static_cast<size_t>(projectFrames) * sizeof(float)) == 0,
                  "mixed-rate edited WAV matches the original project mix bit-for-bit");
    }
    {
        const auto invalidFile = dir.getChildFile("invalid-float.wav");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = invalidFile.createOutputStream();
        const auto opts =
            juce::AudioFormatWriterOptions{}
                .withSampleRate(44100)
                .withNumChannels(1)
                .withBitsPerSample(32)
                .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        auto writer = wav.createWriterFor(stream, opts);
        check(writer != nullptr, "create floating WAV fixture");
        juce::AudioBuffer<float> data(1, 32);
        data.clear();
        data.setSample(0, 5, std::numeric_limits<float>::quiet_NaN());
        check(writer->writeFromAudioSampleBuffer(data, 0, 32), "write non-finite audio fixture");
        writer.reset();
        check(!carta::loadAudio(formats, invalidFile, error), "reject non-finite source samples");
    }
    {
        const auto known = dir.getChildFile("hash-fixture.txt");
        check(known.replaceWithText("abc", false, false, "\n"), "write hash fixture");
        juce::String digest;
        check(carta::hashSourceFile(known, digest, error) &&
                  digest == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "buffered source hash matches SHA-256 reference");
#if JUCE_MAC || JUCE_LINUX
        {
            const auto fifo = dir.getChildFile("source-fifo");
            check(::mkfifo(fifo.getFullPathName().toRawUTF8(), 0600) == 0,
                  "create source FIFO fixture");
            const auto child = ::fork();
            check(child >= 0, "fork FIFO check");
            if (child == 0) {
                ::alarm(3);
                juce::String hash, failure;
                const bool accepted = carta::hashSourceFile(fifo, hash, failure);
                auto fifoAudio = std::make_shared<carta::Audio>();
                fifoAudio->file = fifo;
                fifoAudio->sourceHash = digest;
                std::vector<carta::SourceCheck> results;
                const bool checked = carta::verifyTrackSources({{fifoAudio, 1}}, results, failure);
                ::_exit(!accepted && checked && results.size() == 1 &&
                                results[0].status != carta::SourceStatus::matches
                            ? 0
                            : 1);
            }
            int status = 0;
            check(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                      WEXITSTATUS(status) == 0,
                  "source hashing rejects a FIFO promptly without a writer");
            check(fifo.deleteFile(), "remove source FIFO fixture");
            const carta::FileObservation original(known);
            const int fd = ::open(known.getFullPathName().toRawUTF8(), O_RDONLY | O_NONBLOCK);
            check(fd >= 0 && original.isRegularFile() && original.matchesDescriptor(fd),
                  "opened regular source matches its path observation");
            check(::close(fd) == 0, "close source observation fixture");
            const auto other = dir.getChildFile("other-observation.txt");
            check(other.replaceWithText("abc", false, false, "\n"),
                  "write other observation fixture");
            const int otherFd = ::open(other.getFullPathName().toRawUTF8(), O_RDONLY | O_NONBLOCK);
            check(otherFd >= 0 && !original.matchesDescriptor(otherFd),
                  "different opened file cannot pass the path identity check");
            check(::close(otherFd) == 0, "close other observation fixture");
        }
#endif
        {
            const auto different = dir.getChildFile("different-source.txt");
            check(different.replaceWithText("xyz", false, false, "\n"),
                  "write changed source fixture");
            auto matched = std::make_shared<carta::Audio>();
            matched->file = known;
            matched->sourceHash = digest;
            auto changed = std::make_shared<carta::Audio>();
            changed->file = different;
            changed->sourceHash = digest;
            auto missing = std::make_shared<carta::Audio>();
            missing->file = dir.getChildFile("missing-source.txt");
            missing->sourceHash = digest;
            auto unknown = std::make_shared<carta::Audio>();
            unknown->file = known;
            std::vector<carta::TrackSource> sources{
                {matched, 2}, {changed, 1}, {missing, 1}, {unknown, 1}, {nullptr, 1}};
            std::vector<carta::SourceCheck> checks;
            double last = -1;
            bool monotonic = true;
            check(carta::verifyTrackSources(sources, checks, error,
                                            [&](double value) {
                                                if (value < last)
                                                    monotonic = false;
                                                last = value;
                                                return true;
                                            }) &&
                      checks.size() == 5 && monotonic && std::abs(last - 1) < 1e-12,
                  "source verification completes with monotonic progress");
            {
                carta::SourceVerificationCache cache;
                auto first = sources[0].audio, second = sources[1].audio;
                carta::Session sourceSession;
                sourceSession.tracks.push_back(
                    {"First", {{first, 0, 0, 1}, {first, 1, 0, 1}, {second, 2, 0, 1}}});
                sourceSession.tracks.push_back({"Shared", {{first, 0, 0, 1}}});
                const auto unique = carta::sessionSources(sourceSession);
                check(unique.size() == 2 && unique[0].audio == first && unique[0].clipCount == 3 &&
                          unique[1].audio == second && unique[1].clipCount == 1,
                      "session verification deduplicates shared source identities and counts all "
                      "clips");
                const std::vector<carta::SourceCheck> statuses{
                    {carta::SourceStatus::missing, "Missing"},
                    {carta::SourceStatus::matches, "Matches"}};
                check(!cache.lookup(first) && cache.record(unique, statuses, "test time") &&
                          cache.trackHasIssue(sourceSession.tracks[0]) &&
                          cache.trackHasIssue(sourceSession.tracks[1]),
                      "last failed source check appears on every track sharing that source");
                juce::String checkedAt;
                check(cache.lookup(first, &checkedAt) &&
                          cache.lookup(first)->status == carta::SourceStatus::missing &&
                          checkedAt == "test time",
                      "source cache retains the result and check time");
                check(carta::describeTimeline(sourceSession, {}, 1, -1, -1, 0, false, &cache)
                              .contains("source needs attention") &&
                          carta::makeNavigationRows(sourceSession, &cache)[0].description.contains(
                              "source needs attention"),
                      "source check issues appear in accessible track descriptions");
                check(!cache.record(unique, {}, "bad") &&
                          cache.trackHasIssue(sourceSession.tracks[1]),
                      "invalid result batches preserve earlier checks");
                const auto oldHash = first->sourceHash;
                first->sourceHash = "changed identity";
                check(!cache.lookup(first) && !cache.trackHasIssue(sourceSession.tracks[1]),
                      "changed source identity does not inherit a stale result");
                first->sourceHash = oldHash;
                check(cache.record({{first, 1}}, {{carta::SourceStatus::matches, "Restored"}},
                                   "later") &&
                          !cache.trackHasIssue(sourceSession.tracks[0]),
                      "a later successful check clears the shared warning");
                std::weak_ptr<carta::Audio> weak;
                {
                    auto temporary = std::make_shared<carta::Audio>();
                    weak = temporary;
                    check(cache.record({{temporary, 1}},
                                       {{carta::SourceStatus::missing, "Missing"}}, "now"),
                          "record temporary source");
                }
                check(weak.expired(), "source status cache does not keep decoded media alive");
                cache.prune();
            }

            check(checks[0].status == carta::SourceStatus::matches &&
                      checks[1].status == carta::SourceStatus::changed &&
                      checks[2].status == carta::SourceStatus::missing &&
                      checks[3].status == carta::SourceStatus::unverifiable &&
                      checks[4].status == carta::SourceStatus::unreadable,
                  "source verification distinguishes matching, changed, missing, unverified and "
                  "unavailable files");
            {
                carta::SourceVerificationCache cache;
                carta::Track missingTrack;
                missingTrack.clips.push_back({missing, 0, 0, 1});
                check(cache.record({{missing, 1}}, {checks[2]}, "earlier") &&
                          cache.trackHasIssue(missingTrack),
                      "missing file verification sets track warning");
                check(missing->file.replaceWithText("abc", false, false, "\n"),
                      "restore matching source fixture");
                std::vector<carta::SourceCheck> restoredCheck;
                check(carta::verifyTrackSources({{missing, 1}}, restoredCheck, error) &&
                          restoredCheck.size() == 1 &&
                          restoredCheck[0].status == carta::SourceStatus::matches &&
                          cache.record({{missing, 1}}, restoredCheck, "later") &&
                          !cache.trackHasIssue(missingTrack),
                      "rechecking an actually restored file clears its last-check warning");
            }

            std::vector<carta::SourceCheck> retained{{carta::SourceStatus::changed, "retained"}};
            check(!carta::verifyTrackSources(sources, retained, error,
                                             [](double value) { return value < 0.1; }) &&
                      error == "Source check cancelled." && retained.size() == 1 &&
                      retained[0].detail == "retained",
                  "cancelled source verification leaves prior results unchanged");
            const auto unstableFile = dir.getChildFile("unstable-source.txt");
            check(unstableFile.replaceWithText("abc", false, false, "\n"),
                  "write unstable source fixture");
            auto unstable = std::make_shared<carta::Audio>();
            unstable->file = unstableFile;
            unstable->sourceHash = digest;
            bool modified = false;
            check(carta::verifyTrackSources({{unstable, 1}}, checks, error,
                                            [&](double value) {
                                                if (value >= 1 && !modified) {
                                                    modified = unstableFile.replaceWithText(
                                                        "different length", false, false, "\n");
                                                }
                                                return true;
                                            }) &&
                      modified && checks.size() == 1 &&
                      checks[0].status == carta::SourceStatus::changedDuringCheck,
                  "source modified while hashing is reported as unstable");
#if JUCE_MAC || JUCE_LINUX
            const auto replacedFile = dir.getChildFile("replaced-during-check.txt");
            check(replacedFile.replaceWithText("abc", false, false, "\n"),
                  "write replacement-check fixture");
            auto replacedAudio = std::make_shared<carta::Audio>();
            replacedAudio->file = replacedFile;
            replacedAudio->sourceHash = digest;
            const auto savedTime = replacedFile.getLastModificationTime();
            const auto savedSize = replacedFile.getSize();
            bool replaced = false, sameMetadata = false;
            check(carta::verifyTrackSources(
                      {{replacedAudio, 1}}, checks, error,
                      [&](double value) {
                          if (value >= 1 && !replaced) {
                              replaced = replacedFile.replaceWithText("xyz", false, false, "\n");
                              sameMetadata = replaced &&
                                             replacedFile.setLastModificationTime(savedTime) &&
                                             replacedFile.getSize() == savedSize &&
                                             replacedFile.getLastModificationTime() == savedTime;
                          }
                          return true;
                      }) &&
                      replaced && sameMetadata && checks.size() == 1 &&
                      checks[0].status == carta::SourceStatus::changedDuringCheck,
                  "source verification detects a same-metadata path replacement after hashing");
#endif
        }
        const auto longFile = dir.getChildFile("chunked.wav");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = longFile.createOutputStream();
        auto opts = juce::AudioFormatWriterOptions{}
                        .withSampleRate(44100)
                        .withNumChannels(4)
                        .withBitsPerSample(24);
        auto writer = wav.createWriterFor(stream, opts);
        check(writer != nullptr, "create chunked source fixture");
        juce::AudioBuffer<float> block(4, 70000);
        for (int ch = 0; ch < 4; ++ch)
            for (int i = 0; i < 70000; ++i)
                block.setSample(ch, i,
                                0.01f * static_cast<float>(ch + 1) +
                                    0.000001f * static_cast<float>(i % 100));
        check(writer->writeFromAudioSampleBuffer(block, 0, 70000), "write chunked source fixture");
        writer.reset();
#if JUCE_MAC || JUCE_LINUX
        {
            const auto race = dir.getChildFile("replaced-before-decoder.wav");
            check(longFile.copyFileTo(race), "prepare decoder replacement fixture");
            const auto child = ::fork();
            check(child >= 0, "fork decoder replacement check");
            if (child == 0) {
                ::alarm(3);
                int boundaries = 0;
                bool replaced = false;
                juce::String failure;
                const auto imported = carta::loadAudio(formats, race, failure, [&](double value) {
                    if (std::abs(value - 0.20) < 1e-12 && ++boundaries == 2)
                        replaced = race.deleteFile() &&
                                   ::mkfifo(race.getFullPathName().toRawUTF8(), 0600) == 0;
                    return true;
                });
                ::_exit(replaced && !imported &&
                                failure.contains("changed while it was being imported")
                            ? 0
                            : 1);
            }
            int status = 0;
            check(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                      WEXITSTATUS(status) == 0,
                  "import rejects FIFO substituted between hashing and decoder open without "
                  "blocking");
            check(race.deleteFile(), "remove decoder replacement FIFO");
        }
#endif
        {
            juce::AiffAudioFormat aiff;
            juce::FlacAudioFormat flac;
            for (auto* format : std::array<juce::AudioFormat*, 2>{&aiff, &flac}) {
                const auto path =
                    dir.getChildFile(format == &aiff ? "stream-check.aiff" : "stream-check.flac");
                std::unique_ptr<juce::OutputStream> output = path.createOutputStream();
                auto options = juce::AudioFormatWriterOptions{}
                                   .withSampleRate(44100)
                                   .withNumChannels(1)
                                   .withBitsPerSample(16);
                auto encoded = format->createWriterFor(output, options);
                check(encoded != nullptr, "create alternate-format fixture");
                juce::AudioBuffer<float> signal(1, 128);
                for (int i = 0; i < 128; ++i)
                    signal.setSample(0, i, (i % 2 ? -0.25f : 0.5f));
                check(encoded->writeFromAudioSampleBuffer(signal, 0, 128),
                      "write alternate-format fixture");
                encoded.reset();
                const auto decoded = carta::loadAudio(formats, path, error);
                check(decoded && decoded->numSamples() == 128 && decoded->numChannels() == 1 &&
                          near(decoded->channelData(0)[0], 0.5f) &&
                          near(decoded->channelData(0)[1], -0.25f),
                      "descriptor-backed decoding preserves alternate audio formats");
            }
        }
#if JUCE_USE_OGGVORBIS
        {
            const auto path = dir.getChildFile("stream-check.ogg");
            juce::OggVorbisAudioFormat ogg;
            std::unique_ptr<juce::OutputStream> output = path.createOutputStream();
            auto options = juce::AudioFormatWriterOptions{}
                               .withSampleRate(44100)
                               .withNumChannels(2)
                               .withBitsPerSample(16)
                               .withQualityOptionIndex(6);
            auto encoded = ogg.createWriterFor(output, options);
            check(encoded != nullptr, "create Vorbis fixture");
            juce::AudioBuffer<float> signal(2, 4410);
            for (int i = 0; i < signal.getNumSamples(); ++i) {
                const auto value =
                    0.4f * std::sin(static_cast<float>(i) * 2.0f * juce::MathConstants<float>::pi *
                                    440.0f / 44100.0f);
                signal.setSample(0, i, value);
                signal.setSample(1, i, -value);
            }
            check(encoded->writeFromAudioSampleBuffer(signal, 0, signal.getNumSamples()),
                  "write Vorbis fixture");
            encoded.reset();
            const auto decoded = carta::loadAudio(formats, path, error);
            check(decoded && decoded->numSamples() == signal.getNumSamples() &&
                      decoded->numChannels() == 2,
                  "import stream-backed Vorbis fixture");
            double squareError = 0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < signal.getNumSamples(); ++i) {
                    const double delta = decoded->channelData(ch)[i] - signal.getSample(ch, i);
                    squareError += delta * delta;
                }
            check(std::sqrt(squareError / (2 * signal.getNumSamples())) < 0.03,
                  "Vorbis import retains the expected stereo tone within lossy-codec tolerance");
#if JUCE_MAC || JUCE_LINUX
            const auto monoPath = dir.getChildFile("stream-check-mono.ogg");
            std::unique_ptr<juce::OutputStream> monoOutput = monoPath.createOutputStream();
            auto monoOptions = juce::AudioFormatWriterOptions{}
                                   .withSampleRate(48000)
                                   .withNumChannels(1)
                                   .withBitsPerSample(16)
                                   .withQualityOptionIndex(6);
            auto monoWriter = ogg.createWriterFor(monoOutput, monoOptions);
            check(monoWriter != nullptr, "create changed-layout Vorbis fixture");
            juce::AudioBuffer<float> monoSignal(1, 512);
            monoSignal.clear();
            check(monoWriter->writeFromAudioSampleBuffer(monoSignal, 0, 512),
                  "write changed-layout Vorbis fixture");
            monoWriter.reset();
            juce::MemoryBlock stereoBytes, monoBytes;
            check(path.loadFileAsData(stereoBytes) && monoPath.loadFileAsData(monoBytes),
                  "read Vorbis link fixtures");
            for (int kind = 0; kind < 3; ++kind) {
                const auto chain = dir.getChildFile("chained-" + juce::String(kind) + ".ogg");
                const auto& first = kind == 1 ? monoBytes : stereoBytes;
                const auto& second = kind == 2 ? stereoBytes : kind == 1 ? stereoBytes : monoBytes;
                check(chain.replaceWithData(first.getData(), first.getSize()) &&
                          chain.appendData(second.getData(), second.getSize()),
                      "write chained Vorbis fixture");
                const auto child = ::fork();
                check(child >= 0, "fork chained Vorbis check");
                if (child == 0) {
                    ::alarm(3);
                    juce::String failure;
                    const auto imported = carta::loadAudio(formats, chain, failure);
                    ::_exit(!imported && failure.contains("multiple logical streams") ? 0 : 1);
                }
                int childStatus = 0;
                check(::waitpid(child, &childStatus, 0) == child && WIFEXITED(childStatus) &&
                          WEXITSTATUS(childStatus) == 0,
                      "chained Vorbis rejects safely before decoding");
            }
#endif
        }
#endif
        double lastProgress = -1;
        bool monotonic = true;
        const auto chunked = carta::loadAudio(formats, longFile, error, [&](double progress) {
            if (progress < lastProgress)
                monotonic = false;
            lastProgress = progress;
            return true;
        });
        check(chunked && chunked->samples.getNumChannels() == 4 &&
                  chunked->samples.getNumSamples() == 70000 && monotonic &&
                  std::abs(lastProgress - 1) < 1e-12,
              "chunked import reports complete monotonic progress");
        for (int ch = 0; ch < 4; ++ch)
            for (int sample : {0, 65535, 65536, 69999})
                check(near(chunked->samples.getSample(ch, sample), block.getSample(ch, sample)),
                      "chunked import preserves every channel across boundaries");
        for (bool replace : {false, true}) {
            const auto changing = dir.getChildFile(replace ? "replaced-during-import.wav"
                                                           : "modified-during-import.wav");
            check(longFile.copyFileTo(changing), "prepare same-metadata import fixture");
            juce::MemoryBlock contents;
            check(changing.loadFileAsData(contents) && contents.getSize() > 100,
                  "read source bytes for import-race fixture");
            auto* bytes = static_cast<unsigned char*>(contents.getData());
            bytes[100] ^= 0x7f;
            const auto originalTime = changing.getLastModificationTime();
            const auto originalSize = changing.getSize();
            bool altered = false, metadataPreserved = false;
            const auto imported = carta::loadAudio(
                formats, changing, error,
                [&](double progress) {
                    if (progress >= 0.55 && !altered) {
                        if (replace)
                            altered =
                                changing.replaceWithData(contents.getData(), contents.getSize());
                        else {
                            auto output = changing.createOutputStream();
                            if (output && output->setPosition(100) &&
                                output->writeByte(static_cast<char>(bytes[100]))) {
                                output->flush();
                                altered = output->getStatus().wasOk();
                            }
                        }
                        metadataPreserved = altered &&
                                            changing.setLastModificationTime(originalTime) &&
                                            changing.getSize() == originalSize &&
                                            changing.getLastModificationTime() == originalTime;
                    }
                    return true;
                },
                replace ? 0 : std::numeric_limits<juce::int64>::max());
            if (!(altered && metadataPreserved && !imported &&
                  error.contains("changed while it was being imported")))
                std::cerr << "Import race fixture replacement=" << replace << " altered=" << altered
                          << " metadata=" << metadataPreserved
                          << " imported=" << static_cast<bool>(imported) << " error=" << error
                          << '\n';
            check(altered && metadataPreserved && !imported &&
                      error.contains("changed while it was being imported"),
                  "same-size source changes with restored timestamps are rejected after decoding");
        }
        {
            const auto source = dir.getChildFile("changing-project-source.wav"),
                       project = dir.getChildFile("changing-source-project.carta");
            check(longFile.copyFileTo(source), "prepare changing project source");
            const auto changingAudio = carta::loadAudio(formats, source, error);
            check(changingAudio != nullptr, "import changing project source");
            carta::Session saved;
            saved.tracks.push_back({"Source", {{changingAudio, 0, 0, changingAudio->length()}}});
            check(carta::saveSession(saved, 2, project, error),
                  "save project before source changes");
            juce::MemoryBlock contents;
            check(source.loadFileAsData(contents) && contents.getSize() > 100,
                  "read changing project source");
            static_cast<unsigned char*>(contents.getData())[100] ^= 0x7f;
            const auto originalTime = source.getLastModificationTime();
            const auto originalSize = source.getSize();
            bool altered = false, metadataPreserved = false;
            carta::Session retained;
            retained.tracks.push_back({"Retained", {}});
            int retainedChannels = 1;
            carta::SourceRelinkRequest request;
            const bool opened = carta::loadSession(
                retained, retainedChannels, formats, project, error,
                [&](double progress) {
                    if (progress >= 0.95 * 0.55 && !altered) {
                        altered = source.replaceWithData(contents.getData(), contents.getSize());
                        metadataPreserved = altered &&
                                            source.setLastModificationTime(originalTime) &&
                                            source.getSize() == originalSize &&
                                            source.getLastModificationTime() == originalTime;
                    }
                    return true;
                },
                nullptr, nullptr, &request);
            check(altered && metadataPreserved && !opened && retained.tracks.size() == 1 &&
                      retained.tracks[0].name == "Retained" && retainedChannels == 1 &&
                      request.original == source &&
                      request.expectedHash == changingAudio->sourceHash,
                  "changing source during project open retains the old session and offers verified "
                  "relinking");
        }
#if JUCE_MAC || JUCE_LINUX
        {
            const auto project = dir.getChildFile("changing-project-file.carta");
            carta::Session saved;
            saved.tracks.push_back({"Source", {}});
            check(carta::saveSession(saved, 2, project, error), "save project-file race fixture");
            juce::MemoryBlock contents;
            check(project.loadFileAsData(contents) && contents.getSize() > 10,
                  "read project-file race fixture");
            auto* bytes = static_cast<char*>(contents.getData());
            for (size_t i = 0; i < contents.getSize(); ++i)
                if (bytes[i] == 'S') {
                    bytes[i] = 'T';
                    break;
                }
            const auto originalTime = project.getLastModificationTime();
            const auto originalSize = project.getSize();
            bool altered = false, metadataPreserved = false;
            int callbacks = 0;
            carta::Session retained;
            retained.tracks.push_back({"Retained", {}});
            int channels = 1;
            const bool opened =
                carta::loadSession(retained, channels, formats, project, error, [&](double) {
                    if (++callbacks == 2) {
                        altered = project.replaceWithData(contents.getData(), contents.getSize());
                        metadataPreserved = altered &&
                                            project.setLastModificationTime(originalTime) &&
                                            project.getSize() == originalSize &&
                                            project.getLastModificationTime() == originalTime;
                    }
                    return true;
                });
            check(altered && metadataPreserved && !opened &&
                      error.contains("changed while it was being read") && channels == 1 &&
                      retained.tracks.size() == 1 && retained.tracks[0].name == "Retained",
                  "project replacement during reading preserves the current session");
        }
        {
            const auto source = dir.getChildFile("changing-save-source.wav"),
                       project = dir.getChildFile("changing-save-project.carta");
            check(longFile.copyFileTo(source), "prepare changing source for save");
            const auto sourceAudio = carta::loadAudio(formats, source, error);
            check(sourceAudio != nullptr, "load source for safe-save fixture");
            carta::Session saved;
            saved.tracks.push_back({"Source", {{sourceAudio, 0, 0, sourceAudio->length()}}});
            check(project.replaceWithText("existing project sentinel"), "write safe-save sentinel");
            juce::MemoryBlock contents;
            check(source.loadFileAsData(contents) && contents.getSize() > 100,
                  "read safe-save source");
            static_cast<unsigned char*>(contents.getData())[100] ^= 0x7f;
            const auto originalTime = source.getLastModificationTime();
            const auto originalSize = source.getSize();
            bool replaced = false, sameMetadata = false;
            const bool written = carta::saveSession(saved, 2, project, error, [&](double progress) {
                if (progress >= 0.9 && !replaced) {
                    replaced = source.replaceWithData(contents.getData(), contents.getSize());
                    sameMetadata = replaced && source.setLastModificationTime(originalTime) &&
                                   source.getSize() == originalSize &&
                                   source.getLastModificationTime() == originalTime;
                }
                return true;
            });
            check(replaced && sameMetadata && !written &&
                      error.contains("changed while being checked") &&
                      project.loadFileAsString() == "existing project sentinel",
                  "project save rejects same-metadata source replacement during hashing and "
                  "preserves the existing project");
            check(longFile.copyFileTo(source), "restore source before final-commit race");
            replaced = false;
            sameMetadata = false;
            const auto restoredTime = source.getLastModificationTime();
            const bool lateWritten =
                carta::saveSession(saved, 2, project, error, [&](double progress) {
                    if (progress >= 0.99 && !replaced) {
                        replaced = source.replaceWithData(contents.getData(), contents.getSize());
                        sameMetadata = replaced && source.setLastModificationTime(restoredTime) &&
                                       source.getSize() == originalSize &&
                                       source.getLastModificationTime() == restoredTime;
                    }
                    return true;
                });
            check(replaced && sameMetadata && !lateWritten &&
                      error.contains("changed before project save completed") &&
                      project.loadFileAsString() == "existing project sentinel",
                  "project save detects source replacement at the final cancellation point and "
                  "preserves the destination");
        }
#endif
        {
            const auto matchingLong = dir.getChildFile("chunked-replacement.wav");
            check(longFile.copyFileTo(matchingLong), "copy multiblock replacement fixture");
            juce::String relinkError;
            check(carta::loadVerifiedReplacement(formats, chunked, matchingLong, relinkError) !=
                      nullptr,
                  "live relink compares all channels across multiple PCM blocks");
            auto changed = std::make_shared<carta::Audio>(*chunked);
            changed->samples.setSample(3, 65536, 0.99f);
            check(!carta::loadVerifiedReplacement(formats, changed, matchingLong, relinkError) &&
                      relinkError.contains("different audio samples"),
                  "live relink detects a mismatch after the first PCM block");
        }

        {
            auto smallBudget = std::make_shared<carta::WaveformPeakBudget>(32);
            auto streamed = carta::loadAudio(formats, longFile, error, {}, 0, {}, smallBudget);
            carta::Audio rebuilt = *chunked;
            auto referenceBudget = std::make_shared<carta::WaveformPeakBudget>(32);
            check(streamed &&
                      carta::buildWaveformPeaks(rebuilt, 16u * 1024u * 1024u, {}, referenceBudget),
                  "build constrained streamed and reference peak caches");
            check(streamed->peakCache->blockSize == rebuilt.peakCache->blockSize &&
                      streamed->peakCache->blockSize > 65536 &&
                      streamed->peakCache->peaks == rebuilt.peakCache->peaks &&
                      streamed->peakCache->levels == rebuilt.peakCache->levels &&
                      smallBudget->allocated() == referenceBudget->allocated(),
                  "streamed peaks match reference across decode chunks under a constrained budget");
            streamed.reset();
            check(smallBudget->allocated() == 0,
                  "streamed peak reservation is released with the source");
            auto emptyBudget = std::make_shared<carta::WaveformPeakBudget>(0);
            auto uncached = carta::loadAudio(formats, longFile, error, {}, 0, {}, emptyBudget);
            check(uncached && uncached->peakCache->peaks.empty() && emptyBudget->allocated() == 0 &&
                      std::memcmp(uncached->channelData(3) + 65536, chunked->channelData(3) + 65536,
                                  sizeof(float)) == 0,
                  "mapped import remains usable when the peak budget is exhausted");
            for (const double threshold : {0.3, 0.86}) {
                auto cancelledBudget = std::make_shared<carta::WaveformPeakBudget>(4096);
                check(!carta::loadAudio(
                          formats, longFile, error,
                          [threshold](double progress) { return progress < threshold; }, 0, {},
                          cancelledBudget) &&
                          cancelledBudget->allocated() == 0,
                      "cancelled streamed import releases its peak budget");
            }
        }
        {
            auto mapped = carta::loadAudio(formats, longFile, error, {}, 0);
            check(mapped && mapped->mapped && mapped->numChannels() == 4 &&
                      mapped->numSamples() == 70000,
                  "large import uses mapped planar audio");
            check(mapped->peakCache->blockSize == chunked->peakCache->blockSize &&
                      mapped->peakCache->peaks == chunked->peakCache->peaks &&
                      mapped->peakCache->levels == chunked->peakCache->levels,
                  "streamed mapped peak hierarchy matches the in-memory import");
            const auto temporary = mapped->mapped->temporary.getFile();
#if JUCE_MAC || JUCE_LINUX
            check(!temporary.exists() && mapped->sourceHash == chunked->sourceHash,
                  "mapped source is unlinked while open and has identical hash");
#else
            check(temporary.existsAsFile() && mapped->sourceHash == chunked->sourceHash,
                  "mapped source has temporary storage and identical hash");
#endif
            for (int ch = 0; ch < 4; ++ch)
                for (int sample : {0, 255, 256, 65535, 65536, 69999})
                    check(std::memcmp(mapped->channelData(ch) + sample,
                                      chunked->channelData(ch) + sample, sizeof(float)) == 0,
                          "mapped samples match in-memory import across chunk boundaries");
            for (int ch = 0; ch < 4; ++ch)
                for (auto range : {juce::Range<int>{0, 69999}, juce::Range<int>{251, 65800},
                                   juce::Range<int>{65534, 65539}})
                    check(carta::waveformRange(*mapped, ch, range.getStart(), range.getEnd()) ==
                              carta::waveformRange(*chunked, ch, range.getStart(), range.getEnd()),
                          "mapped waveform peaks match memory");
            carta::Clip mappedClip{mapped, 0, 0, mapped->length()},
                memoryClip{chunked, 0, 0, chunked->length()};
            mappedClip.fadeIn = memoryClip.fadeIn = 0.5;
            mappedClip.gain = memoryClip.gain = 0.7f;
            for (int ch = 0; ch < 4; ++ch) {
                std::vector<carta::WaveformColumn> mappedColumns, memoryColumns;
                check(carta::exactWaveformColumns(mappedClip, ch, 0, 2, 0, 20, 0, 20, mappedColumns,
                                                  {}, true) &&
                          carta::exactWaveformColumns(memoryClip, ch, 0, 2, 0, 20, 0, 20,
                                                      memoryColumns, {}, true),
                      "calculate RMS for mapped and in-memory channels");
                for (size_t column = 0; column < mappedColumns.size(); ++column)
                    check(mappedColumns[column].hasRms == memoryColumns[column].hasRms &&
                              std::memcmp(&mappedColumns[column].rms, &memoryColumns[column].rms,
                                          sizeof(float)) == 0,
                          "mapped and in-memory RMS columns match bit-for-bit");
            }
            auto mappedTile = carta::makeSpectrogramTile(*mapped, 64000, 64, 64),
                 memoryTile = carta::makeSpectrogramTile(*chunked, 64000, 64, 64);
            check(mappedTile.isValid() && memoryTile.isValid(),
                  "mapped and in-memory spectral tiles compute");
            for (int y = 0; y < mappedTile.getHeight(); ++y)
                for (int x = 0; x < mappedTile.getWidth(); ++x)
                    check(mappedTile.getPixelAt(x, y) == memoryTile.getPixelAt(x, y),
                          "mapped and in-memory spectral tiles match");
            carta::Session mappedSession, memorySession;
            mappedSession.tracks.push_back({"mapped", {{mapped, 0, 0, mapped->length()}}});
            memorySession.tracks.push_back({"memory", {{chunked, 0, 0, chunked->length()}}});
            juce::AudioBuffer<float> mappedOut(2, 1024), memoryOut(2, 1024);
            mappedOut.clear();
            memoryOut.clear();
            mappedSession.render(mappedOut, 0, 1024, 1.48, 44100, 2);
            memorySession.render(memoryOut, 0, 1024, 1.48, 44100, 2);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 1024; ++i)
                    check(std::memcmp(mappedOut.getReadPointer(ch) + i,
                                      memoryOut.getReadPointer(ch) + i, sizeof(float)) == 0,
                          "mapped render matches memory");
            mappedSession.tracks.clear();
            mappedClip.audio.reset();
            mapped.reset();
            check(!temporary.exists(), "temporary storage removed after last reference");
        }
        for (double threshold : {0.0, 0.3, 0.6, 0.9}) {
            check(!carta::loadAudio(
                      formats, longFile, error,
                      [threshold](double progress) { return progress < threshold; }, 0) &&
                      error == "Import cancelled.",
                  "disk-backed import cancels without returning partial audio");
        }
        for (double threshold : {0.0, 0.3, 0.6, 0.9}) {
            check(
                !carta::loadAudio(formats, longFile, error,
                                  [threshold](double progress) { return progress < threshold; }) &&
                    error == "Import cancelled.",
                "import cancels without returning partial audio");
        }
    }
    {
        constexpr int frames = 8192;
        auto silence = std::make_shared<carta::Audio>();
        silence->sampleRate = carta::sessionRate;
        silence->samples.setSize(2, frames);
        silence->samples.clear();
        carta::Session mix;
        mix.tracks.push_back({"silence", {{silence, 0, 0, frames / carta::sessionRate}}});
        for (auto encoding : {carta::WavEncoding::pcm16, carta::WavEncoding::pcm24}) {
            const carta::WavOptions opts{encoding, true};
            const auto suffix = encoding == carta::WavEncoding::pcm16 ? "16" : "24";
            const auto whole = dir.getChildFile(juce::String("dither-") + suffix + "-whole.wav");
            const auto first = dir.getChildFile(juce::String("dither-") + suffix + "-first.wav");
            const auto second = dir.getChildFile(juce::String("dither-") + suffix + "-second.wav");
            bool clipped = false;
            check(carta::exportWavRange(mix, 2, {0, frames}, whole, clipped, error, {}, {}, opts) &&
                      !clipped,
                  "export dithered silence");
            check(carta::exportWavRange(mix, 2, {0, 3456}, first, clipped, error, {}, {}, opts) &&
                      carta::exportWavRange(mix, 2, {3456, frames}, second, clipped, error, {}, {},
                                            opts),
                  "export adjacent dithered regions");
            auto all = carta::loadAudio(formats, whole, error),
                 a = carta::loadAudio(formats, first, error),
                 b = carta::loadAudio(formats, second, error);
            check(all && a && b && all->numSamples() == frames, "read dithered PCM output");
            int differentChannels = 0, nonzero = 0;
            double sum = 0, energy = 0;
            const double scale = encoding == carta::WavEncoding::pcm16 ? 32768.0 : 8388608.0;
            for (int i = 0; i < frames; ++i)
                for (int ch = 0; ch < 2; ++ch) {
                    const float sample = all->channelData(ch)[i],
                                region = (i < 3456 ? a->channelData(ch)[i]
                                                   : b->channelData(ch)[i - 3456]);
                    check(std::memcmp(&sample, &region, sizeof(float)) == 0,
                          "dither matches across region and buffer boundaries");
                    if (ch == 0) {
                        const double unit = sample * scale;
                        sum += unit;
                        energy += unit * unit;
                        if (std::abs(unit) > 0.1)
                            ++nonzero;
                    } else if (std::abs(sample - all->channelData(0)[i]) > 0)
                        ++differentChannels;
                }
            check(nonzero > 500 && differentChannels > 500 && std::abs(sum / frames) < 0.05 &&
                      energy / frames > 0.15 && energy / frames < 0.35,
                  "TPDF quantization produces independent, near-zero-mean one-LSB noise");
            const auto repeat = dir.getChildFile(juce::String("dither-") + suffix + "-repeat.wav");
            check(carta::exportWavRange(mix, 2, {0, frames}, repeat, clipped, error, {}, {}, opts),
                  "repeat dither export");
            juce::MemoryBlock bytes1, bytes2;
            check(whole.loadFileAsData(bytes1) && repeat.loadFileAsData(bytes2) && bytes1 == bytes2,
                  "dither output is deterministic");
        }
        bool clipped = false;
        const auto plain = dir.getChildFile("plain16.wav");
        check(carta::exportWavRange(mix, 2, {0, frames}, plain, clipped, error, {}, {},
                                    {carta::WavEncoding::pcm16, false}),
              "export undithered 16-bit PCM");
        std::unique_ptr<juce::AudioFormatReader> pcmReader(formats.createReaderFor(plain));
        check(pcmReader && !pcmReader->usesFloatingPointData && pcmReader->bitsPerSample == 16,
              "PCM WAV reports 16-bit integer encoding");
        auto plainRead = carta::loadAudio(formats, plain, error);
        check(plainRead && plainRead->samples.getMagnitude(0, 0, frames) < 1e-12f,
              "undithered silence stays exact zero");
        for (auto encoding : {carta::WavEncoding::pcm16, carta::WavEncoding::pcm24}) {
            const int bits = encoding == carta::WavEncoding::pcm16 ? 16 : 24;
            const float scale = static_cast<float>(1 << (bits - 1));
            const std::vector<int> integers = {-(1 << (bits - 1)),   -12345, -2, -1, 0, 1, 2, 12345,
                                               (1 << (bits - 1)) - 1};
            auto exact = std::make_shared<carta::Audio>();
            exact->sampleRate = carta::sessionRate;
            exact->samples.setSize(1, static_cast<int>(integers.size()));
            for (int i = 0; i < static_cast<int>(integers.size()); ++i)
                exact->samples.setSample(
                    0, i, static_cast<float>(integers[static_cast<size_t>(i)]) / scale);
            carta::Session exactMix;
            exactMix.tracks.push_back(
                {"Exact PCM",
                 {{exact, 0, 0, static_cast<double>(integers.size()) / carta::sessionRate}}});
            const auto output = dir.getChildFile("exact-" + juce::String(bits) + ".wav");
            check(carta::exportWavRange(exactMix, 1, {0, static_cast<juce::int64>(integers.size())},
                                        output, clipped, error, {}, {}, {encoding, false}) &&
                      !clipped,
                  "export representable PCM samples without dither");
            auto decoded = carta::loadAudio(formats, output, error);
            check(decoded && decoded->numSamples() == static_cast<int>(integers.size()),
                  "read representable PCM samples");
            if (decoded)
                for (int i = 0; i < static_cast<int>(integers.size()); ++i)
                    check(std::memcmp(decoded->channelData(0) + i, exact->channelData(0) + i,
                                      sizeof(float)) == 0,
                          "undithered PCM preserves representable positive and negative samples");
        }
        for (auto encoding : {carta::WavEncoding::pcm16, carta::WavEncoding::pcm24}) {
            const int bits = encoding == carta::WavEncoding::pcm16 ? 16 : 24;
            const auto scale = static_cast<juce::int64>(1) << (bits - 1);
            const std::vector<float> values = {
                -1.0f,
                std::nextafter(-1.0f, -2.0f),
                -0.5f,
                0.5f,
                static_cast<float>(1.0 - 1.0 / static_cast<double>(scale)),
                static_cast<float>(1.0 - 0.5 / static_cast<double>(scale)),
                1.0f};
            auto boundary = std::make_shared<carta::Audio>();
            boundary->sampleRate = carta::sessionRate;
            boundary->samples.setSize(1, static_cast<int>(values.size()));
            for (int i = 0; i < static_cast<int>(values.size()); ++i)
                boundary->samples.setSample(0, i, values[static_cast<size_t>(i)]);
            carta::Session boundaryMix;
            boundaryMix.tracks.push_back(
                {"PCM boundaries",
                 {{boundary, 0, 0, static_cast<double>(values.size()) / carta::sessionRate}}});
            const carta::WavOptions options{encoding, false};
            carta::ExportPreflight boundaryCheck;
            check(carta::scanExportPreflight(boundaryMix, 1,
                                             {0, static_cast<juce::int64>(values.size())},
                                             boundaryCheck, error, {}, options),
                  "scan undithered PCM quantization boundaries");
            juce::int64 count = 0, first = -1;
            for (int i = 0; i < static_cast<int>(values.size()); ++i) {
                const juce::int64 rounded = static_cast<juce::int64>(
                    std::llround(static_cast<double>(values[static_cast<size_t>(i)]) *
                                 static_cast<double>(scale)));
                const bool expected = std::abs(values[static_cast<size_t>(i)]) > 1.0f ||
                                      rounded < -scale || rounded >= scale;
                if (expected) {
                    ++count;
                    if (first < 0)
                        first = i;
                }
                const auto output = dir.getChildFile("boundary-" + juce::String(bits) + ".wav");
                bool limited = false;
                check(carta::exportWavRange(boundaryMix, 1, {i, i + 1}, output, limited, error, {},
                                            {}, options) &&
                          limited == expected,
                      "undithered PCM writer reports rounded values outside the integer range");
                auto decoded = carta::loadAudio(formats, output, error);
                const float expectedSample =
                    static_cast<float>(std::clamp(rounded, -scale, scale - 1)) /
                    static_cast<float>(scale);
                check(decoded &&
                          std::memcmp(decoded->channelData(0), &expectedSample, sizeof(float)) == 0,
                      "undithered PCM boundary uses rounded and saturated integer output");
            }
            check(boundaryCheck.clippedSamples[0] == count &&
                      boundaryCheck.firstClipped[0] == first,
                  "undithered PCM preflight matches integer boundary clipping and first position");
        }
        auto loud = std::make_shared<carta::Audio>();
        loud->sampleRate = carta::sessionRate;
        loud->samples.setSize(1, 4);
        loud->samples.setSample(0, 0, 1.25f);
        loud->samples.setSample(0, 1, -1.5f);
        loud->samples.setSample(0, 2, 0.5f);
        loud->samples.setSample(0, 3, 0);
        carta::Session high;
        high.tracks.push_back({"overload", {{loud, 0, 0, 4 / carta::sessionRate}}});
        const auto floats = dir.getChildFile("float.wav");
        check(carta::exportWavRange(high, 1, {0, 4}, floats, clipped, error, {}, {},
                                    {carta::WavEncoding::float32, false}) &&
                  !clipped,
              "float WAV permits headroom above 0 dBFS");
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(floats));
        check(reader && reader->usesFloatingPointData && reader->bitsPerSample == 32,
              "float WAV reports 32-bit float encoding");
        auto floatsRead = carta::loadAudio(formats, floats, error);
        check(floatsRead && near(floatsRead->channelData(0)[0], 1.25f) &&
                  near(floatsRead->channelData(0)[1], -1.5f),
              "float WAV preserves over-range samples");
        const auto integer = dir.getChildFile("clipped16.wav");
        check(carta::exportWavRange(high, 1, {0, 4}, integer, clipped, error, {}, {},
                                    {carta::WavEncoding::pcm16, false}) &&
                  clipped,
              "integer WAV detects overload");
        check(carta::exportWavRange(high, 1, {0, 4}, integer, clipped, error, {}, {},
                                    {carta::WavEncoding::pcm16, true}) &&
                  clipped,
              "dithered integer WAV detects overload");
        carta::ExportPreflight highCheck;
        check(carta::scanExportPreflight(high, 1, {0, 4}, highCheck, error, {},
                                         {carta::WavEncoding::pcm16, false}) &&
                  near(highCheck.peak[0], 1.5f) && highCheck.clippedSamples[0] == 2 &&
                  highCheck.firstClipped[0] == 0,
              "PCM preflight counts overloaded samples and their first absolute position");
        check(carta::scanExportPreflight(high, 1, {1, 4}, highCheck, error, {},
                                         {carta::WavEncoding::pcm16, false}) &&
                  highCheck.clippedSamples[0] == 1 && highCheck.firstClipped[0] == 1,
              "PCM preflight respects the selected range and absolute positions");
        check(carta::scanExportPreflight(high, 1, {0, 4}, highCheck, error, {},
                                         {carta::WavEncoding::float32, false}) &&
                  highCheck.clippedSamples[0] == 0 && near(highCheck.peak[0], 1.5f),
              "float preflight reports peak but does not classify headroom as clipping");
        const auto savedPreflight = highCheck;
        check(!carta::scanExportPreflight(high, 1, {0, 4}, highCheck, error,
                                          [](double) { return false; }) &&
                  highCheck.channels == savedPreflight.channels &&
                  near(highCheck.peak[0], savedPreflight.peak[0]),
              "cancelled export preflight preserves the prior result");
        constexpr int edgeFrames = 32;
        auto edgeAudio = std::make_shared<carta::Audio>();
        edgeAudio->sampleRate = carta::sessionRate;
        edgeAudio->samples.setSize(2, edgeFrames);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < edgeFrames; ++i)
                edgeAudio->samples.setSample(ch, i,
                                             i % 4 == 0   ? 1.0f
                                             : i % 4 == 1 ? -1.0f
                                             : i % 4 == 2 ? 0.5f
                                                          : -1.1f);
        carta::Session edgeMix;
        edgeMix.tracks.push_back({"Edges", {{edgeAudio, 0, 0, edgeFrames / carta::sessionRate}}});
        for (auto encoding : {carta::WavEncoding::pcm16, carta::WavEncoding::pcm24}) {
            const carta::WavOptions options{encoding, true};
            carta::ExportPreflight edgeCheck;
            check(carta::scanExportPreflight(edgeMix, 2, {0, edgeFrames}, edgeCheck, error, {},
                                             options),
                  "scan dithered PCM boundaries");
            for (int channel = 0; channel < 2; ++channel) {
                auto isolated = std::make_shared<carta::Audio>();
                isolated->sampleRate = carta::sessionRate;
                isolated->samples.setSize(2, edgeFrames);
                isolated->samples.clear();
                for (int i = 0; i < edgeFrames; ++i)
                    isolated->samples.setSample(channel, i,
                                                edgeAudio->samples.getSample(channel, i));
                carta::Session isolatedMix;
                isolatedMix.tracks.push_back(
                    {"Isolated", {{isolated, 0, 0, edgeFrames / carta::sessionRate}}});
                juce::int64 count = 0, first = -1;
                for (int i = 0; i < edgeFrames; ++i) {
                    bool frameClipped = false;
                    check(carta::exportWavRange(isolatedMix, 2, {i, i + 1}, integer, frameClipped,
                                                error, {}, {}, options),
                          "export isolated dithered sample");
                    if (frameClipped) {
                        ++count;
                        if (first < 0)
                            first = i;
                    }
                }
                check(
                    count == edgeCheck.clippedSamples[channel] &&
                        first == edgeCheck.firstClipped[channel],
                    "dither preflight counts and first positions match actual per-channel exports");
            }
        }
        check(!carta::validateWavRange(mix, 1, {0, 1600000000LL}, error,
                                       {carta::WavEncoding::pcm24, false}) &&
                  carta::validateWavRange(mix, 1, {0, 1600000000LL}, error,
                                          {carta::WavEncoding::pcm16, false}),
              "WAV size limit depends on sample format");
        check(!carta::validateWavRange(mix, 2, {0, frames}, error,
                                       {carta::WavEncoding::float32, true}),
              "float format rejects dither setting");
        silence->samples.setSample(0, 12, std::numeric_limits<float>::quiet_NaN());
        float peak = 0;
        juce::MemoryBlock preserved, afterFailure;
        check(plain.loadFileAsData(preserved), "read original PCM destination");
        check(!carta::scanPeak(mix, 2, {0, frames}, peak, error) && error.contains("non-finite"),
              "preflight rejects non-finite mix");
        check(!carta::exportWavRange(mix, 2, {0, frames}, plain, clipped, error) &&
                  error.contains("non-finite") && plain.loadFileAsData(afterFailure) &&
                  afterFailure == preserved,
              "non-finite export preserves destination");
    }
    auto loaded = carta::loadAudio(formats, wavfile, error);
    check(loaded && loaded->samples.getNumChannels() == 4, "read all source channels");
    check(loaded->sourceFormat.isNotEmpty() && loaded->sourceBitsPerSample == 24 &&
              loaded->numChannels() == 4 && loaded->numSamples() == 128,
          "retain reader-reported source metadata");
    {
        const auto parent = dir.getChildFile("portable-parent"),
                   sourceDirA = dir.getChildFile("portable-source-a"),
                   sourceDirB = dir.getChildFile("portable-音源 b");
        check(parent.createDirectory().wasOk() && sourceDirA.createDirectory().wasOk() &&
                  sourceDirB.createDirectory().wasOk(),
              "create portable project fixture directories");
        const auto sourceA = sourceDirA.getChildFile("same name.wav"),
                   sourceB = sourceDirB.getChildFile("other name.wav");
        check(wavfile.copyFileTo(sourceA) && wavfile.copyFileTo(sourceB),
              "prepare portable sources with identical content and distinct names");
        auto audioA = carta::loadAudio(formats, sourceA, error),
             audioB = carta::loadAudio(formats, sourceB, error);
        check(audioA && audioB, "load portable source fixtures");
        carta::Session portable;
        portable.tracks.push_back({"Portable",
                                   {{audioA, 0, 0, audioA->length()},
                                    {audioB, 128 / carta::sessionRate, 0, audioB->length()},
                                    {audioA, 256 / carta::sessionRate, 0, audioA->length()}}});
        portable.tracks[0].clips.push_back({audioB, 384 / carta::sessionRate, 0, audioB->length()});
        confirmDefaultRouting(portable.tracks[0]);
        portable.tracks[0].clips[1].name = "Named clip";
        portable.markers = {{100 / carta::sessionRate, "Second"}};
        const auto occupied = parent.getChildFile("Carta portable");
        check(occupied.createDirectory().wasOk() &&
                  occupied.getChildFile("keep.txt").replaceWithText("keep"),
              "reserve existing portable folder");
        juce::File cancelled;
        check(!carta::exportPortableSession(portable, 2, parent, cancelled, error,
                                            [](double progress) { return progress < 0.1; }) &&
                  cancelled.isDirectory() && !cancelled.getChildFile("Project.carta").exists() &&
                  sourceA.existsAsFile(),
              "cancelled portable export leaves original audio and no project file");
        for (double threshold : {0.75, 0.99}) {
            juce::File partial;
            check(!carta::exportPortableSession(
                      portable, 2, parent, partial, error,
                      [threshold](double progress) { return progress < threshold; }) &&
                      partial.isDirectory() && !partial.getChildFile("Project.carta").exists() &&
                      sourceA.existsAsFile() && sourceB.existsAsFile(),
                  "cancellation during verification or before project commit leaves originals and "
                  "no published project");
        }
        juce::File racedProject;
        bool wroteProject = false;
        check(!carta::exportPortableSession(
                  portable, 2, parent, racedProject, error,
                  [&](double progress) {
                      if (!wroteProject && progress >= 0.7 && racedProject.isDirectory())
                          wroteProject = racedProject.getChildFile("Project.carta")
                                             .replaceWithText("existing project");
                      return true;
                  }) &&
                  wroteProject &&
                  racedProject.getChildFile("Project.carta").loadFileAsString() ==
                      "existing project",
              "portable project commit never replaces a concurrently created project file");
        juce::File racedMedia;
        bool wroteMedia = false;
        check(!carta::exportPortableSession(
                  portable, 2, parent, racedMedia, error,
                  [&](double) {
                      if (!wroteMedia && racedMedia != juce::File{} &&
                          racedMedia.getChildFile("Media").isDirectory())
                          wroteMedia = racedMedia.getChildFile("Media/0001 - same name.wav")
                                           .replaceWithText("existing media");
                      return true;
                  }) &&
                  wroteMedia &&
                  racedMedia.getChildFile("Media/0001 - same name.wav").loadFileAsString() ==
                      "existing media",
              "portable media commit never replaces a concurrently created file");
        juce::MemoryBlock originalSource;
        check(sourceA.loadFileAsData(originalSource), "read portable source identity");
        juce::MemoryBlock changedSource(originalSource);
        static_cast<char*>(changedSource.getData())[changedSource.getSize() - 1] ^= 1;
        check(sourceA.replaceWithData(changedSource.getData(), changedSource.getSize()),
              "change portable source after import");
        juce::File rejected;
        check(!carta::exportPortableSession(portable, 2, parent, rejected, error) &&
                  error.contains("changed") && !rejected.getChildFile("Project.carta").exists(),
              "portable export rejects changed source identity without publishing a project");
        check(sourceA.replaceWithData(originalSource.getData(), originalSource.getSize()),
              "restore portable source fixture");
        check(sourceB.replaceWithData(changedSource.getData(), changedSource.getSize()),
              "change separately imported duplicate after import");
        juce::File rejectedDuplicate;
        check(!carta::exportPortableSession(portable, 2, parent, rejectedDuplicate, error) &&
                  error.contains("changed") &&
                  !rejectedDuplicate.getChildFile("Project.carta").exists(),
              "deduplication still checks every original source");
        check(sourceB.replaceWithData(originalSource.getData(), originalSource.getSize()),
              "restore duplicate fixture");
        juce::File racedCopy;
        bool changedCopy = false;
        check(!carta::exportPortableSession(
                  portable, 2, parent, racedCopy, error,
                  [&](double progress) {
                      if (!changedCopy && progress >= 0.995 && racedCopy.isDirectory()) {
                          const auto files = racedCopy.getChildFile("Media").findChildFiles(
                              juce::File::findFiles, false);
                          if (!files.isEmpty())
                              changedCopy = files[0].replaceWithData(changedSource.getData(),
                                                                     changedSource.getSize());
                      }
                      return true;
                  }) &&
                  changedCopy && error.contains("changed before project save completed") &&
                  !racedCopy.getChildFile("Project.carta").exists(),
              "portable export detects a staged source replaced after hashing and publishes no "
              "project");
        juce::File bundle;
        check(carta::exportPortableSession(portable, 2, parent, bundle, error) &&
                  bundle != occupied &&
                  occupied.getChildFile("keep.txt").loadFileAsString() == "keep",
              "portable export creates a new folder without replacing existing files");
        const auto media =
            bundle.getChildFile("Media").findChildFiles(juce::File::findFiles, false);
        check(media.size() == 1 && bundle.getChildFile("Project.carta").existsAsFile(),
              "portable export copies identical source bytes only once");
        check(sourceA.deleteFile() && sourceB.deleteFile(),
              "remove original portable source fixtures");
        const auto moved = dir.getChildFile("moved-portable");
        check(bundle.moveFileTo(moved), "relocate portable project folder");
        carta::Session reopened;
        int mode = 1;
        check(carta::loadSession(reopened, mode, formats, moved.getChildFile("Project.carta"),
                                 error) &&
                  mode == 2 && reopened.tracks[0].clips.size() == 4 && reopened.markers.size() == 1,
              "portable project reopens after relocation and original source removal");
        check(reopened.tracks[0].clips[0].audio->file.isAChildOf(moved) &&
                  reopened.tracks[0].clips[1].audio->file.isAChildOf(moved) &&
                  reopened.tracks[0].clips[0].audio == reopened.tracks[0].clips[2].audio &&
                  reopened.tracks[0].clips[0].audio == reopened.tracks[0].clips[1].audio &&
                  reopened.tracks[0].clips[1].name == "Named clip" &&
                  carta::clipDisplayName(reopened.tracks[0].clips[0]) == "same name.wav" &&
                  carta::clipDisplayName(reopened.tracks[0].clips[3]) == "other name.wav",
              "portable reopen preserves source sharing, names and internal paths");
        juce::AudioBuffer<float> original(2, 384), restored(2, 384);
        original.clear();
        restored.clear();
        portable.render(original, 0, 384, 0, carta::sessionRate, 2);
        reopened.render(restored, 0, 384, 0, carta::sessionRate, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 384; ++i)
                check(near(original.getSample(ch, i), restored.getSample(ch, i)),
                      "portable relocation preserves rendered audio");
    }
    {
        const auto firstPath = dir.getChildFile("four-channel-a.wav"),
                   secondPath = dir.getChildFile("four-channel-b.wav");
        check(wavfile.copyFileTo(firstPath) && wavfile.copyFileTo(secondPath),
              "create byte-identical multichannel sources");
        auto first = carta::loadAudio(formats, firstPath, error),
             second = carta::loadAudio(formats, secondPath, error);
        check(first && second && first != second && first->numChannels() == 4 &&
                  first->sourceHash == second->sourceHash,
              "load separate four-channel sources with the same bytes");
        carta::Session source;
        source.tracks.push_back({"Layer 1",
                                 {{first, 0, 0, first->length()},
                                  {second, 128 / carta::sessionRate, 0, second->length()}}});
        confirmDefaultRouting(source.tracks[0]);
        source.mdDisc.medium = carta::MdMedium::md74;
        source.mdDisc.defaultCodec = carta::MdCodecMode::spMono;
        source.mdDisc.halfWidthTitle = "Mono review";
        source.mdDisc.tracks = {
            {"whole", "Whole", "", {}, false, false, false, "", {{0, 128}, {128, 256}}},
            {"repeat", "Repeat", "", {}, false, false, false, "", {{0, 128}}}};
        const auto original = dir.getChildFile("multichannel-disc.carta"),
                   moved = dir.getChildFile("multichannel-disc-moved.carta");
        check(carta::saveNativePackage(source, 1, original, error) && original.moveFileTo(moved) &&
                  firstPath.deleteFile() && secondPath.deleteFile(),
              "save and relocate a mono MdDisc package before removing both originals");
        juce::FileInputStream package(moved);
        char magic[8]{};
        check(package.read(magic, 8) == 8 && std::memcmp(magic, "CARTAP01", 8) == 0,
              "read multichannel package header");
        const int entries = package.readInt();
        int media = 0;
        for (int i = 0; i < entries; ++i) {
            const int length = package.readInt();
            const auto bytes = package.readInt64();
            check(length > 0 && length < 4096 && bytes >= 0, "read bounded package entry");
            juce::MemoryBlock name(static_cast<size_t>(length));
            check(package.read(name.getData(), length) == length, "read package entry name");
            const auto path =
                juce::String::fromUTF8(static_cast<const char*>(name.getData()), length);
            if (path.startsWith("Media/"))
                ++media;
            check(package.setPosition(package.getPosition() + 64 + bytes),
                  "skip package entry payload");
        }
        check(media == 1, "identical multichannel sources deduplicate to one stored media file");
        carta::Session restored;
        int channels = 0;
        check(carta::loadNativePackage(restored, channels, formats, moved, error) &&
                  channels == 1 && restored.tracks[0].clips.size() == 2 &&
                  restored.tracks[0].clips[0].audio == restored.tracks[0].clips[1].audio &&
                  restored.tracks[0].clips[0].audio->numChannels() == 4,
              "reopened mono package retains shared four-channel editable audio");
        check(restored.mdDisc.medium == carta::MdMedium::md74 &&
                  restored.mdDisc.defaultCodec == carta::MdCodecMode::spMono &&
                  restored.mdDisc.tracks.size() == 2 &&
                  restored.mdDisc.tracks[0].parts[0] == restored.mdDisc.tracks[1].parts[0],
              "reopened MdDisc retains medium, codec, and repeated parts");
        for (int ch = 0; ch < 4; ++ch)
            for (int i = 0; i < 128; ++i)
                check(std::equal_to<float>{}(
                          restored.tracks[0].clips[0].audio->samples.getSample(ch, i),
                          first->samples.getSample(ch, i)),
                      "all original source channels survive the package round trip");
    }
    {
        auto another = std::make_shared<carta::Audio>();
        another->sampleRate = 48000;
        another->samples.setSize(2, 100);
        carta::Track sources{"Sources",
                             {{loaded, 0, 0, loaded->length()},
                              {another, 1, 0, another->length()},
                              {loaded, 2, 0, loaded->length()}}};
        const auto entries = carta::trackSources(sources);
        check(entries.size() == 2 && entries[0].audio == loaded && entries[0].clipCount == 2 &&
                  entries[1].audio == another && entries[1].clipCount == 1,
              "source information deduplicates shared media in first-use order");
        check(carta::trackSources(carta::Track{}).empty(), "empty track has no sources");
    }
    {
        carta::Session marked;
        marked.tracks.push_back({"master", {{loaded, 0, 0, loaded->length()}}});
        confirmDefaultRouting(marked.tracks[0]);
        marked.markers = {{0, "First"},
                          {25.0 / carta::sessionRate, "Second"},
                          {50.0 / carta::sessionRate, "Third"}};
        check(carta::rippleRemoveRange(marked, {20, 40, 0, 0}, true, &error),
              "prepare ripple marker round trip");
        const auto file = dir.getChildFile("ripple-markers.carta");
        check(carta::saveSession(marked, 2, file, error), "save ripple marker project");
        carta::Session reopened;
        int channels = 1;
        check(carta::loadSession(reopened, channels, formats, file, error) && channels == 2,
              "reopen ripple marker project");
        std::vector<carta::MasterTrack> beforeTracks, afterTracks;
        check(carta::getMasterTracks(marked, beforeTracks, error) &&
                  carta::getMasterTracks(reopened, afterTracks, error) &&
                  beforeTracks.size() == 3 && afterTracks.size() == 3,
              "ripple marker boundaries remain valid after reopen");
        for (size_t i = 0; i < beforeTracks.size(); ++i)
            check(beforeTracks[i].name == afterTracks[i].name &&
                      beforeTracks[i].range.start == afterTracks[i].range.start &&
                      beforeTracks[i].range.end == afterTracks[i].range.end,
                  "ripple marker sample positions and names survive project round trip");
        juce::AudioBuffer<float> first(2, 108), second(2, 108);
        first.clear();
        second.clear();
        marked.render(first, 0, 108, 0, carta::sessionRate, 2);
        reopened.render(second, 0, 108, 0, carta::sessionRate, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 108; ++i)
                check(near(first.getSample(ch, i), second.getSample(ch, i)),
                      "ripple project reopen preserves rendered samples");
    }

    {
        constexpr int frames = 12000;
        auto stereoSource = std::make_shared<carta::Audio>();
        stereoSource->sampleRate = 48000;
        stereoSource->samples.setSize(2, 16000);
        auto surround = std::make_shared<carta::Audio>();
        surround->sampleRate = 96000;
        surround->samples.setSize(4, 32000);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 16000; ++i)
                stereoSource->samples.setSample(
                    ch, i, 0.08f * std::sin(static_cast<float>(i) * (0.031f + 0.007f * ch)));
        for (int ch = 0; ch < 4; ++ch)
            for (int i = 0; i < 32000; ++i)
                surround->samples.setSample(
                    ch, i, 0.05f * std::cos(static_cast<float>(i) * (0.025f + 0.009f * ch)));
        carta::Session mix;
        mix.tracks.push_back({"stereo",
                              {{stereoSource, 0, 0, frames / carta::sessionRate},
                               {stereoSource, 1000 / carta::sessionRate, 200 / 48000.0,
                                8000 / carta::sessionRate}}});
        mix.tracks.push_back({"four", {{surround, 0, 100 / 96000.0, frames / carta::sessionRate}}});
        mix.tracks[0].gain = 0.7f;
        mix.tracks[0].pan = -0.2f;
        mix.tracks[1].gain = 0.8f;
        mix.tracks[1].pan = 0.3f;
        mix.tracks[1].routing = {{0.7f, 0.1f}, {0.1f, 0.6f}, {0.25f, 0.25f}, {-0.1f, 0.2f}};
        check(carta::setClipEnvelope(mix.tracks[0].clips[1], 0.75f, 500 / carta::sessionRate,
                                     700 / carta::sessionRate),
              "prepare overlapping faded clip for playback export parity");
        mix.markers = {{3001 / carta::sessionRate, "two"}, {7007 / carta::sessionRate, "three"}};
        std::vector<carta::MasterTrack> tracks;
        check(carta::getMasterTracks(mix, tracks, error) && tracks.size() == 3 &&
                  tracks[2].range.end == frames,
              "construct mixed-rate marker ranges");
        {
            auto auditionSource = mix;
            auditionSource.tracks[1].solo = true;
            auto isolated = auditionSource;
            juce::int64 start = -1, end = -1;
            check(!carta::isolateClip(isolated, 0, 9, start, end) && start == -1 && end == -1 &&
                      isolated.tracks[0].clips.size() == 2,
                  "invalid isolated clip leaves snapshot and range unchanged");
            check(carta::isolateClip(isolated, 0, 1, start, end) && start == 1000 && end == 9000 &&
                      isolated.tracks[0].clips.size() == 1 && isolated.tracks[1].muted &&
                      !isolated.tracks[1].solo,
                  "clip snapshot preserves selected clip and excludes outside solo");
            for (int channels : {1, 2}) {
                juce::AudioBuffer<float> audition(channels, static_cast<int>(end - start));
                audition.clear();
                double position = start / carta::sessionRate;
                check(!carta::renderPlayback(
                          auditionSource, audition, 0, audition.getNumSamples(), position,
                          carta::sessionRate, channels,
                          {start / carta::sessionRate, end / carta::sessionRate, false, {0, 0}, 1}),
                      "isolated clip audition stops at exclusive end");
                const auto target = dir.getChildFile("isolated-" + juce::String(channels) + ".wav");
                bool clipped = false;
                check(carta::exportWavRange(isolated, channels, {start, end}, target, clipped,
                                            error, {}, {}, {carta::WavEncoding::float32, false}),
                      "export isolated clip");
                const auto loadedClip = carta::loadAudio(formats, target, error);
                check(loadedClip && loadedClip->numSamples() == end - start &&
                          loadedClip->numChannels() == channels,
                      "isolated clip export has exact output format and length");
                for (int ch = 0; ch < channels; ++ch)
                    for (int i = 0; i < loadedClip->numSamples(); ++i)
                        check(near(loadedClip->samples.getSample(ch, i), audition.getSample(ch, i)),
                              "clip export matches isolated audition including fades and routing");
            }
            auditionSource.tracks[0].muted = true;
            auto muted = auditionSource;
            check(carta::isolateClip(muted, 0, 1, start, end) &&
                      near(carta::measurePeak(muted, 2, {start, end}), 0),
                  "muted selected clip remains silent when exported");
        }
        {
            auto source = mix;
            source.tracks.push_back(
                {"unselected solo", {{stereoSource, 0, 0, frames / carta::sessionRate}}});
            source.tracks[2].solo = true;
            auto isolated = source;
            juce::int64 start = -1, end = -1;
            const std::vector<carta::ClipReference> selected{{1, 0}, {0, 1}};
            check(!carta::isolateClipGroup(isolated, {{0, 1}, {0, 1}}, start, end, error) &&
                      start == -1 && end == -1 && isolated.tracks[0].clips.size() == 2,
                  "invalid group isolation preserves session and range");
            check(carta::isolateClipGroup(isolated, selected, start, end, error) && start == 0 &&
                      end == frames && isolated.tracks[0].clips.size() == 1 &&
                      isolated.tracks[1].clips.size() == 1 && isolated.tracks[2].muted &&
                      !isolated.tracks[2].solo,
                  "group isolation retains selected clips on different tracks and excludes "
                  "unrelated solo");
            auto groupReference = source;
            groupReference.tracks[0].clips.erase(groupReference.tracks[0].clips.begin());
            groupReference.tracks[2].muted = true;
            groupReference.tracks[2].solo = false;
            carta::Session groupPreview;
            carta::PlaybackRange groupRange;
            check(carta::prepareClipGroupAudition(source, selected, false, groupPreview, groupRange,
                                                  error) &&
                      !groupRange.loop &&
                      std::abs(groupRange.end * carta::sessionRate - frames) < 1e-8,
                  "prepare group audition with the selected export span");
            auto savedPreview = groupPreview;
            auto savedRange = groupRange;
            check(!carta::prepareClipGroupAudition(source, {{0, 99}}, true, groupPreview,
                                                   groupRange, error) &&
                      groupPreview.tracks[0].clips.size() == savedPreview.tracks[0].clips.size() &&
                      std::memcmp(&groupRange.end, &savedRange.end, sizeof(double)) == 0,
                  "invalid audition preserves the previous preview and range");
            for (int channels : {1, 2}) {
                juce::AudioBuffer<float> expected(channels, frames);
                expected.clear();
                groupReference.render(expected, 0, frames, 0, carta::sessionRate, channels);
                const auto target =
                    dir.getChildFile("selected-clips-" + juce::String(channels) + ".wav");
                bool clipped = false;
                check(carta::exportWavRange(isolated, channels, {start, end}, target, clipped,
                                            error, {}, {}, {carta::WavEncoding::float32, false}),
                      "export selected clips as float WAV");
                const auto loadedGroup = carta::loadAudio(formats, target, error);
                check(loadedGroup && loadedGroup->numSamples() == frames &&
                          loadedGroup->numChannels() == channels,
                      "group export keeps the complete selected span");
                if (loadedGroup && loadedGroup->numSamples() == frames &&
                    loadedGroup->numChannels() == channels)
                    for (int ch = 0; ch < channels; ++ch)
                        for (int i = 0; i < frames; ++i)
                            check(near(loadedGroup->samples.getSample(ch, i),
                                       expected.getSample(ch, i)),
                                  "group export matches independently isolated selected tracks and "
                                  "envelopes");
                juce::AudioBuffer<float> audition(channels, frames);
                audition.clear();
                double cursor = groupRange.start;
                for (int at = 0; at < frames; at += 257)
                    carta::renderPlayback(groupPreview, audition, at, std::min(257, frames - at),
                                          cursor, carta::sessionRate, channels, groupRange);
                for (int ch = 0; ch < channels; ++ch)
                    for (int i = 0; i < frames; ++i)
                        check(near(audition.getSample(ch, i), expected.getSample(ch, i)),
                              "group audition matches group export mix across playback blocks");
            }
            auto solo = source;
            solo.tracks[0].solo = true;
            check(carta::isolateClipGroup(solo, selected, start, end, error) &&
                      solo.tracks[0].solo && !solo.tracks[1].solo && !solo.tracks[2].solo,
                  "group isolation preserves solo within the selected tracks");
            auto muted = source;
            muted.tracks[0].muted = true;
            muted.tracks[1].muted = true;
            check(carta::isolateClipGroup(muted, selected, start, end, error) &&
                      near(carta::measurePeak(muted, 2, {start, end}), 0),
                  "muted selected tracks remain silent in group export");
            auto spaced = source;
            spaced.tracks[0].clips[0].duration = 2000 / carta::sessionRate;
            spaced.tracks[0].clips[1].start = 4000 / carta::sessionRate;
            spaced.tracks[0].clips[1].duration = 1000 / carta::sessionRate;
            check(carta::isolateClipGroup(spaced, {{0, 0}, {0, 1}}, start, end, error) &&
                      start == 0 && end == 5000 &&
                      near(carta::measurePeak(spaced, 2, {2500, 3500}), 0),
                  "group export span preserves silence between selected clips on the same track");
            check(carta::prepareClipGroupAudition(spaced, {{0, 0}, {0, 1}}, true, groupPreview,
                                                  groupRange, error) &&
                      groupRange.loop,
                  "prepare looping clip group audition");
            juce::AudioBuffer<float> looped(2, 10000);
            looped.clear();
            double loopPosition = groupRange.start;
            check(carta::renderPlayback(groupPreview, looped, 0, 10000, loopPosition,
                                        carta::sessionRate, 2, groupRange),
                  "group audition loops across the selected span");
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 5000; ++i)
                    check(near(looped.getSample(ch, i), looped.getSample(ch, i + 5000)),
                          "looped group audition repeats identical audio including silence");
        }
        for (int channels : {1, 2}) {
            carta::ResamplingContext prepared;
            mix.prepareResampling(prepared, carta::sessionRate);
            juce::AudioBuffer<float> playback(channels, frames);
            playback.clear();
            double position = 0;
            for (int pos = 0; pos < frames; pos += 257) {
                const int count = std::min(257, frames - pos);
                const bool continuing = carta::renderPlayback(
                    mix, playback, pos, count, position, carta::sessionRate, channels,
                    {0, frames / carta::sessionRate, false}, &prepared);
                check(continuing == (pos + count < frames),
                      "mixed playback stops on the final callback");
            }
            juce::AudioBuffer<float> looped(channels, frames + 1);
            looped.clear();
            position = 0;
            for (int pos = 0; pos < frames; pos += 257)
                check(carta::renderPlayback(mix, looped, pos, std::min(257, frames - pos), position,
                                            carta::sessionRate, channels,
                                            {0, frames / carta::sessionRate, true}, &prepared),
                      "mixed loop remains active through callback boundaries");
            check(carta::renderPlayback(mix, looped, frames, 1, position, carta::sessionRate,
                                        channels, {0, frames / carta::sessionRate, true},
                                        &prepared),
                  "mixed loop continues after exact endpoint");
            for (int ch = 0; ch < channels; ++ch)
                check(near(looped.getSample(ch, frames), playback.getSample(ch, 0)),
                      "loop restarts on the first sample without an extra endpoint frame");
            const auto whole = dir.getChildFile("parity-" + juce::String(channels) + ".wav");
            bool clipped = false;
            check(carta::exportWavRange(mix, channels, {0, frames}, whole, clipped, error, {}, {},
                                        {carta::WavEncoding::float32, false}) &&
                      !clipped,
                  "export mixed-rate float master");
            const auto full = carta::loadAudio(formats, whole, error);
            check(full && full->numChannels() == channels && full->numSamples() == frames,
                  "read mixed-rate float master");
            float observedPeak = 0;
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < frames; ++i) {
                    const float sample = full->samples.getSample(ch, i);
                    observedPeak = std::max(observedPeak, std::abs(sample));
                    check(near(sample, playback.getSample(ch, i)),
                          "mixed-rate overlap playback matches float master");
                }
            float peak = 0;
            check(carta::scanPeak(mix, channels, {0, frames}, peak, error, {},
                                  {carta::WavEncoding::float32, false}) &&
                      near(peak, observedPeak),
                  "preflight peak matches float master");
            for (size_t n = 0; n < tracks.size(); ++n) {
                const auto path = dir.getChildFile("parity-" + juce::String(channels) + "-" +
                                                   juce::String(static_cast<int>(n)) + ".wav");
                check(carta::exportWavRange(mix, channels, tracks[n].range, path, clipped, error,
                                            {}, {}, {carta::WavEncoding::float32, false}),
                      "export mixed-rate marker track");
                const auto part = carta::loadAudio(formats, path, error);
                check(part && part->numSamples() == tracks[n].range.end - tracks[n].range.start,
                      "marker track has exact sample length");
                for (int ch = 0; ch < channels; ++ch)
                    for (int i = 0; i < part->numSamples(); ++i) {
                        const float a = full->samples.getSample(
                                        ch, static_cast<int>(tracks[n].range.start) + i),
                                    b = part->samples.getSample(ch, i);
                        check(std::memcmp(&a, &b, sizeof(float)) == 0,
                              "concatenated float marker tracks match whole master bit-for-bit");
                    }
            }
        }
        for (double deviceRate : {32000.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
            const int deviceFrames =
                static_cast<int>(std::ceil(frames * deviceRate / carta::sessionRate));
            carta::ResamplingContext prepared;
            mix.prepareResampling(prepared, deviceRate);
            juce::AudioBuffer<float> expectedDevice(2, deviceFrames), chunked(2, deviceFrames);
            expectedDevice.clear();
            chunked.clear();
            mix.render(expectedDevice, 0, deviceFrames, 0, deviceRate, 2, &prepared);
            double cursor = 0;
            for (int p = 0; p < deviceFrames; p += 127) {
                const int count = std::min(127, deviceFrames - p);
                const auto playing =
                    carta::renderPlayback(mix, chunked, p, count, cursor, deviceRate, 2,
                                          {0, frames / carta::sessionRate, false}, &prepared);
                check(playing == (p + count < deviceFrames),
                      "device-rate playback stops on the exclusive project boundary");
            }
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < deviceFrames; ++i)
                    check(near(expectedDevice.getSample(ch, i), chunked.getSample(ch, i)),
                          "mixed-rate playback is independent of device callback size");
            constexpr int loopFrames = 8192;
            for (int length : {1, 19, 157}) {
                const carta::PlaybackRange range{3001 / carta::sessionRate,
                                                 (3001 + length) / carta::sessionRate, true};
                juce::AudioBuffer<float> whole(2, loopFrames), pieces(2, loopFrames),
                    expectedLoop(2, loopFrames);
                whole.clear();
                pieces.clear();
                expectedLoop.clear();
                double a = range.start, b = range.start;
                carta::PlaybackClock wholeClock, pieceClock;
                check(carta::renderPlayback(mix, whole, 0, loopFrames, a, deviceRate, 2, range,
                                            &prepared, &wholeClock),
                      "short mixed-rate loop remains active");
                for (int p = 0; p < loopFrames; p += 113)
                    check(carta::renderPlayback(mix, pieces, p, std::min(113, loopFrames - p), b,
                                                deviceRate, 2, range, &prepared, &pieceClock),
                          "short mixed-rate loop spans callbacks");
                for (int i = 0; i < loopFrames; ++i) {
                    const auto deviceRateInteger = static_cast<juce::int64>(deviceRate);
                    const double phase = static_cast<double>((static_cast<juce::int64>(i) * 44100) %
                                                             (length * deviceRateInteger)) /
                                         static_cast<double>(deviceRateInteger);
                    mix.render(expectedLoop, i, 1, (3001 + phase) / carta::sessionRate, deviceRate,
                               2, &prepared);
                }
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < loopFrames; ++i) {
                        check(near(whole.getSample(ch, i), pieces.getSample(ch, i)),
                              "non-integral device-rate loop is independent of callback size");
                        if (!near(whole.getSample(ch, i), expectedLoop.getSample(ch, i))) {
                            std::cerr << "loop mismatch rate=" << deviceRate << " length=" << length
                                      << " frame=" << i << " channel=" << ch
                                      << " actual=" << whole.getSample(ch, i)
                                      << " expected=" << expectedLoop.getSample(ch, i) << '\n';
                            check(false, "device-rate loop matches ideal phase at every frame");
                        }
                    }
                {
                    juce::AudioBuffer<float> legacy(2, loopFrames), legacyPieces(2, loopFrames);
                    legacy.clear();
                    legacyPieces.clear();
                    double legacyPosition = range.start, piecesPosition = range.start;
                    check(carta::renderPlayback(mix, legacy, 0, loopFrames, legacyPosition,
                                                deviceRate, 2, range, &prepared),
                          "stateless short loop remains active");
                    for (int p = 0; p < loopFrames; p += 113)
                        check(carta::renderPlayback(mix, legacyPieces, p,
                                                    std::min(113, loopFrames - p), piecesPosition,
                                                    deviceRate, 2, range, &prepared),
                              "stateless loop spans callbacks");
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < loopFrames; ++i) {
                            if (!near(legacy.getSample(ch, i), expectedLoop.getSample(ch, i)) ||
                                !near(legacyPieces.getSample(ch, i),
                                      expectedLoop.getSample(ch, i))) {
                                std::cerr << "Stateless loop mismatch rate=" << deviceRate
                                          << " length=" << length << " frame=" << i
                                          << " channel=" << ch << '\n';
                                check(false, "stateless loop matches exact sample phase across "
                                             "callback sizes");
                            }
                        }
                }
            }
        }
    }
    {
        auto hi = std::make_shared<carta::Audio>();
        hi->sampleRate = 96000;
        hi->samples.setSize(2, 9600);
        for (int i = 0; i < 9600; ++i)
            for (int ch = 0; ch < 2; ++ch)
                hi->samples.setSample(ch, i,
                                      0.1f * std::sin(2.0f * juce::MathConstants<float>::pi *
                                                      (ch + 1) * 1000 * i / 96000));
        carta::Session mixSession;
        mixSession.tracks.push_back({"96 kHz", {{hi, 0, 0, 0.1}}});
        mixSession.tracks.push_back({"44.1 kHz", {{loaded, 0, 0, loaded->length()}}});
        confirmDefaultRouting(mixSession.tracks.back());
        carta::ResamplingContext context;
        mixSession.prepareResampling(context, 44100);
        juce::AudioBuffer<float> expected(2, 4410);
        expected.clear();
        mixSession.render(expected, 0, 4410, 0, 44100, 2, &context);
        const auto file = dir.getChildFile("multirate.wav");
        bool wasClipped = false;
        check(carta::exportWav(mixSession, 2, file, wasClipped, error) && !wasClipped,
              "export mixed-rate WAV");
        const auto result = carta::loadAudio(formats, file, error);
        check(result && result->samples.getNumSamples() == 4410, "mixed-rate WAV duration");
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 4410; ++i)
                check(near(expected.getSample(ch, i), result->samples.getSample(ch, i)),
                      "mixed-rate WAV matches playback rendering");
    }
    {
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = 48000;
        source->samples.setSize(1, 48000);
        for (int i = 0; i < 48000; ++i)
            source->samples.setSample(0, i, 0.6f * std::sin(static_cast<float>(i) * 0.041f));
        carta::Session slipped;
        slipped.tracks.push_back({"One", {{source, 1, 0.2, 0.3}}});
        slipped.tracks.push_back({"Two", {{source, 2, 0.25, 0.3}}});
        juce::String slipError;
        check(carta::groupClips(slipped, {{0, 0}, {1, 0}}, slipError),
              "group clips for source slip");
        auto& first = slipped.tracks[0].clips[0];
        first.fadeIn = 0.1;
        first.fadeInStart = 0.2;
        first.fadeOut = 0.1;
        first.fadeOutEnd = 0.5;
        const auto original = slipped;
        const double amount = 100.0 / carta::sessionRate;
        check(carta::slipClipGroup(slipped, {{0, 0}}, 100, slipError),
              "slip expands persistent groups");
        check(std::abs(slipped.tracks[0].clips[0].offset - (0.2 + amount)) < 1e-12 &&
                  std::abs(slipped.tracks[1].clips[0].offset - (0.25 + amount)) < 1e-12,
              "slip shifts both group sources");
        check(std::abs(slipped.tracks[0].clips[0].start - 1) < 1e-12 &&
                  std::abs(slipped.tracks[0].clips[0].duration - 0.3) < 1e-12 &&
                  std::abs(slipped.tracks[0].clips[0].fadeInStart - (0.2 + amount)) < 1e-12 &&
                  std::abs(slipped.tracks[0].clips[0].fadeOutEnd - (0.5 + amount)) < 1e-12,
              "slip preserves timeline boundaries and fade positions");
        auto slipReference = original;
        for (auto& track : slipReference.tracks) {
            auto& clip = track.clips[0];
            clip.start -= amount;
            if (clip.fadeIn > 0)
                clip.fadeInStart += amount;
            if (clip.fadeOut > 0)
                clip.fadeOutEnd += amount;
        }
        for (const double start : {1.02, 1.25, 2.02}) {
            juce::AudioBuffer<float> shiftedMix(2, 2048), referenceMix(2, 2048);
            shiftedMix.clear();
            referenceMix.clear();
            slipped.render(shiftedMix, 0, 2048, start, carta::sessionRate, 2);
            slipReference.render(referenceMix, 0, 2048, start, carta::sessionRate, 2);
            for (int ch = 0; ch < 2; ++ch)
                for (int sample = 0; sample < 2048; ++sample)
                    check(std::abs(shiftedMix.getSample(ch, sample) -
                                   referenceMix.getSample(ch, sample)) < 2e-5f,
                          "slip renders shifted mixed-rate source with the same timeline fade");
        }
        const auto slippedSnapshot = slipped;
        check(!carta::slipClipGroup(slipped, {{0, 0}}, 20000, slipError) &&
                  std::abs(slipped.tracks[0].clips[0].offset -
                           slippedSnapshot.tracks[0].clips[0].offset) < 1e-12 &&
                  std::abs(slipped.tracks[1].clips[0].offset -
                           slippedSnapshot.tracks[1].clips[0].offset) < 1e-12,
              "out of bounds slip is atomic");
        check(!carta::slipClipGroup(slipped, {{0, 0}, {0, 0}}, 1, slipError) &&
                  !carta::slipClipGroup(slipped, {{0, 0}}, 0, slipError),
              "invalid and zero slips are rejected");
        check(carta::slipClipGroup(slipped, {{1, 0}}, -100, slipError) &&
                  std::abs(slipped.tracks[0].clips[0].offset - original.tracks[0].clips[0].offset) <
                      1e-12,
              "negative slip reverses the edit");
        auto repeated = original;
        for (int i = 0; i < 1000; ++i)
            check(carta::slipClipGroup(repeated, {{0, 0}}, 1, slipError),
                  "one-sample forward slip");
        for (int i = 0; i < 1000; ++i)
            check(carta::slipClipGroup(repeated, {{0, 0}}, -1, slipError),
                  "one-sample reverse slip");
        check(std::equal_to<double>{}(repeated.tracks[0].clips[0].offset,
                                      original.tracks[0].clips[0].offset) &&
                  std::equal_to<double>{}(repeated.tracks[1].clips[0].offset,
                                          original.tracks[1].clips[0].offset),
              "repeated grid-aligned slips restore exact source offsets");
        juce::AudioBuffer<float> repeatedMix(2, 1024), originalMix(2, 1024);
        repeatedMix.clear();
        originalMix.clear();
        repeated.render(repeatedMix, 0, 1024, 1.25, carta::sessionRate, 2);
        original.render(originalMix, 0, 1024, 1.25, carta::sessionRate, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int sample = 0; sample < 1024; ++sample)
                check(std::abs(repeatedMix.getSample(ch, sample) -
                               originalMix.getSample(ch, sample)) < 1e-7f,
                      "reversed slips retain the faded audio");
    }
    {
        const double rate = carta::sessionRate;
        auto slipAudio = std::make_shared<carta::Audio>();
        slipAudio->sampleRate = rate;
        slipAudio->samples.setSize(1, 15);
        carta::Session edge;
        edge.tracks.push_back({"Boundary", {{slipAudio, 0, 0, 10 / rate}}});
        auto& clip = edge.tracks[0].clips[0];
        clip.fadeOut = 5 / rate;
        clip.fadeOutEnd = 10 / rate;
        juce::String slipError;
        check(carta::slipClipGroup(edge, {{0, 0}}, 5, slipError),
              "slip can reach exact source end despite floating point addition");
        check(!carta::slipClipGroup(edge, {{0, 0}}, 1, slipError),
              "slip rejects a real one-sample source overrun");
        check(carta::slipClipGroup(edge, {{0, 0}}, -5, slipError) &&
                  std::abs(edge.tracks[0].clips[0].offset) < 1e-18,
              "slip returns to exact source start");
        check(!carta::slipClipGroup(edge, {{0, 0}}, -1, slipError),
              "slip rejects a real one-sample source underrun");
        auto overrun = edge;
        overrun.tracks[0].clips[0].duration += 1e-5 / rate;
        check(!carta::slipClipGroup(overrun, {{0, 0}}, 5, slipError),
              "slip rejects a genuine fractional source overrun");
    }
    {
        const double rate = carta::sessionRate;
        auto slipAudio = std::make_shared<carta::Audio>();
        slipAudio->sampleRate = 48000;
        slipAudio->samples.setSize(1, 48000);
        for (int i = 0; i < 48000; ++i)
            slipAudio->samples.setSample(0, i, 0.7f * std::sin(static_cast<float>(i) * 0.043f));
        for (const double phase : {0.00000004, 0.25, 0.75}) {
            carta::Session fractional;
            fractional.tracks.push_back(
                {"Fractional slip", {{slipAudio, 0, (10000 + phase) / rate, 1000 / rate}}});
            const auto before = fractional;
            juce::String slipError;
            check(carta::slipClipGroup(fractional, {{0, 0}}, 100, slipError),
                  "slip accepts fractional source position");
            const double remainingPhase = fractional.tracks[0].clips[0].offset * rate - 10100;
            check(std::abs(remainingPhase - phase) < 1e-9,
                  "slip preserves fractional source phase instead of snapping it");
            check(carta::slipClipGroup(fractional, {{0, 0}}, -100, slipError),
                  "reverse fractional source slip");
            juce::AudioBuffer<float> actual(2, 512), expected(2, 512);
            actual.clear();
            expected.clear();
            fractional.render(actual, 0, 512, 0, rate, 2);
            before.render(expected, 0, 512, 0, rate, 2);
            for (int ch = 0; ch < 2; ++ch)
                for (int sample = 0; sample < 512; ++sample)
                    check(std::abs(actual.getSample(ch, sample) - expected.getSample(ch, sample)) <
                              1e-7f,
                          "fractional forward and reverse slip retains rendered samples");
        }
    }
    check(near(loaded->samples.getSample(3, 10), 0.4f), "preserve fourth channel");
    {
        carta::Session groups;
        for (const auto* name : {"One", "Two", "Three", "Four"})
            groups.tracks.push_back({name, {{loaded, 0, 0, loaded->length()}}});
        juce::String groupError;
        std::vector<carta::ClipReference> expanded{{3, 0}};
        check(!carta::groupClips(groups, {{0, 0}}, groupError) &&
                  groups.tracks[0].clips[0].groupId.isEmpty(),
              "single-clip group is rejected atomically");
        check(!carta::expandClipGroups(groups, {{0, 0}, {0, 0}}, expanded, groupError) &&
                  expanded.size() == 1 && expanded[0].track == 3,
              "invalid group expansion preserves output");
        check(carta::groupClips(groups, {{0, 0}, {2, 0}}, groupError),
              "create persistent group across tracks");
        const auto firstGroup = groups.tracks[0].clips[0].groupId;
        check(carta::validClipGroupId(firstGroup) && firstGroup.isNotEmpty() &&
                  groups.tracks[2].clips[0].groupId == firstGroup,
              "group members share a valid identity");
        check(carta::groupClips(groups, {{1, 0}, {3, 0}}, groupError),
              "create an independent group");
        check(carta::expandClipGroups(groups, {{2, 0}}, expanded, groupError) &&
                  expanded.size() == 2 && expanded[0].track == 0 && expanded[1].track == 2,
              "expansion returns all members in track order");
        check(carta::groupClips(groups, {{0, 0}, {1, 0}}, groupError),
              "grouping merges all members of the touched groups");
        const auto mergedId = groups.tracks[0].clips[0].groupId;
        check(mergedId != firstGroup && std::all_of(groups.tracks.begin(), groups.tracks.end(),
                                                    [&](const auto& track) {
                                                        return track.clips[0].groupId == mergedId;
                                                    }),
              "merged group uses a distinct identity for every member");
        carta::ClipSelection clipSelection;
        check(clipSelection.toggle(groups, {0, 0}, 7) &&
                  clipSelection.toggleGroup(groups, {2, 0}, 7) && clipSelection.get(7).size() == 4,
              "group-aware toggle expands a partially selected persistent group");
        check(clipSelection.toggleGroup(groups, {3, 0}, 7) && clipSelection.get(7).empty(),
              "group-aware toggle removes all members together");
        check(clipSelection.selectExpanded(groups, {{1, 0}}, 7) &&
                  clipSelection.get(7).size() == 4 &&
                  !clipSelection.toggleGroup(groups, {4, 0}, 7) && clipSelection.get(7).size() == 4,
              "invalid group selection preserves the previous clip selection");
        auto duplicated = groups;
        std::vector<carta::ClipReference> duplicateRefs;
        check(carta::duplicateClipGroup(duplicated, {{0, 0}, {2, 0}}, groupError, &duplicateRefs) &&
                  duplicateRefs.size() == 2,
              "duplicate selected grouped clips");
        const auto duplicateId =
            duplicated.tracks[0].clips[static_cast<size_t>(duplicateRefs[0].clip)].groupId;
        check(duplicateId.isNotEmpty() && duplicateId != mergedId &&
                  duplicated.tracks[2].clips[static_cast<size_t>(duplicateRefs[1].clip)].groupId ==
                      duplicateId &&
                  duplicated.tracks[0].clips[0].groupId == mergedId,
              "duplicate group gets a fresh shared identity and leaves originals unchanged");
        auto single = groups;
        int selectedClip = 0;
        check(carta::duplicateClipAfter(single, 0, selectedClip, groupError) &&
                  single.tracks[0].clips[static_cast<size_t>(selectedClip)].groupId.isEmpty(),
              "a single duplicated member is not joined to its original group");
        carta::Clipboard copied;
        check(carta::copyClipGroup(groups, {{0, 0}, {2, 0}}, copied, groupError),
              "copy members for grouped paste");
        auto pasted = groups;
        std::vector<carta::ClipReference> firstPaste, secondPaste;
        check(carta::pasteRange(pasted, copied, 0, 0, &groupError, &firstPaste) &&
                  carta::pasteRange(pasted, copied, 0, 0, &groupError, &secondPaste),
              "paste grouped clips twice");
        const auto pastedId =
            pasted.tracks[0].clips[static_cast<size_t>(firstPaste[0].clip)].groupId;
        const auto secondId =
            pasted.tracks[0].clips[static_cast<size_t>(secondPaste[0].clip)].groupId;
        check(pastedId.isNotEmpty() && pastedId != mergedId && secondId.isNotEmpty() &&
                  secondId != mergedId && secondId != pastedId &&
                  pasted.tracks[2].clips[static_cast<size_t>(firstPaste[1].clip)].groupId ==
                      pastedId &&
                  pasted.tracks[2].clips[static_cast<size_t>(secondPaste[1].clip)].groupId ==
                      secondId,
              "each paste receives a separate shared group identity");
        for (int policy = 0; policy < 3; ++policy) {
            auto target = groups;
            std::vector<carta::ClipReference> refs;
            if (policy == 1) {
                check(!carta::pasteRangeInserting(target, copied, 0, 0, false, groupError, &refs) &&
                          groupError.contains("clip group") && refs.empty(),
                      "insert paste rejects a grouped destination outside its track scope");
                target.tracks[3].clips[0].groupId.clear();
            }
            const bool ok =
                policy == 0 ? carta::pasteRangeReplacing(target, copied, 0, 0, groupError, &refs)
                            : carta::pasteRangeInserting(target, copied, 0, 0, policy == 2,
                                                         groupError, &refs);
            check(ok && refs.size() == 2,
                  "replace and both insertion policies paste grouped clips");
            const auto id = target.tracks[static_cast<size_t>(refs[0].track)]
                                .clips[static_cast<size_t>(refs[0].clip)]
                                .groupId;
            check(id.isNotEmpty() && id != mergedId &&
                      target.tracks[static_cast<size_t>(refs[1].track)]
                              .clips[static_cast<size_t>(refs[1].clip)]
                              .groupId == id,
                  "each paste policy forks a shared group identity");
        }
        auto invalidCopy = groups.tracks[0].clips[0];
        invalidCopy.groupId = "invalid";
        auto validCopy = groups.tracks[2].clips[0];
        check(!carta::forkCopiedClipGroups(groups, {&validCopy, &invalidCopy}, groupError) &&
                  validCopy.groupId == mergedId && invalidCopy.groupId == "invalid",
              "invalid copied identities are rejected atomically");
        check(carta::rangeMoveLinkedGroups(groups, {0, 1, 0, 0}, {}) == 1 &&
                  carta::rangeMoveLinkedGroups(groups, {0, 1, 0, 3}, {}) == 1,
              "partial range movement reports a group whose other audio remains");
        const auto fullEnd =
            static_cast<juce::int64>(std::ceil(loaded->length() * carta::sessionRate)) + 1;
        check(carta::rangeMoveLinkedGroups(groups, {0, fullEnd, 0, 3}, {}) == 0 &&
                  carta::rangeMoveLinkedGroups(groups, {0, fullEnd, 0, 3}, {0, 2}) == 1,
              "group impact distinguishes all members from nonadjacent partial-track scope");
        check(carta::rangeMoveLinkedGroups(groups, {fullEnd, fullEnd + 10, 0, 3}, {}) == 0,
              "group impact ignores groups outside the selected time");
        auto moved = groups;
        check(carta::moveRange(moved, {0, 1, 0, 0}, 0, 100, groupError),
              "move one sample from a grouped clip");
        check(moved.tracks[0].clips.size() == 2 && moved.tracks[0].clips[0].groupId == mergedId &&
                  moved.tracks[0].clips[1].groupId == mergedId &&
                  moved.tracks[2].clips[0].groupId == mergedId,
              "range movement preserves existing group membership");
        const auto project = dir.getChildFile("clip-groups.carta");
        check(carta::saveSession(groups, 2, project, groupError),
              "save grouped project without an explicit master end");
        juce::var data;
        check(juce::JSON::parse(project.loadFileAsString(), data).wasOk() &&
                  static_cast<int>(data["version"]) == 10,
              "grouped project uses version 10");
        carta::Session reopened;
        int channels = 0;
        check(carta::loadSession(reopened, channels, formats, project, groupError) &&
                  reopened.minimumEndSample == 0 && reopened.tracks[3].clips[0].groupId == mergedId,
              "group identities survive project reopen");
        reopened.minimumEndSample = 100;
        check(carta::saveSession(reopened, 2, project, groupError),
              "save grouped project with explicit master end");
        carta::Session withEnd;
        check(carta::loadSession(withEnd, channels, formats, project, groupError) &&
                  withEnd.minimumEndSample == 100 && withEnd.tracks[0].clips[0].groupId == mergedId,
              "grouped project preserves minimum master duration");
        check(carta::ungroupClips(withEnd, {{1, 0}}, groupError) &&
                  std::all_of(withEnd.tracks.begin(), withEnd.tracks.end(),
                              [](const auto& track) { return track.clips[0].groupId.isEmpty(); }),
              "ungrouping one member dissolves the entire group");
        carta::Session joining;
        joining.tracks.push_back({"Join", {{loaded, 0, 0, loaded->length()}}});
        check(joining.split(0, 0, loaded->length() / 2), "split fixture for group-aware join");
        joining.tracks[0].clips[0].groupId = mergedId;
        int focused = 0;
        check(!carta::joinNextClip(joining, 0, focused, groupError) &&
                  joining.tracks[0].clips.size() == 2,
              "join rejects clips with different group identities");
        data["tracks"]
            .getArray()
            ->getReference(0)["clips"]
            .getArray()
            ->getReference(0)
            .getDynamicObject()
            ->setProperty("groupId", "not-a-valid-group");
        const auto invalid = dir.getChildFile("invalid-groups.carta");
        check(invalid.replaceWithText(juce::JSON::toString(data)), "write malformed group fixture");
        check(!carta::loadSession(reopened, channels, formats, invalid, groupError) &&
                  reopened.tracks[3].clips[0].groupId == mergedId,
              "invalid group identity cannot replace the current session");
        groups.tracks[0].clips[0].groupId = "invalid";
        const auto saved = project.loadFileAsString();
        check(!carta::saveSession(groups, 2, project, groupError) &&
                  project.loadFileAsString() == saved,
              "invalid group identity cannot overwrite a project");
    }
    {
        carta::Session routingTest;
        routingTest.tracks.push_back({"Four channel", {{loaded, 0, 0, loaded->length()}}});
        juce::String routeError;
        bool wasClipped = false;
        const auto routeFile = dir.getChildFile("routing-test.wav");
        check(!carta::validateWavRange(routingTest, 2, {0, 64}, routeError) &&
                  !carta::exportWavRange(routingTest, 2, {0, 64}, routeFile, wasClipped,
                                         routeError) &&
                  !routeFile.existsAsFile(),
              "unconfirmed multichannel routing blocks export");
        routingTest.tracks[0].routing = {{1, 0}, {0, 1}, {0.5f, 0}, {0, 0.25f}};
        check(carta::exportWavRange(routingTest, 2, {0, 64}, routeFile, wasClipped, routeError),
              "export confirmed multichannel matrix");
        const auto stereoRoute = carta::loadAudio(formats, routeFile, routeError);
        check(stereoRoute && near(stereoRoute->samples.getSample(0, 20), 0.25f) &&
                  near(stereoRoute->samples.getSample(1, 20), 0.3f),
              "stereo matrix routes each source channel");
        const auto routedProject = dir.getChildFile("routing.carta");
        check(carta::saveSession(routingTest, 2, routedProject, routeError),
              "save explicit routing");
        carta::Session reopenedRoute;
        int reopenedChannels = 1;
        check(carta::loadSession(reopenedRoute, reopenedChannels, formats, routedProject,
                                 routeError) &&
                  reopenedRoute.tracks[0].routing.size() == 4 &&
                  near(reopenedRoute.tracks[0].routing[2].left, 0.5f) &&
                  near(reopenedRoute.tracks[0].routing[3].right, 0.25f),
              "explicit routing survives project round trip");
        juce::var routeData;
        check(juce::JSON::parse(routedProject.loadFileAsString(), routeData).wasOk(),
              "parse routing fixture");
        auto* routeList = routeData["tracks"].getArray()->getReference(0)["routing"].getArray();
        routeList->getReference(0).getDynamicObject()->setProperty("left", 3.0);
        const auto invalidRoute = dir.getChildFile("invalid-routing.carta");
        check(invalidRoute.replaceWithText(juce::JSON::toString(routeData)),
              "write invalid routing fixture");
        const auto oldRoute = reopenedRoute.tracks[0].routing[0].left;
        check(!carta::loadSession(reopenedRoute, reopenedChannels, formats, invalidRoute,
                                  routeError) &&
                  near(reopenedRoute.tracks[0].routing[0].left, oldRoute),
              "invalid routing cannot replace current session");
        const auto monoRouteFile = dir.getChildFile("routing-mono.wav");
        check(carta::exportWavRange(routingTest, 1, {0, 64}, monoRouteFile, wasClipped, routeError),
              "export mono matrix");
        const auto monoRoute = carta::loadAudio(formats, monoRouteFile, routeError);
        check(monoRoute && near(monoRoute->samples.getSample(0, 20), 0.275f),
              "mono master folds explicit stereo matrix");
        auto badSave = routingTest;
        badSave.tracks[0].routing[0].left = 3;
        const auto oldProjectBytes = routedProject.loadFileAsString();
        check(!carta::saveSession(badSave, 2, routedProject, routeError) &&
                  routedProject.loadFileAsString() == oldProjectBytes,
              "invalid routing cannot overwrite a valid project");
        routingTest.tracks[0].routing.resize(2);
        check(!carta::validateWavRange(routingTest, 2, {0, 64}, routeError),
              "partial routing blocks export");
        routingTest.tracks[0].muted = true;
        check(carta::validateWavRange(routingTest, 2, {0, 64}, routeError),
              "muted unconfirmed track does not block export");
        routingTest.tracks[0].muted = false;
        routingTest.tracks[0].clips[0].start = 1;
        auto monoTrack = std::make_shared<carta::Audio>();
        monoTrack->sampleRate = 44100;
        monoTrack->samples.setSize(1, 128);
        monoTrack->samples.clear();
        routingTest.tracks.push_back({"Mono", {{monoTrack, 0, 0, 128.0 / 44100}}});
        check(carta::validateWavRange(routingTest, 2, {0, 64}, routeError),
              "unconfirmed audio outside the export range does not block export");
        routingTest.tracks[0].clips[0].start = 0;
        routingTest.tracks[1].solo = true;
        check(carta::validateWavRange(routingTest, 2, {0, 64}, routeError),
              "non-solo unconfirmed track does not block export");
        routingTest.tracks[1].solo = false;
        check(!carta::validateWavRange(routingTest, 2, {0, 64}, routeError),
              "audible unconfirmed track blocks export");
    }
    {
        const float precise = 0.123456791f, tiny = 1.0e-12f;
        carta::EditedFloat field;
        check(carta::parseEditedFloat("12.3", "12.3", precise, 0, 400, 0.01, field) &&
                  !field.edited && std::memcmp(&field.value, &precise, sizeof(float)) == 0,
              "unchanged gain field retains exact stored float");
        check(carta::parseEditedFloat(" 0.000000000 ", "0.000000000", tiny, -2, 2, 1, field) &&
                  !field.edited && std::memcmp(&field.value, &tiny, sizeof(float)) == 0,
              "unchanged routing field retains a coefficient below display precision");
        check(carta::parseEditedFloat("25", "12.3", precise, 0, 400, 0.01, field) && field.edited &&
                  near(field.value, 0.25f),
              "edited gain field converts percent to linear value");
        const auto preserved = field;
        for (const auto* invalid : {"", "nan", "inf", "401", "20x", "1e999"}) {
            check(!carta::parseEditedFloat(invalid, "12.3", precise, 0, 400, 0.01, field) &&
                      field.edited == preserved.edited &&
                      std::memcmp(&field.value, &preserved.value, sizeof(float)) == 0,
                  "invalid mix field leaves result unchanged");
        }
        carta::Session mix;
        mix.tracks.push_back({"Precise", {{loaded, 0, 0, loaded->length()}}});
        auto& row = mix.tracks[0];
        row.gain = precise;
        row.pan = 0.23456789f;
        row.clips[0].gain = 0.9876543f;
        confirmDefaultRouting(row);
        row.routing[0].left = tiny;
        auto unchanged = mix;
        auto& unchangedRow = unchanged.tracks[0];
        const auto preserve = [&](float& value, double displayScale, int decimals, double low,
                                  double high, double storeScale) {
            const auto shown = juce::String(value * displayScale, decimals);
            carta::EditedFloat parsed;
            check(carta::parseEditedFloat(shown, shown, value, low, high, storeScale, parsed) &&
                      !parsed.edited,
                  "displayed mix value parses without editing");
            value = parsed.value;
        };
        preserve(unchangedRow.gain, 100, 1, 0, 400, 0.01);
        preserve(unchangedRow.pan, 100, 1, -100, 100, 0.01);
        preserve(unchangedRow.clips[0].gain, 100, 1, 0, 400, 0.01);
        preserve(unchangedRow.routing[0].left, 1, 9, -2, 2, 1);
        juce::AudioBuffer<float> expectedMix(2, 128), output(2, 128);
        expectedMix.clear();
        output.clear();
        mix.render(expectedMix, 0, 128, 0, carta::sessionRate, 2);
        unchanged.render(output, 0, 128, 0, carta::sessionRate, 2);
        check(std::abs(expectedMix.getSample(1, 0)) > 1.0e-5f,
              "mix preservation fixture contains audible signal");
        for (int channel = 0; channel < 2; ++channel)
            check(std::memcmp(expectedMix.getReadPointer(channel), output.getReadPointer(channel),
                              128 * sizeof(float)) == 0,
                  "untouched rounded fields preserve rendered audio bit-for-bit");
    }
    {
        const auto stereoPath = dir.getChildFile("split-stereo-source.wav");
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = stereoPath.createOutputStream();
        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate(carta::sessionRate)
                                 .withNumChannels(2)
                                 .withBitsPerSample(24);
        auto writer = wav.createWriterFor(stream, options);
        check(writer != nullptr, "create stereo channel split fixture");
        juce::AudioBuffer<float> fixture(2, 256);
        for (int i = 0; i < 256; ++i) {
            fixture.setSample(0, i, 0.4f * std::sin(static_cast<float>(i) * 0.1f));
            fixture.setSample(1, i, 0.3f * std::cos(static_cast<float>(i) * 0.07f));
        }
        check(writer->writeFromAudioSampleBuffer(fixture, 0, 256),
              "write stereo channel split fixture");
        writer.reset();
        auto stereoSource = carta::loadAudio(formats, stereoPath, error);
        check(stereoSource && stereoSource->numChannels() == 2,
              "load stereo channel split fixture");
        carta::EditorState original;
        original.session.tracks.push_back(
            {"Stereo", {{stereoSource, 0, 0, stereoSource->length()}}});
        auto& originalTrack = original.session.tracks[0];
        originalTrack.gain = 0.77f;
        originalTrack.pan = 0.2f;
        originalTrack.routing = {{0.7f, -0.2f}, {0.3f, 0.8f}};
        check(carta::setClipEnvelope(originalTrack.clips[0], 0.81f, 24 / carta::sessionRate,
                                     36 / carta::sessionRate),
              "prepare split channel fade");
        auto split = original;
        carta::EditHistory history;
        history.record(original, "Split stereo channels");
        check(carta::splitStereoTrackChannels(split.session, 0, error) &&
                  split.session.tracks.size() == 2,
              "split stereo into two editable tracks");
        check(split.session.tracks[0].clips[0].audio == stereoSource &&
                  split.session.tracks[1].clips[0].audio == stereoSource &&
                  carta::isolatedDisplayChannel(split.session.tracks[0]) == 0 &&
                  carta::isolatedDisplayChannel(split.session.tracks[1]) == 1,
              "split tracks share source and show the corresponding source lane");
        for (int masterChannels : {1, 2})
            for (double rate : {carta::sessionRate, 48000.0}) {
                juce::AudioBuffer<float> beforeSplit(2, 256), afterSplit(2, 256);
                beforeSplit.clear();
                afterSplit.clear();
                original.session.render(beforeSplit, 0, 256, 0, rate, masterChannels);
                split.session.render(afterSplit, 0, 256, 0, rate, masterChannels);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 256; ++i)
                        check(std::abs(beforeSplit.getSample(ch, i) - afterSplit.getSample(ch, i)) <
                                  2.0e-7f,
                              "split stereo routing reconstructs mono and stereo render within "
                              "float rounding");
            }
        auto modified = split.session;
        modified.tracks[0].routing[1].left = 0.01f;
        check(carta::isolatedDisplayChannel(modified.tracks[0]) == -1,
              "unmuting the hidden source channel restores all waveform lanes");
        modified = split.session;
        modified.tracks[0].clips.push_back({loaded, 0, 0, loaded->length()});
        check(carta::isolatedDisplayChannel(modified.tracks[0]) == -1,
              "mixed-channel track restores all source lanes");
        modified = split.session;
        const auto sourceBefore = modified.tracks[0].clips[0].audio;
        juce::String splitError;
        check(!carta::splitStereoTrackChannels(modified, 0, splitError) &&
                  modified.tracks.size() == 2 && modified.tracks[0].clips[0].audio == sourceBefore,
              "already isolated track cannot be split again");
        modified = original.session;
        modified.tracks[0].clips.push_back({loaded, 0, 0, loaded->length()});
        check(!carta::splitStereoTrackChannels(modified, 0, splitError) &&
                  modified.tracks.size() == 1 && modified.tracks[0].clips.size() == 2,
              "mixed channel sources reject split without edits");
        const auto projectPath = dir.getChildFile("split-stereo.carta");
        check(carta::saveSession(split.session, 2, projectPath, error),
              "save split channel project");
        juce::var projectJson;
        check(juce::JSON::parse(projectPath.loadFileAsString(), projectJson).wasOk() &&
                  static_cast<int>(projectJson["version"]) == 8,
              "source-channel view uses project version 8");
        carta::Session reopened;
        int reopenedChannels = 0;
        check(carta::loadSession(reopened, reopenedChannels, formats, projectPath, error) &&
                  reopened.tracks.size() == 2 &&
                  carta::isolatedDisplayChannel(reopened.tracks[0]) == 0 &&
                  carta::isolatedDisplayChannel(reopened.tracks[1]) == 1 &&
                  reopened.tracks[0].clips[0].audio == reopened.tracks[1].clips[0].audio,
              "split channels reopen with shared source and separate views");
        auto zoomed = split.session;
        zoomed.tracks[0].waveformZoom = 4;
        const auto zoomProject = dir.getChildFile("track-waveform-zoom.carta");
        check(carta::saveSession(zoomed, 2, zoomProject, error), "save per-track waveform zoom");
        juce::var zoomJson;
        check(juce::JSON::parse(zoomProject.loadFileAsString(), zoomJson).wasOk() &&
                  static_cast<int>(zoomJson["version"]) == 11,
              "track waveform override uses project version 11");
        carta::Session zoomRestored;
        int zoomChannels = 0;
        check(carta::loadSession(zoomRestored, zoomChannels, formats, zoomProject, error) &&
                  zoomRestored.tracks[0].waveformZoom == 4 &&
                  zoomRestored.tracks[1].waveformZoom == 0 &&
                  carta::trackWaveformZoom(zoomRestored.tracks[0], 2) == 4 &&
                  carta::trackWaveformZoom(zoomRestored.tracks[1], 2) == 2,
              "project restores independent and default waveform zooms");
        juce::AudioBuffer<float> zoomBefore(2, 256), zoomAfter(2, 256);
        zoomBefore.clear();
        zoomAfter.clear();
        split.session.render(zoomBefore, 0, 256, 0, carta::sessionRate, 2);
        zoomRestored.render(zoomAfter, 0, 256, 0, carta::sessionRate, 2);
        for (int ch = 0; ch < 2; ++ch)
            check(std::memcmp(zoomBefore.getReadPointer(ch), zoomAfter.getReadPointer(ch),
                              256 * sizeof(float)) == 0,
                  "waveform zoom changes no rendered audio");
        juce::String combineZoomError;
        check(!carta::combineStereoChannelTracks(zoomed, 0, combineZoomError),
              "channel-track combine rejects mismatched waveform zooms");
        auto weighted = split.session;
        weighted.tracks[0].channelHeightWeights = {3, 1};
        const auto weightProject = dir.getChildFile("channel-heights.carta");
        check(carta::saveSession(weighted, 2, weightProject, error), "save custom channel heights");
        juce::var weightJson;
        check(juce::JSON::parse(weightProject.loadFileAsString(), weightJson).wasOk() &&
                  static_cast<int>(weightJson["version"]) == 12,
              "channel heights use project version 12");
        carta::Session weightRestored;
        int weightChannels = 0;
        check(carta::loadSession(weightRestored, weightChannels, formats, weightProject, error) &&
                  weightRestored.tracks[0].channelHeightWeights == std::vector<int>({3, 1}) &&
                  weightRestored.tracks[1].channelHeightWeights.empty(),
              "project restores independent channel height settings");
        juce::AudioBuffer<float> weightedMix(2, 256);
        weightedMix.clear();
        weightRestored.render(weightedMix, 0, 256, 0, carta::sessionRate, 2);
        for (int ch = 0; ch < 2; ++ch)
            check(std::memcmp(zoomBefore.getReadPointer(ch), weightedMix.getReadPointer(ch),
                              256 * sizeof(float)) == 0,
                  "channel heights do not change audio");
        check(!carta::combineStereoChannelTracks(weighted, 0, combineZoomError),
              "channel-track combine rejects mismatched channel heights");
        weightJson["tracks"].getArray()->getReference(0).getDynamicObject()->setProperty(
            "channelHeightWeights", juce::var(juce::Array<juce::var>{0, 1}));
        check(
            weightProject.replaceWithText(juce::JSON::toString(weightJson)) &&
                !carta::loadSession(weightRestored, weightChannels, formats, weightProject, error),
            "invalid channel weights are rejected on load");
        weighted.tracks[0].channelHeightWeights = {1001, 1};
        check(!carta::saveSession(weighted, 2, weightProject, error),
              "invalid channel weights are rejected on save");
        zoomJson["tracks"].getArray()->getReference(0).getDynamicObject()->setProperty(
            "waveformZoom", 3);
        check(zoomProject.replaceWithText(juce::JSON::toString(zoomJson)) &&
                  !carta::loadSession(zoomRestored, zoomChannels, formats, zoomProject, error),
              "invalid waveform zoom is rejected on project load");
        zoomed.tracks[0].waveformZoom = 3;
        check(!carta::saveSession(zoomed, 2, zoomProject, error),
              "invalid waveform zoom is rejected on project save");
        juce::AudioBuffer<float> originalSplit(2, 256), reopenedSplit(2, 256);
        originalSplit.clear();
        reopenedSplit.clear();
        split.session.render(originalSplit, 0, 256, 0, carta::sessionRate, 2);
        reopened.render(reopenedSplit, 0, 256, 0, carta::sessionRate, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 256; ++i)
                check(std::abs(originalSplit.getSample(ch, i) - reopenedSplit.getSample(ch, i)) <
                          1.0e-7f,
                      "split channel mix survives reopening");
        check(carta::describeTimeline(split.session, {}, 0, 0, -1, 0, false)
                      .contains("Showing source channel 1") &&
                  carta::describeNavigationTarget(split.session, 1, 0)
                      .contains("Showing source channel 2"),
              "source channel view is accessible in timeline and keyboard navigation");
        const auto invalidPath = dir.getChildFile("invalid-split.carta");
        projectJson["tracks"].getArray()->getReference(0).getDynamicObject()->setProperty(
            "displaySourceChannel", 2);
        check(invalidPath.replaceWithText(juce::JSON::toString(projectJson)),
              "write invalid channel display fixture");
        auto untouched = original.session;
        const auto originalAudio = untouched.tracks[0].clips[0].audio;
        int unchangedChannels = 2;
        check(!carta::loadSession(untouched, unchangedChannels, formats, invalidPath, error) &&
                  untouched.tracks.size() == 1 &&
                  untouched.tracks[0].clips[0].audio == originalAudio && unchangedChannels == 2,
              "invalid channel display cannot replace the current session");
        {
            auto combined = reopened;
            juce::String combineError;
            const bool combinedOK = carta::combineStereoChannelTracks(combined, 0, combineError);
            if (!combinedOK || combined.tracks.size() != 1 || combined.tracks[0].name != "Stereo")
                std::cerr << "Combine diagnostic: " << combineError << "; name "
                          << (combined.tracks.empty() ? juce::String("none")
                                                      : combined.tracks[0].name)
                          << '\n';
            check(combinedOK && combined.tracks.size() == 1 &&
                      combined.tracks[0].name == "Stereo" &&
                      combined.tracks[0].displaySourceChannel == -1,
                  "combine matching source-channel tracks into their original stereo arrangement");
            juce::AudioBuffer<float> beforeCombine(2, 256), afterCombine(2, 256);
            beforeCombine.clear();
            afterCombine.clear();
            original.session.render(beforeCombine, 0, 256, 0, carta::sessionRate, 2);
            combined.render(afterCombine, 0, 256, 0, carta::sessionRate, 2);
            for (int ch = 0; ch < 2; ++ch)
                check(std::memcmp(beforeCombine.getReadPointer(ch), afterCombine.getReadPointer(ch),
                                  256 * sizeof(float)) == 0,
                      "recombined routing reconstructs original render bit-for-bit");
            auto reversed = reopened;
            std::swap(reversed.tracks[0], reversed.tracks[1]);
            check(carta::combineStereoChannelTracks(reversed, 0, combineError) &&
                      reversed.tracks[0].name == "Stereo" &&
                      carta::isolatedDisplayChannel(reversed.tracks[0]) == -1,
                  "combine works with reversed channel-track order");
            carta::EditorState beforeCombineState;
            beforeCombineState.session = reopened;
            carta::EditorState afterCombineState = beforeCombineState;
            carta::EditHistory combineHistory;
            combineHistory.record(beforeCombineState, "Combine stereo channels");
            check(carta::combineStereoChannelTracks(afterCombineState.session, 0, combineError) &&
                      combineHistory.undo(afterCombineState) &&
                      afterCombineState.session.tracks.size() == 2 &&
                      combineHistory.redo(afterCombineState) &&
                      afterCombineState.session.tracks.size() == 1,
                  "combining supports undo and redo");
            for (int variant = 0; variant < 14; ++variant) {
                auto different = reopened;
                auto& second = different.tracks[1];
                switch (variant) {
                case 0:
                    second.muted = !second.muted;
                    break;
                case 1:
                    second.solo = !second.solo;
                    break;
                case 2:
                    second.gain = 0.5f;
                    break;
                case 3:
                    second.height += 1;
                    break;
                case 4:
                    second.clips[0].start = 1 / carta::sessionRate;
                    break;
                case 5:
                    second.clips[0].gain = 0.5f;
                    break;
                case 6:
                    second.clips[0].name = "Changed";
                    break;
                case 7:
                    second.routing[0].left = 0.5f;
                    break;
                case 8:
                    second.pan = -0.4f;
                    break;
                case 9:
                    second.clips[0].fadeInStart += 1 / carta::sessionRate;
                    break;
                case 10:
                    second.clips.push_back(second.clips[0]);
                    break;
                case 11:
                    second.clips[0].audio = loaded;
                    break;
                case 12:
                    second.view = carta::TrackView::spectrogram;
                    break;
                case 13:
                    second.routing.push_back({0.4f, 0.2f});
                    break;
                }
                const auto leftSource = different.tracks[0].clips[0].audio,
                           rightSource = different.tracks[1].clips[0].audio;
                check(!carta::combineStereoChannelTracks(different, 0, combineError) &&
                          combineError.isNotEmpty() && different.tracks.size() == 2 &&
                          different.tracks[0].clips[0].audio == leftSource &&
                          different.tracks[1].clips[0].audio == rightSource,
                      "incompatible split tracks reject combining without losing either track");
            }
            auto unicodePair = original.session;
            unicodePair.tracks[0].name = juce::String::fromUTF8(u8"音楽");
            check(carta::splitStereoTrackChannels(unicodePair, 0, combineError) &&
                      carta::combineStereoChannelTracks(unicodePair, 0, combineError) &&
                      unicodePair.tracks[0].name == juce::String::fromUTF8(u8"音楽"),
                  "split and combine preserve Unicode track names");
            auto differentlyNamed = reopened;
            differentlyNamed.tracks[0].name = "Voice";
            differentlyNamed.tracks[1].name = "Ambience";
            check(carta::combineStereoChannelTracks(differentlyNamed, 0, combineError) &&
                      differentlyNamed.tracks[0].name == "Voice / Ambience",
                  "combining retains distinct custom track names");
        }
        {
            const auto replacementPath = dir.getChildFile("relocated stereo.wav");
            check(stereoPath.copyFileTo(replacementPath),
                  "copy matching source for live relink fixture");
            const auto oldSource = reopened.tracks[0].clips[0].audio;
            juce::String relinkError;
            double lastProgress = -1;
            bool monotonic = true;
            const auto replacement = carta::loadVerifiedReplacement(
                formats, oldSource, replacementPath, relinkError, [&](double p) {
                    if (p < lastProgress)
                        monotonic = false;
                    lastProgress = p;
                    return true;
                });
            if (!replacement || !monotonic || std::abs(lastProgress - 1) >= 1.0e-12)
                std::cerr << "Relink diagnostic: " << relinkError << "; monotonic " << monotonic
                          << "; progress " << lastProgress << '\n';
            check(replacement && replacement->file == replacementPath &&
                      replacement->sourceHash == oldSource->sourceHash && monotonic &&
                      std::abs(lastProgress - 1) < 1.0e-12,
                  "live relink verifies identical source bytes and decoded samples");
            check(!carta::loadVerifiedReplacement(formats, oldSource, loaded->file, relinkError) &&
                      relinkError.contains("checksum"),
                  "live relink rejects a different file before replacing anything");
            const auto mappedOriginal = carta::loadAudio(formats, stereoPath, relinkError, {}, 1);
            check(mappedOriginal && mappedOriginal->mapped &&
                      carta::loadVerifiedReplacement(formats, mappedOriginal, replacementPath,
                                                     relinkError),
                  "live relink compares disk-backed source samples against the replacement");
            for (double stopAt : {0.0, 0.5, 0.9})
                check(!carta::loadVerifiedReplacement(formats, oldSource, replacementPath,
                                                      relinkError,
                                                      [stopAt](double p) { return p < stopAt; }) &&
                          relinkError.contains("cancelled"),
                      "live relink can be cancelled without replacing the source");
            auto changedPcm = std::make_shared<carta::Audio>();
            changedPcm->file = oldSource->file;
            changedPcm->sourceHash = oldSource->sourceHash;
            changedPcm->sampleRate = oldSource->sampleRate;
            changedPcm->samples.setSize(oldSource->numChannels(), oldSource->numSamples());
            for (int ch = 0; ch < oldSource->numChannels(); ++ch)
                changedPcm->samples.copyFrom(ch, 0, oldSource->channelData(ch),
                                             oldSource->numSamples());
            changedPcm->samples.setSample(0, 10, 0.99f);
            check(!carta::loadVerifiedReplacement(formats, changedPcm, replacementPath,
                                                  relinkError) &&
                      relinkError.contains("different audio samples"),
                  "matching encoded bytes cannot silently replace different decoded PCM");
            carta::EditorState beforeRelink;
            beforeRelink.session = reopened;
            beforeRelink.session.tracks.push_back(
                {"Unrelated", {{loaded, 0, 0, loaded->length()}}});
            beforeRelink.session.markers = {{0.001, "Keep"}};
            auto afterRelink = beforeRelink;
            carta::EditHistory relinkHistory;
            relinkHistory.record(beforeRelink, "Relink source");
            check(carta::replaceVerifiedSource(afterRelink.session, oldSource, replacement,
                                               relinkError) &&
                      afterRelink.session.tracks[0].clips[0].audio == replacement &&
                      afterRelink.session.tracks[1].clips[0].audio == replacement &&
                      afterRelink.session.tracks[2].clips[0].audio == loaded &&
                      afterRelink.session.markers[0].name == "Keep",
                  "live relink replaces every shared clip source and preserves unrelated audio and "
                  "markers");
            check(relinkHistory.undo(afterRelink) &&
                      afterRelink.session.tracks[0].clips[0].audio == oldSource &&
                      relinkHistory.redo(afterRelink) &&
                      afterRelink.session.tracks[1].clips[0].audio == replacement,
                  "live source relink supports undo and redo");
            const auto movedProject = dir.getChildFile("live-relinked.carta");
            check(carta::saveSession(afterRelink.session, 2, movedProject, relinkError),
                  "save project after live relink");
            const auto backup = dir.getChildFile("temporary-original-backup.wav");
            check(stereoPath.moveFileTo(backup), "move old source out of its original location");
            carta::Session reopenedMoved;
            int movedChannels = 0;
            check(carta::loadSession(reopenedMoved, movedChannels, formats, movedProject,
                                     relinkError) &&
                      reopenedMoved.tracks[0].clips[0].audio->file == replacementPath &&
                      reopenedMoved.tracks[1].clips[0].audio ==
                          reopenedMoved.tracks[0].clips[0].audio,
                  "saved relinked project reopens without the old source path");
            check(backup.moveFileTo(stereoPath), "restore original source fixture");
            carta::Session preserved = beforeRelink.session;
            check(!carta::replaceVerifiedSource(preserved, oldSource, loaded, relinkError) &&
                      preserved.tracks[0].clips[0].audio == oldSource,
                  "invalid source replacement leaves project unchanged");
        }
        check(history.undo(split) && split.session.tracks.size() == 1 && history.redo(split) &&
                  split.session.tracks.size() == 2 &&
                  carta::isolatedDisplayChannel(split.session.tracks[1]) == 1,
              "split channel tracks support undo and redo");
        split.session.tracks[0].clips[0].start = 8 / carta::sessionRate;
        check(std::abs(split.session.tracks[1].clips[0].start) < 1.0e-12,
              "split channel clips can move independently");
    }
    {
        auto signal = std::make_shared<carta::Audio>();
        signal->sampleRate = carta::sessionRate;
        signal->samples.setSize(2, 100);
        for (int i = 0; i < 100; ++i) {
            signal->samples.setSample(0, i, i < 18 || i >= 70 ? 0.5f : -0.5f);
            signal->samples.setSample(1, i, 0.3f);
        }
        signal->samples.setSample(0, 17, 0.01f);
        signal->samples.setSample(0, 18, -0.01f);
        signal->samples.setSample(0, 69, -0.02f);
        signal->samples.setSample(0, 70, 0.02f);
        carta::Session crossing;
        crossing.tracks.push_back({"Crossing", {{signal, 0, 0, signal->length()}}});
        carta::TimeSelection input{20, 72, 0, 0}, output{2, 4, 0, 0};
        juce::String crossingError;
        double lastProgress = -1;
        bool monotonic = true;
        check(carta::snapSelectionToZeroCrossings(crossing, 2, input, 5, output, crossingError,
                                                  [&](double p) {
                                                      if (p < lastProgress)
                                                          monotonic = false;
                                                      lastProgress = p;
                                                      return true;
                                                  }) &&
                  output.start == 18 && output.end == 70 && monotonic &&
                  std::abs(lastProgress - 1) < 1.0e-12,
              "search snaps both endpoints to quiet crossings in selected stereo mix");
        check(carta::snapSelectionToZeroCrossings(crossing, 1, input, 5, output, crossingError) &&
                  output.start == 19 && output.end == 69,
              "zero-crossing search respects mono fold-down");
        output = {2, 4, 0, 0};
        check(!carta::snapSelectionToZeroCrossings(crossing, 2, input, 5, output, crossingError, {},
                                                   true) &&
                  output.start == 2 && output.end == 4 && crossingError.contains("common"),
              "common-channel search rejects a crossing in only one stereo channel atomically");
        for (int i = 0; i < 100; ++i)
            signal->samples.setSample(1, i, signal->samples.getSample(0, i) * 0.5f);
        check(carta::snapSelectionToZeroCrossings(crossing, 2, input, 5, output, crossingError, {},
                                                  true) &&
                  output.start == 18 && output.end == 70,
              "common-channel search finds simultaneous stereo crossings");
        for (int i = 0; i < 100; ++i)
            signal->samples.setSample(1, i, 0.0f);
        check(carta::snapSelectionToZeroCrossings(crossing, 2, input, 5, output, crossingError, {},
                                                  true) &&
                  output.start == 18 && output.end == 70,
              "a silent output channel permits a crossing in the other channel");
        for (int i = 0; i < 100; ++i)
            signal->samples.setSample(1, i, 0.3f);
        auto unrouted = crossing;
        unrouted.tracks[0].clips[0].audio = loaded;
        output = {2, 4, 0, 0};
        check(!carta::snapSelectionToZeroCrossings(unrouted, 2, input, 5, output, crossingError) &&
                  crossingError.contains("routing") && output.start == 2 && output.end == 4,
              "unconfirmed multichannel routing rejects analysis atomically");
        auto mixed = crossing;
        auto silent = std::make_shared<carta::Audio>();
        silent->sampleRate = carta::sessionRate;
        silent->samples.setSize(1, 100);
        silent->samples.clear();
        mixed.tracks.push_back({"Silent", {{silent, 0, 0, silent->length()}}});
        mixed.tracks[1].solo = true;
        auto scoped = mixed;
        check(carta::limitToTracks(scoped, 0, 0), "scope crossing fixture");
        check(carta::snapSelectionToZeroCrossings(scoped, 2, input, 5, output, crossingError) &&
                  output.start == 18 && output.end == 70,
              "selected-track search ignores solo outside its scope");
        auto silenceSession = crossing;
        silenceSession.tracks[0].clips[0].audio = silent;
        check(carta::snapSelectionToZeroCrossings(silenceSession, 2, input, 5, output,
                                                  crossingError) &&
                  output.start == 20 && output.end == 72,
              "silence retains the closest boundaries");
        check(carta::snapSelectionToZeroCrossings(crossing, 2, {0, 100, 0, 0}, 5, output,
                                                  crossingError) &&
                  output.start == 0 && output.end == 100,
              "master edges stay fixed");
        auto dc = std::make_shared<carta::Audio>();
        dc->sampleRate = carta::sessionRate;
        dc->samples.setSize(2, 100);
        for (int i = 0; i < 100; ++i) {
            dc->samples.setSample(0, i, 0.2f);
            dc->samples.setSample(1, i, 0.0f);
        }
        auto dcSession = crossing;
        dcSession.tracks[0].clips[0].audio = dc;
        output = {2, 4, 0, 0};
        check(!carta::snapSelectionToZeroCrossings(dcSession, 2, input, 5, output, crossingError) &&
                  output.start == 2 && output.end == 4 && crossingError.contains("start"),
              "silent channel alone cannot create a false crossing and failure preserves output");
        check(!carta::snapSelectionToZeroCrossings(crossing, 2, {17, 19, 0, 0}, 3, output,
                                                   crossingError) &&
                  crossingError.contains("collapse") && output.start == 2 && output.end == 4,
              "crossings that collapse a selection are rejected atomically");
        check(!carta::snapSelectionToZeroCrossings(crossing, 2, input, 5, output, crossingError,
                                                   [](double p) { return p < 0.5; }) &&
                  crossingError.contains("cancelled") && output.start == 2 && output.end == 4,
              "cancellation preserves prior selection result");
        signal->samples.setSample(0, 17, std::numeric_limits<float>::quiet_NaN());
        check(!carta::snapSelectionToZeroCrossings(crossing, 2, input, 5, output, crossingError) &&
                  crossingError.contains("non-finite") && output.start == 2 && output.end == 4,
              "non-finite audio is rejected without changing selection result");
    }
    {
        const double rate = carta::sessionRate;
        auto monoAudio = std::make_shared<carta::Audio>();
        monoAudio->sampleRate = rate;
        monoAudio->samples.setSize(1, 100);
        monoAudio->samples.clear();
        auto stereoAudio = std::make_shared<carta::Audio>();
        stereoAudio->sampleRate = rate;
        stereoAudio->samples.setSize(4, 100);
        stereoAudio->samples.clear();
        carta::Session consolidation;
        consolidation.tracks.push_back({"A",
                                        {{monoAudio, 0, 0, 50 / rate},
                                         {monoAudio, 50 / rate, 0, 50 / rate},
                                         {stereoAudio, 0, 0, 50 / rate}}});
        consolidation.tracks.push_back({"B", {{monoAudio, 0, 0, 50 / rate}}});
        carta::Session render;
        render.minimumEndSample = 77;
        int track = 17, channels = 19, newClip = 23;
        juce::int64 start = 29, end = 31;
        juce::String consolidationError;
        check(!carta::prepareClipConsolidation(consolidation, {{0, 0}, {0, 2}}, render, track,
                                               channels, start, end, consolidationError) &&
                  consolidationError.contains("matching source channel counts") &&
                  render.minimumEndSample == 77 && track == 17,
              "mixed multichannel counts reject consolidation without changing outputs");
        check(!carta::prepareClipConsolidation(consolidation, {{0, 0}, {1, 0}}, render, track,
                                               channels, start, end, consolidationError) &&
                  consolidationError.contains("one track"),
              "cross-track consolidation rejects without changing the arrangement");
        check(carta::groupClips(consolidation, {{0, 0}, {1, 0}}, consolidationError) &&
                  !carta::prepareClipConsolidation(consolidation, {{0, 0}, {0, 1}}, render, track,
                                                   channels, start, end, consolidationError) &&
                  consolidationError.contains("every member"),
              "partial persistent groups cannot be consolidated");
        check(carta::ungroupClips(consolidation, {{0, 0}}, consolidationError) &&
                  carta::prepareClipConsolidation(consolidation, {{0, 0}, {0, 1}}, render, track,
                                                  channels, start, end, consolidationError) &&
                  track == 0 && channels == 1 && start == 0 && end == 100,
              "same-track mono clips prepare a pre-mix render plan");
        check(!carta::replaceClipsWithConsolidation(consolidation, {{0, 0}, {0, 1}}, monoAudio, 0,
                                                    99, newClip, consolidationError) &&
                  consolidation.tracks[0].clips.size() == 3 && newClip == 23,
              "mismatched rendered geometry rejects consolidation atomically");
        carta::Session verification;
        verification.tracks.push_back(consolidation.tracks[0]);
        auto baked = std::make_shared<carta::Audio>();
        baked->sampleRate = rate;
        baked->samples.setSize(1, 100);
        baked->samples.clear();
        check(carta::verifyClipConsolidation(verification, {{0, 0}, {0, 1}}, baked, 0, 100,
                                             consolidationError),
              "matching consolidation passes project-rate verification");
        check(!carta::verifyClipConsolidation(verification, {{0, 0}, {0, 1}}, baked, 0, 100,
                                              consolidationError, [](double) { return false; }) &&
                  consolidationError.contains("cancelled") &&
                  verification.tracks[0].clips.size() == 3,
              "cancelled consolidation verification leaves the source arrangement untouched");
        baked->samples.setSample(0, 10, 0.01f);
        check(!carta::verifyClipConsolidation(verification, {{0, 0}, {0, 1}}, baked, 0, 100,
                                              consolidationError) &&
                  consolidationError.contains("sample 10") &&
                  verification.tracks[0].clips.size() == 3,
              "consolidation verification rejects changed samples without mutating the snapshot");
        auto wide = std::make_shared<carta::Audio>(), quiet = std::make_shared<carta::Audio>(),
             merged = std::make_shared<carta::Audio>();
        for (const auto& item : {wide, quiet, merged}) {
            item->sampleRate = rate;
            item->samples.setSize(2, 1);
        }
        wide->samples.setSample(0, 0, 1e20f);
        wide->samples.setSample(1, 0, -1e20f);
        quiet->samples.setSample(0, 0, 1);
        quiet->samples.setSample(1, 0, 1);
        merged->samples.setSample(0, 0, 1e20f);
        merged->samples.setSample(1, 0, -1e20f);
        carta::Session monoFold;
        monoFold.tracks.push_back({"Mono fold", {{wide, 0, 0, 1 / rate}, {quiet, 0, 0, 1 / rate}}});
        check(!carta::verifyClipConsolidation(monoFold, {{0, 0}, {0, 1}}, merged, 0, 1,
                                              consolidationError) &&
                  consolidationError.contains("verification tolerance"),
              "consolidation verification rejects a mono-fold change hidden by matching stereo and "
              "source lanes");
        check(!carta::verifyClipConsolidation(monoFold, {{0, 0}, {0, 1}}, merged, 0, 1,
                                              consolidationError, {}, true),
              "routing-change opt-in does not bypass current-mix verification");
    }
    {
        const double rate = carta::sessionRate;
        auto highRate = std::make_shared<carta::Audio>();
        highRate->sampleRate = 48000;
        highRate->samples.setSize(2, 1200);
        auto nativeRate = std::make_shared<carta::Audio>();
        nativeRate->sampleRate = rate;
        nativeRate->samples.setSize(2, 1000);
        for (int channel = 0; channel < 2; ++channel) {
            for (int i = 0; i < 1200; ++i)
                highRate->samples.setSample(
                    channel, i, static_cast<float>(0.12 * std::sin((i + channel * 7) * 0.07)));
            for (int i = 0; i < 1000; ++i)
                nativeRate->samples.setSample(
                    channel, i, static_cast<float>(0.11 * std::cos((i + channel * 11) * 0.05)));
        }
        carta::Session before;
        before.tracks.push_back({"Stereo",
                                 {{highRate, 100 / rate, 0, 441 / rate},
                                  {nativeRate, 200 / rate, 0, 100 / rate},
                                  {nativeRate, 400 / rate, 0, 500 / rate}}});
        before.tracks[0].gain = 0.7f;
        before.tracks[0].pan = -0.3f;
        before.tracks[0].routing = {{0.8f, 0.1f}, {0.2f, 0.9f}};
        check(carta::setClipEnvelope(before.tracks[0].clips[0], 0.8f, 40 / rate, 20 / rate) &&
                  carta::setClipEnvelope(before.tracks[0].clips[2], 0.9f, 30 / rate, 50 / rate),
              "prepare mixed-rate stereo consolidation envelopes");
        carta::Session plan;
        int row = -1, channels = 0, index = -1;
        juce::int64 first = 0, last = 0;
        juce::String consolidationError;
        check(carta::prepareClipConsolidation(before, {{0, 0}, {0, 2}}, plan, row, channels, first,
                                              last, consolidationError) &&
                  row == 0 && channels == 2 && first == 100 && last == 900,
              "prepare stereo consolidation across source rates and overlaps");
        auto baked = std::make_shared<carta::Audio>();
        baked->sampleRate = rate;
        baked->samples.setSize(2, static_cast<int>(last - first));
        baked->samples.clear();
        plan.render(baked->samples, 0, baked->numSamples(), static_cast<double>(first) / rate, rate,
                    2);
        auto consolidated = before;
        check(carta::replaceClipsWithConsolidation(consolidated, {{0, 0}, {0, 2}}, baked, first,
                                                   last, index, consolidationError) &&
                  index == 0 && consolidated.tracks[0].clips.size() == 2,
              "replace stereo clips with rendered media while preserving the unrelated overlap");
        juce::AudioBuffer<float> original(2, 1000), result(2, 1000);
        original.clear();
        result.clear();
        before.render(original, 0, 1000, 0, rate, 2);
        consolidated.render(result, 0, 1000, 0, rate, 2);
        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < 1000; ++i)
                check(std::abs(original.getSample(channel, i) - result.getSample(channel, i)) <
                          2e-7f,
                      "stereo mixed-rate consolidation preserves the project-rate routed mix "
                      "within floating-point rounding");
    }
    {
        const double rate = carta::sessionRate;
        auto monoSource = std::make_shared<carta::Audio>();
        monoSource->sampleRate = 48000;
        monoSource->samples.setSize(1, 1200);
        auto stereoSource = std::make_shared<carta::Audio>();
        stereoSource->sampleRate = rate;
        stereoSource->samples.setSize(2, 1000);
        for (int i = 0; i < 1200; ++i)
            monoSource->samples.setSample(0, i, static_cast<float>(0.12 * std::sin(i * 0.07)));
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 1000; ++i)
                stereoSource->samples.setSample(
                    ch, i, static_cast<float>(0.11 * std::cos((i + ch * 11) * 0.05)));
        for (int routingMode = 0; routingMode < 3; ++routingMode) {
            carta::Session beforeMixed;
            beforeMixed.tracks.push_back({"Mixed",
                                          {{monoSource, 100 / rate, 0, 441 / rate},
                                           {stereoSource, 200 / rate, 0, 100 / rate},
                                           {stereoSource, 400 / rate, 0, 500 / rate}}});
            auto& mixedTrack = beforeMixed.tracks[0];
            mixedTrack.gain = 0.7f;
            mixedTrack.pan = -0.3f;
            if (routingMode == 1)
                mixedTrack.routing = {{0.8f, 0.1f}};
            if (routingMode == 2)
                mixedTrack.routing = {{0.8f, 0.1f}, {0.2f, 0.9f}};
            check(carta::setClipEnvelope(mixedTrack.clips[0], 0.8f, 40 / rate, 20 / rate) &&
                      carta::setClipEnvelope(mixedTrack.clips[2], 0.9f, 30 / rate, 50 / rate),
                  "prepare mixed-channel envelopes");
            carta::Session mixedPlan;
            int row = -1, channels = 0, index = -1;
            juce::int64 first = 0, last = 0;
            juce::String mixedError;
            check(carta::prepareClipConsolidation(beforeMixed, {{0, 0}, {0, 2}}, mixedPlan, row,
                                                  channels, first, last, mixedError) &&
                      channels == 2 && first == 100 && last == 900,
                  "prepare mixed mono and stereo clips as stereo");
            auto bakedMixed = std::make_shared<carta::Audio>();
            bakedMixed->sampleRate = rate;
            bakedMixed->samples.setSize(2, static_cast<int>(last - first));
            bakedMixed->samples.clear();
            mixedPlan.render(bakedMixed->samples, 0, bakedMixed->numSamples(),
                             static_cast<double>(first) / rate, rate, 2);
            auto afterMixed = beforeMixed;
            check(carta::replaceClipsWithConsolidation(afterMixed, {{0, 0}, {0, 2}}, bakedMixed,
                                                       first, last, index, mixedError) &&
                      index == 0 && afterMixed.tracks[0].clips.size() == 2,
                  "replace mixed-channel clips while retaining an unrelated overlap");
            const bool verified = carta::verifyClipConsolidation(
                beforeMixed, {{0, 0}, {0, 2}}, bakedMixed, first, last, mixedError);
            check(!verified && mixedError.contains(routingMode == 0 ? "source-channel"
                                                                    : "automatic routing"),
                  "mixed-channel verification detects changed source lanes or automatic routing");
            check(carta::verifyClipConsolidation(beforeMixed, {{0, 0}, {0, 2}}, bakedMixed, first,
                                                 last, mixedError, {}, true),
                  "explicit routing-change opt-in permits a matching current mixed-channel mix");
            if (routingMode != 0) {
                auto reroutedBefore = beforeMixed, reroutedAfter = afterMixed;
                reroutedBefore.tracks[0].routing = {{0.1f, 0.8f}, {0.9f, 0.2f}};
                reroutedAfter.tracks[0].routing = reroutedBefore.tracks[0].routing;
                juce::AudioBuffer<float> beforeReroute(2, 1000), afterReroute(2, 1000);
                beforeReroute.clear();
                afterReroute.clear();
                reroutedBefore.render(beforeReroute, 0, 1000, 0, rate, 2);
                reroutedAfter.render(afterReroute, 0, 1000, 0, rate, 2);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 1000; ++i)
                        check(std::abs(beforeReroute.getSample(ch, i) -
                                       afterReroute.getSample(ch, i)) < 2e-7f,
                              "explicitly routed mixed consolidation retains the mix after a "
                              "routing change");
            }
            for (int outputChannels = 1; outputChannels <= 2; ++outputChannels) {
                juce::AudioBuffer<float> originalMix(outputChannels, 1000),
                    resultMix(outputChannels, 1000);
                originalMix.clear();
                resultMix.clear();
                beforeMixed.render(originalMix, 0, 1000, 0, rate, outputChannels);
                afterMixed.render(resultMix, 0, 1000, 0, rate, outputChannels);
                for (int ch = 0; ch < outputChannels; ++ch)
                    for (int i = 0; i < 1000; ++i)
                        check(std::abs(originalMix.getSample(ch, i) - resultMix.getSample(ch, i)) <
                                  2e-7f,
                              "mixed-channel consolidation preserves automatic and custom routed "
                              "mixes");
            }
        }
    }
    {
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = carta::sessionRate;
        source->samples.setSize(2, 3000);
        source->samples.clear();
        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < 3000; ++i)
                if (i < 700 || i >= 2200)
                    source->samples.setSample(channel, i, 0.5f);
        source->samples.setSample(1, 1500, 0.25f);
        carta::Clip clip{source, 100 / carta::sessionRate, 0, source->length()};
        std::vector<carta::QuietRegion> silentRegions;
        juce::String silenceError;
        check(carta::findClipSourceSilence(clip, 100, silentRegions, silenceError) &&
                  silentRegions.size() == 2 && silentRegions[0].start == 800 &&
                  silentRegions[0].end == 1600 && silentRegions[1].start == 1601 &&
                  silentRegions[1].end == 2300,
              "source silence requires every channel to be zero and includes exact boundaries");
        silentRegions = {{4, 5}};
        check(!carta::findClipSourceSilence(clip, 1, silentRegions, silenceError,
                                            [](double) { return false; }) &&
                  silentRegions.size() == 1 && silentRegions[0].start == 4 &&
                  silenceError.contains("cancelled"),
              "cancelled source silence scan leaves results untouched");
        source->samples.setSample(0, 1000, std::numeric_limits<float>::quiet_NaN());
        check(!carta::findClipSourceSilence(clip, 1, silentRegions, silenceError) &&
                  silentRegions[0].start == 4 && silenceError.contains("non-finite"),
              "non-finite source silence scan leaves results untouched");
        source->samples.setSample(0, 1000, 0);
        source->sampleRate = 48000;
        clip.duration = source->length();
        check(carta::findClipSourceSilence(clip, 1, silentRegions, silenceError) &&
                  !silentRegions.empty(),
              "mixed-rate source silence scan finds conservative regions");
        const int taps = carta::SincResampler::supportTaps(48000, carta::sessionRate);
        for (const auto& region : silentRegions)
            for (auto frame = region.start; frame < region.end; ++frame) {
                const auto index = static_cast<int>(std::floor(
                    (static_cast<double>(frame) / carta::sessionRate - clip.start) * 48000));
                for (int i = std::max(0, index - (taps / 2 - 1));
                     i < std::min(source->numSamples(), index + taps / 2 + 1); ++i)
                    check(std::fpclassify(source->samples.getSample(0, i)) == FP_ZERO &&
                              std::fpclassify(source->samples.getSample(1, i)) == FP_ZERO,
                          "resampled silent region only uses zero-valued source samples");
            }
        juce::int64 begin = 0, end = 0;
        check(carta::clipSampleRange(clip, begin, end), "resolve mixed-rate silence clip span");
        for (auto frame = begin; frame < end; ++frame) {
            auto position = (static_cast<double>(frame) / carta::sessionRate - clip.start) * 48000;
            const auto nearest = std::round(position);
            if (std::abs(position - nearest) < 1e-7)
                position = nearest;
            const int index = static_cast<int>(std::floor(position));
            bool expected = true;
            for (int i = std::max(0, index - (taps / 2 - 1));
                 i < std::min(source->numSamples(), index + taps / 2 + 1); ++i)
                for (int channel = 0; channel < 2; ++channel)
                    expected = expected &&
                               std::fpclassify(source->samples.getSample(channel, i)) == FP_ZERO;
            const bool found =
                std::any_of(silentRegions.begin(), silentRegions.end(), [&](const auto& region) {
                    return frame >= region.start && frame < region.end;
                });
            check(found == expected,
                  "mixed-rate silence regions match an independent support-window scan");
        }
    }
    {
        auto recording = std::make_shared<carta::Audio>();
        recording->sampleRate = carta::sessionRate;
        recording->samples.setSize(2, 14000);
        recording->samples.clear();
        auto overlayAudio = std::make_shared<carta::Audio>();
        overlayAudio->sampleRate = carta::sessionRate;
        overlayAudio->samples.setSize(1, 14000);
        overlayAudio->samples.clear();
        for (int i = 0; i < 14000; ++i) {
            if (i < 3000 || i >= 11000) {
                recording->samples.setSample(0, i, 0.3f);
                recording->samples.setSample(1, i, -0.2f);
            }
            overlayAudio->samples.setSample(0, i, 0.1f);
        }
        carta::Session original;
        original.tracks.push_back({"Overlap",
                                   {{recording, 0, 0, recording->length()},
                                    {overlayAudio, 0, 0, overlayAudio->length()}}});
        original.markers = {{13000 / carta::sessionRate, "End"}};
        std::vector<carta::QuietRegion> safe;
        juce::String problem;
        check(
            carta::findClipSourceSilence(original.tracks[0].clips[0], 1, safe, problem, {}, true) &&
                safe.size() == 1 && safe[0].start > 3000 && safe[0].end < 11000,
            "conservative silence scan leaves interpolation margins on both sides");
        auto detached = original;
        check(carta::detachClipRange(detached, 0, 0, safe[0].start, safe[0].end, problem) &&
                  detached.tracks[0].clips.size() == 3 &&
                  detached.tracks[0].clips[2].audio == overlayAudio && detached.markers.size() == 1,
              "detach splits only the target clip and keeps overlapping audio and markers");
        for (double outputRate : {22050.0, 44100.0, 48000.0, 96000.0}) {
            const int frames = static_cast<int>(std::ceil(original.length() * outputRate));
            juce::AudioBuffer<float> before(2, frames), detachedMix(2, frames);
            before.clear();
            detachedMix.clear();
            original.render(before, 0, frames, 0, outputRate, 2);
            detached.render(detachedMix, 0, frames, 0, outputRate, 2);
            for (int channel = 0; channel < 2; ++channel)
                check(std::memcmp(before.getReadPointer(channel),
                                  detachedMix.getReadPointer(channel),
                                  static_cast<size_t>(frames) * sizeof(float)) == 0,
                      "detaching source silence preserves the rendered mix at multiple playback "
                      "rates");
        }
        const auto middle = safe[0].start + (safe[0].end - safe[0].start) / 2;
        const std::vector<carta::QuietRegion> batchRanges{{safe[0].start, middle - 10},
                                                          {middle + 10, safe[0].end}};
        auto batch = original;
        check(carta::detachClipRanges(batch, 0, 0, batchRanges, problem) &&
                  batch.tracks[0].clips.size() == 4 &&
                  batch.tracks[0].clips[3].audio == overlayAudio,
              "batch detach splits only the scanned clip for multiple ranges");
        auto invalidBatch = original;
        check(!carta::detachClipRanges(invalidBatch, 0, 0,
                                       {{safe[0].start, middle}, {middle - 1, safe[0].end}},
                                       problem) &&
                  invalidBatch.tracks[0].clips.size() == 2 &&
                  invalidBatch.tracks[0].clips[0].audio == recording,
              "overlapping batch detach ranges reject atomically");
        juce::AudioBuffer<float> batchBefore(2, 14000), batchAfter(2, 14000);
        batchBefore.clear();
        batchAfter.clear();
        original.render(batchBefore, 0, 14000, 0, carta::sessionRate, 2);
        batch.render(batchAfter, 0, 14000, 0, carta::sessionRate, 2);
        for (int channel = 0; channel < 2; ++channel)
            check(std::memcmp(batchBefore.getReadPointer(channel),
                              batchAfter.getReadPointer(channel), 14000 * sizeof(float)) == 0,
                  "batch detach preserves the overlapping mix");
        auto grouped = original;
        check(carta::groupClips(grouped, {{0, 0}, {0, 1}}, problem),
              "prepare grouped source for detach rejection");
        check(!carta::detachClipRange(grouped, 0, 0, safe[0].start, safe[0].end, problem) &&
                  grouped.tracks[0].clips.size() == 2 && problem.contains("Ungroup"),
              "detach rejects a persistent group without changing any clip");
        auto trailing = original;
        trailing.tracks[0].clips.resize(1);
        trailing.markers = {{13000 / carta::sessionRate, "End"}};
        check(carta::detachClipRange(trailing, 0, 0, 12000, 14000, problem) &&
                  carta::projectEndSample(trailing) == 14000 && trailing.markers.size() == 1,
              "detaching a clip tail retains the master duration and markers");
    }
    {
        auto leftAudio = std::make_shared<carta::Audio>();
        leftAudio->sampleRate = carta::sessionRate;
        leftAudio->samples.setSize(1, 1000);
        auto rightAudio = std::make_shared<carta::Audio>();
        rightAudio->sampleRate = 48000;
        rightAudio->samples.setSize(1, 1200);
        for (int i = 0; i < 1000; ++i)
            leftAudio->samples.setSample(0, i, 0.5f);
        for (int i = 0; i < 1200; ++i)
            rightAudio->samples.setSample(0, i, 0.25f);
        carta::Session project;
        project.tracks.push_back({"Left", {{leftAudio, 0, 0, leftAudio->length()}}});
        project.tracks.push_back({"Other", {{leftAudio, 0, 0, leftAudio->length()}}});
        project.tracks.push_back(
            {"Right", {{rightAudio, 100 / carta::sessionRate, 0, 900 / carta::sessionRate}}});
        project.tracks[0].gain = 0.5f;
        project.tracks[0].pan = -1;
        project.tracks[0].muted = true;
        project.tracks[0].routing = {{0, 1}};
        project.tracks[1].solo = true;
        project.tracks[2].gain = 0.5f;
        project.tracks[2].pan = 1;
        project.minimumEndSample = 100000;
        carta::Session render;
        juce::String pairError;
        juce::int64 pairStart = -1, pairEnd = -1;
        check(
            carta::prepareMonoStereoPair(project, 0, 2, render, pairError, &pairStart, &pairEnd) &&
                pairStart == 0 && pairEnd == 1000 && render.tracks.size() == 3 &&
                !render.tracks[0].muted && render.tracks[1].muted && !render.tracks[2].muted &&
                project.tracks[0].muted && project.tracks[1].solo,
            "stereo pair isolates the chosen mono tracks and reports their span without changing "
            "the project");
        juce::AudioBuffer<float> pairMix(2, 1000);
        pairMix.clear();
        render.render(pairMix, 0, 1000, 0, carta::sessionRate, 2);
        for (int i = 0; i < 1000; ++i) {
            check(std::abs(pairMix.getSample(0, i) - 0.25f) < 1e-6f,
                  "left channel uses the first source and its track gain");
            check(std::abs(pairMix.getSample(1, i) - (i < 100 ? 0.0f : 0.125f)) < 1e-6f,
                  "right channel uses the second mixed-rate source at its timeline position");
        }
        carta::Session reversed;
        check(carta::prepareMonoStereoPair(project, 2, 0, reversed, pairError),
              "explicit pair order can be reversed");
        juce::AudioBuffer<float> reverseMix(2, 1000);
        reverseMix.clear();
        reversed.render(reverseMix, 0, 1000, 0, carta::sessionRate, 2);
        for (int i = 0; i < 1000; ++i)
            check(std::abs(reverseMix.getSample(0, i) - pairMix.getSample(1, i)) < 1e-7f &&
                      std::abs(reverseMix.getSample(1, i) - pairMix.getSample(0, i)) < 1e-7f,
                  "reversed pair swaps stereo channels");
        auto invalid = project;
        invalid.tracks[2].clips[0].audio = std::make_shared<carta::Audio>();
        invalid.tracks[2].clips[0].audio->samples.setSize(2, 1000);
        carta::Session preserved = render;
        check(!carta::prepareMonoStereoPair(invalid, 0, 2, preserved, pairError) &&
                  preserved.tracks[0].muted == render.tracks[0].muted &&
                  !carta::prepareMonoStereoPair(project, 0, 0, preserved, pairError),
              "stereo or duplicate inputs reject without changing the output snapshot");
        auto tooShort = project;
        tooShort.tracks[2].clips[0].start = 0.1 / carta::sessionRate;
        tooShort.tracks[2].clips[0].duration = 0.1 / carta::sessionRate;
        pairStart = 42;
        pairEnd = 43;
        check(!carta::prepareMonoStereoPair(tooShort, 0, 2, preserved, pairError, &pairStart,
                                            &pairEnd) &&
                  pairStart == 42 && pairEnd == 43,
              "a track with no project-sample positions rejects without changing the output or "
              "reported bounds");
    }
    {
        auto quietSource = std::make_shared<carta::Audio>();
        quietSource->sampleRate = carta::sessionRate;
        quietSource->samples.setSize(2, 10000);
        for (int i = 0; i < 10000; ++i)
            for (int ch = 0; ch < 2; ++ch)
                quietSource->samples.setSample(ch, i, 0.5f);
        for (int i = 0; i < 100; ++i)
            for (int ch = 0; ch < 2; ++ch)
                quietSource->samples.setSample(ch, i, 0.001f);
        for (int i = 4070; i < 4210; ++i)
            for (int ch = 0; ch < 2; ++ch)
                quietSource->samples.setSample(ch, i, 0.001f);
        for (int i = 6000; i < 6050; ++i)
            for (int ch = 0; ch < 2; ++ch)
                quietSource->samples.setSample(ch, i, 0.001f);
        for (int i = 6500; i < 6700; ++i)
            quietSource->samples.setSample(0, i, 0.001f);
        for (int i = 9000; i < 10000; ++i)
            for (int ch = 0; ch < 2; ++ch)
                quietSource->samples.setSample(ch, i, 0.001f);
        carta::Session quietSession;
        quietSession.tracks.push_back({"Audio", {{quietSource, 0, 0, quietSource->length()}}});
        std::vector<carta::QuietRegion> gaps{{9, 10}};
        juce::String quietError;
        double progress = -1;
        bool monotonic = true;
        check(carta::findQuietRegions(quietSession, 2, -50, 100, gaps, quietError,
                                      [&](double p) {
                                          if (p < progress)
                                              monotonic = false;
                                          progress = p;
                                          return true;
                                      }) &&
                  gaps.size() == 1 && gaps[0].start == 4070 && gaps[0].end == 4210 && monotonic &&
                  std::abs(progress - 1) < 1.0e-12,
              "quiet-gap analysis crosses render blocks and excludes leading, trailing and short "
              "runs");
        auto monoQuiet = quietSession;
        check(carta::findQuietRegions(monoQuiet, 1, -50, 100, gaps, quietError) &&
                  gaps.size() == 1 && gaps[0].start == 4070,
              "quiet-gap analysis supports mono master");
        gaps = {{9, 10}};
        check(!carta::findQuietRegions(quietSession, 2, -50, 100, gaps, quietError,
                                       [](double p) { return p < 0.4; }) &&
                  quietError.contains("cancelled") && gaps.size() == 1 && gaps[0].start == 9,
              "cancelled gap analysis preserves result");
        quietSource->samples.setSample(0, 4200, std::numeric_limits<float>::quiet_NaN());
        check(!carta::findQuietRegions(quietSession, 2, -50, 100, gaps, quietError) &&
                  quietError.contains("non-finite") && gaps[0].start == 9,
              "non-finite quiet-gap mix preserves result");
        quietSource->samples.setSample(0, 4200, 0.001f);
        quietSession.markers = {{1000 / carta::sessionRate, "Existing"}};
        juce::int64 parsedSample = 7;
        juce::String parsedName = "Previous", parseError;
        check(carta::parseQuietMarkerSuggestion(juce::String::fromUTF8(u8" 4140 日本語 "), 10000,
                                                parsedSample, parsedName, parseError) &&
                  parsedSample == 4140 && parsedName == juce::String::fromUTF8(u8"日本語"),
              "review and marker creation parse the same Unicode suggestion line");
        check(!carta::parseQuietMarkerSuggestion("10000 Invalid", 10000, parsedSample, parsedName,
                                                 parseError) &&
                  parsedSample == 4140 && parsedName == juce::String::fromUTF8(u8"日本語"),
              "invalid audition line leaves the parsed result unchanged");
        juce::StringArray suggested;
        std::vector<bool> occupied;
        check(carta::quietMarkerSuggestions(quietSession, {{4070, 4210}, {6000, 6050}}, suggested,
                                            occupied, quietError) &&
                  suggested.size() == 2 && suggested[0].startsWith("4140 ") &&
                  occupied.size() == 2 && !occupied[0],
              "quiet-gap suggestions use deterministic midpoints and names");
        auto marked = quietSession;
        marked.markers.push_back({4070 / carta::sessionRate, "At start"});
        marked.markers.push_back({6050 / carta::sessionRate, "At end"});
        check(carta::quietMarkerSuggestions(marked, {{4070, 4210}, {6000, 6050}}, suggested,
                                            occupied, quietError) &&
                  occupied[0] && !occupied[1] && suggested.size() == 1 &&
                  suggested[0].startsWith("6025 "),
              "suggestion suppression uses half-open gaps including a marker at the start but "
              "excluding one at the end");
        const auto previous = suggested.joinIntoString("\n");
        check(!carta::quietMarkerSuggestions(quietSession, {{4070, 4210}, {4200, 4300}}, suggested,
                                             occupied, quietError) &&
                  suggested.joinIntoString("\n") == previous && occupied.size() == 2 && occupied[0],
              "invalid overlapping gaps preserve previously returned suggestions");
        auto preservedMarkers = quietSession;
        check(!carta::addSuggestedMarkers(quietSession, "4140 First\n1000 Collision", quietError) &&
                  quietSession.markers.size() == 1 && quietSession.markers[0].name == "Existing",
              "colliding suggestion rejects the entire batch");
        check(!carta::addSuggestedMarkers(quietSession, "4140 First\n4140 Duplicate", quietError) &&
                  quietSession.markers.size() == 1,
              "duplicate suggestions reject the entire batch");
        auto fractionalCollision = quietSession;
        fractionalCollision.markers[0].position = 4140.4 / carta::sessionRate;
        check(!carta::addSuggestedMarkers(fractionalCollision, "4140 Name", quietError) &&
                  fractionalCollision.markers.size() == 1,
              "suggestions reject collisions with rounded legacy marker positions");
        check(!carta::addSuggestedMarkers(quietSession, "0 Invalid", quietError) &&
                  !carta::addSuggestedMarkers(quietSession, "10000 End", quietError) &&
                  !carta::addSuggestedMarkers(quietSession, "abc Invalid", quietError) &&
                  quietSession.markers.size() == 1,
              "malformed or boundary suggestions preserve markers");
        carta::EditorState beforeSuggestions;
        beforeSuggestions.session = quietSession;
        carta::EditHistory suggestionHistory;
        suggestionHistory.record(beforeSuggestions, "Add suggested markers");
        check(carta::addSuggestedMarkers(
                  quietSession, juce::String::fromUTF8(u8"4140 First\n7000 日本語"), quietError) &&
                  quietSession.markers.size() == 3 &&
                  quietSession.markers[2].name == juce::String::fromUTF8(u8"日本語"),
              "editable suggestions add named markers without replacing existing markers");
        carta::EditorState applied;
        applied.session = quietSession;
        check(suggestionHistory.undo(applied) &&
                  applied.session.markers.size() == preservedMarkers.markers.size() &&
                  suggestionHistory.redo(applied) && applied.session.markers.size() == 3,
              "suggestion batch supports undo and redo");
    }
    {
        for (const auto sampleOffset : {0, 1, 100, 1000}) {
            const auto longStart =
                static_cast<juce::int64>(80 * 60) * static_cast<juce::int64>(carta::sessionRate) +
                sampleOffset;
            carta::Session placed;
            placed.tracks.push_back({"Late",
                                     {{loaded, static_cast<double>(longStart) / carta::sessionRate,
                                       0, loaded->length()}}});
            confirmDefaultRouting(placed.tracks[0]);
            carta::Session atOrigin = placed;
            atOrigin.tracks[0].clips[0].start = 0;
            juce::AudioBuffer<float> expectedLate(2, 128), actualLate(2, 128);
            expectedLate.clear();
            actualLate.clear();
            atOrigin.render(expectedLate, 0, 128, 0, carta::sessionRate, 2);
            placed.render(actualLate, 0, 128, static_cast<double>(longStart) / carta::sessionRate,
                          carta::sessionRate, 2);
            for (int ch = 0; ch < 2; ++ch)
                check(std::memcmp(expectedLate.getReadPointer(ch), actualLate.getReadPointer(ch),
                                  128 * sizeof(float)) == 0,
                      "short clip placed at an 80-minute timeline position preserves "
                      "project-sample render");
            juce::String trimError;
            auto& lateClip = placed.tracks[0].clips[0];
            for (int iteration = 0; iteration < 1000; ++iteration) {
                const auto cut = 1 + (iteration % 127);
                check(
                    carta::trimClipToSamples(lateClip, longStart + cut, longStart + 128, trimError),
                    "trim near end of a full-disc timeline");
                check(carta::trimClipToSamples(lateClip, longStart, longStart + 128, trimError),
                      "untrim back to the exact source boundary near end of a full-disc timeline");
            }
            actualLate.clear();
            placed.render(actualLate, 0, 128, static_cast<double>(longStart) / carta::sessionRate,
                          carta::sessionRate, 2);
            for (int ch = 0; ch < 2; ++ch)
                check(std::memcmp(expectedLate.getReadPointer(ch), actualLate.getReadPointer(ch),
                                  128 * sizeof(float)) == 0,
                      "repeated full-disc-position trim and untrim preserves rendered samples "
                      "bit-for-bit");
            const auto unchangedLate = lateClip;
            check(!carta::trimClipToSamples(lateClip, longStart, longStart + 129, trimError) &&
                      !carta::trimClipToSamples(lateClip, longStart - 1, longStart + 128,
                                                trimError) &&
                      std::memcmp(&lateClip.start, &unchangedLate.start, sizeof(double)) == 0 &&
                      std::memcmp(&lateClip.offset, &unchangedLate.offset, sizeof(double)) == 0 &&
                      std::memcmp(&lateClip.duration, &unchangedLate.duration, sizeof(double)) == 0,
                  "out-of-source trims remain rejected without mutating clip geometry");
            auto fractionalEdge = unchangedLate;
            fractionalEdge.start = static_cast<double>(longStart + 1) / carta::sessionRate;
            fractionalEdge.offset = 1.0 / carta::sessionRate - 1.0e-12;
            fractionalEdge.duration = 100.0 / carta::sessionRate;
            check(!carta::trimClipToSamples(fractionalEdge, longStart, longStart + 100, trimError),
                  "a real fractional source overrun larger than roundoff is rejected");
            if (sampleOffset == 0) {
                const auto lateProject = dir.getChildFile("late-trim.carta");
                check(carta::saveSession(placed, 2, lateProject, trimError),
                      "save long-position trimmed arrangement");
                carta::Session reopenedLate;
                int reopenedMode = 0;
                check(
                    carta::loadSession(reopenedLate, reopenedMode, formats, lateProject, trimError),
                    "reopen long-position trimmed arrangement");
                juce::AudioBuffer<float> reopenedAudio(2, 128);
                reopenedAudio.clear();
                reopenedLate.render(reopenedAudio, 0, 128,
                                    static_cast<double>(longStart) / carta::sessionRate,
                                    carta::sessionRate, 2);
                for (int ch = 0; ch < 2; ++ch)
                    check(std::memcmp(expectedLate.getReadPointer(ch),
                                      reopenedAudio.getReadPointer(ch), 128 * sizeof(float)) == 0,
                          "long-position trim survives project round trip bit-for-bit");
            }
        }
        auto longSource = std::make_shared<carta::Audio>();
        longSource->sampleRate = carta::sessionRate;
        longSource->samples.setSize(1, 100000);
        longSource->samples.clear();
        const auto base =
            static_cast<juce::int64>(80 * 60) * static_cast<juce::int64>(carta::sessionRate);
        carta::Clip sourceBoundary{longSource, static_cast<double>(base) / carta::sessionRate, 0,
                                   longSource->length()};
        juce::String boundaryError;
        check(
            carta::trimClipToSamples(sourceBoundary, base + 1004, base + 100000, boundaryError) &&
                carta::trimClipToSamples(sourceBoundary, base, base + 100000, boundaryError),
            "sample-aligned source end tolerates only arithmetic roundoff during trim and untrim");
        check(!carta::trimClipToSamples(sourceBoundary, base, base + 100001, boundaryError),
              "one full sample beyond the source remains invalid");
        auto detailed = std::make_shared<carta::Audio>();
        detailed->sampleRate = carta::sessionRate;
        detailed->samples.setSize(2, 256);
        for (int i = 0; i < 256; ++i) {
            detailed->samples.setSample(0, i, static_cast<float>((i * 17) % 31 - 15) / 32.0f);
            detailed->samples.setSample(1, i, static_cast<float>((i * 23) % 29 - 14) / 32.0f);
        }
        {
            const auto fractionalFile = dir.getChildFile("fractional-nudge-48k.wav");
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> stream = fractionalFile.createOutputStream();
            const auto writerOptions = juce::AudioFormatWriterOptions{}
                                           .withSampleRate(48000)
                                           .withNumChannels(2)
                                           .withBitsPerSample(24);
            auto writer = wav.createWriterFor(stream, writerOptions);
            check(writer != nullptr, "create fractional mixed-rate nudge fixture");
            juce::AudioBuffer<float> samples(2, 256);
            for (int i = 0; i < 256; ++i) {
                samples.setSample(0, i, static_cast<float>((i * 17) % 31 - 15) / 32.0f);
                samples.setSample(1, i, static_cast<float>((i * 23) % 29 - 14) / 32.0f);
            }
            check(writer->writeFromAudioSampleBuffer(samples, 0, 256),
                  "write fractional mixed-rate nudge fixture");
            writer.reset();
            auto mixedSource = carta::loadAudio(formats, fractionalFile, error);
            check(mixedSource != nullptr, "load fractional mixed-rate nudge fixture");
            for (int displacement : {0, 1, 100, 1000})
                for (double sourceOffset : {10.375, 10.10908667557703}) {
                    const auto first = base + displacement;
                    carta::Session fractionalSession;
                    fractionalSession.tracks.push_back(
                        {"Fractional",
                         {{mixedSource, (static_cast<double>(first) + 0.25) / carta::sessionRate,
                           sourceOffset / 48000.0, 128.5 / carta::sessionRate}}});
                    auto& fractionalClip = fractionalSession.tracks[0].clips[0];
                    fractionalClip.fadeIn = 60.0 / carta::sessionRate;
                    fractionalClip.fadeInStart = fractionalClip.offset;
                    carta::ResamplingContext context;
                    fractionalSession.prepareResampling(context, carta::sessionRate);
                    juce::AudioBuffer<float> referenceFractional(2, 128);
                    referenceFractional.clear();
                    fractionalSession.render(referenceFractional, 0, 128,
                                             static_cast<double>(first + 1) / carta::sessionRate,
                                             carta::sessionRate, 2, &context);
                    juce::String fractionalError;
                    for (int n = 1; n <= 80; ++n) {
                        check(carta::nudgeClipEdge(fractionalClip, false, 1, fractionalError),
                              "fractional start nudge succeeds at full-disc position");
                        juce::int64 firstSample = 0, lastSample = 0;
                        check(carta::clipSampleRange(fractionalClip, firstSample, lastSample) &&
                                  firstSample == first + n + 1 && lastSample == first + 129,
                              "fractional nudges preserve included sample boundaries");
                        juce::AudioBuffer<float> output(2, 128 - n);
                        output.clear();
                        fractionalSession.render(output, 0, 128 - n,
                                                 static_cast<double>(first + n + 1) /
                                                     carta::sessionRate,
                                                 carta::sessionRate, 2, &context);
                        for (int ch = 0; ch < 2; ++ch)
                            for (int i = 0; i < 128 - n; ++i) {
                                const float difference =
                                    std::abs(output.getSample(ch, i) -
                                             referenceFractional.getSample(ch, n + i));
                                if (difference > 2.0e-7f)
                                    std::cerr << "Fractional nudge diagnostic: step " << n
                                              << ", channel " << ch << ", sample " << i
                                              << ", difference " << difference << '\n';
                                check(difference <= 2.0e-7f, "fractional nudges preserve the "
                                                             "retained mixed-rate audio and fade");
                            }
                        if (n == 80 && displacement == 100 && sourceOffset < 10.2) {
                            const auto project = dir.getChildFile("fractional-nudge-48k.carta");
                            check(
                                carta::saveSession(fractionalSession, 2, project, fractionalError),
                                "save mixed-rate fractional nudges");
                            carta::Session reopened;
                            int master = 0;
                            check(carta::loadSession(reopened, master, formats, project,
                                                     fractionalError),
                                  "reopen mixed-rate fractional nudges");
                            const auto& savedClip = reopened.tracks[0].clips[0];
                            check(std::memcmp(&savedClip.start, &fractionalClip.start,
                                              sizeof(double)) == 0 &&
                                      std::memcmp(&savedClip.offset, &fractionalClip.offset,
                                                  sizeof(double)) == 0 &&
                                      std::memcmp(&savedClip.duration, &fractionalClip.duration,
                                                  sizeof(double)) == 0,
                                  "fractional nudge geometry survives project save exactly");
                            juce::AudioBuffer<float> reopenedAudio(2, 128 - n);
                            reopenedAudio.clear();
                            reopened.render(reopenedAudio, 0, 128 - n,
                                            static_cast<double>(first + n + 1) / carta::sessionRate,
                                            carta::sessionRate, 2);
                            for (int ch = 0; ch < 2; ++ch)
                                check(std::memcmp(output.getReadPointer(ch),
                                                  reopenedAudio.getReadPointer(ch),
                                                  static_cast<size_t>(128 - n) * sizeof(float)) ==
                                          0,
                                      "fractional nudge audio survives project reopening "
                                      "bit-for-bit");
                        }
                    }
                    for (int n = 0; n < 80; ++n)
                        check(carta::nudgeClipEdge(fractionalClip, false, -1, fractionalError),
                              "reverse fractional full-disc nudges");
                    juce::AudioBuffer<float> restoredFractional(2, 128);
                    restoredFractional.clear();
                    fractionalSession.render(restoredFractional, 0, 128,
                                             static_cast<double>(first + 1) / carta::sessionRate,
                                             carta::sessionRate, 2, &context);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 128; ++i)
                            check(std::abs(restoredFractional.getSample(ch, i) -
                                           referenceFractional.getSample(ch, i)) <= 2.0e-7f,
                                  "reverse fractional nudges restore retained audio");
                    for (int n = 1; n <= 80; ++n) {
                        check(carta::nudgeClipEdge(fractionalClip, true, -1, fractionalError),
                              "fractional end nudge succeeds at full-disc position");
                        juce::int64 firstSample = 0, lastSample = 0;
                        check(carta::clipSampleRange(fractionalClip, firstSample, lastSample) &&
                                  firstSample == first + 1 && lastSample == first + 129 - n,
                              "fractional end nudges preserve sample boundaries");
                        juce::AudioBuffer<float> output(2, 128);
                        output.clear();
                        fractionalSession.render(
                            output, 0, 128, static_cast<double>(first + 1) / carta::sessionRate,
                            carta::sessionRate, 2, &context);
                        for (int ch = 0; ch < 2; ++ch) {
                            for (int i = 0; i < 128 - n; ++i)
                                check(std::abs(output.getSample(ch, i) -
                                               referenceFractional.getSample(ch, i)) <= 2.0e-7f,
                                      "fractional end nudge preserves retained audio");
                            for (int i = 128 - n; i < 128; ++i)
                                check(std::fpclassify(output.getSample(ch, i)) == FP_ZERO,
                                      "fractional end nudge excludes later project samples");
                        }
                    }
                    for (int n = 0; n < 80; ++n)
                        check(carta::nudgeClipEdge(fractionalClip, true, 1, fractionalError),
                              "reverse fractional end nudge");
                    restoredFractional.clear();
                    fractionalSession.render(restoredFractional, 0, 128,
                                             static_cast<double>(first + 1) / carta::sessionRate,
                                             carta::sessionRate, 2, &context);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 128; ++i)
                            check(std::abs(restoredFractional.getSample(ch, i) -
                                           referenceFractional.getSample(ch, i)) <= 2.0e-7f,
                                  "reverse fractional end nudges restore audio");
                }
        }
        for (const int displacement : {0, 1, 100, 1000}) {
            const auto first = base + displacement;
            carta::Session source;
            source.tracks.push_back({"Nudge",
                                     {{detailed, static_cast<double>(first) / carta::sessionRate, 0,
                                       detailed->length()}}});
            auto& edge = source.tracks[0].clips[0];
            juce::String edgeError;
            for (int n = 1; n <= 100; ++n) {
                check(carta::nudgeClipEdge(edge, false, 1, edgeError),
                      "start-edge nudge succeeds at a full-disc position");
                juce::int64 actualStart = 0, actualEnd = 0;
                if (!carta::clipSampleRange(edge, actualStart, actualEnd) ||
                    actualStart != first + n || actualEnd != first + 256)
                    std::cerr << "Nudge boundary diagnostic: offset " << displacement << ", step "
                              << n << ", start " << actualStart << ", end " << actualEnd << '\n';
                check(actualStart == first + n && actualEnd == first + 256,
                      "repeated start nudges preserve the exact expected sample boundaries");
                juce::AudioBuffer<float> output(2, 256);
                output.clear();
                source.render(output, 0, 256 - n,
                              static_cast<double>(first + n) / carta::sessionRate,
                              carta::sessionRate, 2);
                for (int ch = 0; ch < 2; ++ch) {
                    if (std::memcmp(output.getReadPointer(ch), detailed->channelData(ch) + n,
                                    static_cast<size_t>(256 - n) * sizeof(float)) != 0)
                        std::cerr << "Nudge audio diagnostic: offset " << displacement << ", step "
                                  << n << ", channel " << ch << '\n';
                    check(std::memcmp(output.getReadPointer(ch), detailed->channelData(ch) + n,
                                      static_cast<size_t>(256 - n) * sizeof(float)) == 0,
                          "nudged source stays aligned to the expected original samples");
                }
            }
            for (int n = 0; n < 100; ++n)
                check(carta::nudgeClipEdge(edge, false, -1, edgeError),
                      "reverse full-disc start nudge");
            juce::int64 restoredStart = 0, restoredEnd = 0;
            check(carta::clipSampleRange(edge, restoredStart, restoredEnd) &&
                      restoredStart == first && restoredEnd == first + 256,
                  "reversing nudges restores original sample boundaries");
            juce::AudioBuffer<float> restored(2, 256);
            restored.clear();
            source.render(restored, 0, 256, static_cast<double>(first) / carta::sessionRate,
                          carta::sessionRate, 2);
            for (int ch = 0; ch < 2; ++ch)
                check(std::memcmp(restored.getReadPointer(ch), detailed->channelData(ch),
                                  256 * sizeof(float)) == 0,
                      "reversed start nudges restore original audio exactly");
            for (int n = 1; n <= 100; ++n) {
                check(carta::nudgeClipEdge(edge, true, -1, edgeError),
                      "end-edge nudge succeeds at a full-disc position");
                juce::int64 actualStart = 0, actualEnd = 0;
                check(carta::clipSampleRange(edge, actualStart, actualEnd) &&
                          actualStart == first && actualEnd == first + 256 - n,
                      "repeated end nudges preserve exact expected sample boundaries");
                juce::AudioBuffer<float> output(2, 256);
                output.clear();
                source.render(output, 0, 256, static_cast<double>(first) / carta::sessionRate,
                              carta::sessionRate, 2);
                for (int ch = 0; ch < 2; ++ch) {
                    check(std::memcmp(output.getReadPointer(ch), detailed->channelData(ch),
                                      static_cast<size_t>(256 - n) * sizeof(float)) == 0,
                          "end nudges preserve retained samples bit-for-bit");
                    for (int i = 256 - n; i < 256; ++i)
                        check(std::fpclassify(output.getSample(ch, i)) == FP_ZERO,
                              "end nudges exclude samples beyond the trimmed boundary");
                }
            }
            for (int n = 0; n < 100; ++n)
                check(carta::nudgeClipEdge(edge, true, 1, edgeError),
                      "reverse full-disc end nudge");
            restored.clear();
            source.render(restored, 0, 256, static_cast<double>(first) / carta::sessionRate,
                          carta::sessionRate, 2);
            for (int ch = 0; ch < 2; ++ch)
                check(std::memcmp(restored.getReadPointer(ch), detailed->channelData(ch),
                                  256 * sizeof(float)) == 0,
                      "reversed end nudges restore original audio exactly");
        }
    }
    carta::Session saved;
    saved.tracks.push_back({"Example", {{loaded, 0, 0, loaded->length()}}});
    saved.tracks[0].height = 220;
    saved.tracks[0].collapsed = true;
    saved.tracks[0].gain = 0.5f;
    saved.tracks[0].pan = -0.25f;
    saved.tracks[0].solo = true;
    saved.tracks[0].view = carta::TrackView::spectrogram;
    confirmDefaultRouting(saved.tracks[0]);
    saved.markers.push_back({0.001, "Track 1"});
    {
        carta::Track track;
        check(!carta::trackShowsSpectrum(track, false) && carta::trackShowsSpectrum(track, true),
              "default track view follows the global view");
        track.view = carta::TrackView::waveform;
        check(!carta::trackShowsSpectrum(track, false) && !carta::trackShowsSpectrum(track, true),
              "waveform override ignores global view");
        track.view = carta::TrackView::spectrogram;
        check(carta::trackShowsSpectrum(track, false) && carta::trackShowsSpectrum(track, true),
              "spectrogram override ignores global view");
        for (int height : {0, 1, 2, 39, 100}) {
            for (bool defaultSpectrum : {false, true}) {
                const auto both =
                    carta::trackViewLayout(height, carta::TrackView::combined, defaultSpectrum);
                check(both.waveformTop == 0 && both.waveformHeight == (height + 1) / 2 &&
                          both.spectrumTop == both.waveformHeight &&
                          both.spectrumHeight == height - both.waveformHeight,
                      "combined view partitions track content without overlap");
                const auto fallback =
                    carta::trackViewLayout(height, carta::TrackView::useDefault, defaultSpectrum);
                check(fallback.waveformHeight == (defaultSpectrum ? 0 : height) &&
                          fallback.spectrumHeight == (defaultSpectrum ? height : 0),
                      "default track layout follows global view");
                const auto wave =
                    carta::trackViewLayout(height, carta::TrackView::waveform, defaultSpectrum);
                const auto spectrumLayout =
                    carta::trackViewLayout(height, carta::TrackView::spectrogram, defaultSpectrum);
                check(wave.waveformHeight == height && wave.spectrumHeight == 0 &&
                          spectrumLayout.waveformHeight == 0 &&
                          spectrumLayout.spectrumHeight == height,
                      "track view overrides global display");
            }
        }
        for (int height = 1; height <= 101; ++height)
            for (int channels = 1; channels <= 8; ++channels) {
                int end = 0;
                for (int lane = 0; lane < channels; ++lane) {
                    const auto bounds = carta::channelLaneBounds(height, lane, channels);
                    check(bounds.getStart() == end && bounds.getEnd() >= end,
                          "channel lanes partition content in order");
                    end = bounds.getEnd();
                }
                check(end == height, "channel lanes cover the full content height");
            }
        check(carta::channelLaneBounds(10, -1, 2).isEmpty() &&
                  carta::channelLaneBounds(10, 2, 2).isEmpty() &&
                  carta::channelLaneBounds(10, 0, 0).isEmpty(),
              "invalid channel lane geometry is empty");
        for (int height = 1; height <= 101; ++height) {
            const auto lanes = carta::channelLaneLayout(height, 3, {3, 1, 2});
            check(lanes.size() == 3 && lanes[0].getStart() == 0 &&
                      lanes[0].getEnd() == lanes[1].getStart() &&
                      lanes[1].getEnd() == lanes[2].getStart() && lanes[2].getEnd() == height,
                  "weighted lanes partition every content pixel");
            check(lanes[0].getEnd() == height / 2 && lanes[1].getEnd() == height * 4 / 6,
                  "weighted lanes use the specified relative heights");
            const auto fallback = carta::channelLaneLayout(height, 2, {3, 1, 2});
            check(fallback.size() == 2 && fallback[0] == carta::channelLaneBounds(height, 0, 2) &&
                      fallback[1] == carta::channelLaneBounds(height, 1, 2),
                  "channel-count mismatch falls back to equal lanes");
        }
        check(carta::validChannelHeightWeights({}) && carta::validChannelHeightWeights({1, 1000}) &&
                  !carta::validChannelHeightWeights({1}) &&
                  !carta::validChannelHeightWeights({0, 1}) &&
                  !carta::validChannelHeightWeights({1001, 1}),
              "channel-height weights enforce supported bounds");
        carta::EditorState state;
        state.session.tracks.push_back({"wave", {{loaded, 0, 0, loaded->length()}}});
        state.session.tracks.push_back({"spectrum", {{loaded, 0, 0, loaded->length()}}});
        state.session.tracks[0].view = carta::TrackView::waveform;
        state.session.tracks[1].view = carta::TrackView::spectrogram;
        auto original = state;
        carta::EditHistory viewHistory;
        viewHistory.record(state);
        state.session.tracks[0].view = carta::TrackView::combined;
        check(viewHistory.undo(state) &&
                  state.session.tracks[0].view == carta::TrackView::waveform &&
                  state.session.tracks[1].view == carta::TrackView::spectrogram &&
                  viewHistory.redo(state) &&
                  state.session.tracks[0].view == carta::TrackView::combined,
              "track view changes are undoable without changing other tracks");
        juce::AudioBuffer<float> beforeView(2, 128), afterView(2, 128);
        beforeView.clear();
        afterView.clear();
        original.session.render(beforeView, 0, 128, 0, carta::sessionRate, 2);
        state.session.render(afterView, 0, 128, 0, carta::sessionRate, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 128; ++i)
                check(near(beforeView.getSample(ch, i), afterView.getSample(ch, i)),
                      "visualization choices do not alter rendered audio");
    }
    {
        const double rate = carta::sessionRate;
        std::vector<carta::Marker> existing{{100 / rate, "Old"},
                                            {200 / rate, "Removed"},
                                            {300 / rate, "Ambiguous A"},
                                            {300 / rate, "Ambiguous B"},
                                            {400.25 / rate, "Off grid"}};
        std::vector<carta::Marker> imported{
            {300 / rate, "New ambiguous"}, {100 / rate, "Renamed"}, {400 / rate, "New"}};
        const auto retainedId = existing[0].id, ambiguousId = imported[0].id;
        const auto reconciled = carta::reconcileImportedMarkers(existing, imported);
        check(reconciled.retained == 1 && imported[1].id == retainedId,
              "marker import retains identity only at unique exact sample boundaries");
        check(imported[0].id == ambiguousId && imported[2].id != existing[4].id,
              "ambiguous and off-grid existing markers are not guessed onto imported boundaries");
        const auto longSample = static_cast<juce::int64>(10) * 60 * 60 * 44100 + 123;
        std::vector<carta::Marker> oldLong{{static_cast<double>(longSample) / rate, "Long"}},
            newLong{{static_cast<double>(longSample) / rate, "Renamed"}};
        check(carta::reconcileImportedMarkers(oldLong, newLong).retained == 1 &&
                  newLong[0].id == oldLong[0].id,
              "marker import matches canonical sample positions beyond ten hours");
    }
    {
        auto markerSession = saved;
        markerSession.markers = {{88.0 / carta::sessionRate, "Second"},
                                 {0, "First"},
                                 {44.0 / carta::sessionRate, "Middle"}};
        {
            auto exchange = markerSession;
            exchange.markers = {
                {88.0 / carta::sessionRate, juce::String::fromUTF8(u8"Blue / \"青\"")},
                {0, "First"},
                {44.0 / carta::sessionRate, "First"}};
            const auto file = dir.getChildFile("markers.json");
            check(carta::writeMarkerFile(exchange, file, error), "write marker exchange file");
            juce::var data;
            check(juce::JSON::parse(file.loadFileAsString(), data).wasOk() &&
                      data["format"].toString() == "carta-markers" &&
                      static_cast<int>(data["sampleRate"]) == 44100,
                  "marker exchange identifies schema and rate");
            std::vector<carta::Marker> decoded{{1, "sentinel"}};
            check(carta::readMarkerFile(exchange, file, decoded, error) && decoded.size() == 3 &&
                      decoded[0].name == "First" && decoded[1].name == "First" &&
                      decoded[2].name == exchange.markers[0].name,
                  "marker exchange round trips sorted Unicode names and duplicate titles");
            check(std::llround(decoded[0].position * carta::sessionRate) == 0 &&
                      std::llround(decoded[1].position * carta::sessionRate) == 44 &&
                      std::llround(decoded[2].position * carta::sessionRate) == 88,
                  "marker exchange preserves integer samples");
            const auto original = file.loadFileAsString();
            {
                auto retained = decoded;
                int calls = 0;
                check(!carta::readMarkerFile(exchange, file, retained, error,
                                             [&] { return ++calls < 2; }) &&
                          error == "Marker exchange cancelled." &&
                          retained.size() == decoded.size() && retained[0].id == decoded[0].id,
                      "cancelled JSON import preserves its output");
                calls = 0;
                const int afterTemporary = static_cast<int>(exchange.markers.size()) + 3;
                check(!carta::writeMarkerFile(exchange, file, error, {}, false,
                                              [&] { return ++calls < afterTemporary; }) &&
                          calls == afterTemporary && error == "Marker exchange cancelled." &&
                          file.loadFileAsString() == original,
                      "cancelling JSON export immediately before commit preserves the destination");
            }
            {
                auto labelsSession = exchange;
                labelsSession.markers.clear();
                for (int n = 0; n < 128; ++n)
                    labelsSession.markers.push_back(
                        {static_cast<double>(n) / carta::sessionRate,
                         "Track " + juce::String(n) + juce::String::fromUTF8(u8" 青")});
                const auto labels = dir.getChildFile("audacity-labels.txt");
                std::vector<carta::Marker> imported;
                check(
                    carta::writeAudacityLabels(labelsSession, labels, error) &&
                        carta::readAudacityLabels(labelsSession, labels, imported, error) &&
                        imported.size() == 128,
                    "Audacity labels round trip Unicode names and every sample in a short master");
                if (imported.size() == 128)
                    for (int n = 0; n < 128; ++n)
                        check(std::llround(imported[static_cast<size_t>(n)].position *
                                           carta::sessionRate) == n &&
                                  imported[static_cast<size_t>(n)].name ==
                                      labelsSession.markers[static_cast<size_t>(n)].name,
                              "label decimals round trip to the original sample and name");
                const auto valid = labels.loadFileAsString();
#if JUCE_MAC || JUCE_LINUX
                for (bool useLabels : {false, true})
                    for (bool replace : {false, true}) {
                        const auto changing = dir.getChildFile(
                            juce::String("changing-marker-input-") +
                            (useLabels ? "labels" : "json") + (replace ? "-replace" : "-modify"));
                        check((useLabels ? labels : file).copyFileTo(changing),
                              "copy marker input race fixture");
                        juce::MemoryBlock contents;
                        check(changing.loadFileAsData(contents) && contents.getSize() > 4,
                              "read marker input race fixture");
                        auto* markerBytes = static_cast<unsigned char*>(contents.getData());
                        markerBytes[0] ^= 1;
                        const auto savedTime = changing.getLastModificationTime();
                        const auto savedSize = changing.getSize();
                        int calls = 0;
                        bool changed = false, metadataPreserved = false;
                        auto retained = decoded;
                        const auto keepGoing = [&] {
                            if (++calls == 2) {
                                if (replace)
                                    changed = changing.replaceWithData(contents.getData(),
                                                                       contents.getSize());
                                else {
                                    auto output = changing.createOutputStream();
                                    if (output && output->setPosition(0) &&
                                        output->writeByte(static_cast<char>(markerBytes[0]))) {
                                        output->flush();
                                        changed = output->getStatus().wasOk();
                                    }
                                }
                                metadataPreserved = changed &&
                                                    changing.setLastModificationTime(savedTime) &&
                                                    changing.getSize() == savedSize &&
                                                    changing.getLastModificationTime() == savedTime;
                            }
                            return true;
                        };
                        const bool markerImportOk =
                            useLabels ? carta::readAudacityLabels(exchange, changing, retained,
                                                                  error, keepGoing)
                                      : carta::readMarkerFile(exchange, changing, retained, error,
                                                              keepGoing);
                        check(changed && metadataPreserved && !markerImportOk &&
                                  error.contains("changed while it was being read") &&
                                  retained.size() == decoded.size() &&
                                  retained[0].id == decoded[0].id,
                              "marker input rejects same-metadata modification or replacement and "
                              "leaves output unchanged");
                    }
#endif
                {
                    auto retained = decoded;
                    int calls = 0;
                    check(!carta::readAudacityLabels(exchange, labels, retained, error,
                                                     [&] { return ++calls < 2; }) &&
                              error == "Marker exchange cancelled." &&
                              retained.size() == decoded.size() && retained[0].id == decoded[0].id,
                          "cancelled label import preserves its output");
                    calls = 0;
                    const int afterTemporary = static_cast<int>(labelsSession.markers.size()) + 3;
                    check(!carta::writeAudacityLabels(labelsSession, labels, error, {}, false,
                                                      [&] { return ++calls < afterTemporary; }) &&
                              calls == afterTemporary && error == "Marker exchange cancelled." &&
                              labels.loadFileAsString() == valid,
                          "cancelling label export immediately before commit preserves the "
                          "destination");
                }
                check(!carta::writeAudacityLabels(labelsSession, labels, error, {}, true) &&
                          labels.loadFileAsString() == valid,
                      "exclusive Audacity label export preserves an existing destination");
                for (const auto* bad :
                     {"0.001\t0.002\tregion\n", "0\t0\tpoint\n\\\t100\t200\n", "0\t0\tbad\tname\n",
                      "1e99\t1e99\tbad\n", "0.0000000001\t0.0000000001\tbad\n",
                      "0\t0\tone\n0.000001\t0.000001\ttwo\n"}) {
                    const auto invalid = dir.getChildFile("bad-audacity-labels.txt");
                    check(invalid.replaceWithText(bad), "write invalid Audacity label fixture");
                    auto retained = decoded;
                    check(!carta::readAudacityLabels(exchange, invalid, retained, error) &&
                              retained.size() == decoded.size() &&
                              retained[0].name == decoded[0].name,
                          "invalid Audacity labels do not replace marker output");
                }
                auto invalidName = exchange;
                invalidName.markers[0].name = "tab\tinside";
                check(!carta::writeAudacityLabels(invalidName, labels, error) &&
                          labels.loadFileAsString() == valid,
                      "unsupported label name preserves destination");
                auto emptyLabels = exchange;
                emptyLabels.markers.clear();
                check(carta::writeAudacityLabels(emptyLabels, labels, error) &&
                          carta::readAudacityLabels(exchange, labels, imported, error) &&
                          imported.empty(),
                      "empty Audacity label file round trips");
                const auto unnamed = dir.getChildFile("unnamed-labels.txt");
                check(unnamed.replaceWithText("0\t0\r\n0.001\t0.001\t\r\n0.002\t0.002\t  \r\n") &&
                          carta::readAudacityLabels(exchange, unnamed, imported, error) &&
                          imported.size() == 3 && imported[0].name.isEmpty() &&
                          imported[1].name.isEmpty() && imported[2].name == "  ",
                      "optional, empty and whitespace Audacity label names import without "
                      "alteration");
                auto unnamedSession = exchange;
                unnamedSession.markers = imported;
                check(carta::writeAudacityLabels(unnamedSession, unnamed, error) &&
                          carta::readAudacityLabels(exchange, unnamed, imported, error) &&
                          imported.size() == 3 && imported[0].name.isEmpty() &&
                          imported[2].name == "  ",
                      "unnamed point labels export and reimport without renaming");
                auto longAudio = std::make_shared<carta::Audio>();
                longAudio->sampleRate = 1;
                longAudio->samples.setSize(1, 4801);
                longAudio->samples.clear();
                carta::Session longMaster;
                longMaster.tracks.push_back({"long", {{longAudio, 0, 0, 4800}}});
                for (juce::int64 n = 0; n < 6000; ++n)
                    longMaster.markers.push_back(
                        {static_cast<double>(n * 35000 + (n * 31) % 32000) / carta::sessionRate,
                         "M" + juce::String(n)});
                const auto longLabels = dir.getChildFile("long-audacity-labels.txt");
                check(carta::writeAudacityLabels(longMaster, longLabels, error) &&
                          carta::readAudacityLabels(longMaster, longLabels, imported, error) &&
                          imported.size() == longMaster.markers.size(),
                      "round trip thousands of point labels over an 80-minute master");
                if (imported.size() == longMaster.markers.size())
                    for (size_t n = 0; n < imported.size(); ++n)
                        check(
                            std::llround(imported[n].position * carta::sessionRate) ==
                                    std::llround(longMaster.markers[n].position *
                                                 carta::sessionRate) &&
                                imported[n].name == longMaster.markers[n].name,
                            "long-master label exchange preserves exact project samples and order");
            }
            for (
                const auto* bad :
                {R"({"format":"wrong","version":1,"sampleRate":44100,"markers":[]})",
                 R"({"format":"carta-markers","version":2,"sampleRate":44100,"markers":[]})",
                 R"({"format":"carta-markers","version":1,"sampleRate":48000,"markers":[]})",
                 R"({"format":"carta-markers","version":1,"sampleRate":44100,"markers":[{"sample":1.5,"name":"x"}]})",
                 R"({"format":"carta-markers","version":1,"sampleRate":44100,"markers":[{"sample":-1,"name":"x"}]})",
                 R"({"format":"carta-markers","version":1,"sampleRate":44100,"markers":[{"sample":18446744073709551616,"name":"x"}]})",
                 R"({"format":"carta-markers","version":18446744073709551617,"sampleRate":44100,"markers":[]})",
                 R"({"format":"carta-markers","version":1,"sampleRate":44100,"markers":[{"sample":1e99999999999999999999,"name":"x"}]})",
                 R"({"format":"carta-markers","version":1,"sampleRate":44100,"markers":[{"sample":-'999999999999999999999999',"name":"x"}]})",
                 R"({"format":"carta-markers","version":1,"sampleRate":44100,"markers":[{"sample":128,"name":"x"}]})",
                 R"({"format":"carta-markers","version":1,"sampleRate":44100,"markers":[{"sample":1,"name":" "}]})",
                 R"({"format":"carta-markers","version":1,"sampleRate":44100,"markers":[{"sample":4,"name":"x"},{"sample":4,"name":"y"}]})"}) {
                const auto invalid = dir.getChildFile("invalid-markers.json");
                check(invalid.replaceWithText(bad), "write invalid marker fixture");
                auto retained = decoded;
                check(!carta::readMarkerFile(exchange, invalid, retained, error) &&
                          retained.size() == 3 && retained[2].name == decoded[2].name,
                      "invalid marker file leaves existing markers unchanged");
            }
            {
                const auto longName = dir.getChildFile("long-numeric-marker-name.json");
                const auto text =
                    juce::String(
                        R"({"format":"carta-markers","version":1,"sampleRate":44100,"markers":[{"sample":0,"name":"escaped \\\" )") +
                    juce::String::repeatedString("9", 150) + R"("}]})";
                check(longName.replaceWithText(text), "write numeric text in marker name");
                auto retained = decoded;
                check(carta::readMarkerFile(exchange, longName, retained, error) &&
                          retained.size() == 1,
                      "numeric text inside an escaped string is not parsed as a number");
            }
            {
                const auto badUtf8 = dir.getChildFile("invalid-utf8.json");
                const unsigned char byte = 0xff;
                check(badUtf8.replaceWithData(&byte, 1), "write invalid UTF-8 marker fixture");
                auto retained = decoded;
                check(!carta::readMarkerFile(exchange, badUtf8, retained, error) &&
                          retained.size() == 3,
                      "invalid UTF-8 leaves markers untouched");
                const auto oversized = dir.getChildFile("oversized-markers.json");
                auto stream = oversized.createOutputStream();
                check(stream && stream->setPosition(8LL * 1024 * 1024) && stream->writeByte(0),
                      "write oversized marker fixture");
                stream.reset();
                check(!carta::readMarkerFile(exchange, oversized, retained, error) &&
                          retained.size() == 3,
                      "oversized marker input is rejected");
                check(!carta::writeMarkerFile(exchange, file, error, file) &&
                          file.loadFileAsString() == original,
                      "marker export cannot replace the open project");
            }
            auto duplicate = exchange;
            duplicate.markers.push_back({0, "collision"});
            check(!carta::writeMarkerFile(duplicate, file, error) &&
                      file.loadFileAsString() == original,
                  "invalid marker export preserves destination");
            auto offGrid = exchange;
            offGrid.markers[0].position = 88.25 / carta::sessionRate;
            check(!carta::writeMarkerFile(offGrid, file, error) && error.contains("off-grid") &&
                      file.loadFileAsString() == original,
                  "marker export rejects fractional positions without replacing the destination");
            check(carta::snapMarkersToSamples(offGrid, error) &&
                      carta::writeMarkerFile(offGrid, file, error) &&
                      carta::readMarkerFile(offGrid, file, decoded, error) &&
                      std::llround(decoded.back().position * carta::sessionRate) == 88,
                  "explicit marker repair enables exact marker exchange");
            check(!carta::writeMarkerFile(exchange, wavfile, error),
                  "marker export cannot replace source audio");
            {
                const auto previous = file.loadFileAsString();
                check(!carta::writeMarkerFile(exchange, file, error, {}, true) &&
                          file.loadFileAsString() == previous,
                      "exclusive marker export preserves an existing destination");
            }
            auto empty = exchange;
            empty.markers.clear();
            check(carta::writeMarkerFile(empty, file, error), "export empty explicit marker list");
            check(carta::readMarkerFile(exchange, file, decoded, error) && decoded.empty(),
                  "empty marker file imports as empty list");
        }
        const auto manifest = dir.getChildFile("tracks.json");
        check(carta::exportTrackManifest(markerSession, 2, manifest, error),
              "export sorted track manifest");
        juce::var data;
        check(juce::JSON::parse(manifest.loadFileAsString(), data).wasOk(), "parse track manifest");
        const auto* manifestTracks = data["tracks"].getArray();
        check(manifestTracks && manifestTracks->size() == 3 &&
                  static_cast<juce::int64>(data["totalSamples"]) == 128,
              "manifest describes full master");
        check((*manifestTracks)[0]["name"].toString() == "First" &&
                  static_cast<juce::int64>((*manifestTracks)[0]["endSample"]) == 44 &&
                  static_cast<juce::int64>((*manifestTracks)[1]["startSample"]) == 44 &&
                  static_cast<juce::int64>((*manifestTracks)[1]["endSample"]) == 88 &&
                  static_cast<juce::int64>((*manifestTracks)[2]["endSample"]) == 128,
              "marker manifestTracks are ordered and contiguous");
        std::vector<carta::MasterTrack> masterTracks;
        check(carta::getMasterTracks(markerSession, masterTracks, error) &&
                  masterTracks.size() == 3 && masterTracks[1].range.start == 44 &&
                  masterTracks[1].range.end == 88,
              "audio and manifest share track boundaries");
        {
            auto marked = markerSession;
            marked.markers[0].name = juce::String::fromUTF8(u8"青い終わり");
            for (const auto encoding : {carta::WavEncoding::pcm16, carta::WavEncoding::pcm24,
                                        carta::WavEncoding::float32}) {
                const carta::WavOptions cueOptions{encoding, false, true};
                bool clipped = false;
                const auto path =
                    dir.getChildFile("cues-" + juce::String(static_cast<int>(encoding)) + ".wav");
                const auto referencePath = dir.getChildFile(
                    "no-cues-" + juce::String(static_cast<int>(encoding)) + ".wav");
                check(carta::exportWavRange(marked, 2, {0, 128}, path, clipped, error, {}, {},
                                            cueOptions) &&
                          carta::exportWavRange(marked, 2, {0, 128}, referencePath, clipped, error,
                                                {}, {}, {encoding, false, false}),
                      "export WAV with and without embedded markers");
                auto reader = formats.createReaderFor(path);
                check(reader && reader->metadataValues["NumCuePoints"] == "3" &&
                          reader->metadataValues["NumCueLabels"] == "3" &&
                          reader->metadataValues["Cue0Offset"] == "0" &&
                          reader->metadataValues["Cue1Offset"] == "44" &&
                          reader->metadataValues["Cue2Offset"] == "88" &&
                          reader->metadataValues["CueLabel2Text"] == marked.markers[0].name,
                      "WAV cue and UTF-8 label metadata preserve chronological marker boundaries");
                const auto withCues = carta::loadAudio(formats, path, error),
                           withoutCues = carta::loadAudio(formats, referencePath, error);
                check(withCues && withoutCues &&
                          withCues->numSamples() == withoutCues->numSamples(),
                      "cue chunks do not change audio length");
                check(withCues && withCues->sourceCueError.isEmpty() &&
                          withCues->sourceCues.size() == 3 &&
                          withCues->sourceCues[1].sample == 44 &&
                          withCues->sourceCues[2].name == marked.markers[0].name,
                      "imported audio snapshots capture WAV cue positions and labels");
                if (withCues && withCues->sourceCues.size() == 3) {
                    carta::EditorState cueState;
                    cueState.session.tracks.push_back(
                        {"trimmed",
                         {{withCues, 200 / carta::sessionRate, 44 / carta::sessionRate,
                           56 / carta::sessionRate}}});
                    const auto before = cueState;
                    carta::EditHistory cueHistory;
                    cueHistory.record(before, "Add WAV cue markers");
                    check(carta::addMarkersFromClipCues(cueState.session, {0, 0}, error) &&
                              cueState.session.markers.size() == 2 &&
                              std::llround(cueState.session.markers[0].position *
                                           carta::sessionRate) == 200 &&
                              std::llround(cueState.session.markers[1].position *
                                           carta::sessionRate) == 244 &&
                              cueState.session.markers[0].name == "Middle" &&
                              cueState.session.markers[1].name == marked.markers[0].name,
                          "WAV cues inside clip trim map to the moved timeline and preserve "
                          "Unicode labels");
                    check(cueHistory.undo(cueState) && cueState.session.markers.empty() &&
                              cueHistory.redo(cueState) && cueState.session.markers.size() == 2,
                          "cue marker import supports undo and redo");
                    check(!carta::addMarkersFromClipCues(cueState.session, {0, 0}, error) &&
                              error.contains("collide") && cueState.session.markers.size() == 2,
                          "duplicate mapped cues leave existing markers untouched");
                }
                if (reader) {
                    auto malformed = reader->metadataValues;
                    malformed.set("Cue1Identifier", malformed["Cue0Identifier"]);
                    std::vector<carta::SourceCue> retained{{12, "sentinel"}};
                    check(!carta::parseWavSourceCues(malformed, 128, retained, error) &&
                              retained.size() == 1 && retained[0].name == "sentinel",
                          "duplicate WAV cue identifiers are rejected atomically");
                    malformed = reader->metadataValues;
                    malformed.set("Cue1Offset", "999999999999999999999999");
                    check(!carta::parseWavSourceCues(malformed, 128, retained, error) &&
                              retained.size() == 1,
                          "oversized WAV cue offsets are rejected");
                    malformed = reader->metadataValues;
                    malformed.set("NumCueRegions", "1");
                    check(!carta::parseWavSourceCues(malformed, 128, retained, error) &&
                              retained.size() == 1,
                          "unsupported WAV cue regions cannot silently lose metadata");
                }
                if (withCues && withoutCues && withCues->numSamples() == withoutCues->numSamples())
                    for (int ch = 0; ch < 2; ++ch)
                        for (int n = 0; n < withCues->numSamples(); ++n)
                            check(near(withCues->samples.getSample(ch, n),
                                       withoutCues->samples.getSample(ch, n)),
                                  "embedding cues does not change audio samples");
                juce::MemoryBlock raw;
                check(path.loadFileAsData(raw), "read raw WAV cue chunks");
                const auto* bytes = static_cast<const char*>(raw.getData());
                const std::string wavBytes(bytes, raw.getSize());
                check(wavBytes.find("cue ") != std::string::npos &&
                          wavBytes.find("adtl") != std::string::npos &&
                          wavBytes.find("labl") != std::string::npos,
                      "WAV physically contains standard cue and associated label chunks");
                const auto cueAt = wavBytes.find("cue ");
                if (cueAt != std::string::npos && cueAt + 84 <= wavBytes.size()) {
                    const auto little = [&](size_t pos) {
                        return static_cast<std::uint32_t>(
                                   static_cast<unsigned char>(wavBytes[pos])) |
                               (static_cast<std::uint32_t>(
                                    static_cast<unsigned char>(wavBytes[pos + 1]))
                                << 8) |
                               (static_cast<std::uint32_t>(
                                    static_cast<unsigned char>(wavBytes[pos + 2]))
                                << 16) |
                               (static_cast<std::uint32_t>(
                                    static_cast<unsigned char>(wavBytes[pos + 3]))
                                << 24);
                    };
                    check(
                        little(cueAt + 8) == 3 && little(cueAt + 12) == 1 &&
                            little(cueAt + 32) == 0 && little(cueAt + 56) == 44 &&
                            little(cueAt + 80) == 88,
                        "raw RIFF cue entries contain three identifiers and exact sample offsets");
                } else
                    check(false, "raw WAV cue chunk has complete entries");
            }
            bool clipped = false;
            const auto selectedPath = dir.getChildFile("relative-cues.wav");
            check(carta::exportWavRange(marked, 2, {44, 88}, selectedPath, clipped, error, {}, {},
                                        {carta::WavEncoding::pcm24, false, true}),
                  "export range with embedded markers");
            auto relative = formats.createReaderFor(selectedPath);
            check(relative && relative->metadataValues["NumCuePoints"] == "1" &&
                      relative->metadataValues["Cue0Offset"] == "0" &&
                      relative->metadataValues["CueLabel0Text"] == "Middle",
                  "range cues are relative to the file and exclude the end boundary");
            auto invalid = marked;
            invalid.markers.push_back({44 / carta::sessionRate, "duplicate"});
            const auto protectedFile = dir.getChildFile("cue-protected.wav");
            check(protectedFile.replaceWithText("preserve") &&
                      !carta::exportWavRange(invalid, 2, {0, 128}, protectedFile, clipped, error,
                                             {}, {}, {carta::WavEncoding::pcm24, false, true}) &&
                      protectedFile.loadFileAsString() == "preserve",
                  "duplicate cue positions reject export without replacing destination");
            invalid = marked;
            invalid.markers[1].name = "";
            check(!carta::exportWavRange(invalid, 2, {0, 128}, protectedFile, clipped, error, {},
                                         {}, {carta::WavEncoding::pcm24, false, true}) &&
                      protectedFile.loadFileAsString() == "preserve",
                  "unnamed in-range cue rejects export without replacing destination");
            check(carta::exportWavRange(invalid, 2, {44, 88}, selectedPath, clipped, error, {}, {},
                                        {carta::WavEncoding::pcm24, false, true}),
                  "invalid label outside the selected range does not block range cues");
            check(!carta::exportWavRange(marked, 2, {0, 128}, protectedFile, clipped, error, {},
                                         [](double) { return false; },
                                         {carta::WavEncoding::pcm24, false, true}) &&
                      protectedFile.loadFileAsString() == "preserve",
                  "cancelled cue export preserves the destination");
            auto largeLabel = marked;
            largeLabel.markers[1].name = juce::String::repeatedString("x", 6000);
            const auto nearRiffLimit =
                (static_cast<juce::int64>(std::numeric_limits<std::uint32_t>::max()) - 4096) / 6 -
                500;
            check(carta::validateWavRange(largeLabel, 2, {0, nearRiffLimit}, error,
                                          {carta::WavEncoding::pcm24, false, false}) &&
                      !carta::validateWavRange(largeLabel, 2, {0, nearRiffLimit}, error,
                                               {carta::WavEncoding::pcm24, false, true}),
                  "WAV size validation reserves space for cue metadata before writing");
        }
        carta::MasterTrack current;
        int trackIndex = -1;
        check(carta::masterTrackAtSample(markerSession, 43, current, trackIndex, error) &&
                  trackIndex == 0 && current.name == "First",
              "sample before boundary selects previous track");
        check(carta::masterTrackAtSample(markerSession, 44, current, trackIndex, error) &&
                  trackIndex == 1 && current.name == "Middle",
              "boundary sample selects next track");
        check(carta::masterTrackAtSample(markerSession, 127, current, trackIndex, error) &&
                  trackIndex == 2 &&
                  !carta::masterTrackAtSample(markerSession, 128, current, trackIndex, error),
              "master end is exclusive");
        {
            auto scopedSession = markerSession;
            scopedSession.tracks.push_back({"Second", {}});
            scopedSession.tracks.push_back({"Third", {}});
            carta::TimeSelection selection{2, 3, 0, 0};
            check(carta::selectMasterTrackAtSample(scopedSession, 43, selection, error) &&
                      selection.start == 0 && selection.end == 44 && selection.firstTrack == 0 &&
                      selection.lastTrack == 2,
                  "master track selection covers all rows up to the exclusive boundary");
            check(carta::selectMasterTrackAtSample(scopedSession, 44, selection, error, {1, 2}) &&
                      selection.start == 44 && selection.end == 88 && selection.firstTrack == 1 &&
                      selection.lastTrack == 2,
                  "boundary selects next master track on scoped rows");
            check(!carta::selectMasterTrackAtSample(scopedSession, 128, selection, error) &&
                      selection.start == 44 && selection.end == 88,
                  "master-end selection fails without changing the selection");
            check(!carta::selectMasterTrackAtSample(scopedSession, 44, selection, error, {2, 3}) &&
                      selection.firstTrack == 1,
                  "invalid row scope fails without changing the selection");
            scopedSession.markers.push_back(scopedSession.markers.front());
            check(!carta::selectMasterTrackAtSample(scopedSession, 44, selection, error) &&
                      error.isNotEmpty() && selection.start == 44,
                  "invalid marker boundaries leave selection unchanged");
            scopedSession.markers.clear();
            check(carta::selectMasterTrackAtSample(scopedSession, 0, selection, error) &&
                      selection.start == 0 && selection.end == 128,
                  "no markers selects the entire implicit master track");
        }
        bool trackClipped = false;
        const auto fullPath = dir.getChildFile("marker-master.wav");
        check(carta::exportWav(markerSession, 2, fullPath, trackClipped, error),
              "export marker master");
        const auto full = carta::loadAudio(formats, fullPath, error);
        check(full && full->samples.getNumSamples() == 128, "read marker master");
        int masterPos = 0;
        for (size_t i = 0; i < masterTracks.size(); ++i) {
            const auto path =
                dir.getChildFile("marker-part-" + juce::String(static_cast<int>(i)) + ".wav");
            check(carta::exportWavRange(markerSession, 2, masterTracks[i].range, path, trackClipped,
                                        error),
                  "export marker region");
            const auto part = carta::loadAudio(formats, path, error);
            check(part != nullptr, "read marker region");
            for (int sample = 0; sample < part->samples.getNumSamples(); ++sample, ++masterPos)
                for (int channel = 0; channel < 2; ++channel) {
                    const auto x = part->samples.getSample(channel, sample),
                               y = full->samples.getSample(channel, masterPos);
                    check(std::memcmp(&x, &y, sizeof(float)) == 0,
                          "marker regions reconstruct master bit-for-bit");
                }
        }
        check(masterPos == 128, "marker regions contain every master sample once");
        const auto batchParent = dir.getChildFile("batch-parent");
        check(batchParent.createDirectory().wasOk(), "create batch export parent");
        carta::TrackBatchResult batch;
        double previousProgress = -1;
        bool progressMonotonic = true;
        check(carta::exportTrackWavs(markerSession, 2, batchParent, batch, error, {},
                                     [&](double value) {
                                         if (value < previousProgress)
                                             progressMonotonic = false;
                                         previousProgress = value;
                                         return true;
                                     },
                                     {carta::WavEncoding::pcm24, false}) &&
                  batch.completed == 3 && progressMonotonic &&
                  std::abs(previousProgress - 1) < 1e-12,
              "export all marker tracks and report progress");
        check(batch.directory.isDirectory() &&
                  batch.directory.getChildFile("Carta tracks.json").existsAsFile(),
              "batch creates a folder and manifest");
        check(batch.directory.findChildFiles(juce::File::findFiles, false, "*").size() == 4,
              "successful batch leaves no temporary files");
        int batchPosition = 0;
        for (int n = 1; n <= 3; ++n) {
            const auto part =
                carta::loadAudio(formats,
                                 batch.directory.getChildFile(
                                     "Track " + juce::String(n).paddedLeft('0', 2) + ".wav"),
                                 error);
            check(part != nullptr, "read batch track WAV");
            for (int i = 0; i < part->numSamples(); ++i, ++batchPosition)
                for (int ch = 0; ch < 2; ++ch) {
                    const auto x = part->samples.getSample(ch, i),
                               y = full->samples.getSample(ch, batchPosition);
                    check(std::memcmp(&x, &y, sizeof(float)) == 0,
                          "batch track WAVs concatenate to the master");
                }
        }
        check(batchPosition == 128, "batch track output contains every master sample once");
        juce::var batchManifest;
        check(
            juce::JSON::parse(batch.directory.getChildFile("Carta tracks.json").loadFileAsString(),
                              batchManifest)
                    .wasOk() &&
                batchManifest["tracks"].isArray() &&
                batchManifest["tracks"].getArray()->size() == 3 &&
                (*batchManifest["tracks"].getArray())[1]["name"].toString() == masterTracks[1].name,
            "batch manifest retains ordered marker names and boundaries");
        const auto firstDirectory = batch.directory;
        check(carta::exportTrackWavs(markerSession, 2, batchParent, batch, error) &&
                  batch.directory != firstDirectory &&
                  firstDirectory.getChildFile("Track 01.wav").existsAsFile(),
              "batch never reuses an existing output folder");
        const auto ditherMaster = dir.getChildFile("batch-dither-master.wav");
        check(carta::exportWavRange(markerSession, 2, {0, 128}, ditherMaster, trackClipped, error,
                                    {}, {}, {carta::WavEncoding::pcm16, true}) &&
                  carta::exportTrackWavs(markerSession, 2, batchParent, batch, error, {}, {},
                                         {carta::WavEncoding::pcm16, true}),
              "export dithered master and batch");
        const auto ditherFull = carta::loadAudio(formats, ditherMaster, error);
        check(ditherFull != nullptr, "read dithered master");
        int ditherPosition = 0;
        for (int n = 1; n <= 3; ++n) {
            const auto part =
                carta::loadAudio(formats,
                                 batch.directory.getChildFile(
                                     "Track " + juce::String(n).paddedLeft('0', 2) + ".wav"),
                                 error);
            check(part != nullptr, "read dithered batch track");
            for (int i = 0; i < part->numSamples(); ++i, ++ditherPosition)
                for (int ch = 0; ch < 2; ++ch) {
                    const auto x = part->samples.getSample(ch, i),
                               y = ditherFull->samples.getSample(ch, ditherPosition);
                    check(std::memcmp(&x, &y, sizeof(float)) == 0,
                          "dithered batch tracks concatenate exactly to the dithered master");
                }
        }
        check(ditherPosition == 128, "dithered batch has the expected length");
        carta::TrackBatchResult cancelled;
        check(!carta::exportTrackWavs(markerSession, 2, batchParent, cancelled, error, {},
                                      [](double value) { return value < 0.35; }) &&
                  cancelled.completed == 1 &&
                  cancelled.directory.getChildFile("Track 01.wav").existsAsFile() &&
                  !cancelled.directory.getChildFile("Track 02.wav").existsAsFile(),
              "cancelled batch keeps completed files and leaves no partial next track");
        carta::TrackBatchResult raced;
        bool injected = false;
        check(!carta::exportTrackWavs(markerSession, 2, batchParent, raced, error, {},
                                      [&](double value) {
                                          if (!injected && value > 0.34 &&
                                              raced.directory != juce::File{}) {
                                              injected = true;
                                              check(raced.directory.getChildFile("Track 01.wav")
                                                        .replaceWithText("external"),
                                                    "create competing track destination");
                                          }
                                          return true;
                                      }) &&
                  injected && raced.completed == 0 &&
                  raced.directory.getChildFile("Track 01.wav").loadFileAsString() == "external",
              "batch atomically refuses a track created during writing");
        carta::TrackBatchResult manifestRace;
        injected = false;
        check(!carta::exportTrackWavs(markerSession, 2, batchParent, manifestRace, error, {},
                                      [&](double) {
                                          if (!injected && manifestRace.completed == 3) {
                                              injected = true;
                                              check(manifestRace.directory
                                                        .getChildFile("Carta tracks.json")
                                                        .replaceWithText("external"),
                                                    "create competing manifest");
                                          }
                                          return true;
                                      }) &&
                  injected && manifestRace.completed == 3 &&
                  manifestRace.directory.getChildFile("Carta tracks.json").loadFileAsString() ==
                      "external",
              "batch preserves a manifest created during export");
        const auto original = manifest.loadFileAsString();
        {
            auto legacy = markerSession;
            legacy.markers[1].position = 0.25 / carta::sessionRate;
            carta::TrackBatchResult rejected;
            carta::MasterTrack unchanged{"Sentinel", {5, 6}};
            int index = 17;
            carta::TimeSelection selection{1, 2, 0, 0};
            check(!carta::getMasterTracks(legacy, masterTracks, error) &&
                      error.contains("off-grid") && masterTracks.size() == 3 &&
                      masterTracks[1].range.start == 44,
                  "off-grid boundaries leave prior track list unchanged");
            check(!carta::masterTrackAtSample(legacy, 10, unchanged, index, error) &&
                      unchanged.name == "Sentinel" && index == 17,
                  "off-grid boundaries reject master-track lookup without modifying outputs");
            check(!carta::selectMasterTrackAtSample(legacy, 10, selection, error) &&
                      selection.start == 1 && selection.end == 2,
                  "off-grid boundaries reject master-track selection without modifying it");
            check(!carta::exportTrackManifest(legacy, 2, manifest, error) &&
                      manifest.loadFileAsString() == original,
                  "off-grid boundaries preserve existing manifest");
            check(!carta::exportTrackWavs(legacy, 2, batchParent, rejected, error) &&
                      rejected.directory == juce::File{},
                  "off-grid boundaries reject batch export before creating a folder");
            check(carta::snapMarkersToSamples(legacy, error) &&
                      carta::getMasterTracks(legacy, masterTracks, error) &&
                      masterTracks[0].range.start == 0 && masterTracks.back().range.end == 128,
                  "explicitly snapped boundaries permit master tracks");
        }
        markerSession.markers.push_back({44.0 / carta::sessionRate, "Duplicate"});
        carta::TrackBatchResult rejectedBatch;
        check(!carta::exportTrackWavs(markerSession, 2, batchParent, rejectedBatch, error) &&
                  rejectedBatch.directory == juce::File{},
              "invalid markers fail before creating a batch folder");
        check(!carta::exportTrackManifest(markerSession, 2, manifest, error) &&
                  manifest.loadFileAsString() == original,
              "duplicate sample boundary preserves destination");
        check(!carta::getMasterTracks(markerSession, masterTracks, error) &&
                  masterTracks.size() == 3 && masterTracks[1].range.start == 44,
              "invalid markers preserve track list");
        markerSession.markers = {{128.0 / carta::sessionRate, "End"}};
        check(!carta::exportTrackManifest(markerSession, 2, manifest, error),
              "end boundary rejected");
        markerSession.markers = {{44.0 / carta::sessionRate, "Middle"}};
        check(carta::exportTrackManifest(markerSession, 2, manifest, error),
              "implicit first boundary");
        check(juce::JSON::parse(manifest.loadFileAsString(), data).wasOk() &&
                  data["tracks"].getArray()->size() == 2 &&
                  static_cast<juce::int64>((*data["tracks"].getArray())[0]["startSample"]) == 0,
              "manifest begins at zero");
        check(!carta::exportTrackManifest(markerSession, 2, wavfile, error),
              "manifest cannot replace source audio");
    }
    check(carta::setClipEnvelope(saved.tracks[0].clips[0], 0.75f, 0.001, 0.001),
          "prepare persisted clip envelope");
    saved.tracks[0].clips[0].name = juce::String::fromUTF8("Intro 日本語");
    {
        carta::Session identities;
        identities.minimumEndSample = 100;
        identities.markers = {{10 / carta::sessionRate, "First"},
                              {20 / carta::sessionRate, "Second"}};
        const auto first = identities.markers[0].id, second = identities.markers[1].id;
        check(first.length() == 32 && second.length() == 32 && first != second,
              "new markers receive distinct persistent identities");
        const auto file = dir.getChildFile("marker-identities.carta");
        check(carta::saveSession(identities, 2, file, error), "save marker identities");
        auto document = juce::JSON::parse(file.loadFileAsString());
        check(static_cast<int>(document["version"]) == 13 &&
                  document["markers"][0]["id"].toString() == first,
              "marker identities use project version 13");
        carta::Session identitySession;
        int identityChannels = 0;
        check(carta::loadSession(identitySession, identityChannels, formats, file, error) &&
                  identitySession.markers[0].id == first && identitySession.markers[1].id == second,
              "marker identities survive reopening");
        check(carta::setMarkerSample(identitySession, 1, 25, error) &&
                  carta::renameMarkerIndices(identitySession, {1}, {"Renamed"}, error) &&
                  carta::deleteMarkers(identitySession, {0}, error) &&
                  identitySession.markers[0].id == second,
              "marker move, rename and index change retain identity");
        const auto original = file.loadFileAsString();
        auto duplicate = identities;
        duplicate.markers[1].id = first;
        check(!carta::saveSession(duplicate, 2, file, error) && file.loadFileAsString() == original,
              "duplicate marker identities cannot replace a saved project");
        document["markers"].getArray()->getReference(1).getDynamicObject()->setProperty("id",
                                                                                        first);
        const auto bad = dir.getChildFile("duplicate-marker-ids.carta");
        bad.replaceWithText(juce::JSON::toString(document, false, 20));
        bool unchangedUpgradeFlag = true;
        check(!carta::loadSession(identitySession, identityChannels, formats, bad, error, {},
                                  nullptr, nullptr, nullptr, nullptr, &unchangedUpgradeFlag) &&
                  unchangedUpgradeFlag && error.contains("identity") &&
                  identitySession.markers.size() == 1 && identitySession.markers[0].id == second,
              "duplicate saved identities reject atomically without changing the upgrade flag");
        document["markers"].getArray()->getReference(1).getDynamicObject()->setProperty(
            "id", "not-an-id");
        bad.replaceWithText(juce::JSON::toString(document, false, 20));
        check(!carta::loadSession(identitySession, identityChannels, formats, bad, error) &&
                  error.contains("identity") && identitySession.markers[0].id == second,
              "malformed saved marker identities reject atomically");
        document["markers"].getArray()->getReference(1).getDynamicObject()->removeProperty("id");
        bad.replaceWithText(juce::JSON::toString(document, false, 20));
        check(!carta::loadSession(identitySession, identityChannels, formats, bad, error) &&
                  error.contains("identity") && identitySession.markers[0].id == second,
              "missing version 13 marker identities reject atomically");
        document.getDynamicObject()->setProperty("version", 12);
        for (auto& marker : *document["markers"].getArray())
            marker.getDynamicObject()->removeProperty("id");
        const auto legacy = dir.getChildFile("legacy-marker-ids.carta");
        legacy.replaceWithText(juce::JSON::toString(document, false, 20));
        bool assignedIds = false;
        check(carta::loadSession(identitySession, identityChannels, formats, legacy, error, {},
                                 nullptr, nullptr, nullptr, nullptr, &assignedIds) &&
                  assignedIds && identitySession.markers.size() == 2 &&
                  identitySession.markers[0].id.length() == 32 &&
                  identitySession.markers[0].id != identitySession.markers[1].id,
              "legacy markers receive distinct identities and report the upgrade on load");
        const auto legacyFirst = identitySession.markers[0].id;
        check(
            carta::saveSession(identitySession, 2, legacy, error) &&
                carta::loadSession(identitySession, identityChannels, formats, legacy, error, {},
                                   nullptr, nullptr, nullptr, nullptr, &assignedIds) &&
                !assignedIds && identitySession.markers[0].id == legacyFirst,
            "saving a legacy project persists its assigned identities and clears the upgrade flag");
    }
    {
        // Default MdDisc does not bump the project schema version.
        carta::Session baseline = saved;
        check(carta::isDefaultMdDisc(baseline.mdDisc), "fresh session starts with default MdDisc");
        const auto basePath = dir.getChildFile("default-mddisc.carta");
        check(carta::saveSession(baseline, 2, basePath, error),
              "save baseline project with default MdDisc");
        check(static_cast<int>(juce::JSON::parse(basePath.loadFileAsString())["version"]) < 25,
              "default MdDisc preserves legacy project version");

        // Full v25 MdDisc persistence: medium, defaultCodec, HW/FW titles, timestamp,
        // autoTrackOnImport, per-track overrides & MD flags, multi-part ranges, groups, and
        // importedUtocSectors.
        carta::Session mdSession = saved;
        mdSession.mdDisc.medium = carta::MdMedium::md74;
        mdSession.mdDisc.defaultCodec = carta::MdCodecMode::lp2;
        mdSession.mdDisc.halfWidthTitle = u8"Disc HW 日本語";
        mdSession.mdDisc.fullWidthTitle = "\xe3\x82\xa2\xe3\x83\xab\xe3\x83\x90\xe3\x83\xa0";
        mdSession.mdDisc.discTimestamp = "2026-11-15 08:30:00";
        mdSession.mdDisc.autoTrackOnImport = false;
        mdSession.mdDisc.tracks = {
            {"t1",
             "Track One HW",
             "\xe7\xac\xac\xe4\xb8\x80\xe6\x9b\xb2",
             carta::MdCodecMode::spStereo,
             true,
             true,
             true,
             "2026-11-15 08:31:00",
             {{0, 100}, {100, 400}}},
            {"t2", "Track Two Repeat", "", std::nullopt, false, false, false, "", {{100, 400}}}};
        mdSession.mdDisc.groups = {{1, 2, "Side A", "\xe9\x9d\xa2\xef\xbc\xa1"}};
        mdSession.mdDisc.importedUtocSectors[0].assign(2352, 0x11);
        mdSession.mdDisc.importedUtocSectors[1].assign(2352, 0x22);
        mdSession.mdDisc.importedUtocSectors[2].assign(2352, 0x33);
        mdSession.mdDisc.importedUtocSectors[3].assign(2352, 0x44);

        const auto path = dir.getChildFile("v25-mddisc.carta");
        check(carta::saveSession(mdSession, 2, path, error), "save v25 MdDisc project");
        const auto data = juce::JSON::parse(path.loadFileAsString());
        check(static_cast<int>(data["version"]) == 25 && data["mdDisc"].isObject(),
              "non-default MdDisc writes version 25");

        carta::Session opened;
        int mode = 0;
        carta::WavRange restoredLoop{3, 4};
        check(carta::loadSession(opened, mode, formats, path, error, {}, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, &restoredLoop) &&
                  opened.mdDisc == mdSession.mdDisc && restoredLoop.start == 0 &&
                  restoredLoop.end == 0,
              "loadSession restores complete v25 MdDisc identically");

        juce::File portable;
        check(carta::exportPortableSession(mdSession, 2, dir, portable, error),
              "export portable session with MdDisc");
        carta::Session portableOpened;
        int portableMode = 0;
        check(carta::loadSession(portableOpened, portableMode, formats,
                                 portable.getChildFile("Project.carta"), error) &&
                  portableOpened.mdDisc == mdSession.mdDisc,
              "portable session preserves complete MdDisc");

        const auto pkgPath = dir.getChildFile("v25-mddisc-pkg.carta");
        check(carta::saveNativePackage(mdSession, 2, pkgPath, error),
              "save native package with MdDisc");
        carta::Session pkgOpened;
        int pkgChannels = 0;
        check(carta::loadNativePackage(pkgOpened, pkgChannels, formats, pkgPath, error) &&
                  pkgOpened.mdDisc == mdSession.mdDisc,
              "native package preserves complete MdDisc");

        const carta::WavRange loop{10, 200};
        check(carta::saveSession(mdSession, 2, path, error, {}, nullptr, loop),
              "save v25 MdDisc with loop");
        check(carta::loadSession(opened, mode, formats, path, error, {}, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, &restoredLoop) &&
                  restoredLoop.start == 10 && restoredLoop.end == 200 &&
                  opened.mdDisc == mdSession.mdDisc,
              "v25 MdDisc and saved loop roundtrip together");

        // Reject corrupt mdDisc data atomically without disturbing opened session.
        auto badVersion = juce::JSON::parse(path.loadFileAsString());
        badVersion.getDynamicObject()->setProperty("version", 24);
        const auto badVerPath = dir.getChildFile("v24-with-mddisc.carta");
        badVerPath.replaceWithText(juce::JSON::toString(badVersion));
        opened.minimumEndSample = 123;
        mode = 1;
        check(!carta::loadSession(opened, mode, formats, badVerPath, error) && mode == 1 &&
                  opened.minimumEndSample == 123 && opened.mdDisc == mdSession.mdDisc,
              "v24 project containing mdDisc is rejected atomically");

        auto badGroup = juce::JSON::parse(path.loadFileAsString());
        badGroup["mdDisc"]["groups"].getArray()->getReference(0).getDynamicObject()->setProperty(
            "endTrack", 9);
        const auto badGroupPath = dir.getChildFile("bad-mddisc-group.carta");
        badGroupPath.replaceWithText(juce::JSON::toString(badGroup));
        check(!carta::loadSession(opened, mode, formats, badGroupPath, error) &&
                  opened.mdDisc == mdSession.mdDisc,
              "out-of-range group in v25 mdDisc is rejected atomically");

        auto badSector = juce::JSON::parse(path.loadFileAsString());
        badSector["mdDisc"]["importedUtocSectors"].getArray()->set(0, juce::var("AAAA"));
        const auto badSecPath = dir.getChildFile("bad-mddisc-sector.carta");
        badSecPath.replaceWithText(juce::JSON::toString(badSector));
        check(!carta::loadSession(opened, mode, formats, badSecPath, error) &&
                  opened.mdDisc == mdSession.mdDisc,
              "malformed importedUtocSectors size is rejected atomically");
    }
    {
        constexpr juce::int64 sample = 1587600008LL;
        const double markerTime = static_cast<double>(sample) / carta::sessionRate;
        check(std::abs(markerTime * carta::sessionRate - static_cast<double>(sample)) > 1e-7 &&
                  carta::markerPositionOnSampleGrid(markerTime),
              "canonical sample time remains aligned after a long-position floating-point round "
              "trip");
        carta::Session longSession;
        longSession.minimumEndSample = sample + 20;
        longSession.tracks.push_back({"Silent master", {}});
        longSession.markers = {{markerTime, "Late track"}};
        const auto inspected = carta::inspectMarkers(longSession);
        check(inspected.size() == 1 && inspected[0].sample == sample && !inspected[0].offGrid,
              "marker inspection handles long-position sample boundary");
        carta::MdDisc synced;
        std::string syncErr;
        check(carta::syncTracksFromMarkers(longSession, synced, syncErr) &&
                  synced.tracks.size() == 2 && synced.tracks[1].parts[0].startSample == sample,
              "syncTracksFromMarkers preserves long-position sample boundary");
        const auto markerFile = dir.getChildFile("long-position-markers.json");
        std::vector<carta::Marker> imported;
        check(carta::writeMarkerFile(longSession, markerFile, error) &&
                  carta::readMarkerFile(longSession, markerFile, imported, error) &&
                  imported.size() == 1 &&
                  std::llround(imported[0].position * carta::sessionRate) == sample,
              "marker exchange preserves a long-position sample boundary");
        const auto shortWav = dir.getChildFile("long-position-cue.wav");
        bool clipped = false;
        auto bounded = longSession;
        bounded.markers.push_back(
            {static_cast<double>(sample + 9) / carta::sessionRate, "Exclusive end"});
        check(carta::exportWavRange(bounded, 2, {sample, sample + 9}, shortWav, clipped, error, {},
                                    {}, {carta::WavEncoding::float32, false, true}),
              "export a short WAV bounded by long-position markers");
        std::unique_ptr<juce::AudioFormatReader> cueReader(formats.createReaderFor(shortWav));
        check(
            cueReader && cueReader->metadataValues["NumCuePoints"] == "1" &&
                cueReader->metadataValues["Cue0Offset"] == "0",
            "a canonical marker at the export start is included and the exclusive end is excluded");
        auto shifted = longSession;
        check(carta::shiftMarkerIndices(shifted, {0}, 1, error) &&
                  std::llround(shifted.markers[0].position * carta::sessionRate) == sample + 1,
              "long-position aligned marker can be moved by one sample");
        const carta::TimeSelection gap{sample - 2, sample - 1, 0, 0};
        auto inserted = longSession, closed = longSession;
        check(carta::insertGap(inserted, gap, true, error) &&
                  std::llround(inserted.markers[0].position * carta::sessionRate) == sample + 1,
              "all-track insertion preserves a long-position aligned marker");
        check(carta::rippleRemoveRange(closed, gap, true, &error) &&
                  std::llround(closed.markers[0].position * carta::sessionRate) == sample - 1,
              "all-track gap removal preserves a long-position aligned marker");
    }
    const auto project = dir.getChildFile("test.carta");
    check(carta::saveSession(saved, 1, project, error), "save project");
    {
        const auto audioEnd = carta::projectEndSample(saved);
        {
            auto shifted = saved;
            shifted.minimumEndSample = audioEnd + 32;
            shifted.markers = {{10.0 / carta::sessionRate, "One"},
                               {20.0 / carta::sessionRate, "Two"},
                               {30.0 / carta::sessionRate, "Three"}};
            check(carta::shiftMarkerIndices(shifted, {0, 2}, 2, error),
                  "shift nonadjacent markers before project save");
            std::vector<carta::MasterTrack> boundaries;
            check(carta::getMasterTracks(shifted, boundaries, error) && boundaries.size() == 4 &&
                      boundaries[1].range.start == 12 && boundaries[2].range.start == 20 &&
                      boundaries[3].range.start == 32 &&
                      boundaries.back().range.end == audioEnd + 32,
                  "master track list reflects shifted and stationary boundaries");
            const auto shiftedPath = dir.getChildFile("shifted-selected-markers.carta");
            check(carta::saveSession(shifted, 1, shiftedPath, error), "save selected marker shift");
            carta::Session restoredShift;
            int restoredChannels = 0;
            check(
                carta::loadSession(restoredShift, restoredChannels, formats, shiftedPath, error) &&
                    carta::getMasterTracks(restoredShift, boundaries, error) &&
                    boundaries[1].range.start == 12 && boundaries[2].range.start == 20 &&
                    boundaries[3].range.start == 32,
                "shifted marker positions survive reopening");
            juce::AudioBuffer<float> originalAudio(1, static_cast<int>(audioEnd)),
                shiftedAudio(1, static_cast<int>(audioEnd));
            originalAudio.clear();
            shiftedAudio.clear();
            saved.render(originalAudio, 0, static_cast<int>(audioEnd), 0, carta::sessionRate, 1);
            restoredShift.render(shiftedAudio, 0, static_cast<int>(audioEnd), 0, carta::sessionRate,
                                 1);
            for (int i = 0; i < static_cast<int>(audioEnd); ++i)
                check(near(originalAudio.getSample(0, i), shiftedAudio.getSample(0, i)),
                      "marker shifts do not change the rendered master audio");
        }
        auto extended = saved;
        extended.markers.clear();
        extended.minimumEndSample = audioEnd + 64;
        extended.markers.push_back(
            {static_cast<double>(audioEnd + 16) / carta::sessionRate, "Silence"});
        check(carta::projectEndSample(extended) == audioEnd + 64,
              "minimum duration extends master past the last clip");
        std::vector<carta::MasterTrack> tracks;
        check(carta::getMasterTracks(extended, tracks, error) && tracks.size() == 2 &&
                  tracks.back().range.start == audioEnd + 16 &&
                  tracks.back().range.end == audioEnd + 64,
              "track markers can divide deliberate trailing silence");
        const auto path = dir.getChildFile("extended.carta");
        check(carta::saveSession(extended, 1, path, error), "save explicit master duration");
        juce::var json;
        check(juce::JSON::parse(path.loadFileAsString(), json).wasOk() &&
                  static_cast<int>(json["version"]) == 13 &&
                  static_cast<juce::int64>(json["minimumEndSample"]) == audioEnd + 64,
              "explicit duration and marker identities retain an integer end");
        auto markerless = extended;
        markerless.markers.clear();
        const auto markerlessPath = dir.getChildFile("extended-markerless.carta");
        check(carta::saveSession(markerless, 1, markerlessPath, error) &&
                  static_cast<int>(
                      juce::JSON::parse(markerlessPath.loadFileAsString())["version"]) == 9,
              "markerless explicit duration retains version 9 compatibility");
        carta::Session reopened;
        int channels = 2;
        check(carta::loadSession(reopened, channels, formats, path, error) &&
                  reopened.minimumEndSample == audioEnd + 64 &&
                  carta::projectEndSample(reopened) == audioEnd + 64,
              "explicit duration survives project reopening");
        bool clipped = false;
        const auto output = dir.getChildFile("extended.wav");
        check(carta::exportWav(reopened, 1, output, clipped, error),
              "export master with trailing silence");
        const auto rendered = carta::loadAudio(formats, output, error);
        check(rendered && rendered->numSamples() == audioEnd + 64,
              "WAV includes the explicit trailing duration");
        for (int i = static_cast<int>(audioEnd); i < rendered->numSamples(); ++i)
            check(std::abs(rendered->samples.getSample(0, i)) < 1e-8f,
                  "trailing exported audio is silent");
        {
            const auto whole = dir.getChildFile("extended-float.wav");
            check(carta::exportWavRange(reopened, 1, {0, audioEnd + 64}, whole, clipped, error, {},
                                        {}, {carta::WavEncoding::float32, false}),
                  "export float master with a silent tail");
            const auto wholeAudio = carta::loadAudio(formats, whole, error);
            check(wholeAudio != nullptr, "read float master with silent tail");
            juce::AudioBuffer<float> playback(1, wholeAudio->numSamples());
            playback.clear();
            const auto range = carta::fullMasterPlaybackRange(reopened);
            carta::ResamplingContext context;
            reopened.prepareResampling(context, carta::sessionRate);
            double pos = 0;
            for (int i = 0; i < playback.getNumSamples(); i += 23) {
                const int count = std::min(23, playback.getNumSamples() - i);
                const bool playing = carta::renderPlayback(reopened, playback, i, count, pos,
                                                           carta::sessionRate, 1, range, &context);
                check(playing == (i + count < playback.getNumSamples()),
                      "silent-tail playback stops on the exported master boundary");
            }
            for (int i = 0; i < playback.getNumSamples(); ++i)
                check(near(playback.getSample(0, i), wholeAudio->samples.getSample(0, i)),
                      "playback and float export agree through the silent tail");
            for (size_t index = 0; index < tracks.size(); ++index) {
                const auto partPath = dir.getChildFile(
                    "extended-part-" + juce::String(static_cast<int>(index)) + ".wav");
                check(carta::exportWavRange(reopened, 1, tracks[index].range, partPath, clipped,
                                            error, {}, {}, {carta::WavEncoding::float32, false}),
                      "export marker-defined track including silent tail");
                const auto part = carta::loadAudio(formats, partPath, error);
                check(part &&
                          part->numSamples() == tracks[index].range.end - tracks[index].range.start,
                      "exported track keeps its exact sample span");
                for (int i = 0; i < part->numSamples(); ++i) {
                    const auto a = part->samples.getSample(0, i),
                               b = wholeAudio->samples.getSample(
                                   0, static_cast<int>(tracks[index].range.start) + i);
                    check(std::memcmp(&a, &b, sizeof(float)) == 0,
                          "marker track exports reconstruct the full master including silence");
                }
            }
        }
        const auto copied = carta::copyRange(reopened, {audioEnd - 16, audioEnd + 64, 0, 0});
        check(copied.hasAudio() && carta::pasteRange(reopened, copied, 0, audioEnd + 64, &error) &&
                  carta::projectEndSample(reopened) == audioEnd + 144,
              "pasting audio plus silence extends the master by the full copied span");
        const auto pastedProject = dir.getChildFile("pasted-tail.carta");
        check(carta::saveSession(reopened, channels, pastedProject, error),
              "save pasted silent tail");
        carta::Session pastedReopened;
        int pastedChannels = 0;
        check(carta::loadSession(pastedReopened, pastedChannels, formats, pastedProject, error) &&
                  carta::projectEndSample(pastedReopened) == audioEnd + 144,
              "pasted silent tail survives reopening");
        const auto pastedWav = dir.getChildFile("pasted-tail.wav");
        check(carta::exportWav(pastedReopened, 1, pastedWav, clipped, error),
              "export pasted silent tail");
        const auto pastedAudio = carta::loadAudio(formats, pastedWav, error);
        check(pastedAudio && pastedAudio->numSamples() == audioEnd + 144,
              "WAV retains the complete pasted span");
        for (int i = static_cast<int>(audioEnd + 80); i < pastedAudio->numSamples(); ++i)
            check(std::abs(pastedAudio->samples.getSample(0, i)) < 1e-8f,
                  "pasted trailing silence remains silent after save and export");
        auto shortened = extended;
        shortened.minimumEndSample = 0;
        check(!carta::preservesMarkerBounds(extended, shortened, error),
              "removing explicit duration cannot orphan a marker");
        auto gap = saved;
        gap.markers.clear();
        check(carta::insertGap(gap, {audioEnd, audioEnd + 64, 0, 0}, true, error) &&
                  carta::projectEndSample(gap) == audioEnd + 64,
              "all-track insertion at the end creates trailing silence");
        check(carta::rippleRemoveRange(gap, {audioEnd, audioEnd + 64, 0, 0}, true, &error) &&
                  carta::projectEndSample(gap) == audioEnd,
              "all-track ripple removes trailing silence");
        auto cutTail = saved;
        cutTail.markers = {{static_cast<double>(audioEnd - 10) / carta::sessionRate, "Tail"}};
        check(carta::removeRange(cutTail, {audioEnd - 20, audioEnd, 0, 0}, &error) &&
                  cutTail.minimumEndSample == audioEnd,
              "deleting the last audio leaves a silent tail");
        const auto cutPath = dir.getChildFile("cut-tail.carta");
        check(carta::saveSession(cutTail, 1, cutPath, error),
              "save project with a deleted silent tail");
        carta::Session cutReopened;
        int cutChannels = 0;
        check(carta::loadSession(cutReopened, cutChannels, formats, cutPath, error) &&
                  carta::projectEndSample(cutReopened) == audioEnd &&
                  cutReopened.markers.size() == 1,
              "reopening retains the silent tail and its marker");
        const auto cutWav = dir.getChildFile("cut-tail.wav");
        check(carta::exportWav(cutReopened, 1, cutWav, clipped, error), "export the cut master");
        const auto cutRender = carta::loadAudio(formats, cutWav, error);
        check(cutRender && cutRender->numSamples() == audioEnd,
              "cut master exports its original duration");
        for (int i = static_cast<int>(audioEnd - 20); i < cutRender->numSamples(); ++i)
            check(near(cutRender->samples.getSample(0, i), 0), "deleted tail exports as silence");
        auto invalid = extended;
        invalid.minimumEndSample = -1;
        const auto previous = path.loadFileAsString();
        check(!carta::saveSession(invalid, 1, path, error) && path.loadFileAsString() == previous,
              "invalid explicit duration cannot replace an existing project");
        auto longest = extended;
        longest.minimumEndSample = 9007199254740991LL;
        const auto limitPath = dir.getChildFile("longest-duration.carta");
        check(carta::saveSession(longest, 1, limitPath, error),
              "save the largest precise master duration");
        carta::Session longestReopened;
        int longestChannels = 0;
        check(carta::loadSession(longestReopened, longestChannels, formats, limitPath, error) &&
                  longestReopened.minimumEndSample == 9007199254740991LL &&
                  carta::projectEndSample(longestReopened) == 9007199254740991LL,
              "largest supported duration survives project reopening exactly");
    }
    {
        const auto existing = project.loadFileAsString();
        auto badName = saved;
        badName.tracks[0].clips[0].name = juce::String::repeatedString("x", 1025);
        check(!carta::saveSession(badName, 1, project, error) &&
                  project.loadFileAsString() == existing,
              "oversized clip name cannot replace a saved project");
        for (double threshold : {0.0, 0.2, 0.6, 0.9, 0.99}) {
            check(!carta::saveSession(
                      saved, 1, project, error,
                      [threshold](double progress) { return progress < threshold; }) &&
                      error == "Project save cancelled.",
                  "project save cancels before destination replacement");
            check(project.loadFileAsString() == existing,
                  "cancelled save preserves previous project bytes");
        }
        double last = -1;
        bool monotonic = true;
        check(carta::saveSession(saved, 1, project, error,
                                 [&](double progress) {
                                     if (progress < last)
                                         monotonic = false;
                                     last = progress;
                                     return true;
                                 }) &&
                  monotonic && std::abs(last - 1) < 1e-12,
              "project save reports monotonic complete progress");
        check(carta::saveSession(saved, 1, project, error,
                                 [](double progress) { return progress < 1.0; }),
              "cancellation after atomic commit does not report a failed save");
        carta::Session reopened;
        int channels = 2;
        check(carta::loadSession(reopened, channels, formats, project, error) && channels == 1,
              "committed project remains readable");
    }
    {
        auto splitProject = saved;
        check(splitProject.split(0, 0, 16.0 / carta::sessionRate),
              "split persisted fade inside fade-in");
        const auto path = dir.getChildFile("faded-split.carta");
        check(carta::saveSession(splitProject, 1, path, error), "save split fade");
        carta::Session reopened;
        int reopenedMode = 2;
        check(carta::loadSession(reopened, reopenedMode, formats, path, error) &&
                  reopened.tracks[0].clips.size() == 2,
              "reopen split fade");
        juce::AudioBuffer<float> before(2, 128), afterReopen(2, 128);
        before.clear();
        afterReopen.clear();
        saved.render(before, 0, 128, 0, 44100, 2);
        reopened.render(afterReopen, 0, 128, 0, 44100, 2);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 128; ++i)
                check(near(before.getSample(ch, i), afterReopen.getSample(ch, i)),
                      "fade survives split and project round trip");
    }
    {
        double last = -1;
        bool monotonic = true;
        carta::Session loadedProject;
        int channels = 2;
        check(carta::loadSession(loadedProject, channels, formats, project, error,
                                 [&](double progress) {
                                     if (progress < last)
                                         monotonic = false;
                                     last = progress;
                                     return true;
                                 }) &&
                  monotonic && std::abs(last - 1) < 1e-12,
              "project opening reports complete monotonic progress");
        for (double threshold : {0.0, 0.2, 0.6, 0.9, 1.0}) {
            auto preserved = saved;
            const auto source = preserved.tracks[0].clips[0].audio;
            int preservedChannels = 2;
            check(!carta::loadSession(
                      preserved, preservedChannels, formats, project, error,
                      [threshold](double progress) { return progress < threshold; }) &&
                      error == "Project opening cancelled.",
                  "project opening can cancel throughout decoding and finalization");
            check(preservedChannels == 2 && preserved.tracks.size() == saved.tracks.size() &&
                      preserved.tracks[0].clips[0].audio == source,
                  "cancelled project opening preserves current session and channels");
        }
    }
    {
        auto combined = saved;
        combined.markers.clear();
        combined.tracks[0].view = carta::TrackView::combined;
        const auto combinedPath = dir.getChildFile("combined-view.carta");
        check(carta::saveSession(combined, 1, combinedPath, error), "save combined track view");
        juce::var data;
        check(juce::JSON::parse(combinedPath.loadFileAsString(), data).wasOk() &&
                  static_cast<int>(data["version"]) == 7 &&
                  static_cast<int>(data["tracks"][0]["view"]) == 3,
              "combined view uses version 7 project format");
        carta::Session reopened;
        int channels = 2;
        check(carta::loadSession(reopened, channels, formats, combinedPath, error) &&
                  reopened.tracks[0].view == carta::TrackView::combined,
              "combined view survives project round trip");
        juce::AudioBuffer<float> beforeCombined(2, 128), afterCombined(2, 128);
        beforeCombined.clear();
        afterCombined.clear();
        combined.render(beforeCombined, 0, 128, 0, carta::sessionRate, 2);
        reopened.render(afterCombined, 0, 128, 0, carta::sessionRate, 2);
        for (int ch = 0; ch < 2; ++ch)
            check(std::memcmp(beforeCombined.getReadPointer(ch), afterCombined.getReadPointer(ch),
                              128 * sizeof(float)) == 0,
                  "combined view round trip preserves rendered samples");
        data.getDynamicObject()->setProperty("version", 6);
        check(combinedPath.replaceWithText(juce::JSON::toString(data)),
              "write incompatible view fixture");
        auto untouched = saved;
        const auto originalSource = untouched.tracks[0].clips[0].audio;
        channels = 2;
        check(!carta::loadSession(untouched, channels, formats, combinedPath, error) &&
                  channels == 2 && untouched.tracks[0].clips[0].audio == originalSource,
              "version 6 rejects combined view without replacing the session");
    }
    carta::Session restored;
    int mode = 2;
    check(carta::loadSession(restored, mode, formats, project, error), "load project");
    check(restored.tracks[0].view == carta::TrackView::spectrogram,
          "per-track view survives project round trip");
    check(restored.tracks[0].collapsed && restored.tracks[0].height == 220,
          "collapsed state and expansion height survive project round trip");
    check(mode == 1 && restored.tracks.size() == 1 && restored.markers.size() == 1 &&
              restored.markers[0].name == "Track 1" && restored.tracks[0].height == 220 &&
              near(restored.tracks[0].gain, 0.5f) && near(restored.tracks[0].pan, -0.25f) &&
              restored.tracks[0].solo && restored.tracks[0].routing.size() == 4 &&
              near(restored.tracks[0].routing[0].left, 0.5f) &&
              near(restored.tracks[0].clips[0].gain, 0.75f) &&
              std::abs(restored.tracks[0].clips[0].fadeIn - 0.001) < 1e-9 &&
              carta::clipDisplayName(restored.tracks[0].clips[0]) ==
                  juce::String::fromUTF8("Intro 日本語"),
          "restore arrangement, mixer, clip envelope and Unicode name");
    {
        const auto projectText = project.loadFileAsString();
        const auto invalidPath = dir.getChildFile("invalid-project.carta");
        const auto rejected = [&](const std::function<void(juce::var&)>& alter,
                                  const char* message) {
            juce::var data;
            check(juce::JSON::parse(projectText, data).wasOk(), "parse project validation fixture");
            alter(data);
            check(invalidPath.replaceWithText(juce::JSON::toString(data)),
                  "write project validation fixture");
            auto untouched = restored;
            const auto prior = untouched.tracks[0].clips[0].audio;
            int channels = 2;
            bool recovery = true, relinked = true;
            carta::SourceRelinkRequest request;
            check(!carta::loadSession(untouched, channels, formats, invalidPath, error, {},
                                      &recovery, nullptr, &request, &relinked) &&
                      channels == 2 && recovery && relinked &&
                      untouched.tracks[0].clips[0].audio == prior && request.expectedHash.isEmpty(),
                  message);
        };
        rejected([](juce::var& v) { v.getDynamicObject()->setProperty("version", 4.5); },
                 "fractional project version is rejected atomically");
        rejected(
            [](juce::var& v) {
                v["tracks"].getArray()->getReference(0).getDynamicObject()->setProperty("view", 9);
            },
            "invalid track view is rejected atomically");
        rejected(
            [](juce::var& v) {
                v["tracks"].getArray()->getReference(0).getDynamicObject()->setProperty("collapsed",
                                                                                        1);
            },
            "invalid collapsed flag is rejected atomically");
        rejected(
            [](juce::var& v) {
                v["tracks"].getArray()->getReference(0).getDynamicObject()->removeProperty("view");
            },
            "missing version 5 track view is rejected atomically");
        rejected(
            [](juce::var& v) {
                v["tracks"]
                    .getArray()
                    ->getReference(0)["clips"]
                    .getArray()
                    ->getReference(0)
                    .getDynamicObject()
                    ->removeProperty("exactGeometry");
            },
            "missing version 6 exact geometry is rejected atomically");
        rejected(
            [](juce::var& v) {
                v["tracks"]
                    .getArray()
                    ->getReference(0)["clips"]
                    .getArray()
                    ->getReference(0)["exactGeometry"]
                    .getArray()
                    ->set(0, "not-hex");
            },
            "malformed exact clip geometry is rejected atomically");
        rejected(
            [](juce::var& v) {
                v["tracks"]
                    .getArray()
                    ->getReference(0)["clips"]
                    .getArray()
                    ->getReference(0)["exactGeometry"]
                    .getArray()
                    ->set(0, "7ff0000000000000");
            },
            "non-finite exact clip geometry is rejected atomically");
        rejected(
            [](juce::var& v) {
                v["tracks"]
                    .getArray()
                    ->getReference(0)["clips"]
                    .getArray()
                    ->getReference(0)["exactGeometry"]
                    .getArray()
                    ->set(0, "3ff0000000000000");
            },
            "exact clip geometry inconsistent with its displayed value is rejected atomically");
        rejected(
            [](juce::var& v) {
                v["markers"].getArray()->getReference(0).getDynamicObject()->removeProperty(
                    "exactPosition");
            },
            "missing exact marker position is rejected atomically");
        rejected(
            [](juce::var& v) {
                v["markers"].getArray()->getReference(0).getDynamicObject()->setProperty(
                    "exactPosition", "7ff8000000000000");
            },
            "non-finite exact marker position is rejected atomically");
        rejected([](juce::var& v) { v.getDynamicObject()->setProperty("masterChannels", 1.5); },
                 "fractional master count is rejected atomically");
        rejected([](juce::var& v) { v.getDynamicObject()->setProperty("recovery", "yes"); },
                 "invalid recovery flag is rejected atomically");
        rejected(
            [](juce::var& v) {
                auto* c = v["tracks"]
                              .getArray()
                              ->getReference(0)["clips"]
                              .getArray()
                              ->getReference(0)
                              .getDynamicObject();
                c->setProperty("start", 1.0e300);
                c->setProperty("source", "missing.wav");
            },
            "unrepresentable timeline is rejected before a missing-source relink prompt");
        rejected(
            [](juce::var& v) {
                auto* c = v["tracks"]
                              .getArray()
                              ->getReference(0)["clips"]
                              .getArray()
                              ->getReference(0)
                              .getDynamicObject();
                c->setProperty("start", 1.0e11);
                c->setProperty("duration", 1.0e-9);
            },
            "clip duration lost to floating-point precision is rejected");
        rejected(
            [](juce::var& v) {
                v["markers"].getArray()->getReference(0).getDynamicObject()->setProperty("position",
                                                                                         1.0e300);
            },
            "unrepresentable marker is rejected atomically");
        auto kept = restored;
        int keptChannels = 2;
        carta::SourceRelinkRequest request;
        const unsigned char badText[] = {0x7b, 0xff, 0x7d};
        check(invalidPath.replaceWithData(badText, sizeof(badText)), "write invalid project UTF-8");
        check(!carta::loadSession(kept, keptChannels, formats, invalidPath, error, {}, nullptr,
                                  nullptr, &request) &&
                  keptChannels == 2 && error.contains("UTF-8"),
              "invalid project UTF-8 preserves session");
        const char nulText[] = {'{', '}', 0, 'x'};
        check(invalidPath.replaceWithData(nulText, sizeof(nulText)),
              "write project with embedded NUL");
        check(!carta::loadSession(kept, keptChannels, formats, invalidPath, error) &&
                  error.contains("UTF-8"),
              "project with embedded NUL is rejected");
        const auto bomProject = dir.getChildFile("bom-project.carta");
        std::string bom = "\xef\xbb\xbf";
        bom += projectText.toStdString();
        check(bomProject.replaceWithData(bom.data(), bom.size()) &&
                  carta::loadSession(kept, keptChannels, formats, bomProject, error) &&
                  keptChannels == 1,
              "UTF-8 BOM and non-ASCII clip names remain readable");
        const auto oversized = dir.getChildFile("oversized-project.carta");
        auto stream = oversized.createOutputStream();
        check(stream && stream->setPosition(64LL * 1024 * 1024) && stream->writeByte(0),
              "write oversized project fixture");
        stream.reset();
        keptChannels = 2;
        check(!carta::loadSession(kept, keptChannels, formats, oversized, error) &&
                  error.contains("64 MiB") && keptChannels == 2,
              "oversized project is rejected before parsing");
        const auto bytes = project.loadFileAsString();
        auto invalidSave = saved;
        invalidSave.tracks[0].clips[0].start = 1.0e300;
        check(!carta::saveSession(invalidSave, 1, project, error) &&
                  project.loadFileAsString() == bytes,
              "invalid clip cannot replace saved project");
        invalidSave = saved;
        invalidSave.tracks[0].gain = std::numeric_limits<float>::quiet_NaN();
        check(!carta::saveSession(invalidSave, 1, project, error) &&
                  project.loadFileAsString() == bytes,
              "invalid track cannot replace saved project");
        invalidSave = saved;
        invalidSave.tracks[0].view = static_cast<carta::TrackView>(9);
        check(!carta::saveSession(invalidSave, 1, project, error) &&
                  project.loadFileAsString() == bytes,
              "invalid view cannot replace saved project");
        invalidSave = saved;
        invalidSave.markers[0].position = 1.0e300;
        check(!carta::saveSession(invalidSave, 1, project, error) &&
                  project.loadFileAsString() == bytes,
              "invalid marker cannot replace saved project");
        auto repairable = saved;
        repairable.markers = {{1000 / carta::sessionRate, "Beyond end"},
                              {1000 / carta::sessionRate, "Duplicate"}};
        const auto repairPath = dir.getChildFile("repair-markers.carta");
        check(carta::saveSession(repairable, 1, repairPath, error) &&
                  carta::loadSession(kept, keptChannels, formats, repairPath, error) &&
                  kept.markers.size() == 2,
              "duplicate or out-of-range legacy markers remain recoverable for editing");
        std::vector<carta::MasterTrack> tracks;
        check(!carta::getMasterTracks(kept, tracks, error),
              "unrepaired markers still block master export");
    }
    juce::var projectData;
    check(juce::JSON::parse(project.loadFileAsString(), projectData).wasOk(),
          "parse project fixture");
    check(static_cast<int>(projectData["version"]) == 13,
          "project with markers uses version 13 and exact geometry");
    {
        const auto invalid = dir.getChildFile("overflow-version.carta");
        check(
            invalid.replaceWithText(
                R"({"version":18446744073709551622,"masterChannels":2,"tracks":[],"markers":[]})"),
            "write overflowing project version");
        auto retained = restored;
        int retainedMode = 1;
        check(!carta::loadSession(retained, retainedMode, formats, invalid, error) &&
                  retained.tracks.size() == restored.tracks.size() && retainedMode == 1,
              "overflowing project version cannot replace the current session");
    }
    {
        auto legacy = juce::JSON::parse(project.loadFileAsString());
        legacy.getDynamicObject()->setProperty("version", 5);
        legacy["tracks"].getArray()->getReference(0).getDynamicObject()->removeProperty(
            "collapsed");
        const auto v5 = dir.getChildFile("version-five-before-collapse.carta");
        check(v5.replaceWithText(juce::JSON::toString(legacy)), "write earlier version 5 fixture");
        carta::Session old;
        int oldMode = 2;
        check(carta::loadSession(old, oldMode, formats, v5, error) && !old.tracks[0].collapsed &&
                  old.tracks[0].height == 220,
              "earlier version 5 projects open expanded with saved height");
    }
    {
        auto legacy = juce::JSON::parse(project.loadFileAsString());
        legacy.getDynamicObject()->setProperty("version", 4);
        legacy["tracks"].getArray()->getReference(0).getDynamicObject()->removeProperty("view");
        const auto v4 = dir.getChildFile("version-four.carta");
        check(v4.replaceWithText(juce::JSON::toString(legacy)), "write version 4 view fixture");
        carta::Session old;
        int oldMode = 2;
        check(carta::loadSession(old, oldMode, formats, v4, error) &&
                  old.tracks[0].view == carta::TrackView::useDefault && !old.tracks[0].collapsed,
              "version 4 tracks use the default expanded view");
    }
    auto* v2Tracks = projectData["tracks"].getArray();
    auto* v2Clip = (*v2Tracks)[0]["clips"].getArray()->getReference(0).getDynamicObject();
    const auto originalName = v2Clip->getProperty("clipName");
    v2Clip->removeProperty("clipName");
    const auto missingName = dir.getChildFile("missing-clip-name.carta");
    check(missingName.replaceWithText(juce::JSON::toString(projectData)),
          "write missing clip name fixture");
    carta::Session unchangedName = restored;
    int unchangedNameMode = 1;
    check(!carta::loadSession(unchangedName, unchangedNameMode, formats, missingName, error) &&
              unchangedName.tracks[0].clips[0].name == juce::String::fromUTF8("Intro 日本語"),
          "invalid version 4 name preserves current session");
    projectData.getDynamicObject()->setProperty("version", 3);
    const auto versionThree = dir.getChildFile("version-three.carta");
    check(versionThree.replaceWithText(juce::JSON::toString(projectData)),
          "write version 3 fixture");
    carta::Session oldV3;
    int oldV3Mode = 2;
    check(carta::loadSession(oldV3, oldV3Mode, formats, versionThree, error) &&
              oldV3.tracks[0].clips[0].name.isEmpty() &&
              carta::clipDisplayName(oldV3.tracks[0].clips[0]) == wavfile.getFileName(),
          "version 3 clips default to source filename");
    v2Clip->setProperty("clipName", originalName);
    projectData.getDynamicObject()->setProperty("version", 4);
    const auto fadeAnchor = v2Clip->getProperty("fadeInStart");
    v2Clip->removeProperty("fadeInStart");
    const auto malformed = dir.getChildFile("incomplete-envelope.carta");
    check(malformed.replaceWithText(juce::JSON::toString(projectData)),
          "write incomplete envelope fixture");
    carta::Session unchangedProject = restored;
    int unchangedProjectMode = 1;
    check(!carta::loadSession(unchangedProject, unchangedProjectMode, formats, malformed, error) &&
              unchangedProject.tracks.size() == restored.tracks.size(),
          "incomplete envelope cannot replace current project");
    v2Clip->setProperty("fadeInStart", fadeAnchor);
    projectData.getDynamicObject()->setProperty("version", 2);
    auto* versionTwoTrack = projectData["tracks"].getArray()->getReference(0).getDynamicObject();
    const auto versionTwo = dir.getChildFile("version-two.carta");
    check(versionTwo.replaceWithText(juce::JSON::toString(projectData)), "write version 2 fixture");
    carta::Session oldV2;
    int oldV2Mode = 2;
    check(carta::loadSession(oldV2, oldV2Mode, formats, versionTwo, error) &&
              oldV2.tracks[0].routing.empty(),
          "version 2 projects load with unconfirmed routing");
    versionTwoTrack->removeProperty("routing");
    projectData.getDynamicObject()->setProperty("version", 1);
    auto* projectTracks = projectData["tracks"].getArray();
    check(projectTracks != nullptr && !projectTracks->isEmpty(), "project tracks present");
    auto* legacyTrack = projectTracks->getReference(0).getDynamicObject();
    auto* legacyClips = legacyTrack->getProperty("clips").getArray();
    check(legacyClips && !legacyClips->isEmpty(), "legacy clip fixture");
    for (const auto* property :
         {"sourceSha256", "gain", "fadeIn", "fadeOut", "fadeInStart", "fadeOutEnd"})
        legacyClips->getReference(0).getDynamicObject()->removeProperty(property);
    for (const auto* property : {"height", "muted", "solo", "gain", "pan"})
        legacyTrack->removeProperty(property);
    const auto oldProject = dir.getChildFile("old.carta");
    check(oldProject.replaceWithText(juce::JSON::toString(projectData)),
          "write old project fixture");
    carta::Session oldSession;
    int oldMode = 2;
    check(carta::loadSession(oldSession, oldMode, formats, oldProject, error) &&
              oldSession.tracks[0].height == 116 && near(oldSession.tracks[0].gain, 1) &&
              near(oldSession.tracks[0].pan, 0) && !oldSession.tracks[0].solo &&
              near(oldSession.tracks[0].clips[0].gain, 1) &&
              std::abs(oldSession.tracks[0].clips[0].fadeIn) < 1e-12,
          "older projects get default track, mixer and clip settings");
    projectTracks->getReference(0).getDynamicObject()->setProperty("height", 900);
    check(oldProject.replaceWithText(juce::JSON::toString(projectData)),
          "write invalid height fixture");
    check(!carta::loadSession(oldSession, oldMode, formats, oldProject, error) &&
              oldSession.tracks[0].height == 116,
          "invalid height does not replace session");
    {
        juce::MemoryBlock originalAudio;
        check(wavfile.loadFileAsData(originalAudio) && originalAudio.getSize() > 100,
              "read source identity fixture");
        auto modifiedAudio = originalAudio;
        static_cast<unsigned char*>(modifiedAudio.getData())[modifiedAudio.getSize() - 1] ^= 1;
        check(wavfile.replaceWithData(modifiedAudio.getData(), modifiedAudio.getSize()),
              "modify source fixture");
        const auto recovery = dir.getChildFile("changed-source.recovery.carta");
        check(carta::saveRecoverySession(saved, 1, recovery, error),
              "snapshot dirty arrangement without rehashing changed source");
        carta::Session recovered = restored;
        int recoveredChannels = 2;
        bool isRecovery = false;
        check(!carta::loadSession(recovered, recoveredChannels, formats, recovery, error, {},
                                  &isRecovery) &&
                  recoveredChannels == 2 && !isRecovery,
              "changed source blocks recovery without replacing current session");
        const auto recoveryBytes = recovery.loadFileAsString();
        auto invalidSnapshot = saved;
        invalidSnapshot.tracks[0].routing[0].left = 3.0f;
        check(!carta::saveRecoverySession(invalidSnapshot, 1, recovery, error) &&
                  recovery.loadFileAsString() == recoveryBytes,
              "invalid recovery snapshot preserves the last good snapshot");
        check(!carta::saveRecoverySession(saved, 1, recovery, error,
                                          [](double progress) { return progress < 0.99; }) &&
                  recovery.loadFileAsString() == recoveryBytes,
              "cancelled recovery snapshot preserves previous snapshot");
        const auto previousProject = project.loadFileAsString();
        check(!carta::saveSession(saved, 1, project, error) &&
                  project.loadFileAsString() == previousProject,
              "changed source prevents save and preserves existing project");
        carta::Session unchanged = restored;
        int unchangedMode = 1;
        check(!carta::loadSession(unchanged, unchangedMode, formats, project, error) &&
                  unchanged.tracks.size() == restored.tracks.size() &&
                  unchanged.tracks[0].clips[0].audio == restored.tracks[0].clips[0].audio,
              "changed source prevents project replacement");
        check(wavfile.replaceWithData(originalAudio.getData(), originalAudio.getSize()),
              "restore source fixture");
        check(carta::loadSession(unchanged, unchangedMode, formats, project, error),
              "restored source opens project");
        check(carta::loadSession(recovered, recoveredChannels, formats, recovery, error, {},
                                 &isRecovery) &&
                  isRecovery && recoveredChannels == 1 &&
                  recovered.markers.size() == saved.markers.size(),
              "recovery verifies restored source and preserves arrangement");
    }
    {
        const auto replacement = dir.getChildFile("moved-source.wav"),
                   wrong = dir.getChildFile("wrong-source.wav");
        juce::MemoryBlock bytes;
        check(wavfile.loadFileAsData(bytes) && wavfile.copyFileTo(replacement),
              "copy source to replacement location");
        auto changed = bytes;
        static_cast<unsigned char*>(changed.getData())[changed.getSize() - 1] ^= 1;
        check(wrong.replaceWithData(changed.getData(), changed.getSize()),
              "write wrong relink candidate");
        const auto recovery = dir.getChildFile("relink.recovery.carta");
        check(carta::saveRecoverySession(saved, 1, recovery, error),
              "prepare recovery relink fixture");
        check(wavfile.deleteFile(), "simulate missing original source");
        carta::Session candidate = restored;
        int candidateMode = 2;
        bool recoveredFlag = false, relinked = false;
        carta::SourceRelinkRequest request;
        check(!carta::loadSession(candidate, candidateMode, formats, project, error, {},
                                  &recoveredFlag, nullptr, &request, &relinked) &&
                  request.original == wavfile && request.expectedHash == loaded->sourceHash &&
                  candidateMode == 2 && !relinked,
              "missing source requests verified relink without replacing session");
        carta::SourceRelinks map{{wavfile.getFullPathName(), wrong}};
        check(!carta::loadSession(candidate, candidateMode, formats, project, error, {},
                                  &recoveredFlag, &map, &request, &relinked) &&
                  request.original == wavfile && !relinked &&
                  candidate.tracks[0].clips[0].audio == restored.tracks[0].clips[0].audio,
              "wrong candidate is rejected and remains relinkable");
        map[wavfile.getFullPathName()] = replacement;
        check(!carta::loadSession(
                  candidate, candidateMode, formats, project, error,
                  [](double p) { return p < 0.6; }, &recoveredFlag, &map, &request, &relinked) &&
                  candidateMode == 2 && !relinked && request.expectedHash.isEmpty(),
              "cancelled relink preserves session and does not prompt again");
        check(carta::loadSession(candidate, candidateMode, formats, project, error, {},
                                 &recoveredFlag, &map, &request, &relinked) &&
                  relinked && !recoveredFlag && candidateMode == 1 &&
                  candidate.tracks[0].clips[0].audio->file == replacement,
              "matching moved source opens and reports relink");
        const auto rewritten = dir.getChildFile("relinked.carta");
        check(carta::saveSession(candidate, 1, rewritten, error), "save relinked project");
        carta::Session reopened;
        int reopenedChannels = 2;
        bool reopenedRelink = true;
        check(carta::loadSession(reopened, reopenedChannels, formats, rewritten, error, {}, nullptr,
                                 nullptr, nullptr, &reopenedRelink) &&
                  !reopenedRelink && reopened.tracks[0].clips[0].audio->file == replacement,
              "saved project reopens using new path without relink");
        candidate = restored;
        candidateMode = 2;
        recoveredFlag = false;
        relinked = false;
        check(carta::loadSession(candidate, candidateMode, formats, recovery, error, {},
                                 &recoveredFlag, &map, &request, &relinked) &&
                  recoveredFlag && relinked,
              "recovery snapshots support verified relinking");
        juce::var multi;
        check(juce::JSON::parse(project.loadFileAsString(), multi).wasOk(),
              "parse multi-source fixture");
        juce::var extra;
        check(juce::JSON::parse(juce::JSON::toString((*multi["tracks"].getArray())[0]), extra)
                  .wasOk(),
              "clone second track");
        extra["clips"].getArray()->getReference(0).getDynamicObject()->setProperty(
            "source", "other-missing.wav");
        multi["tracks"].getArray()->add(extra);
        const auto multiPath = dir.getChildFile("multi-source.carta");
        check(multiPath.replaceWithText(juce::JSON::toString(multi)), "write multi-source fixture");
        candidate = restored;
        candidateMode = 2;
        relinked = false;
        check(!carta::loadSession(candidate, candidateMode, formats, multiPath, error, {}, nullptr,
                                  &map, &request, &relinked) &&
                  request.original == dir.getChildFile("other-missing.wav") && !relinked,
              "each missing source is requested in sequence");
        map[request.original.getFullPathName()] = replacement;
        check(carta::loadSession(candidate, candidateMode, formats, multiPath, error, {}, nullptr,
                                 &map, &request, &relinked) &&
                  relinked && candidate.tracks.size() == 2 &&
                  candidate.tracks[1].clips[0].audio->file == replacement,
              "multiple source mappings open together");
        auto extraMap = map;
        extraMap[dir.getChildFile("unrelated.wav").getFullPathName()] = replacement;
        candidate = restored;
        candidateMode = 2;
        relinked = false;
        check(!carta::loadSession(candidate, candidateMode, formats, multiPath, error, {}, nullptr,
                                  &extraMap, &request, &relinked) &&
                  error.contains("does not belong") && candidateMode == 2 && !relinked &&
                  candidate.tracks[0].clips[0].audio == restored.tracks[0].clips[0].audio,
              "an unused source mapping rejects atomically");
        juce::var legacy;
        check(juce::JSON::parse(project.loadFileAsString(), legacy).wasOk(),
              "parse legacy relink fixture");
        legacy["tracks"]
            .getArray()
            ->getReference(0)["clips"]
            .getArray()
            ->getReference(0)
            .getDynamicObject()
            ->removeProperty("sourceSha256");
        const auto legacyPath = dir.getChildFile("legacy-no-hash.carta");
        check(legacyPath.replaceWithText(juce::JSON::toString(legacy)), "write legacy fixture");
        candidate = restored;
        candidateMode = 2;
        relinked = false;
        check(!carta::loadSession(candidate, candidateMode, formats, legacyPath, error, {}, nullptr,
                                  &map, &request, &relinked) &&
                  error.contains("without source checksums") && !relinked,
              "unverifiable legacy replacement is refused");
        check(!carta::loadSession(candidate, candidateMode, formats, legacyPath, error, {}, nullptr,
                                  nullptr, &request, &relinked) &&
                  error.contains("restore the original") && request.expectedHash.isEmpty(),
              "missing legacy source explains why relinking is unavailable");
        check(replacement.copyFileTo(wavfile), "restore original source fixture");
    }
    restored.tracks[0].gain = 1;
    restored.tracks[0].pan = 0;
    restored.tracks[0].solo = false;
    check(carta::setClipEnvelope(restored.tracks[0].clips[0], 1, 0, 0),
          "reset clip envelope for master fixture");
    {
        auto source = std::make_shared<carta::Audio>();
        source->sampleRate = 44100;
        source->samples.setSize(1, 9000);
        for (int i = 0; i < 9000; ++i)
            source->samples.setSample(0, i, 0.35f * std::sin(static_cast<float>(i) * 0.123f));
        source->samples.setSample(0, 50, 1.5f);
        carta::Session rangeSession;
        rangeSession.tracks.push_back({"Range", {{source, 0, 0, 9000.0 / 44100}}});
        check(carta::setClipEnvelope(rangeSession.tracks[0].clips[0], 1, 0.0005, 0.003),
              "prepare range fade");
        const carta::WavRange entire{123, 8123}, first{123, 4321}, second{4321, 8123};
        const auto wholeFile = dir.getChildFile("range.wav"),
                   firstFile = dir.getChildFile("range-first.wav"),
                   secondFile = dir.getChildFile("range-second.wav");
        bool wasClipped = false;
        check(carta::exportWavRange(rangeSession, 2, entire, wholeFile, wasClipped, error) &&
                  !wasClipped,
              "export selected range without outside clipping");
        check(carta::exportWavRange(rangeSession, 2, first, firstFile, wasClipped, error) &&
                  carta::exportWavRange(rangeSession, 2, second, secondFile, wasClipped, error),
              "export adjacent ranges");
        const auto wholeRange = carta::loadAudio(formats, wholeFile, error),
                   part1 = carta::loadAudio(formats, firstFile, error),
                   part2 = carta::loadAudio(formats, secondFile, error);
        check(wholeRange && part1 && part2 && wholeRange->samples.getNumSamples() == 8000 &&
                  part1->samples.getNumSamples() == 4198 && part2->samples.getNumSamples() == 3802,
              "range export uses exact half-open lengths");
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 8000; ++i) {
                const auto part = i < 4198 ? part1->samples.getSample(ch, i)
                                           : part2->samples.getSample(ch, i - 4198);
                const auto combined = wholeRange->samples.getSample(ch, i);
                check(std::memcmp(&combined, &part, sizeof(float)) == 0,
                      "adjacent exports reconstruct whole selected range bit-for-bit");
            }
        juce::MemoryBlock oldRange, unchangedRange;
        check(wholeFile.loadFileAsData(oldRange), "read exported fixture");
        check(!carta::exportWavRange(rangeSession, 2, {10, 10}, wholeFile, wasClipped, error) &&
                  wholeFile.loadFileAsData(unchangedRange) && oldRange == unchangedRange,
              "invalid range preserves destination");
        check(!carta::validateWavRange(rangeSession, 2, {0, 1000000000}, error),
              "oversized classic WAV rejected before writing");
        check(carta::measurePeak(rangeSession, 2) > 1.0f &&
                  carta::measurePeak(rangeSession, 2, entire) < 1.0f,
              "range preflight ignores outside peaks");
        {
            float measured = 0;
            int peakCalls = 0;
            check(!carta::scanPeak(rangeSession, 2, entire, measured, error,
                                   [&](double progress) {
                                       ++peakCalls;
                                       return progress < 0.5;
                                   }) &&
                      peakCalls >= 3,
                  "peak scan cancels during rendering");
            const auto protectedOutput = dir.getChildFile("cancelled.wav");
            check(protectedOutput.replaceWithText("existing destination"),
                  "create cancellation fixture");
            const auto original = protectedOutput.loadFileAsString();
            int writeCalls = 0;
            juce::Array<juce::File> filesBefore;
            dir.findChildFiles(filesBefore, juce::File::findFiles, false);
            check(!carta::exportWavRange(rangeSession, 2, entire, protectedOutput, wasClipped,
                                         error, {},
                                         [&](double progress) {
                                             ++writeCalls;
                                             return progress < 0.5;
                                         }) &&
                      writeCalls >= 3 && protectedOutput.loadFileAsString() == original,
                  "cancel during write preserves destination");
            const auto absent = dir.getChildFile("cancelled-new.wav");
            check(!carta::exportWavRange(rangeSession, 2, entire, absent, wasClipped, error, {},
                                         [](double progress) { return progress < 0.999999; }) &&
                      !absent.existsAsFile(),
                  "cancel before commit creates no destination");
            check(!carta::exportWavRange(rangeSession, 2, entire, protectedOutput, wasClipped,
                                         error, {},
                                         [](double progress) { return progress < 0.999999; }) &&
                      protectedOutput.loadFileAsString() == original,
                  "cancel before commit preserves destination");
            juce::Array<juce::File> filesAfter;
            dir.findChildFiles(filesAfter, juce::File::findFiles, false);
            check(filesAfter.size() == filesBefore.size(),
                  "cancelled exports clean up temporary files");
            double last = -1;
            bool monotonic = true;
            check(carta::exportWavRange(rangeSession, 2, entire, dir.getChildFile("progress.wav"),
                                        wasClipped, error, {},
                                        [&](double progress) {
                                            if (progress < last)
                                                monotonic = false;
                                            last = progress;
                                            return true;
                                        }) &&
                      monotonic && std::abs(last - 1) < 1e-12,
                  "successful export reports monotonic completion");
        }
        auto other = std::make_shared<carta::Audio>();
        other->sampleRate = 44100;
        other->samples.setSize(1, 9000);
        for (int i = 0; i < 9000; ++i)
            other->samples.setSample(0, i, 0.2f);
        auto selectedMix = rangeSession;
        selectedMix.tracks.push_back({"Second", {{other, 0, 0, 9000.0 / 44100}}});
        {
            auto live = selectedMix;
            live.tracks[0].solo = true;
            live.tracks[1].gain = 0.7f;
            live.tracks[1].pan = -0.4f;
            auto exported = live;
            check(carta::limitToTracks(exported, 1, 1), "prepare selected-track export reference");
            for (int channels : {1, 2}) {
                juce::AudioBuffer<float> actual(2, 40), expected(2, 40);
                actual.clear();
                expected.clear();
                double pos = 123 / carta::sessionRate;
                const carta::PlaybackRange range{pos, 163 / carta::sessionRate, false, {1, 1}};
                check(!carta::renderPlayback(live, actual, 0, 40, pos, carta::sessionRate, channels,
                                             range),
                      "selected-track playback stops at exclusive end");
                exported.render(expected, 0, 40, range.start, carta::sessionRate, channels);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < 40; ++i)
                        check(near(actual.getSample(ch, i), expected.getSample(ch, i)),
                              "selected-track playback matches selected-track export mix, "
                              "including pan and excluded solo");
            }
            juce::AudioBuffer<float> looping(2, 80);
            looping.clear();
            double pos = 123 / carta::sessionRate;
            check(carta::renderPlayback(live, looping, 0, 80, pos, carta::sessionRate, 2,
                                        {pos, 163 / carta::sessionRate, true, {1, 1}}),
                  "selected-track loop remains active");
            for (int i = 0; i < 40; ++i)
                check(near(looping.getSample(0, i), looping.getSample(0, i + 40)),
                      "selected-track loop wraps without changing the mix");
            live.tracks[1].muted = true;
            looping.clear();
            pos = 123 / carta::sessionRate;
            carta::renderPlayback(live, looping, 0, 40, pos, carta::sessionRate, 2,
                                  {pos, 163 / carta::sessionRate, false, {1, 1}});
            check(near(looping.getSample(0, 0), 0), "selected-track playback honors mute");
            pos = 123 / carta::sessionRate;
            looping.clear();
            check(!carta::renderPlayback(live, looping, 0, 40, pos, carta::sessionRate, 2,
                                         {pos, 163 / carta::sessionRate, false, {1, 2}}) &&
                      near(looping.getSample(0, 0), 0),
                  "invalid selected-track playback cannot fall back to full mix");
        }
        auto invalidMix = selectedMix;
        check(!carta::limitToTracks(invalidMix, 1, 2) && !invalidMix.tracks[0].muted,
              "invalid track range leaves session unchanged");
        check(carta::limitToTracks(selectedMix, 1, 1) && selectedMix.tracks[0].muted &&
                  !selectedMix.tracks[1].muted,
              "limit mix to selected track");
        const auto selectedFile = dir.getChildFile("selected-track.wav");
        check(carta::exportWavRange(selectedMix, 2, {123, 163}, selectedFile, wasClipped, error),
              "export selected track range");
        const auto selectedWav = carta::loadAudio(formats, selectedFile, error);
        check(selectedWav && selectedWav->samples.getNumSamples() == 40 &&
                  near(selectedWav->samples.getSample(0, 12), 0.2f),
              "selected track WAV excludes other tracks");
        auto outsideSolo = rangeSession;
        outsideSolo.tracks.push_back({"Second", {{other, 0, 0, 9000.0 / 44100}}});
        outsideSolo.tracks[0].solo = true;
        check(carta::limitToTracks(outsideSolo, 1, 1) &&
                  near(carta::measurePeak(outsideSolo, 2, {123, 163}), 0.2f),
              "outside solo cannot silence the selected-track export");
        outsideSolo.tracks[1].muted = true;
        check(near(carta::measurePeak(outsideSolo, 2, {123, 163}), 0),
              "selected-track export respects selected-track mute");
        auto high = std::make_shared<carta::Audio>();
        high->sampleRate = 96000;
        high->samples.setSize(1, 18000);
        for (int i = 0; i < 18000; ++i)
            high->samples.setSample(0, i, 0.4f * std::sin(static_cast<float>(i) * 0.217f));
        carta::Session rateSession;
        rateSession.tracks.push_back({"96 kHz", {{high, 0, 0, 18000.0 / 96000}}});
        const auto a = dir.getChildFile("rate-whole.wav"), b = dir.getChildFile("rate-first.wav"),
                   c = dir.getChildFile("rate-second.wav");
        check(carta::exportWavRange(rateSession, 2, entire, a, wasClipped, error) &&
                  carta::exportWavRange(rateSession, 2, first, b, wasClipped, error) &&
                  carta::exportWavRange(rateSession, 2, second, c, wasClipped, error),
              "export adjacent resampled ranges");
        const auto fullRate = carta::loadAudio(formats, a, error),
                   firstRate = carta::loadAudio(formats, b, error),
                   secondRate = carta::loadAudio(formats, c, error);
        check(fullRate && firstRate && secondRate, "read resampled ranges");
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 8000; ++i) {
                const auto fullValue = fullRate->samples.getSample(ch, i);
                const auto partValue = i < 4198 ? firstRate->samples.getSample(ch, i)
                                                : secondRate->samples.getSample(ch, i - 4198);
                check(std::memcmp(&fullValue, &partValue, sizeof(float)) == 0,
                      "resampled adjacent exports match bit-for-bit");
            }
    }
    bool clipped = false;
    const auto mix = dir.getChildFile("master.wav");
    check(near(carta::measurePeak(restored, 2), 0.3f), "preflight measures rendered stereo peak");
    check(carta::exportWav(restored, 2, mix, clipped, error) && !clipped, "export stereo WAV");
    auto master = carta::loadAudio(formats, mix, error);
    check(master && master->samples.getNumChannels() == 2 &&
              near(master->samples.getSample(0, 30), 0.2f) &&
              near(master->samples.getSample(1, 30), 0.3f),
          "rendered stereo WAV samples");
    restored.tracks[0].gain = 0.5f;
    restored.tracks[0].pan = -0.5f;
    const auto mixedFile = dir.getChildFile("mixed.wav");
    check(carta::exportWav(restored, 2, mixedFile, clipped, error) && !clipped,
          "export track gain and balance");
    auto mixedMaster = carta::loadAudio(formats, mixedFile, error);
    check(mixedMaster && near(mixedMaster->samples.getSample(0, 30), 0.1f) &&
              near(mixedMaster->samples.getSample(1, 30), 0.075f),
          "WAV uses track mixer settings");
    restored.tracks[0].gain = 1;
    restored.tracks[0].pan = 0;
    const auto monoFile = dir.getChildFile("mono.wav");
    check(carta::exportWav(restored, 1, monoFile, clipped, error), "export mono WAV");
    auto masterMono = carta::loadAudio(formats, monoFile, error);
    check(masterMono && masterMono->samples.getNumChannels() == 1 &&
              near(masterMono->samples.getSample(0, 30), 0.25f),
          "rendered mono WAV samples");
    check(!carta::exportWav(restored, 2, wavfile, clipped, error),
          "refuse to replace audio source");
    {
        const auto protectedProject = dir.getChildFile("protected.carta");
        check(protectedProject.replaceWithText("do not replace"),
              "create protected project fixture");
        const auto before = protectedProject.loadFileAsString();
        check(!carta::exportWavRange(restored, 2, {0, 64}, protectedProject, clipped, error,
                                     protectedProject) &&
                  protectedProject.loadFileAsString() == before,
              "WAV cannot replace open project");
        check(!carta::exportTrackManifest(restored, 2, protectedProject, error, protectedProject) &&
                  protectedProject.loadFileAsString() == before,
              "manifest cannot replace open project");
        const auto projectAlias = dir.getChildFile("protected-alias.wav");
        std::error_code linkError;
        std::filesystem::create_hard_link(
            std::filesystem::u8path(protectedProject.getFullPathName().toStdString()),
            std::filesystem::u8path(projectAlias.getFullPathName().toStdString()), linkError);
        if (!linkError) {
            check(!carta::exportWavRange(restored, 2, {0, 64}, projectAlias, clipped, error,
                                         protectedProject) &&
                      protectedProject.loadFileAsString() == before,
                  "WAV cannot replace a hard link to the project");
            check(!carta::exportTrackManifest(restored, 2, projectAlias, error, protectedProject) &&
                      protectedProject.loadFileAsString() == before,
                  "manifest cannot replace a hard link to the project");
        }
        const carta::Session empty;
        const std::vector<carta::Session> history{restored}, noHistory;
        carta::Clipboard noClipboard;
        check(!carta::validateRetainedMediaDestination(empty, history, noHistory, noClipboard,
                                                       wavfile, error),
              "protect source retained by undo");
        check(!carta::validateRetainedMediaDestination(empty, noHistory, history, noClipboard,
                                                       wavfile, error),
              "protect source retained by redo");
        const auto copied = carta::copyRange(restored, {0, 64, 0, 0});
        check(!carta::validateRetainedMediaDestination(empty, noHistory, noHistory, copied, wavfile,
                                                       error),
              "protect source retained by clipboard");
        check(!carta::validateRetainedMediaDestination(empty, noHistory, noHistory, noClipboard,
                                                       wavfile, error, {nullptr, &restored}),
              "protect source retained only by a background snapshot");
        check(!carta::validateRetainedMediaDestination(empty, noHistory, noHistory, noClipboard,
                                                       wavfile, error, {}, {wavfile}),
              "protect a source currently being imported or checked");
        check(!carta::validateRetainedMediaDestination(empty, noHistory, noHistory, noClipboard,
                                                       wavfile, error,
                                                       {&empty, nullptr, &empty, &restored}),
              "protect sources in later concurrent background snapshots");
        check(carta::validateRetainedMediaDestination(empty, history, noHistory, copied,
                                                      dir.getChildFile("new-output.wav"), error),
              "allow unrelated output destination");
        check(error.isEmpty(), "successful retained-media validation clears a previous error");
        const auto sourceAlias = dir.getChildFile("source-alias.wav");
        linkError.clear();
        std::filesystem::create_hard_link(
            std::filesystem::u8path(wavfile.getFullPathName().toStdString()),
            std::filesystem::u8path(sourceAlias.getFullPathName().toStdString()), linkError);
        if (!linkError) {
            check(!carta::exportWavRange(restored, 2, {0, 64}, sourceAlias, clipped, error),
                  "WAV cannot replace a hard link to source audio");
            check(!carta::validateRetainedMediaDestination(empty, noHistory, noHistory, noClipboard,
                                                           sourceAlias, error, {&restored}),
                  "background snapshots protect hard-link aliases");
            check(!carta::validateRetainedMediaDestination(empty, noHistory, noHistory, noClipboard,
                                                           sourceAlias, error, {}, {wavfile}),
                  "pending source paths protect hard-link aliases");
        }
    }

    for (int i = 0; i < 5; ++i)
        restored.tracks.push_back(restored.tracks[0]);
    check(carta::measurePeak(restored, 2) > 1.0f, "preflight detects clipping before writing");
    check(carta::exportWav(restored, 2, dir.getChildFile("clipped.wav"), clipped, error) && clipped,
          "report clipped mix");
    restored.tracks.resize(1);
    {
        auto rawCurveAudio = std::make_shared<carta::Audio>();
        rawCurveAudio->sampleRate = carta::sessionRate;
        rawCurveAudio->samples.setSize(1, 100);
        for (int i = 0; i < 100; ++i)
            rawCurveAudio->samples.setSample(0, i, 1.0f);
        carta::Session curveSrcSession;
        curveSrcSession.tracks.push_back(
            {"Curved", {{rawCurveAudio, 0, 0, 100 / carta::sessionRate}}});
        const auto curveWav = dir.getChildFile("curve-src.wav");
        check(carta::exportWav(curveSrcSession, 1, curveWav, clipped, error),
              "export curve fixture wav");
        auto curveAudio = carta::loadAudio(formats, curveWav, error);
        check(static_cast<bool>(curveAudio), "load curve fixture wav");
        carta::Session curveSession;
        curveSession.tracks.push_back({"Curved", {{curveAudio, 0, 0, 100 / carta::sessionRate}}});
        curveSession.tracks[0].gain = 12.5f;
        curveSession.tracks[0].clips[0].gain = 15.0f;
        check(carta::setClipEnvelope(curveSession.tracks[0].clips[0], 15.0f,
                                     50 / carta::sessionRate, 25 / carta::sessionRate),
              "set clip fade durations with 15x gain");
        curveSession.tracks[0].clips[0].fadeInShape = carta::FadeShape::equalPower;
        curveSession.tracks[0].clips[0].fadeOutShape = carta::FadeShape::sCurve;
        check(near(carta::fadeCurveGain(0.5, carta::FadeShape::linear), 0.5) &&
                  std::abs(carta::fadeCurveGain(0.5, carta::FadeShape::equalPower) -
                           static_cast<float>(std::sin(3.14159265358979323846 * 0.25))) < 1e-6f &&
                  near(carta::fadeCurveGain(0.5, carta::FadeShape::sCurve), 0.5) &&
                  carta::fadeCurveGain(0.5, carta::FadeShape::exponential) < 0.5,
              "fadeCurveGain shapes produce expected curve gains");
        juce::AudioBuffer<float> curveOut(1, 100);
        curveOut.clear();
        curveSession.render(curveOut, 0, 100, 0, carta::sessionRate, 1);
        check(std::abs(curveOut.getSample(0, 25) -
                       static_cast<float>(
                           12.5 * 15.0 * carta::fadeCurveGain(0.5, carta::FadeShape::equalPower))) <
                  1e-3f,
              "rendered sample applies 16x-compatible track/clip gain and curved fade-in shape");

        const auto v24Path = dir.getChildFile("v24.carta");
        check(carta::saveSession(curveSession, 2, v24Path, error), "save v24 project");
        const auto v24Json = juce::JSON::parse(v24Path.loadFileAsString());
        check(static_cast<int>(v24Json["version"]) == 24,
              "high gain and fade curves trigger project version 24");
        carta::Session reopened24;
        int ch24 = 0;
        check(carta::loadSession(reopened24, ch24, formats, v24Path, error) &&
                  near(reopened24.tracks[0].gain, 12.5f) &&
                  near(reopened24.tracks[0].clips[0].gain, 15.0f) &&
                  reopened24.tracks[0].clips[0].fadeInShape == carta::FadeShape::equalPower &&
                  reopened24.tracks[0].clips[0].fadeOutShape == carta::FadeShape::sCurve,
              "loadSession v24 restores high gain and fade shapes");

        auto badV23 = v24Json;
        badV23.getDynamicObject()->setProperty("version", 23);
        const auto badV23Path = dir.getChildFile("bad-v23.carta");
        badV23Path.replaceWithText(juce::JSON::toString(badV23));
        check(!carta::loadSession(reopened24, ch24, formats, badV23Path, error),
              "v23 project rejects v24 features");

        carta::Session plainSession;
        plainSession.tracks.push_back({"Plain", {{curveAudio, 0, 0, 100 / carta::sessionRate}}});
        const auto plainPath = dir.getChildFile("plain-for-v24.carta");
        check(carta::saveSession(plainSession, 2, plainPath, error), "save plain baseline project");
        auto spoofV24 = juce::JSON::parse(plainPath.loadFileAsString());
        spoofV24.getDynamicObject()->setProperty("version", 24);
        const auto spoofV24Path = dir.getChildFile("spoof-v24.carta");
        spoofV24Path.replaceWithText(juce::JSON::toString(spoofV24));
        check(!carta::loadSession(reopened24, ch24, formats, spoofV24Path, error),
              "v24 project without v24 features is rejected");
    }
    {
        auto audioA = std::make_shared<carta::Audio>();
        audioA->sampleRate = carta::sessionRate;
        audioA->samples.setSize(2, 44100);
        for (int i = 0; i < 22050; ++i) {
            audioA->samples.setSample(0, i, 0.9f);
            audioA->samples.setSample(1, i, -0.9f);
        }
        for (int i = 22050; i < 44100; ++i) {
            audioA->samples.setSample(0, i, 0.25f);
            audioA->samples.setSample(1, i, -0.5f);
        }
        auto audioB = std::make_shared<carta::Audio>();
        audioB->sampleRate = carta::sessionRate;
        audioB->samples.setSize(1, 44100);
        for (int i = 0; i < 44100; ++i)
            audioB->samples.setSample(0, i, 0.25f);
        auto audioSilent = std::make_shared<carta::Audio>();
        audioSilent->sampleRate = carta::sessionRate;
        audioSilent->samples.setSize(1, 44100);
        audioSilent->samples.clear();

        carta::Clip sliceClip{audioA, 0.0, 0.5, 0.5};
        float peak = 0.0f;
        juce::String localError;
        check(carta::measureClipRawPeak(sliceClip, peak, localError) && near(peak, 0.5f),
              "measureClipRawPeak honors offset and duration");

        carta::Clip linFade{audioA, 0.0, 0.5, 0.25};
        linFade.fadeIn = 0.5;
        linFade.fadeInStart = 0.5;
        linFade.fadeInShape = carta::FadeShape::linear;
        carta::Clip expFade = linFade;
        expFade.fadeInShape = carta::FadeShape::exponential;
        float linPeak = 0.0f, expPeak = 0.0f;
        check(carta::measureClipRawPeak(linFade, linPeak, localError) &&
                  carta::measureClipRawPeak(expFade, expPeak, localError) && linPeak > 0.24f &&
                  linPeak < 0.251f && expPeak < linPeak,
              "measureClipRawPeak applies active fade curves");

        carta::Session gainSession;
        carta::Clip c0{audioA, 0.0, 0.5, 0.5};
        c0.gain = 1.0f;
        carta::Clip c1{audioB, 1.0, 0.0, 1.0};
        c1.gain = 0.5f;
        carta::Clip c2{audioSilent, 2.0, 0.0, 1.0};
        c2.gain = 2.0f;
        gainSession.tracks = {{"Main", {c0, c1, c2}}};
        const std::vector<carta::ClipReference> pairRefs{{0, 0}, {0, 1}};
        check(carta::amplifyClips(gainSession, pairRefs, 6.0205999, localError) &&
                  std::abs(gainSession.tracks[0].clips[0].gain - 2.0f) < 0.001f &&
                  std::abs(gainSession.tracks[0].clips[1].gain - 1.0f) < 0.001f,
              "amplifyClips +6 dB doubles clip gains");
        check(carta::amplifyClips(gainSession, pairRefs, -6.0205999, localError) &&
                  std::abs(gainSession.tracks[0].clips[0].gain - 1.0f) < 0.001f &&
                  std::abs(gainSession.tracks[0].clips[1].gain - 0.5f) < 0.001f,
              "amplifyClips -6 dB halves clip gains");

        gainSession.tracks[0].clips[1].gain = 9.0f;
        check(!carta::amplifyClips(gainSession, pairRefs, 6.0205999, localError) &&
                  std::abs(gainSession.tracks[0].clips[0].gain - 1.0f) < 1e-6f &&
                  std::abs(gainSession.tracks[0].clips[1].gain - 9.0f) < 1e-6f,
              "amplifyClips atomically rejects gains above 16x");
        gainSession.tracks[0].clips[1].gain = 0.5f;

        check(carta::normalizeClips(gainSession, pairRefs, -1.0, false, localError),
              "linked normalize to -1 dBFS succeeds");
        const float targetLinear = static_cast<float>(std::pow(10.0, -1.0 / 20.0));
        const float expectedGain0 = targetLinear / 0.5f;
        check(std::abs(gainSession.tracks[0].clips[0].gain - expectedGain0) < 0.001f &&
                  std::abs(gainSession.tracks[0].clips[1].gain - expectedGain0 * 0.5f) < 0.001f,
              "linked normalize preserves relative clip gain ratios");

        gainSession.tracks[0].clips[0].gain = 1.0f;
        gainSession.tracks[0].clips[1].gain = 0.5f;
        check(carta::normalizeClips(gainSession, {{0, 0}, {0, 1}, {0, 2}}, -1.0, true, localError),
              "independent normalize handles silent clip cleanly");
        check(std::abs(gainSession.tracks[0].clips[0].gain - (targetLinear / 0.5f)) < 0.001f &&
                  std::abs(gainSession.tracks[0].clips[1].gain - (targetLinear / 0.25f)) < 0.001f &&
                  std::abs(gainSession.tracks[0].clips[2].gain - 2.0f) < 1e-6f,
              "independent normalize scales each sounded clip to target peak and leaves silent "
              "clips unchanged");

        check(!carta::normalizeClips(gainSession, {{0, 2}}, -1.0, false, localError) &&
                  localError == "Selected audio is silent.",
              "normalizeClips rejects all-silent selection");

        auto quietAudio = std::make_shared<carta::Audio>();
        quietAudio->sampleRate = carta::sessionRate;
        quietAudio->samples.setSize(1, 44100);
        for (int i = 0; i < 44100; ++i)
            quietAudio->samples.setSample(0, i, 0.01f);
        carta::Clip quietClip{quietAudio, 3.0, 0.0, 1.0};
        gainSession.tracks[0].clips[0].gain = 1.0f;
        gainSession.tracks[0].clips.push_back(quietClip);
        check(!carta::normalizeClips(gainSession, {{0, 0}, {0, 3}}, 0.0, true, localError) &&
                  std::abs(gainSession.tracks[0].clips[0].gain - 1.0f) < 1e-6f &&
                  std::abs(gainSession.tracks[0].clips[3].gain - 1.0f) < 1e-6f,
              "normalizeClips atomically rejects normalizing past 16x");
    }
    check(wavfile.deleteFile(), "remove fixture");
    check(!carta::loadSession(restored, mode, formats, project, error) &&
              restored.tracks.size() == 1,
          "missing audio leaves current session intact");
    check(dir.deleteRecursively(), "remove test directory");
    std::cout << "Session and project tests passed\n";
}
