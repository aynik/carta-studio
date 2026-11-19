/**
 * Real-time playback transport, peak metering, and audition preview preparation.
 *
 * Coordinates:
 * - `PlaybackRange` and `position` use timeline seconds on the canonical
 *   44.1 kHz (`sessionRate`) project grid, whereas audio callbacks supply
 *   `deviceRate` and device frame counts (`begin`, `count`).
 * - Preview ranges and `DiscPreviewPlan` spans use half-open `[start, end)`
 *   44.1 kHz project samples (`projectEnd`).
 *
 * Real-time callback contract:
 * - `OutputPeakHold::observe()`, `renderPlayback()`, and `renderDiscPreview()`
 *   perform zero locks and zero heap allocations when supplied with prepared
 *   `ResamplingContext` / `DiscPreviewRenderer` / `RenderClipIndex` state.
 * - All `prepare*` helpers allocate and run off the audio thread.
 */
#pragma once
#include "Core/Session.h"
#include <atomic>

namespace carta {
struct TimeSelection;
struct ClipReference;

// =========================================================================
// Output Validation, Playhead Seeking & Peak Metering
// =========================================================================

/**
 * Check whether the hardware channel count satisfies the master format.
 *
 * Mono can use one output; stereo must never silently fold to one channel.
 */
bool outputSupportsMaster(int activeChannels, int masterChannels) noexcept;

/**
 * Inspect only the active callback region `[start, start + count)`.
 *
 * Finite over-range samples are valid; returns true if any sample is NaN/Inf
 * or the slice bounds are invalid.
 */
bool containsNonFiniteAudio(const juce::AudioBuffer<float>&, int start, int count) noexcept;

/** Discrete playhead navigation commands on the canonical project grid. */
enum class PlayheadSeek { previousSample, nextSample, start, end };

/**
 * Seek on the 44.1 kHz project grid within `[0, projectEnd]`, end inclusive.
 *
 * On failure, `position` is left unchanged.
 */
bool seekPlayhead(double& position, juce::int64 projectEnd, PlayheadSeek) noexcept;

/** Lock-free snapshot of stereo output peak magnitudes. */
struct OutputPeakSnapshot {
    float left = 0;
    float right = 0;
    int channels = 0;
};

/**
 * Atomic stereo peak accumulator shared between the audio callback and UI.
 *
 * Holds output sample peaks until reset; `observe()` performs no allocation or locks.
 */
class OutputPeakHold {
  public:
    void observe(const juce::AudioBuffer<float>&, int start, int count) noexcept;
    void reset() noexcept;
    OutputPeakSnapshot snapshot() const noexcept;

    /** Returns and clears the peaks accumulated since the previous poll. */
    OutputPeakSnapshot takeRecent() noexcept;

  private:
    std::atomic<float> left{0};
    std::atomic<float> right{0};
    std::atomic<int> channels{0};
    std::atomic<float> recentLeft{0};
    std::atomic<float> recentRight{0};
    std::atomic<int> recentChannels{0};
};

/** Apply a 24 dB/second display falloff without changing the audio. */
float decayOutputPeak(float previous, float incoming, double elapsedSeconds) noexcept;

// =========================================================================
// Playback Range & Transport Clock
// =========================================================================

/**
 * Playback interval and track/clip scope.
 *
 * Playback interval is half-open `[start, end)`. The caller clears the
 * destination first. Position is in timeline seconds, and is wrapped into
 * the interval when looping.
 */
struct PlaybackRange {
    double start = 0;
    double end = 0;
    bool loop = false;
    TrackRange tracks{};
    int onlyClip = -1; // When set, tracks must identify exactly one track.
};

/**
 * Retains playback frame count and loop phase across callbacks without
 * accumulating error in large absolute timestamps. One clock per transport.
 */
struct PlaybackClock {
    double phase = 0;
    double lastPosition = 0;
    double start = 0;
    double end = 0;
    double rate = 0;
    double origin = 0;
    juce::int64 frames = 0;
    bool valid = false;
    bool loop = false;

