/**
 * Session persistence, WAV/PNG export, level/clipping preflight, and marker
 * exchange implementation for Carta Studio.
 *
 * Organized into four logical layers:
 *   1. Atomic exclusive file staging (`WavFileOutput`, `commitTemporary`),
 *      numeric JSON pre-checks (`safeJsonNumbers`), IEEE-754 hex double
 *      encoding, and deterministic TPDF dither.
 *   2. Mix validation, RIFF cue assembly, level/clipping preflight scans, and
 *      WAV / master-track batch export.
 *   3. MiniDisc UTOC/track JSON serialization (`encodeMdDisc`/`decodeMdDisc`),
 *      `carta-markers` JSON exchange, and Audacity tab-separated point labels.
 *   4. `.carta` project save/load (versions 1..25), portable media bundling,
 *      and `CARTAP01` single-file archive creation/extraction.
 */

#include "IO/SessionIO.h"
#include "MiniDisc/Utoc.h"
#if CARTA_STUDIO_MINIMP3
#include "IO/NativeMp3Format.h"
#endif
#include "Audio/Resampler.h"
#include "Audio/Transport.h"
#include "IO/FileObservation.h"
#include "IO/CheckedText.h"
#include "IO/CheckedOutput.h"
#include <cmath>
#include <map>
#include <set>
#include <filesystem>
#include <algorithm>
#include <limits>
#include <cstring>
#include <cerrno>
#include <unordered_map>
#if JUCE_MAC || JUCE_LINUX
#include <fcntl.h>
#include <unistd.h>
#endif
#if JUCE_MAC
#include <sys/stdio.h>
#elif JUCE_LINUX
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace carta {
namespace {

#if JUCE_MAC || JUCE_LINUX
/**
 * Exclusive file descriptor OutputStream for WAV exports and packages.
 *
 * Unlike JUCE's buffered file stream, this retains partial-write, seek,
 * fsync, and close failures across writer header rewrites.
 */
class WavFileOutput final : public juce::OutputStream {
  public:
    explicit WavFileOutput(int descriptor) : fd(descriptor) {}

    ~WavFileOutput() override {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    bool write(const void* data, size_t bytes) override {
        if (failed || fd < 0) {
            return false;
        }
        const bool ok = writeAllBytes(data, bytes, [this](const void* source, size_t count) {
            ssize_t result;
            do {
                result = ::write(
                    fd, source,
                    std::min(count, static_cast<size_t>(std::numeric_limits<ssize_t>::max())));
            } while (result < 0 && errno == EINTR);
            return result;
        });
        failed |= !ok;
        return ok;
    }

    juce::int64 getPosition() override {
        const auto position = ::lseek(fd, 0, SEEK_CUR);
        if (position < 0) {
            failed = true;
        }
        return static_cast<juce::int64>(position);
    }

    bool setPosition(juce::int64 position) override {
        if (position < 0 || static_cast<juce::int64>(static_cast<off_t>(position)) != position) {
            failed = true;
            return false;
        }
        const auto result = ::lseek(fd, static_cast<off_t>(position), SEEK_SET);
        if (result != static_cast<off_t>(position)) {
            failed = true;
            return false;
        }
        return true;
    }

    void flush() override {
        if (fd < 0 || ::fsync(fd) != 0) {
            failed = true;
        }
    }

    bool finish() {
        flush();
        if (fd >= 0) {
            if (::close(fd) != 0) {
                failed = true;
            }
            fd = -1;
        }
        return !failed;
    }

  private:
    int fd = -1;
    bool failed = false;
};

/** Exclusively create a temporary file in `target`'s parent directory. */
bool createExclusiveTempFile(const juce::File& target, const char* prefix, int mode,
                             int& descriptor, juce::File& temporaryPath) {
    descriptor = -1;
    for (int attempt = 0; attempt < 16 && descriptor < 0; ++attempt) {
        temporaryPath = target.getParentDirectory().getChildFile(prefix + juce::Uuid().toString());
        descriptor = ::open(temporaryPath.getFullPathName().toRawUTF8(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
        if (descriptor < 0 && errno != EEXIST) {
            break;
        }
    }
    return descriptor >= 0;
}
#endif

constexpr juce::int64 maxProjectBytes = 64LL * 1024 * 1024;
constexpr double maxExactProjectSample = 9007199254740992.0;

/**
 * Guard JUCE's JSON parser against signed integer overflow on malformed numbers.
 *
 * JUCE accumulates the integer prefix and exponent in signed integers before
 * converting to double. String literals are skipped verbatim.
 */
bool safeJsonNumbers(const char* text, size_t size) {
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    char quote = 0;
    bool escaped = false;

    for (size_t i = 0; i < size;) {
        const char c = text[i];
        if (quote) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == quote) {
                quote = 0;
            }
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            ++i;
            continue;
        }
        // JUCE calls its number parser unconditionally after '-'; without a
        // digit it can interpret arbitrary following characters as digits.
        if (c == '-') {
            ++i;
            while (i < size &&
                   (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
                ++i;
            }
            if (i == size || !digit(text[i])) {
                return false;
            }
            continue;
        }
        if (!digit(c)) {
            ++i;
            continue;
        }

        const size_t start = i;
        std::uint64_t integer = 0;
        while (i < size && digit(text[i])) {
            const auto d = static_cast<unsigned>(text[i++] - '0');
            if (integer >
                (static_cast<std::uint64_t>(std::numeric_limits<juce::int64>::max()) - d) / 10) {
                return false;
            }
            integer = integer * 10 + d;
        }
        if (i < size && text[i] == '.') {
            ++i;
            while (i < size && digit(text[i])) {
                ++i;
            }
        }
        if (i < size && (text[i] == 'e' || text[i] == 'E')) {
            ++i;
            if (i < size && (text[i] == '+' || text[i] == '-')) {
                ++i;
            }
            unsigned exponent = 0;
            while (i < size && digit(text[i])) {
                const auto d = static_cast<unsigned>(text[i++] - '0');
                if (exponent > 10000 || exponent * 10 + d > 10000) {
                    return false;
                }
                exponent = exponent * 10 + d;
            }
        }
        if (i - start > 128) {
            return false;
        }
    }
    return true;
}

// Counter-based noise is reproducible across exports and independent of blocks.
std::uint64_t mixNoise(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

double tpdfNoise(juce::int64 sample, int channel) noexcept {
    const auto key = static_cast<std::uint64_t>(sample) * 0xd2b74407b1ce6e93ULL +
                     static_cast<std::uint64_t>(channel) * 0x9e3779b97f4a7c15ULL;
    const auto uniform = [](std::uint64_t bits) {
        return static_cast<double>(bits >> 11) * (1.0 / 9007199254740992.0);
    };
    return uniform(mixNoise(key)) - uniform(mixNoise(key ^ 0xa0761d6478bd642fULL));
}

bool pcmSampleClips(float sample, double scaled, double scale) noexcept {
    // llround rounds halfway away from zero. A rounded value outside the
    // signed PCM range must be saturated, including positive full scale.
    return std::abs(sample) > 1.0f || scaled <= -scale - 0.5 || scaled >= scale - 0.5;
}

juce::var object() {
    return juce::var(new juce::DynamicObject());
}

bool sameFile(const juce::File& a, const juce::File& b) {
    if (a == b) {
        return true;
    }
    std::error_code ec;
    return std::filesystem::equivalent(std::filesystem::u8path(a.getFullPathName().toStdString()),
                                       std::filesystem::u8path(b.getFullPathName().toStdString()),
                                       ec) &&
           !ec;
}

bool commitTemporary(juce::TemporaryFile& temp, const juce::File& target, bool noReplace) {
    if (!noReplace) {
        return replaceTemporaryAtomically(temp, target);
    }

    const auto sourcePath = temp.getFile().getFullPathName().toStdString();
    const auto targetPath = target.getFullPathName().toStdString();
    // Prefer native exclusive rename: unlike hard links, it can work on
    // removable filesystems that do not support linking. Never fall back to
    // an overwriting rename when exclusive operations are unsupported.
#if JUCE_MAC
    if (::renamex_np(sourcePath.c_str(), targetPath.c_str(), RENAME_EXCL) == 0) {
        return true;
    }
    const int renameError = errno;
    if (renameError != ENOTSUP && renameError != EOPNOTSUPP && renameError != EINVAL &&
        renameError != ENOSYS) {
        return false;
    }
#elif JUCE_LINUX && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, sourcePath.c_str(), AT_FDCWD, targetPath.c_str(),
                  RENAME_NOREPLACE) == 0) {
        return true;
    }
    const int renameError = errno;
    if (renameError != ENOTSUP && renameError != EOPNOTSUPP && renameError != EINVAL &&
        renameError != ENOSYS) {
        return false;
    }
#endif
    std::error_code ec;
    // Both paths are in the target directory. The fallback link also fails if
    // the destination exists; TemporaryFile removes its original name.
    std::filesystem::create_hard_link(std::filesystem::u8path(sourcePath),
                                      std::filesystem::u8path(targetPath), ec);
    return !ec;
}

bool number(const juce::var& v, double& n) {
    if (!v.isInt() && !v.isInt64() && !v.isDouble()) {
        return false;
    }
    n = static_cast<double>(v);
    return std::isfinite(n);
}

juce::String encodeDoubleBits(double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559,
                  "Project format requires IEEE-754 binary64.");
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    constexpr char digits[] = "0123456789abcdef";
    char encoded[16];
    for (int i = 15; i >= 0; --i) {
        encoded[i] = digits[bits & 15u];
        bits >>= 4;
    }
    return juce::String::fromUTF8(encoded, 16);
}

bool exactProjectDouble(const juce::var& readable, const juce::var& encoded, double& value) {
    double approximate = 0;
    if (!number(readable, approximate) || !encoded.isString()) {
        return false;
    }
    const auto text = encoded.toString();
    if (text.length() != 16) {
        return false;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        const auto c = text[i];
        const int digit = c >= '0' && c <= '9'
                              ? static_cast<int>(c - '0')
                              : (c >= 'a' && c <= 'f' ? static_cast<int>(c - 'a') + 10 : -1);
        if (digit < 0) {
            return false;
        }
        bits = (bits << 4) | static_cast<std::uint64_t>(digit);
    }
    double decoded = 0;
    std::memcpy(&decoded, &bits, sizeof(decoded));
    if (!std::isfinite(decoded) ||
        std::abs(decoded - approximate) > std::max(1e-18, std::abs(decoded) * 1e-12)) {
        return false;
    }
    value = decoded;
    return true;
}

bool validMdTrackAudioMode(const std::string& mode) {
    return mode.empty() || mode == "sp-stereo" || mode == "sp-mono" || mode == "lp2" ||
           mode == "lp4";
}

bool validMdTimestamp(const std::string& ts) {
    if (ts.empty()) {
        return true;
    }
    UtocTimestamp parsed{};
    std::string ignored;
    return parseUtocTimestampString(ts, parsed, ignored);
}

juce::String fadeShapeName(FadeShape shape) {
    switch (shape) {
    case FadeShape::equalPower:
        return "equal-power";
    case FadeShape::sCurve:
        return "s-curve";
    case FadeShape::exponential:
        return "exponential";
    case FadeShape::linear:
    default:
        return "linear";
    }
}

bool parseFadeShape(const juce::var& value, FadeShape& out) {
    if (!value.isString()) {
        return false;
    }
    const auto s = value.toString();
    if (s == "equal-power") {
        out = FadeShape::equalPower;
        return true;
    }
    if (s == "s-curve") {
        out = FadeShape::sCurve;
        return true;
    }
    if (s == "exponential") {
        out = FadeShape::exponential;
        return true;
    }
    return false;
}

bool validFadeShape(FadeShape shape) noexcept {
    return shape == FadeShape::linear || shape == FadeShape::equalPower ||
           shape == FadeShape::sCurve || shape == FadeShape::exponential;
}

bool validProjectClip(const Clip& clip, juce::String& error) {
    if (!clip.audio || !std::isfinite(clip.audio->sampleRate) || clip.audio->sampleRate <= 0 ||
        clip.audio->numSamples() <= 0 || clip.audio->numChannels() <= 0 ||
        !validTimelineGeometry(clip.start, clip.offset, clip.duration) ||
        !std::isfinite(clip.audio->length()) ||
        clip.offset + clip.duration > clip.audio->length() + 1e-8) {
        error = "Invalid project clip bounds or audio source.";
        return false;
    }
    if (clip.name.length() > 1024) {
        error = "Clip name is too long.";
        return false;
    }
    if (!validClipGroupId(clip.groupId)) {
        error = "Invalid clip group identity.";
        return false;
    }
    if (!std::isfinite(clip.gain) || clip.gain < 0 || clip.gain > maxLinearGain) {
        error = "Invalid clip gain.";
        return false;
    }
    if (!validFadeShape(clip.fadeInShape) || !validFadeShape(clip.fadeOutShape)) {
        error = "Invalid clip fade shape.";
        return false;
    }
    if (!std::isfinite(clip.fadeIn) || !std::isfinite(clip.fadeOut) ||
        !std::isfinite(clip.fadeInStart) || !std::isfinite(clip.fadeOutEnd) || clip.fadeIn < 0 ||
        clip.fadeOut < 0 || clip.fadeInStart < 0 || clip.fadeOutEnd < 0 ||
        clip.fadeIn > clip.audio->length() + 1e-8 || clip.fadeOut > clip.audio->length() + 1e-8 ||
        clip.fadeInStart + clip.fadeIn > clip.audio->length() + 1e-8 ||
        clip.fadeOutEnd > clip.audio->length() + 1e-8 || clip.fadeOut > clip.fadeOutEnd + 1e-8) {
        error = "Invalid clip fade.";
        return false;
    }
    return true;
}

bool validProjectMarker(const Marker& marker) {
    const double frame = marker.position * sessionRate;
    return std::isfinite(frame) && frame >= 0 && frame < maxExactProjectSample;
}

bool validMarkerId(const juce::String& id) {
    if (id.length() != 32) {
        return false;
    }
    for (const auto c : id) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

} // namespace

bool pathsReferToSameFile(const juce::File& a, const juce::File& b) {
    return a != juce::File{} && b != juce::File{} && sameFile(a, b);
}

bool validateOutputDestination(const Session& session, const juce::File& target,
                               const juce::File& protectedProject, juce::String& error) {
    if (target == juce::File{}) {
        error = "Choose an output file.";
        return false;
    }
    if (protectedProject != juce::File{} && sameFile(protectedProject, target)) {
        error = "Choose an output other than the open project file.";
        return false;
    }
    for (const auto& track : session.tracks) {
        for (const auto& clip : track.clips) {
            if (sameFile(clip.audio->file, target)) {
                error = "Choose an output other than a source file.";
                return false;
            }
        }
    }
    return true;
}

bool writeNewPng(const juce::Image& image, const juce::File& target, juce::String& error) {
    if (!image.isValid() || target == juce::File{} || !target.getParentDirectory().isDirectory()) {
        error = "Invalid capture or destination.";
        return false;
    }
    juce::MemoryOutputStream bytes;
    juce::PNGImageFormat png;
    if (!png.writeImageToStream(image, bytes)) {
        error = "Could not encode PNG capture.";
        return false;
    }
    auto temporary = prepareCheckedBytes(target, bytes.getData(), bytes.getDataSize());
    if (!temporary || !commitTemporary(*temporary, target, true)) {
        error = "Could not write capture without replacing an existing file.";
        return false;
    }
    return true;
}

WavRange fullWavRange(const Session& session) {
    return {0, projectEndSample(session)};
}

bool validateMixRange(const Session& session, int channels, WavRange range, juce::String& error,
                      int* affectedTrack) {
    constexpr juce::int64 maxExactSample = 9007199254740992LL;
    if ((channels != 1 && channels != 2) || fullWavRange(session).end <= 0) {
        error = "Nothing to render or invalid channel count.";
        return false;
    }
    if (range.start < 0 || range.end <= range.start || range.end > maxExactSample) {
        error = "Invalid sample range.";
        return false;
    }

    const double start = static_cast<double>(range.start) / sessionRate;
    const double end = static_cast<double>(range.end) / sessionRate;
    const bool anySolo = std::any_of(session.tracks.begin(), session.tracks.end(),
                                     [](const Track& t) { return t.solo; });

    for (size_t index = 0; index < session.tracks.size(); ++index) {
        const auto& track = session.tracks[index];
        if (track.muted || (anySolo && !track.solo)) {
            continue;
        }
        for (const auto& clip : track.clips) {
            if (clip.start < end && clip.start + clip.duration > start) {
                if (!clipRoutingConfirmed(track, clip.audio->numChannels())) {
                    if (affectedTrack) {
                        *affectedTrack = static_cast<int>(index);
                    }
                    error =
                        "Confirm channel routing for track '" + track.name + "' before rendering.";
                    return false;
                }
                if (!SincResampler::hasFullSupport(clip.audio->sampleRate, sessionRate)) {
                    if (affectedTrack) {
                        *affectedTrack = static_cast<int>(index);
                    }
                    error = "Source on track '" + track.name +
                            "' has a sample rate too high for reliable 44.1 kHz conversion. "
                            "Convert the source to a lower sample rate before rendering.";
                    return false;
                }
            }
        }
    }
    return true;
}
namespace {

bool buildWavCues(const Session& session, WavRange range,
                  std::unordered_map<juce::String, juce::String>& metadata, juce::int64& overhead,
                  juce::String& error) {
    struct Cue {
        juce::int64 sample;
        juce::String name;
    };
    if (session.markers.size() > 10000) {
        error = "Too many markers for WAV cue export.";
        return false;
    }

    std::vector<Cue> all;
    all.reserve(session.markers.size());
    const auto end = projectEndSample(session);
    for (const auto& marker : session.markers) {
        const auto frame = marker.position * sessionRate;
        if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
            error = "Repair invalid marker positions before embedding WAV cues.";
            return false;
        }
        const bool aligned = markerPositionOnSampleGrid(marker.position);
        const double comparison = aligned ? std::round(frame) : frame;
        if (comparison < static_cast<double>(range.start) ||
            comparison >= static_cast<double>(range.end)) {
            continue;
        }
        if (!aligned || marker.name.trim().isEmpty()) {
            error =
                "Repair unnamed or off-grid markers in the export range before embedding WAV cues.";
            return false;
        }
        const auto sample = static_cast<juce::int64>(std::llround(frame));
        if (sample >= end) {
            error = "A marker lies outside the master; repair it before embedding WAV cues.";
            return false;
        }
        all.push_back({sample, marker.name});
    }

    std::sort(all.begin(), all.end(),
              [](const auto& a, const auto& b) { return a.sample < b.sample; });
    for (size_t i = 1; i < all.size(); ++i) {
        if (all[i - 1].sample == all[i].sample) {
            error = "Duplicate marker positions cannot be embedded as WAV cues.";
            return false;
        }
    }

    std::unordered_map<juce::String, juce::String> next;
    juce::int64 bytes = 64;
    int count = 0;
    for (const auto& cue : all) {
        const auto offset = cue.sample - range.start;
        if (offset > std::numeric_limits<int>::max()) {
            error = "A WAV cue exceeds the supported sample offset.";
            return false;
        }
        const auto nameBytes = cue.name.getNumBytesAsUTF8();
        if (nameBytes > 8u * 1024u * 1024u ||
            bytes > 8LL * 1024 * 1024 - static_cast<juce::int64>(nameBytes) - 64) {
            error = "WAV cue metadata exceeds 8 MiB.";
            return false;
        }
        bytes += static_cast<juce::int64>(nameBytes) + 64;
        const auto index = juce::String(count);
        const auto id = juce::String(count + 1);
        next["Cue" + index + "Identifier"] = id;
        next["Cue" + index + "Offset"] = juce::String(offset);
        next["CueLabel" + index + "Identifier"] = id;
        next["CueLabel" + index + "Text"] = cue.name;
        ++count;
    }

    if (count > 0) {
        next["NumCuePoints"] = juce::String(count);
        next["NumCueLabels"] = juce::String(count);
    }
    metadata = std::move(next);
    overhead = bytes;
    return true;
}

bool buildDiscWavCues(const Session& session,
                      std::unordered_map<juce::String, juce::String>& metadata,
                      juce::int64& overhead, juce::String& error) {
    if (session.mdDisc.tracks.empty()) {
        error = "Add at least one MiniDisc track first.";
        return false;
    }
    std::string reason;
    if (!validateMdDisc(session.mdDisc, reason)) {
        error = "Invalid MD disc: " + juce::String::fromUTF8(reason.c_str());
        return false;
    }

    std::unordered_map<juce::String, juce::String> next;
    juce::int64 cursor = 0;
    juce::int64 bytes = 64;
    int labels = 0;
    for (size_t i = 0; i < session.mdDisc.tracks.size(); ++i) {
        const auto& trk = session.mdDisc.tracks[i];
        const auto title = trk.displayTitle();
        if (cursor > std::numeric_limits<int>::max()) {
            error = "A disc track cue exceeds the supported WAV sample offset.";
            return false;
        }
        if (title.find('\0') != std::string::npos || title.size() > 8u * 1024u * 1024u ||
            bytes > 8LL * 1024 * 1024 - static_cast<juce::int64>(title.size()) - 64) {
            error = "Disc track titles cannot be represented in WAV cue metadata.";
            return false;
        }
        bytes += static_cast<juce::int64>(title.size()) + 64;
        const auto index = juce::String(static_cast<int>(i));
        const auto id = juce::String(static_cast<int>(i + 1));
        next["Cue" + index + "Identifier"] = id;
        next["Cue" + index + "Offset"] = juce::String(cursor);
        if (!title.empty()) {
            const auto label = juce::String(labels++);
            next["CueLabel" + label + "Identifier"] = id;
            next["CueLabel" + label + "Text"] = juce::String::fromUTF8(title.c_str());
        }
        cursor += trk.totalSamples();
    }

    next["NumCuePoints"] = juce::String(static_cast<int>(session.mdDisc.tracks.size()));
    if (labels > 0) {
        next["NumCueLabels"] = juce::String(labels);
    }
    metadata = std::move(next);
    overhead = bytes;
    return true;
}

} // namespace

bool validateWavRange(const Session& session, int channels, WavRange range, juce::String& error,
                      WavOptions options) {
    if (options.sourceChannels) {
        if (channels < 3 || channels > 1024 || session.tracks.size() != 1 ||
            session.tracks[0].muted || options.encoding != WavEncoding::float32 || options.dither ||
            options.embedMarkers || range.start < 0 || range.end <= range.start ||
            range.end > 9007199254740992LL || fullWavRange(session).end <= 0) {
            error = "Invalid source-channel consolidation render.";
            return false;
        }
        for (const auto& clip : session.tracks[0].clips) {
            if (!clip.audio || clip.audio->numChannels() != channels) {
                error = "Multichannel consolidation requires matching source channel counts.";
                return false;
            }
            if (!SincResampler::hasFullSupport(clip.audio->sampleRate, sessionRate)) {
                error = "Source sample rate is too high for reliable 44.1 kHz conversion.";
                return false;
            }
        }
    } else if (!validateMixRange(session, channels, range, error)) {
        return false;
    }

    // Classic RIFF has 32-bit chunk sizes. Leave space for headers/metadata.
    juce::int64 metadataBytes = 0;
    if (options.embedMarkers) {
        std::unordered_map<juce::String, juce::String> metadata;
        if (!buildWavCues(session, range, metadata, metadataBytes, error)) {
            return false;
        }
    }

    const juce::int64 maxDataBytes =
        static_cast<juce::int64>(std::numeric_limits<std::uint32_t>::max()) - 4096 - metadataBytes;
    int bytesPerSample = 0;
    switch (options.encoding) {
    case WavEncoding::pcm16:
        bytesPerSample = 2;
        break;
    case WavEncoding::pcm24:
        bytesPerSample = 3;
        break;
    case WavEncoding::float32:
        bytesPerSample = 4;
        break;
    default:
        error = "Unsupported WAV format.";
        return false;
    }
    if (options.encoding == WavEncoding::float32 && options.dither) {
        error = "Dither is only available for integer PCM.";
        return false;
    }
    if (range.end - range.start > maxDataBytes / (channels * bytesPerSample)) {
        error = "Selected range is too long for this WAV format.";
        return false;
    }
    return true;
}

bool validateDiscProgramWav(const Session& session, int channels, WavRange& range,
                            juce::String& error) {
    DiscPreviewPlan plan;
    if (!prepareDiscPreview(session, plan, error)) {
        return false;
    }
    const WavRange candidate{0, plan.samples};
    if (!validateMixRange(session, channels, fullWavRange(session), error) ||
        !validateWavRange(session, channels, candidate, error,
                          {WavEncoding::float32, false, false})) {
        return false;
    }
    std::unordered_map<juce::String, juce::String> metadata;
    juce::int64 cueBytes = 0;
    if (!buildDiscWavCues(session, metadata, cueBytes, error)) {
        return false;
    }
    if (candidate.end >
        (static_cast<juce::int64>(std::numeric_limits<std::uint32_t>::max()) - 4096 - cueBytes) /
            (channels * 4)) {
        error = "Disc program and cue metadata are too long for WAV.";
        return false;
    }
    range = candidate;
    error.clear();
    return true;
}

bool scanExportPreflight(const Session& session, int channels, WavRange range,
                         ExportPreflight& result, juce::String& error, ExportProgress progress,
                         WavOptions options) {
    if (options.sourceChannels) {
        error = "Source-channel rendering does not support mix preflight.";
        return false;
    }
    error.clear();
    if (!validateWavRange(session, channels, range, error, options)) {
        return false;
    }

    const auto keepGoing = [&](double fraction) { return !progress || progress(fraction); };
    if (!keepGoing(0)) {
        error = "Export cancelled.";
        return false;
    }

    ExportPreflight next;
    next.channels = channels;
    const bool integer = options.encoding != WavEncoding::float32;
    const double scale = options.encoding == WavEncoding::pcm16 ? 32768.0 : 8388608.0;

    ResamplingContext context;
    session.prepareResampling(context, sessionRate);
    const RenderClipIndex clipIndex(session);
    juce::AudioBuffer<float> buffer(channels, 4096);
    const auto samples = range.end - range.start;

    for (juce::int64 pos = 0; pos < samples; pos += 4096) {
        if (!keepGoing(static_cast<double>(pos) / static_cast<double>(samples))) {
            error = "Export cancelled.";
            return false;
        }
        const int count = static_cast<int>(std::min<juce::int64>(4096, samples - pos));
        buffer.clear();
        session.render(buffer, 0, count, static_cast<double>(range.start + pos) / sessionRate,
                       sessionRate, channels, &context, {}, -1, &clipIndex);

        for (int ch = 0; ch < channels; ++ch) {
            for (int i = 0; i < count; ++i) {
                const auto sample = buffer.getSample(ch, i);
                if (!std::isfinite(sample)) {
                    error = "Mix contains a non-finite sample; export stopped.";
                    return false;
                }
                next.peak[ch] = std::max(next.peak[ch], std::abs(sample));
                const double scaled = static_cast<double>(sample) * scale +
                                      (options.dither ? tpdfNoise(range.start + pos + i, ch) : 0.0);
                if (integer && pcmSampleClips(sample, scaled, scale)) {
                    ++next.clippedSamples[ch];
                    if (next.firstClipped[ch] < 0) {
                        next.firstClipped[ch] = range.start + pos + i;
                    }
                }
            }
        }
    }

    if (!keepGoing(1)) {
        error = "Export cancelled.";
        return false;
    }
    result = next;
    return true;
}

bool scanPeak(const Session& session, int channels, WavRange range, float& peak,
              juce::String& error, ExportProgress progress, WavOptions options) {
    peak = 0;
    ExportPreflight result;
    if (!scanExportPreflight(session, channels, range, result, error, std::move(progress),
                             options)) {
        return false;
    }
    for (int ch = 0; ch < channels; ++ch) {
        peak = std::max(peak, result.peak[ch]);
    }
    return true;
}

bool scanMixLevels(const Session& session, int channels, WavRange range, MixLevels& result,
                   juce::String& error, ExportProgress progress) {
    error.clear();
    if (!validateMixRange(session, channels, range, error)) {
        return false;
    }
    const auto keepGoing = [&](double fraction) { return !progress || progress(fraction); };
    if (!keepGoing(0)) {
        error = "Level analysis cancelled.";
        return false;
    }

    MixLevels next;
    next.channels = channels;
    double sumSquares[2]{};
    ResamplingContext context;
    session.prepareResampling(context, sessionRate);
    const RenderClipIndex clipIndex(session);
    juce::AudioBuffer<float> buffer(channels, 4096);
    const auto samples = range.end - range.start;

    for (juce::int64 pos = 0; pos < samples; pos += 4096) {
        if (!keepGoing(static_cast<double>(pos) / static_cast<double>(samples))) {
            error = "Level analysis cancelled.";
            return false;
        }
        const int count = static_cast<int>(std::min<juce::int64>(4096, samples - pos));
        buffer.clear();
        session.render(buffer, 0, count, static_cast<double>(range.start + pos) / sessionRate,
                       sessionRate, channels, &context, {}, -1, &clipIndex);

        for (int ch = 0; ch < channels; ++ch) {
            for (int i = 0; i < count; ++i) {
                const auto sample = buffer.getSample(ch, i);
                if (!std::isfinite(sample)) {
                    error = "Mix contains a non-finite sample; analysis stopped.";
                    return false;
                }
                const auto magnitude = std::abs(sample);
                if (magnitude > next.peak[ch]) {
                    next.peak[ch] = magnitude;
                    next.peakSample[ch] = range.start + pos + i;
                }
                if (magnitude > 1.0f) {
                    ++next.aboveFullScale[ch];
                    if (next.firstAboveFullScale[ch] < 0) {
                        next.firstAboveFullScale[ch] = range.start + pos + i;
                    }
                }
                sumSquares[ch] += static_cast<double>(sample) * static_cast<double>(sample);
            }
        }
    }

    if (!keepGoing(1)) {
        error = "Level analysis cancelled.";
        return false;
    }
    for (int ch = 0; ch < channels; ++ch) {
        next.rms[ch] = std::sqrt(sumSquares[ch] / static_cast<double>(samples));
    }
    result = next;
    return true;
}

float measurePeak(const Session& session, int channels, WavRange range) {
    float peak = 0;
    juce::String error;
    scanPeak(session, channels, range, peak, error);
    return peak;
}

float measurePeak(const Session& session, int channels) {
    return measurePeak(session, channels, fullWavRange(session));
}

namespace {

bool exportWavImpl(const Session& session, int channels, WavRange range, const juce::File& target,
                   bool& clipped, juce::String& error, const juce::File& protectedProject,
                   ExportProgress progress, WavOptions wavOptions, bool noReplace,
                   DirectorySyncStatus* directorySync, const DiscPreviewPlan* discPlan) {
    clipped = false;
    const auto keepGoing = [&](double fraction) { return !progress || progress(fraction); };

    DiscPreviewRenderer discRenderer;
    if (discPlan &&
        !prepareDiscPreviewRenderer(session, sessionRate, channels, discRenderer, error)) {
        return false;
    }
    if (!validateWavRange(session, channels, range, error, wavOptions)) {
        return false;
    }
    if (!validateOutputDestination(session, target, protectedProject, error)) {
        return false;
    }
    if (!keepGoing(0)) {
        error = "Export cancelled.";
        return false;
    }

#if JUCE_MAC || JUCE_LINUX
    juce::File temporaryPath;
    int descriptor = -1;
    if (!createExclusiveTempFile(target, ".carta-wav-", 0644, descriptor, temporaryPath)) {
        error = "Could not create export file.";
        return false;
    }
    juce::TemporaryFile temp(target, temporaryPath);
    auto fileStream = std::make_unique<WavFileOutput>(descriptor);
#else
    juce::TemporaryFile temp(target);
    auto fileStream = temp.getFile().createOutputStream();
    if (!fileStream || fileStream->getStatus().failed()) {
        error = "Could not create export file.";
        return false;
    }
#endif

    bool streamFailed = false;
    std::unique_ptr<juce::OutputStream> stream =
        std::make_unique<CheckedOutputStream>(*fileStream, streamFailed);

    juce::WavAudioFormat wav;
    std::unordered_map<juce::String, juce::String> metadata;
    juce::int64 cueBytes = 0;
    if (discPlan
            ? !buildDiscWavCues(session, metadata, cueBytes, error)
            : wavOptions.embedMarkers && !buildWavCues(session, range, metadata, cueBytes, error)) {
        return false;
    }
    if (discPlan && range.end - range.start >
                        (static_cast<juce::int64>(std::numeric_limits<std::uint32_t>::max()) -
                         4096 - cueBytes) /
                            (channels * 4)) {
        error = "Disc program and cue metadata are too long for WAV.";
        return false;
    }

    const int bits = wavOptions.encoding == WavEncoding::pcm16
                         ? 16
                         : (wavOptions.encoding == WavEncoding::pcm24 ? 24 : 32);
    auto options =
        juce::AudioFormatWriterOptions{}
            .withSampleRate(sessionRate)
            .withNumChannels(channels)
            .withBitsPerSample(bits)
            .withSampleFormat(wavOptions.encoding == WavEncoding::float32
                                  ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                  : juce::AudioFormatWriterOptions::SampleFormat::integral)
            .withMetadataValues(metadata);
    if (wavOptions.sourceChannels) {
        options = options.withChannelLayout(juce::AudioChannelSet::discreteChannels(channels));
    }

    auto writer = wav.createWriterFor(stream, options);
    if (!writer) {
        error = "Could not create WAV writer.";
        return false;
    }

    ResamplingContext context;
    session.prepareResampling(context, sessionRate);
    const RenderClipIndex clipIndex(session);
    juce::AudioBuffer<float> buffer(channels, 4096);
    std::vector<std::vector<int>> quantized;
    std::vector<const int*> quantizedPointers;
    if (wavOptions.encoding != WavEncoding::float32) {
        quantized.assign(static_cast<size_t>(channels), std::vector<int>(4096));
        quantizedPointers.resize(static_cast<size_t>(channels));
        for (int ch = 0; ch < channels; ++ch) {
            quantizedPointers[static_cast<size_t>(ch)] = quantized[static_cast<size_t>(ch)].data();
        }
    }

    const auto samples = range.end - range.start;
    bool ok = true;
    bool cancelled = false;
    bool invalid = false;
    for (juce::int64 pos = 0; pos < samples && ok; pos += 4096) {
        if (!keepGoing(static_cast<double>(pos) / static_cast<double>(samples))) {
            cancelled = true;
            break;
        }
        const auto n = static_cast<int>(std::min<juce::int64>(4096, samples - pos));
        buffer.clear();
        if (discPlan) {
            juce::int64 rendered = pos;
            bool unsupported = false;
            renderDiscPreview(session, *discPlan, discRenderer, buffer, 0, n, rendered, sessionRate,
                              channels, &unsupported);
            if (unsupported || rendered != pos + n) {
                error = "Could not render the disc program; destination was not replaced.";
                return false;
            }
        } else {
            session.render(buffer, 0, n, static_cast<double>(range.start + pos) / sessionRate,
                           sessionRate, channels, &context, {}, -1, &clipIndex, nullptr,
                           wavOptions.sourceChannels);
        }

        for (int ch = 0; ch < channels && !invalid; ++ch) {
            for (int i = 0; i < n; ++i) {
                const auto value = buffer.getSample(ch, i);
                if (!std::isfinite(value)) {
                    invalid = true;
                    break;
                }
                if (wavOptions.encoding != WavEncoding::float32) {
                    const auto scale = static_cast<std::int64_t>(1) << (bits - 1);
                    const double scaled =
                        static_cast<double>(value) * static_cast<double>(scale) +
                        (wavOptions.dither ? tpdfNoise(range.start + pos + i, ch) : 0.0);
                    if (pcmSampleClips(value, scaled, static_cast<double>(scale))) {
                        clipped = true;
                    }
                    const double bounded = std::clamp(scaled, -static_cast<double>(scale),
                                                      static_cast<double>(scale - 1));
                    const auto valueInt = static_cast<std::int64_t>(std::llround(bounded));
                    quantized[static_cast<size_t>(ch)][static_cast<size_t>(i)] =
                        static_cast<int>(valueInt * (static_cast<std::int64_t>(1) << (32 - bits)));
                }
            }
        }
        if (invalid) {
            break;
        }
        ok = wavOptions.encoding != WavEncoding::float32
                 ? writer->write(quantizedPointers.data(), n)
                 : writer->writeFromAudioSampleBuffer(buffer, 0, n);
    }

    if (!cancelled && !invalid) {
        ok = writer->flush() && ok;
    }
    writer.reset();

    // JUCE rewrites the WAV header in the writer destructor. Keep the file
    // stream alive to catch failures from that final rewrite and flush.
#if JUCE_MAC || JUCE_LINUX
    if (!fileStream->finish() || streamFailed) {
        ok = false;
    }
#else
    fileStream->flush();
    if (streamFailed || fileStream->getStatus().failed()) {
        ok = false;
    }
#endif
    fileStream.reset();

    if (!cancelled && ok && !keepGoing(0.999999)) {
        cancelled = true;
    }
    if (cancelled) {
        error = "Export cancelled; destination was not replaced.";
        return false;
    }
    if (invalid) {
        error = "Mix contains a non-finite sample; destination was not replaced.";
        return false;
    }
    if (ok) {
        ok = commitTemporary(temp, target, noReplace);
    }
    if (!ok) {
        error = noReplace ? "Export could not commit without replacing the destination. Check the "
                            "destination and filesystem support."
                          : "Export failed; destination was not replaced.";
        return false;
    }

    const auto syncStatus = syncParentDirectory(target);
    if (directorySync) {
        *directorySync = syncStatus;
    }
    if (progress) {
        progress(1);
    }
    return true;
}

bool getMasterTracksImpl(const Session& session, std::vector<MasterTrack>& result,
                         juce::String& error, bool allowUnnamed) {
    const auto total = fullWavRange(session).end;
    if (total <= 0) {
        error = "Nothing to export or invalid master duration.";
        return false;
    }

    struct Boundary {
        juce::int64 sample;
        juce::String name;
    };
    std::vector<Boundary> boundaries;
    for (const auto& marker : session.markers) {
        const double frame = marker.position * sessionRate;
        if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
            error = "Each marker needs a valid position.";
            return false;
        }
        if (!allowUnnamed && marker.name.trim().isEmpty()) {
            error = "Each marker needs a nonempty name.";
            return false;
        }
        if (!markerPositionOnSampleGrid(marker.position)) {
            error = "Repair off-grid markers before using master track boundaries.";
            return false;
        }
        const auto sample = static_cast<juce::int64>(std::llround(frame));
        if (sample >= total) {
            error = "A marker is at or beyond the end of the master.";
            return false;
        }
        boundaries.push_back({sample, marker.name});
    }

    std::sort(boundaries.begin(), boundaries.end(),
              [](const auto& a, const auto& b) { return a.sample < b.sample; });
    for (size_t i = 1; i < boundaries.size(); ++i) {
        if (boundaries[i - 1].sample == boundaries[i].sample) {
            error = "Two markers resolve to the same sample; move or remove one.";
            return false;
        }
    }

    if (boundaries.empty() || boundaries.front().sample != 0) {
        boundaries.insert(boundaries.begin(), {0, "Track 1"});
    }
    std::vector<MasterTrack> next;
    next.reserve(boundaries.size());
    for (size_t i = 0; i < boundaries.size(); ++i) {
        next.push_back(
            {boundaries[i].name,
             {boundaries[i].sample, i + 1 < boundaries.size() ? boundaries[i + 1].sample : total}});
    }
    result = std::move(next);
    return true;
}

} // namespace

