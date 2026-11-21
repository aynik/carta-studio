/**
 * @file NativeCodecs.cpp
 * Implementation of the Porffor ATRAC1/3/3plus native bridge and JUCE mix rendering.
 *
 * Porffor Runtime ABI Invariants:
 * - Each compiled runtime (`carta1`, `carta3`, `carta5`) exposes C entry points
 *   operating on global module scratch memory (`*_pf_output_ptr`,
 *   `*_pf_output_len`, `*_pf_last_error`). All calls for a given runtime acquire
 *   its `CodecRuntimeOps::mutex` and copy results before releasing the lock.
 * - `extractNetMdWirePayload` strips the 2,048-byte `.aea` header for SP
 *   modes or walks RIFF/WAVE chunks to extract the frame-aligned `data` chunk
 *   for LP2/LP4/Hi-MD modes.
 */

#include "MiniDisc/NativeCodecs.h"
#include "Audio/Resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>
#include <mutex>

// ============================================================================
// Porffor Runtime C ABI Symbols
// ============================================================================

extern "C" {
int carta1_pf_create_stream(int is_encoder, int mode_code);
int carta1_pf_destroy_stream(int handle);
int carta1_pf_encode_frame(int handle, const float* left_ptr, const float* right_ptr,
                           unsigned char* dst_ptr);
int carta1_pf_decode_frame(int handle, const unsigned char* src_ptr, int src_len, float* left_ptr,
                           float* right_ptr);
int carta1_pf_encode_container(int mode_code, int channels, const float* left_ptr,
                               const float* right_ptr, int sample_count);
int carta1_pf_decode_container(const unsigned char* data_ptr, int data_len);
const unsigned char* carta1_pf_output_ptr(void);
std::size_t carta1_pf_output_len(void);
int carta1_pf_output_channels(void);
const char* carta1_pf_last_error(void);
void carta1_pf_release_scratch(void);

int carta3_pf_create_stream(int is_encoder, int mode_code);
int carta3_pf_destroy_stream(int handle);
int carta3_pf_encode_frame(int handle, const float* left_ptr, const float* right_ptr,
                           unsigned char* dst_ptr);
int carta3_pf_decode_frame(int handle, const unsigned char* src_ptr, int src_len, float* left_ptr,
                           float* right_ptr);
int carta3_pf_encode_container(int mode_code, int channels, const float* left_ptr,
                               const float* right_ptr, int sample_count);
int carta3_pf_decode_container(const unsigned char* data_ptr, int data_len);
const unsigned char* carta3_pf_output_ptr(void);
std::size_t carta3_pf_output_len(void);
int carta3_pf_output_channels(void);
const char* carta3_pf_last_error(void);
void carta3_pf_release_scratch(void);

int carta5_pf_create_stream(int is_encoder, int mode_code);
int carta5_pf_destroy_stream(int handle);
int carta5_pf_encode_frame(int handle, const float* left_ptr, const float* right_ptr,
                           unsigned char* dst_ptr);
int carta5_pf_decode_frame(int handle, const unsigned char* src_ptr, int src_len, float* left_ptr,
                           float* right_ptr);
int carta5_pf_encode_container(int mode_code, int channels, const float* left_ptr,
                               const float* right_ptr, int sample_count);
int carta5_pf_decode_container(const unsigned char* data_ptr, int data_len);
const unsigned char* carta5_pf_output_ptr(void);
std::size_t carta5_pf_output_len(void);
int carta5_pf_output_channels(void);
const char* carta5_pf_last_error(void);
void carta5_pf_release_scratch(void);
} // extern "C"

