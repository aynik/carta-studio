/**
 * Core project model, source media storage, waveform peak cache, and renderer.
 *
 * Coordinates & invariants:
 * - Project timeline positions use seconds anchored to the canonical 44.1 kHz
 *   grid (`sessionRate = 44100.0`), with half-open intervals `[start, start + duration)`
 *   and project-sample positions strictly below 2^53 (`9007199254740992.0`) so
 *   double-precision seconds round-trip without sample drift.
 * - Clip `offset`, `fadeInStart`, and `fadeOutEnd` are measured in native source
 *   seconds from frame 0 of `Clip::audio`, so splitting or trimming retains the
 *   underlying fade curve.
 * - Decoded PCM inside `Audio` uses the source's native `sampleRate` (stored
 *   either in RAM or in a read-only unlinked `MappedPcm` sparse file).
 *
 * Ownership & threading:
 * - `Audio`, `MappedPcm`, and `WaveformPeakCache` are immutable after publication
 *   and shared across edits, undo snapshots, clipboard, and background workers.
 * - `Session::render()` allocates and locks nothing when supplied a prepared
 *   `ResamplingContext` and `RenderClipIndex`, making it safe for real-time audio
 *   callbacks; import, hashing, verification, and peak building run off-thread.
 */
#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>
#include <optional>
#include "MiniDisc/MdAuthoring.h"
#include <map>
#include <vector>
#include <functional>
#include <atomic>
#include "Audio/Resampler.h"

namespace carta {

// =========================================================================
// Project Grid Constants & Geometry Predicates
// =========================================================================

constexpr double sessionRate = 44100.0;
inline constexpr float maxLinearGain = 16.0f;

/**
 * Recognize canonical sample-derived seconds even when multiplying back at
 * long positions incurs more than the legacy fixed floating-point tolerance.
 */
bool markerPositionOnSampleGrid(double seconds) noexcept;

// =========================================================================
// Waveform Display Scaling
// =========================================================================

/**
 * Waveform display scaling is visual only. The normalized result is clipped to
 * the visible lane; a separate over-range indicator uses the original sample.
 */
enum class WaveformScale { linear, decibels };

/** Map a raw audio sample to normalized lane height `[-1, 1]`. */
float waveformDisplayValue(float sample, int verticalZoom,
                           WaveformScale scale = WaveformScale::linear) noexcept;

/** RMS is a magnitude; limit its overlay to polarities present in the peak range. */
juce::Range<float> waveformRmsDisplayRange(float low, float high, float magnitude) noexcept;

/** dBFS represented by a positive normalized lane magnitude (0 means -inf). */
double waveformDisplayDb(double laneMagnitude, int verticalZoom,
                         WaveformScale scale = WaveformScale::linear) noexcept;

/** Shared by editing and project I/O; the end must remain precisely representable. */
bool validTimelineGeometry(double start, double offset, double duration) noexcept;

// =========================================================================
// Decoded Source Media & Waveform Peak Hierarchy
// =========================================================================

/** Temporary disk-backed float32 planar PCM mapping for large imported audio. */
struct MappedPcm {
    explicit MappedPcm(const juce::File& directory = {})
        : temporary(directory == juce::File{}
                        ? juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("audio.carta-pcm")
                        : directory.getChildFile("audio.carta-pcm")) {}

    juce::TemporaryFile temporary;
    std::unique_ptr<juce::MemoryMappedFile> mapping;
    int channels = 0;
    int frames = 0;
};

/** Cue point imported from WAV `cue `/`labl` chunks in native source frames. */
struct SourceCue {
    juce::int64 sample = 0;
    juce::String name;
};

/**
 * Peak payload reservations are shared by all live sources, including those
 * retained by undo and background jobs. Vector bookkeeping is not counted.
 */
struct WaveformPeakBudget {
    explicit WaveformPeakBudget(size_t limitBytes) : limit(limitBytes) {}