bool exportWavRange(const Session& session, int channels, WavRange range, const juce::File& target,
                    bool& clipped, juce::String& error, const juce::File& protectedProject,
                    ExportProgress progress, WavOptions options, bool noReplace,
                    DirectorySyncStatus* directorySync) {
    return exportWavImpl(session, channels, range, target, clipped, error, protectedProject,
                         std::move(progress), options, noReplace, directorySync, nullptr);
}

bool exportDiscProgramWav(const Session& session, int channels, const juce::File& target,
                          juce::String& error, const juce::File& protectedProject,
                          ExportProgress progress, bool noReplace,
                          DirectorySyncStatus* directorySync) {
    WavRange range;
    if (!validateDiscProgramWav(session, channels, range, error)) {
        return false;
    }
    DiscPreviewPlan plan;
    if (!prepareDiscPreview(session, plan, error)) {
        return false;
    }
    bool clipped = false;
    return exportWavImpl(session, channels, range, target, clipped, error, protectedProject,
                         std::move(progress), {WavEncoding::float32, false, false}, noReplace,
                         directorySync, &plan);
}

bool exportWav(const Session& session, int channels, const juce::File& target, bool& clipped,
               juce::String& error) {
    return exportWavRange(session, channels, fullWavRange(session), target, clipped, error);
}