namespace carta {
namespace {

// ============================================================================
// Internal Runtime Dispatch & Parsing Helpers
// ============================================================================

constexpr AtracModeSpec kModeSpecs[] = {
    {"sp-stereo", "carta1", 0, 2, 44100, 512, 424, 212, "aea"},
    {"sp-mono", "carta1", 1, 1, 44100, 512, 212, 212, "aea"},
    {"lp2", "carta3", 132, 2, 44100, 1024, 384, 384, "wav"},
    {"lp4", "carta3", 66, 2, 44100, 1024, 192, 192, "wav"},
    {"hi-sp", "carta5", 256, 2, 44100, 2048, 1488, 1488, "wav"},
    {"hi-lp", "carta5", 64, 2, 44100, 2048, 376, 376, "wav"},
};

struct CodecRuntimeOps {
    std::mutex mutex;
    int (*createStream)(int, int);
    int (*destroyStream)(int);
    int (*encodeFrame)(int, const float*, const float*, unsigned char*);
    int (*decodeFrame)(int, const unsigned char*, int, float*, float*);
    int (*encodeContainer)(int, int, const float*, const float*, int);
    int (*decodeContainer)(const unsigned char*, int);
    const unsigned char* (*outputPtr)(void);
    std::size_t (*outputLen)(void);
    int (*outputChannels)(void);
    const char* (*lastError)(void);
};

CodecRuntimeOps& runtimeForCodec(const std::string& codec) {
    static CodecRuntimeOps carta1Ops{
        {},
        &carta1_pf_create_stream,
        &carta1_pf_destroy_stream,
        &carta1_pf_encode_frame,
        &carta1_pf_decode_frame,
        &carta1_pf_encode_container,
        &carta1_pf_decode_container,
        &carta1_pf_output_ptr,
        &carta1_pf_output_len,
        &carta1_pf_output_channels,
        &carta1_pf_last_error,
    };
    static CodecRuntimeOps carta3Ops{
        {},
        &carta3_pf_create_stream,
        &carta3_pf_destroy_stream,
        &carta3_pf_encode_frame,
        &carta3_pf_decode_frame,
        &carta3_pf_encode_container,
        &carta3_pf_decode_container,
        &carta3_pf_output_ptr,
        &carta3_pf_output_len,
        &carta3_pf_output_channels,
        &carta3_pf_last_error,
    };
    static CodecRuntimeOps carta5Ops{
        {},
        &carta5_pf_create_stream,
        &carta5_pf_destroy_stream,
        &carta5_pf_encode_frame,
        &carta5_pf_decode_frame,
        &carta5_pf_encode_container,
        &carta5_pf_decode_container,
        &carta5_pf_output_ptr,
        &carta5_pf_output_len,
        &carta5_pf_output_channels,
        &carta5_pf_last_error,
    };
    if (codec == "carta1") {
        return carta1Ops;
    }
    if (codec == "carta3") {
        return carta3Ops;
    }
    return carta5Ops;
}

std::string normalizeAsciiLower(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

std::uint32_t readLe32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string codecErrorText(CodecRuntimeOps& ops, const char* fallback) {
    const char* raw = ops.lastError ? ops.lastError() : nullptr;
    if (raw != nullptr && raw[0] != '\0') {
        return std::string(raw);
    }
    return std::string(fallback);
}

const char* codecNameFromDecodeArg(const std::string& modeOrCodec) {
    const auto lower = normalizeAsciiLower(modeOrCodec);
    if (lower == "carta1" || lower == "atrac1") {
        return "carta1";
    }
    if (lower == "carta3" || lower == "atrac3") {
        return "carta3";
    }
    if (lower == "carta5" || lower == "atrac3plus" || lower == "atracx") {
        return "carta5";
    }
    if (const auto* spec = findAtracModeSpec(lower)) {
        return spec->codec;
    }
    return nullptr;
}

} // namespace

// ============================================================================
// Mode Lookup & Container Encode / Decode
// ============================================================================

const AtracModeSpec* findAtracModeSpec(const std::string& modeOrAlias) noexcept {
    const auto lower = normalizeAsciiLower(modeOrAlias);
    for (const auto& spec : kModeSpecs) {
        if (lower == spec.id) {
            return &spec;
        }
    }
    if (lower == "sp") {
        return &kModeSpecs[0];
    }
    return nullptr;
}

std::vector<AtracModeSpec> supportedAtracModeSpecs() {
    return std::vector<AtracModeSpec>(std::begin(kModeSpecs), std::end(kModeSpecs));
}

bool encodeAtracContainer(const std::string& mode, const float* left, const float* right,
                          int channels, int sampleCount, std::vector<std::uint8_t>& outContainer,
                          std::string& error) {
    outContainer.clear();
    const auto* spec = findAtracModeSpec(mode);
    if (!spec) {
        error = "Unsupported ATRAC mode '" + mode +
                "' (expected sp-stereo, sp-mono, lp2, lp4, hi-sp, or hi-lp).";
        return false;
    }
    if (channels < 1 || channels > 2 || sampleCount < 0 || (sampleCount > 0 && left == nullptr)) {
        error = "Invalid PCM input geometry for ATRAC encoding.";
        return false;
    }

    std::vector<float> downmixedMono;
    const float* passLeft = left;
    const float* passRight = right ? right : left;
    int passChannels = spec->channels;

    if (spec->channels == 1) {
        if (channels == 2 && right != nullptr && sampleCount > 0) {
            downmixedMono.resize(static_cast<std::size_t>(sampleCount));
            for (int i = 0; i < sampleCount; ++i) {
                downmixedMono[static_cast<std::size_t>(i)] = 0.5f * (left[i] + right[i]);
            }
            passLeft = downmixedMono.data();
        }
        passRight = nullptr;
        passChannels = 1;
    } else {
        if (channels == 1 || right == nullptr) {
            passRight = left;
        }
        passChannels = 2;
    }

    auto& ops = runtimeForCodec(spec->codec);
    std::lock_guard<std::mutex> guard(ops.mutex);
    const int rc =
        ops.encodeContainer(spec->modeCode, passChannels, passLeft, passRight, sampleCount);
    if (rc < 0) {
        error = codecErrorText(ops, "Porffor ATRAC container encode failed.");
        return false;
    }
    const auto* ptr = ops.outputPtr();
    const auto len = ops.outputLen();
    if (len > 0 && ptr == nullptr) {
        error = "Porffor ATRAC runtime returned null output pointer.";
        return false;
    }
    outContainer.assign(ptr, ptr + len);
    error.clear();
    return true;
}

bool decodeAtracContainer(const std::string& modeOrCodec, const std::uint8_t* data,
                          std::size_t length, std::vector<float>& outLeft,
                          std::vector<float>& outRight, int& outChannels, std::string& error) {
    outLeft.clear();
    outRight.clear();
    outChannels = 0;

    const char* codec = codecNameFromDecodeArg(modeOrCodec);
    if (!codec) {
        error = "Unsupported ATRAC codec or mode '" + modeOrCodec + "' for decode.";
        return false;
    }
    if (data == nullptr && length > 0) {
        error = "Null ATRAC container buffer.";
        return false;
    }

    auto& ops = runtimeForCodec(codec);
    std::lock_guard<std::mutex> guard(ops.mutex);
    const int samples = ops.decodeContainer(data, static_cast<int>(length));
    if (samples < 0) {
        error = codecErrorText(ops, "Porffor ATRAC container decode failed.");
        return false;
    }
    const int channels = ops.outputChannels();
    if (channels < 1 || channels > 2) {
        error = "Unexpected decoded channel count from Porffor runtime.";
        return false;
    }
    const auto* rawPtr = ops.outputPtr();
    const auto byteLen = ops.outputLen();
    const std::size_t expectedBytes =
        static_cast<std::size_t>(samples) * static_cast<std::size_t>(channels) * sizeof(float);
    if (byteLen != expectedBytes || (byteLen > 0 && rawPtr == nullptr)) {
        error = "Porffor ATRAC decoded PCM length mismatch.";
        return false;
    }

    outLeft.resize(static_cast<std::size_t>(samples));
    if (samples > 0) {
        std::memcpy(outLeft.data(), rawPtr, static_cast<std::size_t>(samples) * sizeof(float));
    }
    if (channels == 2) {
        outRight.resize(static_cast<std::size_t>(samples));
        if (samples > 0) {
            std::memcpy(outRight.data(), rawPtr + static_cast<std::size_t>(samples) * sizeof(float),
                        static_cast<std::size_t>(samples) * sizeof(float));
        }
    }
    outChannels = channels;
    error.clear();
    return true;
}

bool extractNetMdWirePayload(const std::string& mode, const std::vector<std::uint8_t>& container,
                             std::vector<std::uint8_t>& outWireBytes, std::string& error) {
    outWireBytes.clear();
    const auto* spec = findAtracModeSpec(mode);
    if (!spec) {
        error = "Unsupported ATRAC mode '" + mode + "'.";
        return false;
    }

    if (std::strcmp(spec->codec, "carta1") == 0) {
        constexpr std::size_t kAeaHeaderBytes = 2048;
        if (container.size() < kAeaHeaderBytes) {
            error = "AEA container is smaller than the 2048-byte header.";
            return false;
        }
        const std::size_t payloadBytes = container.size() - kAeaHeaderBytes;
        if ((payloadBytes % static_cast<std::size_t>(spec->bytesPerFrame)) != 0) {
            error = "AEA sound-unit payload size is not aligned to ATRAC1 frame boundaries.";
            return false;
        }
        outWireBytes.assign(container.begin() + static_cast<std::ptrdiff_t>(kAeaHeaderBytes),
                            container.end());
        error.clear();
        return true;
    }

    if (container.size() < 12 || std::memcmp(container.data(), "RIFF", 4) != 0 ||
        std::memcmp(container.data() + 8, "WAVE", 4) != 0) {
        error = "Expected RIFF/WAVE container for " + std::string(spec->id) + ".";
        return false;
    }

    std::size_t offset = 12;
    while (offset + 8 <= container.size()) {
        const std::uint8_t* hdr = container.data() + offset;
        const std::uint32_t chunkSize = readLe32(hdr + 4);
        const std::size_t payloadOffset = offset + 8;
        if (payloadOffset + chunkSize > container.size()) {
            error = "Truncated RIFF chunk inside ATRAC WAVE container.";
            return false;
        }
        if (std::memcmp(hdr, "data", 4) == 0) {
            if (spec->bytesPerFrame > 0 &&
                (chunkSize % static_cast<std::uint32_t>(spec->bytesPerFrame)) != 0) {
                error = "ATRAC WAVE data chunk is not aligned to frame size.";
                return false;
            }
            outWireBytes.assign(container.begin() + static_cast<std::ptrdiff_t>(payloadOffset),
                                container.begin() +
                                    static_cast<std::ptrdiff_t>(payloadOffset + chunkSize));
            error.clear();
            return true;
        }
        const std::size_t paddedSize = static_cast<std::size_t>(chunkSize) + (chunkSize & 1u);
        if (payloadOffset + paddedSize <= offset) {
            error = "Invalid RIFF chunk advance.";
            return false;
        }
        offset = payloadOffset + paddedSize;
    }

    error = "ATRAC WAVE container does not contain a 'data' chunk.";
    return false;
}

bool encodeAtracNetMdWire(const std::string& mode, const float* left, const float* right,
                          int channels, int sampleCount, std::vector<std::uint8_t>& outWireBytes,
                          const std::function<void(int, int)>& progressCallback,
                          std::string& error) {
    outWireBytes.clear();
    const auto* spec = findAtracModeSpec(mode);
    if (!spec ||
        (std::strcmp(spec->codec, "carta1") != 0 && std::strcmp(spec->codec, "carta3") != 0)) {
        error = "Unsupported NetMD ATRAC mode '" + mode +
                "' (expected sp-stereo, sp-mono, lp2, or lp4).";
        return false;
    }
    if (channels < 1 || channels > 2 || sampleCount < 0 || (sampleCount > 0 && left == nullptr)) {
        error = "Invalid PCM input geometry for ATRAC NetMD encoding.";
        return false;
    }
    if (sampleCount == 0) {
        if (progressCallback) {
            progressCallback(0, 0);
        }
        error.clear();
        return true;
    }

    std::vector<float> downmixedMono;
    const float* passLeft = left;
    const float* passRight = right ? right : left;
    if (spec->channels == 1) {
        if (channels == 2 && right != nullptr) {
            downmixedMono.resize(static_cast<std::size_t>(sampleCount));
            for (int i = 0; i < sampleCount; ++i) {
                downmixedMono[static_cast<std::size_t>(i)] = 0.5f * (left[i] + right[i]);
            }
            passLeft = downmixedMono.data();
        }
        passRight = nullptr;
    } else if (channels == 1 || right == nullptr) {
        passRight = left;
    }

    NativeAtracStreamEncoder encoder;
    if (!encoder.open(spec->id, error)) {
        return false;
    }

    const int frameSamples = spec->samplesPerFrame;
    const int frameBytes = spec->bytesPerFrame;
    std::vector<float> frameL(static_cast<std::size_t>(frameSamples), 0.0f);
    std::vector<float> frameR(static_cast<std::size_t>(frameSamples), 0.0f);
    std::vector<std::uint8_t> frameOut(static_cast<std::size_t>(frameBytes), 0);

    auto reportStep = [&](int done, int total, bool force) {
        if (progressCallback && (force || (done % 16) == 0)) {
            progressCallback(done, total);
        }
    };

    if (std::strcmp(spec->codec, "carta1") == 0) {
        constexpr int kFlushTailFrames = 1;
        const int pcmFrames = (sampleCount + frameSamples - 1) / frameSamples;
        const int totalFrames = pcmFrames + kFlushTailFrames;
        outWireBytes.reserve(static_cast<std::size_t>(totalFrames) *
                             static_cast<std::size_t>(frameBytes));
        reportStep(0, totalFrames, true);

        int encodedCount = 0;
        for (int pos = 0; pos < sampleCount; pos += frameSamples) {
            std::fill(frameL.begin(), frameL.end(), 0.0f);
            std::fill(frameR.begin(), frameR.end(), 0.0f);
            const int take = std::min(frameSamples, sampleCount - pos);
            std::memcpy(frameL.data(), passLeft + pos,
                        static_cast<std::size_t>(take) * sizeof(float));
            if (spec->channels == 2) {
                std::memcpy(frameR.data(), passRight + pos,
                            static_cast<std::size_t>(take) * sizeof(float));
            }
            const int n =
                encoder.encodeFrame(frameL.data(), spec->channels == 2 ? frameR.data() : nullptr,
                                    frameOut.data(), frameOut.size(), error);
            if (n < 0) {
                outWireBytes.clear();
                return false;
            }
            outWireBytes.insert(outWireBytes.end(), frameOut.begin(), frameOut.begin() + n);
            ++encodedCount;
            reportStep(encodedCount, totalFrames, false);
        }

        std::fill(frameL.begin(), frameL.end(), 0.0f);
        std::fill(frameR.begin(), frameR.end(), 0.0f);
        for (int f = 0; f < kFlushTailFrames; ++f) {
            const int n =
                encoder.encodeFrame(frameL.data(), spec->channels == 2 ? frameR.data() : nullptr,
                                    frameOut.data(), frameOut.size(), error);
            if (n < 0) {
                outWireBytes.clear();
                return false;
            }
            outWireBytes.insert(outWireBytes.end(), frameOut.begin(), frameOut.begin() + n);
            ++encodedCount;
        }
        reportStep(encodedCount, totalFrames, true);
    } else {
        // Replicate WaveStreamingEncoder lead-in (69 samples), first-body-frame drop, and 3 drain
        // frames so wire bytes match `encodeWavePcm` bit-for-bit.
        constexpr int kEncoderLeadSamples = 69;
        constexpr int kDrainFrames = 3;
        const int lead = std::min(sampleCount, kEncoderLeadSamples);
        const int mainCalls =
            (sampleCount > lead) ? ((sampleCount - lead + frameSamples - 1) / frameSamples) : 0;
        const int totalFrames = 1 + mainCalls + kDrainFrames;
        outWireBytes.reserve(static_cast<std::size_t>(totalFrames) *
                             static_cast<std::size_t>(frameBytes));
        reportStep(0, totalFrames, true);

        int encodedCount = 0;
        std::fill(frameL.begin(), frameL.end(), 0.0f);
        std::fill(frameR.begin(), frameR.end(), 0.0f);
        const std::size_t primeDst = static_cast<std::size_t>(frameSamples - kEncoderLeadSamples);
        std::memcpy(frameL.data() + primeDst, passLeft,
                    static_cast<std::size_t>(lead) * sizeof(float));
        std::memcpy(frameR.data() + primeDst, passRight,
                    static_cast<std::size_t>(lead) * sizeof(float));
        int n = encoder.encodeFrame(frameL.data(), frameR.data(), frameOut.data(), frameOut.size(),
                                    error);
        if (n < 0) {
            outWireBytes.clear();
            return false;
        }
        outWireBytes.insert(outWireBytes.end(), frameOut.begin(), frameOut.begin() + n);
        ++encodedCount;
        reportStep(encodedCount, totalFrames, false);

        bool dropNextPcmFrame = true;
        for (int pos = lead; pos < sampleCount; pos += frameSamples) {
            std::fill(frameL.begin(), frameL.end(), 0.0f);
            std::fill(frameR.begin(), frameR.end(), 0.0f);
            const int take = std::min(frameSamples, sampleCount - pos);
            std::memcpy(frameL.data(), passLeft + pos,
                        static_cast<std::size_t>(take) * sizeof(float));
            std::memcpy(frameR.data(), passRight + pos,
                        static_cast<std::size_t>(take) * sizeof(float));
            n = encoder.encodeFrame(frameL.data(), frameR.data(), frameOut.data(), frameOut.size(),
                                    error);
            if (n < 0) {
                outWireBytes.clear();
                return false;
            }
            if (dropNextPcmFrame) {
                dropNextPcmFrame = false;
            } else {
                outWireBytes.insert(outWireBytes.end(), frameOut.begin(), frameOut.begin() + n);
            }
            ++encodedCount;
            reportStep(encodedCount, totalFrames, false);
        }

        std::fill(frameL.begin(), frameL.end(), 0.0f);
        std::fill(frameR.begin(), frameR.end(), 0.0f);
        for (int d = 0; d < kDrainFrames; ++d) {
            n = encoder.encodeFrame(frameL.data(), frameR.data(), frameOut.data(), frameOut.size(),
                                    error);
            if (n < 0) {
                outWireBytes.clear();
                return false;
            }
            outWireBytes.insert(outWireBytes.end(), frameOut.begin(), frameOut.begin() + n);
            ++encodedCount;
        }
        reportStep(encodedCount, totalFrames, true);
    }

    error.clear();
    return true;
}

bool encodeHiMdTrackPayload(const std::string& mode, const float* left, const float* right,
                            int channels, int sampleCount, std::vector<std::uint8_t>& outPayload,
                            const std::function<void(int, int)>& progressCallback,
                            std::string& error) {
    outPayload.clear();
    std::string normalized = normalizeAsciiLower(mode);
    if (normalized == "hisp") {
        normalized = "hi-sp";
    } else if (normalized == "hilp") {
        normalized = "hi-lp";
    }

    if (normalized == "pcm" || normalized == "lpcm") {
        if (channels < 1 || channels > 2 || sampleCount < 0 ||
            (sampleCount > 0 && left == nullptr)) {
            error = "Invalid PCM input geometry for Hi-MD LPCM encoding.";
            return false;
        }
        if (sampleCount == 0) {
            if (progressCallback) {
                progressCallback(0, 0);
            }
            error.clear();
            return true;
        }
        const float* passRight = (channels == 2 && right != nullptr) ? right : left;
        outPayload.reserve(static_cast<std::size_t>(sampleCount) * 4u);
        if (progressCallback) {
            progressCallback(0, sampleCount);
        }
        auto quantizeS16 = [](float sample) -> std::int16_t {
            const float clamped = std::max(-1.0f, std::min(1.0f, sample));
            const long rounded = std::lround(static_cast<double>(clamped) * 32767.0);
            const long bounded = std::max(-32768L, std::min(32767L, rounded));
            return static_cast<std::int16_t>(bounded);
        };
        for (int i = 0; i < sampleCount; ++i) {
            const float sL = left[i];
            const float sR = passRight[i];
            if (!std::isfinite(sL) || !std::isfinite(sR)) {
                outPayload.clear();
                error = "PCM audio contains a non-finite sample.";
                return false;
            }
            const auto uL = static_cast<std::uint16_t>(quantizeS16(sL));
            const auto uR = static_cast<std::uint16_t>(quantizeS16(sR));
            outPayload.push_back(static_cast<std::uint8_t>((uL >> 8) & 0xFFu));
            outPayload.push_back(static_cast<std::uint8_t>(uL & 0xFFu));
            outPayload.push_back(static_cast<std::uint8_t>((uR >> 8) & 0xFFu));
            outPayload.push_back(static_cast<std::uint8_t>(uR & 0xFFu));
            if (progressCallback && ((i + 1) % 16384) == 0 && (i + 1) < sampleCount) {
                progressCallback(i + 1, sampleCount);
            }
        }
        if (progressCallback) {
            progressCallback(sampleCount, sampleCount);
        }
        error.clear();
        return true;
    }

    if (normalized == "lp2" || normalized == "lp4") {
        return encodeAtracNetMdWire(normalized, left, right, channels, sampleCount, outPayload,
                                    progressCallback, error);
    }

    const auto* spec = findAtracModeSpec(normalized);
    if (!spec || std::strcmp(spec->codec, "carta5") != 0) {
        error = "Unsupported Hi-MD codec mode '" + mode +
                "' (expected hi-sp, hi-lp, lp2, lp4, or pcm).";
        return false;
    }
    if (channels < 1 || channels > 2 || sampleCount < 0 || (sampleCount > 0 && left == nullptr)) {
        error = "Invalid PCM input geometry for Hi-MD ATRAC3plus encoding.";
        return false;
    }
    if (sampleCount == 0) {
        if (progressCallback) {
            progressCallback(0, 0);
        }
        error.clear();
        return true;
    }

    const float* passRight = (channels == 2 && right != nullptr) ? right : left;
    NativeAtracStreamEncoder encoder;
    if (!encoder.open(spec->id, error)) {
        return false;
    }

    constexpr int kCarta5DelayFrames = 7;
    constexpr int kCarta5OverlapSamples = 368;
    const int frameSamples = spec->samplesPerFrame;
    const int frameBytes = spec->bytesPerFrame;
    const int remainder = sampleCount % frameSamples;
    const int lastFrameSamples = (remainder == 0) ? frameSamples : remainder;
    const int flushTailFrames =
        kCarta5DelayFrames +
        (lastFrameSamples + kCarta5OverlapSamples + frameSamples - 1) / frameSamples;
    const int pcmFrames = (sampleCount + frameSamples - 1) / frameSamples;
    const int totalCalls = pcmFrames + flushTailFrames;

    outPayload.reserve(static_cast<std::size_t>(pcmFrames + flushTailFrames - kCarta5DelayFrames) *
                       static_cast<std::size_t>(frameBytes));

    std::vector<float> frameL(static_cast<std::size_t>(frameSamples), 0.0f);
    std::vector<float> frameR(static_cast<std::size_t>(frameSamples), 0.0f);
    std::vector<std::uint8_t> frameOut(static_cast<std::size_t>(frameBytes), 0);

    auto reportStep = [&](int done, int total, bool force) {
        if (progressCallback && (force || (done % 16) == 0)) {
            progressCallback(done, total);
        }
    };
    reportStep(0, totalCalls, true);

    int encodedCount = 0;
    for (int pos = 0; pos < sampleCount; pos += frameSamples) {
        std::fill(frameL.begin(), frameL.end(), 0.0f);
        std::fill(frameR.begin(), frameR.end(), 0.0f);
        const int take = std::min(frameSamples, sampleCount - pos);
        std::memcpy(frameL.data(), left + pos, static_cast<std::size_t>(take) * sizeof(float));
        std::memcpy(frameR.data(), passRight + pos, static_cast<std::size_t>(take) * sizeof(float));
        const int n = encoder.encodeFrame(frameL.data(), frameR.data(), frameOut.data(),
                                          frameOut.size(), error);
        if (n < 0) {
            outPayload.clear();
            return false;
        }
        if (n > 0) {
            outPayload.insert(outPayload.end(), frameOut.begin(), frameOut.begin() + n);
        }
        ++encodedCount;
        reportStep(encodedCount, totalCalls, false);
    }

    std::fill(frameL.begin(), frameL.end(), 0.0f);
    std::fill(frameR.begin(), frameR.end(), 0.0f);
    for (int f = 0; f < flushTailFrames; ++f) {
        const int n = encoder.encodeFrame(frameL.data(), frameR.data(), frameOut.data(),
                                          frameOut.size(), error);
        if (n < 0) {
            outPayload.clear();
            return false;
        }
        if (n > 0) {
            outPayload.insert(outPayload.end(), frameOut.begin(), frameOut.begin() + n);
        }
        ++encodedCount;
        reportStep(encodedCount, totalCalls, f + 1 == flushTailFrames);
    }

    error.clear();
    return true;
}

bool encodeHiMdContinuousTracks(const std::string& mode, const float* left, const float* right,
                                int channels, const std::vector<int>& trackEndSamples,
                                std::vector<std::vector<std::uint8_t>>& outTrackPayloads,
                                const std::function<void(int, int)>& progressCallback,
                                std::string& error) {
    outTrackPayloads.clear();
    if (trackEndSamples.empty()) {
        error = "Continuous Hi-MD encoding requires at least one track boundary.";
        return false;
    }
    int prevEnd = 0;
    for (std::size_t i = 0; i < trackEndSamples.size(); ++i) {
        if (trackEndSamples[i] <= prevEnd) {
            error = "Continuous Hi-MD track end sample boundaries must be strictly positive and "
                    "increasing.";
            return false;
        }
        prevEnd = trackEndSamples[i];
    }
    if (channels < 1 || channels > 2 || left == nullptr) {
        error = "Invalid PCM input geometry for continuous Hi-MD encoding.";
        return false;
    }

    if (trackEndSamples.size() == 1u) {
        outTrackPayloads.resize(1u);
        if (!encodeHiMdTrackPayload(mode, left, right, channels, trackEndSamples[0],
                                    outTrackPayloads[0], progressCallback, error)) {
            outTrackPayloads.clear();
            return false;
        }
        return true;
    }

    std::string normalized = normalizeAsciiLower(mode);
    if (normalized == "hisp") {
        normalized = "hi-sp";
    } else if (normalized == "hilp") {
        normalized = "hi-lp";
    } else if (normalized == "lpcm") {
        normalized = "pcm";
    }

    int frameSamples = 0;
    int frameBytes = 0;
    if (normalized == "pcm") {
        frameSamples = 16;
        frameBytes = 64;
    } else if (normalized == "lp2") {
        frameSamples = 1024;
        frameBytes = 384;
    } else if (normalized == "lp4") {
        frameSamples = 1024;
        frameBytes = 192;
    } else if (normalized == "hi-sp") {
        frameSamples = 2048;
        frameBytes = 1488;
    } else if (normalized == "hi-lp") {
        frameSamples = 2048;
        frameBytes = 376;
    } else {
        error = "Unsupported Hi-MD codec mode '" + mode +
                "' (expected hi-sp, hi-lp, lp2, lp4, or pcm).";
        return false;
    }

    const int totalSamples = trackEndSamples.back();
    std::vector<std::uint8_t> fullBytes;
    if (!encodeHiMdTrackPayload(normalized, left, right, channels, totalSamples, fullBytes,
                                progressCallback, error)) {
        return false;
    }

    const std::size_t fb = static_cast<std::size_t>(frameBytes);
    if (normalized == "pcm" && (fullBytes.size() % fb) != 0u) {
        const std::size_t padded = ((fullBytes.size() + fb - 1u) / fb) * fb;
        fullBytes.resize(padded, 0u);
    }
    if (fullBytes.empty() || (fullBytes.size() % fb) != 0u) {
        error = "Continuous Hi-MD encoded stream is not aligned to codec frames.";
        return false;
    }

    const int totalFrames = static_cast<int>(fullBytes.size() / fb);
    const std::size_t kCount = trackEndSamples.size();
    if (totalFrames < static_cast<int>(kCount)) {
        error = "Continuous Hi-MD track run is too short to assign at least one codec frame per "
                "track.";
        return false;
    }

    outTrackPayloads.resize(kCount);
    int prevF = 0;
    for (std::size_t j = 0; j + 1u < kCount; ++j) {
        const std::int64_t s = static_cast<std::int64_t>(trackEndSamples[j]);
        const int ideal =
            static_cast<int>((s + frameSamples / 2) / static_cast<std::int64_t>(frameSamples));
        const int minF = prevF + 1;
        const int maxF = totalFrames - static_cast<int>(kCount - 1u - j);
        const int cutF = std::max(minF, std::min(maxF, ideal));
        outTrackPayloads[j].assign(
            fullBytes.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(prevF) * fb),
            fullBytes.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(cutF) * fb));
        prevF = cutF;
    }
    outTrackPayloads[kCount - 1u].assign(
        fullBytes.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(prevF) * fb),
        fullBytes.end());

    error.clear();
    return true;
}