    size_t reserveUpTo(size_t requested) noexcept;
    void release(size_t bytes) noexcept { used.fetch_sub(bytes, std::memory_order_relaxed); }
    size_t allocated() const noexcept { return used.load(std::memory_order_relaxed); }

    const size_t limit;

  private:
    std::atomic<size_t> used{0};
};

/** Two-tier (base block + 16x pyramid) min/max waveform peak cache per channel. */
struct WaveformPeakCache {
    WaveformPeakCache() = default;
    WaveformPeakCache(const WaveformPeakCache&) = delete;
    WaveformPeakCache& operator=(const WaveformPeakCache&) = delete;

    ~WaveformPeakCache() {
        if (budget && reserved) {
            budget->release(reserved);
        }
    }

    std::vector<std::vector<juce::Range<float>>> peaks;
    juce::int64 blockSize = 256;
    std::vector<std::vector<std::vector<juce::Range<float>>>> levels;
    std::shared_ptr<WaveformPeakBudget> budget;
    size_t reserved = 0;
};

/**
 * Immutable decoded audio source shared across clips, undo history, and workers.
 *
 * PCM channels are stored planar either in `samples` or in `mapped`.
 */
struct Audio {
    juce::File file;
    // Owns extracted package media for as long as any clip, undo snapshot or
    // background job retains this source. Ordinary imports leave it empty.
    std::shared_ptr<void> sourceOwner;
    juce::String sourceHash; // SHA-256 of the source bytes as imported.
    juce::AudioBuffer<float> samples;
    double sampleRate = 0;
    juce::String sourceFormat;            // Reader-reported source format; empty when unknown.
    unsigned int sourceBitsPerSample = 0; // Reader-reported; zero when unknown.
    std::vector<SourceCue> sourceCues;    // Captured from WAV metadata at import.
    juce::String sourceCueError; // Invalid or unsupported cue metadata; audio remains usable.
    // Immutable after publication; copied Audio objects share the cache and
    // its budget reservation instead of duplicating peak payload.
    std::shared_ptr<WaveformPeakCache> peakCache = std::make_shared<WaveformPeakCache>();
    std::shared_ptr<MappedPcm> mapped;

    int numChannels() const noexcept {
        return mapped ? mapped->channels : samples.getNumChannels();
    }
    int numSamples() const noexcept { return mapped ? mapped->frames : samples.getNumSamples(); }

    const float* channelData(int channel) const noexcept {
        return mapped ? static_cast<const float*>(mapped->mapping->getData()) +
                            static_cast<size_t>(channel) * static_cast<size_t>(mapped->frames)
                      : samples.getReadPointer(channel);
    }

    juce::Range<float> minMax(int channel, int start, int count) const noexcept {
        return juce::FloatVectorOperations::findMinAndMax(channelData(channel) + start, count);
    }

