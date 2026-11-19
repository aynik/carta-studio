/**
 * Real-time playback transport, peak metering, and audition preview preparation.
 *
 * Separates off-thread audition preparation (`prepare*`) from lock-free,
 * allocation-free callback rendering (`renderPlayback`, `renderDiscPreview`,
 * `OutputPeakHold::observe`).
 */
#include "Audio/Transport.h"
#include "Editing/Editing.h"
#include "IO/SessionIO.h"
#include <map>
#include <algorithm>
#include <cmath>
#include <limits>

namespace carta {
namespace {

bool sameDouble(double a, double b) noexcept {
    return std::equal_to<double>{}(a, b);
}

bool renderDiscSamples(const Session& session, const DiscPreviewPlan& plan,
                       juce::AudioBuffer<float>& buffer, int destination, juce::int64 start,
                       int count, int channels, const ResamplingContext& context,
                       bool* unsupportedRate) {
    int written = 0;
    while (written < count) {
        const auto logical = start + written;
        const auto found = std::upper_bound(
            plan.spans.begin(), plan.spans.end(), logical,
            [](juce::int64 position, const auto& span) { return position < span.logicalEnd; });
        if (found == plan.spans.end() || logical < found->logicalStart) {
            return false;
        }

        const int n =
            static_cast<int>(std::min<juce::int64>(count - written, found->logicalEnd - logical));
        const double sourceTime =
            static_cast<double>(found->sourceStart + logical - found->logicalStart) / sessionRate;
        session.render(buffer, destination + written, n, sourceTime, sessionRate, channels,
                       &context, {}, -1, nullptr, unsupportedRate);
        if (unsupportedRate && *unsupportedRate) {
            return false;
        }
        written += n;
    }

    return true;
}

bool renderLoopedPlayback(const Session& session, juce::AudioBuffer<float>& output, int begin,
                          int count, double& position, double rate, int channels,
                          const PlaybackRange& range, const ResamplingContext* prepared,
                          PlaybackClock* clock, const RenderClipIndex* index,
                          bool* unsupportedRate) {
    const double startFrame = range.start * sessionRate;
    const double endFrame = range.end * sessionRate;
    const double roundedStart = std::round(startFrame);
    const double roundedEnd = std::round(endFrame);
    const bool aligned = std::abs(startFrame - roundedStart) < 1e-7 &&
                         std::abs(endFrame - roundedEnd) < 1e-7 && roundedEnd > roundedStart;
    const double duration =
        aligned ? (roundedEnd - roundedStart) / sessionRate : range.end - range.start;
    if (!(duration > 0) || !std::isfinite(duration)) {
        clock->reset();
        return false;
    }

    if (!clock->valid || !clock->loop || !sameDouble(position, clock->lastPosition) ||
        !sameDouble(range.start, clock->start) || !sameDouble(range.end, clock->end) ||
        !sameDouble(rate, clock->rate)) {
        clock->phase = std::max(0.0, position - range.start);
        if (clock->phase >= duration) {
            clock->phase = std::fmod(clock->phase, duration);
        }
        clock->start = range.start;
        clock->end = range.end;
        clock->rate = rate;
        clock->valid = true;
        clock->loop = true;
    }

    int rendered = 0;
    while (rendered < count) {
        const double frames = (duration - clock->phase) * rate;
        const double tolerance = std::min(1e-9, duration * rate * 0.25);
        const int n = static_cast<int>(std::min(static_cast<double>(count - rendered),
                                                std::max(1.0, std::ceil(frames - tolerance))));
        session.render(output, begin + rendered, n, range.start + clock->phase, rate, channels,
                       prepared, range.tracks, range.onlyClip, index, unsupportedRate);
        if (unsupportedRate && *unsupportedRate) {
            clock->reset();
            return false;
        }

        clock->phase += static_cast<double>(n) / rate;
        if ((clock->phase - duration) * rate >= -tolerance) {
            if (clock->phase < duration) {
                clock->phase = duration;
            }
            clock->phase = std::fmod(clock->phase, duration);
        }
        rendered += n;
    }

    position = range.start + clock->phase;
    clock->lastPosition = position;
    return true;
}

bool renderLinearPlayback(const Session& session, juce::AudioBuffer<float>& output, int begin,
                          int count, double& position, double rate, int channels,
                          const PlaybackRange& range, const ResamplingContext* prepared,
                          PlaybackClock* clock, const RenderClipIndex* index,
                          bool* unsupportedRate) {
    if (position < range.start) {
        position = range.start;
    }

    if (!clock->valid || clock->loop || !sameDouble(position, clock->lastPosition) ||
        !sameDouble(range.start, clock->start) || !sameDouble(range.end, clock->end) ||
        !sameDouble(rate, clock->rate)) {
        clock->origin = position;
        clock->frames = 0;
        clock->start = range.start;
        clock->end = range.end;
        clock->rate = rate;
        clock->loop = false;
        clock->valid = true;
    }

    const double originFrame = clock->origin * sessionRate;
    const double endFrame = range.end * sessionRate;
    const double roundedOrigin = std::round(originFrame);
    const double roundedEnd = std::round(endFrame);
    const bool aligned =
        std::abs(originFrame - roundedOrigin) < 1e-7 && std::abs(endFrame - roundedEnd) < 1e-7;
    const double duration =
        aligned ? (roundedEnd - roundedOrigin) / sessionRate : range.end - clock->origin;
    const double tolerance = std::min(1e-7, std::max(0.0, duration * rate * 0.25));
    const double remaining = duration * rate - static_cast<double>(clock->frames);
    if (remaining <= tolerance) {
        position = range.end;
        clock->reset();
        return false;
    }

    const auto available = std::max(1.0, std::ceil(remaining - tolerance));
    const int n = static_cast<int>(std::min(static_cast<double>(std::max(0, count)), available));
    session.render(output, begin, n, clock->origin + static_cast<double>(clock->frames) / rate,
                   rate, channels, prepared, range.tracks, range.onlyClip, index, unsupportedRate);
    if (unsupportedRate && *unsupportedRate) {
        clock->reset();
        return false;
    }

    clock->frames += n;
    if (duration * rate - static_cast<double>(clock->frames) <= tolerance) {
        position = range.end;
        clock->reset();
        return false;
    }

    position = clock->origin + static_cast<double>(clock->frames) / rate;
    clock->lastPosition = position;
    return true;
}

} // namespace

PlaybackRange fullMasterPlaybackRange(const Session& session) noexcept {
    return {0, static_cast<double>(projectEndSample(session)) / sessionRate, false};
}

bool outputSupportsMaster(int activeChannels, int masterChannels) noexcept {
    return (masterChannels == 1 && activeChannels >= 1) ||
           (masterChannels == 2 && activeChannels >= 2);
}

bool containsNonFiniteAudio(const juce::AudioBuffer<float>& buffer, int start, int count) noexcept {
    if (start < 0 || count < 0 || start > buffer.getNumSamples() ||
        count > buffer.getNumSamples() - start) {
        return true;
    }

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel) {
        const auto* data = buffer.getReadPointer(channel, start);
        for (int i = 0; i < count; ++i) {
            if (!std::isfinite(data[i])) {
                return true;
            }
        }
    }