// ============================================================================
// Offline Session Rendering & File Export
// ============================================================================

bool renderSessionMixRange(const Session& session, std::int64_t startSample, std::int64_t endSample,
                           int targetChannels, std::vector<float>& outLeft,
                           std::vector<float>& outRight, std::string& error) {
    outLeft.clear();
    outRight.clear();
    if (startSample < 0 || endSample < startSample ||
        (targetChannels != 1 && targetChannels != 2)) {
        error = "Invalid session render range or channel count.";
        return false;
    }

    const std::int64_t totalSamples = endSample - startSample;
    outLeft.assign(static_cast<std::size_t>(totalSamples), 0.0f);
    if (targetChannels == 2) {
        outRight.assign(static_cast<std::size_t>(totalSamples), 0.0f);
    }
    if (totalSamples == 0) {
        error.clear();
        return true;
    }

    ResamplingContext prepared;
    session.prepareResampling(prepared, sessionRate);

    constexpr int kBlockSamples = 16384;
    juce::AudioBuffer<float> block(targetChannels, kBlockSamples);
    std::int64_t pos = 0;
    while (pos < totalSamples) {
        const int chunk =
            static_cast<int>(std::min<std::int64_t>(kBlockSamples, totalSamples - pos));
        block.clear(0, chunk);
        bool unsupportedRate = false;
        const double startTime = static_cast<double>(startSample + pos) / sessionRate;
        session.render(block, 0, chunk, startTime, sessionRate, targetChannels, &prepared, {}, -1,
                       nullptr, &unsupportedRate, false);
        if (unsupportedRate) {
            error = "Clip sample rate conversion ratio is unsupported at 44100 Hz.";
            return false;
        }
        std::memcpy(outLeft.data() + static_cast<std::size_t>(pos), block.getReadPointer(0),
                    static_cast<std::size_t>(chunk) * sizeof(float));
        if (targetChannels == 2 && block.getNumChannels() >= 2) {
            std::memcpy(outRight.data() + static_cast<std::size_t>(pos), block.getReadPointer(1),
                        static_cast<std::size_t>(chunk) * sizeof(float));
        }
        pos += chunk;
    }

    error.clear();
    return true;
}