    double length() const { return numSamples() / sampleRate; }
};

/**
 * Rebuild the per-channel min/max cache after decoding source media.
 *
 * The budget limits range payload per source; vector overhead is additional.
 * Cancellation leaves any previous cache intact.
 */
bool buildWaveformPeaks(Audio&, size_t maxBytes = 16u * 1024u * 1024u,
                        std::function<bool(double)> progress = {},
                        std::shared_ptr<WaveformPeakBudget> budget = {});

/** Exact min/max of `[begin, end)`; uses cached blocks only when fully covered. */
juce::Range<float> waveformRange(const Audio&, int channel, int begin, int end);

// =========================================================================
// Clips, Envelopes & Waveform Geometry
// =========================================================================

/** Curve shapes supported for clip fade-in and fade-out envelopes. */
enum class FadeShape { linear = 0, equalPower = 1, sCurve = 2, exponential = 3 };

/** Evaluate normalized fade gain in `[0, 1]` at progress `t` in `[0, 1]`. */
float fadeCurveGain(double t, FadeShape shape) noexcept;

const char* fadeShapeToString(FadeShape shape) noexcept;
bool parseFadeShape(const std::string& text, FadeShape& out) noexcept;

/**
 * Single timeline clip referencing an immutable `Audio` source.
 *
 * `start` and `duration` place `[start, start + duration)` in timeline seconds;
 * `offset` and fade anchors (`fadeInStart`, `fadeOutEnd`) are source-time seconds.
 */
struct Clip {
    std::shared_ptr<Audio> audio;
    double start = 0;
    double offset = 0;
    double duration = 0;
    float gain = 1.0f;
    // Fade anchors are source times, so splitting or trimming keeps the curve.
    double fadeIn = 0;
    double fadeOut = 0;
    double fadeInStart = 0;
    double fadeOutEnd = 0;
    juce::String name{};    // Empty uses the source filename.
    juce::String groupId{}; // Empty means ungrouped; otherwise 32 lowercase hexadecimal digits.
    FadeShape fadeInShape = FadeShape::linear;
    FadeShape fadeOutShape = FadeShape::linear;
};

inline bool validClipGroupId(const juce::String& id) noexcept {
    return id.isEmpty() || (id.length() == 32 && id.containsOnly("0123456789abcdef"));
}

/** Conservative cached bounds without reading PCM; false means no complete cache. */
bool cachedWaveformBounds(const Clip&, int channel, int begin, int end, juce::Range<float>&);

/**
 * Map a half-open timeline interval to the source samples whose timestamps it
 * contains, intersected with the clip's trimmed source range.
 */
bool waveformSampleRange(const Clip&, double timelineStart, double timelineEnd, int& begin,
                         int& end) noexcept;

/**
 * Visible source points plus at most one neighbor on either side for line
 * continuity, clipped to the source and the clip's half-open trimmed range.
 */
bool waveformPointRange(const Clip&, double timelineStart, double timelineEnd, int& begin,
                        int& end) noexcept;

juce::String clipDisplayName(const Clip&);
float clipLevel(const Clip&, double sourceTime) noexcept;

/**
 * Clip-envelope waveform min/max for `[begin, end)` source samples.
 *
 * Small ranges are exact; larger fading ranges conservatively bound all
 * affected samples. If supplied, `exact` reports whether the returned extrema
 * came from exact samples or a conservative wide-fade bound.
 */
juce::Range<float> clipWaveformRange(const Clip&, int channel, int begin, int end,
                                     bool* exact = nullptr);

/** Peak and optional RMS metrics for one horizontal pixel column of a clip lane. */
struct WaveformColumn {
    juce::Range<float> range;
    bool hasSamples = false;
    float rms = 0;
    bool hasRms = false;
};

/**
 * Exact visible-column peaks after the clip envelope.
 *
 * Intended for a worker: cancellation or invalid geometry leaves `output` unchanged.
 */
bool exactWaveformColumns(const Clip&, int channel, double viewStart, double viewSpan,
                          int timelineLeft, int timelineWidth, int firstX, int columns,
                          std::vector<WaveformColumn>& output, std::function<bool()> cancelled = {},
                          bool includeRms = false);

bool setClipEnvelope(Clip&, float gain, double fadeInSeconds, double fadeOutSeconds) noexcept;

/** Change one fade and anchor it to the corresponding current clip edge. */
bool setClipFade(Clip&, bool fadeIn, double seconds) noexcept;

// =========================================================================
// Tracks, Channel Routing & Visual Layout
// =========================================================================

/** Linear gain matrix row routing one source channel to stereo `(left, right)`. */
struct StereoRoute {
    float left = 0;
    float right = 0;
};

/** Per-track timeline display mode override. */
enum class TrackView { useDefault = 0, waveform = 1, spectrogram = 2, combined = 3 };

/** The automatic routing centers mono and averages odd/even numbered channels. */
StereoRoute defaultChannelRoute(int channel, int channelCount) noexcept;

/** Timeline layer holding clips, mix controls, routing, and lane layout settings. */
struct Track {
    juce::String name;
    std::vector<Clip> clips;
    int height = 116;
    bool muted = false;
    bool solo = false;
    float gain = 1.0f;
    float pan = 0.0f; // Linear gain; -1 left, +1 right.
    // Empty means automatic mono/stereo. A nonempty matrix maps source
    // channel indices to stereo; extra input channels require confirmation.
    std::vector<StereoRoute> routing{};
    TrackView view = TrackView::useDefault;
    bool collapsed = false;        // Retains height for expansion.
    int displaySourceChannel = -1; // -1 shows all source channels; 0/1 are stereo-channel views.
    int waveformZoom = 0;          // 0 follows the session default; otherwise 1, 2, 4 or 8.
    std::vector<int>
        channelHeightWeights{}; // Empty uses equal heights; one positive weight per source lane.
};

inline bool validWaveformZoom(int zoom) noexcept {
    return zoom == 0 || zoom == 1 || zoom == 2 || zoom == 4 || zoom == 8;
}

inline int trackWaveformZoom(const Track& track, int defaultZoom) noexcept {
    return track.waveformZoom == 0 ? defaultZoom : track.waveformZoom;
}

constexpr int collapsedTrackHeight = 40;

inline int trackDisplayHeight(const Track& track) noexcept {
    return track.collapsed ? collapsedTrackHeight : track.height;
}

inline bool trackShowsSpectrum(const Track& track, bool defaultSpectrum) noexcept {
    return track.view == TrackView::spectrogram ||
           (track.view == TrackView::useDefault && defaultSpectrum);
}

/** Vertical pixel regions for waveform and spectrogram panes inside one track row. */
struct TrackViewLayout {
    int waveformTop = 0;
    int waveformHeight = 0;
    int spectrumTop = 0;
    int spectrumHeight = 0;
};

/** Offsets and heights within a track's content area, excluding its title. */
TrackViewLayout trackViewLayout(int height, TrackView view, bool defaultSpectrum) noexcept;

/** Pixel offsets for one source-channel lane in a shared track layout. */
juce::Range<int> channelLaneBounds(int height, int lane, int channels) noexcept;

bool validChannelHeightWeights(const std::vector<int>& weights) noexcept;

/** Covers `[0, height)` without gaps. A mismatched saved layout falls back to equal lanes. */
std::vector<juce::Range<int>> channelLaneLayout(int height, int channels,
                                                const std::vector<int>& weights);

/** Reference-counted summary of one decoded `Audio` source used in a track or session. */
struct TrackSource {
    std::shared_ptr<Audio> audio;
    int clipCount = 0;
};

/** One entry per shared decoded source, in first-use order. */
std::vector<TrackSource> trackSources(const Track&);

int maxTrackChannels(const Track&) noexcept;

/** Returns the requested isolated source lane only while routing and all clips support it. */
int isolatedDisplayChannel(const Track&) noexcept;

bool clipRoutingConfirmed(const Track&, int channelCount) noexcept;

/**
 * Whether applying a complete matrix changes any current per-clip route,
 * including fallback routes for clips with fewer source channels.
 */
bool routingMatrixChangesPreview(const Track&, const std::vector<StereoRoute>&);

/** Master track boundary marker; `id` follows the marker through moves, undo, and project saves. */
struct Marker {
    double position = 0;
    juce::String name;
    juce::String id = juce::Uuid().toString().removeCharacters("-").toLowerCase();
};

/** Horizontal title allocation for one clip in the shared track title strip. */
struct ClipTitleRegion {
    int clip;
    juce::Range<int> bounds;
};

/**
 * Normalize only near-integer project-sample boundaries, matching the renderer.
 *
 * Non-grid fractional positions are returned unchanged.
 */
double canonicalProjectTime(double time) noexcept;

/** Half-open timeline intervals where at least two clips on one track overlap. */
std::vector<juce::Range<double>> clipOverlapRanges(const Track&, double viewportStart,
                                                   double viewportEnd);

/** Number of other clips with a nonempty temporal intersection. */
int overlappingClipCount(const Track&, int clip) noexcept;

/** Counts in clip-index order; touching or invalid intervals do not overlap. */
std::vector<int> overlappingClipCounts(const Track&);

// =========================================================================
// Session, Spatial Render Index & Audio Import
// =========================================================================

/**
 * Conservative output-block interval that can contain timestamps in a clip.
 *
 * The renderer performs the definitive boundary check inside this interval.
 */
juce::Range<int> clipRenderWindow(double clipStart, double clipEnd, double blockStart,
                                  double outputRate, int blockSize) noexcept;

/**
 * Conservative coarse rejection before exact sample-boundary normalization.
 *
 * Non-finite inputs are retained for the detailed path.
 */
bool clipMayIntersectBlock(double clipStart, double clipEnd, double blockStart,
                           double blockLast) noexcept;

/** Inclusive track range, or `{-1, -1}` for the full mix. */
struct TrackRange {
    int first = -1;
    int last = -1;
};

struct Session;

/**
 * Hierarchical bounds in original clip order.
 *
 * Valid only while the session's track/clip geometry is unchanged; callers
 * build it for immutable snapshots before real-time callback rendering.
 */
class RenderClipIndex {
  public:
    using Geometry = std::vector<std::vector<std::pair<double, double>>>;
    static Geometry captureGeometry(const Session&);

