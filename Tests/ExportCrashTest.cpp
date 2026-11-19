#include "ExportCrashTest.h"
#include "Core/Session.h"
#include "IO/SessionIO.h"
#include <cstdlib>
#include <iostream>

int runExportCrashChild(const juce::File& target, bool beforeCommit, bool noReplace) {
    auto audio = std::make_shared<carta::Audio>();
    audio->sampleRate = carta::sessionRate;
    audio->samples.setSize(2, 44100);
    audio->samples.clear();
    carta::Session session;
    session.tracks.push_back({"Source", {{audio, 0, 0, 1}}});
    juce::String error;
    bool clipped = false;
    const bool ok = carta::exportWavRange(
        session, 2, {0, 44100}, target, clipped, error, {},
        [beforeCommit](double progress) {
            // _Exit skips destructors, simulating termination before temporary-file cleanup.
            if (beforeCommit ? progress > 0.9999 && progress < 1 : progress > 0 && progress < 0.9)
                std::_Exit(77);
            return true;
        },
        {carta::WavEncoding::float32, false, false}, noReplace);
    std::cerr << "Export did not reach the requested interruption point: " << error << '\n';
    return ok ? 2 : 3;
}

int runExportCrashTest() {
#if JUCE_MAC || JUCE_LINUX
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("carta-export-crash", {}, false);
    if (root.createDirectory().failed()) {
        std::cerr << "Could not create export crash-test directory.\n";
        return 1;
    }
    struct Cleanup {
        juce::File root;
        ~Cleanup() { root.deleteRecursively(); }
    } cleanup{root};
    const juce::String sentinel = "Existing master must remain intact.";
    for (bool beforeCommit : {false, true})
        for (bool noReplace : {false, true})
            for (bool exists : {false, true}) {
                const auto target =
                    root.getChildFile(juce::String(beforeCommit ? "commit" : "writing") +
                                      (noReplace ? "-exclusive" : "-replace") +
                                      (exists ? "-existing.wav" : "-new.wav"));
                if (exists && !target.replaceWithText(sentinel)) {
                    std::cerr << "Could not write destination fixture.\n";
                    return 1;
                }
                juce::ChildProcess child;
                const juce::StringArray args{
                    juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                        .getFullPathName(),
                    "--export-crash-child", target.getFullPathName(),
                    beforeCommit ? "commit" : "writing", noReplace ? "exclusive" : "replace"};
                if (!child.start(args)) {
                    std::cerr << "Could not start export crash-test child.\n";
                    return 1;
                }
                if (!child.waitForProcessToFinish(15000)) {
                    child.kill();
                    std::cerr << "Export crash-test child timed out.\n";
                    return 1;
                }
                if (child.getExitCode() != 77 ||
                    (exists ? !target.existsAsFile() || target.loadFileAsString() != sentinel
                            : target.exists())) {
                    std::cerr << "Interrupted export changed its destination; child exit "
                              << child.getExitCode() << ".\n";
                    return 1;
                }
            }
    return 0;
#else
    return 0;
#endif
}

int runProjectCrashChild(const juce::File& target, const juce::File& source,
                         const juce::String& phase, const juce::String& mode) {
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    juce::String error;
    auto audio = carta::loadAudio(formats, source, error);
    if (!audio) {
        std::cerr << "Could not load project fixture: " << error << '\n';
        return 3;
    }
    carta::Session session;
    session.tracks.push_back({"Source", {{audio, 0, 0, audio->length()}}});
    session.markers.push_back({100.0 / carta::sessionRate, "Boundary"});
    const auto progress = [&](double value) {
        // A forced exit bypasses destructors and cleanup, like abrupt termination.
        if ((phase == "prepare" && value >= 0.9) || (phase == "commit" && value >= 0.99) ||
            (phase == "after" && value >= 1.0))
            std::_Exit(77);
        return true;
    };
    const bool ok = mode == "recovery"
                        ? carta::saveRecoverySession(session, 2, target, error, progress)
                    : mode == "copy" ? carta::saveSessionCopy(session, 2, target, error, progress)
                                     : carta::saveSession(session, 2, target, error, progress);
    if (!ok && mode == "copy" && phase == "after" && target.existsAsFile() &&
        error.contains("without replacing"))
        return 78;
    std::cerr << "Project save did not reach the requested interruption point: " << error << '\n';
    return ok ? 2 : 3;
}