bool getMasterTracks(const Session& session, std::vector<MasterTrack>& result,
                     juce::String& error) {
    return getMasterTracksImpl(session, result, error, false);
}

bool masterTrackAtSample(const Session& session, juce::int64 sample, MasterTrack& result,
                         int& index, juce::String& error) {
    std::vector<MasterTrack> tracks;
    if (!getMasterTracks(session, tracks, error)) {
        return false;
    }
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (sample >= tracks[i].range.start && sample < tracks[i].range.end) {
            result = tracks[i];
            index = static_cast<int>(i);
            return true;
        }
    }
    error = "Place the playhead within the master.";
    return false;
}

bool exportTrackWavs(const Session& session, int channels, const juce::File& parent,
                     TrackBatchResult& result, juce::String& error,
                     const juce::File& protectedProject, ExportProgress progress,
                     WavOptions options) {
    result = {};
    error.clear();
    std::vector<MasterTrack> tracks;
    if (!parent.isDirectory()) {
        error = "Choose an existing destination folder.";
        return false;
    }
    if (!getMasterTracks(session, tracks, error)) {
        return false;
    }
    for (const auto& track : tracks) {
        if (!validateWavRange(session, channels, track.range, error, options)) {
            return false;
        }
    }
    if (progress && !progress(0)) {
        error = "Track export cancelled.";
        return false;
    }

    // Atomic directory creation claims a fresh namespace and never reuses an
    // existing folder, even when several exports start at the same time.
    for (int suffix = 0; suffix < 10000; ++suffix) {
        const auto name = suffix == 0 ? juce::String("Carta tracks")
                                      : "Carta tracks (" + juce::String(suffix + 1) + ")";
        const auto candidate = parent.getChildFile(name);
        std::error_code ec;
        if (std::filesystem::create_directory(
                std::filesystem::u8path(candidate.getFullPathName().toStdString()), ec)) {
            result.directory = candidate;
            break;
        }
        if (ec && ec != std::errc::file_exists) {
            error = "Could not create a new track export folder: " + juce::String(ec.message());
            return false;
        }
    }
    if (result.directory == juce::File{}) {
        error = "Could not find an unused track export folder name.";
        return false;
    }

    const auto mergeSync = [&](DirectorySyncStatus status) {
        if (status == DirectorySyncStatus::unconfirmed ||
            result.directorySync == DirectorySyncStatus::unconfirmed) {
            result.directorySync = DirectorySyncStatus::unconfirmed;
        } else if (status == DirectorySyncStatus::unsupported ||
                   result.directorySync == DirectorySyncStatus::unsupported) {
            result.directorySync = DirectorySyncStatus::unsupported;
        } else {
            result.directorySync = DirectorySyncStatus::confirmed;
        }
    };
    result.directorySync = DirectorySyncStatus::confirmed;
    mergeSync(syncParentDirectory(result.directory));

    const double total = static_cast<double>(tracks.back().range.end);
    double done = 0;
    const int digits =
        std::max(2, static_cast<int>(juce::String(static_cast<int>(tracks.size())).length()));

    for (size_t i = 0; i < tracks.size(); ++i) {
        const auto& track = tracks[i];
        const auto target = result.directory.getChildFile(
            "Track " + juce::String(static_cast<int>(i + 1)).paddedLeft('0', digits) + ".wav");
        if (target.exists()) {
            error = "A track output appeared in the new folder; it was not replaced.";
            return false;
        }
        const double length = static_cast<double>(track.range.end - track.range.start);
        bool clipped = false;
        const auto report = [&](double fraction) {
            return !progress || progress(std::min(0.999999, (done + length * fraction) / total));
        };
        DirectorySyncStatus trackSync = DirectorySyncStatus::unsupported;
        if (!exportWavRange(session, channels, track.range, target, clipped, error,
                            protectedProject, report, options, true, &trackSync)) {
            return false;
        }
        mergeSync(trackSync);
        ++result.completed;
        result.clipped |= clipped;
        done += length;
    }

    if (progress && !progress(0.999999)) {
        error = "Track export cancelled before the manifest was written.";
        return false;
    }
    const auto manifest = result.directory.getChildFile("Carta tracks.json");
    if (manifest.exists()) {
        error = "A manifest appeared in the new folder; it was not replaced.";
        return false;
    }
    if (!exportTrackManifest(session, channels, manifest, error, protectedProject, true)) {
        return false;
    }
    mergeSync(syncParentDirectory(manifest));
    if (progress) {
        progress(1);
    }
    return true;
}

bool exportTrackManifest(const Session& session, int channels, const juce::File& file,
                         juce::String& error, const juce::File& protectedProject, bool noReplace) {
    if (channels != 1 && channels != 2) {
        error = "Invalid master channel count.";
        return false;
    }
    if (session.minimumEndSample < 0 || session.minimumEndSample >= 9007199254740992LL) {
        error = "Invalid minimum master duration.";
        return false;
    }
    if (!validateOutputDestination(session, file, protectedProject, error)) {
        return false;
    }

    std::vector<MasterTrack> masterTracks;
    if (!getMasterTracks(session, masterTracks, error)) {
        return false;
    }
    const auto total = masterTracks.back().range.end;

    auto root = object();
    auto* obj = root.getDynamicObject();
    obj->setProperty("format", "carta-track-manifest");
    obj->setProperty("version", 1);
    obj->setProperty("sampleRate", static_cast<int>(sessionRate));
    obj->setProperty("masterChannels", channels);
    obj->setProperty("totalSamples", total);

    juce::Array<juce::var> tracks;
    for (size_t i = 0; i < masterTracks.size(); ++i) {
        const auto& region = masterTracks[i];
        auto track = object();
        auto* t = track.getDynamicObject();
        t->setProperty("number", static_cast<int>(i + 1));
        t->setProperty("name", region.name);
        t->setProperty("startSample", region.range.start);
        t->setProperty("endSample", region.range.end);
        t->setProperty("lengthSamples", region.range.end - region.range.start);
        tracks.add(track);
    }
    obj->setProperty("tracks", tracks);

    auto temp = prepareCheckedText(file, juce::JSON::toString(root));
    if (!temp || !commitTemporary(*temp, file, noReplace)) {
        error = noReplace ? "Could not save the manifest without replacement. Check the "
                            "destination and filesystem support."
                          : "Could not save track manifest; destination was not replaced.";
        return false;
    }
    return true;
}
namespace {

constexpr juce::int64 maxMarkerFileBytes = 8LL * 1024 * 1024;
constexpr int maxExchangeMarkers = 10000;

bool validateExchangeMarkers(const Session& session, std::vector<Marker>& markers,
                             juce::String& error, bool requireAligned, bool requireName = true) {
    const auto total = projectEndSample(session);
    if (total <= 0) {
        error = "Import audio before working with track markers.";
        return false;
    }
    if (markers.size() > static_cast<size_t>(maxExchangeMarkers)) {
        error = "Marker file contains too many markers.";
        return false;
    }
    for (auto& marker : markers) {
        const double frame = marker.position * sessionRate;
        if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0 ||
            (requireName && marker.name.trim().isEmpty())) {
            error = requireName ? "Each marker needs a nonempty name and a valid sample position."
                                : "Each marker needs a valid sample position.";
            return false;
        }
        if (requireAligned && !markerPositionOnSampleGrid(marker.position)) {
            error = "Repair off-grid markers before exporting them.";
            return false;
        }
        const auto sample = static_cast<juce::int64>(std::llround(frame));
        if (sample >= total) {
            error = "A marker is at or beyond the end of the master.";
            return false;
        }
        marker.position = static_cast<double>(sample) / sessionRate;
    }
    std::sort(markers.begin(), markers.end(),
              [](const Marker& a, const Marker& b) { return a.position < b.position; });
    for (size_t i = 1; i < markers.size(); ++i) {
        if (std::llround(markers[i - 1].position * sessionRate) ==
            std::llround(markers[i].position * sessionRate)) {
            error = "Two markers resolve to the same sample.";
            return false;
        }
    }
    return true;
}

bool integerValue(const juce::var& value, juce::int64& result) {
    if (!value.isInt() && !value.isInt64()) {
        return false;
    }
    result = static_cast<juce::int64>(value);
    return true;
}

constexpr size_t maxDiscTextBytes = 65536;

bool validDiscText(const std::string& text) {
    return text.size() <= maxDiscTextBytes && text.find('\0') == std::string::npos &&
           juce::CharPointer_UTF8::isValidString(text.data(), static_cast<int>(text.size()));
}

bool encodeMdDisc(const MdDisc& disc, juce::var& output, juce::String& error) {
    std::string reason;
    if (!validateMdDisc(disc, reason)) {
        error = "Invalid MD disc: " + juce::String::fromUTF8(reason.c_str());
        return false;
    }
    if (!validDiscText(disc.halfWidthTitle) || !validDiscText(disc.fullWidthTitle) ||
        !validDiscText(disc.discTimestamp)) {
        error = "MD disc titles or timestamp contain invalid UTF-8 text.";
        return false;
    }

    auto value = object();
    auto* props = value.getDynamicObject();
    props->setProperty("medium", juce::String(mdMediumToString(disc.medium)));
    props->setProperty("defaultCodec", juce::String(mdCodecModeToString(disc.defaultCodec)));
    props->setProperty("autoTrackOnImport", disc.autoTrackOnImport);
    if (!disc.halfWidthTitle.empty()) {
        props->setProperty("halfWidthTitle", juce::String::fromUTF8(disc.halfWidthTitle.c_str()));
    }
    if (!disc.fullWidthTitle.empty()) {
        props->setProperty("fullWidthTitle", juce::String::fromUTF8(disc.fullWidthTitle.c_str()));
    }
    if (!disc.discTimestamp.empty()) {
        props->setProperty("discTimestamp", juce::String::fromUTF8(disc.discTimestamp.c_str()));
    }

    juce::Array<juce::var> tracks;
    for (const auto& track : disc.tracks) {
        if (!validDiscText(track.id) || !validDiscText(track.audioId) ||
            !validDiscText(track.halfWidthTitle) || !validDiscText(track.fullWidthTitle) ||
            !validDiscText(track.artist) || !validDiscText(track.album) ||
            !validDiscText(track.timestamp)) {
            error = "MD track text contains invalid UTF-8 text.";
            return false;
        }
        auto item = object();
        auto* p = item.getDynamicObject();
        p->setProperty("id", juce::String::fromUTF8(track.id.c_str()));
        if (!track.audioId.empty()) {
            p->setProperty("audioId", juce::String::fromUTF8(track.audioId.c_str()));
        }
        if (!track.halfWidthTitle.empty()) {
            p->setProperty("halfWidthTitle", juce::String::fromUTF8(track.halfWidthTitle.c_str()));
        }
        if (!track.fullWidthTitle.empty()) {
            p->setProperty("fullWidthTitle", juce::String::fromUTF8(track.fullWidthTitle.c_str()));
        }
        if (!track.artist.empty()) {
            p->setProperty("artist", juce::String::fromUTF8(track.artist.c_str()));
        }
        if (!track.album.empty()) {
            p->setProperty("album", juce::String::fromUTF8(track.album.c_str()));
        }
        if (track.codecOverride.has_value()) {
            p->setProperty("codecOverride",
                           juce::String(mdCodecModeToString(*track.codecOverride)));
        }
        if (track.writeProtected) {
            p->setProperty("writeProtected", true);
        }
        if (track.scmsCopyProtected) {
            p->setProperty("scmsCopyProtected", true);
        }
        if (track.emphasis) {
            p->setProperty("emphasis", true);
        }
        if (track.forceGapBefore) {
            p->setProperty("forceGapBefore", true);
        }
        if (!track.timestamp.empty()) {
            p->setProperty("timestamp", juce::String::fromUTF8(track.timestamp.c_str()));
        }

        juce::Array<juce::var> parts;
        for (const auto& part : track.parts) {
            auto pt = object();
            pt.getDynamicObject()->setProperty("start", static_cast<juce::int64>(part.startSample));
            pt.getDynamicObject()->setProperty("end", static_cast<juce::int64>(part.endSample));
            parts.add(pt);
        }
        p->setProperty("parts", parts);
        tracks.add(item);
    }
    props->setProperty("tracks", tracks);

    if (!disc.groups.empty()) {
        juce::Array<juce::var> groups;
        for (const auto& group : disc.groups) {
            if (!validDiscText(group.halfWidthTitle) || !validDiscText(group.fullWidthTitle)) {
                error = "MD group title contains invalid UTF-8 text.";
                return false;
            }
            auto item = object();
            auto* g = item.getDynamicObject();
            g->setProperty("startTrack", group.startTrack);
            g->setProperty("endTrack", group.endTrack);
            if (!group.halfWidthTitle.empty()) {
                g->setProperty("halfWidthTitle",
                               juce::String::fromUTF8(group.halfWidthTitle.c_str()));
            }
            if (!group.fullWidthTitle.empty()) {
                g->setProperty("fullWidthTitle",
                               juce::String::fromUTF8(group.fullWidthTitle.c_str()));
            }
            groups.add(item);
        }
        props->setProperty("groups", groups);
    }

    auto encodeAddrVar = [](const MdPhysAddr& a) {
        auto v = object();
        auto* o = v.getDynamicObject();
        o->setProperty("cluster", static_cast<int>(a.cluster));
        o->setProperty("sector", static_cast<int>(a.sector));
        o->setProperty("group", static_cast<int>(a.group));
        return v;
    };
    auto encodeRangeArray = [&encodeAddrVar](const std::vector<MdPhysRange>& ranges) {
        juce::Array<juce::var> arr;
        for (const auto& r : ranges) {
            auto v = object();
            auto* o = v.getDynamicObject();
            o->setProperty("start", encodeAddrVar(r.start));
            o->setProperty("end", encodeAddrVar(r.end));
            arr.add(v);
        }
        return arr;
    };

    const bool hasUtoc =
        std::any_of(disc.importedUtocSectors.begin(), disc.importedUtocSectors.end(),
                    [](const auto& s) { return !s.empty(); });
    if (hasUtoc) {
        juce::Array<juce::var> sectors;
        for (const auto& sector : disc.importedUtocSectors) {
            sectors.add(sector.empty() ? juce::String()
                                       : juce::Base64::toBase64(sector.data(), sector.size()));
        }
        props->setProperty("importedUtocSectors", sectors);
    }

    if (disc.loadedDisc.active) {
        const auto& ld = disc.loadedDisc;
        if (!validDiscText(ld.sourceDeviceId) || !validDiscText(ld.baselineSector0Hash)) {
            error = "Loaded MiniDisc metadata contains invalid UTF-8 text.";
            return false;
        }
        auto loadedVar = object();
        auto* lo = loadedVar.getDynamicObject();
        lo->setProperty("active", true);
        lo->setProperty("sourceDeviceId", juce::String::fromUTF8(ld.sourceDeviceId.c_str()));
        lo->setProperty("baselineSector0Hash",
                        juce::String::fromUTF8(ld.baselineSector0Hash.c_str()));
        lo->setProperty("deviceSignature", static_cast<int>(ld.deviceSignature));
        lo->setProperty("physicalStartCluster", static_cast<int>(ld.physicalStartCluster));
        lo->setProperty("physicalLastCluster", static_cast<int>(ld.physicalLastCluster));

        juce::Array<juce::var> regs;
        for (const auto& r : ld.regions) {
            if (!validDiscText(r.id)) {
                error = "Loaded MiniDisc region ID contains invalid UTF-8 text.";
                return false;
            }
            auto rv = object();
            auto* ro = rv.getDynamicObject();
            ro->setProperty("id", juce::String::fromUTF8(r.id.c_str()));
            ro->setProperty("codec", juce::String(mdCodecModeToString(r.codec)));
            ro->setProperty("rawModeByte", static_cast<int>(r.rawModeByte));
            ro->setProperty("physStart", encodeAddrVar(r.physStart));
            ro->setProperty("physEnd", encodeAddrVar(r.physEnd));
            ro->setProperty("unitCount", r.unitCount);
            ro->setProperty("timelineStartSample", static_cast<juce::int64>(r.timelineStartSample));
            ro->setProperty("timelineEndSample", static_cast<juce::int64>(r.timelineEndSample));
            ro->setProperty("audioDownloaded", r.audioDownloaded);
            if (r.hiMdSlot > 0) {
                ro->setProperty("hiMdSlot", r.hiMdSlot);
            }
            regs.add(rv);
        }
        lo->setProperty("regions", regs);
        lo->setProperty("initialFreeRanges", encodeRangeArray(ld.initialFreeRanges));
        lo->setProperty("defectiveRanges", encodeRangeArray(ld.defectiveRanges));
        if (ld.rawSector3.size() == kUtocSectorBytes) {
            lo->setProperty("rawSector3",
                            juce::Base64::toBase64(ld.rawSector3.data(), ld.rawSector3.size()));
        }
        props->setProperty("loadedDisc", loadedVar);
    }

    output = std::move(value);
    return true;
}

