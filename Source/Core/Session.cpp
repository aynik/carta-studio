/**
 * Core project model, source media loading, peak caching, and audio renderer.
 *
 * Implements canonical 44.1 kHz timeline geometry (`canonicalProjectTime`,
 * `projectEndSample`), stereo routing/panning and real-time-safe rendering
 * (`Session::render`), adaptive min/max waveform peak pyramids, SHA-256 source
 * verification, WAV cue import, and sparse-file PCM mapping (`MappedPcm`).
 */
#include "Core/Session.h"
#include "IO/FileObservation.h"
#if CARTA_STUDIO_MINIMP3
#include "IO/NativeMp3Format.h"
#endif
#if JUCE_MAC || CARTA_STUDIO_MINIMP3
#include "IO/Mp3Gapless.h"
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <charconv>
#include <map>
#include <set>
#include <juce_cryptography/juce_cryptography.h>
#if JUCE_MAC || JUCE_LINUX
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace carta {
namespace {

std::shared_ptr<WaveformPeakBudget> defaultPeakBudget() {
    static auto budget = std::make_shared<WaveformPeakBudget>(128u * 1024u * 1024u);
    return budget;
}

bool clipInterval(const Clip& clip, double& start, double& end) noexcept {
    start = clip.start;
    end = clip.start + clip.duration;
    if (!clip.audio || !std::isfinite(start) || start < 0 || !std::isfinite(clip.duration) ||
        clip.duration <= 0 || !std::isfinite(end) || end <= start) {
        return false;
    }

    start = canonicalProjectTime(start);
    end = canonicalProjectTime(end);
    return end > start;
}

} // namespace

bool markerPositionOnSampleGrid(double seconds) noexcept {
    const double frame = seconds * sessionRate;
    if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
        return false;
    }

    const double nearest = std::round(frame);
    return std::abs(frame - nearest) <= 1e-7 ||
           std::equal_to<double>{}(seconds, nearest / sessionRate);
}

size_t WaveformPeakBudget::reserveUpTo(size_t requested) noexcept {
    auto current = used.load(std::memory_order_relaxed);
    for (;;) {
        const auto amount = std::min(requested, limit - current);
        if (used.compare_exchange_weak(current, current + amount, std::memory_order_relaxed)) {
            return amount;
        }
    }
}

double canonicalProjectTime(double time) noexcept {
    const double frame = time * sessionRate;
    const double nearest = std::round(frame);
    return std::isfinite(frame) && std::abs(frame - nearest) < 1.0e-7 ? nearest / sessionRate
                                                                      : time;
}

std::vector<juce::Range<double>> clipOverlapRanges(const Track& track, double viewportStart,
                                                   double viewportEnd) {
    std::vector<juce::Range<double>> result;
    if (!std::isfinite(viewportStart) || !std::isfinite(viewportEnd) || viewportStart < 0 ||
        viewportEnd <= viewportStart) {
        return result;
    }

    std::vector<std::pair<double, int>> events;
    events.reserve(track.clips.size() * 2);

    for (const auto& clip : track.clips) {
        double start = 0;
        double end = 0;
        if (!clipInterval(clip, start, end)) {
            continue;
        }

        start = std::max(start, viewportStart);
        end = std::min(end, viewportEnd);
        if (start < end) {
            events.emplace_back(start, 1);
            events.emplace_back(end, -1);
        }
    }

    std::sort(events.begin(), events.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    int active = 0;
    double overlapStart = 0;
    for (size_t i = 0; i < events.size();) {
        const double time = events[i].first;
        int change = 0;
        do {
            change += events[i++].second;
        } while (i < events.size() && !(time < events[i].first));

        const int next = active + change;
        if (active < 2 && next >= 2) {
            overlapStart = time;
        }
        if (active >= 2 && next < 2 && overlapStart < time) {
            result.emplace_back(overlapStart, time);
        }
        active = next;
    }

    return result;
}

int overlappingClipCount(const Track& track, int index) noexcept {
    if (index < 0 || index >= static_cast<int>(track.clips.size())) {
        return 0;
    }

    double start = 0;
    double end = 0;
    if (!clipInterval(track.clips[static_cast<size_t>(index)], start, end)) {
        return 0;
    }

    int count = 0;
    for (size_t i = 0; i < track.clips.size(); ++i) {
        if (i == static_cast<size_t>(index)) {
            continue;
        }
        double otherStart = 0;
        double otherEnd = 0;
        if (clipInterval(track.clips[i], otherStart, otherEnd) && start < otherEnd &&
            otherStart < end) {
            ++count;
        }
    }

    return count;
}

std::vector<int> overlappingClipCounts(const Track& track) {
    std::vector<double> starts;
    std::vector<double> ends;
    starts.reserve(track.clips.size());
    ends.reserve(track.clips.size());

    for (const auto& clip : track.clips) {
        double start = 0;
        double end = 0;
        if (clipInterval(clip, start, end)) {
            starts.push_back(start);
            ends.push_back(end);
        }
    }

    std::sort(starts.begin(), starts.end());
    std::sort(ends.begin(), ends.end());

    std::vector<int> counts(track.clips.size());
    for (size_t i = 0; i < track.clips.size(); ++i) {
        double start = 0;
        double end = 0;
        if (!clipInterval(track.clips[i], start, end)) {
            continue;
        }
        const auto begun = std::lower_bound(starts.begin(), starts.end(), end) - starts.begin();
        const auto finished = std::upper_bound(ends.begin(), ends.end(), start) - ends.begin();
        counts[i] = static_cast<int>(begun - finished - 1);
    }

    return counts;
}

juce::Range<float> waveformRmsDisplayRange(float low, float high, float magnitude) noexcept {
    if (!std::isfinite(low) || !std::isfinite(high) || !std::isfinite(magnitude) || low > high ||
        magnitude < 0) {
        return {};
    }

    return {std::max(std::min(low, 0.0f), -magnitude), std::min(std::max(high, 0.0f), magnitude)};
}

float waveformDisplayValue(float sample, int verticalZoom, WaveformScale scale) noexcept {
    if (!std::isfinite(sample) || verticalZoom <= 0) {
        return 0;
    }

    if (scale == WaveformScale::decibels) {
        const double magnitude = std::abs(static_cast<double>(sample)) * verticalZoom;
        if (magnitude <= 0) {
            return 0;
        }
        const auto normalized =
            static_cast<float>(std::clamp((20.0 * std::log10(magnitude) + 60.0) / 60.0, 0.0, 1.0));
        return std::copysign(normalized, sample);
    }

    return std::clamp(sample * static_cast<float>(verticalZoom), -1.0f, 1.0f);
}

double waveformDisplayDb(double laneMagnitude, int verticalZoom, WaveformScale scale) noexcept {
    if (!std::isfinite(laneMagnitude) || laneMagnitude <= 0 || verticalZoom <= 0) {
        return -std::numeric_limits<double>::infinity();
    }

    if (scale == WaveformScale::decibels) {
        return (laneMagnitude - 1.0) * 60.0 - 20.0 * std::log10(static_cast<double>(verticalZoom));
    }

    return 20.0 * std::log10(laneMagnitude / static_cast<double>(verticalZoom));
}

TrackViewLayout trackViewLayout(int height, TrackView view, bool defaultSpectrum) noexcept {
    height = std::max(0, height);
    if (view == TrackView::combined) {
        const int waveform = height / 2 + height % 2;
        return {0, waveform, waveform, height - waveform};
    }
    if (view == TrackView::spectrogram || (view == TrackView::useDefault && defaultSpectrum)) {
        return {0, 0, 0, height};
    }

    return {0, height, 0, 0};
}

juce::Range<int> channelLaneBounds(int height, int lane, int channels) noexcept {
    if (height <= 0 || channels <= 0 || lane < 0 || lane >= channels) {
        return {};
    }

    const auto h = static_cast<juce::int64>(height);
    return {static_cast<int>(h * lane / channels),
            static_cast<int>(h * (static_cast<juce::int64>(lane) + 1) / channels)};
}

bool validChannelHeightWeights(const std::vector<int>& weights) noexcept {
    return weights.empty() || (weights.size() >= 2 && weights.size() <= 1024 &&
                               std::all_of(weights.begin(), weights.end(),
                                           [](int value) { return value >= 1 && value <= 1000; }));
}

std::vector<juce::Range<int>> channelLaneLayout(int height, int channels,
                                                const std::vector<int>& weights) {
    std::vector<juce::Range<int>> result;
    if (height <= 0 || channels <= 0 || channels > 1024) {
        return result;
    }

    result.reserve(static_cast<size_t>(channels));
    const bool custom =
        static_cast<int>(weights.size()) == channels && validChannelHeightWeights(weights);

    juce::int64 total = 0;
    if (custom) {
        for (int value : weights) {
            total += value;
        }
    }

    juce::int64 cumulative = 0;
    for (int lane = 0; lane < channels; ++lane) {
        if (!custom) {
            result.push_back(channelLaneBounds(height, lane, channels));
        } else {
            const auto start =
                static_cast<int>((static_cast<juce::int64>(height) * cumulative) / total);
            cumulative += weights[static_cast<size_t>(lane)];
            const auto end =
                static_cast<int>((static_cast<juce::int64>(height) * cumulative) / total);
            result.emplace_back(start, end);
        }
    }

    return result;
}

bool validTimelineGeometry(double start, double offset, double duration) noexcept {
    const double end = start + duration;
    return std::isfinite(start) && std::isfinite(offset) && std::isfinite(duration) && start >= 0 &&
           offset >= 0 && duration > 0 && std::isfinite(end) && end > start &&
           end * sessionRate < 9007199254740992.0 && std::isfinite(offset + duration);
}

juce::String clipDisplayName(const Clip& clip) {
    return clip.name.isNotEmpty() ? clip.name
           : clip.audio           ? clip.audio->file.getFileName()
                                  : juce::String("Missing audio");
}

std::vector<ClipTitleRegion> layoutClipTitles(const std::vector<juce::Range<int>>& clips,
                                              juce::Range<int> viewport) {
    struct Event {
        int x;
        int clip;
        int delta;
    };

    std::vector<ClipTitleRegion> regions;
    if (viewport.isEmpty() || clips.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return regions;
    }

    std::vector<Event> events;
    events.reserve(std::min(clips.size(), size_t{4096}) * 2);
    std::vector<int> visibleClips;
    visibleClips.reserve(std::min(clips.size(), size_t{4096}));

    for (size_t i = 0; i < clips.size(); ++i) {
        const auto visible = clips[i].getIntersectionWith(viewport);
        if (!visible.isEmpty()) {
            const int index = static_cast<int>(visibleClips.size());
            visibleClips.push_back(static_cast<int>(i));
            events.push_back({visible.getStart(), index, 1});
            events.push_back({visible.getEnd(), index, -1});
        }
    }

    if (events.empty()) {
        return regions;
    }

    std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.x < b.x; });

    // Fenwick counts retain input clip order and select an active clip by rank.
    // This avoids rescanning the entire track for every boundary, including
    // dense overlaps where most clips cannot receive even one title pixel.
    std::vector<int> tree(visibleClips.size() + 1);
    int active = 0;

    const auto update = [&](int clip, int delta) {
        for (size_t i = static_cast<size_t>(clip) + 1; i < tree.size(); i += i & (~i + 1)) {
            tree[i] += delta;
        }
        active += delta;
    };

    const auto atRank = [&](int rank) {
        size_t index = 0;
        size_t bit = 1;
        while (bit < tree.size() / 2) {
            bit *= 2;
        }
        for (; bit > 0; bit /= 2) {
            const size_t next = index + bit;
            if (next < tree.size() && tree[next] <= rank) {
                index = next;
                rank -= tree[next];
            }
        }
        return visibleClips[index];
    };

    size_t cursor = 0;
    while (cursor < events.size()) {
        const int start = events[cursor].x;
        do {
            update(events[cursor].clip, events[cursor].delta);
            ++cursor;
        } while (cursor < events.size() && events[cursor].x == start);

        if (cursor == events.size() || active <= 0) {
            continue;
        }

        const int end = events[cursor].x;
        const auto width = static_cast<juce::int64>(end) - start;

        const auto emit = [&](int clip, juce::int64 left, juce::int64 right) {
            const int a = static_cast<int>(left);
            const int b = static_cast<int>(right);
            if (!regions.empty() && regions.back().clip == clip &&
                regions.back().bounds.getEnd() == a) {
                regions.back().bounds = regions.back().bounds.withEnd(b);
            } else {
                regions.push_back({clip, {a, b}});
            }
        };

        if (width < active) {
            for (juce::int64 pixel = 1; pixel <= width; ++pixel) {
                const auto rank = (pixel * active + width - 1) / width - 1;
                emit(atRank(static_cast<int>(rank)), static_cast<juce::int64>(start) + pixel - 1,
                     static_cast<juce::int64>(start) + pixel);
            }
        } else {
            for (int rank = 0; rank < active; ++rank) {
                const auto left = static_cast<juce::int64>(start) + width * rank / active;
                const auto right = static_cast<juce::int64>(start) +
                                   width * (static_cast<juce::int64>(rank) + 1) / active;
                emit(atRank(rank), left, right);
            }
        }
    }

    return regions;
}

