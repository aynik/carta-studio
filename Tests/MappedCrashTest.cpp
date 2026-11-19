#include "MappedCrashTest.h"
#include "Core/Session.h"
#include <cstdlib>
#include <iostream>

int runMappedCrashChild(const juce::File& source, const juce::File& temporaryDirectory) {
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    juce::String error;
    const auto audio = carta::loadAudio(
        formats, source, error,
        [](double progress) {
            // The second decoding chunk starts after PCM has been mapped/unlinked.
            if (progress > 0.20 && progress < 0.55)
                std::_Exit(77);
            return true;
        },
        0, temporaryDirectory);
    std::cerr << "Import did not reach the crash point: " << error << '\n';
    return audio ? 2 : 3;
}
int runMappedCrashTest() {
#if JUCE_MAC || JUCE_LINUX
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("carta-mapped-crash", {}, false);
    const auto pcmDirectory = root.getChildFile("pcm");
    if (pcmDirectory.createDirectory().failed()) {
        std::cerr << "Could not create crash-test directory.\n";
        return 1;
    }
    struct Cleanup {
        juce::File root;
        ~Cleanup() { root.deleteRecursively(); }
    } cleanup{root};
    const auto source = root.getChildFile("source.wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = source.createOutputStream();
        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate(44100)
                                 .withNumChannels(2)
                                 .withBitsPerSample(16);
        auto writer = wav.createWriterFor(stream, options);
        juce::AudioBuffer<float> data(2, 131072);
        data.clear();
        if (!writer || !writer->writeFromAudioSampleBuffer(data, 0, data.getNumSamples())) {
            std::cerr << "Could not create source fixture.\n";
            return 1;
        }
    }
    const auto sourceBytes = source.getSize();
    juce::String beforeHash, afterHash, error;
    if (!carta::hashSourceFile(source, beforeHash, error)) {
        std::cerr << "Could not hash source fixture.\n";
        return 1;
    }
    juce::ChildProcess child;
    const juce::StringArray args{
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
        "--mapped-crash-child", source.getFullPathName(), pcmDirectory.getFullPathName()};
    if (!child.start(args)) {
        std::cerr << "Could not start crash-test child.\n";
        return 1;
    }
    if (!child.waitForProcessToFinish(15000)) {
        child.kill();
        std::cerr << "Crash-test child timed out.\n";
        return 1;
    }
    if (child.getExitCode() != 77 || source.getSize() != sourceBytes ||
        !carta::hashSourceFile(source, afterHash, error) || beforeHash != afterHash ||
        !pcmDirectory.findChildFiles(juce::File::findFilesAndDirectories, false).isEmpty()) {
        std::cerr << "Forced import exit left named PCM or changed the source; child exit "
                  << child.getExitCode() << ".\n";
        return 1;
    }
    return 0;
#else
    return 0;
#endif
}