bool encodeWavFileToAtrac(const std::string& mode, const juce::File& inputWav,
                          const juce::File& outputFile, std::int64_t& samplesOut,
                          std::int64_t& bytesOut, std::string& error) {
    samplesOut = 0;
    bytesOut = 0;
    const auto* spec = findAtracModeSpec(mode);
    if (!spec) {
        error = "Unsupported ATRAC mode '" + mode +
                "' (expected sp-stereo, sp-mono, lp2, lp4, hi-sp, or hi-lp).";
        return false;
    }
    if (!inputWav.existsAsFile()) {
        error = "Input audio file does not exist: " + inputWav.getFullPathName().toStdString();
        return false;
    }
    if (outputFile == juce::File{}) {
        error = "Output file path is empty.";
        return false;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(inputWav));
    if (!reader || reader->sampleRate <= 0.0 || reader->numChannels == 0) {
        error = "Could not open input audio file: " + inputWav.getFullPathName().toStdString();
        return false;
    }
    if (reader->lengthInSamples > 200000000LL) {
        error = "Input audio file exceeds supported sample count limit.";
        return false;
    }

    const int srcSamples = static_cast<int>(reader->lengthInSamples);
    const int readChannels = std::min<int>(2, static_cast<int>(reader->numChannels));
    juce::AudioBuffer<float> raw(readChannels, std::max(1, srcSamples));
    raw.clear();
    if (srcSamples > 0 && !reader->read(&raw, 0, srcSamples, 0, true, readChannels >= 2)) {
        error = "Failed reading samples from input audio file.";
        return false;
    }

    std::vector<float> left(static_cast<std::size_t>(srcSamples));
    std::vector<float> right(static_cast<std::size_t>(srcSamples));
    if (srcSamples > 0) {
        std::memcpy(left.data(), raw.getReadPointer(0),
                    static_cast<std::size_t>(srcSamples) * sizeof(float));
        if (readChannels >= 2) {
            std::memcpy(right.data(), raw.getReadPointer(1),
                        static_cast<std::size_t>(srcSamples) * sizeof(float));
        } else {
            right = left;
        }
    }

    int encodeSamples = srcSamples;
    if (srcSamples > 0 && std::abs(reader->sampleRate - 44100.0) > 1.0e-6) {
        const int targetSamples = static_cast<int>(std::max<std::int64_t>(
            1, static_cast<std::int64_t>(
                   std::llround(static_cast<double>(srcSamples) * 44100.0 / reader->sampleRate))));
        std::vector<float> resampledLeft(static_cast<std::size_t>(targetSamples));
        std::vector<float> resampledRight(static_cast<std::size_t>(targetSamples));
        if (SincResampler::hasFullSupport(reader->sampleRate, 44100.0)) {
            SincResampler sinc(reader->sampleRate, 44100.0);
            const double step = reader->sampleRate / 44100.0;
            for (int i = 0; i < targetSamples; ++i) {
                const auto s = sinc.sampleStereo(left.data(), right.data(), srcSamples,
                                                 static_cast<double>(i) * step);
                resampledLeft[static_cast<std::size_t>(i)] = s.first;
                resampledRight[static_cast<std::size_t>(i)] = s.second;
            }
        } else {
            std::vector<float> padLeft(left);
            std::vector<float> padRight(right);
            padLeft.resize(static_cast<std::size_t>(srcSamples) + 16,
                           left.empty() ? 0.0f : left.back());
            padRight.resize(static_cast<std::size_t>(srcSamples) + 16,
                            right.empty() ? 0.0f : right.back());
            juce::LagrangeInterpolator interpL;
            juce::LagrangeInterpolator interpR;
            const double ratio = reader->sampleRate / 44100.0;
            interpL.process(ratio, padLeft.data(), resampledLeft.data(), targetSamples);
            interpR.process(ratio, padRight.data(), resampledRight.data(), targetSamples);
        }
        left = std::move(resampledLeft);
        right = std::move(resampledRight);
        encodeSamples = targetSamples;
    }

    std::vector<std::uint8_t> encoded;
    if (!encodeAtracContainer(spec->id, left.empty() ? nullptr : left.data(),
                              right.empty() ? nullptr : right.data(), readChannels, encodeSamples,
                              encoded, error)) {
        return false;
    }

    const auto parent = outputFile.getParentDirectory();
    if (parent != juce::File{} && !parent.exists()) {
        parent.createDirectory();
    }
    if (!outputFile.replaceWithData(encoded.data(), encoded.size())) {
        error =
            "Could not write encoded output file: " + outputFile.getFullPathName().toStdString();
        return false;
    }

    samplesOut = encodeSamples;
    bytesOut = static_cast<std::int64_t>(encoded.size());
    error.clear();
    return true;
}