int runProjectCrashTest() {
#if JUCE_MAC || JUCE_LINUX
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("carta-project-crash", {}, false);
    if (root.createDirectory().failed()) {
        std::cerr << "Could not create project crash-test directory.\n";
        return 1;
    }
    struct Cleanup {
        juce::File root;
        ~Cleanup() { root.deleteRecursively(); }
    } cleanup{root};
    const auto source = root.getChildFile("source.wav");
    {
        auto out = source.createOutputStream();
        if (!out)
            return 1;
        constexpr int samples = 1024, dataBytes = samples * 2;
        out->write("RIFF", 4);
        out->writeInt(36 + dataBytes);
        out->write("WAVEfmt ", 8);
        out->writeInt(16);
        out->writeShort(1);
        out->writeShort(1);
        out->writeInt(44100);
        out->writeInt(88200);
        out->writeShort(2);
        out->writeShort(16);
        out->write("data", 4);
        out->writeInt(dataBytes);
        for (int i = 0; i < samples; ++i)
            out->writeShort(static_cast<short>((i * 97) % 20000 - 10000));
        out->flush();
        if (out->getStatus().failed())
            return 1;
    }
    juce::MemoryBlock sourceBytes;
    if (!source.loadFileAsData(sourceBytes))
        return 1;
    const juce::String sentinel = "Existing project must remain intact.";
    for (const juce::String mode : {"project", "recovery", "copy"})
        for (const juce::String phase : {"prepare", "commit", "after"})
            for (bool exists : {false, true}) {
                const auto target = root.getChildFile(mode + "-" + phase +
                                                      (exists ? "-existing.carta" : "-new.carta"));
                if (exists && !target.replaceWithText(sentinel))
                    return 1;
                juce::ChildProcess child;
                const juce::StringArray args{
                    juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                        .getFullPathName(),
                    "--project-crash-child",
                    target.getFullPathName(),
                    source.getFullPathName(),
                    phase,
                    mode};
                if (!child.start(args)) {
                    std::cerr << "Could not start project crash-test child.\n";
                    return 1;
                }
                if (!child.waitForProcessToFinish(15000)) {
                    child.kill();
                    std::cerr << "Project crash-test child timed out.\n";
                    return 1;
                }
                const bool exclusiveConflict = phase == "after" && mode == "copy" && exists;
                const bool committed = phase == "after" && !exclusiveConflict;
                if (child.getExitCode() != (exclusiveConflict ? 78 : 77)) {
                    std::cerr << "Project crash-test child exit " << child.getExitCode() << " for "
                              << mode << " " << phase << ".\n";
                    return 1;
                }
                if (committed) {
                    juce::AudioFormatManager formats;
                    formats.registerBasicFormats();
                    carta::Session reopened;
                    int channels = 0;
                    bool recovered = false;
                    juce::String error;
                    if (!carta::loadSession(reopened, channels, formats, target, error, {},
                                            &recovered) ||
                        channels != 2 || recovered != (mode == "recovery") ||
                        reopened.tracks.size() != 1 || reopened.tracks[0].clips.size() != 1 ||
                        reopened.markers.size() != 1 || reopened.markers[0].name != "Boundary" ||
                        std::abs(reopened.markers[0].position * carta::sessionRate - 100) > 1e-8 ||
                        reopened.tracks[0].clips[0].audio->file != source ||
                        reopened.tracks[0].clips[0].audio->numSamples() != 1024) {
                        std::cerr << "Committed project could not be reopened after termination: "
                                  << error << '\n';
                        return 1;
                    }
                    for (int sample = 0; sample < 1024; ++sample) {
                        const auto expected =
                            static_cast<float>((sample * 97) % 20000 - 10000) / 32768.0f;
                        if (!std::equal_to<float>{}(
                                reopened.tracks[0].clips[0].audio->channelData(0)[sample],
                                expected)) {
                            std::cerr << "Reopened project audio differs after termination.\n";
                            return 1;
                        }
                    }
                } else if (exists ? !target.existsAsFile() || target.loadFileAsString() != sentinel
                                  : target.exists()) {
                    std::cerr << "Interrupted project save changed its destination.\n";
                    return 1;
                }
                juce::MemoryBlock current;
                if (!source.loadFileAsData(current) || current != sourceBytes) {
                    std::cerr << "Project save changed source media.\n";
                    return 1;
                }
            }
    return 0;
#else
    return 0;
#endif
}