    return false;
}

bool seekPlayhead(double& position, juce::int64 projectEnd, PlayheadSeek command) noexcept {
    if (!std::isfinite(position) || projectEnd <= 0 || projectEnd >= 9007199254740992LL) {
        return false;
    }

    const double frame = position * sessionRate;
    if (!std::isfinite(frame)) {
        return false;
    }

    auto sample = static_cast<juce::int64>(
        std::llround(std::clamp(frame, 0.0, static_cast<double>(projectEnd))));
    switch (command) {
    case PlayheadSeek::previousSample:
        if (sample > 0) {
            --sample;
        }
        break;
    case PlayheadSeek::nextSample:
        if (sample < projectEnd) {
            ++sample;
        }
        break;
    case PlayheadSeek::start:
        sample = 0;
        break;
    case PlayheadSeek::end:
        sample = projectEnd;
        break;
    default:
        return false;
    }

    position = static_cast<double>(sample) / sessionRate;
    return true;
}

void OutputPeakHold::observe(const juce::AudioBuffer<float>& buffer, int start,
                             int count) noexcept {
    if (start < 0 || count <= 0 || start > buffer.getNumSamples() ||
        count > buffer.getNumSamples() - start || buffer.getNumChannels() <= 0) {
        return;
    }

    const auto hold = [](std::atomic<float>& target, float peak) {
        if (std::isnan(peak) || peak < 0) {
            return;
        }
        auto old = target.load(std::memory_order_relaxed);
        while (old < peak && !target.compare_exchange_weak(old, peak, std::memory_order_relaxed)) {}
    };

    const float l = buffer.getMagnitude(0, start, count);
    hold(left, l);
    hold(recentLeft, l);

    if (buffer.getNumChannels() > 1) {
        const float r = buffer.getMagnitude(1, start, count);
        hold(right, r);
        hold(recentRight, r);
    }

    const int activeChannels = std::min(2, buffer.getNumChannels());
    channels.store(activeChannels, std::memory_order_relaxed);
    recentChannels.store(activeChannels, std::memory_order_relaxed);
}