bool decodeMdDisc(const juce::var& value, MdDisc& output, juce::String& error) {
    if (!value.isObject() || !value["tracks"].isArray()) {
        error = "Invalid MD disc structure.";
        return false;
    }
    const auto* props = value.getDynamicObject();
    MdDisc next;

    if (props->hasProperty("medium")) {
        if (!value["medium"].isString() ||
            !parseMdMedium(value["medium"].toString().toStdString(), next.medium)) {
            error = "Invalid MD medium.";
            return false;
        }
    }
    if (props->hasProperty("defaultCodec")) {
        if (!value["defaultCodec"].isString() ||
            !parseMdCodecMode(value["defaultCodec"].toString().toStdString(), next.defaultCodec)) {
            error = "Invalid MD default codec.";
            return false;
        }
    }
    if (props->hasProperty("autoTrackOnImport")) {
        if (!value["autoTrackOnImport"].isBool()) {
            error = "Invalid autoTrackOnImport flag.";
            return false;
        }
        next.autoTrackOnImport = static_cast<bool>(value["autoTrackOnImport"]);
    }
    if (props->hasProperty("halfWidthTitle")) {
        if (!value["halfWidthTitle"].isString()) {
            error = "Invalid MD disc title.";
            return false;
        }
        next.halfWidthTitle = value["halfWidthTitle"].toString().toStdString();
    }
    if (props->hasProperty("fullWidthTitle")) {
        if (!value["fullWidthTitle"].isString()) {
            error = "Invalid MD disc full-width title.";
            return false;
        }
        next.fullWidthTitle = value["fullWidthTitle"].toString().toStdString();
    }
    if (props->hasProperty("discTimestamp")) {
        if (!value["discTimestamp"].isString()) {
            error = "Invalid MD disc timestamp.";
            return false;
        }
        next.discTimestamp = value["discTimestamp"].toString().toStdString();
    }

    const auto* tracksArr = value["tracks"].getArray();
    if (tracksArr->size() > mdMaxTracks(next.medium)) {
        error = "Too many MD tracks.";
        return false;
    }
    for (const auto& item : *tracksArr) {
        if (!item.isObject() || !item["id"].isString() || !item["parts"].isArray()) {
            error = "Invalid MD track entry.";
            return false;
        }
        const auto* tp = item.getDynamicObject();
        MdTrack trk;
        trk.id = item["id"].toString().toStdString();
        if (tp->hasProperty("audioId")) {
            if (!item["audioId"].isString()) {
                error = "Invalid MD track audioId.";
                return false;
            }
            trk.audioId = item["audioId"].toString().toStdString();
        }
        if (tp->hasProperty("halfWidthTitle")) {
            if (!item["halfWidthTitle"].isString()) {
                error = "Invalid MD track title.";
                return false;
            }
            trk.halfWidthTitle = item["halfWidthTitle"].toString().toStdString();
        }
        if (tp->hasProperty("fullWidthTitle")) {
            if (!item["fullWidthTitle"].isString()) {
                error = "Invalid MD track full-width title.";
                return false;
            }
            trk.fullWidthTitle = item["fullWidthTitle"].toString().toStdString();
        }
        if (tp->hasProperty("artist")) {
            if (!item["artist"].isString()) {
                error = "Invalid MD track artist.";
                return false;
            }
            trk.artist = item["artist"].toString().toStdString();
        }
        if (tp->hasProperty("album")) {
            if (!item["album"].isString()) {
                error = "Invalid MD track album.";
                return false;
            }
            trk.album = item["album"].toString().toStdString();
        }
        if (tp->hasProperty("codecOverride")) {
            MdCodecMode ov{};
            if (!item["codecOverride"].isString() ||
                !parseMdCodecMode(item["codecOverride"].toString().toStdString(), ov)) {
                error = "Invalid MD track codec override.";
                return false;
            }
            trk.codecOverride = ov;
        }
        if (tp->hasProperty("writeProtected")) {
            if (!item["writeProtected"].isBool()) {
                error = "Invalid MD track writeProtected flag.";
                return false;
            }
            trk.writeProtected = static_cast<bool>(item["writeProtected"]);
        }
        if (tp->hasProperty("scmsCopyProtected")) {
            if (!item["scmsCopyProtected"].isBool()) {
                error = "Invalid MD track SCMS flag.";
                return false;
            }
            trk.scmsCopyProtected = static_cast<bool>(item["scmsCopyProtected"]);
        }
        if (tp->hasProperty("emphasis")) {
            if (!item["emphasis"].isBool()) {
                error = "Invalid MD track emphasis flag.";
                return false;
            }
            trk.emphasis = static_cast<bool>(item["emphasis"]);
        }
        if (tp->hasProperty("forceGapBefore")) {
            if (!item["forceGapBefore"].isBool()) {
                error = "Invalid MD track forceGapBefore flag.";
                return false;
            }
            trk.forceGapBefore = static_cast<bool>(item["forceGapBefore"]);
        }
        if (tp->hasProperty("timestamp")) {
            if (!item["timestamp"].isString()) {
                error = "Invalid MD track timestamp.";
                return false;
            }
            trk.timestamp = item["timestamp"].toString().toStdString();
        }
        for (const auto& pv : *item["parts"].getArray()) {
            juce::int64 s = 0;
            juce::int64 e = 0;
            if (!pv.isObject() || !integerValue(pv["start"], s) || !integerValue(pv["end"], e) ||
                s < 0 || e <= s || e >= 9007199254740992LL) {
                error = "Invalid MD track part bounds.";
                return false;
            }
            trk.parts.push_back({s, e});
        }
        next.tracks.push_back(std::move(trk));
    }

    if (props->hasProperty("groups")) {
        if (!value["groups"].isArray()) {
            error = "Invalid MD groups array.";
            return false;
        }
        for (const auto& gv : *value["groups"].getArray()) {
            juce::int64 s = 0;
            juce::int64 e = 0;
            if (!gv.isObject() || !integerValue(gv["startTrack"], s) ||
                !integerValue(gv["endTrack"], e)) {
                error = "Invalid MD group entry.";
                return false;
            }
            const auto* gp = gv.getDynamicObject();
            MdGroup grp;
            grp.startTrack = static_cast<int>(s);
            grp.endTrack = static_cast<int>(e);
            if (gp->hasProperty("halfWidthTitle")) {
                if (!gv["halfWidthTitle"].isString()) {
                    error = "Invalid MD group title.";
                    return false;
                }
                grp.halfWidthTitle = gv["halfWidthTitle"].toString().toStdString();
            }
            if (gp->hasProperty("fullWidthTitle")) {
                if (!gv["fullWidthTitle"].isString()) {
                    error = "Invalid MD group full-width title.";
                    return false;
                }
                grp.fullWidthTitle = gv["fullWidthTitle"].toString().toStdString();
            }
            next.groups.push_back(std::move(grp));
        }
    }

    if (props->hasProperty("importedUtocSectors")) {
        const auto sectors = value["importedUtocSectors"];
        if (!sectors.isArray() || sectors.size() != 4) {
            error = "Invalid imported UTOC sector array.";
            return false;
        }
        for (int i = 0; i < 4; ++i) {
            const auto& entry = (*sectors.getArray())[i];
            if (!entry.isString()) {
                error = "Invalid imported UTOC sector encoding.";
                return false;
            }
            const auto s = entry.toString();
            if (s.isEmpty()) {
                continue;
            }
            juce::MemoryOutputStream decoded;
            if (!juce::Base64::convertFromBase64(decoded, s) ||
                decoded.getDataSize() != kUtocSectorBytes) {
                error = "Invalid imported UTOC sector size.";
                return false;
            }
            const auto* raw = static_cast<const std::uint8_t*>(decoded.getData());
            next.importedUtocSectors[static_cast<size_t>(i)].assign(raw, raw + kUtocSectorBytes);
        }
    }

    if (props->hasProperty("loadedDisc")) {
        const auto ldv = value["loadedDisc"];
        if (!ldv.isObject() || !ldv["active"].isBool()) {
            error = "Invalid loadedDisc state.";
            return false;
        }
        if (static_cast<bool>(ldv["active"])) {
            const auto* lo = ldv.getDynamicObject();
            auto parseAddrVar = [&](const juce::var& v, MdPhysAddr& out) -> bool {
                juce::int64 cl = 0, sec = 0, grp = 0;
                if (!v.isObject() || !integerValue(v["cluster"], cl) ||
                    !integerValue(v["sector"], sec) || !integerValue(v["group"], grp) ||
                    cl < 0 || cl > 16383 || sec < 0 || sec >= kMdSectorsPerCluster || grp < 0 ||
                    grp >= kMdUnitsPerSector) {
                    error = "Invalid MD physical address in loadedDisc.";
                    return false;
                }
                out = {static_cast<std::uint16_t>(cl), static_cast<std::uint8_t>(sec),
                       static_cast<std::uint8_t>(grp)};
                return true;
            };
            auto parseRangeList = [&](const char* prop, std::vector<MdPhysRange>& out) -> bool {
                if (!lo->hasProperty(prop)) {
                    return true;
                }
                if (!ldv[prop].isArray()) {
                    error = "Invalid range list in loadedDisc.";
                    return false;
                }
                for (const auto& rv : *ldv[prop].getArray()) {
                    MdPhysRange r;
                    if (!rv.isObject() || !parseAddrVar(rv["start"], r.start) ||
                        !parseAddrVar(rv["end"], r.end)) {
                        return false;
                    }
                    out.push_back(r);
                }
                return true;
            };

            auto& ld = next.loadedDisc;
            ld.active = true;
            if (lo->hasProperty("sourceDeviceId")) {
                if (!ldv["sourceDeviceId"].isString()) {
                    error = "Invalid loadedDisc sourceDeviceId.";
                    return false;
                }
                ld.sourceDeviceId = ldv["sourceDeviceId"].toString().toStdString();
            }
            if (lo->hasProperty("baselineSector0Hash")) {
                if (!ldv["baselineSector0Hash"].isString()) {
                    error = "Invalid loadedDisc baselineSector0Hash.";
                    return false;
                }
                ld.baselineSector0Hash = ldv["baselineSector0Hash"].toString().toStdString();
            }
            juce::int64 sig = 0, pStart = 50, pLast = 2251;
            if ((lo->hasProperty("deviceSignature") &&
                 (!integerValue(ldv["deviceSignature"], sig) || sig < 0 || sig > 65535)) ||
                !integerValue(ldv["physicalStartCluster"], pStart) || pStart < 1 ||
                pStart > 16383 || !integerValue(ldv["physicalLastCluster"], pLast) ||
                pLast < pStart || pLast > 16383) {
                error = "Invalid loadedDisc geometry or signature.";
                return false;
            }
            ld.deviceSignature = static_cast<std::uint16_t>(sig);
            ld.physicalStartCluster = static_cast<std::uint16_t>(pStart);
            ld.physicalLastCluster = static_cast<std::uint16_t>(pLast);

            if (!ldv["regions"].isArray()) {
                error = "Invalid loadedDisc regions array.";
                return false;
            }
            for (const auto& rv : *ldv["regions"].getArray()) {
                if (!rv.isObject() || !rv["id"].isString() || !rv["codec"].isString()) {
                    error = "Invalid loadedDisc region entry.";
                    return false;
                }
                MdOnDiscRegion reg;
                reg.id = rv["id"].toString().toStdString();
                if (!parseMdCodecMode(rv["codec"].toString().toStdString(), reg.codec)) {
                    error = "Invalid codec in loadedDisc region.";
                    return false;
                }
                juce::int64 rawMode = 0, units = 0, ts = 0, te = 0;
                if (!integerValue(rv["rawModeByte"], rawMode) || rawMode < 0 || rawMode > 255 ||
                    !parseAddrVar(rv["physStart"], reg.physStart) ||
                    !parseAddrVar(rv["physEnd"], reg.physEnd) ||
                    !integerValue(rv["unitCount"], units) ||
                    (isHiMdMedium(next.medium) ? (units < 0) : (units <= 0)) ||
                    !integerValue(rv["timelineStartSample"], ts) || ts < 0 ||
                    !integerValue(rv["timelineEndSample"], te) || te <= ts ||
                    te >= 9007199254740992LL) {
                    error = "Invalid numeric bounds in loadedDisc region.";
                    return false;
                }
                reg.rawModeByte = static_cast<std::uint8_t>(rawMode);
                reg.unitCount = static_cast<int>(units);
                reg.timelineStartSample = ts;
                reg.timelineEndSample = te;
                if (rv.getDynamicObject()->hasProperty("audioDownloaded")) {
                    if (!rv["audioDownloaded"].isBool()) {
                        error = "Invalid audioDownloaded flag in loadedDisc region.";
                        return false;
                    }
                    reg.audioDownloaded = static_cast<bool>(rv["audioDownloaded"]);
                }
                if (rv.getDynamicObject()->hasProperty("hiMdSlot")) {
                    juce::int64 hiMdSlot = 0;
                    if (!integerValue(rv["hiMdSlot"], hiMdSlot) || hiMdSlot < 0 ||
                        hiMdSlot > kHiMdMaxAudioSlots) {
                        error = "Invalid hiMdSlot in loadedDisc region.";
                        return false;
                    }
                    reg.hiMdSlot = static_cast<int>(hiMdSlot);
                }
                ld.regions.push_back(std::move(reg));
            }
            if (!parseRangeList("initialFreeRanges", ld.initialFreeRanges) ||
                !parseRangeList("defectiveRanges", ld.defectiveRanges)) {
                return false;
            }
            if (lo->hasProperty("rawSector3")) {
                if (!ldv["rawSector3"].isString()) {
                    error = "Invalid loadedDisc rawSector3.";
                    return false;
                }
                const auto s = ldv["rawSector3"].toString();
                if (s.isNotEmpty()) {
                    juce::MemoryOutputStream decoded;
                    if (!juce::Base64::convertFromBase64(decoded, s) ||
                        decoded.getDataSize() != kUtocSectorBytes) {
                        error = "Invalid loadedDisc rawSector3 size.";
                        return false;
                    }
                    const auto* raw = static_cast<const std::uint8_t*>(decoded.getData());
                    ld.rawSector3.assign(raw, raw + kUtocSectorBytes);
                }
            }
        }
    }

    std::string reason;
    if (!validateMdDisc(next, reason)) {
        error = "Invalid MD disc: " + juce::String::fromUTF8(reason.c_str());
        return false;
    }
    output = std::move(next);
    return true;
}

bool continueMarkerExchange(const MarkerExchangeContinue& keepGoing, juce::String& error) {
    if (!keepGoing || keepGoing()) {
        return true;
    }
    error = "Marker exchange cancelled.";
    return false;
}

bool readMarkerBytes(const juce::File& file, bool allowEmpty, juce::MemoryBlock& bytes,
                     juce::String& error, const MarkerExchangeContinue& keepGoing) {
    if (!continueMarkerExchange(keepGoing, error)) {
        return false;
    }
    const auto size = file.getSize();
    if (!file.existsAsFile() || size < 0 || (!allowEmpty && size == 0) ||
        size > maxMarkerFileBytes) {
        error = "Cannot read marker file, or it exceeds 8 MiB.";
        return false;
    }
    const FileObservation initial(file);
    if (!initial.isValid()) {
        error = "Could not read marker file.";
        return false;
    }
    auto input = file.createInputStream();
    if (!input || input->getStatus().failed()) {
        error = "Could not read marker file.";
        return false;
    }
    input->readIntoMemoryBlock(bytes, static_cast<std::ptrdiff_t>(maxMarkerFileBytes + 1));
    if (!continueMarkerExchange(keepGoing, error)) {
        return false;
    }
    if (input->getStatus().failed() || bytes.getSize() > static_cast<size_t>(maxMarkerFileBytes)) {
        error = "Could not read marker file, or it exceeds 8 MiB.";
        return false;
    }
    if (static_cast<juce::int64>(bytes.getSize()) != size || !initial.matches(file)) {
        error = "Marker file changed while it was being read.";
        return false;
    }
    return true;
}