double Session::length() const {
    double end = minimumEndSample > 0 && minimumEndSample < 9007199254740992LL
                     ? static_cast<double>(minimumEndSample) / sessionRate
                     : 0;
    for (const auto& track : tracks) {
        for (const auto& clip : track.clips) {
            end = std::max(end, clip.start + clip.duration);
        }
    }
    return end;
}

bool addImportedAudioTrack(Session& session, const std::shared_ptr<Audio>& audio,
                           juce::int64 startSample) {
    if (!audio || startSample < 0 || startSample >= 9007199254740992LL ||
        audio->numChannels() <= 0 || audio->numSamples() <= 0 ||
        !std::isfinite(audio->sampleRate) || audio->sampleRate <= 0) {
        return false;
    }

    const double duration = audio->length();
    const double start = static_cast<double>(startSample) / sessionRate;
    if (!std::isfinite(duration) || duration <= 0 || !std::isfinite(start + duration) ||
        (start + duration) * sessionRate >= 9007199254740992.0) {
        return false;
    }

    Track track;
    track.name = "Layer " + juce::String(static_cast<int>(session.tracks.size() + 1));
    track.clips.push_back({audio, start, 0, duration});
    session.tracks.push_back(std::move(track));
    return true;
}

bool addImportedAudioToTrack(Session& session, int track, const std::shared_ptr<Audio>& audio,
                             juce::int64 startSample) {
    if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
        return false;
    }

    Session temporary;
    if (!addImportedAudioTrack(temporary, audio, startSample)) {
        return false;
    }

    session.tracks[static_cast<size_t>(track)].clips.push_back(
        std::move(temporary.tracks.front().clips.front()));
    return true;
}

juce::int64 projectEndSampleForAudioEnd(double audioEnd, juce::int64 minimumEndSample) noexcept {
    const double frames = audioEnd * sessionRate;
    if (!std::isfinite(frames) || frames >= 9007199254740992.0 || minimumEndSample < 0 ||
        minimumEndSample >= 9007199254740992LL) {
        return 0;
    }
    if (frames <= 0) {
        return minimumEndSample;
    }

    const double nearest = std::round(frames);
    return std::max(minimumEndSample,
                    static_cast<juce::int64>(nearest >= 1 && std::abs(frames - nearest) < 1.0e-7
                                                 ? nearest
                                                 : std::ceil(frames)));
}

juce::int64 projectEndSample(const Session& session) noexcept {
    double audioEnd = 0;
    for (const auto& track : session.tracks) {
        for (const auto& clip : track.clips) {
            audioEnd = std::max(audioEnd, clip.start + clip.duration);
        }
    }
    return projectEndSampleForAudioEnd(audioEnd, session.minimumEndSample);
}

bool Session::split(int track, int index, double at) {
    if (track < 0 || track >= static_cast<int>(tracks.size())) {
        return false;
    }

    auto& clips = tracks[static_cast<size_t>(track)].clips;
    if (index < 0 || index >= static_cast<int>(clips.size())) {
        return false;
    }

    auto& clip = clips[static_cast<size_t>(index)];
    const auto local = at - clip.start;
    if (local < 1.0 / sessionRate || local > clip.duration - 1.0 / sessionRate) {
        return false;
    }

    Clip right = clip;
    right.start = at;
    right.offset += local;
    right.duration -= local;
    clip.duration = local;
    clips.insert(clips.begin() + index + 1, std::move(right));
    return true;
}

StereoRoute defaultChannelRoute(int channel, int channelCount) noexcept {
    if (channel < 0 || channel >= channelCount) {
        return {};
    }
    if (channelCount == 1) {
        return {1, 1};
    }
    if ((channel & 1) == 0) {
        return {1.0f / static_cast<float>((channelCount + 1) / 2), 0};
    }

    return {0, 1.0f / static_cast<float>(channelCount / 2)};
}

int maxTrackChannels(const Track& track) noexcept {
    int maximum = 0;
    for (const auto& clip : track.clips) {
        if (clip.audio) {
            maximum = std::max(maximum, clip.audio->numChannels());
        }
    }
    return maximum;
}

std::vector<TrackSource> trackSources(const Track& track) {
    std::vector<TrackSource> sources;
    std::map<const Audio*, size_t, std::less<const Audio*>> positions;

    for (const auto& clip : track.clips) {
        if (!clip.audio) {
            continue;
        }
        const auto inserted = positions.emplace(clip.audio.get(), sources.size());
        if (inserted.second) {
            sources.push_back({clip.audio, 1});
        } else {
            ++sources[inserted.first->second].clipCount;
        }
    }

    return sources;
}

int isolatedDisplayChannel(const Track& track) noexcept {
    const int channel = track.displaySourceChannel;
    if ((channel != 0 && channel != 1) || track.clips.empty() || track.routing.size() < 2) {
        return -1;
    }

    for (const auto& clip : track.clips) {
        if (!clip.audio || clip.audio->numChannels() != 2) {
            return -1;
        }
    }

    const auto& other = track.routing[static_cast<size_t>(1 - channel)];
    if (std::fpclassify(other.left) != FP_ZERO || std::fpclassify(other.right) != FP_ZERO) {
        return -1;
    }

    return channel;
}

bool clipRoutingConfirmed(const Track& track, int channelCount) noexcept {
    return track.routing.empty() ? channelCount <= 2
                                 : static_cast<int>(track.routing.size()) >= channelCount;
}

bool routingMatrixChangesPreview(const Track& track, const std::vector<StereoRoute>& matrix) {
    std::set<int> counts;
    for (const auto& clip : track.clips) {
        if (clip.audio && clip.audio->numChannels() > 0) {
            counts.insert(clip.audio->numChannels());
        }
    }

    for (int channels : counts) {
        if (matrix.size() < static_cast<size_t>(channels)) {
            return true;
        }

        for (int channel = 0; channel < channels; ++channel) {
            const auto current = static_cast<size_t>(channel) < track.routing.size()
                                     ? track.routing[static_cast<size_t>(channel)]
                                     : defaultChannelRoute(channel, channels);
            const auto proposed = matrix[static_cast<size_t>(channel)];
            if (!std::equal_to<float>{}(current.left, proposed.left) ||
                !std::equal_to<float>{}(current.right, proposed.right)) {
                return true;
            }
        }
    }

    return false;
}

float fadeCurveGain(double t, FadeShape shape) noexcept {
    if (!(t > 0.0)) {
        return 0.0f;
    }
    if (t >= 1.0) {
        return 1.0f;
    }

    switch (shape) {
    case FadeShape::equalPower:
        return static_cast<float>(std::sin(t * 1.5707963267948966));
    case FadeShape::sCurve:
        return static_cast<float>(t * t * (3.0 - 2.0 * t));
    case FadeShape::exponential:
        return static_cast<float>(t * t);
    case FadeShape::linear:
    default:
        return static_cast<float>(t);
    }
}