void OutputPeakHold::reset() noexcept {
    left.store(0, std::memory_order_relaxed);
    right.store(0, std::memory_order_relaxed);
    channels.store(0, std::memory_order_relaxed);
    recentLeft.store(0, std::memory_order_relaxed);
    recentRight.store(0, std::memory_order_relaxed);
    recentChannels.store(0, std::memory_order_relaxed);
}

OutputPeakSnapshot OutputPeakHold::snapshot() const noexcept {
    return {left.load(std::memory_order_relaxed), right.load(std::memory_order_relaxed),
            channels.load(std::memory_order_relaxed)};
}

OutputPeakSnapshot OutputPeakHold::takeRecent() noexcept {
    return {recentLeft.exchange(0, std::memory_order_relaxed),
            recentRight.exchange(0, std::memory_order_relaxed),
            recentChannels.exchange(0, std::memory_order_relaxed)};
}

float decayOutputPeak(float previous, float incoming, double elapsedSeconds) noexcept {
    if (!std::isfinite(previous) || previous < 0) {
        previous = 0;
    }
    if (std::isnan(incoming) || incoming < 0) {
        incoming = 0;
    }
    if (!std::isfinite(elapsedSeconds) || elapsedSeconds < 0) {
        elapsedSeconds = 0;
    }

    const auto decayed =
        static_cast<float>(previous * std::pow(10.0, -24.0 * elapsedSeconds / 20.0));
    return std::max(incoming, decayed);
}

bool boundaryPreviewRange(juce::int64 boundary, juce::int64 before, juce::int64 after,
                          juce::int64 projectEnd, PlaybackRange& result) {
    if (projectEnd <= 0 || boundary < 0 || boundary > projectEnd || before < 0 || after < 0) {
        return false;
    }

    const auto start = boundary - std::min(before, boundary);
    const auto end = boundary + std::min(after, projectEnd - boundary);
    if (start >= end) {
        return false;
    }

    result = {static_cast<double>(start) / sessionRate, static_cast<double>(end) / sessionRate,
              false};
    return true;
}

bool selectionPreviewRange(juce::int64 start, juce::int64 end, juce::int64 before,
                           juce::int64 after, juce::int64 projectEnd, PlaybackRange& result) {
    if (projectEnd <= 0 || projectEnd >= 9007199254740992LL || start < 0 || start >= end ||
        end > projectEnd || before < 0 || after < 0) {
        return false;
    }

    const auto first = start - std::min(before, start);
    const auto last = end + std::min(after, projectEnd - end);
    result = {static_cast<double>(first) / sessionRate, static_cast<double>(last) / sessionRate,
              false};
    return true;
}

