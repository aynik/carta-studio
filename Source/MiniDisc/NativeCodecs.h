/**
 * @file NativeCodecs.h
 * Native bridge to Porffor-compiled Carta ATRAC codecs and offline JUCE audio rendering.
 *
 * Supported Codec Geometries (44.1 kHz):
 * - ATRAC1 (`carta1`): `sp-stereo` (512 samples/frame, 424 bytes = two 212-byte
 *   sound units) and `sp-mono` (512 samples/frame, 212 bytes = one sound unit),
 *   wrapped in `.aea` (2048-byte header + sound units).
 * - ATRAC3 (`carta3`): `lp2` (1024 samples/frame, 384 bytes) and `lp4` (1024
 *   samples/frame, 192 bytes), wrapped in RIFF/WAVE `.wav`.
 * - ATRAC3plus (`carta5`): `hi-sp` (2048 samples/frame, 1488 bytes) and `hi-lp`
 *   (2048 samples/frame, 376 bytes), wrapped in RIFF/WAVE `.wav`.
 *
 * Ownership & Threading:
 * - Each Porffor runtime owns a single global scratch/output buffer protected
 *   by a per-codec mutex inside `NativeCodecs.cpp`, making container/stream calls
 *   thread-safe across threads while copying output immediately out of runtime
 *   storage.
 * - `NativeAtracStreamEncoder` and `NativeAtracStreamDecoder` manage RAII
 *   stream handles (non-copyable, movable, closed on destruction).
 */

#pragma once

#include "Core/Session.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <juce_core/juce_core.h>
#include <string>
#include <vector>

namespace carta {

// ============================================================================
// Mode Metadata
// ============================================================================

/** Static geometry and container descriptor for a supported ATRAC mode. */
struct AtracModeSpec {
    const char* id = nullptr;
    const char* codec = nullptr; // "carta1", "carta3", "carta5"
    int modeCode = 0; // 0 (sp-stereo), 1 (sp-mono), 132 (lp2), 66 (lp4), 256 (hi-sp), 64 (hi-lp)
    int channels = 2;
    int sampleRate = 44100;
    int samplesPerFrame = 0;
    int bytesPerFrame = 0;  // bytes per encodeFrame call (SP stereo = 2 * 212 = 424)
    int soundUnitBytes = 0; // bytes per wire sound unit / packet
    const char* containerExtension = nullptr;
};

/** Looks up mode specification by ID or alias ("sp" -> "sp-stereo"); returns nullptr if unknown. */
const AtracModeSpec* findAtracModeSpec(const std::string& modeOrAlias) noexcept;

/** Returns a copy of all supported ATRAC mode descriptors. */
std::vector<AtracModeSpec> supportedAtracModeSpecs();

// ============================================================================
// Container Encode, Decode, & Session Rendering
// ============================================================================

/**
 * Encodes 44.1 kHz planar float PCM into an `.aea` or RIFF/WAVE ATRAC container.
 * Downmixes stereo input for `sp-mono` and duplicates mono input for stereo modes.
 * Clears `outContainer` and populates `error` on failure.
 */
bool encodeAtracContainer(const std::string& mode, const float* left, const float* right,
                          int channels, int sampleCount, std::vector<std::uint8_t>& outContainer,
                          std::string& error);

/**
 * Decodes an `.aea` or RIFF/WAVE ATRAC container into planar float PCM.
 * Clears `outLeft`/`outRight`, resets `outChannels` to 0, and populates `error` on failure.
 */
bool decodeAtracContainer(const std::string& modeOrCodec, const std::uint8_t* data,
                          std::size_t length, std::vector<float>& outLeft,
                          std::vector<float>& outRight, int& outChannels, std::string& error);

/**
 * Decodes raw frame-aligned ATRAC1/ATRAC3/ATRAC3plus payload bytes (or 16-bit big-endian stereo
 * LPCM for `"pcm"` / `"lpcm"`) into 44.1 kHz planar float PCM.
 */
bool decodeRawAtracPayload(const std::string& mode, const std::uint8_t* data, std::size_t length,
                           std::vector<float>& outLeft, std::vector<float>& outRight,
                           int& outChannels, std::string& error);

/**
 * Strips `.aea` (2048-byte) or RIFF/WAVE headers to obtain frame-aligned raw wire bytes.
 * Clears `outWireBytes` and populates `error` on failure.
 */
bool extractNetMdWirePayload(const std::string& mode, const std::vector<std::uint8_t>& container,
                             std::vector<std::uint8_t>& outWireBytes, std::string& error);

/**
 * Encodes 44.1 kHz planar float PCM frame-by-frame into raw NetMD wire bytes
 * (`sp-stereo`, `sp-mono`, `lp2`, `lp4`), matching `encodeAtracContainer` +
 * `extractNetMdWirePayload` bit-for-bit while invoking `progressCallback` with
 * `(encodedFrames, totalFrames)` periodically during encoding.
 */
bool encodeAtracNetMdWire(const std::string& mode, const float* left, const float* right,
                          int channels, int sampleCount, std::vector<std::uint8_t>& outWireBytes,
                          const std::function<void(int, int)>& progressCallback,
                          std::string& error);

/**
 * Encodes 44.1 kHz planar float PCM into raw Hi-MD track upload payload bytes:
 * - `"hi-sp"` / `"hi-lp"`: raw ATRAC3plus frames matching `encodeAtracContainer` +
 *   `extractNetMdWirePayload` bit-for-bit (without `.oma` EA3 container headers),
 * - `"lp2"` / `"lp4"`: raw ATRAC3 frames matching `encodeAtracNetMdWire`,
 * - `"pcm"` / `"lpcm"`: 16-bit signed big-endian stereo interleaved LPCM bytes.
 * Invokes `progressCallback(doneUnits, totalUnits)` periodically during encoding.
 */
bool encodeHiMdTrackPayload(const std::string& mode, const float* left, const float* right,
                            int channels, int sampleCount, std::vector<std::uint8_t>& outPayload,
                            const std::function<void(int, int)>& progressCallback,
                            std::string& error);

/**
 * Encodes `trackEndSamples.size()` contiguous tracks continuously inside a single
 * codec instance and slices frame-aligned raw Hi-MD track payloads at the cumulative
 * sample boundaries `trackEndSamples = [s_1, s_2, ..., s_K]` (`0 < s_1 < ... < s_K`).
 * When `trackEndSamples.size() == 1`, delegates directly to `encodeHiMdTrackPayload`.
 * For `K >= 2`, concatenating `outTrackPayloads[0..K-1]` equals the continuous single-stream
 * encoding of `[0, s_K)` bit-for-bit (preserving MDCT overlap across track boundaries).
 */
bool encodeHiMdContinuousTracks(const std::string& mode, const float* left, const float* right,
                                int channels, const std::vector<int>& trackEndSamples,
                                std::vector<std::vector<std::uint8_t>>& outTrackPayloads,
                                const std::function<void(int, int)>& progressCallback,
                                std::string& error);

/**
 * Renders a half-open session timeline range `[startSample, endSample)` at 44.1 kHz.
 * Clears output vectors and populates `error` on invalid geometry or unsupported sample rate.
 */
bool renderSessionMixRange(const Session& session, std::int64_t startSample, std::int64_t endSample,
                           int targetChannels, std::vector<float>& outLeft,
                           std::vector<float>& outRight, std::string& error);

/**
 * Reads an audio file via JUCE, resamples to 44.1 kHz if needed, and encodes to ATRAC container.
 * Resets `samplesOut`/`bytesOut` to 0 and populates `error` on failure.
 */
bool encodeWavFileToAtrac(const std::string& mode, const juce::File& inputWav,
                          const juce::File& outputFile, std::int64_t& samplesOut,
                          std::int64_t& bytesOut, std::string& error);

// ============================================================================
// Frame-Streaming Codecs
// ============================================================================

/** RAII wrapper over a Porffor streaming ATRAC encoder handle. */
class NativeAtracStreamEncoder final {
  public:
    NativeAtracStreamEncoder() = default;
    ~NativeAtracStreamEncoder();
    NativeAtracStreamEncoder(const NativeAtracStreamEncoder&) = delete;
    NativeAtracStreamEncoder& operator=(const NativeAtracStreamEncoder&) = delete;
    NativeAtracStreamEncoder(NativeAtracStreamEncoder&& other) noexcept;
    NativeAtracStreamEncoder& operator=(NativeAtracStreamEncoder&& other) noexcept;