const char* fadeShapeToString(FadeShape shape) noexcept {
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

bool parseFadeShape(const std::string& text, FadeShape& out) noexcept {
    if (text == "linear") {
        out = FadeShape::linear;
        return true;
    }
    if (text == "equal-power") {
        out = FadeShape::equalPower;
        return true;
    }
    if (text == "s-curve") {
        out = FadeShape::sCurve;
        return true;
    }
    if (text == "exponential") {
        out = FadeShape::exponential;
        return true;
    }
    return false;
}

float clipLevel(const Clip& clip, double sourceTime) noexcept {
    double level = clip.gain;
    if (clip.fadeIn > 0) {
        level *= fadeCurveGain((sourceTime - clip.fadeInStart) / clip.fadeIn, clip.fadeInShape);
    }
    if (clip.fadeOut > 0) {
        level *= fadeCurveGain((clip.fadeOutEnd - sourceTime) / clip.fadeOut, clip.fadeOutShape);
    }
    return static_cast<float>(level);
}

bool setClipEnvelope(Clip& clip, float gain, double fadeInSeconds, double fadeOutSeconds) noexcept {
    if (!std::isfinite(gain) || gain < 0 || gain > maxLinearGain || !std::isfinite(fadeInSeconds) ||
        !std::isfinite(fadeOutSeconds) || fadeInSeconds < 0 || fadeOutSeconds < 0 ||
        fadeInSeconds > clip.duration || fadeOutSeconds > clip.duration) {
        return false;
    }

    clip.gain = gain;
    clip.fadeIn = fadeInSeconds;
    clip.fadeOut = fadeOutSeconds;
    clip.fadeInStart = clip.offset;
    clip.fadeOutEnd = clip.offset + clip.duration;
    return true;
}

bool setClipFade(Clip& clip, bool fadeIn, double seconds) noexcept {
    if (!std::isfinite(seconds) || !std::isfinite(clip.offset) || !std::isfinite(clip.duration) ||
        clip.offset < 0 || clip.duration < 0 || seconds < 0 || seconds > clip.duration ||
        !std::isfinite(clip.offset + clip.duration)) {
        return false;
    }

    if (fadeIn) {
        clip.fadeIn = seconds;
        clip.fadeInStart = clip.offset;
    } else {
        clip.fadeOut = seconds;
        clip.fadeOutEnd = clip.offset + clip.duration;
    }
    return true;
}

void Session::prepareResampling(ResamplingContext& context, double outputRate) const {
    for (const auto& track : tracks) {
        for (const auto& clip : track.clips) {
            context.registerSource(clip.audio->sampleRate, outputRate);
        }
    }
}

bool clipMayIntersectBlock(double clipStart, double clipEnd, double blockStart,
                           double blockLast) noexcept {
    if (!std::isfinite(clipStart) || !std::isfinite(clipEnd) || !std::isfinite(blockStart) ||
        !std::isfinite(blockLast)) {
        return true;
    }

    const double scale = std::max(
        {1.0, std::abs(blockStart), std::abs(blockLast), std::abs(clipStart), std::abs(clipEnd)});
    const double margin = 1.0e-6 + 32.0 * std::numeric_limits<double>::epsilon() * scale;
    return !std::isfinite(margin) ||
           (clipEnd >= blockStart - margin && clipStart <= blockLast + margin);
}

RenderClipIndex::Geometry RenderClipIndex::captureGeometry(const Session& session) {
    Geometry geometry;
    geometry.reserve(session.tracks.size());

    for (const auto& track : session.tracks) {
        auto& row = geometry.emplace_back();
        row.reserve(track.clips.size());
        for (const auto& clip : track.clips) {
            row.emplace_back(clip.start, clip.start + clip.duration);
        }
    }

    return geometry;
}

RenderClipIndex::RenderClipIndex(const Session& session)
    : RenderClipIndex(captureGeometry(session)) {
}

RenderClipIndex::RenderClipIndex(const Geometry& geometry) {
    trees.resize(geometry.size());

    for (size_t track = 0; track < geometry.size(); ++track) {
        const auto& clips = geometry[track];
        auto& tree = trees[track];
        if (clips.empty()) {
            continue;
        }

        tree.resize(clips.size() * 4);
        const auto build = [&](const auto& self, size_t node, size_t first, size_t last) -> void {
            auto& entry = tree[node];
            if (last - first == 1) {
                entry.start = clips[first].first;
                entry.end = clips[first].second;
                entry.clip = static_cast<int>(first);
                return;
            }

            const auto middle = first + (last - first) / 2;
            self(self, node * 2 + 1, first, middle);
            self(self, node * 2 + 2, middle, last);

            const auto& left = tree[node * 2 + 1];
            const auto& right = tree[node * 2 + 2];
            if (!std::isfinite(left.start) || !std::isfinite(left.end) ||
                !std::isfinite(right.start) || !std::isfinite(right.end)) {
                entry.start = entry.end = std::numeric_limits<double>::quiet_NaN();
            } else {
                entry.start = std::min(left.start, right.start);
                entry.end = std::max(left.end, right.end);
            }
        };
        build(build, 0, 0, clips.size());
    }
}

juce::Range<int> clipRenderWindow(double clipStart, double clipEnd, double blockStart,
                                  double outputRate, int blockSize) noexcept {
    if (blockSize <= 0) {
        return {};
    }
    if (!std::isfinite(clipStart) || !std::isfinite(clipEnd) || !std::isfinite(blockStart) ||
        !std::isfinite(outputRate) || outputRate <= 0) {
        return {0, blockSize};
    }
    if (clipEnd <= clipStart) {
        return {};
    }

    const double margin =
        4.0 + 8.0 * std::numeric_limits<double>::epsilon() * outputRate *
                  (std::abs(clipStart) + std::abs(clipEnd) + std::abs(blockStart));
    if (!std::isfinite(margin) || margin >= blockSize) {
        return {0, blockSize};
    }

    const double first = std::floor((clipStart - blockStart) * outputRate - margin);
    const double last = std::ceil((clipEnd - blockStart) * outputRate + margin);
    const int from = static_cast<int>(std::clamp(first, 0.0, static_cast<double>(blockSize)));
    const int to = static_cast<int>(std::clamp(last, 0.0, static_cast<double>(blockSize)));
    return {from, std::max(from, to)};
}

void Session::render(juce::AudioBuffer<float>& out, int begin, int count, double startTime,
                     double rate, int masterChannels, const ResamplingContext* prepared,
                     TrackRange scope, int onlyClip, const RenderClipIndex* renderIndex,
                     bool* unsupportedRate, bool sourceChannels) const {
    if (!std::isfinite(rate) || rate <= 0 || !std::isfinite(startTime) ||
        out.getNumChannels() == 0) {
        return;
    }

    const bool all = scope.first == -1 && scope.last == -1;
    if (!all && (scope.first < 0 || scope.last < scope.first ||
                 scope.last >= static_cast<int>(tracks.size()))) {
        return;
    }
    if (onlyClip < -1 ||
        (onlyClip >= 0 &&
         (all || scope.first != scope.last ||
          onlyClip >= static_cast<int>(tracks[static_cast<size_t>(scope.first)].clips.size())))) {
        return;
    }

    ResamplingContext temporary;
    if (!prepared) {
        prepareResampling(temporary, rate);
        prepared = &temporary;
    }

    // When the requested start lies on the output sample grid, calculate each
    // timestamp from its absolute frame number. This avoids block-size-dependent
    // rounding differences between full and adjacent range exports.
    const double frame = startTime * rate;
    const double roundedFrame = std::round(frame);
    const bool aligned = std::isfinite(frame) && std::abs(frame - roundedFrame) < 1.0e-7 &&
                         std::abs(roundedFrame) < 9007199254740992.0 - std::max(0, count);
    const double blockStart = aligned ? roundedFrame / rate : startTime;
    const double blockLast = blockStart + static_cast<double>(count > 0 ? count - 1 : 0) / rate;
    const int first = all ? 0 : scope.first;
    const int last = all ? static_cast<int>(tracks.size()) - 1 : scope.last;

    bool anySolo = false;
    for (int i = first; i <= last; ++i) {
        anySolo |= tracks[static_cast<size_t>(i)].solo;
    }

    for (int trackIndex = first; trackIndex <= last; ++trackIndex) {
        const auto& track = tracks[static_cast<size_t>(trackIndex)];
        if (track.muted || (anySolo && !track.solo)) {
            continue;
        }

        const float leftGain = track.gain * (track.pan > 0 ? 1.0f - track.pan : 1.0f);
        const float rightGain = track.gain * (track.pan < 0 ? 1.0f + track.pan : 1.0f);

        const auto renderClip = [&](int clipIndex) {
            const auto& clip = track.clips[static_cast<size_t>(clipIndex)];

            // Most clips in a long arrangement do not intersect this block. Use a
            // deliberately generous margin before exact canonicalization/windowing;
            // it accounts for sample-boundary normalization and floating error at
            // large timeline positions. Non-finite arithmetic falls through.
            if (!clipMayIntersectBlock(clip.start, clip.start + clip.duration, blockStart,
                                       blockLast)) {
                return;
            }

            const auto& src = *clip.audio;
            const int channels = src.numChannels();
            const int samples = src.numSamples();
            if (channels == 0 || samples == 0) {
                return;
            }

            // Project files use decimal seconds. Treat only near-integer project
            // frame boundaries as their exact grid positions after a round trip.
            const double clipStart = canonicalProjectTime(clip.start);
            const double clipEnd = canonicalProjectTime(clip.start + clip.duration);
            const auto window = clipRenderWindow(clipStart, clipEnd, blockStart, rate, count);
            if (window.isEmpty()) {
                return;
            }

            if (unsupportedRate && !SincResampler::hasFullSupport(src.sampleRate, rate)) {
                *unsupportedRate = true;
                return;
            }

            const auto* kernel = prepared->find(clip.audio->sampleRate, rate);
            // Prepared callers must include every source rate before rendering.
            jassert(kernel != nullptr);
            if (!kernel) {
                return;
            }

            const bool sameRate = std::abs(clip.audio->sampleRate - rate) < 1.0e-9;
            for (int i = window.getStart(); i < window.getEnd(); ++i) {
                const double time = aligned ? (roundedFrame + static_cast<double>(i)) / rate
                                            : startTime + static_cast<double>(i) / rate;
                if (time < clipStart || time >= clipEnd) {
                    continue;
                }

                double position = (clip.offset + time - clipStart) * clip.audio->sampleRate;
                const auto nearest = std::round(position);
                if (std::abs(position - nearest) < 1.0e-7) {
                    position = nearest;
                }

                // Check in floating point before any integer conversion. A
                // malformed or extreme-rate source can otherwise overflow an int.
                if (!std::isfinite(position) || position < 0 || position >= samples) {
                    continue;
                }

                const bool exact = sameRate && std::abs(position - nearest) < 1.0e-8 &&
                                   nearest >= 0 && nearest < samples;

                if (sourceChannels) {
                    const float envelope = clipLevel(clip, clip.offset + time - clipStart);
                    for (int ch = 0; ch < std::min(channels, out.getNumChannels()); ++ch) {
                        const auto* data = src.channelData(ch);
                        const float value = exact ? data[static_cast<int>(nearest)]
                                                  : kernel->sample(data, samples, position);
                        out.addSample(ch, begin + i, value * envelope);
                    }
                    continue;
                }

                float left = 0;
                float right = 0;
                if (channels == 2 && !exact) {
                    const auto values = kernel->sampleStereo(src.channelData(0), src.channelData(1),
                                                             samples, position);
                    const auto routeLeft =
                        track.routing.empty() ? defaultChannelRoute(0, channels) : track.routing[0];
                    const auto routeRight = track.routing.size() > 1
                                                ? track.routing[1]
                                                : defaultChannelRoute(1, channels);
                    left += values.first * routeLeft.left;
                    right += values.first * routeLeft.right;
                    left += values.second * routeRight.left;
                    right += values.second * routeRight.right;
                } else {
                    for (int ch = 0; ch < channels; ++ch) {
                        const auto* data = src.channelData(ch);
                        const float value = exact ? data[static_cast<int>(nearest)]
                                                  : kernel->sample(data, samples, position);
                        const auto route = static_cast<size_t>(ch) < track.routing.size()
                                               ? track.routing[static_cast<size_t>(ch)]
                                               : defaultChannelRoute(ch, channels);
                        left += value * route.left;
                        right += value * route.right;
                    }
                }

                const float envelope = clipLevel(clip, clip.offset + time - clipStart);
                left *= leftGain * envelope;
                right *= rightGain * envelope;

                if (masterChannels == 1 || out.getNumChannels() == 1) {
                    out.addSample(0, begin + i, (left + right) * 0.5f);
                    if (masterChannels == 1 && out.getNumChannels() > 1) {
                        out.addSample(1, begin + i, (left + right) * 0.5f);
                    }
                } else {
                    out.addSample(0, begin + i, left);
                    out.addSample(1, begin + i, right);
                }
            }
        };

        if (onlyClip >= 0) {
            renderClip(onlyClip);
        } else if (renderIndex) {
            renderIndex->forEach(trackIndex, blockStart, blockLast, renderClip);
        } else {
            for (int clipIndex = 0; clipIndex < static_cast<int>(track.clips.size()); ++clipIndex) {
                renderClip(clipIndex);
            }
        }
    }
}
namespace {

juce::uint64 peakHierarchyPayloadBytes(int sampleCount, int channelCount,
                                       juce::int64 blockSize) noexcept {
    auto entries = (static_cast<juce::int64>(sampleCount) + blockSize - 1) / blockSize;
    auto total = entries;
    while (entries >= 16) {
        entries /= 16;
        total += entries;
    }
    return static_cast<juce::uint64>(total) * static_cast<juce::uint64>(channelCount) *
           sizeof(juce::Range<float>);
}

bool buildPeakPyramidLevels(const std::vector<juce::Range<float>>& basePeaks,
                            std::vector<std::vector<juce::Range<float>>>& levels,
                            const LoadProgress& progress, double progressValue) {
    levels.reserve(8);
    const auto* previous = &basePeaks;
    while (previous->size() >= 16) {
        auto& level = levels.emplace_back();
        const size_t groups = previous->size() / 16;
        level.reserve(groups);

        for (size_t group = 0; group < groups; ++group) {
            if ((group % 4096) == 0 && progress && !progress(progressValue)) {
                return false;
            }
            auto range = (*previous)[group * 16];
            for (size_t i = 1; i < 16; ++i) {
                range = range.getUnionWith((*previous)[group * 16 + i]);
            }
            level.push_back(range);
        }

        previous = &level;
    }
    return true;
}

// Build the same adaptive min/max hierarchy while the decoder already has
// each source block in memory. This avoids touching the entire mapped PCM a
// second time before the source is published to the editor.
class ImportedPeakBuilder {
  public:
    ImportedPeakBuilder(int samples, int channelCount, size_t maxBytes,
                        std::shared_ptr<WaveformPeakBudget> budget)
        : size(samples), channels(channelCount), cache(std::make_shared<WaveformPeakCache>()) {
        cache->budget = std::move(budget);
        if (cache->budget) {
            cache->reserved = cache->budget->reserveUpTo(std::min<size_t>(
                maxBytes,
                static_cast<size_t>(peakHierarchyPayloadBytes(size, channels, blockSize))));
            maxBytes = cache->reserved;
        }

        while (peakHierarchyPayloadBytes(size, channels, blockSize) > maxBytes &&
               blockSize < size) {
            blockSize *= 2;
        }

        enabled = peakHierarchyPayloadBytes(size, channels, blockSize) <= maxBytes;
        if (cache->budget) {
            const auto needed =
                enabled ? static_cast<size_t>(peakHierarchyPayloadBytes(size, channels, blockSize))
                        : size_t{0};
            cache->budget->release(cache->reserved - needed);
            cache->reserved = needed;
        }

        cache->blockSize = blockSize;
        if (!enabled) {
            return;
        }

        cache->peaks.resize(static_cast<size_t>(channels));
        cache->levels.resize(static_cast<size_t>(channels));
        pending.resize(static_cast<size_t>(channels));
        counts.resize(static_cast<size_t>(channels));
        fed.resize(static_cast<size_t>(channels));
        for (auto& channelPeaks : cache->peaks) {
            channelPeaks.reserve(
                static_cast<size_t>((static_cast<juce::int64>(size) + blockSize - 1) / blockSize));
        }
    }

    void feed(int channel, const float* samples, int count) {
        if (!enabled) {
            return;
        }

        const auto index = static_cast<size_t>(channel);
        while (count > 0) {
            const int amount =
                static_cast<int>(std::min<juce::int64>(count, blockSize - counts[index]));
            const auto part = juce::FloatVectorOperations::findMinAndMax(samples, amount);
            pending[index] = counts[index] == 0 ? part : pending[index].getUnionWith(part);
            counts[index] += amount;
            fed[index] += amount;
            samples += amount;
            count -= amount;
            if (counts[index] == blockSize || fed[index] == size) {
                cache->peaks[index].push_back(pending[index]);
                counts[index] = 0;
            }
        }
    }

    bool finish(Audio& audio, const LoadProgress& progress) {
        if (progress && !progress(0)) {
            return false;
        }

        if (enabled) {
            for (int channel = 0; channel < channels; ++channel) {
                const auto index = static_cast<size_t>(channel);
                if (fed[index] != size || counts[index] != 0) {
                    return false;
                }
                if (!buildPeakPyramidLevels(cache->peaks[index], cache->levels[index], progress,
                                            static_cast<double>(channel) / channels)) {
                    return false;
                }
            }
        }

        if (progress && !progress(1)) {
            return false;
        }

        audio.peakCache = std::move(cache);
        return true;
    }

  private:
    int size;
    int channels;
    juce::int64 blockSize = 256;
    bool enabled = false;
    std::shared_ptr<WaveformPeakCache> cache;
    std::vector<juce::Range<float>> pending;
    std::vector<juce::int64> counts;
    std::vector<juce::int64> fed;
};

bool buildPeaks(Audio& audio, const LoadProgress& progress, size_t maxBytes = 16u * 1024u * 1024u,
                std::shared_ptr<WaveformPeakBudget> budget = {}) {
    const int size = audio.numSamples();
    const int channels = audio.numChannels();
    juce::int64 blockSize = 256;
    std::vector<std::vector<juce::Range<float>>> peaksByChannel;
    std::vector<std::vector<std::vector<juce::Range<float>>>> levelsByChannel;

    // Rebuilds of a managed source remain under the same shared budget.
    if (!budget && audio.peakCache) {
        budget = audio.peakCache->budget;
    }

    auto cache = std::make_shared<WaveformPeakCache>();
    const auto commit = [&] {
        cache->peaks = std::move(peaksByChannel);
        cache->levels = std::move(levelsByChannel);
        cache->blockSize = blockSize;
        audio.peakCache = std::move(cache);
    };

    if (size <= 0 || channels <= 0) {
        if (progress && !progress(1)) {
            return false;
        }
        commit();
        return true;
    }

    if (budget) {
        cache->budget = std::move(budget);
        cache->reserved = cache->budget->reserveUpTo(std::min<size_t>(
            maxBytes, static_cast<size_t>(peakHierarchyPayloadBytes(size, channels, blockSize))));
        maxBytes = cache->reserved;
    }

    while (peakHierarchyPayloadBytes(size, channels, blockSize) > maxBytes && blockSize < size) {
        blockSize *= 2;
    }

    if (cache->budget) {
        const auto needed =
            peakHierarchyPayloadBytes(size, channels, blockSize) <= maxBytes
                ? static_cast<size_t>(peakHierarchyPayloadBytes(size, channels, blockSize))
                : size_t{0};
        cache->budget->release(cache->reserved - needed);
        cache->reserved = needed;
    }

    if (peakHierarchyPayloadBytes(size, channels, blockSize) > maxBytes) {
        if (progress && !progress(1)) {
            return false;
        }
        commit();
        return true;
    }

    peaksByChannel.resize(static_cast<size_t>(channels));
    levelsByChannel.resize(static_cast<size_t>(channels));

    for (int channel = 0; channel < channels; ++channel) {
        auto& peaks = peaksByChannel[static_cast<size_t>(channel)];
        peaks.reserve(
            static_cast<size_t>((static_cast<juce::int64>(size) + blockSize - 1) / blockSize));
        juce::int64 nextCheck = 0;

        for (juce::int64 pos = 0; pos < size; pos += blockSize) {
            const auto blockEnd = std::min<juce::int64>(size, pos + blockSize);
            bool initialized = false;
            juce::Range<float> range;

            for (auto scan = pos; scan < blockEnd;) {
                if (scan >= nextCheck && progress) {
                    if (!progress(
                            (static_cast<double>(channel) + static_cast<double>(scan) / size) /
                            channels)) {
                        return false;
                    }
                    nextCheck = scan + 262144;
                }

                const auto end = std::min<juce::int64>(blockEnd, scan + 262144);
                const auto part =
                    audio.minMax(channel, static_cast<int>(scan), static_cast<int>(end - scan));
                range = initialized ? range.getUnionWith(part) : part;
                initialized = true;
                scan = end;
            }
            peaks.push_back(range);
        }

        if (!buildPeakPyramidLevels(peaks, levelsByChannel[static_cast<size_t>(channel)], progress,
                                    (static_cast<double>(channel) + 1.0) / channels)) {
            return false;
        }
    }

    if (progress && !progress(1)) {
        return false;
    }

    commit();
    return true;
}

} // namespace

bool buildWaveformPeaks(Audio& audio, size_t maxBytes, std::function<bool(double)> progress,
                        std::shared_ptr<WaveformPeakBudget> budget) {
    return buildPeaks(audio, progress, maxBytes, std::move(budget));
}

bool waveformSampleRange(const Clip& clip, double timelineStart, double timelineEnd, int& begin,
                         int& end) noexcept {
    if (!clip.audio || clip.audio->numSamples() <= 0 || !std::isfinite(clip.audio->sampleRate) ||
        clip.audio->sampleRate <= 0 || !std::isfinite(clip.start) || !std::isfinite(clip.offset) ||
        !std::isfinite(clip.duration) || clip.duration <= 0 || !std::isfinite(timelineStart) ||
        !std::isfinite(timelineEnd) || timelineEnd <= timelineStart) {
        return false;
    }

    const double left = std::max(timelineStart, clip.start);
    const double right = std::min(timelineEnd, clip.start + clip.duration);
    if (!(right > left)) {
        return false;
    }

    const auto frames = static_cast<double>(clip.audio->numSamples());
    const double sourceLeft = (clip.offset + (left - clip.start)) * clip.audio->sampleRate;
    const double sourceRight = (clip.offset + (right - clip.start)) * clip.audio->sampleRate;
    if (!std::isfinite(sourceLeft) || !std::isfinite(sourceRight)) {
        return false;
    }

    const auto first = static_cast<int>(std::ceil(std::clamp(sourceLeft, 0.0, frames) - 1.0e-9));
    const auto last = static_cast<int>(std::ceil(std::clamp(sourceRight, 0.0, frames) - 1.0e-9));
    if (first >= last) {
        return false;
    }

    begin = first;
    end = last;
    return true;
}

bool waveformPointRange(const Clip& clip, double timelineStart, double timelineEnd, int& begin,
                        int& end) noexcept {
    if (!clip.audio || clip.audio->numSamples() <= 0 || !std::isfinite(clip.audio->sampleRate) ||
        clip.audio->sampleRate <= 0 || !std::isfinite(clip.start) || !std::isfinite(clip.offset) ||
        clip.offset < 0 || !std::isfinite(clip.duration) || clip.duration <= 0 ||
        !std::isfinite(timelineStart) || !std::isfinite(timelineEnd) ||
        timelineEnd <= timelineStart) {
        return false;
    }

    const double clipEnd = clip.start + clip.duration;
    if (!std::isfinite(clipEnd) || timelineEnd <= clip.start || timelineStart >= clipEnd) {
        return false;
    }

    const double frames = static_cast<double>(clip.audio->numSamples());
    const double rate = clip.audio->sampleRate;
    const double sourceStart = clip.offset * rate;
    const double sourceEnd = (clip.offset + clip.duration) * rate;
    const double visibleStart = (clip.offset + (timelineStart - clip.start)) * rate;
    const double visibleEnd = (clip.offset + (timelineEnd - clip.start)) * rate;
    if (!std::isfinite(sourceStart) || !std::isfinite(sourceEnd) || !std::isfinite(visibleStart) ||
        !std::isfinite(visibleEnd)) {
        return false;
    }

    const auto first = static_cast<int>(std::ceil(std::clamp(sourceStart, 0.0, frames) - 1.0e-9));
    const auto last = static_cast<int>(std::ceil(std::clamp(sourceEnd, 0.0, frames) - 1.0e-9));
    if (last <= first) {
        return false;
    }

    const auto from =
        static_cast<int>(std::max(0.0, std::floor(std::clamp(visibleStart, 0.0, frames)) - 1.0));
    const auto to =
        static_cast<int>(std::min(frames, std::ceil(std::clamp(visibleEnd, 0.0, frames)) + 1.0));
    const int lo = std::clamp(from, first, last);
    const int hi = std::clamp(to, lo, last);
    if (lo >= hi) {
        return false;
    }

    begin = lo;
    end = hi;
    return true;
}

juce::Range<float> waveformRange(const Audio& audio, int channel, int begin, int end) {
    const int size = audio.numSamples();
    if (channel < 0 || channel >= audio.numChannels()) {
        return {};
    }

    begin = juce::jlimit(0, size, begin);
    end = juce::jlimit(begin, size, end);
    if (begin == end) {
        return {};
    }

    const auto* cache = static_cast<size_t>(channel) < audio.peakCache->peaks.size()
                            ? &audio.peakCache->peaks[static_cast<size_t>(channel)]
                            : nullptr;
    const auto blockSize = std::max<juce::int64>(1, audio.peakCache->blockSize);

    bool initialized = false;
    juce::Range<float> result;
    const auto add = [&](juce::Range<float> part) {
        if (!initialized) {
            result = part;
            initialized = true;
        } else {
            result = result.getUnionWith(part);
        }
    };

    int pos = begin;
    const int prefix = static_cast<int>(std::min<juce::int64>(
        end, ((static_cast<juce::int64>(pos) + blockSize - 1) / blockSize) * blockSize));
    if (prefix > pos) {
        add(audio.minMax(channel, pos, prefix - pos));
        pos = prefix;
    }

    while (static_cast<juce::int64>(pos) + blockSize <= end && cache &&
           static_cast<size_t>(pos / blockSize) < cache->size()) {
        const size_t block = static_cast<size_t>(pos / blockSize);
        const size_t remaining = static_cast<size_t>((end - pos) / blockSize);
        size_t factor = 1;
        size_t width = 1;
        const juce::Range<float>* best = &(*cache)[block];

        if (static_cast<size_t>(channel) < audio.peakCache->levels.size()) {
            for (const auto& level : audio.peakCache->levels[static_cast<size_t>(channel)]) {
                if (width > remaining / 16) {
                    break;
                }
                width *= 16;
                if (block % width != 0 || block / width >= level.size()) {
                    break;
                }
                best = &level[block / width];
                factor = width;
            }
        }

        add(*best);
        pos += static_cast<int>(static_cast<juce::int64>(factor) * blockSize);
    }

    if (pos < end) {
        add(audio.minMax(channel, pos, end - pos));
    }

    return result;
}

bool cachedWaveformBounds(const Clip& clip, int channel, int begin, int end,
                          juce::Range<float>& output) {
    if (!clip.audio || channel < 0 || channel >= clip.audio->numChannels() ||
        !std::isfinite(clip.gain)) {
        return false;
    }

    const auto& audio = *clip.audio;
    const auto block = audio.peakCache->blockSize;
    begin = juce::jlimit(0, audio.numSamples(), begin);
    end = juce::jlimit(begin, audio.numSamples(), end);
    if (begin == end || block <= 0 ||
        static_cast<size_t>(channel) >= audio.peakCache->peaks.size()) {
        return false;
    }

    const auto& cache = audio.peakCache->peaks[static_cast<size_t>(channel)];
    const auto required =
        static_cast<size_t>((static_cast<juce::int64>(audio.numSamples()) + block - 1) / block);
    if (cache.size() < required) {
        return false;
    }

    size_t pos = static_cast<size_t>(begin / block);
    const size_t last = static_cast<size_t>((static_cast<juce::int64>(end) + block - 1) / block);
    bool initialized = false;
    juce::Range<float> bounds;

    while (pos < last) {
        size_t factor = 1;
        size_t width = 1;
        const auto* best = &cache[pos];

        if (static_cast<size_t>(channel) < audio.peakCache->levels.size()) {
            for (const auto& level : audio.peakCache->levels[static_cast<size_t>(channel)]) {
                if (width > (last - pos) / 16) {
                    break;
                }
                width *= 16;
                if (pos % width != 0 || pos / width >= level.size()) {
                    break;
                }
                best = &level[pos / width];
                factor = width;
            }
        }

        bounds = initialized ? bounds.getUnionWith(*best) : *best;
        initialized = true;
        pos += factor;
    }

    const float a = bounds.getStart() * clip.gain;
    const float b = bounds.getEnd() * clip.gain;
    output = {std::min({0.0f, a, b}), std::max({0.0f, a, b})};
    return true;
}

juce::Range<float> clipWaveformRange(const Clip& clip, int channel, int begin, int end,
                                     bool* exact) {
    if (exact) {
        *exact = false;
    }
    if (!clip.audio || channel < 0 || channel >= clip.audio->numChannels() ||
        !std::isfinite(clip.audio->sampleRate) || clip.audio->sampleRate <= 0) {
        return {};
    }

    const auto& audio = *clip.audio;
    begin = juce::jlimit(0, audio.numSamples(), begin);
    end = juce::jlimit(begin, audio.numSamples(), end);
    if (begin == end) {
        return {};
    }

    const double first = static_cast<double>(begin) / audio.sampleRate;
    const double last = static_cast<double>(end - 1) / audio.sampleRate;
    float minLevel = std::min(clipLevel(clip, first), clipLevel(clip, last));
    float maxLevel = std::max(clipLevel(clip, first), clipLevel(clip, last));

    for (double time :
         {clip.fadeInStart, clip.fadeInStart + clip.fadeIn, clip.fadeOutEnd - clip.fadeOut,
          clip.fadeOutEnd, (clip.fadeInStart + clip.fadeOutEnd) * 0.5}) {
        if (time > first && time < last) {
            maxLevel = std::max(maxLevel, clipLevel(clip, time));
        }
    }

    if (minLevel >= maxLevel) {
        const auto raw = waveformRange(audio, channel, begin, end);
        if (exact) {
            *exact = true;
        }
        return {raw.getStart() * minLevel, raw.getEnd() * minLevel};
    }

    if (end - begin <= 4096) {
        const auto* data = audio.channelData(channel);
        float low = data[begin] * clipLevel(clip, first);
        float high = low;
        for (int i = begin + 1; i < end; ++i) {
            const float value =
                data[i] * clipLevel(clip, static_cast<double>(i) / audio.sampleRate);
            low = std::min(low, value);
            high = std::max(high, value);
        }
        if (exact) {
            *exact = true;
        }
        return {low, high};
    }

    const auto raw = waveformRange(audio, channel, begin, end);
    const float candidates[]{raw.getStart() * minLevel, raw.getStart() * maxLevel,
                             raw.getEnd() * minLevel, raw.getEnd() * maxLevel};
    return {*std::min_element(std::begin(candidates), std::end(candidates)),
            *std::max_element(std::begin(candidates), std::end(candidates))};
}

bool exactWaveformColumns(const Clip& clip, int channel, double viewStart, double viewSpan,
                          int timelineLeft, int timelineWidth, int firstX, int columns,
                          std::vector<WaveformColumn>& output, std::function<bool()> cancelled,
                          bool includeRms) {
    if (!clip.audio || channel < 0 || channel >= clip.audio->numChannels() ||
        !std::isfinite(clip.audio->sampleRate) || clip.audio->sampleRate <= 0 ||
        !std::isfinite(viewStart) || viewStart < 0 || !std::isfinite(viewSpan) || viewSpan <= 0 ||
        !std::isfinite(viewStart + viewSpan) || timelineWidth <= 0 || columns <= 0 ||
        columns > 8192 || !std::isfinite(clip.start) || !std::isfinite(clip.offset) ||
        !std::isfinite(clip.duration) || clip.duration <= 0 || !std::isfinite(clip.gain) ||
        !std::isfinite(clip.fadeIn) || !std::isfinite(clip.fadeOut) ||
        !std::isfinite(clip.fadeInStart) || !std::isfinite(clip.fadeOutEnd)) {
        return false;
    }

    const auto timeAt = [&](double x) {
        return std::clamp(viewStart + (x - timelineLeft) * viewSpan / timelineWidth, 0.0,
                          viewStart + viewSpan);
    };

    const auto* samples = clip.audio->channelData(channel);
    const double rate = clip.audio->sampleRate;
    const auto block = clip.audio->peakCache->blockSize;
    const bool usablePeaks =
        block > 0 && block <= 262144 &&
        static_cast<size_t>(channel) < clip.audio->peakCache->peaks.size() &&
        clip.audio->peakCache->peaks[static_cast<size_t>(channel)].size() >=
            static_cast<size_t>((static_cast<juce::int64>(clip.audio->numSamples()) + block - 1) /
                                block);

    std::vector<WaveformColumn> result(static_cast<size_t>(columns));
    for (int column = 0; column < columns; ++column) {
        if (cancelled && cancelled()) {
            return false;
        }

        const double x = static_cast<double>(firstX) + column;
        int begin = 0;
        int end = 0;
        if (!waveformSampleRange(clip, timeAt(x), timeAt(x + 1), begin, end)) {
            continue;
        }

        const double first = static_cast<double>(begin) / rate;
        const double last = static_cast<double>(end - 1) / rate;
        const bool flatIn =
            clip.fadeIn <= 0 || last <= clip.fadeInStart || first >= clip.fadeInStart + clip.fadeIn;
        const bool flatOut =
            clip.fadeOut <= 0 || last <= clip.fadeOutEnd - clip.fadeOut || first >= clip.fadeOutEnd;

        if (usablePeaks && flatIn && flatOut) {
            const auto raw = waveformRange(*clip.audio, channel, begin, end);
            const float level = clipLevel(clip, first);
            double squares = 0;

            if (includeRms) {
                for (int base = begin; base < end;) {
                    if (cancelled && cancelled()) {
                        return false;
                    }
                    const int limit = std::min(end, base + std::min(4096, end - base));
                    for (int i = base; i < limit; ++i) {
                        const float value = samples[i] * level;
                        if (!std::isfinite(value)) {
                            return false;
                        }
                        squares += static_cast<double>(value) * static_cast<double>(value);
                    }
                    base = limit;
                }
            }

            const juce::Range<float> scaled =
                level >= 0 ? juce::Range<float>{raw.getStart() * level, raw.getEnd() * level}
                           : juce::Range<float>{raw.getEnd() * level, raw.getStart() * level};
            const float rmsValue =
                includeRms
                    ? static_cast<float>(std::sqrt(squares / static_cast<double>(end - begin)))
                    : 0.0f;
            result[static_cast<size_t>(column)] = {scaled, true, rmsValue, includeRms};
            continue;
        }

        float low = samples[begin] * clipLevel(clip, static_cast<double>(begin) / rate);
        float high = low;
        if (includeRms && !std::isfinite(low)) {
            return false;
        }

        double squares = includeRms ? static_cast<double>(low) * static_cast<double>(low) : 0;
        for (int i = begin + 1; i < end; ++i) {
            if ((i - begin) % 4096 == 0 && cancelled && cancelled()) {
                return false;
            }
            const float value = samples[i] * clipLevel(clip, static_cast<double>(i) / rate);
            low = std::min(low, value);
            high = std::max(high, value);
            if (includeRms) {
                if (!std::isfinite(value)) {
                    return false;
                }
                squares += static_cast<double>(value) * static_cast<double>(value);
            }
        }

        const float rmsValue =
            includeRms ? static_cast<float>(std::sqrt(squares / static_cast<double>(end - begin)))
                       : 0.0f;
        result[static_cast<size_t>(column)] = {{low, high}, true, rmsValue, includeRms};
    }

    if (cancelled && cancelled()) {
        return false;
    }

    output = std::move(result);
    return true;
}
namespace {
#if JUCE_MAC || JUCE_LINUX
// Open without waiting for a FIFO writer, then check the opened object before
// reading. A path may be replaced between its initial stat and open.
class SourceInput final : public juce::InputStream {
  public:
    explicit SourceInput(const juce::File& file) {
        descriptor = ::open(file.getFullPathName().toRawUTF8(),
                            O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
        if (descriptor >= 0) {
            struct stat info {};
            if (::fstat(descriptor, &info) == 0) {
                length = info.st_size;
            } else {
                failed = true;
            }
        }
    }

    ~SourceInput() override {
        if (descriptor >= 0) {
            ::close(descriptor);
        }
    }

    bool opened() const noexcept { return descriptor >= 0 && !failed; }
    int fd() const noexcept { return descriptor; }
    bool hasFailed() const noexcept { return failed; }
    juce::int64 getTotalLength() override { return length; }
    juce::int64 getPosition() override { return position; }

    bool setPosition(juce::int64 target) override {
        if (target < 0 || ::lseek(descriptor, static_cast<off_t>(target), SEEK_SET) < 0) {
            failed = true;
            return false;
        }
        position = target;
        return true;
    }

    bool isExhausted() override { return failed || position >= length; }

    int read(void* destination, int count) override {
        if (failed || count <= 0) {
            return 0;
        }

        ssize_t amount;
        do {
            amount = ::read(descriptor, destination, static_cast<size_t>(count));
        } while (amount < 0 && errno == EINTR);

        if (amount < 0 || (amount == 0 && position < length)) {
            failed = true;
            return 0;
        }

        position += amount;
        return static_cast<int>(amount);
    }

  private:
    int descriptor = -1;
    juce::int64 length = 0;
    juce::int64 position = 0;
    bool failed = false;
};

// JUCE's Vorbis reader assumes every logical link has the first link's
// channels and rate. A later link with fewer channels can dereference an
// absent channel; a different rate would also corrupt timing. Reject such
// containers before any decoder examines them until links are handled safely.
bool validateOggLayout(SourceInput& input, const juce::File& file, juce::String& error,
                       const LoadProgress& progress) {
    unsigned char header[27]{};
    const auto readExact = [&](void* destination, int count) {
        auto* bytes = static_cast<unsigned char*>(destination);
        int done = 0;
        while (done < count) {
            const int amount = input.read(bytes + done, count - done);
            if (amount <= 0) {
                return false;
            }
            done += amount;
        }
        return true;
    };

    if (input.getTotalLength() < 4) {
        return true;
    }
    if (!readExact(header, 4) || !input.setPosition(0)) {
        error = "Could not inspect audio source: " + file.getFileName();
        return false;
    }
    if (std::memcmp(header, "OggS", 4) != 0) {
        return true;
    }

    bool first = true;
    unsigned char serial[4]{};
    size_t page = 0;
    while (input.getPosition() < input.getTotalLength()) {
        if ((page++ % 128) == 0 && progress && !progress(0.20)) {
            error = "Import cancelled.";
            return false;
        }
        if (!readExact(header, 27) || std::memcmp(header, "OggS", 4) != 0 || header[4] != 0 ||
            (header[5] & 0xf8) != 0) {
            error = "Invalid Ogg page in audio source: " + file.getFileName();
            return false;
        }
        if (first) {
            if ((header[5] & 2) == 0) {
                error = "Invalid Ogg stream start: " + file.getFileName();
                return false;
            }
            std::memcpy(serial, header + 14, 4);
            first = false;
        } else if ((header[5] & 2) != 0 || std::memcmp(serial, header + 14, 4) != 0) {
            error = "Ogg files with multiple logical streams are not yet supported: " +
                    file.getFileName();
            return false;
        }

        unsigned char segments[255]{};
        const int count = header[26];
        if (!readExact(segments, count)) {
            error = "Invalid Ogg segment table: " + file.getFileName();
            return false;
        }

        juce::int64 bytes = 0;
        for (int i = 0; i < count; ++i) {
            bytes += segments[i];
        }

        const auto position = input.getPosition();
        if (bytes > input.getTotalLength() - position || !input.setPosition(position + bytes)) {
            error = "Truncated Ogg page: " + file.getFileName();
            return false;
        }
    }

    if (!input.setPosition(0)) {
        error = "Could not inspect audio source: " + file.getFileName();
        return false;
    }

    return true;
}
#endif

class CancellableInput final : public juce::InputStream {
  public:
    CancellableInput(juce::InputStream& source, LoadProgress callback)
        : buffered(source, 65536), progress(std::move(callback)) {}

    juce::int64 getTotalLength() override { return buffered.getTotalLength(); }
    juce::int64 getPosition() override { return buffered.getPosition(); }
    bool setPosition(juce::int64 position) override { return buffered.setPosition(position); }
    bool isExhausted() override { return cancelled || buffered.isExhausted(); }

    int read(void* destination, int count) override {
        if (cancelled) {
            return 0;
        }

        if (sinceCheck >= 65536) {
            sinceCheck = 0;
            const auto length = getTotalLength();
            const double fraction = length > 0 ? std::min(1.0, static_cast<double>(getPosition()) /
                                                                   static_cast<double>(length))
                                               : 0.0;
            if (progress && !progress(fraction)) {
                cancelled = true;
                return 0;
            }
        }

        const int readCount = buffered.read(destination, count);
        sinceCheck += readCount;
        return readCount;
    }

    bool wasCancelled() const noexcept { return cancelled; }

  private:
    juce::BufferedInputStream buffered;
    LoadProgress progress;
    int sinceCheck = 65536;
    bool cancelled = false;
};

} // namespace

bool hashSourceFile(const juce::File& file, juce::String& hash, juce::String& error,
                    LoadProgress progress) {
    const FileObservation observation(file);
    if (!observation.isValid()) {
        error = "Could not read audio source: " + file.getFileName();
        return false;
    }

#if JUCE_MAC || JUCE_LINUX
    if (!observation.isRegularFile()) {
        error = "Audio source is not a regular file: " + file.getFileName();
        return false;
    }

    SourceInput source(file);
    if (!source.opened()) {
        error = "Could not read audio source: " + file.getFileName();
        return false;
    }
    if (!observation.matchesDescriptor(source.fd())) {
        error = "Audio source changed while being checked: " + file.getFileName();
        return false;
    }

    CancellableInput buffered(source, progress);
#else
    auto input = file.createInputStream();
    if (!input || input->getStatus().failed()) {
        error = "Could not read audio source: " + file.getFileName();
        return false;
    }

    CancellableInput buffered(*input, progress);
#endif

    const juce::SHA256 digest(buffered);
    if (buffered.wasCancelled() || (progress && !progress(1))) {
        error = "Import cancelled.";
        return false;
    }

#if JUCE_MAC || JUCE_LINUX
    if (source.hasFailed()) {
        error = "Could not read audio source: " + file.getFileName();
        return false;
    }
    if (!observation.matchesDescriptor(source.fd())) {
        error = "Audio source changed while being checked: " + file.getFileName();
        return false;
    }
#else
    if (input->getStatus().failed()) {
        error = "Could not read audio source: " + file.getFileName();
        return false;
    }
#endif

    if (!observation.matches(file)) {
        error = "Audio source changed while being checked: " + file.getFileName();
        return false;
    }

    hash = digest.toHexString();
    return true;
}

bool verifyTrackSources(const std::vector<TrackSource>& sources, std::vector<SourceCheck>& result,
                        juce::String& error, LoadProgress progress) {
    error.clear();
    std::vector<SourceCheck> checks;
    checks.reserve(sources.size());

    const auto report = [&](double value) { return !progress || progress(value); };
    if (!report(0)) {
        error = "Source check cancelled.";
        return false;
    }

    for (size_t i = 0; i < sources.size(); ++i) {
        const auto& audio = sources[i].audio;
        SourceCheck check;

        if (!audio) {
            check = {SourceStatus::unreadable, "Missing source information"};
        } else if (!audio->file.existsAsFile()) {
            check = {SourceStatus::missing, "File missing or inaccessible"};
        } else if (audio->sourceHash.length() != 64 ||
                   !audio->sourceHash.containsOnly("0123456789abcdef")) {
            check = {SourceStatus::unverifiable, "No imported checksum"};
        } else {
            const auto size = audio->file.getSize();
            const auto modified = audio->file.getLastModificationTime();
            juce::String hash;
            juce::String hashError;
            const auto advance = [&](double fraction) {
                return report((static_cast<double>(i) + fraction) /
                              static_cast<double>(sources.size()));
            };

            if (!hashSourceFile(audio->file, hash, hashError, advance)) {
                if (hashError == "Import cancelled.") {
                    error = "Source check cancelled.";
                    return false;
                }
                check =
                    hashError.startsWith("Audio source changed while being checked:")
                        ? SourceCheck{SourceStatus::changedDuringCheck, "File changed during check"}
                        : SourceCheck{SourceStatus::unreadable, hashError};
            } else if (!audio->file.existsAsFile() || audio->file.getSize() != size ||
                       audio->file.getLastModificationTime() != modified) {
                check = {SourceStatus::changedDuringCheck, "File changed during check"};
            } else if (hash != audio->sourceHash) {
                check = {SourceStatus::changed, "Content changed since import"};
            } else {
                check = {SourceStatus::matches, "Matches imported checksum"};
            }
        }

        checks.push_back(std::move(check));
        if (!report(static_cast<double>(i + 1) / static_cast<double>(sources.size()))) {
            error = "Source check cancelled.";
            return false;
        }
    }

    if (sources.empty() && !report(1)) {
        error = "Source check cancelled.";
        return false;
    }

    result = std::move(checks);
    return true;
}

std::vector<TrackSource> sessionSources(const Session& session) {
    std::vector<TrackSource> sources;
    std::map<const Audio*, size_t, std::less<const Audio*>> positions;

    for (const auto& track : session.tracks) {
        for (const auto& clip : track.clips) {
            if (!clip.audio) {
                continue;
            }
            const auto inserted = positions.emplace(clip.audio.get(), sources.size());
            if (inserted.second) {
                sources.push_back({clip.audio, 1});
            } else {
                ++sources[inserted.first->second].clipCount;
            }
        }
    }

    return sources;
}

bool SourceVerificationCache::record(const std::vector<TrackSource>& sources,
                                     const std::vector<SourceCheck>& checks,
                                     const juce::String& time) {
    if (sources.size() != checks.size()) {
        return false;
    }
    for (const auto& source : sources) {
        if (!source.audio) {
            return false;
        }
    }

    prune();
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto& audio = sources[i].audio;
        entries[audio.get()] = {audio, audio->file, audio->sourceHash, time, checks[i]};
    }

    return true;
}

const SourceCheck* SourceVerificationCache::lookup(const std::shared_ptr<Audio>& audio,
                                                   juce::String* checkedAt) const noexcept {
    if (!audio) {
        return nullptr;
    }

    const auto found = entries.find(audio.get());
    if (found == entries.end() || found->second.audio.lock() != audio ||
        found->second.path != audio->file || found->second.hash != audio->sourceHash) {
        return nullptr;
    }

    if (checkedAt) {
        *checkedAt = found->second.checkedAt;
    }
    return &found->second.check;
}

bool SourceVerificationCache::trackHasIssue(const Track& track) const noexcept {
    if (entries.empty()) {
        return false;
    }

    for (const auto& clip : track.clips) {
        const auto* checked = lookup(clip.audio);
        if (checked && checked->status != SourceStatus::matches) {
            return true;
        }
    }

    return false;
}

void SourceVerificationCache::prune() noexcept {
    for (auto it = entries.begin(); it != entries.end();) {
        if (it->second.audio.expired()) {
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
}

bool parseWavSourceCues(const juce::StringPairArray& values, juce::int64 sourceSamples,
                        std::vector<SourceCue>& result, juce::String& error) {
    error.clear();

    const auto readUnsigned = [&](const juce::String& key, juce::uint64& output,
                                  bool optional = false) {
        if (!values.containsKey(key)) {
            if (optional) {
                output = 0;
                return true;
            }
            return false;
        }

        const auto text = values[key].toStdString();
        if (text.empty()) {
            return false;
        }

        juce::uint64 parsed = 0;
        const auto parsedResult = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (parsedResult.ec != std::errc{} || parsedResult.ptr != text.data() + text.size()) {
            return false;
        }

        output = parsed;
        return true;
    };

    juce::uint64 count = 0;
    juce::uint64 labelCount = 0;
    juce::uint64 regions = 0;
    juce::uint64 notes = 0;
    if (sourceSamples <= 0 || !readUnsigned("NumCuePoints", count, true) ||
        !readUnsigned("NumCueLabels", labelCount, true) ||
        !readUnsigned("NumCueRegions", regions, true) ||
        !readUnsigned("NumCueNotes", notes, true) || count > 10000 || labelCount > 10000) {
        error = "Invalid or excessive WAV cue metadata.";
        return false;
    }
    if (regions || notes) {
        error = "WAV cue regions or notes are not supported for marker import.";
        return false;
    }

    std::map<juce::uint64, juce::String> labels;
    size_t labelBytes = 0;
    for (juce::uint64 i = 0; i < labelCount; ++i) {
        const auto prefix = "CueLabel" + juce::String(static_cast<juce::int64>(i));
        juce::uint64 identifier = 0;
        if (!readUnsigned(prefix + "Identifier", identifier) ||
            identifier > std::numeric_limits<std::uint32_t>::max() ||
            !values.containsKey(prefix + "Text")) {
            error = "Invalid WAV cue label.";
            return false;
        }

        auto name = values[prefix + "Text"];
        const auto size = name.getNumBytesAsUTF8();
        if (size > 8u * 1024u * 1024u - labelBytes ||
            !labels.emplace(identifier, std::move(name)).second) {
            error = "Duplicate or excessive WAV cue labels.";
            return false;
        }
        labelBytes += size;
    }

    std::set<juce::uint64> identifiers;
    std::vector<SourceCue> next;
    next.reserve(static_cast<size_t>(count));

    for (juce::uint64 i = 0; i < count; ++i) {
        const auto prefix = "Cue" + juce::String(static_cast<juce::int64>(i));
        juce::uint64 identifier = 0;
        juce::uint64 offset = 0;
        juce::uint64 chunk = 0;
        juce::uint64 chunkStart = 0;
        juce::uint64 blockStart = 0;
        if (!readUnsigned(prefix + "Identifier", identifier) ||
            !readUnsigned(prefix + "Offset", offset) || !readUnsigned(prefix + "ChunkID", chunk) ||
            !readUnsigned(prefix + "ChunkStart", chunkStart) ||
            !readUnsigned(prefix + "BlockStart", blockStart) ||
            identifier > std::numeric_limits<std::uint32_t>::max() ||
            chunk != juce::ByteOrder::littleEndianInt("data") || chunkStart != 0 ||
            blockStart != 0 || offset >= static_cast<juce::uint64>(sourceSamples) ||
            !identifiers.insert(identifier).second) {
            error = "Invalid or unsupported WAV cue point.";
            return false;
        }

        const auto found = labels.find(identifier);
        next.push_back({static_cast<juce::int64>(offset),
                        found == labels.end() ? juce::String() : found->second});
    }

    for (const auto& label : labels) {
        if (!identifiers.count(label.first)) {
            error = "WAV cue label has no matching cue point.";
            return false;
        }
    }

    std::sort(next.begin(), next.end(),
              [](const auto& a, const auto& b) { return a.sample < b.sample; });
    result = std::move(next);
    return true;
}

std::shared_ptr<Audio> loadAudio(juce::AudioFormatManager& formats, const juce::File& file,
                                 juce::String& error, LoadProgress progress,
                                 juce::int64 memoryThresholdBytes,
                                 const juce::File& temporaryDirectory,
                                 std::shared_ptr<WaveformPeakBudget> peakBudget) {
    const auto keepGoing = [&](double value) { return !progress || progress(value); };

#if CARTA_STUDIO_MINIMP3
    registerNativeMp3Format(formats);
#endif
    if (!keepGoing(0)) {
        error = "Import cancelled.";
        return {};
    }

    // 1. Observe source metadata and compute initial pre-decode SHA-256.
    const FileObservation initial(file);
    if (!initial.isValid()) {
        error = "Could not read audio source: " + file.getFileName();
        return {};
    }

    juce::String initialHash;
    if (!hashSourceFile(file, initialHash, error,
                        [&](double value) { return keepGoing(value * 0.20); })) {
        if (error.startsWith("Audio source changed while being checked:")) {
            error = "Audio source changed while it was being imported: " + file.getFileName();
        }
        return {};
    }
    if (!keepGoing(0.20)) {
        error = "Import cancelled.";
        return {};
    }

    // 2. Open descriptor-verified input and validate container / MP3 gapless layout.
#if JUCE_MAC || JUCE_LINUX
    auto sourceStream = std::make_unique<SourceInput>(file);
    if (!sourceStream->opened()) {
        error = "Could not read audio source: " + file.getFileName();
        return {};
    }
    if (!initial.isRegularFile() || !initial.matchesDescriptor(sourceStream->fd())) {
        error = "Audio source changed while it was being imported: " + file.getFileName();
        return {};
    }
    if (!validateOggLayout(*sourceStream, file, error, progress)) {
        return {};
    }

#if JUCE_MAC || CARTA_STUDIO_MINIMP3
    // Validate supported MP3 timing with cancellation before decoding.
    Mp3GaplessInfo mp3Timing;
    juce::String mp3Error;
    bool recognizedMp3 = false;
    const bool hasMp3Timing = inspectMp3Gapless(
        *sourceStream, mp3Timing, mp3Error, [&] { return keepGoing(0.20); }, &recognizedMp3);
    if (mp3Error == "Import cancelled.") {
        error = mp3Error;
        return {};
    }
    if (sourceStream->hasFailed()) {
        error = "Could not inspect audio source: " + file.getFileName();
        return {};
    }
#endif

    auto* decoderSource = sourceStream.get();
    std::unique_ptr<juce::AudioFormatReader> reader(
        formats.createReaderFor(std::move(sourceStream)));
#else
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
#endif

    if (!reader || !std::isfinite(reader->sampleRate) || reader->sampleRate <= 0 ||
        reader->numChannels == 0 || reader->lengthInSamples <= 0 ||
        reader->lengthInSamples > 0x7fffffff) {
#if JUCE_LINUX && CARTA_STUDIO_MINIMP3
        if (!reader && file.hasFileExtension("mp3") && !hasMp3Timing) {
            error = "Cannot import MP3: " + mp3Error;
            return {};
        }
#endif
        error = "Unsupported or empty audio file: " + file.getFileName();
        return {};
    }
    if (reader->numChannels > 1024) {
        error = "Too many source channels: " + file.getFileName();
        return {};
    }

#if JUCE_MAC
    if (recognizedMp3 && !hasMp3Timing && reader->getFormatName() == "CoreAudio supported file") {
        error = "Cannot import MP3: " + mp3Error;
        return {};
    }
#endif

    juce::int64 firstSourceSample = 0;
    juce::int64 sourceSamples = reader->lengthInSamples;
#if JUCE_MAC
    if (hasMp3Timing && reader->getFormatName() == "CoreAudio supported file" &&
        reader->numChannels == static_cast<unsigned int>(mp3Timing.channels) &&
        std::abs(reader->sampleRate - static_cast<double>(mp3Timing.sampleRate)) < 0.5 &&
        reader->lengthInSamples == mp3Timing.rawFrames) {
        // CoreAudio's output has already compensated its synthesis delay; the
        // remaining leading samples are the encoder delay from the tag.
        firstSourceSample = mp3Timing.encoderDelay;
        sourceSamples = mp3Timing.sampleCount;
    }
#endif

    // 3. Allocate in-memory or self-cleaning sparse memory-mapped PCM storage.
    auto audio = std::make_shared<Audio>();
    audio->file = file;
    audio->sampleRate = reader->sampleRate;
    audio->sourceFormat = reader->getFormatName();
    audio->sourceBitsPerSample = reader->bitsPerSample;
    if (audio->sourceFormat == "WAV file") {
        parseWavSourceCues(reader->metadataValues, reader->lengthInSamples, audio->sourceCues,
                           audio->sourceCueError);
    }

    const int channels = static_cast<int>(reader->numChannels);
    const int size = static_cast<int>(sourceSamples);
    const auto bytes =
        static_cast<juce::int64>(channels) * size * static_cast<juce::int64>(sizeof(float));
    const bool diskBacked = bytes > memoryThresholdBytes;

    std::shared_ptr<MappedPcm> storage;
    std::unique_ptr<juce::FileOutputStream> output;
    juce::AudioBuffer<float> scratch;
    std::unique_ptr<ImportedPeakBuilder> importedPeaks;

    if (diskBacked) {
        if (temporaryDirectory != juce::File{} && !temporaryDirectory.isDirectory()) {
            error = "Temporary audio directory does not exist.";
            return {};
        }

        storage = std::make_shared<MappedPcm>(temporaryDirectory);
        storage->channels = channels;
        storage->frames = size;
        output = storage->temporary.getFile().createOutputStream();
        if (!output || output->getStatus().failed()) {
            error = "Could not create temporary audio storage.";
            return {};
        }

#if JUCE_MAC || JUCE_LINUX
        // Size and map the sparse file before decoding, then unlink its name.
        // The open writer and shared mapping remain usable, and the OS reclaims
        // the unlinked storage even if the process exits during decoding.
        if (static_cast<juce::uint64>(bytes) > std::numeric_limits<size_t>::max() ||
            !output->setPosition(bytes - 1) || !output->writeByte(0)) {
            error = "Could not size temporary audio storage.";
            return {};
        }
        output->flush();
        if (output->getStatus().failed() || storage->temporary.getFile().getSize() != bytes) {
            error = "Could not size temporary audio storage (check available disk space).";
            return {};
        }
        storage->mapping = std::make_unique<juce::MemoryMappedFile>(
            storage->temporary.getFile(), juce::MemoryMappedFile::readOnly);
        if (!storage->mapping->getData() ||
            storage->mapping->getSize() != static_cast<size_t>(bytes)) {
            error = "Could not map temporary audio storage.";
            return {};
        }
        if (!storage->temporary.getFile().deleteFile()) {
            error = "Could not make temporary audio storage self-cleaning.";
            return {};
        }
#endif

        scratch.setSize(channels, std::min(65536, size));
        importedPeaks = std::make_unique<ImportedPeakBuilder>(size, channels, 16u * 1024u * 1024u,
                                                              peakBudget ? std::move(peakBudget)
                                                                         : defaultPeakBudget());
    } else {
        audio->samples.setSize(channels, size);
    }

    // 4. Decode source audio in 64K-sample chunks and verify sample finiteness.
    std::vector<float*> pointers(static_cast<size_t>(channels));
    for (juce::int64 start = 0; start < size; start += 65536) {
        if (!keepGoing(0.20 + 0.35 * static_cast<double>(start) / size)) {
            error = "Import cancelled.";
            return {};
        }

        const int count = static_cast<int>(std::min<juce::int64>(65536, size - start));
        for (int ch = 0; ch < channels; ++ch) {
            pointers[static_cast<size_t>(ch)] =
                diskBacked ? scratch.getWritePointer(ch)
                           : audio->samples.getWritePointer(ch, static_cast<int>(start));
        }

        if (!reader->read(pointers.data(), channels, start + firstSourceSample, count)) {
            error = "Could not read audio: " + file.getFileName();
            return {};
        }

        for (int ch = 0; ch < channels; ++ch) {
            const auto* data = pointers[static_cast<size_t>(ch)];
            for (int i = 0; i < count; ++i) {
                if (!std::isfinite(data[i])) {
                    error = "Audio contains a non-finite sample: " + file.getFileName();
                    return {};
                }
            }

            if (diskBacked && (!output->setPosition((static_cast<juce::int64>(ch) * size + start) *
                                                    static_cast<juce::int64>(sizeof(float))) ||
                               !output->write(data, static_cast<size_t>(count) * sizeof(float)))) {
                error = "Could not write temporary audio storage (check available disk space).";
                return {};
            }
            if (diskBacked) {
                importedPeaks->feed(ch, data, count);
            }
        }
    }

#if JUCE_MAC || JUCE_LINUX
    if (decoderSource->hasFailed() || !initial.matchesDescriptor(decoderSource->fd())) {
        error = "Audio source changed or could not be read while it was being imported: " +
                file.getFileName();
        return {};
    }
#endif

    if (diskBacked) {
        output->flush();
        if (output->getStatus().failed()) {
            error = "Could not write temporary audio storage (check available disk space).";
            return {};
        }
        output.reset();

#if !(JUCE_MAC || JUCE_LINUX)
        if (storage->temporary.getFile().getSize() != bytes ||
            static_cast<juce::uint64>(bytes) > std::numeric_limits<size_t>::max()) {
            error = "Temporary audio storage is incomplete or too large to map.";
            return {};
        }
        storage->mapping = std::make_unique<juce::MemoryMappedFile>(
            storage->temporary.getFile(), juce::MemoryMappedFile::readOnly);
        if (!storage->mapping->getData() ||
            storage->mapping->getSize() != static_cast<size_t>(bytes)) {
            error = "Could not map temporary audio storage.";
            return {};
        }
#endif
        audio->mapped = std::move(storage);
    }

    // 5. Re-hash to detect concurrent modification and finalize waveform peak cache.
    if (!hashSourceFile(file, audio->sourceHash, error,
                        [&](double value) { return keepGoing(0.55 + value * 0.30); })) {
        if (error.startsWith("Audio source changed while being checked:")) {
            error = "Audio source changed while it was being imported: " + file.getFileName();
        }
        return {};
    }

    const bool sourceChanged = audio->sourceHash != initialHash || !initial.matches(file);
    if (sourceChanged) {
        error = "Audio source changed while it was being imported: " + file.getFileName();
        return {};
    }

    const auto peakProgress = [&](double value) {
        return keepGoing((0.55 + 0.30) + (1.0 - (0.55 + 0.30)) * value);
    };
    const bool peaksReady =
        diskBacked ? importedPeaks->finish(*audio, peakProgress)
                   : buildPeaks(*audio, peakProgress, 16u * 1024u * 1024u,
                                peakBudget ? std::move(peakBudget) : defaultPeakBudget());
    if (!peaksReady) {
        error = "Import cancelled.";
        return {};
    }

    return audio;
}

std::shared_ptr<Audio> loadVerifiedReplacement(juce::AudioFormatManager& formats,
                                               const std::shared_ptr<Audio>& original,
                                               const juce::File& file, juce::String& error,
                                               LoadProgress progress) {
    error.clear();
    if (!original || original->sourceHash.length() != 64 ||
        !original->sourceHash.containsOnly("0123456789abcdef") || original->numChannels() <= 0 ||
        original->numSamples() <= 0 || !std::isfinite(original->sampleRate) ||
        original->sampleRate <= 0) {
        error = "The original source has no verifiable identity.";
        return {};
    }

    double lastProgress = 0;
    const auto keepGoing = [&](double value) {
        value = std::clamp(value, lastProgress, 1.0);
        lastProgress = value;
        if (!progress || progress(value)) {
            return true;
        }
        error = "Source relink cancelled.";
        return false;
    };

    if (!keepGoing(0)) {
        return {};
    }

    juce::String hash;
    juce::String hashError;
    if (!hashSourceFile(file, hash, hashError, [&](double p) { return keepGoing(p * 0.20); })) {
        if (error.isEmpty()) {
            error = hashError;
        }
        return {};
    }

    if (hash != original->sourceHash) {
        error = "Replacement file checksum does not match the imported source.";
        return {};
    }

    auto replacement =
        loadAudio(formats, file, error, [&](double p) { return keepGoing(0.20 + 0.65 * p); });
    if (!replacement) {
        if (error == "Import cancelled.") {
            error = "Source relink cancelled.";
        }
        return {};
    }

    if (replacement->sourceHash != original->sourceHash ||
        replacement->numChannels() != original->numChannels() ||
        replacement->numSamples() != original->numSamples() ||
        std::memcmp(&replacement->sampleRate, &original->sampleRate, sizeof(double)) != 0) {
        error = "Replacement file changed or decodes to a different audio layout.";
        return {};
    }

    const auto total = static_cast<juce::int64>(original->numSamples()) * original->numChannels();
    juce::int64 checked = 0;
    for (int ch = 0; ch < original->numChannels(); ++ch) {
        for (juce::int64 offset = 0; offset < original->numSamples(); offset += 65536) {
            if (!keepGoing(0.85 +
                           0.15 * static_cast<double>(checked) / static_cast<double>(total))) {
                return {};
            }

            const auto count = std::min<juce::int64>(65536, original->numSamples() - offset);
            if (std::memcmp(original->channelData(ch) + offset,
                            replacement->channelData(ch) + offset,
                            static_cast<size_t>(count) * sizeof(float)) != 0) {
                error = "Replacement decodes to different audio samples.";
                return {};
            }
            checked += count;
        }
    }

    if (!keepGoing(1)) {
        return {};
    }

    return replacement;
}

} // namespace carta