bool prepareRipplePreview(const Session& source, const TimeSelection& selection, bool allTracks,
                          juce::int64 before, juce::int64 after, Session& result,
                          PlaybackRange& range, juce::String& error) {
    auto candidate = source;
    PlaybackRange candidateRange;
    if (!rippleRemoveRange(candidate, selection, allTracks, &error)) {
        if (error.isEmpty()) {
            error = "Nothing follows or intersects the selected range.";
        }
        return false;
    }

    if (!boundaryPreviewRange(selection.start, before, after, projectEndSample(candidate),
                              candidateRange)) {
        error = "No audio time available around the proposed join.";
        return false;
    }

    result = std::move(candidate);
    range = candidateRange;
    error.clear();
    return true;
}

bool prepareIndependentRipplePreview(const Session& source, const TimeSelection& selection,
                                     const std::vector<int>& tracks, juce::int64 before,
                                     juce::int64 after, Session& result, PlaybackRange& range,
                                     juce::String& error) {
    auto candidate = source;
    Clipboard unused;
    PlaybackRange candidateRange;
    if (!editIndependentTracks(candidate, selection, tracks, IndependentRangeEdit::closeGap, unused,
                               error)) {
        return false;
    }

    if (!boundaryPreviewRange(selection.start, before, after, projectEndSample(candidate),
                              candidateRange)) {
        error = "No audio time available around the proposed join.";
        return false;
    }

    result = std::move(candidate);
    range = candidateRange;
    error.clear();
    return true;
}

bool prepareIndependentTrackAudition(const Session& source, const TimeSelection& selection,
                                     const std::vector<int>& tracks, bool loop, Session& result,
                                     PlaybackRange& range, juce::String& error) {
    if (!selection.hasRange() || selection.lastTrack >= static_cast<int>(source.tracks.size()) ||
        selection.end >= 9007199254740992LL) {
        error = "Select a valid time range.";
        return false;
    }

    auto candidate = source;
    if (!limitToTrackIndices(candidate, tracks)) {
        error = "Select valid independent tracks.";
        return false;
    }

    PlaybackRange candidateRange{static_cast<double>(selection.start) / sessionRate,
                                 static_cast<double>(selection.end) / sessionRate, loop};
    result = std::move(candidate);
    range = candidateRange;
    error.clear();
    return true;
}

bool prepareSelectedCrossfadePreview(const Session& source, ClipReference first,
                                     ClipReference second, juce::int64 before, juce::int64 after,
                                     Session& result, PlaybackRange& range, juce::String& error) {
    if (before < 0 || after < 0) {
        error = "Invalid audition context.";
        return false;
    }

    auto candidate = source;
    juce::Range<double> overlap;
    if (!crossfadeClipPair(candidate, first, second, error, &overlap)) {
        return false;
    }

    const auto& clip =
        candidate.tracks[static_cast<size_t>(first.track)].clips[static_cast<size_t>(first.clip)];
    juce::int64 a = 0;
    juce::int64 b = 0;
    if (!clipSampleRangeWithin(clip, overlap.getStart(), overlap.getEnd(), a, b)) {
        error = "The overlap contains no project samples to audition.";
        return false;
    }

    PlaybackRange candidateRange;
    if (!selectionPreviewRange(a, b, before, after, projectEndSample(candidate), candidateRange)) {
        error = "Invalid crossfade range.";
        return false;
    }

    result = std::move(candidate);
    range = candidateRange;
    error.clear();
    return true;
}