    explicit RenderClipIndex(const Session&);
    explicit RenderClipIndex(const Geometry&);

    template <class Visit> void forEach(int track, double first, double last, Visit&& visit) const {
        if (track < 0 || track >= static_cast<int>(trees.size())) {
            return;
        }

        const auto& tree = trees[static_cast<size_t>(track)];
        const auto walk = [&](const auto& self, size_t node) -> void {
            if (node >= tree.size()) {
                return;
            }
            const auto& item = tree[node];
            if (!clipMayIntersectBlock(item.start, item.end, first, last)) {
                return;
            }
            if (item.clip >= 0) {
                visit(item.clip);
                return;
            }
            self(self, node * 2 + 1);
            self(self, node * 2 + 2);
        };
        walk(walk, 0);
    }

  private:
    struct Node {
        double start = 0;
        double end = 0;
        int clip = -1;
    };

    std::vector<std::vector<Node>> trees;
};

/** Partition the shared title strip without placing titles over one another. */
std::vector<ClipTitleRegion> layoutClipTitles(const std::vector<juce::Range<int>>&,
                                              juce::Range<int> viewport);

/** Complete editable project state including tracks, markers, and MiniDisc UTOC layout. */
struct Session {
    std::vector<Track> tracks;
    std::vector<Marker> markers;
    // Logical disc metadata only; bank lengths do not imply encoded audio exists.
    MdDisc mdDisc;
    // Optional minimum end, in project samples; clips may extend beyond it.
    juce::int64 minimumEndSample = 0;