// ============================================================================
// Frame-Streaming Encoder & Decoder Classes
// ============================================================================

NativeAtracStreamEncoder::~NativeAtracStreamEncoder() {
    close();
}

NativeAtracStreamEncoder::NativeAtracStreamEncoder(NativeAtracStreamEncoder&& other) noexcept
    : spec(other.spec), handle(other.handle) {
    other.spec = nullptr;
    other.handle = -1;
}

NativeAtracStreamEncoder&
NativeAtracStreamEncoder::operator=(NativeAtracStreamEncoder&& other) noexcept {
    if (this != &other) {
        close();
        spec = other.spec;
        handle = other.handle;
        other.spec = nullptr;
        other.handle = -1;
    }
    return *this;
}

bool NativeAtracStreamEncoder::open(const std::string& mode, std::string& error) {
    close();
    const auto* found = findAtracModeSpec(mode);
    if (!found) {
        error = "Unsupported ATRAC mode '" + mode + "'.";
        return false;
    }
    auto& ops = runtimeForCodec(found->codec);
    std::lock_guard<std::mutex> guard(ops.mutex);
    const int h = ops.createStream(1, found->modeCode);
    if (h < 0) {
        error = codecErrorText(ops, "Failed creating Porffor ATRAC encoder stream.");
        return false;
    }
    spec = found;
    handle = h;
    error.clear();
    return true;
}