bool prepareCrossfadePreview(const Session& source, int track, int clip, bool acrossTracks,
                             juce::int64 before, juce::int64 after, Session& result,
                             PlaybackRange& range, juce::String& error) {
    if (before < 0 || after < 0) {
        error = "Invalid audition context.";
        return false;
    }

    auto candidate = source;
    juce::Range<double> overlap;
    const bool changed = acrossTracks
                             ? crossfadeAcrossTracks(candidate, track, clip, error, &overlap)
                             : crossfadeOverlap(candidate, track, clip, error, &overlap);
    if (!changed) {
        return false;
    }

    const auto& selected =
        candidate.tracks[static_cast<size_t>(track)].clips[static_cast<size_t>(clip)];
    juce::int64 first = 0;
    juce::int64 last = 0;
    if (!clipSampleRangeWithin(selected, overlap.getStart(), overlap.getEnd(), first, last)) {
        error = "The overlap contains no project samples to audition.";
        return false;
    }

    const auto end = projectEndSample(candidate);
    if (last > end) {
        error = "Invalid crossfade range.";
        return false;
    }

    const auto start = first - std::min(first, before);
    const auto finish = last + std::min(after, end - last);
    PlaybackRange candidateRange{static_cast<double>(start) / sessionRate,
                                 static_cast<double>(finish) / sessionRate, false};
    result = std::move(candidate);
    range = candidateRange;
    error.clear();
    return true;
}

bool prepareClipGroupAudition(const Session& source, const std::vector<ClipReference>& clips,
                              bool loop, Session& result, PlaybackRange& range,
                              juce::String& error) {
    auto candidate = source;
    juce::int64 start = 0;
    juce::int64 end = 0;
    if (!isolateClipGroup(candidate, clips, start, end, error)) {
        return false;
    }

    PlaybackRange candidateRange{static_cast<double>(start) / sessionRate,
                                 static_cast<double>(end) / sessionRate, loop};
    result = std::move(candidate);
    range = candidateRange;
    error.clear();
    return true;
}

bool prepareDiscPreview(const Session& session, DiscPreviewPlan& result, juce::String& error,
                        const juce::String& trackId, bool throughEnd) {
    if (throughEnd && trackId.isEmpty()) {
        error = "Select an MD track to preview from.";
        return false;
    }
    if (session.mdDisc.tracks.empty()) {
        error = "Add at least one MiniDisc track first.";
        return false;
    }

    std::string reason;
    if (!validateMdDisc(session.mdDisc, reason)) {
        error = "Invalid MD disc: " + juce::String::fromUTF8(reason.c_str());
        return false;
    }

    const auto masterEnd = static_cast<juce::int64>(projectEndSample(session));
    std::vector<juce::int64> trackSamples(session.mdDisc.tracks.size(), 0);
    for (size_t i = 0; i < session.mdDisc.tracks.size(); ++i) {
        const auto& trk = session.mdDisc.tracks[i];
        if (trk.parts.empty()) {
            error = "MD track " + juce::String(static_cast<int>(i + 1)) + " has no timeline parts.";
            return false;
        }
        for (const auto& part : trk.parts) {
            if (part.startSample < 0 || part.endSample <= part.startSample ||
                part.endSample > masterEnd) {
                error = "MD track " + juce::String(static_cast<int>(i + 1)) +
                        " references audio outside the current timeline.";
                return false;
            }
            const auto len = part.endSample - part.startSample;
            if (len > std::numeric_limits<juce::int64>::max() - trackSamples[i]) {
                error = "MD track duration overflows supported sample range.";
                return false;
            }
            trackSamples[i] += len;
        }
    }

    int targetIdx = -1;
    if (trackId.isNotEmpty()) {
        targetIdx = findMdTrackIndex(session.mdDisc, trackId.toStdString());
        if (targetIdx < 0) {
            error = "MD track not found.";
            return false;
        }
    }

    DiscPreviewPlan next;
    juce::int64 cursor = 0;
    if (throughEnd && targetIdx > 0) {
        for (int i = 0; i < targetIdx; ++i) {
            next.programStart += trackSamples[static_cast<size_t>(i)];
        }
    }

    // Keep enough preceding logical audio for every supported output-rate
    // resampler, without allocating a span per earlier occurrence.
    const auto maxContext = static_cast<juce::int64>(
        SincResampler::supportTaps(sessionRate, sessionRate / SincResampler::maxFullSupportRatio) /
        2);
    const auto contextStart =
        throughEnd ? std::max<juce::int64>(0, next.programStart - maxContext) : 0;

    for (size_t i = 0; i < session.mdDisc.tracks.size(); ++i) {
        const auto& track = session.mdDisc.tracks[i];
        if (targetIdx >= 0 && !throughEnd && static_cast<int>(i) != targetIdx) {
            continue;
        }
        if (throughEnd && cursor + trackSamples[i] <= contextStart) {
            cursor += trackSamples[i];
            continue;
        }

        for (const auto& part : track.parts) {
            const auto length = part.endSample - part.startSample;
            if (throughEnd && cursor + length <= contextStart) {
                cursor += length;
                continue;
            }

            const auto visibleStart = std::max(cursor, contextStart);
            const auto sourceStart = part.startSample + (visibleStart - cursor);

            // Adjacent logical ranges that also touch in the source are one
            // continuous render. Keep non-linear jumps and repeats separate.
            if (!next.spans.empty() &&
                next.spans.back().sourceStart +
                        (next.spans.back().logicalEnd - next.spans.back().logicalStart) ==
                    sourceStart) {
                next.spans.back().logicalEnd = cursor + length;
            } else {
                next.spans.push_back({visibleStart, cursor + length, sourceStart});
            }
            cursor += length;
        }
    }

    next.samples = cursor - next.programStart;
    result = std::move(next);
    error.clear();
    return true;
}