bool parseLabelTime(const juce::String& input, juce::int64& whole, juce::int64& nanos,
                    juce::int64& sample) {
    const auto s = input.trim().toStdString();
    if (s.empty()) {
        return false;
    }
    size_t i = 0;
    juce::int64 seconds = 0;
    juce::int64 fraction = 0;
    int digits = 0;

    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        const int d = s[i++] - '0';
        if (seconds > (9007199254740991LL / 44100 - d) / 10) {
            return false;
        }
        seconds = seconds * 10 + d;
    }
    if (i == 0) {
        return false;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            if (++digits > 9) {
                return false;
            }
            fraction = fraction * 10 + (s[i++] - '0');
        }
        if (digits == 0) {
            return false;
        }
    }
    if (i != s.size()) {
        return false;
    }

    for (; digits < 9; ++digits) {
        fraction *= 10;
    }
    const auto frames = seconds * 44100 + (fraction * 44100 + 500000000) / 1000000000;
    if (frames >= 9007199254740992LL) {
        return false;
    }
    whole = seconds;
    nanos = fraction;
    sample = frames;
    return true;
}

} // namespace

bool readMarkerFile(const Session& session, const juce::File& file, std::vector<Marker>& result,
                    juce::String& error, MarkerExchangeContinue keepGoing) {
    juce::MemoryBlock bytes;
    if (!readMarkerBytes(file, false, bytes, error, keepGoing)) {
        return false;
    }
    const auto* data = static_cast<const char*>(bytes.getData());
    if (!data || std::memchr(data, 0, bytes.getSize()) ||
        !juce::CharPointer_UTF8::isValidString(data, static_cast<int>(bytes.getSize()))) {
        error = "Marker file must be UTF-8 text.";
        return false;
    }
    if (!safeJsonNumbers(data, bytes.getSize())) {
        error = "Invalid numeric value in marker file.";
        return false;
    }

    const auto text = juce::String::fromUTF8(data, static_cast<int>(bytes.getSize()));
    juce::var root;
    juce::int64 version = 0;
    juce::int64 rate = 0;
    if (juce::JSON::parse(text, root).failed() || !root.isObject() || !root["format"].isString() ||
        root["format"].toString() != "carta-markers" || !integerValue(root["version"], version) ||
        version != 1 || !integerValue(root["sampleRate"], rate) ||
        rate != static_cast<juce::int64>(sessionRate) || !root["markers"].isArray() ||
        root["markers"].getArray()->size() > maxExchangeMarkers) {
        error = "Invalid or unsupported Carta marker file.";
        return false;
    }

    std::vector<Marker> next;
    next.reserve(static_cast<size_t>(root["markers"].getArray()->size()));
    for (const auto& item : *root["markers"].getArray()) {
        if (!continueMarkerExchange(keepGoing, error)) {
            return false;
        }
        juce::int64 sample = 0;
        if (!item.isObject() || !item["name"].isString() || !integerValue(item["sample"], sample) ||
            sample < 0 || sample >= 9007199254740992LL) {
            error = "Each marker must have a name and a nonnegative integer sample.";
            return false;
        }
        next.push_back({static_cast<double>(sample) / sessionRate, item["name"].toString()});
    }

    if (!validateExchangeMarkers(session, next, error, false)) {
        return false;
    }
    if (!continueMarkerExchange(keepGoing, error)) {
        return false;
    }
    result = std::move(next);
    return true;
}

bool writeMarkerFile(const Session& session, const juce::File& file, juce::String& error,
                     const juce::File& protectedProject, bool noReplace,
                     MarkerExchangeContinue keepGoing) {
    if (!continueMarkerExchange(keepGoing, error)) {
        return false;
    }
    if (!validateOutputDestination(session, file, protectedProject, error)) {
        return false;
    }
    auto markers = session.markers;
    if (!validateExchangeMarkers(session, markers, error, true)) {
        return false;
    }

    auto root = object();
    auto* properties = root.getDynamicObject();
    properties->setProperty("format", "carta-markers");
    properties->setProperty("version", 1);
    properties->setProperty("sampleRate", static_cast<int>(sessionRate));

    juce::Array<juce::var> items;
    for (const auto& marker : markers) {
        if (!continueMarkerExchange(keepGoing, error)) {
            return false;
        }
        auto item = object();
        item.getDynamicObject()->setProperty(
            "sample", static_cast<juce::int64>(std::llround(marker.position * sessionRate)));
        item.getDynamicObject()->setProperty("name", marker.name);
        items.add(item);
    }
    properties->setProperty("markers", items);

    const auto text = juce::JSON::toString(root);
    if (static_cast<juce::int64>(text.getNumBytesAsUTF8()) > maxMarkerFileBytes) {
        error = "Marker file would exceed 8 MiB.";
        return false;
    }
    if (!continueMarkerExchange(keepGoing, error)) {
        return false;
    }
    auto temp = prepareCheckedText(file, text);
    if (!continueMarkerExchange(keepGoing, error)) {
        return false;
    }
    if (!temp || !commitTemporary(*temp, file, noReplace)) {
        error = noReplace ? "Could not save markers without replacing an existing file."
                          : "Could not save markers; destination was not replaced.";
        return false;
    }
    return true;
}

bool readAudacityLabels(const Session& session, const juce::File& file, std::vector<Marker>& result,
                        juce::String& error, MarkerExchangeContinue keepGoing) {
    juce::MemoryBlock bytes;
    if (!readMarkerBytes(file, true, bytes, error, keepGoing)) {
        return false;
    }
    const auto* data = static_cast<const char*>(bytes.getData());
    if (bytes.getSize() &&
        (!data || std::memchr(data, 0, bytes.getSize()) ||
         !juce::CharPointer_UTF8::isValidString(data, static_cast<int>(bytes.getSize())))) {
        error = "Labels must be UTF-8 text.";
        return false;
    }

    size_t prefix = 0;
    if (bytes.getSize() >= 3 && static_cast<unsigned char>(data[0]) == 0xef &&
        static_cast<unsigned char>(data[1]) == 0xbb &&
        static_cast<unsigned char>(data[2]) == 0xbf) {
        prefix = 3;
    }
    const auto text =
        bytes.getSize() > prefix
            ? juce::String::fromUTF8(data + prefix, static_cast<int>(bytes.getSize() - prefix))
            : juce::String();

    juce::StringArray lines;
    lines.addLines(text);
    std::vector<Marker> next;
    for (int i = 0; i < lines.size(); ++i) {
        if (!continueMarkerExchange(keepGoing, error)) {
            return false;
        }
        const auto line = lines[i];
        if (line.isEmpty()) {
            continue;
        }
        const int first = line.indexOfChar('\t');
        const int second = first < 0 ? -1 : line.indexOfChar(first + 1, '\t');
        if (first < 0 || (second >= 0 && line.indexOfChar(second + 1, '\t') >= 0)) {
            error = "Label line " + juce::String(i + 1) +
                    " must contain start and end seconds, with an optional name, separated by "
                    "tabs; extended labels are not supported.";
            return false;
        }
        juce::int64 sec1 = 0;
        juce::int64 sec2 = 0;
        juce::int64 ns1 = 0;
        juce::int64 ns2 = 0;
        juce::int64 sample1 = 0;
        juce::int64 sample2 = 0;
        const auto endField =
            second < 0 ? line.substring(first + 1) : line.substring(first + 1, second);
        if (!parseLabelTime(line.substring(0, first), sec1, ns1, sample1) ||
            !parseLabelTime(endField, sec2, ns2, sample2)) {
            error = "Invalid label time on line " + juce::String(i + 1) +
                    "; use nonnegative decimal seconds with up to nine decimal places.";
            return false;
        }
        if (sec1 != sec2 || ns1 != ns2) {
            error = "Region labels are not supported (line " + juce::String(i + 1) + ").";
            return false;
        }
        if (next.size() >= static_cast<size_t>(maxExchangeMarkers)) {
            error = "Label file contains too many markers.";
            return false;
        }
        next.push_back({static_cast<double>(sample1) / sessionRate,
                        second < 0 ? juce::String() : line.substring(second + 1)});
    }

    if (!validateExchangeMarkers(session, next, error, false, false)) {
        return false;
    }
    if (!continueMarkerExchange(keepGoing, error)) {
        return false;
    }
    result = std::move(next);
    return true;
}