    void reset() noexcept { valid = false; }
};

/** Full master uses the same exclusive project-sample end as WAV export. */
PlaybackRange fullMasterPlaybackRange(const Session&) noexcept;

/**
 * Build a non-looping, sample-aligned preview, clipped to the project bounds.
 *
 * Returns false for invalid inputs or an empty preview without altering `result`.
 */
bool boundaryPreviewRange(juce::int64 boundary, juce::int64 before, juce::int64 after,
                          juce::int64 projectEnd, PlaybackRange& result);

/** Expand a selected half-open sample range by context, clipping to the master. */
bool selectionPreviewRange(juce::int64 start, juce::int64 end, juce::int64 before,
                           juce::int64 after, juce::int64 projectEnd, PlaybackRange& result);

// =========================================================================
// Edit & Crossfade Audition Preparation (Off-Thread)
// =========================================================================

/**
 * Build the same candidate as a ripple deletion without changing the source.
 *
 * On failure, neither output is modified.
 */
bool prepareRipplePreview(const Session&, const TimeSelection&, bool allTracks, juce::int64 before,
                          juce::int64 after, Session& result, PlaybackRange& range,
                          juce::String& error);

/** Build a ripple-deletion audition candidate restricted to explicit tracks. */
bool prepareIndependentRipplePreview(const Session&, const TimeSelection&,
                                     const std::vector<int>& tracks, juce::int64 before,
                                     juce::int64 after, Session& result, PlaybackRange& range,
                                     juce::String& error);

/** Isolate explicit tracks for auditioning a time selection without mutating `Session`. */
bool prepareIndependentTrackAudition(const Session&, const TimeSelection&,
                                     const std::vector<int>& tracks, bool loop, Session& result,
                                     PlaybackRange& range, juce::String& error);

/**
 * Audition the actual crossfade over its complete overlap and surrounding context.
 *
 * Failure leaves the candidate and range unchanged.
 */
bool prepareSelectedCrossfadePreview(const Session&, ClipReference first, ClipReference second,
                                     juce::int64 before, juce::int64 after, Session& result,
                                     PlaybackRange& range, juce::String& error);

/** Audition the crossfade of a single clip against its unambiguous same/cross-track neighbor. */
bool prepareCrossfadePreview(const Session&, int track, int clip, bool acrossTracks,
                             juce::int64 before, juce::int64 after, Session& result,
                             PlaybackRange& range, juce::String& error);

/** Prepare the same isolated clip mix and sample span used by group export. */
bool prepareClipGroupAudition(const Session&, const std::vector<ClipReference>&, bool loop,
                              Session& result, PlaybackRange& range, juce::String& error);

// =========================================================================
// MiniDisc Logical Disc Preview & Real-Time Renderer
// =========================================================================

/**
 * Immutable mapping of logical disc playback to the current timeline master.
 *
 * Prepared away from the audio callback; repeated ranges reference one bank.
 */
struct DiscPreviewSpan {
    juce::int64 logicalStart = 0;
    juce::int64 logicalEnd = 0;
    juce::int64 sourceStart = 0;
};

/**
 * Spans cover any preceding filter context as well as the audible suffix.
 *
 * `samples` is audible length; `programStart` is both its offset into spans and
 * the program-clock position. Standalone track previews start at zero.
 */
struct DiscPreviewPlan {
    std::vector<DiscPreviewSpan> spans;
    juce::int64 samples = 0;
    juce::int64 programStart = 0;
};

/** Build a logical MiniDisc playback plan for the full disc or from `trackId`. */
bool prepareDiscPreview(const Session&, DiscPreviewPlan&, juce::String& error,
                        const juce::String& trackId = {}, bool throughEnd = false);

/** Working storage and resampling tables are prepared before playback. */
struct DiscPreviewRenderer {
    static constexpr int maxOutputChunk = 256;
    double deviceRate = 0;
    int channels = 0;
    int taps = 0;
    ResamplingContext bankResampling;
    std::unique_ptr<SincResampler> outputResampler;
    juce::AudioBuffer<float> scratch;
};

/** Prepare 44.1 kHz bank resampling and device-rate conversion storage off-thread. */
bool prepareDiscPreviewRenderer(const Session&, double deviceRate, int masterChannels,
                                DiscPreviewRenderer&, juce::String& error);

/**
 * Renders without allocation, retaining device-frame position across callbacks.
 *
 * The destination is cleared by the caller; returns false at the logical end.
 */
bool renderDiscPreview(const Session&, const DiscPreviewPlan&, DiscPreviewRenderer&,
                       juce::AudioBuffer<float>&, int begin, int count, juce::int64& outputFrames,
                       double deviceRate, int masterChannels, bool* unsupportedRate = nullptr);

// =========================================================================
// Real-Time Timeline Playback
// =========================================================================

/**
 * Render `count` device frames at `deviceRate` into `output` starting at `begin`.
 *
 * Returns false when the interval ends; remaining output stays untouched.
 * Retain a `PlaybackClock` across callbacks for accurate long-position loops and
 * continuous playback. Without one, phase is reconstructed from `position` at
 * each call and can accumulate rounding error, especially for short loops.
 * If provided, `unsupportedRate` must initially be false; it is set when an
 * audible clip exceeds the converter's full-support ratio and playback stops.
 */
bool renderPlayback(const Session&, juce::AudioBuffer<float>&, int begin, int count,
                    double& position, double deviceRate, int masterChannels, const PlaybackRange&,
                    const ResamplingContext* prepared = nullptr, PlaybackClock* clock = nullptr,
                    const RenderClipIndex* index = nullptr, bool* unsupportedRate = nullptr);

} // namespace carta