bool prepareDiscPreviewRenderer(const Session& session, double rate, int channels,
                                DiscPreviewRenderer& result, juce::String& error) {
    if (!std::isfinite(rate) || rate <= 0 || rate > 768000 ||
        !SincResampler::hasFullSupport(sessionRate, rate)) {
        error = "Audio output rate is unsupported for disc preview.";
        return false;
    }
    if (!validateMixRange(session, channels, {0, projectEndSample(session)}, error)) {
        return false;
    }

    DiscPreviewRenderer next;
    next.deviceRate = rate;
    next.channels = channels;
    session.prepareResampling(next.bankResampling, sessionRate);

    if (std::abs(rate - sessionRate) > 1e-9) {
        next.taps = SincResampler::supportTaps(sessionRate, rate);
        next.outputResampler = std::make_unique<SincResampler>(sessionRate, rate);
        if (!next.outputResampler->valid()) {
            error = "Could not prepare disc preview resampling.";
            return false;
        }
        const int needed =
            static_cast<int>(std::ceil(DiscPreviewRenderer::maxOutputChunk * sessionRate / rate)) +
            next.taps + 8;
        next.scratch.setSize(channels, needed);
    }

    result = std::move(next);
    error.clear();
    return true;
}

bool renderDiscPreview(const Session& session, const DiscPreviewPlan& plan,
                       DiscPreviewRenderer& renderer, juce::AudioBuffer<float>& output, int begin,
                       int count, juce::int64& frames, double rate, int channels,
                       bool* unsupportedRate) {
    if (!std::isfinite(rate) || rate <= 0 || !sameDouble(rate, renderer.deviceRate) ||
        channels != renderer.channels || frames < 0 || count < 0 || begin < 0 ||
        begin > output.getNumSamples() || count > output.getNumSamples() - begin ||
        !outputSupportsMaster(output.getNumChannels(), channels) || plan.spans.empty() ||
        plan.samples <= 0) {
        return false;
    }

    const long double endFrame =
        std::ceil(static_cast<long double>(plan.samples) * rate / sessionRate - 1.0e-10L);
    int written = 0;

    while (written < count && static_cast<long double>(frames) < endFrame) {
        const int n = static_cast<int>(std::min<long double>(
            {static_cast<long double>(count - written),
             static_cast<long double>(DiscPreviewRenderer::maxOutputChunk), endFrame - frames}));
        if (n <= 0) {
            break;
        }

        if (!renderer.outputResampler) {
            if (!renderDiscSamples(session, plan, output, begin + written,
                                   plan.programStart + frames, n, channels, renderer.bankResampling,
                                   unsupportedRate)) {
                return false;
            }
        } else {
            const long double firstPosition = static_cast<long double>(plan.programStart) +
                                              static_cast<long double>(frames) * sessionRate / rate;
            const long double lastPosition =
                static_cast<long double>(plan.programStart) +
                static_cast<long double>(frames + n - 1) * sessionRate / rate;
            const auto first = std::max<juce::int64>(
                0, static_cast<juce::int64>(std::floor(firstPosition)) - (renderer.taps / 2 - 1));
            const auto last = std::min<juce::int64>(
                plan.programStart + plan.samples,
                static_cast<juce::int64>(std::floor(lastPosition)) + renderer.taps / 2 + 1);
            const auto required = last - first;
            if (required <= 0 || required > renderer.scratch.getNumSamples()) {
                return false;
            }

            const int scratchCount = static_cast<int>(required);
            renderer.scratch.clear(0, scratchCount);
            if (!renderDiscSamples(session, plan, renderer.scratch, 0, first, scratchCount,
                                   channels, renderer.bankResampling, unsupportedRate)) {
                return false;
            }

            for (int i = 0; i < n; ++i) {
                const auto samplePosition = static_cast<double>(
                    static_cast<long double>(plan.programStart) +
                    static_cast<long double>(frames + i) * sessionRate / rate - first);
                if (channels == 1) {
                    output.addSample(
                        0, begin + written + i,
                        renderer.outputResampler->sample(renderer.scratch.getReadPointer(0),
                                                         scratchCount, samplePosition));
                } else {
                    const auto pair = renderer.outputResampler->sampleStereo(
                        renderer.scratch.getReadPointer(0), renderer.scratch.getReadPointer(1),
                        scratchCount, samplePosition);
                    output.addSample(0, begin + written + i, pair.first);
                    output.addSample(1, begin + written + i, pair.second);
                }
            }
        }

        frames += n;
        written += n;
    }

    return static_cast<long double>(frames) < endFrame;
}