bool writeAudacityLabels(const Session& session, const juce::File& file, juce::String& error,
                         const juce::File& protectedProject, bool noReplace,
                         MarkerExchangeContinue keepGoing) {
    if (!continueMarkerExchange(keepGoing, error)) {
        return false;
    }
    if (!validateOutputDestination(session, file, protectedProject, error)) {
        return false;
    }
    auto markers = session.markers;
    if (!validateExchangeMarkers(session, markers, error, true, false)) {
        return false;
    }

    juce::StringArray lines;
    size_t totalBytes = 0;
    for (const auto& marker : markers) {
        if (!continueMarkerExchange(keepGoing, error)) {
            return false;
        }
        if (marker.name.containsAnyOf("\t\r\n")) {
            error = "Marker names cannot contain tabs or line breaks in Audacity labels.";
            return false;
        }
        const auto sample = static_cast<juce::int64>(std::llround(marker.position * sessionRate));
        const auto seconds = sample / 44100;
        const auto nanos = ((sample % 44100) * 1000000000LL + 22050) / 44100;
        const auto time = juce::String(seconds) + "." + juce::String(nanos).paddedLeft('0', 9);
        auto line = time + "\t" + time + "\t" + marker.name + "\n";
        totalBytes += line.getNumBytesAsUTF8();
        if (totalBytes > static_cast<size_t>(maxMarkerFileBytes)) {
            error = "Label file would exceed 8 MiB.";
            return false;
        }
        lines.add(std::move(line));
    }

    if (!continueMarkerExchange(keepGoing, error)) {
        return false;
    }
    auto temp = prepareCheckedText(file, lines.joinIntoString({}));
    if (!continueMarkerExchange(keepGoing, error)) {
        return false;
    }
    if (!temp || !commitTemporary(*temp, file, noReplace)) {
        error = noReplace ? "Could not save labels without replacing an existing file."
                          : "Could not save labels; destination was not replaced.";
        return false;
    }
    return true;
}
namespace {

juce::File resolveSaveSourceFile(const Audio* audio,
                                 const std::map<const Audio*, juce::File>* sourcePaths) {
    if (!sourcePaths) {
        return audio->file;
    }
    const auto it = sourcePaths->find(audio);
    return it == sourcePaths->end() ? juce::File{} : it->second;
}

bool determineProjectVersion(const Session& session, WavRange savedLoop, bool& hasMdDisc,
                             juce::var& mdDiscValue, int& projectVersion, juce::String& error) {
    const bool hasChannelView =
        std::any_of(session.tracks.begin(), session.tracks.end(),
                    [](const auto& track) { return track.displaySourceChannel != -1; });
    const bool hasCombinedView =
        std::any_of(session.tracks.begin(), session.tracks.end(),
                    [](const auto& track) { return track.view == TrackView::combined; });
    const bool hasGroups =
        std::any_of(session.tracks.begin(), session.tracks.end(), [](const auto& track) {
            return std::any_of(track.clips.begin(), track.clips.end(),
                               [](const auto& clip) { return clip.groupId.isNotEmpty(); });
        });
    const bool hasWaveformZoom =
        std::any_of(session.tracks.begin(), session.tracks.end(),
                    [](const auto& track) { return track.waveformZoom != 0; });
    const bool hasChannelHeights =
        std::any_of(session.tracks.begin(), session.tracks.end(),
                    [](const auto& track) { return !track.channelHeightWeights.empty(); });

    const bool hasLoadedOnDisc =
        session.mdDisc.loadedDisc.active ||
        std::any_of(session.tracks.begin(), session.tracks.end(), [](const auto& track) {
            return std::any_of(track.clips.begin(), track.clips.end(), [](const auto& clip) {
                return clip.lockedOnDisc || (clip.audio && clip.audio->onDiscPlaceholder);
            });
        });

    hasMdDisc = !isDefaultMdDisc(session.mdDisc) || hasLoadedOnDisc;
    if (hasMdDisc && !encodeMdDisc(session.mdDisc, mdDiscValue, error)) {
        return false;
    }

    const bool hasHighGain =
        std::any_of(session.tracks.begin(), session.tracks.end(), [](const auto& track) {
            return track.gain > 4.0f ||
                   std::any_of(track.clips.begin(), track.clips.end(),
                               [](const auto& clip) { return clip.gain > 4.0f; });
        });
    const bool hasFadeShapes =
        std::any_of(session.tracks.begin(), session.tracks.end(), [](const auto& track) {
            return std::any_of(track.clips.begin(), track.clips.end(), [](const auto& clip) {
                return clip.fadeInShape != FadeShape::linear ||
                       clip.fadeOutShape != FadeShape::linear;
            });
        });

    const bool hasPlayOrderFeatures =
        static_cast<int>(session.mdDisc.tracks.size()) > kMdMaxStandardTracks ||
        std::any_of(session.mdDisc.tracks.begin(), session.mdDisc.tracks.end(),
                    [](const MdTrack& track) {
                        return !track.audioId.empty() || track.forceGapBefore;
                    });
    const bool hasHiMdFeatures =
        isHiMdMedium(session.mdDisc.medium) || isHiMdOnlyCodec(session.mdDisc.defaultCodec) ||
        std::any_of(session.mdDisc.tracks.begin(), session.mdDisc.tracks.end(),
                    [](const MdTrack& track) {
                        return (track.codecOverride.has_value() &&
                                isHiMdOnlyCodec(*track.codecOverride)) ||
                               !track.artist.empty() || !track.album.empty();
                    });

    if (hasHiMdFeatures || hasPlayOrderFeatures) {
        projectVersion = 27;
    } else if (hasLoadedOnDisc) {
        projectVersion = 26;
    } else if (hasMdDisc) {
        projectVersion = 25;
    } else if (hasHighGain || hasFadeShapes) {
        projectVersion = 24;
    } else if (savedLoop.end > savedLoop.start) {
        projectVersion = 14;
    } else if (!session.markers.empty()) {
        projectVersion = 13;
    } else if (hasChannelHeights) {
        projectVersion = 12;
    } else if (hasWaveformZoom) {
        projectVersion = 11;
    } else if (hasGroups) {
        projectVersion = 10;
    } else if (session.minimumEndSample > 0) {
        projectVersion = 9;
    } else if (hasChannelView) {
        projectVersion = 8;
    } else if (hasCombinedView) {
        projectVersion = 7;
    } else {
        projectVersion = 6;
    }
    return true;
}

template <typename KeepGoing>
bool collectAndVerifySaveSources(
    const Session& session, const juce::File& projectFile, bool recovery,
    const std::map<const Audio*, juce::File>* sourcePaths, const KeepGoing& keepGoing,
    std::vector<std::pair<juce::File, FileObservation>>& sourceObservations, juce::String& error) {
    // Check each distinct source before writing so the project never silently
    // points at different audio from the media currently held in memory.
    std::map<const Audio*, double> checkedSources;
    std::vector<const Audio*> sources;
    double totalBytes = 0;
    double processedBytes = 0;

    for (const auto& track : session.tracks) {
        for (const auto& clip : track.clips) {
            if (!keepGoing(0)) {
                return false;
            }
            if (!validProjectClip(clip, error)) {
                return false;
            }
            if (clip.audio->onDiscPlaceholder) {
                continue;
            }
            const auto path = resolveSaveSourceFile(clip.audio.get(), sourcePaths);
            if (path == juce::File{}) {
                error = sourcePaths ? "A portable source path is missing."
                                    : "An audio source path is missing.";
                return false;
            }
            if (sameFile(path, projectFile)) {
                error = "Project file cannot replace an audio source.";
                return false;
            }
            if (checkedSources.count(clip.audio.get()) == 0) {
                if (clip.audio->sourceHash.length() != 64 ||
                    !clip.audio->sourceHash.containsOnly("0123456789abcdef")) {
                    error = "Audio source identity is unavailable or invalid: " +
                            clip.audio->file.getFileName();
                    return false;
                }
                const double size = static_cast<double>(std::max<juce::int64>(1, path.getSize()));
                checkedSources.emplace(clip.audio.get(), size);
                sources.push_back(clip.audio.get());
                totalBytes += size;
            }
        }
    }

    totalBytes = std::max(1.0, totalBytes);
    if (!recovery) {
        for (const auto* source : sources) {
            const double size = checkedSources.at(source);
            juce::String current;
            const auto path = resolveSaveSourceFile(source, sourcePaths);
            sourceObservations.emplace_back(path, FileObservation(path));
            if (!sourceObservations.back().second.isValid()) {
                error = "Could not observe audio source: " + source->file.getFileName();
                return false;
            }
            if (!hashSourceFile(path, current, error, [&](double value) {
                    return keepGoing(0.9 * (processedBytes + size * value) / totalBytes);
                })) {
                if (error == "Import cancelled.") {
                    error = "Project save cancelled.";
                }
                return false;
            }
            if (current != source->sourceHash) {
                error = "Audio source changed since import: " + source->file.getFileName();
                return false;
            }
            processedBytes += size;
        }
    }
    return true;
}

template <typename KeepGoing>
bool serializeProjectTracks(const Session& session, const juce::File& projectFile,
                            int projectVersion,
                            const std::map<const Audio*, juce::File>* sourcePaths,
                            const KeepGoing& keepGoing, juce::Array<juce::var>& tracks,
                            juce::String& error) {
    for (const auto& track : session.tracks) {
        if (!keepGoing(0.9)) {
            return false;
        }
        if (track.height < 72 || track.height > 800 || !std::isfinite(track.gain) ||
            track.gain < 0 || track.gain > maxLinearGain || !std::isfinite(track.pan) ||
            track.pan < -1 || track.pan > 1 ||
            (track.view != TrackView::useDefault && track.view != TrackView::waveform &&
             track.view != TrackView::spectrogram && track.view != TrackView::combined) ||
            track.displaySourceChannel < -1 || track.displaySourceChannel > 1 ||
            !validWaveformZoom(track.waveformZoom) ||
            !validChannelHeightWeights(track.channelHeightWeights)) {
            error = "Invalid project track settings.";
            return false;
        }
        if (track.routing.size() > 4096) {
            error = "Track routing has too many channels.";
            return false;
        }
        for (const auto& route : track.routing) {
            if (!std::isfinite(route.left) || !std::isfinite(route.right) || route.left < -2 ||
                route.left > 2 || route.right < -2 || route.right > 2) {
                error = "Invalid track routing coefficient.";
                return false;
            }
        }

        auto t = object();
        t.getDynamicObject()->setProperty("name", track.name);
        t.getDynamicObject()->setProperty("height", track.height);
        t.getDynamicObject()->setProperty("view", static_cast<int>(track.view));
        if (projectVersion >= 8) {
            t.getDynamicObject()->setProperty("displaySourceChannel", track.displaySourceChannel);
        }
        if (projectVersion >= 11) {
            t.getDynamicObject()->setProperty("waveformZoom", track.waveformZoom);
        }
        if (projectVersion >= 12) {
            juce::Array<juce::var> heights;
            for (int weight : track.channelHeightWeights) {
                heights.add(weight);
            }
            t.getDynamicObject()->setProperty("channelHeightWeights", heights);
        }
        t.getDynamicObject()->setProperty("collapsed", track.collapsed);
        t.getDynamicObject()->setProperty("muted", track.muted);
        t.getDynamicObject()->setProperty("solo", track.solo);
        t.getDynamicObject()->setProperty("gain", static_cast<double>(track.gain));
        t.getDynamicObject()->setProperty("pan", static_cast<double>(track.pan));

        juce::Array<juce::var> routing;
        for (const auto& route : track.routing) {
            auto item = object();
            item.getDynamicObject()->setProperty("left", static_cast<double>(route.left));
            item.getDynamicObject()->setProperty("right", static_cast<double>(route.right));
            routing.add(item);
        }
        t.getDynamicObject()->setProperty("routing", routing);

        juce::Array<juce::var> clips;
        for (const auto& clip : track.clips) {
            if (!keepGoing(0.9)) {
                return false;
            }
            auto c = object();
            auto* o = c.getDynamicObject();
            if (clip.audio->onDiscPlaceholder) {
                o->setProperty("source", juce::String());
                o->setProperty("sourceSha256", juce::String());
                o->setProperty("onDiscPlaceholder", true);
                o->setProperty("placeholderChannels", clip.audio->placeholderChannels);
                o->setProperty("placeholderFrames", clip.audio->placeholderFrames);
            } else {
                const auto path = resolveSaveSourceFile(clip.audio.get(), sourcePaths);
                if (path == juce::File{} || sameFile(path, projectFile)) {
                    error = "Project file cannot replace an audio source.";
                    return false;
                }
                const auto relative = path.getRelativePathFrom(projectFile.getParentDirectory());
                o->setProperty("source",
                               sourcePaths ? relative.replaceCharacter('\\', '/') : relative);
                o->setProperty("sourceSha256", clip.audio->sourceHash);
            }
            if (projectVersion >= 26 && clip.lockedOnDisc) {
                o->setProperty("lockedOnDisc", true);
                o->setProperty("onDiscRegionId",
                               juce::String::fromUTF8(clip.onDiscRegionId.c_str()));
            }
            o->setProperty("clipName",
                           sourcePaths && clip.name.isEmpty() ? clipDisplayName(clip) : clip.name);
            if (projectVersion >= 10) {
                o->setProperty("groupId", clip.groupId);
            }
            o->setProperty("start", clip.start);
            o->setProperty("offset", clip.offset);
            o->setProperty("duration", clip.duration);
            o->setProperty("gain", static_cast<double>(clip.gain));
            o->setProperty("fadeIn", clip.fadeIn);
            o->setProperty("fadeOut", clip.fadeOut);
            o->setProperty("fadeInStart", clip.fadeInStart);
            o->setProperty("fadeOutEnd", clip.fadeOutEnd);
            if (clip.fadeInShape != FadeShape::linear) {
                o->setProperty("fadeInShape", fadeShapeName(clip.fadeInShape));
            }
            if (clip.fadeOutShape != FadeShape::linear) {
                o->setProperty("fadeOutShape", fadeShapeName(clip.fadeOutShape));
            }
            juce::Array<juce::var> exact;
            for (const auto value : {clip.start, clip.offset, clip.duration, clip.fadeIn,
                                     clip.fadeOut, clip.fadeInStart, clip.fadeOutEnd}) {
                exact.add(encodeDoubleBits(value));
            }
            o->setProperty("exactGeometry", exact);
            clips.add(c);
        }
        t.getDynamicObject()->setProperty("clips", clips);
        tracks.add(t);
    }
    return true;
}

template <typename KeepGoing>
bool serializeProjectMarkers(const Session& session, const KeepGoing& keepGoing,
                             juce::Array<juce::var>& markers, juce::String& error) {
    std::set<juce::String> markerIds;
    for (const auto& marker : session.markers) {
        if (!keepGoing(0.9)) {
            return false;
        }
        if (!validProjectMarker(marker)) {
            error = "Invalid project marker position.";
            return false;
        }
        if (!validMarkerId(marker.id) || !markerIds.insert(marker.id).second) {
            error = "Invalid marker identity.";
            return false;
        }
        auto m = object();
        m.getDynamicObject()->setProperty("id", marker.id);
        m.getDynamicObject()->setProperty("name", marker.name);
        m.getDynamicObject()->setProperty("position", marker.position);
        m.getDynamicObject()->setProperty("exactPosition", encodeDoubleBits(marker.position));
        markers.add(m);
    }
    return true;
}

bool saveSessionImpl(const Session& session, int channels, const juce::File& file,
                     juce::String& error, LoadProgress progress, bool recovery,
                     const std::map<const Audio*, juce::File>* sourcePaths = nullptr,
                     bool noReplace = false, DirectorySyncStatus* directorySync = nullptr,
                     WavRange savedLoop = {}) {
    const auto keepGoing = [&](double value) {
        if (!progress || progress(value)) {
            return true;
        }
        error = "Project save cancelled.";
        return false;
    };
    if (!keepGoing(0)) {
        return false;
    }
    if (channels != 1 && channels != 2) {
        error = "Invalid master channel count.";
        return false;
    }
    if (session.minimumEndSample < 0 || session.minimumEndSample >= 9007199254740992LL) {
        error = "Invalid minimum master duration.";
        return false;
    }
    if (!(savedLoop.start == 0 && savedLoop.end == 0) &&
        (savedLoop.start < 0 || savedLoop.end <= savedLoop.start ||
         savedLoop.end >= 9007199254740992LL)) {
        error = "Invalid saved loop region.";
        return false;
    }

    bool hasMdDisc = false;
    juce::var mdDiscValue;
    int projectVersion = 0;
    if (!determineProjectVersion(session, savedLoop, hasMdDisc, mdDiscValue, projectVersion,
                                 error)) {
        return false;
    }

    auto root = object();
    auto* obj = root.getDynamicObject();
    obj->setProperty("version", projectVersion);
    obj->setProperty("masterChannels", channels);
    if (projectVersion >= 9) {
        obj->setProperty("minimumEndSample", session.minimumEndSample);
    }
    if (projectVersion >= 14) {
        obj->setProperty("loopStartSample", savedLoop.start);
        obj->setProperty("loopEndSample", savedLoop.end);
    }
    if (hasMdDisc) {
        obj->setProperty("mdDisc", mdDiscValue);
    }
    if (recovery) {
        obj->setProperty("recovery", true);
    }

    std::vector<std::pair<juce::File, FileObservation>> sourceObservations;
    if (!collectAndVerifySaveSources(session, file, recovery, sourcePaths, keepGoing,
                                     sourceObservations, error)) {
        return false;
    }
    if (!keepGoing(0.9)) {
        return false;
    }

    juce::Array<juce::var> tracks;
    if (!serializeProjectTracks(session, file, projectVersion, sourcePaths, keepGoing, tracks,
                                error)) {
        return false;
    }

    juce::Array<juce::var> markers;
    if (!serializeProjectMarkers(session, keepGoing, markers, error)) {
        return false;
    }

    obj->setProperty("tracks", tracks);
    obj->setProperty("markers", markers);

    // JUCE's default 15 decimal places can change fractional clip geometry
    // after reopening. Use enough fractional/scientific digits to round-trip
    // finite doubles across the supported project timeline.
    const auto text =
        juce::JSON::toString(root, juce::JSON::FormatOptions{}.withMaxDecimalPlaces(24));
    if (text.getNumBytesAsUTF8() > static_cast<size_t>(maxProjectBytes)) {
        error = "Project exceeds the 64 MiB size limit.";
        return false;
    }

    auto temp = prepareCheckedText(file, text);
    if (!temp) {
        error = "Could not save project; destination was not replaced.";
        return false;
    }

    // The destination is not touched until the last cancellation point.
    if (!keepGoing(0.99)) {
        return false;
    }
    for (const auto& entry : sourceObservations) {
        if (!entry.second.matches(entry.first)) {
            error =
                "Audio source changed before project save completed: " + entry.first.getFileName();
            return false;
        }
    }
    if (!commitTemporary(*temp, file, noReplace)) {
        error = noReplace ? "Could not save the project without replacing an existing file."
                          : "Could not save project.";
        return false;
    }

    const auto syncStatus = syncParentDirectory(file);
    if (directorySync) {
        *directorySync = syncStatus;
    }
    if (progress) {
        progress(1);
    }
    return true;
}

template <typename KeepGoing>
bool readVerifiedProjectJson(const juce::File& file, juce::var& root, juce::String& error,
                             const KeepGoing& keepGoing) {
    const auto size = file.getSize();
    if (!file.existsAsFile() || size <= 0 || size > maxProjectBytes) {
        error = "Cannot read project, or it exceeds the 64 MiB size limit.";
        return false;
    }

    const FileObservation initial(file);
    if (!initial.isValid()) {
        error = "Could not read project file.";
        return false;
    }

    auto input = file.createInputStream();
    if (!input || input->getStatus().failed()) {
        error = "Could not read project file.";
        return false;
    }

    juce::MemoryBlock bytes;
    input->readIntoMemoryBlock(bytes, static_cast<std::ptrdiff_t>(maxProjectBytes + 1));
    if (!keepGoing(0)) {
        return false;
    }
    if (input->getStatus().failed() || bytes.getSize() > static_cast<size_t>(maxProjectBytes)) {
        error = "Could not read project file, or it exceeds the 64 MiB size limit.";
        return false;
    }
    if (static_cast<juce::int64>(bytes.getSize()) != size || !initial.matches(file)) {
        error = "Project file changed while it was being read.";
        return false;
    }

    const auto* data = static_cast<const char*>(bytes.getData());
    if (!data || std::memchr(data, 0, bytes.getSize()) ||
        !juce::CharPointer_UTF8::isValidString(data, static_cast<int>(bytes.getSize()))) {
        error = "Project must be UTF-8 text.";
        return false;
    }
    if (!safeJsonNumbers(data, bytes.getSize())) {
        error = "Invalid numeric value in project file.";
        return false;
    }

    size_t prefix = 0;
    if (bytes.getSize() >= 3 && static_cast<unsigned char>(data[0]) == 0xef &&
        static_cast<unsigned char>(data[1]) == 0xbb &&
        static_cast<unsigned char>(data[2]) == 0xbf) {
        prefix = 3;
    }

    const auto parsed = juce::JSON::parse(
        juce::String::fromUTF8(data + prefix, static_cast<int>(bytes.getSize() - prefix)), root);
    if (parsed.failed()) {
        error = "Invalid or unsupported Carta Studio project.";
        return false;
    }
    return true;
}

struct ProjectLoadContext {
    juce::AudioFormatManager& formats;
    const juce::File& projectFile;
    juce::int64 version = 0;
    double maxAllowedGain = 4.0;
    const SourceRelinks* relinks = nullptr;
    SourceRelinkRequest* request = nullptr;
    std::map<juce::String, std::shared_ptr<Audio>> sources;
    std::set<juce::String> appliedRelinks;
    bool usedRelink = false;
    bool usedV24Feature = false;
    juce::int64 completed = 0;
    double total = 1.0;
    std::function<bool(double)> keepGoing;
};

bool loadProjectClip(ProjectLoadContext& ctx, const juce::var& cv, Track& track,
                     juce::String& error) {
    if (!ctx.keepGoing(0.95 * static_cast<double>(ctx.completed) / ctx.total)) {
        return false;
    }
    if (!cv.isObject() || !cv["source"].isString()) {
        error = "Invalid project clip.";
        return false;
    }

    double start = 0;
    double offset = 0;
    double duration = 0;
    double exact[7]{};
    if (ctx.version >= 6) {
        const auto* values = cv["exactGeometry"].getArray();
        constexpr const char* names[] = {"start",   "offset",      "duration",  "fadeIn",
                                         "fadeOut", "fadeInStart", "fadeOutEnd"};
        if (!values || values->size() != 7) {
            error = "Invalid exact clip geometry.";
            return false;
        }
        for (int i = 0; i < 7; ++i) {
            if (!exactProjectDouble(cv[names[i]], (*values)[i], exact[i])) {
                error = "Invalid exact clip geometry.";
                return false;
            }
        }
        start = exact[0];
        offset = exact[1];
        duration = exact[2];
    } else if (!number(cv["start"], start) || !number(cv["offset"], offset) ||
               !number(cv["duration"], duration)) {
        error = "Invalid project clip bounds.";
        return false;
    }

    if (!validTimelineGeometry(start, offset, duration)) {
        error = "Invalid project clip bounds.";
        return false;
    }

    const auto* clipProps = cv.getDynamicObject();
    const bool isPlaceholder = ctx.version >= 26 && clipProps->hasProperty("onDiscPlaceholder") &&
                               cv["onDiscPlaceholder"].isBool() &&
                               static_cast<bool>(cv["onDiscPlaceholder"]);

    std::shared_ptr<Audio> clipAudio;
    if (isPlaceholder) {
        juce::int64 phChannels = 0;
        juce::int64 phFrames = 0;
        if (!integerValue(cv["placeholderChannels"], phChannels) || phChannels < 1 ||
            phChannels > 8 || !integerValue(cv["placeholderFrames"], phFrames) ||
            phFrames <= 0 || phFrames > std::numeric_limits<int>::max()) {
            error = "Invalid on-disc placeholder audio dimensions.";
            return false;
        }
        clipAudio = std::make_shared<Audio>();
        clipAudio->onDiscPlaceholder = true;
        clipAudio->placeholderChannels = static_cast<int>(phChannels);
        clipAudio->placeholderFrames = static_cast<int>(phFrames);
        clipAudio->sampleRate = sessionRate;
    } else {
        const auto name = cv["source"].toString();
        if (name.isEmpty()) {
            error = "Missing audio source path.";
            return false;
        }
        const auto original = juce::File::isAbsolutePath(name)
                                  ? juce::File(name)
                                  : ctx.projectFile.getParentDirectory().getChildFile(name);
        const auto key = original.getFullPathName();
        auto source = original;

        const bool hasHash = clipProps->hasProperty("sourceSha256");
        const auto expected = cv["sourceSha256"].toString();
        if (hasHash && (!cv["sourceSha256"].isString() || expected.length() != 64 ||
                        !expected.containsOnly("0123456789abcdef"))) {
            error = "Invalid audio source checksum in project.";
            return false;
        }

        if (ctx.relinks) {
            const auto replacement = ctx.relinks->find(key);
            if (replacement != ctx.relinks->end()) {
                if (!hasHash) {
                    error = "Cannot verify a replacement for a project without source checksums.";
                    return false;
                }
                source = replacement->second;
                ctx.usedRelink = true;
                ctx.appliedRelinks.insert(key);
            }
        }

        const auto offerRelink = [&] {
            if (ctx.request && hasHash) {
                *ctx.request = {original, expected};
            }
        };

        auto it = ctx.sources.find(key);
        if (it == ctx.sources.end()) {
            auto audio = loadAudio(ctx.formats, source, error, [&](double value) {
                return ctx.keepGoing(0.95 * (static_cast<double>(ctx.completed) + value) /
                                     ctx.total);
            });
            if (!audio) {
                if (error == "Import cancelled.") {
                    error = "Project opening cancelled.";
                } else if (!source.existsAsFile() ||
                           error.startsWith("Unsupported or empty audio file:") ||
                           error.startsWith("Could not read audio:") ||
                           error.startsWith("Could not read audio source:") ||
                           error.startsWith("Audio source changed while it was being imported:") ||
                           error.startsWith("Audio contains a non-finite sample:")) {
                    if (hasHash) {
                        offerRelink();
                    } else {
                        error += " This project has no source checksum; restore the original file "
                                 "at its saved path.";
                    }
                }
                return false;
            }
            it = ctx.sources.emplace(key, std::move(audio)).first;
        }

        if (hasHash && expected != it->second->sourceHash) {
            error = "Audio source does not match the saved project: " + source.getFileName();
            offerRelink();
            return false;
        }
        clipAudio = it->second;
    }

    if (offset + duration > clipAudio->length() + 1e-8) {
        error = "Invalid project clip bounds.";
        return false;
    }

    Clip clip{clipAudio, start, offset, duration};
    const auto* props = clipProps;
    if (ctx.version >= 26) {
        if (props->hasProperty("lockedOnDisc")) {
            if (!cv["lockedOnDisc"].isBool()) {
                error = "Invalid lockedOnDisc clip flag.";
                return false;
            }
            clip.lockedOnDisc = static_cast<bool>(cv["lockedOnDisc"]);
        }
        if (props->hasProperty("onDiscRegionId")) {
            if (!cv["onDiscRegionId"].isString()) {
                error = "Invalid onDiscRegionId clip property.";
                return false;
            }
            clip.onDiscRegionId = cv["onDiscRegionId"].toString().toStdString();
        }
    }
    if (ctx.version >= 10) {
        if (!cv["groupId"].isString() || !validClipGroupId(cv["groupId"].toString())) {
            error = "Invalid clip group identity.";
            return false;
        }
        clip.groupId = cv["groupId"].toString();
    }
    if (ctx.version >= 4) {
        if (!cv["clipName"].isString() || cv["clipName"].toString().length() > 1024) {
            error = "Invalid clip name.";
            return false;
        }
        clip.name = cv["clipName"].toString();
    }
    if (ctx.version >= 2 && (!props->hasProperty("gain") || !props->hasProperty("fadeIn") ||
                             !props->hasProperty("fadeOut") || !props->hasProperty("fadeInStart") ||
                             !props->hasProperty("fadeOutEnd"))) {
        error = "Incomplete clip envelope.";
        return false;
    }
    if (props->hasProperty("gain")) {
        double gain;
        if (!number(cv["gain"], gain) || gain < 0 || gain > ctx.maxAllowedGain) {
            error = "Invalid clip gain.";
            return false;
        }
        clip.gain = static_cast<float>(gain);
        ctx.usedV24Feature |= clip.gain > 4.0f;
    }
    for (const auto& pair :
         {std::pair<const char*, FadeShape*>("fadeInShape", &clip.fadeInShape),
          std::pair<const char*, FadeShape*>("fadeOutShape", &clip.fadeOutShape)}) {
        if (!props->hasProperty(pair.first)) {
            continue;
        }
        if (ctx.version < 24 || !parseFadeShape(cv[pair.first], *pair.second)) {
            error = "Invalid clip fade shape or project version.";
            return false;
        }
        ctx.usedV24Feature = true;
    }

    const auto optionalNumber = [&](const char* property, double& value) {
        return !props->hasProperty(property) || number(cv[property], value);
    };
    if (!optionalNumber("fadeIn", clip.fadeIn) || !optionalNumber("fadeOut", clip.fadeOut) ||
        !optionalNumber("fadeInStart", clip.fadeInStart) ||
        !optionalNumber("fadeOutEnd", clip.fadeOutEnd)) {
        error = "Invalid clip fade.";
        return false;
    }
    if (ctx.version >= 6) {
        clip.fadeIn = exact[3];
        clip.fadeOut = exact[4];
        clip.fadeInStart = exact[5];
        clip.fadeOutEnd = exact[6];
    }
    if (clip.fadeIn < 0 || clip.fadeOut < 0 || clip.fadeInStart < 0 || clip.fadeOutEnd < 0 ||
        clip.fadeIn > clipAudio->length() + 1e-8 || clip.fadeOut > clipAudio->length() + 1e-8 ||
        clip.fadeInStart + clip.fadeIn > clipAudio->length() + 1e-8 ||
        clip.fadeOutEnd > clipAudio->length() + 1e-8 || clip.fadeOut > clip.fadeOutEnd + 1e-8) {
        error = "Invalid clip fade.";
        return false;
    }
    if (!validProjectClip(clip, error)) {
        return false;
    }

    track.clips.push_back(std::move(clip));
    ++ctx.completed;
    return true;
}

bool loadProjectTrack(ProjectLoadContext& ctx, const juce::var& tv, Track& track,
                      juce::String& error) {
    if (!ctx.keepGoing(0.95 * static_cast<double>(ctx.completed) / ctx.total)) {
        return false;
    }
    if (!tv.isObject() || !tv["name"].isString() || !tv["clips"].isArray()) {
        error = "Invalid project track.";
        return false;
    }
    track.name = tv["name"].toString();

    if (ctx.version >= 5) {
        juce::int64 view = 0;
        if (!integerValue(tv["view"], view) || view < 0 || view > (ctx.version >= 7 ? 3 : 2)) {
            error = "Invalid track view.";
            return false;
        }
        track.view = static_cast<TrackView>(view);
        if (tv.getDynamicObject()->hasProperty("collapsed")) {
            if (!tv["collapsed"].isBool()) {
                error = "Invalid track collapse state.";
                return false;
            }
            track.collapsed = static_cast<bool>(tv["collapsed"]);
        }
    }
    if (ctx.version >= 8) {
        juce::int64 channel = 0;
        if (!integerValue(tv["displaySourceChannel"], channel) || channel < -1 || channel > 1) {
            error = "Invalid source-channel display.";
            return false;
        }
        track.displaySourceChannel = static_cast<int>(channel);
    }
    if (ctx.version >= 11) {
        juce::int64 zoom = 0;
        if (!integerValue(tv["waveformZoom"], zoom) || zoom < 0 || zoom > 8 ||
            !validWaveformZoom(static_cast<int>(zoom))) {
            error = "Invalid track waveform zoom.";
            return false;
        }
        track.waveformZoom = static_cast<int>(zoom);
    }
    if (ctx.version >= 12) {
        const auto* heights = tv["channelHeightWeights"].getArray();
        if (!heights || heights->size() > 1024) {
            error = "Invalid channel height weights.";
            return false;
        }
        for (const auto& value : *heights) {
            juce::int64 weight = 0;
            if (!integerValue(value, weight) || weight < 1 || weight > 1000) {
                error = "Invalid channel height weight.";
                return false;
            }
            track.channelHeightWeights.push_back(static_cast<int>(weight));
        }
        if (!validChannelHeightWeights(track.channelHeightWeights)) {
            error = "Invalid channel height weights.";
            return false;
        }
    }
    if (tv.getDynamicObject()->hasProperty("height")) {
        double height;
        if (!number(tv["height"], height) || height < 72 || height > 800 ||
            std::floor(height) != height) {
            error = "Invalid track height.";
            return false;
        }
        track.height = static_cast<int>(height);
    }

    const auto* properties = tv.getDynamicObject();
    if (properties->hasProperty("muted")) {
        if (!tv["muted"].isBool()) {
            error = "Invalid mute state.";
            return false;
        }
        track.muted = static_cast<bool>(tv["muted"]);
    }
    if (properties->hasProperty("solo")) {
        if (!tv["solo"].isBool()) {
            error = "Invalid solo state.";
            return false;
        }
        track.solo = static_cast<bool>(tv["solo"]);
    }
    if (properties->hasProperty("gain")) {
        double gain;
        if (!number(tv["gain"], gain) || gain < 0 || gain > ctx.maxAllowedGain) {
            error = "Invalid track gain.";
            return false;
        }
        track.gain = static_cast<float>(gain);
        ctx.usedV24Feature |= track.gain > 4.0f;
    }
    if (properties->hasProperty("pan")) {
        double pan;
        if (!number(tv["pan"], pan) || pan < -1 || pan > 1) {
            error = "Invalid track pan.";
            return false;
        }
        track.pan = static_cast<float>(pan);
    }
    if (ctx.version >= 3 && !tv["routing"].isArray()) {
        error = "Missing track routing.";
        return false;
    }
    if (ctx.version >= 3) {
        if (!tv["routing"].isArray() || tv["routing"].getArray()->size() > 4096) {
            error = "Invalid track routing.";
            return false;
        }
        for (const auto& route : *tv["routing"].getArray()) {
            double left;
            double right;
            if (!route.isObject() || !number(route["left"], left) ||
                !number(route["right"], right) || left < -2 || left > 2 || right < -2 ||
                right > 2) {
                error = "Invalid track routing coefficient.";
                return false;
            }
            track.routing.push_back({static_cast<float>(left), static_cast<float>(right)});
        }
    }

    for (const auto& cv : *tv["clips"].getArray()) {
        if (!loadProjectClip(ctx, cv, track, error)) {
            return false;
        }
    }
    return true;
}

bool loadProjectMarkers(ProjectLoadContext& ctx, const juce::var& root,
                        std::vector<Marker>& markersOut, juce::String& error) {
    std::set<juce::String> markerIds;
    for (const auto& mv : *root["markers"].getArray()) {
        if (!ctx.keepGoing(0.97)) {
            return false;
        }
        double position;
        if (!mv.isObject() || !mv["name"].isString() ||
            !(ctx.version >= 6 ? exactProjectDouble(mv["position"], mv["exactPosition"], position)
                               : number(mv["position"], position)) ||
            position < 0) {
            error = "Invalid project marker.";
            return false;
        }
        Marker marker{position, mv["name"].toString()};
        for (const auto* titleKey : {"halfWidthTitle", "fullWidthTitle"}) {
            if (mv.getDynamicObject()->hasProperty(titleKey) &&
                (ctx.version < 22 || !mv[titleKey].isString() ||
                 mv[titleKey].toString().isEmpty() ||
                 !validDiscText(mv[titleKey].toString().toStdString()))) {
                error = "Invalid marker title form or project version.";
                return false;
            }
        }
        if (mv.getDynamicObject()->hasProperty("audioMode")) {
            if (ctx.version < 24 || !mv["audioMode"].isString() ||
                mv["audioMode"].toString().isEmpty() ||
                !validMdTrackAudioMode(mv["audioMode"].toString().toStdString())) {
                error = "Invalid marker audio mode or project version.";
                return false;
            }
            ctx.usedV24Feature = true;
        }
        if (mv.getDynamicObject()->hasProperty("writeProtected")) {
            if (ctx.version < 24 || !mv["writeProtected"].isBool() ||
                !static_cast<bool>(mv["writeProtected"])) {
                error = "Invalid marker write-protect flag or project version.";
                return false;
            }
            ctx.usedV24Feature = true;
        }
        if (mv.getDynamicObject()->hasProperty("scms")) {
            if (ctx.version < 24 || !mv["scms"].isBool() || !static_cast<bool>(mv["scms"])) {
                error = "Invalid marker SCMS flag or project version.";
                return false;
            }
            ctx.usedV24Feature = true;
        }
        if (mv.getDynamicObject()->hasProperty("timestamp")) {
            if (ctx.version < 24 || !mv["timestamp"].isString() ||
                mv["timestamp"].toString().isEmpty() ||
                !validMdTimestamp(mv["timestamp"].toString().toStdString())) {
                error = "Invalid marker timestamp or project version.";
                return false;
            }
            ctx.usedV24Feature = true;
        }

        if (ctx.version >= 13) {
            if (!mv["id"].isString() || !validMarkerId(mv["id"].toString()) ||
                !markerIds.insert(mv["id"].toString()).second) {
                error = "Invalid or duplicate project marker identity.";
                return false;
            }
            marker.id = mv["id"].toString();
        } else {
            while (!markerIds.insert(marker.id).second) {
                marker.id = juce::Uuid().toString().removeCharacters("-").toLowerCase();
            }
        }

        if (!validProjectMarker(marker)) {
            error = "Invalid project marker position.";
            return false;
        }
        markersOut.push_back(std::move(marker));
    }
    return true;
}

} // namespace