    double length() const;
    bool split(int track, int clip, double at);

    /**
     * Render `[startSample, startSample + count)` into `output` starting at `startTime`.
     *
     * Additive routing to stereo followed by optional mono fold-down.
     * `sourceChannels` is for a validated consolidation snapshot: it renders
     * source lanes with clip envelopes before track gain, pan and routing.
     */
    void render(juce::AudioBuffer<float>& output, int startSample, int count, double startTime,
                double outputRate, int masterChannels, const ResamplingContext* prepared = nullptr,
                TrackRange scope = {}, int onlyClip = -1, const RenderClipIndex* index = nullptr,
                bool* unsupportedRate = nullptr, bool sourceChannels = false) const;

    void prepareResampling(ResamplingContext&, double outputRate) const;
};

/**
 * Add an imported source on a new track at a project sample position.
 *
 * Failure leaves the session unchanged.
 */
bool addImportedAudioTrack(Session&, const std::shared_ptr<Audio>&, juce::int64 startSample);

/** Add an imported source onto an existing `track` at `startSample`; failure changes nothing. */
bool addImportedAudioToTrack(Session&, int track, const std::shared_ptr<Audio>&,
                             juce::int64 startSample);

/**
 * Exclusive 44.1 kHz end frame, with near-integer floating-point noise removed.
 *
 * Returns zero for an empty or unrepresentable arrangement.
 */
juce::int64 projectEndSample(const Session&) noexcept;

/** Same sample-boundary normalization as `projectEndSample` for a known audio end. */
juce::int64 projectEndSampleForAudioEnd(double audioEnd, juce::int64 minimumEndSample) noexcept;

// =========================================================================
// Source Integrity, Checksum Verification & Audio Loading
// =========================================================================

using LoadProgress = std::function<bool(double)>;

/** Compute SHA-256 of a regular file while verifying no concurrent modification occurs. */
bool hashSourceFile(const juce::File&, juce::String& hash, juce::String& error,
                    LoadProgress progress = {});

/** Outcome of verifying a source file on disk against its imported SHA-256 digest. */
enum class SourceStatus { matches, changed, missing, unverifiable, unreadable, changedDuringCheck };

/** Per-source integrity status and human-readable detail. */
struct SourceCheck {
    SourceStatus status = SourceStatus::unreadable;
    juce::String detail;
};

/**
 * Verify a snapshot of sources against their imported hashes.
 *
 * Cancellation returns false and leaves the output unchanged; other per-file
 * failures are reported in the corresponding result.
 */
bool verifyTrackSources(const std::vector<TrackSource>&, std::vector<SourceCheck>&,
                        juce::String& error, LoadProgress progress = {});

/** Last successful checks are session-local and do not retain decoded media. */
class SourceVerificationCache {
  public:
    bool record(const std::vector<TrackSource>&, const std::vector<SourceCheck>&,
                const juce::String& checkedAt);
    const SourceCheck* lookup(const std::shared_ptr<Audio>&,
                              juce::String* checkedAt = nullptr) const noexcept;
    bool trackHasIssue(const Track&) const noexcept;
    void prune() noexcept;