    /** Opens an encoder stream for `mode` (closing any existing stream first). */
    bool open(const std::string& mode, std::string& error);

    /** Releases the underlying Porffor stream handle if open. */
    void close();

    bool isOpen() const noexcept { return handle >= 0 && spec != nullptr; }
    const AtracModeSpec* modeSpec() const noexcept { return spec; }

    /**
     * Encodes one `spec->samplesPerFrame` frame into `dst` (at least `spec->bytesPerFrame` bytes).
     * Returns written byte count on success, or -1 with `error` set on failure.
     */
    int encodeFrame(const float* left, const float* right, std::uint8_t* dst,
                    std::size_t dstCapacity, std::string& error);

  private:
    const AtracModeSpec* spec = nullptr;
    int handle = -1;
};

/** RAII wrapper over a Porffor streaming ATRAC decoder handle. */
class NativeAtracStreamDecoder final {
  public:
    NativeAtracStreamDecoder() = default;
    ~NativeAtracStreamDecoder();
    NativeAtracStreamDecoder(const NativeAtracStreamDecoder&) = delete;
    NativeAtracStreamDecoder& operator=(const NativeAtracStreamDecoder&) = delete;
    NativeAtracStreamDecoder(NativeAtracStreamDecoder&& other) noexcept;
    NativeAtracStreamDecoder& operator=(NativeAtracStreamDecoder&& other) noexcept;

    /** Opens a decoder stream for `mode` (closing any existing stream first). */
    bool open(const std::string& mode, std::string& error);

    /** Releases the underlying Porffor stream handle if open. */
    void close();

    bool isOpen() const noexcept { return handle >= 0 && spec != nullptr; }
    const AtracModeSpec* modeSpec() const noexcept { return spec; }

    /**
     * Decodes one frame (`srcLen >= spec->bytesPerFrame`) into planar `left`/`right` buffers.
     * Returns decoded sample count per channel on success, or -1 with `error` set on failure.
     */
    int decodeFrame(const std::uint8_t* src, std::size_t srcLen, float* left, float* right,
                    std::string& error);

  private:
    const AtracModeSpec* spec = nullptr;
    int handle = -1;
};

} // namespace carta