bool saveSession(const Session& session, int channels, const juce::File& file, juce::String& error,
                 LoadProgress progress, DirectorySyncStatus* directorySync, WavRange savedLoop) {
    return saveSessionImpl(session, channels, file, error, std::move(progress), false, nullptr,
                           false, directorySync, savedLoop);
}

bool saveSessionCopy(const Session& session, int channels, const juce::File& file,
                     juce::String& error, LoadProgress progress, DirectorySyncStatus* directorySync,
                     WavRange savedLoop) {
    return saveSessionImpl(session, channels, file, error, std::move(progress), false, nullptr,
                           true, directorySync, savedLoop);
}

bool exportPortableSession(const Session& session, int channels, const juce::File& parent,
                           juce::File& resultDirectory, juce::String& error, LoadProgress progress,
                           WavRange savedLoop) {
    resultDirectory = juce::File{};
    error.clear();
    const auto keepGoing = [&](double value) {
        if (!progress || progress(value)) {
            return true;
        }
        error = "Portable project export cancelled.";
        return false;
    };

    if (!parent.isDirectory()) {
        error = "Choose an existing destination folder.";
        return false;
    }
    if (channels != 1 && channels != 2) {
        error = "Invalid master channel count.";
        return false;
    }

    std::vector<const Audio*> sources;
    std::set<const Audio*> seen;
    double total = 0;
    for (const auto& track : session.tracks) {
        for (const auto& clip : track.clips) {
            if (!validProjectClip(clip, error)) {
                return false;
            }
            if (clip.audio->onDiscPlaceholder) {
                continue;
            }
            if (seen.insert(clip.audio.get()).second) {
                if (clip.audio->sourceHash.length() != 64 ||
                    !clip.audio->sourceHash.containsOnly("0123456789abcdef") ||
                    !clip.audio->file.existsAsFile()) {
                    error = "A source is missing or has no verified identity: " +
                            clip.audio->file.getFileName();
                    return false;
                }
                sources.push_back(clip.audio.get());
                total += static_cast<double>(std::max<juce::int64>(1, clip.audio->file.getSize()));
            }
        }
    }
    if (!keepGoing(0)) {
        return false;
    }

    for (int suffix = 0; suffix < 10000; ++suffix) {
        const auto name = suffix == 0 ? juce::String("Carta portable")
                                      : "Carta portable (" + juce::String(suffix + 1) + ")";
        const auto candidate = parent.getChildFile(name);
        std::error_code ec;
        if (std::filesystem::create_directory(
                std::filesystem::u8path(candidate.getFullPathName().toStdString()), ec)) {
            resultDirectory = candidate;
            break;
        }
        if (ec && ec != std::errc::file_exists) {
            error = "Could not create a portable project folder: " + juce::String(ec.message());
            return false;
        }
    }
    if (resultDirectory == juce::File{}) {
        error = "Could not find an unused portable project folder name.";
        return false;
    }

    const auto media = resultDirectory.getChildFile("Media");
    std::error_code directoryError;
    if (!std::filesystem::create_directory(
            std::filesystem::u8path(media.getFullPathName().toStdString()), directoryError)) {
        error = "Could not create the portable media folder.";
        return false;
    }

    std::map<const Audio*, juce::File> copies;
    std::map<juce::String, juce::File> copiesByHash;
    std::vector<char> buffer(1024u * 1024u);
    double done = 0;
    total = std::max(1.0, total);

    for (size_t index = 0; index < sources.size(); ++index) {
        const auto* audio = sources[index];
        const double size = static_cast<double>(std::max<juce::int64>(1, audio->file.getSize()));
        const auto duplicate = copiesByHash.find(audio->sourceHash);
        if (duplicate != copiesByHash.end()) {
            // Even when bytes are already copied, verify this distinct original:
            // stale media must not be silently replaced by another file with its old hash.
            juce::String current;
            if (!hashSourceFile(audio->file, current, error, [&](double value) {
                    return keepGoing(0.7 * std::min(1.0, (done + size * value) / total));
                })) {
                if (error == "Import cancelled.") {
                    error = "Portable project export cancelled.";
                }
                return false;
            }
            if (current != audio->sourceHash) {
                error = "Audio source changed since import: " + audio->file.getFileName();
                return false;
            }
            copies.emplace(audio, duplicate->second);
            done += size;
            continue;
        }

        const auto base = juce::File::createLegalFileName(audio->file.getFileNameWithoutExtension())
                              .substring(0, 40);
        const auto extension =
            juce::File::createLegalFileName(audio->file.getFileExtension()).substring(0, 16);
        const auto target =
            media.getChildFile(juce::String(static_cast<int>(index + 1)).paddedLeft('0', 4) +
                               " - " + (base.isEmpty() ? juce::String("audio") : base) + extension);

        juce::FileInputStream input(audio->file);
        if (!input.openedOk()) {
            error = "Could not read source: " + audio->file.getFileName();
            return false;
        }
        juce::TemporaryFile temporary(target);
        auto output = temporary.getFile().createOutputStream();
        if (!output) {
            error = "Could not create portable media copy.";
            return false;
        }

        double copied = 0;
        for (;;) {
            if (!keepGoing(0.7 * std::min(1.0, (done + copied) / total))) {
                return false;
            }
            const int count = input.read(buffer.data(), static_cast<int>(buffer.size()));
            if (count <= 0) {
                break;
            }
            if (!output->write(buffer.data(), static_cast<size_t>(count))) {
                error = "Could not write portable media copy.";
                return false;
            }
            copied += count;
        }
        if (input.getStatus().failed()) {
            error = "Could not read source: " + audio->file.getFileName();
            return false;
        }
        output->flush();
        if (output->getStatus().failed()) {
            error = "Could not finish portable media copy.";
            return false;
        }
        output.reset();
        if (!keepGoing(0.7 * std::min(1.0, (done + size) / total))) {
            return false;
        }
        if (!commitTemporary(temporary, target, true)) {
            error = "Could not commit portable media without replacing an existing file.";
            return false;
        }
        copies.emplace(audio, target);
        copiesByHash.emplace(audio->sourceHash, target);
        done += size;
    }

    const auto project = resultDirectory.getChildFile("Project.carta");
    return saveSessionImpl(
        session, channels, project, error,
        [&](double value) { return keepGoing(0.7 + 0.3 * value); }, false, &copies, true, nullptr,
        savedLoop);
}

bool saveRecoverySession(const Session& session, int channels, const juce::File& file,
                         juce::String& error, LoadProgress progress,
                         DirectorySyncStatus* directorySync, WavRange savedLoop) {
    return saveSessionImpl(session, channels, file, error, std::move(progress), true, nullptr,
                           false, directorySync, savedLoop);
}

bool loadSession(Session& result, int& master, juce::AudioFormatManager& formats,
                 const juce::File& file, juce::String& error, LoadProgress progress,
                 bool* isRecovery, const SourceRelinks* relinks, SourceRelinkRequest* request,
                 bool* didRelink, bool* didAssignMarkerIds, WavRange* savedLoop) {
    if (request) {
        *request = {};
    }
    const auto keepGoing = [&](double value) {
        if (!progress || progress(value)) {
            return true;
        }
        error = "Project opening cancelled.";
        return false;
    };
    if (!keepGoing(0)) {
        return false;
    }

    juce::var root;
    if (!readVerifiedProjectJson(file, root, error, keepGoing)) {
        return false;
    }

    juce::int64 version = 0;
    juce::int64 masterValue = 0;
    if (!root.isObject() || !integerValue(root["version"], version) || version < 1 ||
        version > 27 || !root["tracks"].isArray() || !root["markers"].isArray()) {
        error = "Invalid or unsupported Carta Studio project.";
        return false;
    }
    if (!integerValue(root["masterChannels"], masterValue) ||
        (masterValue != 1 && masterValue != 2)) {
        error = "Invalid master channel count.";
        return false;
    }
    if (root.getDynamicObject()->hasProperty("recovery") && !root["recovery"].isBool()) {
        error = "Invalid recovery flag.";
        return false;
    }

    const int channels = static_cast<int>(masterValue);
    juce::int64 clipCount = 0;
    for (const auto& track : *root["tracks"].getArray()) {
        if (track.isObject() && track["clips"].isArray()) {
            clipCount += track["clips"].getArray()->size();
        }
    }

    Session next;
    WavRange nextLoop;
    if (version >= 14 &&
        (!integerValue(root["loopStartSample"], nextLoop.start) ||
         !integerValue(root["loopEndSample"], nextLoop.end) || nextLoop.start < 0 ||
         (nextLoop.end <= nextLoop.start &&
          !(version >= 15 && nextLoop.start == 0 && nextLoop.end == 0)) ||
         nextLoop.end >= 9007199254740992LL)) {
        error = "Invalid saved loop region.";
        return false;
    }
    if (version < 25 && root.getDynamicObject()->hasProperty("mdDisc")) {
        error = "MD disc authoring data requires project version 25 or later.";
        return false;
    }
    if (version >= 25) {
        if (!root.getDynamicObject()->hasProperty("mdDisc")) {
            error = "Missing MD disc authoring data for version " + juce::String(version) +
                    " project.";
            return false;
        }
        if (!decodeMdDisc(root["mdDisc"], next.mdDisc, error)) {
            return false;
        }
        const bool usesPlayOrderV27 =
            static_cast<int>(next.mdDisc.tracks.size()) > kMdMaxStandardTracks ||
            std::any_of(next.mdDisc.tracks.begin(), next.mdDisc.tracks.end(),
                        [](const MdTrack& track) {
                            return !track.audioId.empty() || track.forceGapBefore;
                        });
        const bool usesHiMd =
            isHiMdMedium(next.mdDisc.medium) || isHiMdOnlyCodec(next.mdDisc.defaultCodec) ||
            std::any_of(next.mdDisc.tracks.begin(), next.mdDisc.tracks.end(),
                        [](const MdTrack& track) {
                            return (track.codecOverride.has_value() &&
                                    isHiMdOnlyCodec(*track.codecOverride)) ||
                                   !track.artist.empty() || !track.album.empty();
                        });
        if ((usesHiMd || usesPlayOrderV27) && version < 27) {
            error = "Hi-MD and Play Order aliasing features require project version 27 or later.";
            return false;
        }
    }

    if (version >= 9 &&
        (!integerValue(root["minimumEndSample"], next.minimumEndSample) ||
         next.minimumEndSample < 0 || (version == 9 && next.minimumEndSample == 0) ||
         next.minimumEndSample >= 9007199254740992LL)) {
        error = "Invalid minimum master duration.";
        return false;
    }

    ProjectLoadContext ctx{
        formats,   file,    version, version >= 24 ? static_cast<double>(maxLinearGain) : 4.0,
        relinks,   request, {},      {},
        false,     false,   0,       static_cast<double>(std::max<juce::int64>(1, clipCount)),
        keepGoing,
    };

    for (const auto& tv : *root["tracks"].getArray()) {
        Track t;
        if (!loadProjectTrack(ctx, tv, t, error)) {
            return false;
        }
        next.tracks.push_back(std::move(t));
    }

    if (relinks && ctx.appliedRelinks.size() != relinks->size()) {
        error = "A requested source relink does not belong to this project.";
        return false;
    }
    if (!keepGoing(0.97)) {
        return false;
    }

    if (!loadProjectMarkers(ctx, root, next.markers, error)) {
        return false;
    }
    if (!keepGoing(1)) {
        return false;
    }
    if (version == 24 && !ctx.usedV24Feature &&
        !root.getDynamicObject()->hasProperty("discProgram")) {
        error = "Invalid version 24 project without version 24 features.";
        return false;
    }

    const auto flag = root["recovery"];
    const bool recovered = flag.isBool() && static_cast<bool>(flag);
    const bool assignedMarkerIds = version < 13 && !next.markers.empty();

    result = std::move(next);
    master = channels;
    if (isRecovery) {
        *isRecovery = recovered;
    }
    if (didRelink) {
        *didRelink = ctx.usedRelink;
    }
    if (didAssignMarkerIds) {
        *didAssignMarkerIds = assignedMarkerIds;
    }
    if (savedLoop) {
        *savedLoop = nextLoop;
    }
    return true;
}