bool renderPlayback(const Session& session, juce::AudioBuffer<float>& output, int begin, int count,
                    double& position, double rate, int channels, const PlaybackRange& range,
                    const ResamplingContext* prepared, PlaybackClock* clock,
                    const RenderClipIndex* index, bool* unsupportedRate) {
    if (!std::isfinite(rate) || rate <= 0 || !std::isfinite(position) ||
        !std::isfinite(range.start) || !std::isfinite(range.end) || range.start < 0 ||
        range.end <= range.start) {
        return false;
    }

    const bool all = range.tracks.first == -1 && range.tracks.last == -1;
    if (!all && (range.tracks.first < 0 || range.tracks.last < range.tracks.first ||
                 range.tracks.last >= static_cast<int>(session.tracks.size()))) {
        return false;
    }
    if (range.onlyClip < -1 ||
        (range.onlyClip >= 0 &&
         (all || range.tracks.first != range.tracks.last ||
          range.onlyClip >=
              static_cast<int>(
                  session.tracks[static_cast<size_t>(range.tracks.first)].clips.size())))) {
        return false;
    }

    ResamplingContext temporary;
    if (!prepared) {
        session.prepareResampling(temporary, rate);
        prepared = &temporary;
    }

    // Keep phase relative to the loop even when the caller does not retain a
    // clock; absolute-time wrap arithmetic loses precision in short loops.
    PlaybackClock localClock;
    if (!clock) {
        clock = &localClock;
    }

    if (range.loop) {
        return renderLoopedPlayback(session, output, begin, count, position, rate, channels, range,
                                    prepared, clock, index, unsupportedRate);
    }

    return renderLinearPlayback(session, output, begin, count, position, rate, channels, range,
                                prepared, clock, index, unsupportedRate);
}

} // namespace carta