void NativeAtracStreamEncoder::close() {
    if (handle >= 0 && spec != nullptr) {
        auto& ops = runtimeForCodec(spec->codec);
        std::lock_guard<std::mutex> guard(ops.mutex);
        ops.destroyStream(handle);
    }
    spec = nullptr;
    handle = -1;
}

int NativeAtracStreamEncoder::encodeFrame(const float* left, const float* right, std::uint8_t* dst,
                                          std::size_t dstCapacity, std::string& error) {
    if (!isOpen()) {
        error = "ATRAC encoder stream is not open.";
        return -1;
    }
    if (left == nullptr || (spec->channels == 2 && right == nullptr) || dst == nullptr ||
        dstCapacity < static_cast<std::size_t>(spec->bytesPerFrame)) {
        error = "Invalid buffers for ATRAC stream frame encode.";
        return -1;
    }
    auto& ops = runtimeForCodec(spec->codec);
    std::lock_guard<std::mutex> guard(ops.mutex);
    const int rc = ops.encodeFrame(handle, left, right, dst);
    if (rc < 0) {
        error = codecErrorText(ops, "Porffor ATRAC frame encode failed.");
        return -1;
    }
    error.clear();
    return rc;
}

NativeAtracStreamDecoder::~NativeAtracStreamDecoder() {
    close();
}

