/**
 * MPEG Layer III gapless metadata inspection and bitstream validation.
 *
 * Parses ID3v2 headers, Xing/Info information frames, LAME/Lavc/Lavf encoder
 * delay/padding tags, LAME CRC-16 tag checksums, MPEG protected-frame CRCs,
 * and walks all audio frames (plus optional single trailing ID3v1 tag) to
 * verify exact gapless sample bounds without decoding audio.
 */

#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

namespace carta {

/**
 * Candidate timing for a single MPEG Layer III stream with a recognized
 * Xing/Info encoder extension.
 *
 * This is not an audio decoder; the caller must independently verify that the
 * decoder uses the same frame and 529-sample decoder-delay model.
 */
struct Mp3GaplessInfo {
    int sampleRate = 0;
    int channels = 0;
    int encoderDelay = 0;
    int encoderPadding = 0;
    juce::int64 rawFrames = 0;
    juce::int64 leadingSamples = 0;
    juce::int64 sampleCount = 0;
    juce::int64 infoFrameOffset = 0;
    bool protectedInfoFrame = false;
};

/**
 * Inspect a seekable MPEG Layer III stream for gapless timing and integrity.
 *
 * Restores the input stream's original position on both success and failure.
 * On failure, leaves `output` unchanged and populates `error`; unsupported and
 * malformed streams are rejected rather than guessing duration. When non-null,
 * `*recognized` is set to true once a Xing/Info header with LAME/Lavc/Lavf
 * encoder fields is identified.
 */
inline bool inspectMp3Gapless(juce::InputStream& input, Mp3GaplessInfo& output, juce::String& error,
                              std::function<bool()> keepGoing = {}, bool* recognized = nullptr) {
    if (recognized) {
        *recognized = false;
    }
    error.clear();

    const auto original = input.getPosition();
    const auto length = input.getTotalLength();
    struct Restore {
        juce::InputStream& stream;
        juce::int64 pos;
        bool done = false;
        ~Restore() {
            if (!done) {
                stream.setPosition(pos);
            }
        }
    } restore{input, original};

    const auto fail = [&](const char* message) {
        error = message;
        return false;
    };

    const auto readAt = [&](juce::int64 pos, void* destination, int count) {
        if (pos < 0 || count < 0 || pos > length || count > length - pos ||
            !input.setPosition(pos)) {
            return false;
        }
        auto* bytes = static_cast<unsigned char*>(destination);
        int done = 0;
        while (done < count) {
            const int n = input.read(bytes + done, count - done);
            if (n <= 0) {
                return false;
            }
            done += n;
        }
        return true;
    };

    const auto be32 = [](const unsigned char* p) {
        return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    };

    if (length < 10) {
        return fail("MP3 file is too short for gapless metadata.");
    }

    std::array<unsigned char, 10> id3{};
    if (!readAt(0, id3.data(), 10)) {
        return fail("Could not read MP3 metadata.");
    }

    juce::int64 first = 0;
    if (std::memcmp(id3.data(), "ID3", 3) == 0) {
        if ((id3[3] != 3 && id3[3] != 4) || id3[4] == 0xff ||
            (id3[3] == 4 && (id3[5] & 0x10) != 0) ||
            ((id3[6] | id3[7] | id3[8] | id3[9]) & 0x80) != 0) {
            return fail("Unsupported or invalid ID3 header.");
        }
        const auto tagSize = (static_cast<juce::int64>(id3[6]) << 21) |
                             (static_cast<juce::int64>(id3[7]) << 14) |
                             (static_cast<juce::int64>(id3[8]) << 7) | id3[9];
        first = 10 + tagSize;
        if (first > length - 4) {
            return fail("Truncated ID3 header.");
        }
    }

    static constexpr int mpeg1Bitrates[]{0,   32,  40,  48,  56,  64,  80,  96,
                                         112, 128, 160, 192, 224, 256, 320, 0};
    static constexpr int lowBitrates[]{0,  8,  16, 24,  32,  40,  48,  56,
                                       64, 80, 96, 112, 128, 144, 160, 0};
    static constexpr int rates[]{44100, 48000, 32000, 0};

    const auto decodeHeader = [&](const unsigned char* bytes, int& version, int& rate,
                                  int& channels, int& frameBytes) {
        const auto h = be32(bytes);
        const int bitrate = static_cast<int>((h >> 12) & 15u);
        const int sampleRate = static_cast<int>((h >> 10) & 3u);
        version = static_cast<int>((h >> 19) & 3u);
        if ((h & 0xffe00000u) != 0xffe00000u || version == 1 || ((h >> 17) & 3u) != 1u ||
            rates[sampleRate] == 0) {
            return false;
        }
        const int kbps = version == 3 ? mpeg1Bitrates[bitrate] : lowBitrates[bitrate];
        if (kbps == 0) {
            return false;
        }
        rate = rates[sampleRate] / (version == 3 ? 1 : version == 2 ? 2 : 4);
        channels = ((h >> 6) & 3u) == 3u ? 1 : 2;
        frameBytes =
            ((version == 3 ? 144000 : 72000) * kbps) / rate + static_cast<int>((h >> 9) & 1u);
        return frameBytes >= 4;
    };

    std::array<unsigned char, 4> header{};
    int version = 0;
    int rate = 0;
    int channels = 0;
    int frameBytes = 0;
    if (!readAt(first, header.data(), 4) ||
        !decodeHeader(header.data(), version, rate, channels, frameBytes)) {
        return fail("MP3 gapless inspection supports MPEG Layer III only.");
    }
    if (frameBytes > length - first) {
        return fail("Truncated MP3 information frame.");
    }

    std::vector<unsigned char> frame(static_cast<size_t>(frameBytes));
    if (!readAt(first, frame.data(), frameBytes)) {
        return fail("Could not read MP3 information frame.");
    }

    const int samplesPerFrame = version == 3 ? 1152 : 576;
    const int sideInfo = version == 3 ? (channels == 1 ? 17 : 32) : (channels == 1 ? 9 : 17);
    size_t cursor = static_cast<size_t>(4 + sideInfo);
    const auto available = [&](size_t count) {
        return cursor <= frame.size() && count <= frame.size() - cursor;
    };

    if (!available(8) || (std::memcmp(frame.data() + cursor, "Xing", 4) != 0 &&
                          std::memcmp(frame.data() + cursor, "Info", 4) != 0)) {
        return fail("MP3 has no supported gapless information header.");
    }
    cursor += 4;
    const auto flags = be32(frame.data() + cursor);
    cursor += 4;

    if ((flags & 1u) == 0 || (flags & ~15u) != 0 || !available(4)) {
        return fail("MP3 gapless frame count is missing or invalid.");
    }
    const auto frames = be32(frame.data() + cursor);
    cursor += 4;
    if (frames == 0 ||
        static_cast<juce::uint64>(frames) >
            static_cast<juce::uint64>(std::numeric_limits<int>::max() / samplesPerFrame)) {
        return fail("MP3 gapless frame count is out of range.");
    }
    if ((flags & 2u) != 0) {
        if (!available(4)) {
            return fail("Truncated MP3 information header.");
        }
        cursor += 4;
    }
    if ((flags & 4u) != 0) {
        if (!available(100)) {
            return fail("Truncated MP3 information header.");
        }
        cursor += 100;
    }
    if ((flags & 8u) != 0) {
        if (!available(4)) {
            return fail("Truncated MP3 information header.");
        }
        cursor += 4;
    }
    if (!available(24) || (std::memcmp(frame.data() + cursor, "LAME", 4) != 0 &&
                           std::memcmp(frame.data() + cursor, "Lavc", 4) != 0 &&
                           std::memcmp(frame.data() + cursor, "Lavf", 4) != 0)) {
        return fail("MP3 encoder delay and padding are unavailable.");
    }
    if (recognized) {
        *recognized = true;
    }

    // LAME's tag CRC covers the information frame through the encoder fields,
    // including delay and padding. Some encoders omit it (zero), and Lavc/Lavf
    // tags have differing checksum layouts, so only validate a present LAME CRC.
    if (std::memcmp(frame.data() + cursor, "LAME", 4) == 0 && available(36)) {
        const auto tagCrc = static_cast<std::uint16_t>(
            (static_cast<unsigned int>(frame[cursor + 34]) << 8) | frame[cursor + 35]);
        if (tagCrc != 0) {
            std::uint16_t calculated = 0;
            for (size_t i = 0; i < cursor + 34; ++i) {
                calculated = static_cast<std::uint16_t>(calculated ^ frame[i]);
                for (int bit = 0; bit < 8; ++bit) {
                    calculated = static_cast<std::uint16_t>(
                        (calculated >> 1) ^ ((calculated & 1u) != 0 ? 0xa001u : 0u));
                }
            }
            if (calculated != tagCrc) {
                return fail("MP3 timing tag checksum mismatch.");
            }
        }
    }

    // A protected Xing frame at the conventional fixed offset overlaps the
    // end of the protected side information. Only normalize empty metadata
    // frames; nonzero preceding side information could describe audio.
    if ((frame[1] & 1u) == 0) {
        for (size_t i = 6; i < static_cast<size_t>(4 + sideInfo); ++i) {
            if (frame[i] != 0) {
                return fail("Unsupported protected MP3 information frame.");
            }
        }
    }

    // MPEG Layer III protection covers the last two header bytes and side
    // information, not the main audio data. Unprotected frames have no CRC.
    const auto verifyFrameCrc = [&](juce::int64 position, const unsigned char* h, int bytes) {
        if ((h[1] & 1u) != 0) {
            return true;
        }
        if (bytes < 6 + sideInfo) {
            return false;
        }
        std::array<unsigned char, 34> data{};
        if (!readAt(position + 4, data.data(), 2 + sideInfo)) {
            return false;
        }
        std::uint16_t crc = 0xffff;
        const auto update = [&](unsigned char value) {
            for (int bit = 7; bit >= 0; --bit) {
                const bool high = ((crc >> 15) ^ static_cast<unsigned int>(value >> bit)) & 1u;
                crc = static_cast<std::uint16_t>((static_cast<unsigned int>(crc) << 1) ^
                                                 (high ? 0x8005u : 0u));
            }
        };
        update(h[2]);
        update(h[3]);
        for (int i = 0; i < sideInfo; ++i) {
            update(data[static_cast<size_t>(i + 2)]);
        }
        return crc ==
               static_cast<std::uint16_t>((static_cast<unsigned int>(data[0]) << 8) | data[1]);
    };

    if (!verifyFrameCrc(first, frame.data(), frameBytes)) {
        return fail("MP3 frame checksum mismatch.");
    }

    const int delay = (static_cast<int>(frame[cursor + 21]) << 4) | (frame[cursor + 22] >> 4);
    const int padding = ((frame[cursor + 22] & 15) << 8) | frame[cursor + 23];
    constexpr int decoderDelay = 529;
    const auto raw = static_cast<juce::int64>(frames) * samplesPerFrame;
    const auto leading = static_cast<juce::int64>(delay) + decoderDelay;
    const auto count = raw - delay - padding;
    if (padding < decoderDelay || count <= 0 || leading > raw || count > raw - leading) {
        return fail("Invalid MP3 encoder delay or padding.");
    }

    juce::int64 position = first + frameBytes;
    for (std::uint32_t i = 0; i < frames; ++i) {
        if ((i % 128u) == 0 && keepGoing && !keepGoing()) {
            return fail("Import cancelled.");
        }
        int nextVersion = 0;
        int nextRate = 0;
        int nextChannels = 0;
        int nextBytes = 0;
        if (!readAt(position, header.data(), 4) ||
            !decodeHeader(header.data(), nextVersion, nextRate, nextChannels, nextBytes) ||
            nextVersion != version || nextRate != rate || nextChannels != channels ||
            nextBytes > length - position) {
            return fail("Truncated, inconsistent or unsupported MP3 audio frame.");
        }
        if (!verifyFrameCrc(position, header.data(), nextBytes)) {
            return fail("MP3 frame checksum mismatch.");
        }
        position += nextBytes;
    }

    // A single trailing ID3v1 tag is allowed; concatenated streams and other
    // trailing bytes need separate handling instead of guessing boundaries.
    if (position != length) {
        std::array<unsigned char, 3> tag{};
        if (length - position != 128 || !readAt(position, tag.data(), 3) ||
            std::memcmp(tag.data(), "TAG", 3) != 0) {
            return fail("Unexpected trailing data or concatenated MP3 stream.");
        }
    }

    Mp3GaplessInfo next;
    next.sampleRate = rate;
    next.channels = channels;
    next.encoderDelay = delay;
    next.encoderPadding = padding;
    next.rawFrames = raw;
    next.leadingSamples = leading;
    next.sampleCount = count;
    next.infoFrameOffset = first;
    next.protectedInfoFrame = (frame[1] & 1u) == 0;

    if (!input.setPosition(original) || input.getPosition() != original) {
        return fail("Could not restore the MP3 input position.");
    }
    restore.done = true;
    output = next;
    return true;
}

} // namespace carta