  private:
    struct Entry {
        std::weak_ptr<Audio> audio;
        juce::File path;
        juce::String hash;
        juce::String checkedAt;
        SourceCheck check;
    };

    std::map<const Audio*, Entry, std::less<const Audio*>> entries;
};

/** Collect unique decoded `Audio` sources across all tracks in first-use order. */
std::vector<TrackSource> sessionSources(const Session&);

/**
 * Parse JUCE's WAV cue metadata atomically.
 *
 * Unsupported cue regions/notes or malformed identifiers and offsets return an
 * error without changing `result`.
 */
bool parseWavSourceCues(const juce::StringPairArray&, juce::int64 sourceSamples,
                        std::vector<SourceCue>& result, juce::String& error);

/**
 * Decode an audio file into an immutable `Audio` payload with SHA-256 and waveform peaks.
 *
 * A nonempty `temporaryDirectory` must already exist; the default uses JUCE's
 * platform temporary directory.
 */
std::shared_ptr<Audio> loadAudio(juce::AudioFormatManager&, const juce::File&, juce::String& error,
                                 LoadProgress progress = {},
                                 juce::int64 memoryThresholdBytes = 64LL * 1024 * 1024,
                                 const juce::File& temporaryDirectory = {},
                                 std::shared_ptr<WaveformPeakBudget> peakBudget = {});

/**
 * Decode a checksum-identical replacement and compare every decoded PCM bit.
 *
 * Failure or cancellation leaves the original source untouched.
 */
std::shared_ptr<Audio> loadVerifiedReplacement(juce::AudioFormatManager&,
                                               const std::shared_ptr<Audio>& original,
                                               const juce::File&, juce::String& error,
                                               LoadProgress progress = {});

} // namespace carta