NativeAtracStreamDecoder::NativeAtracStreamDecoder(NativeAtracStreamDecoder&& other) noexcept
    : spec(other.spec), handle(other.handle) {
    other.spec = nullptr;
    other.handle = -1;
}

NativeAtracStreamDecoder&
NativeAtracStreamDecoder::operator=(NativeAtracStreamDecoder&& other) noexcept {
    if (this != &other) {
        close();
        spec = other.spec;
        handle = other.handle;
        other.spec = nullptr;
        other.handle = -1;
    }
    return *this;
}

bool NativeAtracStreamDecoder::open(const std::string& mode, std::string& error) {
    close();
    const auto* found = findAtracModeSpec(mode);
    if (!found) {
        error = "Unsupported ATRAC mode '" + mode + "'.";
        return false;
    }
    auto& ops = runtimeForCodec(found->codec);
    std::lock_guard<std::mutex> guard(ops.mutex);
    const int h = ops.createStream(0, found->modeCode);
    if (h < 0) {
        error = codecErrorText(ops, "Failed creating Porffor ATRAC decoder stream.");
        return false;
    }
    spec = found;
    handle = h;
    error.clear();
    return true;
}

void NativeAtracStreamDecoder::close() {
    if (handle >= 0 && spec != nullptr) {
        auto& ops = runtimeForCodec(spec->codec);
        std::lock_guard<std::mutex> guard(ops.mutex);
        ops.destroyStream(handle);
    }
    spec = nullptr;
    handle = -1;
}

int NativeAtracStreamDecoder::decodeFrame(const std::uint8_t* src, std::size_t srcLen, float* left,
                                          float* right, std::string& error) {
    if (!isOpen()) {
        error = "ATRAC decoder stream is not open.";
        return -1;
    }
    if (src == nullptr || srcLen < static_cast<std::size_t>(spec->bytesPerFrame) ||
        left == nullptr || (spec->channels == 2 && right == nullptr)) {
        error = "Invalid buffers for ATRAC stream frame decode.";
        return -1;
    }
    auto& ops = runtimeForCodec(spec->codec);
    std::lock_guard<std::mutex> guard(ops.mutex);
    const int rc = ops.decodeFrame(handle, src, static_cast<int>(srcLen), left, right);
    if (rc < 0) {
        error = codecErrorText(ops, "Porffor ATRAC frame decode failed.");
        return -1;
    }
    error.clear();
    return rc;
}

} // namespace carta
