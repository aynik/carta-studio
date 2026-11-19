/**
 * Streaming JUCE AudioFormat implementation for gapless-verified MP3 sources.
 *
 * Validates streams through `inspectMp3Gapless` before opening `mp3dec_ex_t`
 * in float output mode, and patches protected Xing/Info frame headers in the
 * read callback so minimp3 honors LAME gapless tags without mutating source
 * files on disk.
 */

#include "IO/NativeMp3Format.h"
#include "IO/Mp3Gapless.h"
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_NO_STDIO
#define MINIMP3_IMPLEMENTATION
#include <minimp3_ex.h>
#include <algorithm>
#include <array>
#include <limits>

namespace carta {
namespace {

constexpr const char* nativeMp3Name = "Carta MP3";

/**
 * Streaming AudioFormatReader backed by minimp3 and gapless Xing/Info timing.
 */
class Reader final : public juce::AudioFormatReader {
  public:
    explicit Reader(juce::InputStream* source) : AudioFormatReader(source, nativeMp3Name) {
        Mp3GaplessInfo timing;
        juce::String error;
        if (!inspectMp3Gapless(*input, timing, error)) {
            return;
        }
        infoFrameOffset = timing.infoFrameOffset;
        normalizeProtectedInfo = timing.protectedInfoFrame;

        io.read = [](void* buffer, size_t size, void* data) -> size_t {
            auto& reader = *static_cast<Reader*>(data);
            const auto streamPosition = reader.input->getPosition();
            const auto n = reader.input->read(
                buffer, static_cast<int>(
                            std::min(size, static_cast<size_t>(std::numeric_limits<int>::max()))));
            if (n > 0 && reader.normalizeProtectedInfo) {
                auto* bytes = static_cast<unsigned char*>(buffer);
                // The Xing frame carries metadata, not audio. LAME places the
                // tag at the non-CRC offset even when its frame is protected.
                // Present an unprotected, empty side-info frame to minimp3;
                // the source bytes and all audio frames remain unchanged.
                const auto edit = [&](juce::int64 absolute, unsigned char value, bool setBit) {
                    if (absolute >= streamPosition && absolute - streamPosition < n) {
                        auto& byte = bytes[static_cast<size_t>(absolute - streamPosition)];
                        if (setBit) {
                            byte |= value;
                        } else {
                            byte = value;
                        }
                    }
                };
                edit(reader.infoFrameOffset + 1, 1, true);
                edit(reader.infoFrameOffset + 4, 0, false);
                edit(reader.infoFrameOffset + 5, 0, false);
            }
            return n > 0 ? static_cast<size_t>(n) : 0;
        };

        io.seek = [](uint64_t pos, void* data) -> int {
            if (pos > static_cast<uint64_t>(std::numeric_limits<juce::int64>::max())) {
                return 1;
            }
            return static_cast<Reader*>(data)->input->setPosition(static_cast<juce::int64>(pos))
                       ? 0
                       : 1;
        };

        io.read_data = this;
        io.seek_data = this;
        opened = true;
        if (mp3dec_ex_open_cb(&decoder, &io, MP3D_SEEK_TO_SAMPLE | MP3D_DO_NOT_SCAN) != 0 ||
            decoder.info.layer != 3 || decoder.info.channels != timing.channels ||
            decoder.info.hz != timing.sampleRate ||
            decoder.samples != static_cast<uint64_t>(timing.sampleCount) *
                                   static_cast<uint64_t>(timing.channels)) {
            return;
        }

        sampleRate = timing.sampleRate;
        numChannels = static_cast<unsigned int>(timing.channels);
        lengthInSamples = timing.sampleCount;
        bitsPerSample = 32;
        usesFloatingPointData = true;
    }

    ~Reader() override {
        if (opened) {
            mp3dec_ex_close(&decoder);
        }
    }

    bool readSamples(int* const* destinations, int destChannels, int offset, juce::int64 start,
                     int count) override {
        if (start < 0 || count < 0 || start > lengthInSamples || count > lengthInSamples - start ||
            !destinations) {
            return false;
        }
        if (position != start) {
            if (mp3dec_ex_seek(&decoder, static_cast<uint64_t>(start) * numChannels) != 0) {
                return false;
            }
            position = start;
        }

        std::array<float, 8192> buffer{};
        while (count > 0) {
            const int frames = std::min(count, static_cast<int>(buffer.size() / numChannels));
            const size_t samples = static_cast<size_t>(frames) * numChannels;
            if (mp3dec_ex_read(&decoder, buffer.data(), samples) != samples || decoder.last_error) {
                return false;
            }
            for (int channel = 0; channel < destChannels; ++channel) {
                if (destinations[channel]) {
                    auto* target = reinterpret_cast<float*>(destinations[channel]) + offset;
                    const auto sourceChannel =
                        std::min(static_cast<unsigned int>(channel), numChannels - 1);
                    for (int i = 0; i < frames; ++i) {
                        target[i] = buffer[static_cast<size_t>(i) * numChannels + sourceChannel];
                    }
                }
            }
            offset += frames;
            position += frames;
            count -= frames;
        }
        return true;
    }

  private:
    mp3dec_io_t io{};
    mp3dec_ex_t decoder{};
    juce::int64 position = 0;
    juce::int64 infoFrameOffset = 0;
    bool opened = false;
    bool normalizeProtectedInfo = false;
};

/**
 * JUCE AudioFormat wrapper for read-only gapless MP3 import.
 */
class Format final : public juce::AudioFormat {
  public:
    Format() : AudioFormat(nativeMp3Name, ".mp3") {}

    juce::Array<int> getPossibleSampleRates() override {
        return {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    }

    juce::Array<int> getPossibleBitDepths() override { return {32}; }
    bool canDoStereo() override { return true; }
    bool canDoMono() override { return true; }
    bool isCompressed() override { return true; }

    juce::AudioFormatReader* createReaderFor(juce::InputStream* stream,
                                             bool deleteOnFailure) override {
        auto reader = std::make_unique<Reader>(stream);
        if (reader->sampleRate > 0) {
            return reader.release();
        }
        if (!deleteOnFailure) {
            reader->input = nullptr;
        }
        return nullptr;
    }

    std::unique_ptr<juce::AudioFormatWriter>
    createWriterFor(std::unique_ptr<juce::OutputStream>&,
                    const juce::AudioFormatWriterOptions&) override {
        return {};
    }
};

} // namespace

void registerNativeMp3Format(juce::AudioFormatManager& formats) {
    for (int i = 0; i < formats.getNumKnownFormats(); ++i) {
        if (formats.getKnownFormat(i)->getFormatName() == nativeMp3Name) {
            return;
        }
    }
    formats.registerFormat(new Format(), false);
}

} // namespace carta
