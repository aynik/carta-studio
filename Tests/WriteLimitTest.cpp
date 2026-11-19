#include "WriteLimitTest.h"
#include "IO/SessionIO.h"
#include "Audio/DeviceSettings.h"
#include <iostream>
#if JUCE_MAC || JUCE_LINUX
#include <csignal>
#include <sys/resource.h>
#endif

int runWriteLimitTest() {
#if JUCE_MAC || JUCE_LINUX
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getNonexistentChildFile("carta-write-limit", {}, false);
    if (dir.createDirectory().failed()) {
        std::cerr << "Could not create test directory.\n";
        return 1;
    }
    struct Cleanup {
        juce::File dir;
        ~Cleanup() { dir.deleteRecursively(); }
    } cleanup{dir};
    const auto original = juce::String("Existing destination must survive.");
    const auto target = dir.getChildFile("existing.wav");
    if (!target.replaceWithText(original)) {
        std::cerr << "Could not create destination fixture.\n";
        return 1;
    }
    auto audio = std::make_shared<carta::Audio>();
    audio->sampleRate = carta::sessionRate;
    audio->samples.setSize(2, 8192);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 8192; ++i)
            audio->samples.setSample(ch, i, 0.1f);
    carta::Session session;
    session.tracks.push_back({"Test", {{audio, 0, 0, 8192 / carta::sessionRate}}});
    const auto source = dir.getChildFile("source.wav"),
               project = dir.getChildFile("existing.carta"),
               recovery = dir.getChildFile("recovery.carta");
    const auto markers = dir.getChildFile("markers.json"),
               manifest = dir.getChildFile("manifest.json"),
               settings = dir.getChildFile("settings.xml");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = source.createOutputStream();
        auto options = juce::AudioFormatWriterOptions{}
                           .withSampleRate(carta::sessionRate)
                           .withNumChannels(2)
                           .withBitsPerSample(16);
        auto writer = wav.createWriterFor(stream, options);
        if (!writer || !writer->writeFromAudioSampleBuffer(audio->samples, 0, 8192)) {
            std::cerr << "Could not create source fixture.\n";
            return 1;
        }
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    juce::String projectError;
    const auto imported = carta::loadAudio(formats, source, projectError);
    if (!imported || !project.replaceWithText(original) || !recovery.replaceWithText(original) ||
        !markers.replaceWithText(original) || !manifest.replaceWithText(original) ||
        !settings.replaceWithText(original)) {
        std::cerr << "Could not create project fixtures.\n";
        return 1;
    }
    carta::Session projectSession;
    projectSession.tracks.push_back({"Test", {{imported, 0, 0, 8192 / carta::sessionRate}}});
    for (int i = 0; i < 64; ++i)
        projectSession.markers.push_back({static_cast<double>(i * 100) / carta::sessionRate,
                                          juce::String::repeatedString("x", 180)});
    const auto occupiedPng = dir.getChildFile("occupied.png");
    juce::String pngError;
    juce::Image tiny(juce::Image::RGB, 1, 1, true);
    if (!occupiedPng.replaceWithText(original) || carta::writeNewPng(tiny, occupiedPng, pngError) ||
        pngError.isEmpty() || occupiedPng.loadFileAsString() != original) {
        std::cerr << "PNG capture replaced an existing destination.\n";
        return 1;
    }
    const int fixtureCount = dir.findChildFiles(juce::File::findFiles, false).size();
    rlimit oldLimit{};
    if (getrlimit(RLIMIT_FSIZE, &oldLimit) != 0) {
        std::cerr << "Could not read file size limit.\n";
        return 1;
    }
    auto limited = oldLimit;
    limited.rlim_cur = std::min<rlim_t>(oldLimit.rlim_cur, 4096);
    if (std::signal(SIGXFSZ, SIG_IGN) == SIG_ERR || setrlimit(RLIMIT_FSIZE, &limited) != 0) {
        std::cerr << "Could not set file size limit.\n";
        return 1;
    }
    juce::Image capture(juce::Image::RGB, 200, 200, true);
    juce::Random random(17);
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 200; ++x)
            capture.setPixelAt(x, y,
                               juce::Colour(static_cast<juce::uint8>(random.nextInt(256)),
                                            static_cast<juce::uint8>(random.nextInt(256)),
                                            static_cast<juce::uint8>(random.nextInt(256))));
    juce::String captureError;
    const auto captureTarget = dir.getChildFile("capture.png");
    if (carta::writeNewPng(capture, captureTarget, captureError) || captureError.isEmpty() ||
        captureTarget.exists() ||
        dir.findChildFiles(juce::File::findFiles, false).size() != fixtureCount) {
        std::cerr << "A failed PNG write did not remove its partial temporary file.\n";
        return 1;
    }
    if (carta::loadAudio(formats, source, projectError, {}, 0) || projectError.isEmpty() ||
        !source.existsAsFile()) {
        std::cerr << "A failed disk-backed import did not report the write failure safely.\n";
        return 1;
    }
    juce::File partialPortable;
    juce::String portableError, sourceHash;
    if (carta::exportPortableSession(projectSession, 2, dir, partialPortable, portableError) ||
        portableError.isEmpty() || !partialPortable.isDirectory() ||
        partialPortable.getChildFile("Project.carta").exists() ||
        !partialPortable.getChildFile("Media")
             .findChildFiles(juce::File::findFiles, false)
             .isEmpty() ||
        !carta::hashSourceFile(source, sourceHash, projectError) ||
        sourceHash != imported->sourceHash) {
        std::cerr << "A short portable-media write did not preserve the source or remove its "
                     "temporary copy.\n";
        return 1;
    }
    for (const auto encoding :
         {carta::WavEncoding::pcm16, carta::WavEncoding::pcm24, carta::WavEncoding::float32}) {
        bool clipped = false;
        juce::String error;
        if (carta::exportWavRange(session, 2, {0, 8192}, target, clipped, error, {}, {},
                                  {encoding, false}) ||
            error.isEmpty() || target.loadFileAsString() != original) {
            std::cerr << "A failed write did not preserve the existing WAV destination.\n";
            return 1;
        }
        if (dir.findChildFiles(juce::File::findFiles, false).size() != fixtureCount) {
            std::cerr << "A failed write left a temporary file.\n";
            return 1;
        }
    }
    const bool projectSaved = carta::saveSession(projectSession, 2, project, projectError);
    const auto projectMessage = projectError;
    const bool projectPreserved = project.loadFileAsString() == original;
    const bool recoverySaved =
        carta::saveRecoverySession(projectSession, 2, recovery, projectError);
    const bool recoveryPreserved = recovery.loadFileAsString() == original;
    const auto remaining = dir.findChildFiles(juce::File::findFiles, false).size();
    if (projectSaved || projectMessage.isEmpty() || !projectPreserved || recoverySaved ||
        projectError.isEmpty() || !recoveryPreserved || remaining != fixtureCount) {
        std::cerr << "Project saved=" << projectSaved << " preserved=" << projectPreserved
                  << " error=" << projectMessage << "; recovery saved=" << recoverySaved
                  << " preserved=" << recoveryPreserved << " error=" << projectError
                  << "; files=" << remaining << "/" << fixtureCount << '\n';
        return 1;
    }
    juce::XmlElement device("DEVICESETUP");
    device.setAttribute("test", juce::String::repeatedString("x", 6000));
    const bool markerSaved = carta::writeMarkerFile(projectSession, markers, projectError);
    const auto markerError = projectError;
    const bool manifestSaved =
        carta::exportTrackManifest(projectSession, 2, manifest, projectError);
    const auto manifestError = projectError;
    const bool settingsSaved = carta::saveDeviceSettings(settings, device, projectError);
    if (markerSaved || markerError.isEmpty() || markers.loadFileAsString() != original ||
        manifestSaved || manifestError.isEmpty() || manifest.loadFileAsString() != original ||
        settingsSaved || projectError.isEmpty() || settings.loadFileAsString() != original ||
        dir.findChildFiles(juce::File::findFiles, false).size() != fixtureCount) {
        std::cerr << "A failed marker, manifest or settings write did not preserve its destination "
                     "or clean up.\n";
        return 1;
    }
    if (setrlimit(RLIMIT_FSIZE, &oldLimit) != 0) {
        std::cerr << "Could not restore file size limit.\n";
        return 1;
    }
    return 0;
#else
    return 0;
#endif
}