namespace {

struct PackageScratch {
    juce::File directory;

    ~PackageScratch() {
        if (directory != juce::File{}) {
            directory.deleteRecursively();
        }
    }

    bool create() {
        const auto parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
        for (int i = 0; i < 16; ++i) {
            const auto candidate = parent.getChildFile("carta-package-" + juce::Uuid().toString());
#if JUCE_MAC || JUCE_LINUX
            if (::mkdir(candidate.getFullPathName().toRawUTF8(), 0700) == 0) {
                directory = candidate;
                return true;
            }
            if (errno != EEXIST) {
                return false;
            }
#else
            std::error_code ec;
            if (std::filesystem::create_directory(
                    std::filesystem::u8path(candidate.getFullPathName().toStdString()), ec)) {
                directory = candidate;
                return true;
            }
            if (ec && ec != std::errc::file_exists) {
                return false;
            }
#endif
        }
        return false;
    }
};

bool packageNameValid(const juce::String& name) {
    if (name == "Project.carta" || name == "Bank.json" || name == "Timeline-bank.wav") {
        return true;
    }
    if (!name.startsWith("Media/")) {
        return false;
    }
    const auto leaf = name.substring(6);
    return leaf.isNotEmpty() && leaf != "." && leaf != ".." && !leaf.containsAnyOf("/\\:");
}

// Validate paths before invoking the normal loader so an untrusted package
// cannot cause it to open a source outside the private extraction directory.
// Also reconciles Unicode normalization differences (e.g. macOS NFD vs NFC
// filenames in Media/) when a verified package entry matches by SHA-256.
bool packageProjectPathsValid(const juce::File& project, const std::set<juce::String>& names,
                              const std::map<juce::String, juce::String>& hashToMediaName,
                              bool& projectRewritten, juce::String& error) {
    projectRewritten = false;
    const auto size = project.getSize();
    if (size <= 0 || size > maxProjectBytes) {
        error = "Invalid package project size.";
        return false;
    }
    juce::MemoryBlock bytes;
    if (!project.loadFileAsData(bytes) || static_cast<juce::int64>(bytes.getSize()) != size) {
        error = "Cannot read package project.";
        return false;
    }
    const auto* data = static_cast<const char*>(bytes.getData());
    if (!data || std::memchr(data, 0, bytes.getSize()) ||
        !juce::CharPointer_UTF8::isValidString(data, static_cast<int>(bytes.getSize())) ||
        !safeJsonNumbers(data, bytes.getSize())) {
        error = "Invalid package project text.";
        return false;
    }

    juce::var root;
    if (juce::JSON::parse(juce::String::fromUTF8(data, static_cast<int>(bytes.getSize())), root)
            .failed() ||
        !root.isObject() || !root["tracks"].isArray()) {
        error = "Invalid package project.";
        return false;
    }

    juce::int64 pkgVersion = 0;
    integerValue(root["version"], pkgVersion);

    for (auto& track : *root["tracks"].getArray()) {
        if (!track.isObject() || !track["clips"].isArray()) {
            error = "Invalid package track.";
            return false;
        }
        for (auto& clip : *track["clips"].getArray()) {
            if (pkgVersion >= 26 && clip.isObject() &&
                clip.getDynamicObject()->hasProperty("onDiscPlaceholder") &&
                clip["onDiscPlaceholder"].isBool() &&
                static_cast<bool>(clip["onDiscPlaceholder"])) {
                continue;
            }
            const auto name = clip["source"].toString().replaceCharacter('\\', '/');
            const auto hash = clip["sourceSha256"].toString();
            if (!clip.isObject() || !clip["source"].isString() ||
                !clip["sourceSha256"].isString() || hash.length() != 64 ||
                !hash.containsOnly("0123456789abcdef") || !name.startsWith("Media/") ||
                !packageNameValid(name)) {
                error = "Package project refers to missing, unverified or external media.";
                return false;
            }
            if (!names.count(name)) {
                const auto it = hashToMediaName.find(hash);
                if (it == hashToMediaName.end()) {
                    error = "Package project refers to missing, unverified or external media.";
                    return false;
                }
                clip.getDynamicObject()->setProperty("source", it->second);
                projectRewritten = true;
            }
        }
    }
    if (projectRewritten) {
        auto temp = prepareCheckedText(
            project,
            juce::JSON::toString(root, juce::JSON::FormatOptions{}.withMaxDecimalPlaces(24)));
        if (!temp || !commitTemporary(*temp, project, false)) {
            error = "Could not normalize package project media paths.";
            return false;
        }
    }
    return true;
}

} // namespace

bool isNativePackage(const juce::File& file) {
    juce::FileInputStream input(file);
    char magic[8]{};
    return input.openedOk() && input.read(magic, 8) == 8 && std::memcmp(magic, "CARTAP01", 8) == 0;
}

bool saveNativePackage(const Session& session, int channels, const juce::File& target,
                       juce::String& error, LoadProgress progress,
                       DirectorySyncStatus* directorySync, WavRange savedLoop, bool noReplace,
                       bool recovery) {
    error.clear();
    const auto keepGoing = [&](double p) {
        if (!progress || progress(p)) {
            return true;
        }
        error = "Package save cancelled.";
        return false;
    };
    if (!validateOutputDestination(session, target, {}, error) || !keepGoing(0)) {
        return false;
    }

    auto scratch = std::make_shared<PackageScratch>();
    if (!scratch->create()) {
        error = "Could not create package staging directory.";
        return false;
    }

    juce::File folder;
    const auto stage = [&](double p) { return keepGoing(0.5 * p); };
    if (!exportPortableSession(session, channels, scratch->directory, folder, error, stage,
                               savedLoop)) {
        return false;
    }

    if (recovery) {
        const auto project = folder.getChildFile("Project.carta");
        auto root = juce::JSON::parse(project.loadFileAsString());
        if (!root.isObject()) {
            error = "Could not prepare package recovery metadata.";
            return false;
        }
        root.getDynamicObject()->setProperty("recovery", true);
        auto temp = prepareCheckedText(
            project,
            juce::JSON::toString(root, juce::JSON::FormatOptions{}.withMaxDecimalPlaces(24)));
        if (!temp || !commitTemporary(*temp, project, false)) {
            error = "Could not prepare package recovery metadata.";
            return false;
        }
    }

    std::vector<std::pair<juce::String, juce::File>> files{
        {"Project.carta", folder.getChildFile("Project.carta")}};
    for (const auto& mediaFile :
         folder.getChildFile("Media").findChildFiles(juce::File::findFiles, false)) {
        files.emplace_back("Media/" + mediaFile.getFileName(), mediaFile);
    }
    if (files.size() > 100000) {
        error = "Too many package entries.";
        return false;
    }

    double total = 0;
    double done = 0;
    for (const auto& entry : files) {
        total += static_cast<double>(std::max<juce::int64>(1, entry.second.getSize()));
    }
    total = std::max(1.0, total);

#if JUCE_MAC || JUCE_LINUX
    juce::File temporaryPath;
    int descriptor = -1;
    if (!createExclusiveTempFile(target, ".carta-package-", 0644, descriptor, temporaryPath)) {
        error = "Could not create package file.";
        return false;
    }
    juce::TemporaryFile temp(target, temporaryPath);
    auto output = std::make_unique<WavFileOutput>(descriptor);
#else
    juce::TemporaryFile temp(target);
    auto output = temp.getFile().createOutputStream();
    if (!output || output->getStatus().failed()) {
        error = "Could not create package file.";
        return false;
    }
#endif

    if (!output->write("CARTAP01", 8) || !output->writeInt(static_cast<int>(files.size()))) {
        error = "Could not write package header.";
        return false;
    }

    std::vector<char> buffer(1024u * 1024u);
    std::vector<std::pair<juce::File, FileObservation>> observations;
    for (const auto& entry : files) {
        const auto nameBytes = entry.first.getNumBytesAsUTF8();
        const auto size = entry.second.getSize();
        if (!packageNameValid(entry.first) || nameBytes == 0 || nameBytes > 512 || size <= 0) {
            error = "Invalid package entry.";
            return false;
        }
        observations.emplace_back(entry.second, FileObservation(entry.second));
        juce::String hash;
        if (!observations.back().second.isValid() ||
            !hashSourceFile(entry.second, hash, error, [&](double p) {
                return keepGoing(0.5 + 0.49 * (done + static_cast<double>(size) * p * 0.5) / total);
            })) {
            return false;
        }

        juce::FileInputStream input(entry.second);
        if (!input.openedOk() || !output->writeInt(static_cast<int>(nameBytes)) ||
            !output->writeInt64(size) || !output->write(entry.first.toRawUTF8(), nameBytes) ||
            !output->write(hash.toRawUTF8(), 64)) {
            error = "Could not write package entry.";
            return false;
        }

        juce::int64 copied = 0;
        while (copied < size) {
            if (!keepGoing(0.5 + 0.49 *
                                     (done + static_cast<double>(size) * 0.5 +
                                      static_cast<double>(copied) * 0.5) /
                                     total)) {
                return false;
            }
            const auto count = input.read(
                buffer.data(), static_cast<int>(std::min<juce::int64>(
                                   size - copied, static_cast<juce::int64>(buffer.size()))));
            if (count <= 0 || !output->write(buffer.data(), static_cast<size_t>(count))) {
                error = "Could not copy package entry.";
                return false;
            }
            copied += count;
        }
        if (input.getStatus().failed()) {
            error = "Could not read package entry.";
            return false;
        }
        done += static_cast<double>(size);
    }

#if JUCE_MAC || JUCE_LINUX
    if (!output->finish()) {
        error = "Could not finish package file.";
        return false;
    }
#else
    output->flush();
    if (output->getStatus().failed()) {
        error = "Could not finish package file.";
        return false;
    }
#endif
    output.reset();

    if (!keepGoing(0.999)) {
        return false;
    }
    for (const auto& entry : observations) {
        if (!entry.second.matches(entry.first)) {
            error = "Package staging media changed while saving.";
            return false;
        }
    }
    if (!commitTemporary(temp, target, noReplace)) {
        error = "Could not commit package; destination was not replaced.";
        return false;
    }

    const auto synced = syncParentDirectory(target);
    if (directorySync) {
        *directorySync = synced;
    }
    if (progress) {
        progress(1);
    }
    return true;
}

bool loadNativePackage(Session& result, int& master, juce::AudioFormatManager& formats,
                       const juce::File& package, juce::String& error, LoadProgress progress,
                       WavRange* savedLoop, bool* isRecovery) {
    error.clear();
    const auto keepGoing = [&](double p) {
        if (!progress || progress(p)) {
            return true;
        }
        error = "Package opening cancelled.";
        return false;
    };
    if (!keepGoing(0)) {
        return false;
    }

    const FileObservation observation(package);
    juce::FileInputStream input(package);
    const auto length = package.getSize();
    char magic[8]{};
    if (!observation.isValid() || !input.openedOk() || length < 12 || input.read(magic, 8) != 8 ||
        std::memcmp(magic, "CARTAP01", 8) != 0) {
        error = "Invalid or unsupported package header.";
        return false;
    }

    const auto count = input.readInt();
    if (count <= 0 || count > 100000) {
        error = "Invalid package entry count.";
        return false;
    }

    auto scratch = std::make_shared<PackageScratch>();
    if (!scratch->create() || scratch->directory.getChildFile("Media").createDirectory().failed()) {
        error = "Could not create package extraction directory.";
        return false;
    }

    std::set<juce::String> names;
    std::set<juce::String> foldedNames;
    std::map<juce::String, juce::String> hashToMediaName;
    std::vector<std::pair<juce::File, FileObservation>> extracted;
    std::vector<char> buffer(1024u * 1024u);

    for (int i = 0; i < count; ++i) {
        if (!keepGoing(0.70 * static_cast<double>(input.getPosition()) /
                       static_cast<double>(length))) {
            return false;
        }
        if (length - input.getPosition() < 12) {
            error = "Truncated package entry.";
            return false;
        }
        const auto nameLength = input.readInt();
        const auto size = input.readInt64();
        if (nameLength <= 0 || nameLength > 512 || size <= 0 ||
            size > length - input.getPosition() ||
            length - input.getPosition() - size < static_cast<juce::int64>(nameLength) + 64) {
            error = "Invalid package entry size.";
            return false;
        }

        std::vector<char> nameBuffer(static_cast<size_t>(nameLength));
        char hashBuffer[64];
        if (input.read(nameBuffer.data(), nameLength) != nameLength ||
            input.read(hashBuffer, 64) != 64 ||
            std::memchr(nameBuffer.data(), 0, nameBuffer.size()) ||
            !juce::CharPointer_UTF8::isValidString(nameBuffer.data(), nameLength) ||
            !std::all_of(std::begin(hashBuffer), std::end(hashBuffer),
                         [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) {
            error = "Invalid package entry name or checksum.";
            return false;
        }

        const auto name = juce::String::fromUTF8(nameBuffer.data(), nameLength);
        const auto expected = juce::String::fromUTF8(hashBuffer, 64);
        if (!packageNameValid(name) || !names.insert(name).second ||
            !foldedNames.insert(name.toLowerCase()).second || expected.length() != 64 ||
            !expected.containsOnly("0123456789abcdef")) {
            error = "Invalid or duplicate package entry.";
            return false;
        }

        const auto file = scratch->directory.getChildFile(name);
        auto output = file.createOutputStream();
        if (!output) {
            error = "Could not extract package entry.";
            return false;
        }

        juce::int64 copied = 0;
        while (copied < size) {
            if (!keepGoing(0.70 * static_cast<double>(input.getPosition()) /
                           static_cast<double>(length))) {
                return false;
            }
            const auto got = input.read(
                buffer.data(), static_cast<int>(std::min<juce::int64>(
                                   size - copied, static_cast<juce::int64>(buffer.size()))));
            if (got <= 0 || !output->write(buffer.data(), static_cast<size_t>(got))) {
                error = "Could not extract package entry.";
                return false;
            }
            copied += got;
        }
        output->flush();
        if (output->getStatus().failed()) {
            error = "Could not finish extracting package entry.";
            return false;
        }
        output.reset();

        juce::String actual;
        if (!hashSourceFile(file, actual, error, [&](double) {
                return keepGoing(0.70 * static_cast<double>(input.getPosition()) /
                                 static_cast<double>(length));
            })) {
            return false;
        }
        if (actual != expected) {
            error = "Package entry checksum does not match: " + name;
            return false;
        }
        if (name.startsWith("Media/") && !hashToMediaName.count(expected)) {
            hashToMediaName.emplace(expected, name);
        }

        extracted.emplace_back(file, FileObservation(file));
        if (!extracted.back().second.isValid()) {
            error = "Package entry disappeared after extraction.";
            return false;
        }
    }

    if (input.getPosition() != length || input.getStatus().failed() ||
        !names.count("Project.carta") ||
        names.count("Bank.json") != names.count("Timeline-bank.wav") ||
        !observation.matches(package)) {
        error = "Package is incomplete, changed or has trailing data.";
        return false;
    }
    for (const auto& entry : extracted) {
        if (!entry.second.matches(entry.first)) {
            error = "Package entries changed during extraction.";
            return false;
        }
    }

    const auto project = scratch->directory.getChildFile("Project.carta");
    bool projectRewritten = false;
    if (!packageProjectPathsValid(project, names, hashToMediaName, projectRewritten, error)) {
        return false;
    }
    if (projectRewritten) {
        for (auto& entry : extracted) {
            if (entry.first == project) {
                entry.second = FileObservation(project);
                break;
            }
        }
    }

    Session next;
    int nextMaster = 0;
    WavRange nextLoop;
    bool recovered = false;
    if (!loadSession(
            next, nextMaster, formats, project, error,
            [&](double p) { return keepGoing(0.70 + 0.29 * p); }, &recovered, nullptr, nullptr,
            nullptr, nullptr, &nextLoop)) {
        return false;
    }
    if (!keepGoing(1) || !observation.matches(package)) {
        if (error.isEmpty()) {
            error = "Package changed while opening.";
        }
        return false;
    }
    for (const auto& entry : extracted) {
        if (!entry.second.matches(entry.first)) {
            error = "Package entries changed while opening.";
            return false;
        }
    }

    for (const auto& track : next.tracks) {
        for (const auto& clip : track.clips) {
            clip.audio->sourceOwner = scratch;
        }
    }

    result = std::move(next);
    master = nextMaster;
    if (savedLoop) {
        *savedLoop = nextLoop;
    }
    if (isRecovery) {
        *isRecovery = recovered;
    }
    return true;
}

} // namespace carta
