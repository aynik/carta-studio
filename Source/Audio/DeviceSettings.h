/**
 * Persistence and restore-verification helpers for JUCE audio device state.
 *
 * Device setup XML (`DEVICESETUP`) is bounded in size, written atomically via
 * same-directory temporary files, and compared against the active hardware
 * output snapshot on startup to report configuration drift.
 *
 * Threading: message thread / file I/O only; never called from audio callbacks.
 */
#pragma once
#include <juce_core/juce_core.h>
#include <memory>

namespace carta {

// =========================================================================
// Device Setup XML Persistence
// =========================================================================

/**
 * Load JUCE audio device XML state from disk.
 *
 * Missing settings are valid and return a null state. Malformed or oversized
 * settings return an error without modifying the caller's state.
 */
bool loadDeviceSettings(const juce::File&, std::unique_ptr<juce::XmlElement>&, juce::String& error);

/**
 * Write a validated JUCE device state using a same-directory temporary file.
 *
 * Returns false and sets `error` without overwriting the destination if the
 * element is invalid, oversized, or cannot be committed atomically.
 */
bool saveDeviceSettings(const juce::File&, const juce::XmlElement&, juce::String& error);

// =========================================================================
// Restore Verification Snapshot
// =========================================================================

/** Summary of the currently open audio output device used for restore checks. */
struct AudioOutputSnapshot {
    bool open = false;
    juce::String name;
    juce::String type;
    double sampleRate = 0;
    int bufferSize = 0;
    juce::BigInteger channels;
};

/**
 * Describe any mismatch between saved XML settings and the active output device.
 *
 * Empty means every explicitly saved output setting matches the active device.
 */
juce::String describeDeviceRestoreDifference(const juce::XmlElement&, const AudioOutputSnapshot&);

} // namespace carta
