/**
 * Persistence and restore-verification helpers for JUCE audio device state.
 *
 * Enforces a 64 KiB cap on `DEVICESETUP` XML files and performs atomic
 * replacement via `CheckedText` so corrupted settings never replace valid ones.
 */
#include "Audio/DeviceSettings.h"
#include "IO/CheckedText.h"
#include <cmath>

namespace carta {
namespace {
constexpr juce::int64 maxDeviceSettingsBytes = 64 * 1024;
} // namespace

bool loadDeviceSettings(const juce::File& file, std::unique_ptr<juce::XmlElement>& state,
                        juce::String& error) {
    error.clear();

    if (!file.exists()) {
        state.reset();
        return true;
    }

    if (!file.existsAsFile() || file.getSize() <= 0 || file.getSize() > maxDeviceSettingsBytes) {
        error = "Audio settings file is empty, too large, or not a file.";
        return false;
    }

    auto xml = juce::XmlDocument::parse(file);
    if (!xml || !xml->hasTagName("DEVICESETUP")) {
        error = "Audio settings file is invalid.";
        return false;
    }

    state = std::move(xml);
    return true;
}

bool saveDeviceSettings(const juce::File& file, const juce::XmlElement& state,
                        juce::String& error) {
    error.clear();

    const auto content = state.toString();
    if (!state.hasTagName("DEVICESETUP") || content.getNumBytesAsUTF8() > maxDeviceSettingsBytes) {
        error = "Audio settings are invalid or too large.";
        return false;
    }

    if (file.getParentDirectory().createDirectory().failed()) {
        error = "Could not create the audio settings directory.";
        return false;
    }

    auto temp = prepareCheckedText(file, content);
    if (!temp || !replaceTemporaryAtomically(*temp, file)) {
        error = "Could not save audio settings.";
        return false;
    }

    return true;
}

juce::String describeDeviceRestoreDifference(const juce::XmlElement& state,
                                             const AudioOutputSnapshot& actual) {
    if (!actual.open || actual.channels.isZero()) {
        return "No audio output is active. Check Transport > Audio output settings.";
    }

    juce::StringArray differences;
    const auto name = state.getStringAttribute("audioOutputDeviceName",
                                               state.getStringAttribute("audioDeviceName"));
    const auto type = state.getStringAttribute("deviceType");

    if (name.isNotEmpty() && name != actual.name) {
        differences.add("output device");
    }
    if (type.isNotEmpty() && type != actual.type) {
        differences.add("device type");
    }

    if (state.hasAttribute("audioDeviceRate")) {
        const auto rate = state.getDoubleAttribute("audioDeviceRate");
        if (!std::isfinite(rate) || rate <= 0 || !std::isfinite(actual.sampleRate) ||
            std::abs(rate - actual.sampleRate) > 0.01) {
            differences.add("sample rate");
        }
    }

    if (state.hasAttribute("audioDeviceBufferSize")) {
        const auto buffer = state.getIntAttribute("audioDeviceBufferSize");
        if (buffer <= 0 || buffer != actual.bufferSize) {
            differences.add("buffer size");
        }
    }

    if (state.hasAttribute("audioDeviceOutChans")) {
        juce::BigInteger requested;
        requested.parseString(state.getStringAttribute("audioDeviceOutChans"), 2);
        if (requested != actual.channels) {
            differences.add("output channels");
        }
    }

    return differences.isEmpty() ? juce::String()
                                 : "Audio settings differ: " + differences.joinIntoString(", ") +
                                       ". Check Transport > Audio output settings.";
}

} // namespace carta
