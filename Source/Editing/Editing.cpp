/**
 * Timeline editing operations, selection geometry, undo history, and marker tools.
 *
 * Implements sample-grid time arithmetic, range and clip-group edits, atomic
 * marker validation (`preservesMarkerBounds`), undo/redo metadata & media
 * accounting, silence/quiet-gap scanners, and assistive timeline descriptions.
 */
#include "Editing/Editing.h"
#include "IO/SessionIO.h"
#include <set>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <tuple>
#include <charconv>
#include <string_view>
#include <unordered_set>

namespace carta {

bool parseEditedFloat(const juce::String& input, const juce::String& displayed, float original,
                      double minimum, double maximum, double scale, EditedFloat& output) {
    if (!std::isfinite(original) || !std::isfinite(minimum) || !std::isfinite(maximum) ||
        minimum > maximum || !std::isfinite(scale) || scale <= 0) {
        return false;
    }

    const auto text = input.trim().toStdString();
    if (text.empty()) {
        return false;
    }

    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || !std::isfinite(parsed) || parsed < minimum ||
        parsed > maximum) {
        return false;
    }

    const bool edited = input.trim() != displayed;
    const auto value = edited ? static_cast<float>(parsed * scale) : original;
    if (!std::isfinite(value)) {
        return false;
    }

    output = {value, edited};
    return true;
}

bool ZoomMemory::remember(TimelineViewport value) noexcept {
    if (!std::isfinite(value.start) || !std::isfinite(value.span) || value.start < 0 ||
        value.span <= 0) {
        return false;
    }

    previous = value;
    present = true;
    return true;
}

bool ZoomMemory::toggle(TimelineViewport& current, double fullLength) noexcept {
    if (!present || !std::isfinite(fullLength) || fullLength <= 0 ||
        !std::isfinite(current.start) || current.start < 0 || !std::isfinite(current.span) ||
        current.span <= 0) {
        return false;
    }

    const auto clampView = [&](TimelineViewport view) {
        view.span = std::clamp(view.span, std::min(fullLength, 16.0 / sessionRate), fullLength);
        view.start = std::clamp(view.start, 0.0, std::max(0.0, fullLength - view.span));
        return view;
    };

    const auto old = clampView(current);
    current = clampView(previous);
    previous = old;
    return true;
}

bool SelectionMemory::remember(const TimeSelection& value, int trackCount,
                               juce::uint64 currentGeneration) noexcept {
    if (!value.hasRange() || value.lastTrack >= trackCount) {
        return false;
    }

    selection = value;
    generation = currentGeneration;
    present = true;
    return true;
}

bool SelectionMemory::canRestore(int trackCount, juce::uint64 currentGeneration) const noexcept {
    return present && generation == currentGeneration && selection.hasRange() &&
           selection.lastTrack < trackCount;
}

bool SelectionMemory::restore(TimeSelection& target, int trackCount,
                              juce::uint64 currentGeneration) const noexcept {
    if (!canRestore(trackCount, currentGeneration)) {
        return false;
    }

    target = selection;
    return true;
}

void SelectionMemory::retainAfterGeometryStableEdit(juce::uint64 previousGeneration,
                                                    juce::uint64 nextGeneration) noexcept {
    if (present && generation == previousGeneration) {
        generation = nextGeneration;
    }
}

bool selectionFromLength(juce::int64 start, juce::int64 length, int firstTrack, int lastTrack,
                         juce::int64 limit, int trackCount, TimeSelection& result) noexcept {
    if (start < 0 || length <= 0 || limit <= 0 || start >= limit || length > limit - start ||
        firstTrack < 0 || lastTrack < firstTrack || lastTrack >= trackCount) {
        return false;
    }

    result = {start, start + length, firstTrack, lastTrack};
    return true;
}

bool selectMasterTrackAtSample(const Session& session, juce::int64 playhead, TimeSelection& result,
                               juce::String& error, TrackRange scope) {
    error.clear();
    const auto count = static_cast<int>(session.tracks.size());
    if (scope.first == -1 && scope.last == -1) {
        scope = {0, count - 1};
    }
    if (scope.first < 0 || scope.last < scope.first || scope.last >= count) {
        error = "Select valid destination tracks first.";
        return false;
    }

    MasterTrack track;
    int index = 0;
    if (!masterTrackAtSample(session, playhead, track, index, error)) {
        return false;
    }

    result = {track.range.start, track.range.end, scope.first, scope.last};
    return true;
}

bool selectToBoundary(const Session& session, juce::int64 playhead, SelectionBoundary boundary,
                      TimeSelection& result, TrackRange scope) noexcept {
    const auto end = projectEndSample(session);
    const auto count = static_cast<int>(session.tracks.size());
    if (count <= 0 || end <= 0 || playhead < 0 || playhead > end) {
        return false;
    }

    if (scope.first == -1 && scope.last == -1) {
        scope = {0, count - 1};
    }
    if (scope.first < 0 || scope.last < scope.first || scope.last >= count) {
        return false;
    }

    juce::int64 target = 0;
    bool found = true;
    switch (boundary) {
    case SelectionBoundary::masterStart:
        target = 0;
        break;
    case SelectionBoundary::masterEnd:
        target = end;
        break;
    case SelectionBoundary::previousClip:
    case SelectionBoundary::nextClip: {
        const bool next = boundary == SelectionBoundary::nextClip;
        found = false;
        for (int t = scope.first; t <= scope.last; ++t) {
            for (const auto& clip : session.tracks[static_cast<size_t>(t)].clips) {
                juce::int64 first = 0;
                juce::int64 last = 0;
                if (!clipSampleRange(clip, first, last)) {
                    continue;
                }
                for (const auto value : {first, last}) {
                    if ((next ? value > playhead : value < playhead) &&
                        (!found || (next ? value < target : value > target))) {
                        target = value;
                        found = true;
                    }
                }
            }
        }
        break;
    }
    case SelectionBoundary::previousMarker:
    case SelectionBoundary::nextMarker: {
        const bool next = boundary == SelectionBoundary::nextMarker;
        found = false;
        for (const auto& marker : session.markers) {
            const auto frame = marker.position * sessionRate;
            if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
                continue;
            }
            const auto sample = static_cast<juce::int64>(std::llround(frame));
            if (sample >= end || (next ? sample <= playhead : sample >= playhead)) {
                continue;
            }
            if (!found || (next ? sample < target : sample > target)) {
                target = sample;
                found = true;
            }
        }
        break;
    }
    default:
        return false;
    }

    if (!found || target < 0 || target > end || target == playhead) {
        return false;
    }

    result = {std::min(playhead, target), std::max(playhead, target), scope.first, scope.last};
    return true;
}

bool selectToBoundaryOnTracks(const Session& session, juce::int64 playhead,
                              SelectionBoundary boundary, const std::vector<int>& tracks,
                              TimeSelection& result) {
    if (tracks.empty()) {
        return false;
    }

    std::set<int> chosen;
    for (int track : tracks) {
        if (track < 0 || track >= static_cast<int>(session.tracks.size()) ||
            !chosen.insert(track).second) {
            return false;
        }
    }

    const TrackRange span{*chosen.begin(), *chosen.rbegin()};
    if (boundary != SelectionBoundary::previousClip && boundary != SelectionBoundary::nextClip) {
        return selectToBoundary(session, playhead, boundary, result, span);
    }

    bool found = false;
    TimeSelection best;
    for (int track : chosen) {
        TimeSelection candidate;
        if (!selectToBoundary(session, playhead, boundary, candidate, {track, track})) {
            continue;
        }
        if (!found || (boundary == SelectionBoundary::previousClip ? candidate.start > best.start
                                                                   : candidate.end < best.end)) {
            best = candidate;
            found = true;
        }
    }

    if (!found) {
        return false;
    }

    best.firstTrack = span.first;
    best.lastTrack = span.last;
    result = best;
    return true;
}

namespace {

bool selectClipGapWithinEnd(const Session& session, int track, juce::int64 sample, juce::int64 end,
                            TimeSelection& result) noexcept {
    if (track < 0 || track >= static_cast<int>(session.tracks.size()) || sample < 0 ||
        sample >= end) {
        return false;
    }

    juce::int64 left = 0;
    juce::int64 right = end;
    for (const auto& clip : session.tracks[static_cast<size_t>(track)].clips) {
        juce::int64 first = 0;
        juce::int64 last = 0;
        if (!clipSampleRange(clip, first, last)) {
            continue;
        }
        if (sample >= first && sample < last) {
            return false;
        }
        if (last <= sample) {
            left = std::max(left, last);
        }
        if (first > sample) {
            right = std::min(right, first);
        }
    }

    if (left >= right) {
        return false;
    }

    result = {left, right, track, track};
    return true;
}

} // namespace

bool selectClipGap(const Session& session, int track, juce::int64 sample,
                   TimeSelection& result) noexcept {
    return selectClipGapWithinEnd(session, track, sample, projectEndSample(session), result);
}

bool selectClipGapOnTracks(const Session& session, const std::vector<int>& tracks,
                           juce::int64 sample, TimeSelection& result) {
    if (tracks.empty()) {
        return false;
    }

    const auto end = projectEndSample(session);
    std::set<int> chosen;
    TimeSelection combined;
    bool first = true;

    for (const int track : tracks) {
        if (!chosen.insert(track).second) {
            return false;
        }
        TimeSelection gap;
        if (!selectClipGapWithinEnd(session, track, sample, end, gap)) {
            return false;
        }
        if (first) {
            combined = gap;
            first = false;
        } else {
            combined.start = std::max(combined.start, gap.start);
            combined.end = std::min(combined.end, gap.end);
        }
    }

    if (combined.start >= combined.end) {
        return false;
    }

    combined.firstTrack = *chosen.begin();
    combined.lastTrack = *chosen.rbegin();
    result = combined;
    return true;
}

juce::String formatProjectTime(juce::int64 sample) {
    sample = std::max<juce::int64>(0, sample);
    const auto secondsTotal = sample / 44100;
    const auto millis = (sample % 44100) * 1000 / 44100;
    const auto two = [](juce::int64 n) { return juce::String(n).paddedLeft('0', 2); };
    const auto hours = secondsTotal / 3600;
    const auto minutes = hours > 0 ? (secondsTotal / 60) % 60 : secondsTotal / 60;
    return (hours > 0 ? two(hours) + ":" : juce::String()) + two(minutes) + ":" +
           two(secondsTotal % 60) + "." + juce::String(millis).paddedLeft('0', 3);
}

juce::String formatTimelineTime(juce::int64 sample, TimelineTimeFormat format) {
    sample = std::max<juce::int64>(0, sample);
    if (format == TimelineTimeFormat::samples) {
        return juce::String(sample) + " smp";
    }
    if (format == TimelineTimeFormat::automatic) {
        return formatProjectTime(sample);
    }

    const auto total = sample / 44100;
    const auto micros = ((sample % 44100) * 1000000 + 22050) / 44100;
    const auto fraction = juce::String(micros).paddedLeft('0', 6);
    if (format == TimelineTimeFormat::seconds) {
        return juce::String(total) + "." + fraction + " s";
    }

    const auto two = [](juce::int64 n) { return juce::String(n).paddedLeft('0', 2); };
    const auto hours = total / 3600;
    const auto minutes = hours > 0 ? (total / 60) % 60 : total / 60;
    return (hours > 0 ? two(hours) + ":" : juce::String()) + two(minutes) + ":" + two(total % 60) +
           "." + fraction;
}

bool parseSampleOrTime(const juce::String& text, juce::int64& result) {
    constexpr juce::uint64 maximum = 9007199254740991ULL;
    constexpr juce::uint64 rate = 44100;

    auto normalized = text.trim();
    bool secondsSuffix = false;
    bool samplesSuffix = false;
    if (normalized.endsWithIgnoreCase("smp")) {
        samplesSuffix = true;
        normalized = normalized.dropLastCharacters(3).trimEnd();
    } else if (normalized.endsWithIgnoreCase("s")) {
        secondsSuffix = true;
        normalized = normalized.dropLastCharacters(1).trimEnd();
    }

    const auto storage = normalized.toStdString();
    const std::string_view input(storage);
    const auto number = [](std::string_view part, juce::uint64& value) {
        if (part.empty()) {
            return false;
        }
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == part.data() + part.size();
    };

    const auto colon = input.find(':');
    if (colon != std::string_view::npos && (secondsSuffix || samplesSuffix)) {
        return false;
    }

    if (colon == std::string_view::npos) {
        if (secondsSuffix) {
            const auto dot = input.find('.');
            juce::uint64 seconds = 0;
            juce::uint64 fraction = 0;
            juce::uint64 denominator = 1;
            if (!number(input.substr(0, dot), seconds) || seconds > maximum / rate) {
                return false;
            }
            if (dot != std::string_view::npos) {
                const auto digits = input.substr(dot + 1);
                if (digits.empty() || digits.size() > 9 || !number(digits, fraction)) {
                    return false;
                }
                for (size_t i = 0; i < digits.size(); ++i) {
                    denominator *= 10;
                }
            }
            const auto whole = seconds * rate;
            const auto partial = (fraction * rate + denominator / 2) / denominator;
            if (partial > maximum - whole) {
                return false;
            }
            result = static_cast<juce::int64>(whole + partial);
            return true;
        }

        juce::uint64 samples = 0;
        if (!number(input, samples) || samples > maximum) {
            return false;
        }
        result = static_cast<juce::int64>(samples);
        return true;
    }

    const auto second = input.find(':', colon + 1);
    if (second != std::string_view::npos && input.find(':', second + 1) != std::string_view::npos) {
        return false;
    }

    juce::uint64 hours = 0;
    juce::uint64 minutes = 0;
    juce::uint64 seconds = 0;
    juce::uint64 fraction = 0;
    juce::uint64 denominator = 1;

    if (second == std::string_view::npos) {
        if (!number(input.substr(0, colon), minutes)) {
            return false;
        }
    } else if (!number(input.substr(0, colon), hours) ||
               !number(input.substr(colon + 1, second - colon - 1), minutes) || minutes >= 60) {
        return false;
    }

    const auto secondsText = input.substr((second == std::string_view::npos ? colon : second) + 1);
    const auto dot = secondsText.find('.');
    if (!number(secondsText.substr(0, dot), seconds) || seconds >= 60) {
        return false;
    }
    if (dot != std::string_view::npos) {
        const auto digits = secondsText.substr(dot + 1);
        if (digits.empty() || digits.size() > 9 || !number(digits, fraction)) {
            return false;
        }
        for (size_t i = 0; i < digits.size(); ++i) {
            denominator *= 10;
        }
    }

    const auto maxSeconds = maximum / rate;
    if (hours > maxSeconds / 3600 || minutes > maxSeconds / 60) {
        return false;
    }

    const auto h = hours * 3600;
    const auto m = minutes * 60;
    if (h > maxSeconds - m || h + m > maxSeconds - seconds) {
        return false;
    }

    const auto whole = (h + m + seconds) * rate;
    const auto partial = (fraction * rate + denominator / 2) / denominator;
    if (partial > maximum - whole) {
        return false;
    }

    result = static_cast<juce::int64>(whole + partial);
    return true;
}

namespace {

juce::String describeClipSource(const Clip& clip) {
    if (!clip.audio) {
        return "Source unavailable. ";
    }
    const int count = clip.audio->numChannels();
    return juce::String(count) + (count == 1 ? " source channel, " : " source channels, ") +
           juce::String(clip.audio->sampleRate, 2) + " Hz. ";
}

} // namespace

juce::String describeNavigationTarget(const Session& session, int track, int clip,
                                      const SourceVerificationCache* checks) {
    if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
        return "No layer selected.";
    }

    const auto& t = session.tracks[static_cast<size_t>(track)];
    juce::String text = "Layer " + juce::String(track + 1) + " of " +
                        juce::String(static_cast<int>(session.tracks.size())) + ": " +
                        (t.name.trim().isEmpty() ? juce::String("unnamed") : t.name) + ". ";
    if (t.muted) {
        text += "Muted. ";
    }
    if (t.solo) {
        text += "Solo. ";
    }
    if (t.collapsed) {
        text += "Collapsed. ";
    }

    text += (t.collapsed ? "Expanded height " : "Height ") + juce::String(t.height) + " pixels. ";
    if (t.waveformZoom > 0) {
        text += "Waveform zoom " + juce::String(t.waveformZoom) + " times. ";
    }
    if (!t.channelHeightWeights.empty() &&
        static_cast<int>(t.channelHeightWeights.size()) != maxTrackChannels(t)) {
        text += "Channel heights differ from the source count; showing equal heights. ";
    }

    const int displayedChannel = isolatedDisplayChannel(t);
    if (displayedChannel >= 0) {
        text += "Showing source channel " + juce::String(displayedChannel + 1) + ". ";
    } else if (t.displaySourceChannel >= 0) {
        text += "Showing all source channels because isolation changed. ";
    }

    if (checks && checks->trackHasIssue(t)) {
        text += "A source needs attention according to the last file check. ";
    }

    const auto channels = maxTrackChannels(t);
    if (channels > 0 && !clipRoutingConfirmed(t, channels)) {
        text += "Channel routing needs confirmation. ";
    }

    if (clip >= 0 && clip < static_cast<int>(t.clips.size())) {
        const auto& item = t.clips[static_cast<size_t>(clip)];
        text += "Clip: " + clipDisplayName(item) + ". " + describeClipSource(item);
        if (item.groupId.isNotEmpty()) {
            text += "Grouped clip. ";
        }

        juce::int64 first = 0;
        juce::int64 last = 0;
        if (clipSampleRange(item, first, last)) {
            text +=
                "Samples " + juce::String(first) + " to " + juce::String(last) + ", end exclusive.";
        }

        const auto overlaps = overlappingClipCount(t, clip);
        if (overlaps > 0) {
            text += " Overlaps " + juce::String(overlaps) +
                    (overlaps == 1 ? " other clip" : " other clips") +
                    "; signals sum when audible.";
        }
    }

    return text.trimEnd();
}

std::vector<NavigationRow> makeNavigationRows(const Session& session,
                                              const SourceVerificationCache* checks) {
    std::vector<NavigationRow> rows;
    for (int track = 0; track < static_cast<int>(session.tracks.size()); ++track) {
        const auto& t = session.tracks[static_cast<size_t>(track)];
        const auto name = t.name.trim().isEmpty() ? juce::String("unnamed") : t.name;
        const auto heading = "Layer " + juce::String(track + 1) + ": " + name;
        const auto description = describeNavigationTarget(session, track, -1, checks);
        rows.push_back({track, -1, heading, description});

        std::vector<int> indices;
        indices.reserve(t.clips.size());
        for (int i = 0; i < static_cast<int>(t.clips.size()); ++i) {
            const auto& c = t.clips[static_cast<size_t>(i)];
            if (c.audio && std::isfinite(c.start) && c.start >= 0 && std::isfinite(c.duration) &&
                c.duration > 0 && std::isfinite(c.start + c.duration)) {
                indices.push_back(i);
            }
        }

        std::sort(indices.begin(), indices.end(), [&](int a, int b) {
            const auto& x = t.clips[static_cast<size_t>(a)];
            const auto& y = t.clips[static_cast<size_t>(b)];
            return std::make_tuple(x.start, x.start + x.duration, a) <
                   std::make_tuple(y.start, y.start + y.duration, b);
        });

        const auto overlapCounts = overlappingClipCounts(t);
        for (size_t ordinal = 0; ordinal < indices.size(); ++ordinal) {
            const int index = indices[ordinal];
            const auto& c = t.clips[static_cast<size_t>(index)];
            juce::int64 first = 0;
            juce::int64 last = 0;
            const auto nameClip = clipDisplayName(c);
            const bool hasRange = clipSampleRange(c, first, last);
            const auto bounds = hasRange ? "samples " + juce::String(first) + " to " +
                                               juce::String(last) + ", end exclusive"
                                         : juce::String("no project samples");
            const auto number = juce::String(static_cast<int>(ordinal + 1));
            auto clipDescription = description + " Clip: " + nameClip + ", " + number + " of " +
                                   juce::String(static_cast<int>(indices.size())) + ", " + bounds +
                                   ". " + describeClipSource(c) +
                                   (c.groupId.isNotEmpty() ? "Grouped clip. " : "");
            const int overlaps = overlapCounts[static_cast<size_t>(index)];
            if (overlaps > 0) {
                clipDescription += "Overlaps " + juce::String(overlaps) +
                                   (overlaps == 1 ? " other clip" : " other clips") +
                                   "; signals sum when audible.";
            }
            rows.push_back({track, index, "    " + number + ". " + nameClip + "  [" + bounds + "]",
                            clipDescription.trimEnd(), hasRange});
        }
    }

    return rows;
}

bool updateBrowserClipSelection(const std::vector<NavigationRow>& rows, int cursor,
                                BrowserSelectionAction action, juce::SparseSet<int>& selection) {
    const int count = static_cast<int>(rows.size());
    if ((action == BrowserSelectionAction::toggle || action == BrowserSelectionAction::track) &&
        (cursor < 0 || cursor >= count)) {
        return false;
    }
    if (action == BrowserSelectionAction::toggle && rows[static_cast<size_t>(cursor)].clip < 0) {
        return false;
    }

    auto result = selection;
    if (action == BrowserSelectionAction::all || action == BrowserSelectionAction::clear) {
        result.clear();
    }
    result.removeRange({count, std::numeric_limits<int>::max()});

    for (int row = 0; row < count; ++row) {
        const auto& entry = rows[static_cast<size_t>(row)];
        if (entry.clip < 0) {
            result.removeRange({row, row + 1});
        } else if (action == BrowserSelectionAction::all ||
                   (action == BrowserSelectionAction::track &&
                    entry.track == rows[static_cast<size_t>(cursor)].track)) {
            result.addRange({row, row + 1});
        }
    }

    if (action == BrowserSelectionAction::toggle) {
        if (result.contains(cursor)) {
            result.removeRange({cursor, cursor + 1});
        } else {
            result.addRange({cursor, cursor + 1});
        }
    }

    selection = std::move(result);
    return true;
}

juce::Range<int> visibleTrackRows(const Session& session, int firstTrack,
                                  int viewportHeight) noexcept {
    const auto count = static_cast<int>(session.tracks.size());
    if (firstTrack < 0 || firstTrack >= count || viewportHeight <= 0) {
        return {};
    }

    juce::int64 used = 0;
    int end = firstTrack;
    while (end < count && used < viewportHeight) {
        const int height = trackDisplayHeight(session.tracks[static_cast<size_t>(end)]);
        if (height <= 0) {
            break;
        }
        used += height;
        ++end;
    }

    return {firstTrack, end};
}

int timelineTimeToPixel(double time, double viewStart, double viewDuration, int left,
                        int width) noexcept {
    if (!std::isfinite(time) || !std::isfinite(viewStart) || !std::isfinite(viewDuration) ||
        viewDuration <= 0 || width <= 0) {
        return left;
    }

    const double distance = (time - viewStart) / viewDuration * static_cast<double>(width);
    const double bounded = std::clamp(distance, -100000000.0, 100000000.0);
    const auto pixel = static_cast<juce::int64>(left) + juce::roundToInt(bounded);
    return static_cast<int>(std::clamp(pixel,
                                       static_cast<juce::int64>(std::numeric_limits<int>::min()),
                                       static_cast<juce::int64>(std::numeric_limits<int>::max())));
}

std::vector<MarkerLabelRegion> layoutMarkerLabels(const std::vector<int>& positions,
                                                  juce::Range<int> viewport, int selectedMarker) {
    constexpr juce::int64 padding = 3;
    constexpr juce::int64 maxWidth = 90;
    constexpr juce::int64 minWidth = 32;
    constexpr juce::int64 gap = 2;

    std::vector<MarkerLabelRegion> result;
    if (viewport.isEmpty()) {
        return result;
    }

    const auto regionFor = [&](int index) -> juce::Range<int> {
        if (index < 0 || static_cast<size_t>(index) >= positions.size() ||
            !viewport.contains(positions[static_cast<size_t>(index)])) {
            return {};
        }
        const auto start =
            static_cast<juce::int64>(positions[static_cast<size_t>(index)]) + padding;
        const auto end = std::min(static_cast<juce::int64>(viewport.getEnd()), start + maxWidth);
        return end - start >= minWidth
                   ? juce::Range<int>{static_cast<int>(start), static_cast<int>(end)}
                   : juce::Range<int>{};
    };

    const auto reserved = regionFor(selectedMarker);
    if (!reserved.isEmpty()) {
        result.push_back({selectedMarker, reserved});
    }

    std::vector<int> order;
    order.reserve(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        if (static_cast<int>(i) != selectedMarker && viewport.contains(positions[i])) {
            order.push_back(static_cast<int>(i));
        }
    }

    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return positions[static_cast<size_t>(a)] < positions[static_cast<size_t>(b)];
    });

    juce::int64 previousEnd = viewport.getStart();
    for (int index : order) {
        auto bounds = regionFor(index);
        if (bounds.isEmpty() || static_cast<juce::int64>(bounds.getStart()) < previousEnd + gap) {
            continue;
        }

        if (!reserved.isEmpty() && bounds.getStart() < reserved.getEnd() + gap &&
            static_cast<juce::int64>(bounds.getEnd()) >
                static_cast<juce::int64>(reserved.getStart()) - gap) {
            if (bounds.getStart() >= reserved.getStart()) {
                continue;
            }
            const auto end = static_cast<juce::int64>(reserved.getStart()) - gap;
            if (end - bounds.getStart() < minWidth) {
                continue;
            }
            bounds.setEnd(static_cast<int>(end));
        }

        result.push_back({index, bounds});
        previousEnd = bounds.getEnd();
    }

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.bounds.getStart() < b.bounds.getStart();
    });
    return result;
}

juce::String describeMarkerPosition(const Session& session, int marker, juce::int64 masterEnd) {
    if (marker < 0 || marker >= static_cast<int>(session.markers.size())) {
        return "No marker selected.";
    }

    const double frame = session.markers[static_cast<size_t>(marker)].position * sessionRate;
    if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
        return "Invalid marker position.";
    }

    const auto sample = static_cast<juce::int64>(std::llround(frame));
    juce::String text =
        !markerPositionOnSampleGrid(session.markers[static_cast<size_t>(marker)].position)
            ? "Marker is off the sample grid; nearest sample " + juce::String(sample) + "."
            : "Marker sample " + juce::String(sample) + ".";
    if (masterEnd <= 0 || sample >= masterEnd) {
        text += " Outside the master duration.";
    }

    for (size_t i = 0; i < session.markers.size(); ++i) {
        if (i != static_cast<size_t>(marker)) {
            const double other = session.markers[i].position * sessionRate;
            if (std::isfinite(other) && other >= 0 && other < 9007199254740992.0 &&
                static_cast<juce::int64>(std::llround(other)) == sample) {
                text += " Another marker rounds to this sample.";
                break;
            }
        }
    }

    return text;
}

juce::String describeTimeline(const Session& session, const TimeSelection& selection, int track,
                              int clip, int marker, juce::int64 playhead, bool playing,
                              const SourceVerificationCache* checks, bool independentTrackScope) {
    juce::String text = playing ? "Playing. " : "Stopped or paused. ";
    text += "Playhead " + formatProjectTime(playhead) + ", sample " + juce::String(playhead) + ". ";

    const auto masterEnd = projectEndSample(session);
    text += "Master end sample " + juce::String(masterEnd) + ", end exclusive. ";
    if (session.minimumEndSample > 0) {
        text += "Minimum master duration " + juce::String(session.minimumEndSample) + " samples. ";
    }
    text += juce::String(static_cast<int>(session.tracks.size())) +
            (session.tracks.size() == 1 ? " layer. " : " layers. ");

    if (track >= 0 && track < static_cast<int>(session.tracks.size())) {
        const auto& t = session.tracks[static_cast<size_t>(track)];
        text += "Layer " + juce::String(track + 1) + ": " +
                (t.name.trim().isEmpty() ? juce::String("unnamed") : t.name) + ". ";
        if (checks && checks->trackHasIssue(t)) {
            text += "A source needs attention according to the last file check. ";
        }
        if (t.muted) {
            text += "Muted. ";
        }
        if (t.solo) {
            text += "Solo. ";
        }
        if (t.collapsed) {
            text += "Collapsed. ";
        }
        if (t.waveformZoom > 0) {
            text += "Waveform zoom " + juce::String(t.waveformZoom) + " times. ";
        }
        if (!t.channelHeightWeights.empty() &&
            static_cast<int>(t.channelHeightWeights.size()) != maxTrackChannels(t)) {
            text += "Channel heights differ from the source count; showing equal heights. ";
        }

        const int displayedChannel = isolatedDisplayChannel(t);
        if (displayedChannel >= 0) {
            text += "Showing source channel " + juce::String(displayedChannel + 1) + ". ";
        } else if (t.displaySourceChannel >= 0) {
            text += "Showing all source channels because isolation changed. ";
        }
        if (maxTrackChannels(t) > 0 && !clipRoutingConfirmed(t, maxTrackChannels(t))) {
            text += "Channel routing needs confirmation. ";
        }

        if (clip >= 0 && clip < static_cast<int>(t.clips.size())) {
            const auto& c = t.clips[static_cast<size_t>(clip)];
            text += "Clip: " + clipDisplayName(c) + ". " + describeClipSource(c);
            if (c.groupId.isNotEmpty()) {
                text += "Grouped clip. ";
            }
            juce::int64 start = 0;
            juce::int64 end = 0;
            if (clipSampleRange(c, start, end)) {
                text += "Clip samples " + juce::String(start) + " to " + juce::String(end) +
                        ", end exclusive. ";
            }
            const auto overlaps = overlappingClipCount(t, clip);
            if (overlaps > 0) {
                text += "Overlaps " + juce::String(overlaps) +
                        (overlaps == 1 ? " other clip" : " other clips") +
                        "; signals sum when audible. ";
            }
        }
    } else {
        text += "No layer selected. ";
    }

    if (marker >= 0 && marker < static_cast<int>(session.markers.size())) {
        const auto& m = session.markers[static_cast<size_t>(marker)];
        text += "Marker: " + (m.name.trim().isEmpty() ? juce::String("unnamed") : m.name) + ". ";
        text += describeMarkerPosition(session, marker, masterEnd) + " ";
    }

    if (selection.hasRange() && selection.lastTrack < static_cast<int>(session.tracks.size())) {
        text += "Selection samples " + juce::String(selection.start) + " to " +
                juce::String(selection.end) + ", end exclusive, " +
                (independentTrackScope ? juce::String("on independent layers.")
                                       : "layers " + juce::String(selection.firstTrack + 1) +
                                             " to " + juce::String(selection.lastTrack + 1) + ".");
    } else if (selection.hasPoint() &&
               selection.firstTrack < static_cast<int>(session.tracks.size())) {
        text += "Edit point sample " + juce::String(selection.start) + ", layer " +
                juce::String(selection.firstTrack + 1) + ".";
    } else {
        text += "No time selection.";
    }

    return text;
}

RulerTicks makeRulerTicks(double startSeconds, double spanSeconds, int widthPixels) {
    RulerTicks result;
    if (!std::isfinite(startSeconds) || !std::isfinite(spanSeconds) || startSeconds < 0 ||
        spanSeconds <= 0 || widthPixels <= 0) {
        return result;
    }

    const double start = startSeconds * sessionRate;
    const double end = (startSeconds + spanSeconds) * sessionRate;
    if (!std::isfinite(end) ||
        end >= static_cast<double>(std::numeric_limits<juce::int64>::max()) || end <= start) {
        return result;
    }

    const double required = std::max(1.0, spanSeconds * sessionRate * 110.0 / widthPixels);
    static constexpr juce::int64 steps[] = {
        1,      2,       5,       10,      20,       50,       100,      200,      441,
        882,    2205,    4410,    8820,    22050,    44100,    88200,    220500,   441000,
        661500, 1323000, 2646000, 5292000, 13230000, 26460000, 39690000, 79380000, 158760000};
    for (const auto step : steps) {
        if (static_cast<double>(step) >= required) {
            result.interval = step;
            break;
        }
    }

    if (result.interval == 0) {
        double power = 1;
        const double hours = required / (3600.0 * sessionRate);
        while (power * 10 < hours) {
            power *= 10;
        }

        double rounded = power;
        if (rounded < hours) {
            rounded = 2 * power;
        }
        if (rounded < hours) {
            rounded = 5 * power;
        }
        if (rounded < hours) {
            rounded = 10 * power;
        }

        const double step = rounded * 3600.0 * sessionRate;
        if (!std::isfinite(step) ||
            step >= static_cast<double>(std::numeric_limits<juce::int64>::max())) {
            return result;
        }
        result.interval = static_cast<juce::int64>(step);
    }

    const double first = std::ceil(start / static_cast<double>(result.interval)) *
                         static_cast<double>(result.interval);
    if (!std::isfinite(first) ||
        first >= static_cast<double>(std::numeric_limits<juce::int64>::max())) {
        return {};
    }

    auto tick = static_cast<juce::int64>(first);
    while (static_cast<double>(tick) <= end && result.samples.size() < 1000) {
        result.samples.push_back(tick);
        if (tick > std::numeric_limits<juce::int64>::max() - result.interval) {
            break;
        }
        tick += result.interval;
    }

    return result;
}

juce::String formatRulerTick(juce::int64 sample, juce::int64 interval, TimelineTimeFormat format) {
    if (format == TimelineTimeFormat::samples ||
        (format == TimelineTimeFormat::automatic && interval < 441)) {
        return formatTimelineTime(sample, TimelineTimeFormat::samples);
    }
    if (format == TimelineTimeFormat::seconds) {
        const auto precise = formatTimelineTime(sample, format);
        if (interval >= 44100) {
            return precise.upToFirstOccurrenceOf(".", false, false) + " s";
        }
        if (interval >= 441) {
            return precise.upToLastOccurrenceOf(".", true, false) +
                   precise.fromFirstOccurrenceOf(".", false, false).substring(0, 3) + " s";
        }
        return precise;
    }
    if (format == TimelineTimeFormat::clock && interval < 441) {
        return formatTimelineTime(sample, format);
    }

    auto label = formatProjectTime(sample);
    if (interval >= 44100) {
        label = label.upToLastOccurrenceOf(".", false, false);
    }
    return label;
}

double followPlayheadViewport(double start, double span, double fullLength, double playhead,
                              bool pinned) noexcept {
    if (!std::isfinite(start) || !std::isfinite(span) || !std::isfinite(fullLength) ||
        !std::isfinite(playhead) || span <= 0 || fullLength < span || playhead < 0) {
        return start;
    }

    const auto maximum = std::max(0.0, fullLength - span);
    start = std::clamp(start, 0.0, maximum);
    if (pinned) {
        return std::clamp(playhead - span * 0.5, 0.0, maximum);
    }
    if (playhead < start || playhead >= start + span * 0.9) {
        return std::clamp(playhead - span * 0.1, 0.0, maximum);
    }
    return start;
}

double selectionEdgeScroll(double start, double span, double fullLength, int left, int right,
                           int pointer, double elapsedSeconds) noexcept {
    if (!std::isfinite(start) || !std::isfinite(span) || !std::isfinite(fullLength) ||
        !std::isfinite(elapsedSeconds) || span <= 0 || fullLength < span || right <= left ||
        elapsedSeconds <= 0) {
        return start;
    }

    const double edge = std::min(24.0, static_cast<double>(right - left) * 0.25);
    const double towardLeft =
        std::clamp((static_cast<double>(left) + edge - pointer) / edge, 0.0, 1.0);
    const double towardRight =
        std::clamp((pointer - (static_cast<double>(right) - edge)) / edge, 0.0, 1.0);
    const double delta = (towardRight - towardLeft) * span * 0.6 * std::min(elapsedSeconds, 0.1);
    return std::clamp(start + delta, 0.0, std::max(0.0, fullLength - span));
}

double clipDragDelta(int pointer, int anchor, int left, int right, double viewStart,
                     double initialViewStart, double span) noexcept {
    if (right <= left || !std::isfinite(viewStart) || !std::isfinite(initialViewStart) ||
        !std::isfinite(span) || span <= 0) {
        return 0;
    }

    const int x = std::clamp(pointer, left, right);
    return viewStart - initialViewStart +
           (static_cast<double>(x) - anchor) * span / static_cast<double>(right - left);
}

int selectionTrackScrollDirection(int top, int bottom, int pointer, int firstTrack, int trackCount,
                                  int lastTrackBottom) noexcept {
    if (bottom <= top || trackCount <= 0 || firstTrack < 0 || firstTrack >= trackCount) {
        return 0;
    }

    const int edge = std::min(16, std::max(1, (bottom - top) / 4));
    if (pointer < top + edge && firstTrack > 0) {
        return -1;
    }
    if (pointer >= bottom - edge && firstTrack < trackCount - 1 && lastTrackBottom > bottom) {
        return 1;
    }
    return 0;
}

namespace {

bool validSelection(const Session& session, const TimeSelection& selection) {
    return selection.hasRange() && selection.lastTrack < static_cast<int>(session.tracks.size());
}

double seconds(juce::int64 samples) {
    return static_cast<double>(samples) / sessionRate;
}

double timelineDifference(double end, double start) {
    const double a = end * sessionRate;
    const double b = start * sessionRate;
    const double ar = std::round(a);
    const double br = std::round(b);
    if (std::isfinite(a) && std::isfinite(b) && a >= 0 && b >= 0 && ar < 9007199254740992.0 &&
        br < 9007199254740992.0 && std::abs(a - ar) < 1e-7 && std::abs(b - br) < 1e-7) {
        return (ar - br) / sessionRate;
    }
    return end - start;
}

// Use the same on-grid tolerance as timelineDifference when comparing clip
// geometry with a project-sample boundary. Preserve true fractional edges.
int compareTimeToSample(double time, juce::int64 sample) {
    const double difference = timelineDifference(time, seconds(sample));
    return difference < 0 ? -1 : difference > 0 ? 1 : 0;
}

} // namespace

double shiftTimelineBySamples(double time, juce::int64 delta) noexcept {
    if (delta == 0) {
        return time;
    }

    const double frame = time * sessionRate;
    const double nearest = std::round(frame);
    if (std::isfinite(frame) && frame >= 0 && nearest < 9007199254740992.0 &&
        std::abs(frame - nearest) < 1e-7) {
        return (nearest + static_cast<double>(delta)) / sessionRate;
    }

    return time + seconds(delta);
}

namespace {

constexpr juce::int64 maxPreciseSample = 9007199254740991LL;

bool validClipForSampleEdit(const Clip& clip) {
    return clip.audio && std::isfinite(clip.start) && std::isfinite(clip.offset) &&
           std::isfinite(clip.duration) && clip.start >= 0 && clip.offset >= 0 &&
           clip.duration > 0 && std::isfinite(clip.audio->length());
}

} // namespace

TimeSelection resizeTimeSelection(const TimeSelection& original, bool leftEdge, juce::int64 sample,
                                  int track) {
    if (!original.hasRange() || sample < 0 || track < 0) {
        return original;
    }

    const auto anchor = leftEdge ? original.end : original.start;
    return {std::min(anchor, sample), std::max(anchor, sample),
            std::min(original.firstTrack, track), std::max(original.lastTrack, track)};
}

bool extendSelectionTracks(TimeSelection& selection, int selectedTrack, int trackCount,
                           int direction, KeyboardTrackExtension& gesture) noexcept {
    if (!selection.hasRange() || selection.lastTrack >= trackCount ||
        (direction != -1 && direction != 1)) {
        return false;
    }

    auto next = gesture;
    if (!next.active || next.start != selection.start || next.end != selection.end ||
        next.first != selection.firstTrack || next.last != selection.lastTrack || next.anchor < 0 ||
        next.anchor >= trackCount || next.focus < 0 || next.focus >= trackCount) {
        next.active = true;
        next.start = selection.start;
        next.end = selection.end;
        if (selectedTrack == selection.firstTrack) {
            next.anchor = selection.firstTrack;
            next.focus = selection.lastTrack;
        } else if (selectedTrack == selection.lastTrack) {
            next.anchor = selection.lastTrack;
            next.focus = selection.firstTrack;
        } else {
            next.anchor = direction < 0 ? selection.lastTrack : selection.firstTrack;
            next.focus = direction < 0 ? selection.firstTrack : selection.lastTrack;
        }
    }

    if ((direction < 0 && next.focus == 0) || (direction > 0 && next.focus == trackCount - 1)) {
        return false;
    }

    next.focus += direction;
    next.first = std::min(next.anchor, next.focus);
    next.last = std::max(next.anchor, next.focus);
    selection.firstTrack = next.first;
    selection.lastTrack = next.last;
    gesture = next;
    return true;
}

bool nudgeTimeSelection(TimeSelection& selection, SelectionPart part, juce::int64 delta,
                        juce::int64 limit) noexcept {
    if (!selection.hasRange() || limit <= 0 || limit > maxPreciseSample || selection.end > limit ||
        delta == 0) {
        return false;
    }

    juce::int64 lower = 0;
    juce::int64 upper = 0;
    juce::int64 current = 0;
    switch (part) {
    case SelectionPart::start:
        current = selection.start;
        upper = selection.end - 1;
        break;
    case SelectionPart::end:
        current = selection.end;
        lower = selection.start + 1;
        upper = limit;
        break;
    case SelectionPart::whole:
        current = selection.start;
        upper = limit - (selection.end - selection.start);
        break;
    default:
        return false;
    }

    if (delta < lower - current || delta > upper - current) {
        return false;
    }

    if (part != SelectionPart::end) {
        selection.start += delta;
    }
    if (part != SelectionPart::start) {
        selection.end += delta;
    }
    return true;
}

bool extendTimeSelection(TimeSelection& selection, double& playhead, int selectedTrack,
                         int trackCount, int direction, juce::int64 limit,
                         KeyboardRangeExtension& state, bool independentPlayback) noexcept {
    if ((direction != -1 && direction != 1) || trackCount <= 0 || limit <= 0 ||
        limit > maxPreciseSample || !std::isfinite(playhead) || playhead < 0 ||
        playhead * sessionRate >= 9007199254740992.0) {
        return false;
    }

    const double cursor = playhead * sessionRate;
    auto next = state;
    const bool continues =
        next.active && next.anchor >= 0 && next.focus >= 0 && next.anchor <= limit &&
        next.focus <= limit && next.firstTrack >= 0 && next.lastTrack >= next.firstTrack &&
        next.lastTrack < trackCount && selection.start == std::min(next.anchor, next.focus) &&
        selection.end == std::max(next.anchor, next.focus) &&
        selection.firstTrack == next.firstTrack && selection.lastTrack == next.lastTrack &&
        (next.independent || std::abs(cursor - static_cast<double>(next.focus)) < 1e-7);

    if (!continues) {
        next.independent = independentPlayback || selection.hasPoint();
        if (selection.hasRange()) {
            if (selection.lastTrack >= trackCount || selection.end > limit) {
                return false;
            }
            const bool atStart = std::abs(cursor - static_cast<double>(selection.start)) < 0.5;
            next.anchor = atStart ? selection.end : selection.start;
            next.focus = atStart ? selection.start : selection.end;
            next.firstTrack = selection.firstTrack;
            next.lastTrack = selection.lastTrack;
        } else if (selection.hasPoint()) {
            if (selection.firstTrack >= trackCount || selection.start > limit) {
                return false;
            }
            next.anchor = next.focus = selection.start;
            next.firstTrack = next.lastTrack = selection.firstTrack;
        } else {
            if (selectedTrack < 0 || selectedTrack >= trackCount) {
                return false;
            }
            next.anchor = next.focus = static_cast<juce::int64>(
                std::llround(std::min(cursor, static_cast<double>(limit))));
            next.firstTrack = next.lastTrack = selectedTrack;
        }
        next.active = true;
    }

    if ((direction < 0 && next.focus == 0) || (direction > 0 && next.focus == limit)) {
        return false;
    }

    next.focus += direction;
    selection = {std::min(next.anchor, next.focus), std::max(next.anchor, next.focus),
                 next.firstTrack, next.lastTrack};
    playhead = static_cast<double>(next.focus) / sessionRate;
    state = next;
    return true;
}

bool clipSampleRange(const Clip& clip, juce::int64& start, juce::int64& end) noexcept {
    if (!validClipForSampleEdit(clip)) {
        return false;
    }

    const auto convert = [](double time, juce::int64& sample) {
        const double frames = time * sessionRate;
        if (!std::isfinite(frames) || frames < 0 || frames >= 9007199254740992.0) {
            return false;
        }
        const double nearest = std::round(frames);
        sample = static_cast<juce::int64>(std::abs(frames - nearest) < 1e-7 ? nearest
                                                                            : std::ceil(frames));
        return true;
    };

    juce::int64 first = 0;
    juce::int64 last = 0;
    if (!convert(clip.start, first) || !convert(clip.start + clip.duration, last) ||
        first >= last) {
        return false;
    }

    start = first;
    end = last;
    return true;
}

bool clipSampleRangeWithin(const Clip& clip, double begin, double end, juce::int64& start,
                           juce::int64& finish) noexcept {
    if (!std::isfinite(begin) || !std::isfinite(end) || begin < 0 || end <= begin ||
        end * sessionRate >= 9007199254740992.0) {
        return false;
    }

    juce::int64 first = 0;
    juce::int64 last = 0;
    if (!clipSampleRange(clip, first, last)) {
        return false;
    }

    const auto boundary = [](double time) {
        const double value = time * sessionRate;
        const double nearest = std::round(value);
        return static_cast<juce::int64>(std::abs(value - nearest) < 1.0e-7 ? nearest
                                                                           : std::ceil(value));
    };

    first = std::max(first, boundary(begin));
    last = std::min(last, boundary(end));
    if (first >= last) {
        return false;
    }

    start = first;
    finish = last;
    return true;
}

bool snapSelectionToZeroCrossings(const Session& session, int channels,
                                  const TimeSelection& selection, juce::int64 radius,
                                  TimeSelection& result, juce::String& error, LoadProgress progress,
                                  bool requireAllChannels) {
    error.clear();
    const auto masterEnd = projectEndSample(session);
    if (!validSelection(session, selection) || selection.end > masterEnd ||
        (channels != 1 && channels != 2) || radius < 1 || radius > 44100) {
        error = "Select a range within the master and valid tracks.";
        return false;
    }

    const auto keepGoing = [&](double value) {
        if (!progress || progress(value)) {
            return true;
        }
        error = "Zero-crossing search cancelled.";
        return false;
    };
    if (!keepGoing(0)) {
        return false;
    }

    ResamplingContext context;
    session.prepareResampling(context, sessionRate);

    auto findBoundary = [&](juce::int64 centre, int which, juce::int64& target) {
        if (centre == 0 || centre == masterEnd) {
            target = centre;
            return keepGoing((which + 1) * 0.5);
        }

        const auto first = std::max<juce::int64>(1, centre - radius);
        const auto last = std::min(masterEnd - 1, centre + radius);
        if (first > last) {
            error = "No samples available around a selection boundary.";
            return false;
        }

        juce::String validation;
        if (!validateMixRange(session, channels, {first - 1, last + 1}, validation)) {
            error = validation;
            return false;
        }

        const int frames = static_cast<int>(last - first + 2);
        juce::AudioBuffer<float> buffer(channels, frames);
        buffer.clear();
        for (int offset = 0; offset < frames; offset += 256) {
            if (!keepGoing((which + 0.5 * static_cast<double>(offset) / frames) * 0.5)) {
                return false;
            }
            const int amount = std::min(256, frames - offset);
            session.render(buffer, offset, amount,
                           static_cast<double>(first - 1 + offset) / sessionRate, sessionRate,
                           channels, &context);
        }

        bool found = false;
        float best = 0;
        juce::int64 bestDistance = 0;
        juce::int64 bestSample = 0;

        for (juce::int64 sample = first; sample <= last; ++sample) {
            const int index = static_cast<int>(sample - first);
            float score = 0;
            bool allSilent = true;
            bool crossing = false;
            bool everyChannelCrosses = true;

            for (int ch = 0; ch < channels; ++ch) {
                const float before = buffer.getSample(ch, index);
                const float after = buffer.getSample(ch, index + 1);
                if (!std::isfinite(before) || !std::isfinite(after)) {
                    error = "Mix contains a non-finite sample.";
                    return false;
                }

                const bool beforeZero = std::fpclassify(before) == FP_ZERO;
                const bool afterZero = std::fpclassify(after) == FP_ZERO;
                const bool channelCrosses =
                    beforeZero || afterZero || std::signbit(before) != std::signbit(after);
                everyChannelCrosses = everyChannelCrosses && channelCrosses;
                if (!beforeZero || !afterZero) {
                    allSilent = false;
                    if (channelCrosses) {
                        crossing = true;
                    }
                }
                score = std::max(score, std::max(std::abs(before), std::abs(after)));
            }

            if (requireAllChannels ? !everyChannelCrosses : (!crossing && !allSilent)) {
                continue;
            }

            const auto distance = std::abs(sample - centre);
            if (!found || score < best ||
                (!(best < score) &&
                 (distance < bestDistance || (distance == bestDistance && sample < bestSample)))) {
                found = true;
                best = score;
                bestDistance = distance;
                bestSample = sample;
            }
        }

        if (!found) {
            error = juce::String(requireAllChannels
                                     ? "No common output-channel zero crossing near selection "
                                     : "No zero crossing near selection ") +
                    (which == 0 ? "start." : "end.");
            return false;
        }

        target = bestSample;
        return keepGoing((which + 1) * 0.5);
    };

    TimeSelection next = selection;
    if (!findBoundary(selection.start, 0, next.start) ||
        !findBoundary(selection.end, 1, next.end)) {
        return false;
    }
    if (!next.hasRange()) {
        error = "Zero crossings would collapse or reverse the selection.";
        return false;
    }

    result = next;
    return true;
}
std::vector<OverlapRegion> overlapRegions(const Session& session) {
    std::vector<OverlapRegion> result;
    const auto masterEnd = projectEndSample(session);
    if (masterEnd <= 0 || masterEnd >= 9007199254740992LL) {
        return result;
    }

    const double end = static_cast<double>(masterEnd) / sessionRate;
    const auto boundary = [](double time) {
        const double value = time * sessionRate;
        const double nearest = std::round(value);
        return static_cast<juce::int64>(std::abs(value - nearest) < 1.0e-7 ? nearest
                                                                           : std::ceil(value));
    };

    for (int track = 0; track < static_cast<int>(session.tracks.size()); ++track) {
        for (const auto& overlap :
             clipOverlapRanges(session.tracks[static_cast<size_t>(track)], 0, end)) {
            const auto first = boundary(overlap.getStart());
            const auto last = std::min(masterEnd, boundary(overlap.getEnd()));
            if (first < last) {
                result.push_back({track, first, last});
            }
        }
    }

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return std::tie(a.start, a.track, a.end) < std::tie(b.start, b.track, b.end);
    });
    return result;
}

bool adjacentClipBoundary(const Session& session, int track, double position, bool next,
                          juce::int64& sample) noexcept {
    if (track < -1 || track >= static_cast<int>(session.tracks.size()) ||
        !std::isfinite(position) || position < 0 || position * sessionRate >= 9007199254740992.0) {
        return false;
    }

    const double current = position * sessionRate;
    juce::int64 best = next ? maxPreciseSample : 0;
    bool found = false;

    for (int t = 0; t < static_cast<int>(session.tracks.size()); ++t) {
        if (track >= 0 && track != t) {
            continue;
        }
        for (const auto& clip : session.tracks[static_cast<size_t>(t)].clips) {
            juce::int64 first = 0;
            juce::int64 last = 0;
            if (!clipSampleRange(clip, first, last)) {
                continue;
            }
            for (auto value : {first, last}) {
                const double difference = static_cast<double>(value) - current;
                if ((next ? difference > 1e-7 : difference < -1e-7) &&
                    (!found || (next ? value < best : value > best))) {
                    best = value;
                    found = true;
                }
            }
        }
    }

    if (found) {
        sample = best;
    }
    return found;
}

bool adjacentClipIndex(const Track& track, int current, bool next, int& result) noexcept {
    const auto size = static_cast<int>(track.clips.size());
    if (current < -1 || current >= size) {
        return false;
    }

    const auto valid = [&](int index) {
        const auto& clip = track.clips[static_cast<size_t>(index)];
        return clip.audio && std::isfinite(clip.start) && clip.start >= 0 &&
               std::isfinite(clip.duration) && clip.duration > 0 &&
               std::isfinite(clip.start + clip.duration);
    };
    if (current >= 0 && !valid(current)) {
        return false;
    }

    const auto key = [&](int index) {
        const auto& c = track.clips[static_cast<size_t>(index)];
        return std::make_tuple(c.start, c.start + c.duration, index);
    };

    int best = -1;
    for (int i = 0; i < size; ++i) {
        if (!valid(i) ||
            (current >= 0 && (next ? key(i) <= key(current) : key(i) >= key(current)))) {
            continue;
        }
        if (best < 0 || (next ? key(i) < key(best) : key(i) > key(best))) {
            best = i;
        }
    }

    if (best < 0) {
        return false;
    }

    result = best;
    return true;
}

std::vector<double> timelineSnapTargets(const Session& session, const TimeSelection& selection,
                                        double playhead, int excludedTrack, int excludedClip,
                                        bool includeMarkers, bool includeSelection,
                                        bool projectSampleTargets, SnapTargetOptions options) {
    std::vector<double> targets;
    const auto add = [&](double value) {
        if (!std::isfinite(value) || value < 0 || value * sessionRate >= 9007199254740992.0) {
            return;
        }
        if (projectSampleTargets) {
            value = std::round(value * sessionRate) / sessionRate;
        }
        targets.push_back(value);
    };

    if (options.timelineStart) {
        add(0);
    }
    if (options.playhead) {
        add(playhead);
    }
    if (options.selection && includeSelection && selection.hasRange()) {
        add(seconds(selection.start));
        add(seconds(selection.end));
    }
    if (options.markers && includeMarkers) {
        for (const auto& marker : session.markers) {
            add(marker.position);
        }
        for (const auto& mdTrack : session.mdDisc.tracks) {
            for (const auto& part : mdTrack.parts) {
                add(seconds(part.startSample));
                add(seconds(part.endSample));
            }
        }
    }
    if (options.clipEdges) {
        for (int track = 0; track < static_cast<int>(session.tracks.size()); ++track) {
            const auto& clips = session.tracks[static_cast<size_t>(track)].clips;
            for (int index = 0; index < static_cast<int>(clips.size()); ++index) {
                if (track != excludedTrack || index != excludedClip) {
                    const auto& clip = clips[static_cast<size_t>(index)];
                    add(clip.start);
                    add(clip.start + clip.duration);
                }
            }
        }
    }

    return targets;
}

SnapResult snapTimelinePosition(double proposed, double minimum, double maximum, bool sampleGrid,
                                double tolerance, const std::vector<double>& targets,
                                double trailingOffset, juce::int64 gridSamples) {
    if (!std::isfinite(proposed) || !std::isfinite(minimum) || !std::isfinite(maximum) ||
        minimum > maximum) {
        return {};
    }

    proposed = std::clamp(proposed, minimum, maximum);
    SnapResult result{proposed, 0, false};
    double best = std::max(0.0, tolerance);

    for (const auto target : targets) {
        if (!std::isfinite(target)) {
            continue;
        }
        for (const double edgeOffset : {0.0, trailingOffset}) {
            const double candidate = target - edgeOffset;
            if (candidate < minimum || candidate > maximum) {
                continue;
            }
            const double distance = std::abs(candidate - proposed);
            if (distance <= best && (!result.hasGuide || distance < best)) {
                result = {candidate, target, true};
                best = distance;
            }
        }
    }

    if (result.hasGuide || !sampleGrid || gridSamples <= 0 || gridSamples > maxPreciseSample) {
        return result;
    }

    const double grid = static_cast<double>(gridSamples);
    const double first = std::ceil((minimum * sessionRate - 1e-9) / grid);
    const double last = std::floor((maximum * sessionRate + 1e-9) / grid);
    if (first <= last && first >= 0 && last * grid < 9007199254740992.0) {
        const double frame =
            std::clamp(std::round(proposed * sessionRate / grid), first, last) * grid;
        result.time = frame / sessionRate;
        if (gridSamples > 1) {
            result.hasGuide = true;
            result.guide = result.time;
        }
    }

    return result;
}

namespace {

juce::uint64 clipReferenceKey(ClipReference ref) noexcept {
    return (static_cast<juce::uint64>(static_cast<unsigned>(ref.track)) << 32) |
           static_cast<unsigned>(ref.clip);
}

} // namespace

bool expandClipGroups(const Session& session, const std::vector<ClipReference>& input,
                      std::vector<ClipReference>& output, juce::String& error) {
    error.clear();
    if (input.empty()) {
        error = "Select clips first.";
        return false;
    }

    std::unordered_set<juce::uint64> selected;
    std::set<juce::String> groups;
    for (const auto& ref : input) {
        if (ref.track < 0 || ref.track >= static_cast<int>(session.tracks.size()) || ref.clip < 0 ||
            ref.clip >=
                static_cast<int>(session.tracks[static_cast<size_t>(ref.track)].clips.size()) ||
            !selected.insert(clipReferenceKey(ref)).second) {
            error = "Invalid or duplicate clip reference.";
            return false;
        }

        const auto& id = session.tracks[static_cast<size_t>(ref.track)]
                             .clips[static_cast<size_t>(ref.clip)]
                             .groupId;
        if (!validClipGroupId(id)) {
            error = "Invalid clip group identity.";
            return false;
        }
        if (id.isNotEmpty()) {
            groups.insert(id);
        }
    }

    if (groups.empty()) {
        auto result = input;
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            return std::tie(a.track, a.clip) < std::tie(b.track, b.clip);
        });
        output = std::move(result);
        return true;
    }

    std::vector<ClipReference> result;
    for (size_t track = 0; track < session.tracks.size(); ++track) {
        for (size_t clip = 0; clip < session.tracks[track].clips.size(); ++clip) {
            const ClipReference ref{static_cast<int>(track), static_cast<int>(clip)};
            const auto& id = session.tracks[track].clips[clip].groupId;
            if (selected.count(clipReferenceKey(ref)) || (id.isNotEmpty() && groups.count(id))) {
                result.push_back(ref);
            }
        }
    }

    output = std::move(result);
    return true;
}

bool groupClips(Session& session, const std::vector<ClipReference>& input, juce::String& error) {
    std::vector<ClipReference> members;
    if (!expandClipGroups(session, input, members, error)) {
        return false;
    }
    if (members.size() < 2) {
        error = "Select at least two clips to group.";
        return false;
    }

    const auto& first = session.tracks[static_cast<size_t>(members[0].track)]
                            .clips[static_cast<size_t>(members[0].clip)]
                            .groupId;
    const bool sameGroup =
        first.isNotEmpty() && std::all_of(members.begin(), members.end(), [&](const auto& ref) {
            return session.tracks[static_cast<size_t>(ref.track)]
                       .clips[static_cast<size_t>(ref.clip)]
                       .groupId == first;
        });
    if (sameGroup) {
        error = "Clips already belong to the same group.";
        return false;
    }

    std::set<juce::String> used;
    for (const auto& track : session.tracks) {
        for (const auto& clip : track.clips) {
            if (clip.groupId.isNotEmpty()) {
                used.insert(clip.groupId);
            }
        }
    }

    juce::String id;
    do {
        id = juce::Uuid().toString().removeCharacters("-").toLowerCase();
    } while (used.count(id));
    if (!validClipGroupId(id) || id.isEmpty()) {
        error = "Could not create a clip group identity.";
        return false;
    }

    for (const auto& ref : members) {
        session.tracks[static_cast<size_t>(ref.track)]
            .clips[static_cast<size_t>(ref.clip)]
            .groupId = id;
    }
    return true;
}

bool ungroupClips(Session& session, const std::vector<ClipReference>& input, juce::String& error) {
    std::vector<ClipReference> members;
    if (!expandClipGroups(session, input, members, error)) {
        return false;
    }

    const bool grouped = std::any_of(members.begin(), members.end(), [&](const auto& ref) {
        return session.tracks[static_cast<size_t>(ref.track)]
            .clips[static_cast<size_t>(ref.clip)]
            .groupId.isNotEmpty();
    });
    if (!grouped) {
        error = "No selected clips are grouped.";
        return false;
    }

    for (const auto& ref : members) {
        session.tracks[static_cast<size_t>(ref.track)]
            .clips[static_cast<size_t>(ref.clip)]
            .groupId.clear();
    }
    return true;
}

bool forkCopiedClipGroups(const Session& session, const std::vector<Clip*>& copies,
                          juce::String& error) {
    error.clear();
    std::set<const Clip*> unique;
    std::map<juce::String, size_t> counts;
    std::set<juce::String> used;

    for (const auto* clip : copies) {
        if (clip == nullptr || !unique.insert(clip).second || !validClipGroupId(clip->groupId)) {
            error = "Invalid copied clip group identity.";
            return false;
        }
        if (clip->groupId.isNotEmpty()) {
            ++counts[clip->groupId];
            used.insert(clip->groupId);
        }
    }

    if (counts.empty()) {
        return true;
    }

    for (const auto& track : session.tracks) {
        for (const auto& clip : track.clips) {
            if (clip.groupId.isNotEmpty()) {
                used.insert(clip.groupId);
            }
        }
    }

    std::map<juce::String, juce::String> replacements;
    for (const auto& [id, count] : counts) {
        juce::String replacement;
        if (count > 1) {
            do {
                replacement = juce::Uuid().toString().removeCharacters("-").toLowerCase();
            } while (used.count(replacement));
            if (replacement.isEmpty() || !validClipGroupId(replacement)) {
                error = "Could not create a clip group identity.";
                return false;
            }
            used.insert(replacement);
        }
        replacements.emplace(id, replacement);
    }

    for (auto* clip : copies) {
        if (clip->groupId.isNotEmpty()) {
            clip->groupId = replacements.at(clip->groupId);
        }
    }
    return true;
}

bool ClipSelection::toggle(const Session& session, ClipReference ref, juce::uint64 generation) {
    if (ref.track < 0 || ref.track >= static_cast<int>(session.tracks.size()) || ref.clip < 0 ||
        ref.clip >= static_cast<int>(session.tracks[static_cast<size_t>(ref.track)].clips.size())) {
        return false;
    }

    if (generation != revision) {
        clear();
    }
    revision = generation;

    const auto found = std::find_if(refs.begin(), refs.end(), [&](const auto& value) {
        return value.track == ref.track && value.clip == ref.clip;
    });
    if (found == refs.end()) {
        refs.push_back(ref);
        keys.insert(clipReferenceKey(ref));
    } else {
        refs.erase(found);
        keys.erase(clipReferenceKey(ref));
    }

    return true;
}

bool ClipSelection::selectExpanded(const Session& session,
                                   const std::vector<ClipReference>& selected,
                                   juce::uint64 generation) {
    std::vector<ClipReference> expanded;
    juce::String error;
    return expandClipGroups(session, selected, expanded, error) &&
           restore(session, expanded, generation);
}

bool ClipSelection::toggleGroup(const Session& session, ClipReference ref,
                                juce::uint64 generation) {
    std::vector<ClipReference> members;
    juce::String error;
    if (!expandClipGroups(session, {ref}, members, error)) {
        return false;
    }

    const auto& current = get(generation);
    std::vector<ClipReference> next = current;
    std::unordered_set<juce::uint64> memberKeys;
    for (const auto& member : members) {
        memberKeys.insert(clipReferenceKey(member));
    }

    const bool allSelected = std::all_of(members.begin(), members.end(), [&](const auto& member) {
        return contains(member.track, member.clip, generation);
    });
    if (allSelected) {
        next.erase(std::remove_if(next.begin(), next.end(),
                                  [&](const auto& member) {
                                      return memberKeys.count(clipReferenceKey(member)) != 0;
                                  }),
                   next.end());
    } else {
        for (const auto& member : members) {
            if (!contains(member.track, member.clip, generation)) {
                next.push_back(member);
            }
        }
    }

    return restore(session, next, generation);
}

bool ClipSelection::restore(const Session& session, const std::vector<ClipReference>& selected,
                            juce::uint64 generation) {
    std::unordered_set<juce::uint64> nextKeys;
    nextKeys.reserve(selected.size());

    for (const auto& ref : selected) {
        if (ref.track < 0 || ref.track >= static_cast<int>(session.tracks.size()) || ref.clip < 0 ||
            ref.clip >=
                static_cast<int>(session.tracks[static_cast<size_t>(ref.track)].clips.size()) ||
            !nextKeys.insert(clipReferenceKey(ref)).second) {
            return false;
        }
    }

    refs = selected;
    keys = std::move(nextKeys);
    revision = generation;
    return true;
}

bool ClipSelection::selectTracks(const Session& session, TrackRange tracks,
                                 const TimeSelection* range, juce::uint64 generation) {
    if (tracks.first < 0 || tracks.last < tracks.first ||
        tracks.last >= static_cast<int>(session.tracks.size()) || (range && !range->hasRange())) {
        return false;
    }

    std::vector<int> indices;
    indices.reserve(static_cast<size_t>(tracks.last - tracks.first + 1));
    for (int track = tracks.first; track <= tracks.last; ++track) {
        indices.push_back(track);
    }

    return selectTrackIndices(session, indices, range, generation);
}

bool ClipSelection::selectTrackIndices(const Session& session, const std::vector<int>& tracks,
                                       const TimeSelection* range, juce::uint64 generation) {
    if (tracks.empty() || (range && !range->hasRange())) {
        return false;
    }

    std::set<int> chosen;
    for (int track : tracks) {
        if (track < 0 || track >= static_cast<int>(session.tracks.size()) ||
            !chosen.insert(track).second) {
            return false;
        }
    }

    std::vector<ClipReference> next;
    for (int t : chosen) {
        const auto& clips = session.tracks[static_cast<size_t>(t)].clips;
        for (size_t i = 0; i < clips.size(); ++i) {
            const auto& clip = clips[i];
            if (range && (compareTimeToSample(clip.start, range->end) >= 0 ||
                          compareTimeToSample(clip.start + clip.duration, range->start) <= 0)) {
                continue;
            }
            next.push_back(ClipReference{t, static_cast<int>(i)});
        }
    }

    if (next.empty()) {
        return false;
    }

    return selectExpanded(session, next, generation);
}

bool ClipSelection::selectSourceOnTracks(const Session& session,
                                         const std::shared_ptr<Audio>& source,
                                         const std::vector<int>& tracks, const TimeSelection* range,
                                         juce::uint64 generation) {
    if (!source || tracks.empty() || (range && !range->hasRange())) {
        return false;
    }

    std::set<int> rows;
    for (int track : tracks) {
        if (track < 0 || track >= static_cast<int>(session.tracks.size()) ||
            !rows.insert(track).second) {
            return false;
        }
    }

    std::vector<ClipReference> matches;
    for (int track : rows) {
        const auto& clips = session.tracks[static_cast<size_t>(track)].clips;
        for (size_t index = 0; index < clips.size(); ++index) {
            const auto& clip = clips[index];
            const auto& candidate = clip.audio;
            if (!candidate || !(candidate == source ||
                                (source->file != juce::File{} && candidate->file == source->file &&
                                 source->sourceHash.isNotEmpty() &&
                                 candidate->sourceHash == source->sourceHash))) {
                continue;
            }
            if (range && (compareTimeToSample(clip.start, range->end) >= 0 ||
                          compareTimeToSample(clip.start + clip.duration, range->start) <= 0)) {
                continue;
            }
            matches.push_back({track, static_cast<int>(index)});
        }
    }

    return !matches.empty() && selectExpanded(session, matches, generation);
}

const std::vector<ClipReference>& ClipSelection::get(juce::uint64 generation) const noexcept {
    static const std::vector<ClipReference> empty;
    return revision == generation ? refs : empty;
}

bool ClipSelection::contains(int track, int clip, juce::uint64 generation) const noexcept {
    return generation == revision && track >= 0 && clip >= 0 &&
           keys.count(clipReferenceKey({track, clip})) != 0;
}

bool ClipSelection::retainAfterStableEdit(juce::uint64 previous, juce::uint64 current) noexcept {
    if (revision != previous || refs.empty() || previous == current) {
        return false;
    }
    revision = current;
    return true;
}

bool activateTimeSelection(TimeSelection& current, ClipSelection& clips, const TimeSelection& next,
                           int trackCount) noexcept {
    if (!next.hasRange() || next.lastTrack >= trackCount) {
        return false;
    }

    current = next;
    clips.clear();
    return true;
}

juce::String describeClipSelection(const ClipSelection& selection, juce::uint64 generation,
                                   int focusedTrack, int focusedClip) {
    const auto& selected = selection.get(generation);
    if (selected.empty()) {
        return "No clip group selected.";
    }

    auto text = juce::String(selected.size()) +
                (selected.size() == 1 ? " clip selected." : " clips selected.");
    if (focusedTrack >= 0 && focusedClip >= 0) {
        text += selection.contains(focusedTrack, focusedClip, generation)
                    ? " Focused clip is selected."
                    : " Focused clip is not selected.";
    } else {
        text += " No clip focused.";
    }
    return text;
}

ClipGroupShortcut clipGroupShortcut(int keyCode, bool isDeleteKey, bool plain, bool plainCommand,
                                    bool hasSelection) noexcept {
    if (!hasSelection) {
        return ClipGroupShortcut::none;
    }
    if (plain && isDeleteKey) {
        return ClipGroupShortcut::remove;
    }
    if (!plainCommand) {
        return ClipGroupShortcut::none;
    }

    switch (juce::CharacterFunctions::toLowerCase(static_cast<juce::juce_wchar>(keyCode))) {
    case 'c':
        return ClipGroupShortcut::copy;
    case 'x':
        return ClipGroupShortcut::cut;
    case 'd':
        return ClipGroupShortcut::duplicate;
    case 'b':
        return ClipGroupShortcut::split;
    default:
        return ClipGroupShortcut::none;
    }
}

bool validateClipGroupMove(const Session& session, const std::vector<ClipReference>& selected,
                           juce::int64 sampleDelta, juce::int64 trackDelta, juce::String& error) {
    error.clear();
    const auto count = static_cast<juce::int64>(session.tracks.size());
    if (selected.empty()) {
        error = "Select at least one clip.";
        return false;
    }
    if (sampleDelta < -maxPreciseSample || sampleDelta > maxPreciseSample) {
        error = "The sample offset is outside the precise timeline range.";
        return false;
    }

    std::set<std::pair<int, int>> seen;
    for (const auto& ref : selected) {
        if (ref.track < 0 || ref.track >= count || ref.clip < 0 ||
            ref.clip >=
                static_cast<int>(session.tracks[static_cast<size_t>(ref.track)].clips.size()) ||
            !seen.emplace(ref.track, ref.clip).second) {
            error = "The clip selection is invalid or contains duplicates.";
            return false;
        }
        if (trackDelta < -static_cast<juce::int64>(ref.track) || trackDelta >= count - ref.track) {
            error = "The move requires destination tracks that do not exist.";
            return false;
        }

        const auto& clip =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        if (!validClipForSampleEdit(clip) ||
            !validTimelineGeometry(shiftTimelineBySamples(clip.start, sampleDelta), clip.offset,
                                   clip.duration)) {
            error = "A clip would move outside the precise timeline range.";
            return false;
        }
    }

    return true;
}

bool slipClipGroup(Session& session, const std::vector<ClipReference>& selected,
                   juce::int64 samples, juce::String& error) {
    std::vector<ClipReference> clips;
    if (!expandClipGroups(session, selected, clips, error) ||
        !validateClipGroupMove(session, clips, 0, 0, error)) {
        return false;
    }
    if (samples < -maxPreciseSample || samples > maxPreciseSample) {
        error = "The sample offset is outside the precise range.";
        return false;
    }
    if (samples == 0) {
        error = "Enter a nonzero sample offset.";
        return false;
    }

    std::vector<Clip> result;
    result.reserve(clips.size());
    const double shift = static_cast<double>(samples) / sessionRate;

    for (const auto& ref : clips) {
        auto clip =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];

        // Only canonical grid values are snapped: a nearly integral source
        // position can still encode a deliberate fractional source phase.
        const double frame = clip.offset * sessionRate;
        const double nearest = std::round(frame);
        const bool onGrid = std::isfinite(frame) && nearest >= 0 && nearest < 9007199254740992.0 &&
                            std::equal_to<double>{}(clip.offset, nearest / sessionRate);
        double offset =
            onGrid ? (nearest + static_cast<double>(samples)) / sessionRate : clip.offset + shift;

        const double sourceLength = clip.audio->length();
        const double tolerance =
            std::min(1.0e-4 / sessionRate,
                     8.0 * std::numeric_limits<double>::epsilon() *
                         std::max({1.0, std::abs(offset), clip.duration, sourceLength}));
        if (offset < 0 && offset >= -tolerance) {
            offset = 0;
        }
        if (!std::isfinite(offset) || offset < 0 ||
            offset + clip.duration > sourceLength + tolerance) {
            error = "A clip would extend beyond its source audio.";
            return false;
        }

        const double actual = offset - clip.offset;
        if (std::abs((actual - shift) * sessionRate) > 1e-7) {
            error = "The source offset cannot be represented precisely.";
            return false;
        }

        clip.offset = offset;
        if (clip.fadeIn > 0) {
            clip.fadeInStart += actual;
        }
        if (clip.fadeOut > 0) {
            clip.fadeOutEnd += actual;
        }
        if (clip.fadeInStart < 0 && clip.fadeInStart >= -tolerance) {
            clip.fadeInStart = 0;
        }
        if (clip.fadeOutEnd < 0 && clip.fadeOutEnd >= -tolerance) {
            clip.fadeOutEnd = 0;
        }
        if (clip.fadeInStart < 0 || clip.fadeOutEnd < 0 ||
            clip.fadeInStart + clip.fadeIn > sourceLength + tolerance ||
            clip.fadeOutEnd > sourceLength + tolerance ||
            clip.fadeOut > clip.fadeOutEnd + tolerance) {
            error = "A clip fade would extend beyond its source audio.";
            return false;
        }

        result.push_back(std::move(clip));
    }

    for (size_t i = 0; i < clips.size(); ++i) {
        session.tracks[static_cast<size_t>(clips[i].track)]
            .clips[static_cast<size_t>(clips[i].clip)] = std::move(result[i]);
    }
    return true;
}

bool ClipGroupMovePreview::prepare(const Session& session,
                                   const std::vector<ClipReference>& selected,
                                   juce::String& error) {
    if (!validateClipGroupMove(session, selected, 0, 0, error)) {
        return false;
    }

    std::vector<Geometry> prepared;
    prepared.reserve(selected.size());
    for (const auto& ref : selected) {
        const auto& clip =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        prepared.push_back({ref.track, clip.start, clip.offset, clip.duration});
    }

    clips = std::move(prepared);
    trackCount = static_cast<juce::int64>(session.tracks.size());
    return true;
}

bool ClipGroupMovePreview::validate(juce::int64 sampleDelta, juce::int64 trackDelta,
                                    juce::String& error) const {
    error.clear();
    if (clips.empty()) {
        error = "Select at least one clip.";
        return false;
    }
    if (sampleDelta < -maxPreciseSample || sampleDelta > maxPreciseSample) {
        error = "The sample offset is outside the precise timeline range.";
        return false;
    }

    for (const auto& clip : clips) {
        if (trackDelta < -static_cast<juce::int64>(clip.track) ||
            trackDelta >= trackCount - clip.track) {
            error = "The move requires destination tracks that do not exist.";
            return false;
        }
        if (!validTimelineGeometry(shiftTimelineBySamples(clip.start, sampleDelta), clip.offset,
                                   clip.duration)) {
            error = "A clip would move outside the precise timeline range.";
            return false;
        }
    }

    return true;
}

bool ClipOverlapPreview::prepare(const Session& session, const std::vector<ClipReference>& selected,
                                 juce::String& error) {
    if (!validateClipGroupMove(session, selected, 0, 0, error)) {
        return false;
    }

    std::set<std::pair<int, int>> excluded;
    std::vector<Moving> nextMoving;
    nextMoving.reserve(selected.size());
    for (const auto& ref : selected) {
        excluded.emplace(ref.track, ref.clip);
        const auto& clip =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        nextMoving.push_back({ref.track, clip.start, clip.duration});
    }

    std::vector<std::vector<Interval>> nextStationary(session.tracks.size());
    for (size_t track = 0; track < session.tracks.size(); ++track) {
        auto& ranges = nextStationary[track];
        const auto& clips = session.tracks[track].clips;
        for (size_t index = 0; index < clips.size(); ++index) {
            if (excluded.count({static_cast<int>(track), static_cast<int>(index)})) {
                continue;
            }
            const auto& clip = clips[index];
            const double start = canonicalProjectTime(clip.start);
            const double end = canonicalProjectTime(clip.start + clip.duration);
            if (std::isfinite(start) && std::isfinite(end) && clip.duration > 0 && end > start) {
                ranges.push_back({start, end});
            }
        }

        std::sort(ranges.begin(), ranges.end(), [](const Interval& a, const Interval& b) {
            return std::tie(a.start, a.end) < std::tie(b.start, b.end);
        });

        size_t merged = 0;
        for (const auto interval : ranges) {
            if (merged > 0 && interval.start <= ranges[merged - 1].end) {
                ranges[merged - 1].end = std::max(ranges[merged - 1].end, interval.end);
            } else {
                ranges[merged++] = interval;
            }
        }
        ranges.resize(merged);
    }

    moving = std::move(nextMoving);
    stationary = std::move(nextStationary);
    return true;
}

bool ClipOverlapPreview::intersects(int track, double start, double duration) const noexcept {
    if (track < 0 || static_cast<size_t>(track) >= stationary.size() || !std::isfinite(start) ||
        !std::isfinite(duration) || duration <= 0) {
        return false;
    }

    const double end = canonicalProjectTime(start + duration);
    start = canonicalProjectTime(start);
    if (!std::isfinite(end) || end <= start) {
        return false;
    }

    const auto& ranges = stationary[static_cast<size_t>(track)];
    const auto found =
        std::lower_bound(ranges.begin(), ranges.end(), start,
                         [](const Interval& range, double value) { return range.end <= value; });
    return found != ranges.end() && found->start < end;
}

bool ClipOverlapPreview::overlaps(juce::int64 sampleDelta, juce::int64 trackDelta) const noexcept {
    for (const auto& clip : moving) {
        if (trackDelta < -static_cast<juce::int64>(clip.track) ||
            trackDelta >= static_cast<juce::int64>(stationary.size()) - clip.track) {
            continue;
        }
        if (intersects(static_cast<int>(clip.track + trackDelta),
                       shiftTimelineBySamples(clip.start, sampleDelta), clip.duration)) {
            return true;
        }
    }
    return false;
}

bool ClipOverlapPreview::overlapsSingle(double start, int destinationTrack) const noexcept {
    return moving.size() == 1 && intersects(destinationTrack, start, moving.front().duration);
}

std::vector<double> clipGroupSnapTargets(const Session& session, const TimeSelection& selection,
                                         double playhead,
                                         const std::vector<ClipReference>& excluded,
                                         SnapTargetOptions options) {
    std::set<std::pair<int, int>> skip;
    for (const auto& ref : excluded) {
        skip.emplace(ref.track, ref.clip);
    }

    auto targets =
        timelineSnapTargets(Session{}, selection, playhead, -1, -1, true, true, false, options);

    if (options.markers) {
        for (const auto& marker : session.markers) {
            if (std::isfinite(marker.position) && marker.position >= 0 &&
                marker.position * sessionRate < 9007199254740992.0) {
                targets.push_back(marker.position);
            }
        }
    }

    if (options.clipEdges) {
        for (int t = 0; t < static_cast<int>(session.tracks.size()); ++t) {
            for (int c = 0;
                 c < static_cast<int>(session.tracks[static_cast<size_t>(t)].clips.size()); ++c) {
                if (skip.count({t, c})) {
                    continue;
                }
                const auto& clip =
                    session.tracks[static_cast<size_t>(t)].clips[static_cast<size_t>(c)];
                for (const auto edge : {clip.start, clip.start + clip.duration}) {
                    if (std::isfinite(edge) && edge >= 0 &&
                        edge * sessionRate < 9007199254740992.0) {
                        targets.push_back(edge);
                    }
                }
            }
        }
    }

    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return targets;
}

GroupSnapResult snapClipGroupMove(double anchorStart, double anchorDuration,
                                  juce::int64 requestedDelta, double tolerance,
                                  const std::vector<double>& targets) noexcept {
    GroupSnapResult result{requestedDelta, 0, false};
    if (!std::isfinite(anchorStart) || !std::isfinite(anchorDuration) || anchorDuration <= 0 ||
        !std::isfinite(tolerance) || tolerance < 0) {
        return result;
    }

    double best = tolerance;
    const auto consider = [&](double target, double edge) {
        const double needed = (target - anchorStart - edge) * sessionRate;
        if (!std::isfinite(needed) || std::abs(needed) >= 9007199254740992.0) {
            return false;
        }
        const auto candidate = static_cast<juce::int64>(std::llround(needed));
        const double distance = std::abs(
            (anchorStart + edge + static_cast<double>(requestedDelta) / sessionRate) - target);
        const double residual =
            std::abs((anchorStart + edge + static_cast<double>(candidate) / sessionRate) - target);
        if (residual > 0.5 / sessionRate + 1e-10) {
            return false;
        }
        if (distance <= best) {
            best = distance;
            result = {candidate, target, true};
        }
        return true;
    };

    for (const double edge : {0.0, anchorDuration}) {
        const auto sought = anchorStart + edge + static_cast<double>(requestedDelta) / sessionRate;
        const auto middle = std::lower_bound(targets.begin(), targets.end(), sought);
        for (auto next = middle; next != targets.end() && *next - sought <= best; ++next) {
            if (consider(*next, edge)) {
                break;
            }
        }
        for (auto previous = middle; previous != targets.begin();) {
            --previous;
            if (sought - *previous > best) {
                break;
            }
            if (consider(*previous, edge)) {
                break;
            }
        }
    }

    return result;
}

bool snapClipGroupToGrid(double anchorStart, juce::int64 requestedDelta, juce::int64 gridSamples,
                         juce::int64& result) noexcept {
    if (!std::isfinite(anchorStart) || anchorStart < 0 || gridSamples <= 0 ||
        gridSamples > maxPreciseSample || requestedDelta < -maxPreciseSample ||
        requestedDelta > maxPreciseSample) {
        return false;
    }

    const double anchor = anchorStart * sessionRate;
    const double grid = static_cast<double>(gridSamples);
    const double proposed = anchor + static_cast<double>(requestedDelta);
    if (!std::isfinite(anchor) || anchor >= 9007199254740992.0 || proposed < 0 ||
        proposed >= 9007199254740992.0) {
        return false;
    }

    const double target = std::round(proposed / grid) * grid;
    const double delta = target - anchor;
    if (target < 0 || target >= 9007199254740992.0 || std::abs(delta) >= 9007199254740992.0) {
        return false;
    }

    result = static_cast<juce::int64>(std::llround(delta));
    return true;
}

bool prepareClipGroupMove(const Session& session, Session& output,
                          const std::vector<ClipReference>& selected, juce::int64 sampleDelta,
                          juce::int64 trackDelta, juce::String& error,
                          std::vector<ClipReference>* movedRefs) {
    error.clear();
    if (selected.empty()) {
        error = "Select at least one clip.";
        return false;
    }
    if (sampleDelta == 0 && trackDelta == 0) {
        error = "The clip positions are unchanged.";
        return false;
    }
    if (!validateClipGroupMove(session, selected, sampleDelta, trackDelta, error)) {
        return false;
    }

    struct Move {
        int destination;
        Clip clip;
        std::pair<int, int> source;
    };

    std::map<std::pair<int, int>, ClipReference> remapped;
    std::vector<Move> moves;
    moves.reserve(selected.size());
    std::set<std::pair<int, int>> seen;
    for (const auto& ref : selected) {
        seen.emplace(ref.track, ref.clip);
    }

    auto candidate = session;
    if (trackDelta == 0) {
        // Retain track order and overlap stacking for horizontal moves.
        for (const auto& ref : seen) {
            auto& clip = candidate.tracks[static_cast<size_t>(ref.first)]
                             .clips[static_cast<size_t>(ref.second)];
            clip.start = shiftTimelineBySamples(clip.start, sampleDelta);
            if (movedRefs) {
                remapped.emplace(ref, ClipReference{ref.first, ref.second});
            }
        }
    } else {
        // Selection order may differ from the source's stacking order.
        for (const auto& ref : seen) {
            auto clip = session.tracks[static_cast<size_t>(ref.first)]
                            .clips[static_cast<size_t>(ref.second)];
            clip.start = shiftTimelineBySamples(clip.start, sampleDelta);
            moves.push_back({static_cast<int>(ref.first + trackDelta), std::move(clip), ref});
        }
        for (auto it = seen.rbegin(); it != seen.rend(); ++it) {
            auto& clips = candidate.tracks[static_cast<size_t>(it->first)].clips;
            clips.erase(clips.begin() + it->second);
        }
        for (auto& move : moves) {
            auto& clips = candidate.tracks[static_cast<size_t>(move.destination)].clips;
            if (movedRefs) {
                remapped.emplace(move.source,
                                 ClipReference{move.destination, static_cast<int>(clips.size())});
            }
            clips.push_back(std::move(move.clip));
        }
    }

    if (!preservesMarkerBounds(session, candidate, error)) {
        return false;
    }

    std::vector<ClipReference> result;
    if (movedRefs) {
        result.reserve(selected.size());
        for (const auto& ref : selected) {
            result.push_back(remapped.at({ref.track, ref.clip}));
        }
    }

    output = std::move(candidate);
    if (movedRefs) {
        *movedRefs = std::move(result);
    }
    return true;
}

bool prepareClipGroupCopy(const Session& session, Session& output,
                          const std::vector<ClipReference>& selected, juce::int64 sampleDelta,
                          juce::int64 trackDelta, juce::String& error,
                          std::vector<ClipReference>* copiedRefs) {
    error.clear();
    if (sampleDelta == 0 && trackDelta == 0) {
        error = "The copy destination is unchanged.";
        return false;
    }
    if (!validateClipGroupMove(session, selected, sampleDelta, trackDelta, error)) {
        return false;
    }

    std::set<std::pair<int, int>> ordered;
    for (const auto& ref : selected) {
        ordered.emplace(ref.track, ref.clip);
    }

    struct Copy {
        std::pair<int, int> source;
        int destination;
        Clip clip;
    };

    std::vector<Copy> copies;
    copies.reserve(ordered.size());
    for (const auto& ref : ordered) {
        auto clip =
            session.tracks[static_cast<size_t>(ref.first)].clips[static_cast<size_t>(ref.second)];
        clip.start = shiftTimelineBySamples(clip.start, sampleDelta);
        copies.push_back({ref, static_cast<int>(ref.first + trackDelta), std::move(clip)});
    }

    std::vector<Clip*> groupMembers;
    groupMembers.reserve(copies.size());
    for (auto& copy : copies) {
        groupMembers.push_back(&copy.clip);
    }
    if (!forkCopiedClipGroups(session, groupMembers, error)) {
        return false;
    }

    auto candidate = session;
    std::map<std::pair<int, int>, ClipReference> mapping;
    for (auto& copy : copies) {
        auto& target = candidate.tracks[static_cast<size_t>(copy.destination)].clips;
        mapping.emplace(copy.source,
                        ClipReference{copy.destination, static_cast<int>(target.size())});
        target.push_back(std::move(copy.clip));
    }

    std::vector<ClipReference> result;
    if (copiedRefs) {
        result.reserve(selected.size());
        for (const auto& ref : selected) {
            result.push_back(mapping.at({ref.track, ref.clip}));
        }
    }

    output = std::move(candidate);
    if (copiedRefs) {
        *copiedRefs = std::move(result);
    }
    return true;
}

bool moveClipGroup(Session& session, const std::vector<ClipReference>& selected,
                   juce::int64 sampleDelta, juce::int64 trackDelta, juce::String& error,
                   std::vector<ClipReference>* movedRefs) {
    return prepareClipGroupMove(session, session, selected, sampleDelta, trackDelta, error,
                                movedRefs);
}

namespace {

struct ArrangementUnit {
    std::vector<ClipReference> clips;
    double start = 0;
    double end = 0;
};

bool arrangementUnits(const Session& session, const std::vector<ClipReference>& selected,
                      std::vector<ArrangementUnit>& units, juce::String& error) {
    std::vector<ClipReference> expanded;
    if (!expandClipGroups(session, selected, expanded, error) ||
        !validateClipGroupMove(session, expanded, 0, 0, error)) {
        return false;
    }

    std::map<juce::String, size_t> groups;
    std::vector<ArrangementUnit> result;
    for (const auto& ref : expanded) {
        const auto& clip =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        size_t index = result.size();
        if (clip.groupId.isNotEmpty()) {
            const auto found = groups.find(clip.groupId);
            if (found != groups.end()) {
                index = found->second;
            } else {
                groups.emplace(clip.groupId, index);
            }
        }
        if (index == result.size()) {
            result.push_back({{}, clip.start, clip.start + clip.duration});
        }
        auto& unit = result[index];
        unit.clips.push_back(ref);
        unit.start = std::min(unit.start, clip.start);
        unit.end = std::max(unit.end, clip.start + clip.duration);
    }

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return std::tie(a.start, a.clips.front().track, a.clips.front().clip) <
               std::tie(b.start, b.clips.front().track, b.clips.front().clip);
    });
    units = std::move(result);
    return true;
}

bool placeArrangementUnit(const Session& original, Session& candidate, const ArrangementUnit& unit,
                          double target, bool& changed, juce::String& error) {
    const double delta = target - unit.start;
    const double frames = delta * sessionRate;
    const double nearest = std::round(frames);
    if (!std::isfinite(delta) || !std::isfinite(frames)) {
        error = "The arrangement is outside the precise timeline range.";
        return false;
    }

    const bool sampleShift =
        std::abs(frames - nearest) < 1e-7 && std::abs(nearest) < 9007199254740992.0;
    for (const auto& ref : unit.clips) {
        const auto& source =
            original.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        auto& clip =
            candidate.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        const double start =
            std::equal_to<double>{}(source.start, unit.start) ? target
            : sampleShift ? shiftTimelineBySamples(source.start, static_cast<juce::int64>(nearest))
                          : source.start + delta;
        if (!validTimelineGeometry(start, clip.offset, clip.duration) ||
            std::abs(((start - source.start) - delta) * sessionRate) > 1e-4) {
            error = "A clip cannot retain its group position within the precise timeline range.";
            return false;
        }
        if (!std::equal_to<double>{}(clip.start, start)) {
            changed = true;
        }
        clip.start = start;
    }

    return true;
}

double arrangementUnitSpan(const Session& session, const ArrangementUnit& unit) {
    if (unit.clips.size() == 1) {
        const auto& ref = unit.clips.front();
        return session.tracks[static_cast<size_t>(ref.track)]
            .clips[static_cast<size_t>(ref.clip)]
            .duration;
    }
    return unit.end - unit.start;
}

} // namespace

bool prepareSequenceClipGroup(const Session& session, Session& output,
                              const std::vector<ClipReference>& selected, juce::String& error) {
    std::vector<ArrangementUnit> units;
    if (!arrangementUnits(session, selected, units, error)) {
        return false;
    }
    if (units.size() < 2) {
        error = "Select at least two clips or groups to sequence.";
        return false;
    }

    auto candidate = session;
    bool changed = false;
    double cursor = units.front().start;
    for (const auto& unit : units) {
        if (!placeArrangementUnit(session, candidate, unit, cursor, changed, error)) {
            return false;
        }
        cursor += arrangementUnitSpan(session, unit);
    }

    if (!changed) {
        error = "Selected clips and groups are already consecutive.";
        return false;
    }
    if (!preservesMarkerBounds(session, candidate, error)) {
        return false;
    }

    output = std::move(candidate);
    return true;
}

bool sequenceClipGroup(Session& session, const std::vector<ClipReference>& selected,
                       juce::String& error) {
    return prepareSequenceClipGroup(session, session, selected, error);
}

bool prepareDistributeClipGroup(const Session& session, Session& output,
                                const std::vector<ClipReference>& selected, juce::String& error) {
    std::vector<ArrangementUnit> units;
    if (!arrangementUnits(session, selected, units, error)) {
        return false;
    }
    if (units.size() < 3) {
        error = "Select at least three clips or groups to distribute.";
        return false;
    }

    long double occupied = 0;
    for (size_t i = 0; i + 1 < units.size(); ++i) {
        occupied += static_cast<long double>(arrangementUnitSpan(session, units[i]));
    }

    const long double available = static_cast<long double>(units.back().start) -
                                  static_cast<long double>(units.front().start) - occupied;
    if (available < -1e-12L) {
        error = "Selected clips or groups do not fit between the fixed endpoints without overlap.";
        return false;
    }

    const long double gap = std::max(0.0L, available) / static_cast<long double>(units.size() - 1);
    auto candidate = session;
    bool changed = false;
    long double cursor = units.front().start;
    for (size_t i = 0; i + 1 < units.size(); ++i) {
        if (!placeArrangementUnit(session, candidate, units[i],
                                  i == 0 ? units.front().start : static_cast<double>(cursor),
                                  changed, error)) {
            return false;
        }
        cursor += static_cast<long double>(arrangementUnitSpan(session, units[i])) + gap;
    }

    if (!changed) {
        error = "Selected clips and groups already have equal gaps.";
        return false;
    }
    if (!preservesMarkerBounds(session, candidate, error)) {
        return false;
    }

    output = std::move(candidate);
    return true;
}

bool distributeClipGroup(Session& session, const std::vector<ClipReference>& selected,
                         juce::String& error) {
    return prepareDistributeClipGroup(session, session, selected, error);
}

bool copyClipGroup(const Session& session, const std::vector<ClipReference>& selected,
                   Clipboard& output, juce::String& error) {
    error.clear();
    if (selected.empty()) {
        error = "Select at least one clip.";
        return false;
    }

    std::set<std::pair<int, int>> seen;
    int firstTrack = static_cast<int>(session.tracks.size());
    int lastTrack = -1;
    double firstFrame = std::numeric_limits<double>::infinity();
    double lastFrame = 0;

    for (const auto& ref : selected) {
        if (ref.track < 0 || ref.track >= static_cast<int>(session.tracks.size()) || ref.clip < 0 ||
            ref.clip >=
                static_cast<int>(session.tracks[static_cast<size_t>(ref.track)].clips.size()) ||
            !seen.emplace(ref.track, ref.clip).second) {
            error = "The clip selection is invalid or contains duplicates.";
            return false;
        }

        const auto& clip =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        if (!validClipForSampleEdit(clip) ||
            !validTimelineGeometry(clip.start, clip.offset, clip.duration)) {
            error = "A selected clip has invalid geometry.";
            return false;
        }

        const auto canonical = [](double frame) {
            const double nearest = std::round(frame);
            return std::abs(frame - nearest) < 1e-7 ? nearest : frame;
        };
        firstFrame = std::min(firstFrame, canonical(clip.start * sessionRate));
        lastFrame = std::max(lastFrame, canonical((clip.start + clip.duration) * sessionRate));
        firstTrack = std::min(firstTrack, ref.track);
        lastTrack = std::max(lastTrack, ref.track);
    }

    const double first = std::floor(firstFrame);
    const double last = std::ceil(lastFrame);
    if (!std::isfinite(first) || !std::isfinite(last) || first < 0 ||
        last > static_cast<double>(maxPreciseSample) || last <= first) {
        error = "Selected clips exceed the precise timeline range.";
        return false;
    }

    Clipboard result;
    result.lengthSamples = static_cast<juce::int64>(last - first);
    result.wholeClips = true;
    result.tracks.resize(static_cast<size_t>(lastTrack - firstTrack + 1));

    const double origin = first / sessionRate;
    for (const auto& ref : seen) {
        auto clip =
            session.tracks[static_cast<size_t>(ref.first)].clips[static_cast<size_t>(ref.second)];
        clip.start -= origin;
        // Decimal round-trip noise at a sample-aligned origin must not create
        // a tiny negative relative position that makes a paste at zero fail.
        if (clip.start < 0 && clip.start * sessionRate > -1e-7) {
            clip.start = 0;
        }
        result.tracks[static_cast<size_t>(ref.first - firstTrack)].push_back(std::move(clip));
    }

    output = std::move(result);
    return true;
}
bool canSplitClipAtSample(const Clip& clip, juce::int64 sample) noexcept {
    juce::int64 first = 0;
    juce::int64 last = 0;
    if (sample < 0 || sample > maxPreciseSample || !clipSampleRange(clip, first, last) ||
        sample <= first || sample >= last) {
        return false;
    }

    const double at = static_cast<double>(sample) / sessionRate;
    return at > clip.start && at < clip.start + clip.duration;
}

namespace {

// Keep sample-aligned lengths and source shifts in integer-sample arithmetic;
// subtracting large absolute timestamps can move the source phase by ulps.
bool splitClipGeometry(const Clip& clip, juce::int64 sample, Clip& left, Clip& right) {
    if (sample < 0 || sample > maxPreciseSample) {
        return false;
    }

    const double at = static_cast<double>(sample) / sessionRate;
    const double end = clip.start + clip.duration;
    if (!(at > clip.start && at < end)) {
        return false;
    }

    const double firstFrame = clip.start * sessionRate;
    const double lastFrame = end * sessionRate;
    const double nearestFirst = std::round(firstFrame);
    const double nearestLast = std::round(lastFrame);
    const bool grid = std::isfinite(firstFrame) && std::isfinite(lastFrame) && firstFrame >= 0 &&
                      nearestFirst < 9007199254740992.0 && nearestLast < 9007199254740992.0 &&
                      std::abs(firstFrame - nearestFirst) < 1.0e-7 &&
                      std::abs(lastFrame - nearestLast) < 1.0e-7;

    const double local =
        grid ? (static_cast<double>(sample) - nearestFirst) / sessionRate : at - clip.start;
    const double remaining =
        grid ? (nearestLast - static_cast<double>(sample)) / sessionRate : end - at;
    if (!(local > 0 && remaining > 0)) {
        return false;
    }

    left = clip;
    right = clip;
    left.duration = local;
    right.start = at;
    right.offset += local;
    right.duration = remaining;
    return true;
}

} // namespace

bool splitClipAtSample(Session& session, int track, int clip, juce::int64 sample) {
    if (sample < 0 || sample > maxPreciseSample || track < 0 ||
        track >= static_cast<int>(session.tracks.size()) || clip < 0 ||
        clip >= static_cast<int>(session.tracks[static_cast<size_t>(track)].clips.size())) {
        return false;
    }

    auto& clips = session.tracks[static_cast<size_t>(track)].clips;
    auto& original = clips[static_cast<size_t>(clip)];
    if (!canSplitClipAtSample(original, sample)) {
        return false;
    }

    Clip left;
    Clip right;
    if (!splitClipGeometry(original, sample, left, right)) {
        return false;
    }

    original = std::move(left);
    clips.insert(clips.begin() + clip + 1, std::move(right));
    return true;
}

bool splitClipGroupAtSample(Session& session, const std::vector<ClipReference>& selected,
                            juce::int64 sample, juce::String& error,
                            std::vector<std::vector<ClipReference>>* pieces) {
    error.clear();
    if (selected.empty()) {
        error = "Select at least one clip.";
        return false;
    }
    if (sample < 0 || sample > maxPreciseSample) {
        error = "Invalid split position.";
        return false;
    }

    std::set<std::pair<int, int>> seen;
    for (const auto& ref : selected) {
        if (ref.track < 0 || ref.track >= static_cast<int>(session.tracks.size()) || ref.clip < 0 ||
            ref.clip >=
                static_cast<int>(session.tracks[static_cast<size_t>(ref.track)].clips.size()) ||
            !seen.emplace(ref.track, ref.clip).second ||
            !validClipForSampleEdit(session.tracks[static_cast<size_t>(ref.track)]
                                        .clips[static_cast<size_t>(ref.clip)])) {
            error = "The clip selection is invalid or contains duplicates.";
            return false;
        }
    }

    auto candidate = session;
    std::set<std::pair<int, int>> split;
    // Descending clip indices prevent inserts from invalidating later targets.
    for (auto it = seen.rbegin(); it != seen.rend(); ++it) {
        if (splitClipAtSample(candidate, it->first, it->second, sample)) {
            split.insert(*it);
        }
    }

    if (split.empty()) {
        error =
            "No selected clips can be split at the playhead; each side needs at least one sample.";
        return false;
    }

    std::vector<std::vector<ClipReference>> result;
    if (pieces) {
        std::map<std::pair<int, int>, std::vector<ClipReference>> mapping;
        int track = -1;
        int shift = 0;
        for (const auto& ref : seen) {
            if (ref.first != track) {
                track = ref.first;
                shift = 0;
            }
            auto& entry = mapping[ref];
            entry.push_back({ref.first, ref.second + shift});
            if (split.count(ref) > 0) {
                entry.push_back({ref.first, ref.second + shift + 1});
                ++shift;
            }
        }
        result.reserve(selected.size());
        for (const auto& ref : selected) {
            result.push_back(mapping.at({ref.track, ref.clip}));
        }
    }

    session = std::move(candidate);
    if (pieces) {
        *pieces = std::move(result);
    }
    return true;
}

namespace {

void keepMasterEnd(Session& session, juce::int64 previousEnd) noexcept {
    if (projectEndSample(session) < previousEnd) {
        session.minimumEndSample = previousEnd;
    }
}

} // namespace

bool detachClipRanges(Session& session, int track, int clip, const std::vector<QuietRegion>& ranges,
                      juce::String& error) {
    error.clear();
    if (track < 0 || track >= static_cast<int>(session.tracks.size()) || clip < 0 ||
        clip >= static_cast<int>(session.tracks[static_cast<size_t>(track)].clips.size())) {
        error = "The target clip no longer exists.";
        return false;
    }

    const auto& original =
        session.tracks[static_cast<size_t>(track)].clips[static_cast<size_t>(clip)];
    juce::int64 first = 0;
    juce::int64 last = 0;
    if (!clipSampleRange(original, first, last) || ranges.empty()) {
        error = "Select at least one valid detach range.";
        return false;
    }

    juce::int64 previous = first;
    for (const auto& range : ranges) {
        if (range.start < previous || range.start < first || range.end > last ||
            range.end <= range.start) {
            error = "Detach ranges must be ordered, nonoverlapping and inside the clip.";
            return false;
        }
        previous = range.end;
    }

    if (original.groupId.isNotEmpty()) {
        error = "Ungroup this clip before detaching silence from it.";
        return false;
    }

    const double clipEnd = original.start + original.duration;
    double cursor = original.start;
    std::vector<Clip> pieces;
    pieces.reserve(ranges.size() + 1);

    const auto append = [&](double from, double to) {
        if (to <= from) {
            return;
        }
        auto piece = original;
        piece.start = from;
        piece.offset += timelineDifference(from, original.start);
        piece.duration = timelineDifference(to, from);
        if (piece.duration > 0) {
            pieces.push_back(std::move(piece));
        }
    };

    for (const auto& range : ranges) {
        append(cursor, std::min(clipEnd, seconds(range.start)));
        cursor = std::min(clipEnd, seconds(range.end));
    }
    append(cursor, clipEnd);

    for (const auto& piece : pieces) {
        if (!validTimelineGeometry(piece.start, piece.offset, piece.duration)) {
            error = "Detaching would create invalid clip geometry.";
            return false;
        }
    }

    auto candidate = session;
    auto& clips = candidate.tracks[static_cast<size_t>(track)].clips;
    auto at = clips.erase(clips.begin() + clip);
    clips.insert(at, pieces.begin(), pieces.end());
    keepMasterEnd(candidate, projectEndSample(session));
    if (!preservesMarkerBounds(session, candidate, error)) {
        return false;
    }

    session = std::move(candidate);
    return true;
}

bool detachClipRange(Session& session, int track, int clip, juce::int64 start, juce::int64 end,
                     juce::String& error) {
    return detachClipRanges(session, track, clip, {{start, end}}, error);
}

bool deleteClipGroup(Session& session, const std::vector<ClipReference>& selected,
                     juce::String& error) {
    error.clear();
    if (selected.empty()) {
        error = "Select at least one clip.";
        return false;
    }

    std::set<std::pair<int, int>> seen;
    for (const auto& ref : selected) {
        if (ref.track < 0 || ref.track >= static_cast<int>(session.tracks.size()) || ref.clip < 0 ||
            ref.clip >=
                static_cast<int>(session.tracks[static_cast<size_t>(ref.track)].clips.size()) ||
            !seen.emplace(ref.track, ref.clip).second) {
            error = "The clip selection is invalid or contains duplicates.";
            return false;
        }
    }

    auto candidate = session;
    for (auto it = seen.rbegin(); it != seen.rend(); ++it) {
        auto& clips = candidate.tracks[static_cast<size_t>(it->first)].clips;
        clips.erase(clips.begin() + it->second);
    }

    keepMasterEnd(candidate, projectEndSample(session));
    if (!preservesMarkerBounds(session, candidate, error)) {
        return false;
    }

    session = std::move(candidate);
    return true;
}

bool cutClipGroup(Session& session, const std::vector<ClipReference>& selected, Clipboard& output,
                  juce::String& error) {
    Clipboard copied;
    if (!copyClipGroup(session, selected, copied, error) ||
        !deleteClipGroup(session, selected, error)) {
        return false;
    }

    output = std::move(copied);
    return true;
}

bool duplicateClipGroup(Session& session, const std::vector<ClipReference>& selected,
                        juce::String& error, std::vector<ClipReference>* duplicatedRefs) {
    error.clear();
    if (selected.empty()) {
        error = "Select at least one clip.";
        return false;
    }

    auto candidate = session;
    std::vector<ClipReference> copies;
    copies.reserve(selected.size());
    std::set<std::pair<int, int>> seen;
    juce::int64 first = maxPreciseSample;
    juce::int64 last = 0;

    for (const auto& ref : selected) {
        if (ref.track < 0 || ref.track >= static_cast<int>(session.tracks.size()) || ref.clip < 0 ||
            ref.clip >=
                static_cast<int>(session.tracks[static_cast<size_t>(ref.track)].clips.size()) ||
            !seen.emplace(ref.track, ref.clip).second) {
            error = "The clip selection is invalid or contains duplicates.";
            return false;
        }

        const auto& clip =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        juce::int64 start = 0;
        juce::int64 end = 0;
        if (!validClipForSampleEdit(clip) || !clipSampleRange(clip, start, end)) {
            error = "A selected clip has no valid project-sample range.";
            return false;
        }
        first = std::min(first, start);
        last = std::max(last, end);
    }

    // Keep the originals' stacking order even if selection order is reversed.
    std::map<std::pair<int, int>, ClipReference> mapping;
    for (const auto& ref : seen) {
        const auto& source =
            session.tracks[static_cast<size_t>(ref.first)].clips[static_cast<size_t>(ref.second)];
        auto& dest = candidate.tracks[static_cast<size_t>(ref.first)].clips;
        const ClipReference copy{ref.first, static_cast<int>(dest.size())};
        copies.push_back(copy);
        mapping.emplace(ref, copy);
        dest.push_back(source);
    }

    if (!moveClipGroup(candidate, copies, last - first, 0, error)) {
        return false;
    }

    std::vector<Clip*> copiedClips;
    copiedClips.reserve(copies.size());
    for (const auto& ref : copies) {
        copiedClips.push_back(
            &candidate.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)]);
    }
    if (!forkCopiedClipGroups(session, copiedClips, error)) {
        return false;
    }

    std::vector<ClipReference> result;
    if (duplicatedRefs) {
        result.reserve(selected.size());
        for (const auto& ref : selected) {
            result.push_back(mapping.at({ref.track, ref.clip}));
        }
    }

    session = std::move(candidate);
    if (duplicatedRefs) {
        *duplicatedRefs = std::move(result);
    }
    return true;
}

bool moveClipToTrack(Session& session, int sourceTrack, int& clip, int destinationTrack) {
    const int count = static_cast<int>(session.tracks.size());
    if (sourceTrack < 0 || sourceTrack >= count || destinationTrack < 0 ||
        destinationTrack >= count || sourceTrack == destinationTrack) {
        return false;
    }

    auto& source = session.tracks[static_cast<size_t>(sourceTrack)].clips;
    auto& destination = session.tracks[static_cast<size_t>(destinationTrack)].clips;
    if (clip < 0 || clip >= static_cast<int>(source.size())) {
        return false;
    }

    destination.push_back(source[static_cast<size_t>(clip)]);
    source.erase(source.begin() + clip);
    clip = static_cast<int>(destination.size()) - 1;
    return true;
}

bool moveClipToSample(Clip& clip, juce::int64 start, juce::String& error) {
    if (!validClipForSampleEdit(clip) || start < 0 || start > maxPreciseSample) {
        error = "Invalid clip position.";
        return false;
    }

    const auto position = static_cast<double>(start) / sessionRate;
    if (!validTimelineGeometry(position, clip.offset, clip.duration)) {
        error = "Clip would extend beyond the precise timeline range.";
        return false;
    }

    clip.start = position;
    return true;
}

bool alignClipEdgeToSample(Clip& clip, bool endEdge, juce::int64 sample, juce::String& error) {
    if (!validClipForSampleEdit(clip) || sample < 0 || sample > maxPreciseSample) {
        error = "Invalid clip or alignment position.";
        return false;
    }

    return alignClipEdgeToTime(clip, endEdge, static_cast<double>(sample) / sessionRate, error);
}

bool alignClipEdgeToTime(Clip& clip, bool endEdge, double target, juce::String& error) {
    if (!validClipForSampleEdit(clip) || !std::isfinite(target) || target < 0 ||
        target * sessionRate > static_cast<double>(maxPreciseSample)) {
        error = "Invalid clip or alignment position.";
        return false;
    }

    const double start = endEdge ? target - clip.duration : target;
    if (!validTimelineGeometry(start, clip.offset, clip.duration)) {
        error = "Clip cannot fit at that alignment position.";
        return false;
    }

    clip.start = start;
    return true;
}

bool alignClipGroupEdgeToTime(Session& session, const std::vector<ClipReference>& selected,
                              ClipReference anchor, bool endEdge, double target,
                              juce::String& error) {
    if (!validateClipGroupMove(session, selected, 0, 0, error)) {
        return false;
    }
    if (std::none_of(selected.begin(), selected.end(), [&](const auto& ref) {
            return ref.track == anchor.track && ref.clip == anchor.clip;
        })) {
        error = "The anchor clip is not selected.";
        return false;
    }

    const auto& originalAnchor =
        session.tracks[static_cast<size_t>(anchor.track)].clips[static_cast<size_t>(anchor.clip)];
    auto aligned = originalAnchor;
    if (!alignClipEdgeToTime(aligned, endEdge, target, error)) {
        return false;
    }
    if (std::abs((aligned.start + (endEdge ? aligned.duration : 0.0) - target) * sessionRate) >
        1e-4) {
        error = "The anchor cannot reach that boundary accurately.";
        return false;
    }

    const double delta = aligned.start - originalAnchor.start;
    if (std::equal_to<double>{}(delta, 0.0)) {
        error = "Clip group is already aligned.";
        return false;
    }

    const double frames = delta * sessionRate;
    const double rounded = std::round(frames);
    const bool sampleShift = std::isfinite(frames) && std::abs(frames - rounded) < 1e-7 &&
                             std::abs(rounded) < 9007199254740992.0;

    auto candidate = session;
    for (const auto& ref : selected) {
        const auto& original =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        auto& edited =
            candidate.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        const bool isAnchor = ref.track == anchor.track && ref.clip == anchor.clip;
        edited.start = isAnchor      ? aligned.start
                       : sampleShift ? shiftTimelineBySamples(original.start,
                                                              static_cast<juce::int64>(rounded))
                                     : original.start + delta;
        if (!validTimelineGeometry(edited.start, edited.offset, edited.duration) ||
            std::abs(((edited.start - original.start) - delta) * sessionRate) > 1e-4) {
            error = "A grouped clip cannot move accurately to that alignment.";
            return false;
        }
    }

    if (!preservesMarkerBounds(session, candidate, error)) {
        return false;
    }

    session = std::move(candidate);
    return true;
}

bool adjacentClipEdgeExcluding(const Session& session, ClipReference ref, bool endEdge, bool next,
                               const std::vector<ClipReference>& excluded,
                               double& target) noexcept {
    if (ref.track < 0 || ref.track >= static_cast<int>(session.tracks.size())) {
        return false;
    }
    const auto& clips = session.tracks[static_cast<size_t>(ref.track)].clips;
    if (ref.clip < 0 || ref.clip >= static_cast<int>(clips.size())) {
        return false;
    }
    const auto& clip = clips[static_cast<size_t>(ref.clip)];
    if (!validClipForSampleEdit(clip)) {
        return false;
    }

    const double origin = clip.start + (endEdge ? clip.duration : 0.0);
    bool found = false;
    double nearest = 0;

    for (size_t t = 0; t < session.tracks.size(); ++t) {
        const auto& row = session.tracks[t].clips;
        for (size_t c = 0; c < row.size(); ++c) {
            if (t == static_cast<size_t>(ref.track) && c == static_cast<size_t>(ref.clip)) {
                continue;
            }
            if (std::any_of(excluded.begin(), excluded.end(), [&](const auto& item) {
                    return item.track == static_cast<int>(t) && item.clip == static_cast<int>(c);
                })) {
                continue;
            }

            const auto& other = row[c];
            if (!validClipForSampleEdit(other)) {
                continue;
            }
            for (const auto edge : {other.start, other.start + other.duration}) {
                if ((next ? edge > origin : edge < origin) &&
                    (!found || (next ? edge < nearest : edge > nearest))) {
                    nearest = edge;
                    found = true;
                }
            }
        }
    }

    if (found) {
        target = nearest;
    }
    return found;
}

bool adjacentClipEdge(const Session& session, ClipReference ref, bool endEdge, bool next,
                      double& target) noexcept {
    return adjacentClipEdgeExcluding(session, ref, endEdge, next, {}, target);
}

bool trimClipToSamples(Clip& clip, juce::int64 start, juce::int64 end, juce::String& error) {
    if (!validClipForSampleEdit(clip) || start < 0 || end <= start || end > maxPreciseSample) {
        error = "Enter a valid start and exclusive end sample.";
        return false;
    }

    const double newStart = static_cast<double>(start) / sessionRate;
    const double oldFrame = clip.start * sessionRate;
    const double nearest = std::round(oldFrame);
    const bool oldOnGrid = std::isfinite(oldFrame) && oldFrame >= 0 &&
                           nearest < 9007199254740992.0 && std::abs(oldFrame - nearest) < 1.0e-7;
    const double shift =
        oldOnGrid ? (static_cast<double>(start) - nearest) / sessionRate : newStart - clip.start;
    double newOffset = clip.offset + shift;
    const double newDuration = static_cast<double>(end - start) / sessionRate;
    const double sourceLength = clip.audio->length();

    // Permit only floating-point roundoff at a source edge, never a meaningful
    // fraction of a project sample. Keep the requested sample duration.
    const double tolerance = std::min(
        1.0e-4 / sessionRate, 8.0 * std::numeric_limits<double>::epsilon() *
                                  std::max({1.0, std::abs(newOffset), newDuration, sourceLength}));
    if (newOffset < 0 && newOffset >= -tolerance) {
        newOffset = 0;
    }
    if (!validTimelineGeometry(newStart, newOffset, newDuration) ||
        newOffset + newDuration > sourceLength + tolerance) {
        error = "Trim range extends beyond the source audio.";
        return false;
    }

    clip.start = newStart;
    clip.offset = newOffset;
    clip.duration = newDuration;
    return true;
}

namespace {

bool adjustClipEdge(Clip& clip, bool endEdge, juce::int64 direction, juce::String& error) {
    error.clear();
    if (!validClipForSampleEdit(clip) || direction == 0 || direction < -maxPreciseSample ||
        direction > maxPreciseSample) {
        error = "Invalid clip or sample offset.";
        return false;
    }

    const double delta = static_cast<double>(direction) / sessionRate;
    auto changed = clip;
    const double firstFrame = clip.start * sessionRate;
    const double lastFrame = (clip.start + clip.duration) * sessionRate;
    const double nearestFirst = std::round(firstFrame);
    const double nearestLast = std::round(lastFrame);
    const bool onGrid = std::isfinite(firstFrame) && std::isfinite(lastFrame) && firstFrame >= 0 &&
                        nearestFirst < 9007199254740992.0 && nearestLast < 9007199254740992.0 &&
                        std::abs(firstFrame - nearestFirst) < 1.0e-7 &&
                        std::abs(lastFrame - nearestLast) < 1.0e-7;

    if (onGrid) {
        auto first = static_cast<juce::int64>(nearestFirst);
        auto last = static_cast<juce::int64>(nearestLast);
        const auto edge = endEdge ? last : first;
        if (direction < -edge || direction > maxPreciseSample - edge) {
            error = "Clip edge would move outside the precise timeline range.";
            return false;
        }
        if (endEdge) {
            last += direction;
        } else {
            first += direction;
        }
        if (!trimClipToSamples(changed, first, last, error)) {
            return false;
        }

        const double oldEdge = endEdge ? clip.start + clip.duration : clip.start;
        const double newEdge = endEdge ? changed.start + changed.duration : changed.start;
        if (std::abs((newEdge - oldEdge) * sessionRate - direction) > 1e-4) {
            error = "Clip edge cannot be adjusted accurately at this position.";
            return false;
        }

        clip = std::move(changed);
        return true;
    }

    if (endEdge) {
        changed.duration += delta;
    } else {
        changed.start += delta;
        // At long timeline positions the representable start movement can
        // differ slightly from delta. Apply that same movement to the source
        // offset and duration so the retained source audio stays aligned.
        const double actualShift = changed.start - clip.start;
        changed.offset += actualShift;
        changed.duration -= actualShift;
    }

    const double minimum = std::min(clip.duration, 1.0 / sessionRate);
    if (changed.duration + 1e-12 < minimum ||
        !validTimelineGeometry(changed.start, changed.offset, changed.duration) ||
        changed.offset + changed.duration > clip.audio->length() + 1e-8) {
        error = "Clip edge cannot move farther within the source and timeline bounds.";
        return false;
    }

    const double oldEdge = endEdge ? clip.start + clip.duration : clip.start;
    const double newEdge = endEdge ? changed.start + changed.duration : changed.start;
    if (std::abs((newEdge - oldEdge) * sessionRate - direction) > 1e-4) {
        error = "Clip edge cannot be adjusted accurately at this position.";
        return false;
    }

    clip = std::move(changed);
    return true;
}

} // namespace

bool nudgeClipEdge(Clip& clip, bool endEdge, int direction, juce::String& error) {
    if (direction != -1 && direction != 1) {
        error = "Invalid clip or trim direction.";
        return false;
    }
    return adjustClipEdge(clip, endEdge, direction, error);
}

bool previewClipGroupEdges(const Session& session, const std::vector<ClipReference>& selected,
                           bool endEdge, juce::int64 direction, std::vector<Clip>& result,
                           juce::String& error) {
    if (!validateClipGroupMove(session, selected, 0, 0, error)) {
        return false;
    }

    std::vector<Clip> adjusted;
    adjusted.reserve(selected.size());
    for (const auto& ref : selected) {
        auto clip =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        if (!adjustClipEdge(clip, endEdge, direction, error)) {
            return false;
        }
        adjusted.push_back(std::move(clip));
    }

    result = std::move(adjusted);
    return true;
}

bool ClipGroupEdgePreview::prepare(const Session& session,
                                   const std::vector<ClipReference>& selected,
                                   juce::String& error) {
    originals.clear();
    scratch.clear();
    if (!validateClipGroupMove(session, selected, 0, 0, error)) {
        return false;
    }

    originals.reserve(selected.size());
    scratch.reserve(selected.size());
    for (const auto& ref : selected) {
        originals.push_back(
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)]);
    }
    return true;
}

bool ClipGroupEdgePreview::calculate(bool endEdge, juce::int64 samples, std::vector<Clip>& result,
                                     juce::String& error) {
    error.clear();
    scratch.clear();
    if (originals.empty()) {
        error = "The clip preview has not been prepared.";
        return false;
    }

    scratch.reserve(originals.size());
    for (const auto& original : originals) {
        auto clip = original;
        if (!adjustClipEdge(clip, endEdge, samples, error)) {
            scratch.clear();
            return false;
        }
        scratch.push_back(std::move(clip));
    }

    result.swap(scratch);
    return true;
}

bool ClipGroupEdgeMarkerPreview::prepare(const Session& session,
                                         const std::vector<ClipReference>& selected,
                                         juce::String& error) {
    if (!validateClipGroupMove(session, selected, 0, 0, error)) {
        return false;
    }

    std::set<std::pair<int, int>> selectedKeys;
    for (const auto& ref : selected) {
        selectedKeys.emplace(ref.track, ref.clip);
    }

    double fixedEnd = 0;
    for (size_t track = 0; track < session.tracks.size(); ++track) {
        const auto& clips = session.tracks[track].clips;
        for (size_t clip = 0; clip < clips.size(); ++clip) {
            if (!selectedKeys.count({static_cast<int>(track), static_cast<int>(clip)})) {
                fixedEnd = std::max(fixedEnd, clips[clip].start + clips[clip].duration);
            }
        }
    }

    const auto oldEnd = projectEndSample(session);
    juce::int64 floor = 0;
    for (const auto& marker : session.markers) {
        const double frame = marker.position * sessionRate;
        if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
            continue;
        }
        const auto sample = static_cast<juce::int64>(std::llround(frame));
        if (sample < oldEnd) {
            floor = std::max(floor, sample + 1);
        }
    }

    unselectedEnd = fixedEnd;
    minimumEnd = session.minimumEndSample;
    requiredEnd = floor;
    clipCount = selected.size();
    return true;
}

bool ClipGroupEdgeMarkerPreview::validate(const std::vector<Clip>& adjusted,
                                          juce::String& error) const {
    if (adjusted.size() != clipCount || clipCount == 0) {
        error = "The clip preview is invalid.";
        return false;
    }

    double end = unselectedEnd;
    for (const auto& clip : adjusted) {
        end = std::max(end, clip.start + clip.duration);
    }

    if (projectEndSampleForAudioEnd(end, minimumEnd) < requiredEnd) {
        error = "This edit would put a track marker beyond the master. Move or delete the marker "
                "first.";
        return false;
    }
    return true;
}

bool nudgeClipGroupEdges(Session& session, const std::vector<ClipReference>& selected, bool endEdge,
                         juce::int64 direction, juce::String& error) {
    std::vector<Clip> adjusted;
    if (!previewClipGroupEdges(session, selected, endEdge, direction, adjusted, error)) {
        return false;
    }

    for (size_t i = 0; i < selected.size(); ++i) {
        const auto& ref = selected[i];
        session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)] =
            std::move(adjusted[i]);
    }
    return true;
}

namespace {

bool previewClipGroupAnchorRange(const Session& session, const std::vector<ClipReference>& selected,
                                 ClipReference anchor, bool setStart, juce::int64 start,
                                 bool setEnd, juce::int64 end, std::vector<Clip>& output,
                                 juce::String& error) {
    if (!validateClipGroupMove(session, selected, 0, 0, error)) {
        return false;
    }
    if ((!setStart && !setEnd) || (setStart && (start < 0 || start > maxPreciseSample)) ||
        (setEnd && (end < 0 || end > maxPreciseSample))) {
        error = "Enter a changed, valid sample boundary.";
        return false;
    }
    if (std::none_of(selected.begin(), selected.end(), [&](const auto& ref) {
            return ref.track == anchor.track && ref.clip == anchor.clip;
        })) {
        error = "The anchor clip is not selected.";
        return false;
    }

    const auto& anchorClip =
        session.tracks[static_cast<size_t>(anchor.track)].clips[static_cast<size_t>(anchor.clip)];
    const double anchorEnd = anchorClip.start + anchorClip.duration;
    const double newStart = setStart ? static_cast<double>(start) / sessionRate : anchorClip.start;
    const double newEnd = setEnd ? static_cast<double>(end) / sessionRate : anchorEnd;
    if (!(newEnd > newStart)) {
        error = "The exclusive end must follow the start.";
        return false;
    }

    const double startShift = newStart - anchorClip.start;
    const double endShift = newEnd - anchorEnd;
    if (std::equal_to<double>{}(startShift, 0.0) && std::equal_to<double>{}(endShift, 0.0)) {
        error = "Clip boundaries are unchanged.";
        return false;
    }

    const double anchorFirstFrame = anchorClip.start * sessionRate;
    const double anchorLastFrame = anchorEnd * sessionRate;
    const double anchorFirst = std::round(anchorFirstFrame);
    const double anchorLast = std::round(anchorLastFrame);
    const bool anchorGrid = std::abs(anchorFirstFrame - anchorFirst) < 1e-7 &&
                            std::abs(anchorLastFrame - anchorLast) < 1e-7;

    std::vector<Clip> result;
    result.reserve(selected.size());
    for (const auto& ref : selected) {
        const auto& original =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        auto adjusted = original;
        const double originalEnd = original.start + original.duration;
        const double firstFrame = original.start * sessionRate;
        const double lastFrame = originalEnd * sessionRate;
        const double first = std::round(firstFrame);
        const double last = std::round(lastFrame);

        // An untouched edge may be very close to the sample grid without
        // being bit-identical to it. Do not quantize it via the grid helper.
        const bool grid = setStart && setEnd && anchorGrid && std::abs(firstFrame - first) < 1e-7 &&
                          std::abs(lastFrame - last) < 1e-7;
        if (grid) {
            const double a = first + (setStart ? static_cast<double>(start) - anchorFirst : 0.0);
            const double b = last + (setEnd ? static_cast<double>(end) - anchorLast : 0.0);
            if (a < 0 || b > static_cast<double>(maxPreciseSample) || b <= a) {
                error = "A grouped clip would have invalid sample boundaries.";
                return false;
            }
            if (!trimClipToSamples(adjusted, static_cast<juce::int64>(a),
                                   static_cast<juce::int64>(b), error)) {
                return false;
            }
        } else {
            const bool isAnchor = ref.track == anchor.track && ref.clip == anchor.clip;
            adjusted.start = isAnchor ? newStart : original.start + startShift;
            const double targetEnd = isAnchor ? newEnd : originalEnd + endShift;
            adjusted.offset = original.offset + (adjusted.start - original.start);
            adjusted.duration = targetEnd - adjusted.start;

            const double sourceLength = original.audio->length();
            const double tolerance = std::min(
                1.0e-4 / sessionRate, 8.0 * std::numeric_limits<double>::epsilon() *
                                          std::max({1.0, std::abs(adjusted.offset),
                                                    std::abs(adjusted.duration), sourceLength}));
            if (adjusted.offset < 0 && adjusted.offset >= -tolerance) {
                adjusted.offset = 0;
            }

            juce::int64 firstSample = 0;
            juce::int64 lastSample = 0;
            if (!validTimelineGeometry(adjusted.start, adjusted.offset, adjusted.duration) ||
                adjusted.offset + adjusted.duration > sourceLength + tolerance ||
                !clipSampleRange(adjusted, firstSample, lastSample) ||
                std::abs(((adjusted.start - original.start) - startShift) * sessionRate) > 1e-4 ||
                std::abs((((adjusted.start + adjusted.duration) - originalEnd) - endShift) *
                         sessionRate) > 1e-4) {
                error = "A grouped clip cannot preserve the requested edge movement within its "
                        "source and timeline bounds.";
                return false;
            }
        }

        result.push_back(std::move(adjusted));
    }

    output = std::move(result);
    return true;
}

} // namespace

bool trimClipGroupToAnchorRange(Session& session, const std::vector<ClipReference>& selected,
                                ClipReference anchor, bool setStart, juce::int64 start, bool setEnd,
                                juce::int64 end, juce::String& error) {
    std::vector<Clip> adjusted;
    if (!previewClipGroupAnchorRange(session, selected, anchor, setStart, start, setEnd, end,
                                     adjusted, error)) {
        return false;
    }

    auto candidate = session;
    for (size_t i = 0; i < selected.size(); ++i) {
        const auto& ref = selected[i];
        candidate.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)] =
            std::move(adjusted[i]);
    }
    if (!preservesMarkerBounds(session, candidate, error)) {
        return false;
    }

    session = std::move(candidate);
    return true;
}

bool trimClipUnitsToSample(Session& session, const std::vector<ClipReference>& selected,
                           ClipReference focus, bool endEdge, juce::int64 sample,
                           juce::String& error) {
    if (sample < 0 || sample > maxPreciseSample) {
        error = "Invalid project sample.";
        return false;
    }

    std::vector<ArrangementUnit> units;
    if (!arrangementUnits(session, selected, units, error)) {
        return false;
    }

    std::vector<std::pair<ClipReference, Clip>> updates;
    for (const auto& unit : units) {
        auto anchor = unit.clips.front();
        const auto hasFocus =
            std::find_if(unit.clips.begin(), unit.clips.end(), [&](const auto& ref) {
                return ref.track == focus.track && ref.clip == focus.clip;
            });
        if (hasFocus != unit.clips.end()) {
            anchor = *hasFocus;
        } else {
            for (const auto& ref : unit.clips) {
                const auto& clip = session.tracks[static_cast<size_t>(ref.track)]
                                       .clips[static_cast<size_t>(ref.clip)];
                const auto& chosen = session.tracks[static_cast<size_t>(anchor.track)]
                                         .clips[static_cast<size_t>(anchor.clip)];
                const double edge = clip.start + (endEdge ? clip.duration : 0.0);
                const double chosenEdge = chosen.start + (endEdge ? chosen.duration : 0.0);
                if (endEdge ? edge > chosenEdge : edge < chosenEdge) {
                    anchor = ref;
                }
            }
        }

        const auto& clip = session.tracks[static_cast<size_t>(anchor.track)]
                               .clips[static_cast<size_t>(anchor.clip)];
        const double edge = (clip.start + (endEdge ? clip.duration : 0.0)) * sessionRate;
        if (std::abs(edge - static_cast<double>(sample)) < 1e-7) {
            continue;
        }

        std::vector<Clip> adjusted;
        if (!previewClipGroupAnchorRange(session, unit.clips, anchor, !endEdge, sample, endEdge,
                                         sample, adjusted, error)) {
            return false;
        }
        for (size_t i = 0; i < unit.clips.size(); ++i) {
            updates.emplace_back(unit.clips[i], std::move(adjusted[i]));
        }
    }

    if (updates.empty()) {
        error = "Selected clip edges are already at the playhead.";
        return false;
    }

    auto candidate = session;
    for (auto& update : updates) {
        candidate.tracks[static_cast<size_t>(update.first.track)]
            .clips[static_cast<size_t>(update.first.clip)] = std::move(update.second);
    }
    if (!preservesMarkerBounds(session, candidate, error)) {
        return false;
    }

    session = std::move(candidate);
    return true;
}

bool trimClipGroupEdgesToSample(Session& session, const std::vector<ClipReference>& selected,
                                bool endEdge, juce::int64 sample, juce::String& error) {
    if (!validateClipGroupMove(session, selected, 0, 0, error)) {
        return false;
    }
    if (sample < 0 || sample > maxPreciseSample) {
        error = "Invalid project sample.";
        return false;
    }

    std::vector<Clip> adjusted;
    adjusted.reserve(selected.size());
    bool changedAny = false;

    for (const auto& ref : selected) {
        const auto& original =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        auto clip = original;
        const double oldEnd = original.start + original.duration;
        const double oldEdge = endEdge ? oldEnd : original.start;
        if (std::abs(oldEdge * sessionRate - static_cast<double>(sample)) < 1.0e-7) {
            adjusted.push_back(std::move(clip));
            continue;
        }

        const double target = static_cast<double>(sample) / sessionRate;
        const double firstFrame = original.start * sessionRate;
        const double lastFrame = oldEnd * sessionRate;
        const double roundedFirst = std::round(firstFrame);
        const double roundedLast = std::round(lastFrame);
        const bool onGrid = std::abs(firstFrame - roundedFirst) < 1e-7 &&
                            std::abs(lastFrame - roundedLast) < 1e-7 && roundedFirst >= 0 &&
                            roundedLast <= static_cast<double>(maxPreciseSample);

        if (onGrid) {
            const auto first = endEdge ? static_cast<juce::int64>(roundedFirst) : sample;
            const auto last = endEdge ? sample : static_cast<juce::int64>(roundedLast);
            if (!trimClipToSamples(clip, first, last, error)) {
                return false;
            }
        } else {
            if (endEdge) {
                clip.duration = target - original.start;
            } else {
                const double shift = target - original.start;
                clip.start = target;
                clip.offset = original.offset + shift;
                clip.duration = oldEnd - target;
            }

            const double sourceLength = clip.audio->length();
            const double tolerance = std::min(
                1.0e-4 / sessionRate,
                8.0 * std::numeric_limits<double>::epsilon() *
                    std::max({1.0, std::abs(clip.offset), std::abs(clip.duration), sourceLength}));
            if (clip.offset < 0 && clip.offset >= -tolerance) {
                clip.offset = 0;
            }

            juce::int64 first = 0;
            juce::int64 last = 0;
            if (!validTimelineGeometry(clip.start, clip.offset, clip.duration) ||
                clip.offset + clip.duration > sourceLength + tolerance ||
                !clipSampleRange(clip, first, last)) {
                error = "A selected clip cannot reach that edge within its source and timeline "
                        "bounds.";
                return false;
            }
        }

        changedAny = true;
        adjusted.push_back(std::move(clip));
    }

    if (!changedAny) {
        error = "Selected clip edges are already at that sample.";
        return false;
    }

    for (size_t i = 0; i < selected.size(); ++i) {
        const auto& ref = selected[i];
        session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)] =
            std::move(adjusted[i]);
    }
    return true;
}

bool duplicateClipAfter(Session& session, int track, int& clip, juce::String& error) {
    error.clear();
    if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
        error = "Select a clip first.";
        return false;
    }

    auto& clips = session.tracks[static_cast<size_t>(track)].clips;
    if (clip < 0 || clip >= static_cast<int>(clips.size())) {
        error = "Select a clip first.";
        return false;
    }

    auto copy = clips[static_cast<size_t>(clip)];
    copy.start += copy.duration;
    if (!validClipForSampleEdit(copy) ||
        !validTimelineGeometry(copy.start, copy.offset, copy.duration)) {
        error = "Duplicated clip would extend beyond the precise timeline range.";
        return false;
    }
    if (!validClipGroupId(copy.groupId)) {
        error = "Invalid clip group identity.";
        return false;
    }

    copy.groupId.clear();
    clips.push_back(std::move(copy));
    clip = static_cast<int>(clips.size()) - 1;
    return true;
}

namespace {

template <class Unsigned> Unsigned saturatingAdd(Unsigned a, Unsigned b) noexcept {
    constexpr Unsigned limit = std::numeric_limits<Unsigned>::max();
    return b > limit - a ? limit : a + b;
}

template <class Unsigned> Unsigned saturatingMul(Unsigned count, Unsigned item) noexcept {
    constexpr Unsigned limit = std::numeric_limits<Unsigned>::max();
    return count > limit / item ? limit : count * item;
}

} // namespace

void EditHistory::reset() noexcept {
    undoStack.clear();
    redoStack.clear();
    currentRevision = 0;
    savedRevision = 0;
    nextRevision = 1;
    budgetPruned = false;
}

size_t EditHistory::estimate(const EditorState& state) noexcept {
    size_t total = sizeof(Entry);
    const auto add = [&](size_t n) { total = saturatingAdd(total, n); };
    const auto addVector = [&](size_t count, size_t item) { add(saturatingMul(count, item)); };
    const auto addText = [&](const juce::String& text) {
        add(text.getCharPointer().sizeInBytes());
    };

    addVector(state.session.tracks.capacity(), sizeof(Track));
    addVector(state.session.markers.capacity(), sizeof(Marker));
    addVector(state.selectedClips.capacity(), sizeof(ClipReference));
    addVector(state.selectedTracks.capacity(), sizeof(int));

    for (const auto& marker : state.session.markers) {
        addText(marker.name);
        addText(marker.id);
    }

    {
        const auto& md = state.session.mdDisc;
        const auto addString = [&](const std::string& text) { add(text.capacity()); };
        addString(md.halfWidthTitle);
        addString(md.fullWidthTitle);
        addString(md.discTimestamp);
        addVector(md.tracks.capacity(), sizeof(MdTrack));
        for (const auto& track : md.tracks) {
            addString(track.id);
            addString(track.halfWidthTitle);
            addString(track.fullWidthTitle);
            addString(track.timestamp);
            addVector(track.parts.capacity(), sizeof(MdPart));
        }
        addVector(md.groups.capacity(), sizeof(MdGroup));
        for (const auto& group : md.groups) {
            addString(group.halfWidthTitle);
            addString(group.fullWidthTitle);
        }
        for (const auto& sector : md.importedUtocSectors) {
            addVector(sector.capacity(), 1);
        }
    }

    for (const auto& track : state.session.tracks) {
        addText(track.name);
        addVector(track.clips.capacity(), sizeof(Clip));
        addVector(track.routing.capacity(), sizeof(StereoRoute));
        addVector(track.channelHeightWeights.capacity(), sizeof(int));
        for (const auto& clip : track.clips) {
            addText(clip.name);
            addText(clip.groupId);
        }
    }

    return total;
}

EditHistory::Entry EditHistory::makeEntry(EditorState&& state, juce::uint64 revision,
                                          juce::String action, bool renderGeometryStable) {
    Entry entry;
    entry.state = std::move(state);
    entry.revision = revision;
    entry.action = action.isNotEmpty() ? std::move(action) : juce::String("Edit");
    entry.renderGeometryStable = renderGeometryStable;

    std::unordered_set<const Audio*> seen;
    for (const auto& track : entry.state.session.tracks) {
        for (const auto& clip : track.clips) {
            if (clip.audio && seen.insert(clip.audio.get()).second) {
                entry.sources.push_back(clip.audio.get());
            }
        }
    }

    entry.bytes = estimate(entry.state);
    entry.bytes =
        saturatingAdd(entry.bytes, saturatingMul(entry.sources.capacity(), sizeof(const Audio*)));
    entry.bytes = saturatingAdd(entry.bytes, entry.action.getCharPointer().sizeInBytes());
    return entry;
}

size_t EditHistory::estimatedMetadataBytes() const noexcept {
    size_t total = 0;
    const auto add = [&](const auto& stack) {
        for (const auto& entry : stack) {
            total = saturatingAdd(total, entry.bytes);
        }
    };
    add(undoStack);
    add(redoStack);
    return total;
}

void EditHistory::discardDistantEntry() {
    // The back of each stack is the nearest action to the current state.
    if (redoStack.empty() || (!undoStack.empty() && undoStack.size() >= redoStack.size())) {
        undoStack.erase(undoStack.begin());
    } else {
        redoStack.erase(redoStack.begin());
    }
    budgetPruned = true;
}

void EditHistory::trimBudget() {
    while ((!undoStack.empty() || !redoStack.empty()) && estimatedMetadataBytes() > metadataLimit) {
        discardDistantEntry();
    }
}

size_t EditHistory::estimatedRetainedMediaBytes(const Session& current,
                                                const Clipboard& clipboard) const {
    std::unordered_set<const Audio*> excluded;
    std::unordered_set<const Audio*> counted;
    for (const auto& track : current.tracks) {
        for (const auto& clip : track.clips) {
            if (clip.audio) {
                excluded.insert(clip.audio.get());
            }
        }
    }
    for (const auto& row : clipboard.tracks) {
        for (const auto& clip : row) {
            if (clip.audio) {
                excluded.insert(clip.audio.get());
            }
        }
    }

    size_t total = 0;
    const auto add = [&](size_t count, size_t item) {
        total = saturatingAdd(total, saturatingMul(count, item));
    };

    const auto visit = [&](const auto& stack) {
        for (const auto& entry : stack) {
            for (const auto* audio : entry.sources) {
                if (!audio || excluded.count(audio) || !counted.insert(audio).second) {
                    continue;
                }
                if (!audio->mapped && audio->numChannels() > 0 && audio->numSamples() > 0) {
                    add(static_cast<size_t>(audio->numSamples()),
                        static_cast<size_t>(audio->numChannels()) * sizeof(float));
                }
                for (const auto& channel : audio->peakCache->peaks) {
                    add(channel.capacity(), sizeof(juce::Range<float>));
                }
                for (const auto& channel : audio->peakCache->levels) {
                    for (const auto& level : channel) {
                        add(level.capacity(), sizeof(juce::Range<float>));
                    }
                }
                add(audio->sourceCues.capacity(), sizeof(SourceCue));
                for (const auto& cue : audio->sourceCues) {
                    add(cue.name.getCharPointer().sizeInBytes(), 1);
                }
                if (audio->sourceCueError.isNotEmpty()) {
                    add(audio->sourceCueError.getCharPointer().sizeInBytes(), 1);
                }
            }
        }
    };

    visit(undoStack);
    visit(redoStack);
    return total;
}

juce::uint64 EditHistory::estimatedRetainedMappedBytes(const Session& current,
                                                       const Clipboard& clipboard) const {
    std::unordered_set<const MappedPcm*> excluded;
    std::unordered_set<const MappedPcm*> counted;
    for (const auto& track : current.tracks) {
        for (const auto& clip : track.clips) {
            if (clip.audio && clip.audio->mapped) {
                excluded.insert(clip.audio->mapped.get());
            }
        }
    }
    for (const auto& row : clipboard.tracks) {
        for (const auto& clip : row) {
            if (clip.audio && clip.audio->mapped) {
                excluded.insert(clip.audio->mapped.get());
            }
        }
    }

    juce::uint64 total = 0;
    const auto visit = [&](const auto& stack) {
        for (const auto& entry : stack) {
            for (const auto* audio : entry.sources) {
                const auto* pcm = audio->mapped.get();
                if (!pcm || excluded.count(pcm) || !counted.insert(pcm).second ||
                    pcm->channels <= 0 || pcm->frames <= 0) {
                    continue;
                }
                const auto channels = static_cast<juce::uint64>(pcm->channels);
                const auto frames = static_cast<juce::uint64>(pcm->frames);
                const auto bytes = saturatingMul(
                    channels, saturatingMul(frames, static_cast<juce::uint64>(sizeof(float))));
                total = saturatingAdd(total, bytes);
            }
        }
    };

    visit(undoStack);
    visit(redoStack);
    return total;
}

void EditHistory::enforceRetainedMediaBudget(const Session& current, const Clipboard& clipboard,
                                             size_t limitBytes, juce::uint64 mappedLimitBytes) {
    while ((!undoStack.empty() || !redoStack.empty()) &&
           (estimatedRetainedMediaBytes(current, clipboard) > limitBytes ||
            estimatedRetainedMappedBytes(current, clipboard) > mappedLimitBytes)) {
        discardDistantEntry();
    }
}

void EditHistory::record(EditorState before, juce::String action, bool renderGeometryStable) {
    if (undoStack.size() >= 40) {
        undoStack.erase(undoStack.begin());
    }
    undoStack.push_back(
        makeEntry(std::move(before), currentRevision, std::move(action), renderGeometryStable));
    redoStack.clear();
    currentRevision = nextRevision++;
    trimBudget();
}

bool EditHistory::undo(EditorState& current, bool* renderGeometryStable) {
    if (undoStack.empty()) {
        return false;
    }

    auto entry = std::move(undoStack.back());
    undoStack.pop_back();
    redoStack.push_back(makeEntry(std::move(current), currentRevision, std::move(entry.action),
                                  entry.renderGeometryStable));
    if (renderGeometryStable) {
        *renderGeometryStable = entry.renderGeometryStable;
    }
    current = std::move(entry.state);
    currentRevision = entry.revision;
    trimBudget();
    return true;
}

bool EditHistory::redo(EditorState& current, bool* renderGeometryStable) {
    if (redoStack.empty()) {
        return false;
    }

    auto entry = std::move(redoStack.back());
    redoStack.pop_back();
    undoStack.push_back(makeEntry(std::move(current), currentRevision, std::move(entry.action),
                                  entry.renderGeometryStable));
    if (renderGeometryStable) {
        *renderGeometryStable = entry.renderGeometryStable;
    }
    current = std::move(entry.state);
    currentRevision = entry.revision;
    trimBudget();
    return true;
}

std::vector<HistoryTarget> EditHistory::targets(bool redo) const {
    const auto& stack = redo ? redoStack : undoStack;
    std::vector<HistoryTarget> result;
    result.reserve(stack.size());
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        result.push_back({it->revision, it->action});
    }
    return result;
}

std::vector<Session> EditHistory::undoSessions() const {
    std::vector<Session> result;
    result.reserve(undoStack.size());
    for (const auto& entry : undoStack) {
        result.push_back(entry.state.session);
    }
    return result;
}

std::vector<Session> EditHistory::redoSessions() const {
    std::vector<Session> result;
    result.reserve(redoStack.size());
    for (const auto& entry : redoStack) {
        result.push_back(entry.state.session);
    }
    return result;
}

bool crossfadeClipPair(Session& session, ClipReference a, ClipReference b, juce::String& error,
                       juce::Range<double>* overlapRange) {
    error.clear();
    const auto valid = [&](ClipReference ref) {
        return ref.track >= 0 && ref.track < static_cast<int>(session.tracks.size()) &&
               ref.clip >= 0 &&
               ref.clip <
                   static_cast<int>(session.tracks[static_cast<size_t>(ref.track)].clips.size());
    };
    if (!valid(a) || !valid(b) || (a.track == b.track && a.clip == b.clip)) {
        error = "Select two distinct valid clips.";
        return false;
    }

    Clip* first = &session.tracks[static_cast<size_t>(a.track)].clips[static_cast<size_t>(a.clip)];
    Clip* second = &session.tracks[static_cast<size_t>(b.track)].clips[static_cast<size_t>(b.clip)];
    if (first->start > second->start) {
        std::swap(first, second);
    }

    const double endFirst = first->start + first->duration;
    const double endSecond = second->start + second->duration;
    if (!(first->start < second->start && second->start < endFirst && endFirst < endSecond)) {
        error = "Crossfade requires a partial overlap, without one clip containing the other.";
        return false;
    }

    juce::int64 overlapStart = 0;
    juce::int64 overlapEnd = 0;
    if (!clipSampleRangeWithin(*first, second->start, endFirst, overlapStart, overlapEnd)) {
        error = "The overlap contains no project samples.";
        return false;
    }

    const double overlap = endFirst - second->start;
    first->fadeOut = overlap;
    first->fadeOutEnd = first->offset + first->duration;
    second->fadeIn = overlap;
    second->fadeInStart = second->offset;
    if (overlapRange) {
        *overlapRange = {second->start, endFirst};
    }
    return true;
}

bool crossfadeOverlap(Session& session, int track, int selected, juce::String& error,
                      juce::Range<double>* overlapRange) {
    error.clear();
    if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
        error = "Select a clip first.";
        return false;
    }

    auto& clips = session.tracks[static_cast<size_t>(track)].clips;
    if (selected < 0 || selected >= static_cast<int>(clips.size())) {
        error = "Select a clip first.";
        return false;
    }

    const auto& chosen = clips[static_cast<size_t>(selected)];
    int other = -1;
    for (int i = 0; i < static_cast<int>(clips.size()); ++i) {
        if (i == selected) {
            continue;
        }
        const auto& c = clips[static_cast<size_t>(i)];
        if (c.start < chosen.start + chosen.duration && chosen.start < c.start + c.duration) {
            if (other >= 0) {
                error = "Select a clip overlapping exactly one other clip.";
                return false;
            }
            other = i;
        }
    }

    if (other < 0) {
        error = "The selected clip does not overlap another clip on this track.";
        return false;
    }

    return crossfadeClipPair(session, {track, selected}, {track, other}, error, overlapRange);
}

bool crossfadeAcrossTracks(Session& session, int track, int selected, juce::String& error,
                           juce::Range<double>* overlapRange) {
    error.clear();
    if (track < 0 || track >= static_cast<int>(session.tracks.size()) || selected < 0 ||
        selected >= static_cast<int>(session.tracks[static_cast<size_t>(track)].clips.size())) {
        error = "Select a clip first.";
        return false;
    }

    auto& chosen = session.tracks[static_cast<size_t>(track)].clips[static_cast<size_t>(selected)];
    int otherTrack = -1;
    int otherIndex = -1;
    for (int t = 0; t < static_cast<int>(session.tracks.size()); ++t) {
        if (t == track) {
            continue;
        }
        const auto& clips = session.tracks[static_cast<size_t>(t)].clips;
        for (int i = 0; i < static_cast<int>(clips.size()); ++i) {
            const auto& candidate = clips[static_cast<size_t>(i)];
            if (candidate.start < chosen.start + chosen.duration &&
                chosen.start < candidate.start + candidate.duration) {
                if (otherTrack >= 0) {
                    error = "Cross-track crossfade requires exactly one overlapping clip on "
                            "other tracks.";
                    return false;
                }
                otherTrack = t;
                otherIndex = i;
            }
        }
    }

    if (otherTrack < 0) {
        error = "No overlapping clip on another track.";
        return false;
    }

    const auto& other =
        session.tracks[static_cast<size_t>(otherTrack)].clips[static_cast<size_t>(otherIndex)];
    const double overlapStart = std::max(chosen.start, other.start);
    const double overlapEnd =
        std::min(chosen.start + chosen.duration, other.start + other.duration);
    for (const int t : {track, otherTrack}) {
        const auto& clips = session.tracks[static_cast<size_t>(t)].clips;
        for (int i = 0; i < static_cast<int>(clips.size()); ++i) {
            if ((t == track && i == selected) || (t == otherTrack && i == otherIndex)) {
                continue;
            }
            const auto& clip = clips[static_cast<size_t>(i)];
            if (clip.start < overlapEnd && clip.start + clip.duration > overlapStart) {
                error = "Additional clips overlap the crossfade; explicitly select the pair.";
                return false;
            }
        }
    }

    return crossfadeClipPair(session, {track, selected}, {otherTrack, otherIndex}, error,
                             overlapRange);
}
namespace {

bool joinGridSample(double time, double& sample) {
    const double frames = time * sessionRate;
    const double nearest = std::round(frames);
    if (!std::isfinite(frames) || frames < 0 || nearest > static_cast<double>(maxPreciseSample) ||
        std::abs(frames - nearest) >= 1.0e-7) {
        return false;
    }

    sample = nearest;
    return true;
}

bool joinBoundaryEqual(double a, double b) {
    if (std::equal_to<double>{}(a, b)) {
        return true;
    }

    double first = 0;
    double second = 0;
    return joinGridSample(a, first) && joinGridSample(b, second) &&
           std::equal_to<double>{}(first, second);
}

bool validateStereoTrack(const Session& session, int trackIndex, juce::String& error) {
    if (trackIndex < 0 || trackIndex >= static_cast<int>(session.tracks.size())) {
        error = "Select a track first.";
        return false;
    }

    const auto& track = session.tracks[static_cast<size_t>(trackIndex)];
    if (track.clips.empty()) {
        error = "The track has no stereo audio.";
        return false;
    }

    for (const auto& clip : track.clips) {
        if (!clip.audio || clip.audio->numChannels() != 2) {
            error = "Every clip on the track must have exactly two source channels.";
            return false;
        }
    }

    return true;
}

} // namespace

bool joinNextClip(Session& session, int track, int& selected, juce::String& error) {
    error.clear();
    if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
        error = "Select a clip first.";
        return false;
    }

    auto& clips = session.tracks[static_cast<size_t>(track)].clips;
    if (selected < 0 || selected >= static_cast<int>(clips.size())) {
        error = "Select a clip first.";
        return false;
    }

    const auto& first = clips[static_cast<size_t>(selected)];
    const double boundary = first.start + first.duration;
    int next = -1;
    for (int i = 0; i < static_cast<int>(clips.size()); ++i) {
        if (i == selected) {
            continue;
        }
        const auto& clip = clips[static_cast<size_t>(i)];
        if (joinBoundaryEqual(clip.start, boundary)) {
            if (next >= 0) {
                error = "More than one clip starts at this boundary.";
                return false;
            }
            next = i;
        }
    }

    if (next < 0) {
        error = "No clip starts exactly at the selected clip's end; gaps and overlaps cannot be "
                "joined.";
        return false;
    }

    const auto& second = clips[static_cast<size_t>(next)];
    const auto same = [](const auto& a, const auto& b) {
        return std::memcmp(&a, &b, sizeof(a)) == 0;
    };
    const double sourceEnd = first.offset + first.duration;
    double firstSample = 0;
    double boundarySample = 0;
    double nextSample = 0;
    double endSample = 0;
    const double end = second.start + second.duration;
    const bool grid = joinGridSample(first.start, firstSample) &&
                      joinGridSample(boundary, boundarySample) &&
                      joinGridSample(second.start, nextSample) && joinGridSample(end, endSample) &&
                      std::equal_to<double>{}(boundarySample, nextSample);
    const bool contiguous = std::equal_to<double>{}(sourceEnd, second.offset) ||
                            (grid && std::abs(sourceEnd - second.offset) * sessionRate < 1.0e-7);

    if (!first.audio || first.audio != second.audio || !contiguous ||
        !same(first.gain, second.gain) || !same(first.fadeIn, second.fadeIn) ||
        !same(first.fadeOut, second.fadeOut) || !same(first.fadeInStart, second.fadeInStart) ||
        !same(first.fadeOutEnd, second.fadeOutEnd) || first.fadeInShape != second.fadeInShape ||
        first.fadeOutShape != second.fadeOutShape || first.name != second.name ||
        first.groupId != second.groupId) {
        error = "Joining requires contiguous audio from the same source with identical names, gain "
                "and fades.";
        return false;
    }

    const double duration = grid ? (endSample - firstSample) / sessionRate : end - first.start;
    if (!std::isfinite(duration) || duration <= 0 ||
        !joinBoundaryEqual(first.start + duration, end)) {
        error = "The joined clip cannot preserve its exact endpoint.";
        return false;
    }

    clips[static_cast<size_t>(selected)].duration = duration;
    clips.erase(clips.begin() + next);
    if (next < selected) {
        --selected;
    }
    return true;
}

bool joinSelectedClipChains(Session& session, const std::vector<ClipReference>& selected,
                            juce::String& error, std::vector<ClipReference>* mapping) {
    if (!validateClipGroupMove(session, selected, 0, 0, error)) {
        return false;
    }

    std::map<int, std::vector<size_t>> rows;
    for (size_t i = 0; i < selected.size(); ++i) {
        rows[selected[i].track].push_back(i);
    }

    auto candidate = session;
    std::vector<ClipReference> result(selected.size());
    bool joined = false;

    for (auto& entry : rows) {
        const int track = entry.first;
        auto& order = entry.second;
        const auto& original = session.tracks[static_cast<size_t>(track)].clips;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const auto& left = original[static_cast<size_t>(selected[a].clip)];
            const auto& right = original[static_cast<size_t>(selected[b].clip)];
            return left.start < right.start ||
                   (!(right.start < left.start) && selected[a].clip < selected[b].clip);
        });

        std::vector<int> indices;
        indices.reserve(order.size());
        for (size_t item : order) {
            indices.push_back(selected[item].clip);
        }

        for (size_t j = 1; j < indices.size(); ++j) {
            int anchor = indices[j - 1];
            const int next = indices[j];
            const auto& clips = candidate.tracks[static_cast<size_t>(track)].clips;
            const double boundary = clips[static_cast<size_t>(anchor)].start +
                                    clips[static_cast<size_t>(anchor)].duration;
            const double nextStart = clips[static_cast<size_t>(next)].start;
            if (!joinBoundaryEqual(nextStart, boundary)) {
                if (nextStart > boundary) {
                    continue;
                }
                error = "Overlapping selected clips cannot be joined.";
                return false;
            }
            if (!joinNextClip(candidate, track, anchor, error)) {
                return false;
            }
            for (size_t k = 0; k < indices.size(); ++k) {
                if (k != j && indices[k] > next) {
                    --indices[k];
                }
            }
            indices[j] = anchor;
            joined = true;
        }

        for (size_t i = 0; i < order.size(); ++i) {
            result[order[i]] = {track, indices[i]};
        }
    }

    if (!joined) {
        error = "Select at least two contiguous clips on one track.";
        return false;
    }

    session = std::move(candidate);
    if (mapping) {
        *mapping = std::move(result);
    }
    return true;
}

bool swapStereoSourceChannels(Session& session, int trackIndex, juce::String& error) {
    error.clear();
    if (!validateStereoTrack(session, trackIndex, error)) {
        return false;
    }

    auto& track = session.tracks[static_cast<size_t>(trackIndex)];
    auto routing = track.routing;
    while (routing.size() < 2) {
        routing.push_back(defaultChannelRoute(static_cast<int>(routing.size()), 2));
    }

    if (std::memcmp(&routing[0].left, &routing[1].left, sizeof(float)) == 0 &&
        std::memcmp(&routing[0].right, &routing[1].right, sizeof(float)) == 0) {
        error = "The source channels already have identical routing.";
        return false;
    }

    std::swap(routing[0], routing[1]);
    track.routing = std::move(routing);
    return true;
}

bool foldStereoTrackToMono(Session& session, int trackIndex, juce::String& error) {
    error.clear();
    if (!validateStereoTrack(session, trackIndex, error)) {
        return false;
    }

    auto& track = session.tracks[static_cast<size_t>(trackIndex)];
    auto routing = track.routing;
    while (routing.size() < 2) {
        routing.push_back(defaultChannelRoute(static_cast<int>(routing.size()), 2));
    }

    bool changed = false;
    for (int i = 0; i < 2; ++i) {
        auto& route = routing[static_cast<size_t>(i)];
        if (std::memcmp(&route.left, &route.right, sizeof(float)) != 0) {
            changed = true;
        }
        const float mono = (route.left + route.right) * 0.5f;
        route.left = mono;
        route.right = mono;
    }

    if (!changed) {
        error = "The track routing is already mono.";
        return false;
    }

    track.routing = std::move(routing);
    return true;
}

bool replaceVerifiedSource(Session& session, const std::shared_ptr<Audio>& original,
                           const std::shared_ptr<Audio>& replacement, juce::String& error) {
    error.clear();
    if (!original || !replacement || original == replacement ||
        original->file == replacement->file || original->sourceHash.length() != 64 ||
        original->sourceHash != replacement->sourceHash ||
        original->numChannels() != replacement->numChannels() ||
        original->numSamples() != replacement->numSamples() ||
        std::memcmp(&original->sampleRate, &replacement->sampleRate, sizeof(double)) != 0) {
        error = "The replacement source is invalid or already in use.";
        return false;
    }

    size_t count = 0;
    for (const auto& track : session.tracks) {
        for (const auto& clip : track.clips) {
            if (clip.audio == original) {
                ++count;
            }
        }
    }
    if (count == 0) {
        error = "The original source is no longer used by this project.";
        return false;
    }

    for (auto& track : session.tracks) {
        for (auto& clip : track.clips) {
            if (clip.audio == original) {
                clip.audio = replacement;
            }
        }
    }
    return true;
}

bool splitStereoTrackChannels(Session& session, int trackIndex, juce::String& error) {
    error.clear();
    if (!validateStereoTrack(session, trackIndex, error)) {
        return false;
    }

    const auto& original = session.tracks[static_cast<size_t>(trackIndex)];
    if (isolatedDisplayChannel(original) >= 0) {
        error = "This track already displays an isolated source channel.";
        return false;
    }

    auto left = original;
    auto right = original;
    while (left.routing.size() < 2) {
        left.routing.push_back(defaultChannelRoute(static_cast<int>(left.routing.size()), 2));
    }
    right.routing = left.routing;
    left.routing[1] = {0, 0};
    right.routing[0] = {0, 0};
    left.displaySourceChannel = 0;
    right.displaySourceChannel = 1;

    const auto channelSuffix = juce::String::fromUTF8(" — channel ");
    left.name = original.name + channelSuffix + "1";
    right.name = original.name + channelSuffix + "2";

    session.tracks.insert(session.tracks.begin() + trackIndex + 1, std::move(right));
    session.tracks[static_cast<size_t>(trackIndex)] = std::move(left);
    return true;
}

bool combineStereoChannelTracks(Session& session, int firstTrack, juce::String& error) {
    error.clear();
    if (firstTrack < 0 || firstTrack >= static_cast<int>(session.tracks.size()) - 1) {
        error = "Select the first of two adjacent source-channel tracks.";
        return false;
    }

    const auto& first = session.tracks[static_cast<size_t>(firstTrack)];
    const auto& second = session.tracks[static_cast<size_t>(firstTrack + 1)];
    const int firstChannel = isolatedDisplayChannel(first);
    const int secondChannel = isolatedDisplayChannel(second);
    if (firstChannel < 0 || secondChannel < 0 || firstChannel == secondChannel) {
        error = "Adjacent tracks must isolate different channels of stereo sources.";
        return false;
    }

    const auto same = [](const auto& a, const auto& b) {
        return std::memcmp(&a, &b, sizeof(a)) == 0;
    };
    if (first.muted != second.muted || first.solo != second.solo ||
        !same(first.gain, second.gain) || !same(first.pan, second.pan) ||
        first.height != second.height || first.collapsed != second.collapsed ||
        first.view != second.view || first.waveformZoom != second.waveformZoom ||
        first.channelHeightWeights != second.channelHeightWeights ||
        first.routing.size() != second.routing.size()) {
        error = "Track mix, display settings and routing sizes must match before combining.";
        return false;
    }

    for (size_t channel = 2; channel < first.routing.size(); ++channel) {
        if (!same(first.routing[channel].left, second.routing[channel].left) ||
            !same(first.routing[channel].right, second.routing[channel].right)) {
            error = "Extra routing coefficients differ between the tracks.";
            return false;
        }
    }

    if (first.clips.size() != second.clips.size()) {
        error = "Both tracks must contain matching clips in the same order.";
        return false;
    }

    for (size_t index = 0; index < first.clips.size(); ++index) {
        const auto& a = first.clips[index];
        const auto& b = second.clips[index];
        if (a.audio != b.audio || a.name != b.name || a.groupId != b.groupId ||
            !same(a.start, b.start) || !same(a.offset, b.offset) || !same(a.duration, b.duration) ||
            !same(a.gain, b.gain) || !same(a.fadeIn, b.fadeIn) || !same(a.fadeOut, b.fadeOut) ||
            !same(a.fadeInStart, b.fadeInStart) || !same(a.fadeOutEnd, b.fadeOutEnd) ||
            a.fadeInShape != b.fadeInShape || a.fadeOutShape != b.fadeOutShape) {
            error =
                "Both tracks must contain matching sources, clip positions, names, gain and fades.";
            return false;
        }
    }

    auto merged = first;
    const auto& channelZero = firstChannel == 0 ? first : second;
    const auto& channelOne = firstChannel == 1 ? first : second;
    merged.routing[0] = channelZero.routing[0];
    merged.routing[1] = channelOne.routing[1];
    merged.displaySourceChannel = -1;

    const auto channelSuffix = juce::String::fromUTF8(" — channel ");
    const auto firstSuffix = channelSuffix + juce::String(firstChannel + 1);
    const auto secondSuffix = channelSuffix + juce::String(secondChannel + 1);
    const auto firstBase = first.name.endsWith(firstSuffix)
                               ? first.name.dropLastCharacters(firstSuffix.length())
                               : first.name;
    const auto secondBase = second.name.endsWith(secondSuffix)
                                ? second.name.dropLastCharacters(secondSuffix.length())
                                : second.name;
    merged.name = firstBase == secondBase ? firstBase : first.name + " / " + second.name;

    session.tracks[static_cast<size_t>(firstTrack)] = std::move(merged);
    session.tracks.erase(session.tracks.begin() + firstTrack + 1);
    return true;
}

bool setTrackFlag(Session& session, TrackRange range, TrackFlag flag, bool value) noexcept {
    if (range.first < 0 || range.last < range.first ||
        range.last >= static_cast<int>(session.tracks.size()) ||
        (flag != TrackFlag::mute && flag != TrackFlag::solo)) {
        return false;
    }

    bool changed = false;
    for (int t = range.first; t <= range.last; ++t) {
        auto& track = session.tracks[static_cast<size_t>(t)];
        auto& target = flag == TrackFlag::mute ? track.muted : track.solo;
        if (target != value) {
            target = value;
            changed = true;
        }
    }

    return changed;
}

bool setTrackFlags(Session& session, const std::vector<int>& tracks, TrackFlag flag, bool value) {
    if (tracks.empty() || (flag != TrackFlag::mute && flag != TrackFlag::solo)) {
        return false;
    }

    std::set<int> seen;
    for (int index : tracks) {
        if (index < 0 || index >= static_cast<int>(session.tracks.size()) ||
            !seen.insert(index).second) {
            return false;
        }
    }

    bool changed = false;
    for (int index : tracks) {
        auto& track = session.tracks[static_cast<size_t>(index)];
        auto& target = flag == TrackFlag::mute ? track.muted : track.solo;
        if (target != value) {
            target = value;
            changed = true;
        }
    }

    return changed;
}

bool limitToTracks(Session& session, int firstTrack, int lastTrack) {
    if (firstTrack < 0 || lastTrack < firstTrack ||
        lastTrack >= static_cast<int>(session.tracks.size())) {
        return false;
    }

    for (int t = 0; t < static_cast<int>(session.tracks.size()); ++t) {
        if (t < firstTrack || t > lastTrack) {
            auto& track = session.tracks[static_cast<size_t>(t)];
            track.muted = true;
            track.solo = false;
        }
    }

    return true;
}

bool limitToTrackIndices(Session& session, const std::vector<int>& tracks) {
    if (tracks.empty()) {
        return false;
    }

    std::set<int> chosen;
    for (int track : tracks) {
        if (track < 0 || track >= static_cast<int>(session.tracks.size()) ||
            !chosen.insert(track).second) {
            return false;
        }
    }

    for (int t = 0; t < static_cast<int>(session.tracks.size()); ++t) {
        if (chosen.count(t) == 0) {
            auto& track = session.tracks[static_cast<size_t>(t)];
            track.muted = true;
            track.solo = false;
        }
    }

    return true;
}

bool isolateClip(Session& session, int track, int clip, juce::int64& start, juce::int64& end) {
    if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
        return false;
    }

    auto& clips = session.tracks[static_cast<size_t>(track)].clips;
    if (clip < 0 || clip >= static_cast<int>(clips.size())) {
        return false;
    }

    juce::int64 first = 0;
    juce::int64 last = 0;
    if (!clipSampleRange(clips[static_cast<size_t>(clip)], first, last)) {
        return false;
    }

    std::vector<Clip> isolated;
    isolated.push_back(clips[static_cast<size_t>(clip)]);
    clips = std::move(isolated);
    limitToTracks(session, track, track);
    start = first;
    end = last;
    return true;
}

bool isolateClipGroup(Session& session, const std::vector<ClipReference>& selected,
                      juce::int64& start, juce::int64& end, juce::String& error) {
    if (!validateClipGroupMove(session, selected, 0, 0, error)) {
        return false;
    }

    std::set<std::pair<int, int>> refs;
    juce::int64 first = std::numeric_limits<juce::int64>::max();
    juce::int64 last = 0;
    for (const auto& ref : selected) {
        juce::int64 a = 0;
        juce::int64 b = 0;
        if (!clipSampleRange(
                session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)],
                a, b)) {
            error = "A selected clip contains no project sample positions.";
            return false;
        }
        refs.emplace(ref.track, ref.clip);
        first = std::min(first, a);
        last = std::max(last, b);
    }

    auto isolated = session;
    for (size_t t = 0; t < isolated.tracks.size(); ++t) {
        auto& track = isolated.tracks[t];
        std::vector<Clip> kept;
        for (size_t c = 0; c < track.clips.size(); ++c) {
            if (refs.count({static_cast<int>(t), static_cast<int>(c)})) {
                kept.push_back(track.clips[c]);
            }
        }
        if (kept.empty()) {
            track.muted = true;
            track.solo = false;
        } else {
            track.clips = std::move(kept);
        }
    }

    session = std::move(isolated);
    start = first;
    end = last;
    return true;
}

namespace {

bool consolidationInfo(const Session& session, const std::vector<ClipReference>& selected,
                       std::vector<ClipReference>& ordered, int& track, int& channels, bool& mixed,
                       juce::int64& start, juce::int64& end, juce::String& error) {
    std::vector<ClipReference> expanded;
    if (!expandClipGroups(session, selected, expanded, error)) {
        return false;
    }
    if (expanded.size() != selected.size()) {
        error = "Select every member of each clip group before consolidating.";
        return false;
    }
    if (expanded.size() < 2) {
        error = "Select at least two clips to consolidate.";
        return false;
    }

    const int chosenTrack = expanded.front().track;
    int sourceChannels = 0;
    bool mixedChannels = false;
    juce::int64 first = std::numeric_limits<juce::int64>::max();
    juce::int64 last = 0;

    for (const auto& ref : expanded) {
        if (ref.track != chosenTrack) {
            error = "Consolidation requires clips on one track; ungroup cross-track clips first.";
            return false;
        }
        const auto& clip =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        if (!clip.audio || clip.audio->numChannels() < 1 || clip.audio->numChannels() > 1024) {
            error = "Unsupported source channel count for consolidation.";
            return false;
        }
        if (sourceChannels == 0) {
            sourceChannels = clip.audio->numChannels();
        } else if (sourceChannels != clip.audio->numChannels()) {
            mixedChannels = true;
        }
        juce::int64 a = 0;
        juce::int64 b = 0;
        if (!clipSampleRange(clip, a, b)) {
            error = "A selected clip contains no project samples.";
            return false;
        }
        first = std::min(first, a);
        last = std::max(last, b);
    }

    if (mixedChannels && std::any_of(expanded.begin(), expanded.end(), [&](const auto& ref) {
            return session.tracks[static_cast<size_t>(ref.track)]
                       .clips[static_cast<size_t>(ref.clip)]
                       .audio->numChannels() > 2;
        })) {
        error = "Multichannel consolidation requires matching source channel counts.";
        return false;
    }

    ordered = std::move(expanded);
    track = chosenTrack;
    channels = mixedChannels ? 2 : sourceChannels;
    mixed = mixedChannels;
    start = first;
    end = last;
    return true;
}

} // namespace

bool prepareMonoStereoPair(const Session& session, int leftTrack, int rightTrack, Session& output,
                           juce::String& error, juce::int64* startSample, juce::int64* endSample) {
    error.clear();
    if (leftTrack < 0 || rightTrack < 0 || leftTrack == rightTrack ||
        leftTrack >= static_cast<int>(session.tracks.size()) ||
        rightTrack >= static_cast<int>(session.tracks.size())) {
        error = "Choose two distinct tracks for the left and right channels.";
        return false;
    }

    juce::int64 first = std::numeric_limits<juce::int64>::max();
    juce::int64 last = 0;
    for (int index : {leftTrack, rightTrack}) {
        const auto& track = session.tracks[static_cast<size_t>(index)];
        if (track.clips.empty()) {
            error = "Both mono tracks must contain audio clips.";
            return false;
        }
        bool hasSamples = false;
        for (const auto& clip : track.clips) {
            if (!clip.audio || clip.audio->numChannels() != 1) {
                error = "Both tracks must contain only mono source clips.";
                return false;
            }
            juce::int64 a = 0;
            juce::int64 b = 0;
            if (clipSampleRange(clip, a, b)) {
                first = std::min(first, a);
                last = std::max(last, b);
                hasSamples = true;
            }
        }
        if (!hasSamples) {
            error = "Both tracks must contain clips with project-sample positions.";
            return false;
        }
    }

    auto next = session;
    for (size_t index = 0; index < next.tracks.size(); ++index) {
        auto& track = next.tracks[index];
        track.solo = false;
        const bool left = static_cast<int>(index) == leftTrack;
        const bool right = static_cast<int>(index) == rightTrack;
        track.muted = !left && !right;
        if (left || right) {
            track.pan = 0;
            track.routing = {{left ? 1.0f : 0.0f, right ? 1.0f : 0.0f}};
        }
    }

    output = std::move(next);
    if (startSample) {
        *startSample = first;
    }
    if (endSample) {
        *endSample = last;
    }
    return true;
}

bool prepareClipConsolidation(const Session& session, const std::vector<ClipReference>& selected,
                              Session& render, int& track, int& channels, juce::int64& start,
                              juce::int64& end, juce::String& error) {
    error.clear();
    std::vector<ClipReference> ordered;
    int row = -1;
    int count = 0;
    bool mixed = false;
    juce::int64 first = 0;
    juce::int64 last = 0;
    if (!consolidationInfo(session, selected, ordered, row, count, mixed, first, last, error)) {
        return false;
    }

    Session next;
    Track raw = session.tracks[static_cast<size_t>(row)];
    raw.clips.clear();
    raw.muted = false;
    raw.solo = false;
    raw.gain = 1;
    raw.pan = 0;
    raw.routing.clear();

    // Explicit track routing treats mono as source channel zero; retain that
    // distinction in the stereo intermediate so the original matrix still applies.
    if (mixed && !session.tracks[static_cast<size_t>(row)].routing.empty()) {
        raw.routing = {{1, 0}, {0, 1}};
    }

    for (const auto& ref : ordered) {
        raw.clips.push_back(
            session.tracks[static_cast<size_t>(row)].clips[static_cast<size_t>(ref.clip)]);
    }

    next.tracks.push_back(std::move(raw));
    render = std::move(next);
    track = row;
    channels = count;
    start = first;
    end = last;
    return true;
}

bool replaceClipsWithConsolidation(Session& session, const std::vector<ClipReference>& selected,
                                   const std::shared_ptr<Audio>& audio, juce::int64 start,
                                   juce::int64 end, int& newClip, juce::String& error) {
    error.clear();
    std::vector<ClipReference> ordered;
    int row = -1;
    int count = 0;
    bool mixed = false;
    juce::int64 first = 0;
    juce::int64 last = 0;
    if (!consolidationInfo(session, selected, ordered, row, count, mixed, first, last, error)) {
        return false;
    }

    if (!audio || first != start || last != end || audio->numChannels() != count ||
        !std::equal_to<double>{}(audio->sampleRate, sessionRate) ||
        static_cast<juce::int64>(audio->numSamples()) != end - start) {
        error = "Rendered media does not match the selected clips.";
        return false;
    }

    auto candidate = session;
    auto& clips = candidate.tracks[static_cast<size_t>(row)].clips;
    std::vector<Clip> replacement;
    replacement.reserve(clips.size() - ordered.size() + 1);

    size_t selectedIndex = 0;
    int inserted = -1;
    for (size_t i = 0; i < clips.size(); ++i) {
        if (selectedIndex < ordered.size() && static_cast<int>(i) == ordered[selectedIndex].clip) {
            if (inserted < 0) {
                inserted = static_cast<int>(replacement.size());
                replacement.push_back({audio, seconds(start), 0, seconds(end - start)});
            }
            ++selectedIndex;
        } else {
            replacement.push_back(clips[i]);
        }
    }

    clips = std::move(replacement);
    if (!preservesMarkerBounds(session, candidate, error)) {
        return false;
    }

    session = std::move(candidate);
    newClip = inserted;
    return true;
}

bool verifyClipConsolidation(const Session& snapshot, const std::vector<ClipReference>& selected,
                             const std::shared_ptr<Audio>& audio, juce::int64 start,
                             juce::int64 end, juce::String& error,
                             std::function<bool(double)> progress, bool allowRoutingChange) {
    error.clear();
    const auto keepGoing = [&](double fraction) {
        if (!progress || progress(fraction)) {
            return true;
        }
        error = "Consolidation cancelled during audio verification.";
        return false;
    };

    if (!keepGoing(0)) {
        return false;
    }
    if (selected.empty() || start < 0 || end <= start) {
        error = "Invalid consolidation verification snapshot.";
        return false;
    }

    const int target = selected.front().track;
    if (target < 0 || target >= static_cast<int>(snapshot.tracks.size())) {
        error = "Invalid consolidation verification track.";
        return false;
    }

    Session masterAfter = snapshot;
    int replacement = -1;
    if (!replaceClipsWithConsolidation(masterAfter, selected, audio, start, end, replacement,
                                       error)) {
        return false;
    }
    if (!keepGoing(0)) {
        return false;
    }

    Session before;
    Session after;
    before.tracks.push_back(snapshot.tracks[static_cast<size_t>(target)]);
    after.tracks.push_back(masterAfter.tracks[static_cast<size_t>(target)]);
    for (auto* isolated : {&before, &after}) {
        auto& track = isolated->tracks[0];
        track.muted = false;
        track.solo = false;
        track.gain = 1;
        track.pan = 0;
    }

    const auto& track = before.tracks[0];
    const int selectedChannels =
        track.clips[static_cast<size_t>(selected.front().clip)].audio->numChannels();
    bool mixed = false;
    for (const auto& ref : selected) {
        if (track.clips[static_cast<size_t>(ref.clip)].audio->numChannels() != selectedChannels) {
            mixed = true;
        }
    }

    const bool checkSource = !(mixed && allowRoutingChange);
    const bool checkAutomatic = mixed && !allowRoutingChange;
    Session automaticBefore;
    Session automaticAfter;
    if (checkAutomatic) {
        automaticBefore = before;
        automaticAfter = after;
        automaticBefore.tracks[0].routing.clear();
        automaticAfter.tracks[0].routing.clear();
    }

    const int rawChannels = std::max(maxTrackChannels(track), maxTrackChannels(after.tracks[0]));
    const int masterStart = 1 + (checkSource ? 1 : 0) + (checkAutomatic ? 1 : 0);
    const int passes = masterStart + 2;

    ResamplingContext beforeRates;
    ResamplingContext afterRates;
    snapshot.prepareResampling(beforeRates, sessionRate);
    masterAfter.prepareResampling(afterRates, sessionRate);
    if (!keepGoing(0)) {
        return false;
    }

    const RenderClipIndex beforeIndex(before);
    const RenderClipIndex afterIndex(after);
    const RenderClipIndex masterBeforeIndex(snapshot);
    const RenderClipIndex masterAfterIndex(masterAfter);
    if (!keepGoing(0)) {
        return false;
    }

    for (int pass = 0; pass < passes; ++pass) {
        const bool raw = checkSource && pass == 1;
        const bool automatic = checkAutomatic && pass == 1 + (checkSource ? 1 : 0);
        const int masterPass = pass - masterStart;
        const bool master = masterPass >= 0;
        const int channels = raw ? rawChannels : masterPass == 1 ? 1 : 2;
        const auto& leftSession = master ? snapshot : automatic ? automaticBefore : before;
        const auto& rightSession = master ? masterAfter : automatic ? automaticAfter : after;
        const auto* leftIndex = master ? &masterBeforeIndex : &beforeIndex;
        const auto* rightIndex = master ? &masterAfterIndex : &afterIndex;

        juce::AudioBuffer<float> left(channels, 4096);
        juce::AudioBuffer<float> right(channels, 4096);
        for (juce::int64 pos = start; pos < end;) {
            if (!keepGoing((static_cast<double>(pass) +
                            static_cast<double>(pos - start) / static_cast<double>(end - start)) /
                           passes)) {
                return false;
            }

            const int count = static_cast<int>(std::min<juce::int64>(4096, end - pos));
            left.clear();
            right.clear();
            bool unsupported = false;
            leftSession.render(left, 0, count, static_cast<double>(pos) / sessionRate, sessionRate,
                               channels, &beforeRates, {}, -1, leftIndex, &unsupported, raw);
            rightSession.render(right, 0, count, static_cast<double>(pos) / sessionRate,
                                sessionRate, channels, &afterRates, {}, -1, rightIndex,
                                &unsupported, raw);
            if (unsupported) {
                error = "Cannot verify consolidation at a source's sample rate.";
                return false;
            }

            for (int ch = 0; ch < channels; ++ch) {
                for (int i = 0; i < count; ++i) {
                    const double a = left.getSample(ch, i);
                    const double b = right.getSample(ch, i);
                    if (!std::isfinite(a) || !std::isfinite(b) ||
                        std::abs(a - b) > 1e-6 * std::max({1.0, std::abs(a), std::abs(b)})) {
                        error = (raw ? juce::String("Consolidation would change source-channel "
                                                    "audio beyond the verification tolerance at "
                                                    "project sample ")
                                 : automatic ? juce::String("Consolidation would change automatic "
                                                            "routing audio beyond the verification "
                                                            "tolerance at project sample ")
                                             : juce::String("Consolidation would change audio "
                                                            "beyond the verification tolerance at "
                                                            "project sample ")) +
                                juce::String(pos + i) + "; clips were not replaced.";
                        if (mixed && (raw || automatic)) {
                            error += " Render selected clips to a separate track to retain the "
                                     "originals.";
                        }
                        return false;
                    }
                }
            }
            pos += count;
        }
    }

    return keepGoing(1);
}

bool fitTrackHeights(Session& session, int availablePixels, bool& allVisible) {
    const auto collapsed = std::count_if(session.tracks.begin(), session.tracks.end(),
                                         [](const auto& track) { return track.collapsed; });
    const auto count = static_cast<juce::int64>(session.tracks.size()) - collapsed;
    const auto available = static_cast<juce::int64>(std::max(0, availablePixels));
    const auto remaining = std::max<juce::int64>(0, available - collapsed * collapsedTrackHeight);
    allVisible = availablePixels >= 0 && collapsed * collapsedTrackHeight <= available &&
                 count <= remaining / 72;
    if (count == 0) {
        return false;
    }

    const auto each = std::clamp(static_cast<int>(remaining / count), 72, 800);
    const auto remainder = each < 800 && allVisible ? remaining % count : 0;
    bool changed = false;
    juce::int64 expandedIndex = 0;
    for (auto& track : session.tracks) {
        if (track.collapsed) {
            continue;
        }
        const int height = each + (expandedIndex++ < remainder ? 1 : 0);
        if (track.height != height) {
            track.height = height;
            changed = true;
        }
    }

    return changed;
}

bool validateRetainedMediaDestination(const Session& current, const std::vector<Session>& undo,
                                      const std::vector<Session>& redo, const Clipboard& clipboard,
                                      const juce::File& target, juce::String& error,
                                      std::initializer_list<const Session*> background,
                                      const std::vector<juce::File>& pendingSources) {
    error.clear();
    std::set<const Audio*> checked;

    const auto checkClip = [&](const Clip& clip) {
        if (!clip.audio || !checked.insert(clip.audio.get()).second) {
            return true;
        }
        if (pathsReferToSameFile(clip.audio->file, target)) {
            error = "Choose an output other than retained source audio.";
            return false;
        }
        return true;
    };

    const auto checkSession = [&](const Session& session) {
        for (const auto& track : session.tracks) {
            for (const auto& clip : track.clips) {
                if (!checkClip(clip)) {
                    return false;
                }
            }
        }
        return true;
    };

    if (!checkSession(current)) {
        return false;
    }
    for (const auto* snapshot : background) {
        if (snapshot && !checkSession(*snapshot)) {
            return false;
        }
    }
    for (const auto& state : undo) {
        if (!checkSession(state)) {
            return false;
        }
    }
    for (const auto& state : redo) {
        if (!checkSession(state)) {
            return false;
        }
    }
    for (const auto& track : clipboard.tracks) {
        for (const auto& clip : track) {
            if (!checkClip(clip)) {
                return false;
            }
        }
    }
    for (const auto& file : pendingSources) {
        if (pathsReferToSameFile(file, target)) {
            error = "Choose an output other than a file currently being read.";
            return false;
        }
    }

    return true;
}

bool validateMarkerSample(const Session& session, int marker, juce::int64 sample, juce::int64 total,
                          juce::String& error) {
    if (marker < 0 || marker >= static_cast<int>(session.markers.size())) {
        error = "Select a marker first.";
        return false;
    }
    if (sample < 0 || total <= 0 || sample >= total) {
        error = "Marker must be within the master duration.";
        return false;
    }

    for (size_t i = 0; i < session.markers.size(); ++i) {
        if (static_cast<int>(i) != marker && std::isfinite(session.markers[i].position) &&
            std::floor(session.markers[i].position * sessionRate + 0.5) ==
                static_cast<double>(sample)) {
            error = "Another marker already occupies that sample.";
            return false;
        }
    }

    return true;
}

bool setMarkerSample(Session& session, int marker, juce::int64 sample, juce::String& error) {
    if (!validateMarkerSample(session, marker, sample, projectEndSample(session), error)) {
        return false;
    }

    session.markers[static_cast<size_t>(marker)].position = seconds(sample);
    return true;
}

bool deleteMarkers(Session& session, const std::vector<int>& indices, juce::String& error) {
    error.clear();
    if (indices.empty()) {
        error = "Select at least one marker.";
        return false;
    }

    auto ordered = indices;
    std::sort(ordered.begin(), ordered.end());
    for (size_t i = 0; i < ordered.size(); ++i) {
        if (ordered[i] < 0 || static_cast<size_t>(ordered[i]) >= session.markers.size() ||
            (i > 0 && ordered[i] == ordered[i - 1])) {
            error = "The marker selection is invalid or contains duplicates.";
            return false;
        }
    }

    std::vector<Marker> kept;
    kept.reserve(session.markers.size() - ordered.size());
    size_t next = 0;
    for (size_t i = 0; i < session.markers.size(); ++i) {
        if (next < ordered.size() && i == static_cast<size_t>(ordered[next])) {
            ++next;
        } else {
            kept.push_back(session.markers[i]);
        }
    }

    session.markers = std::move(kept);
    return true;
}

bool findClipSourceSilence(const Clip& clip, juce::int64 minimumSamples,
                           std::vector<QuietRegion>& result, juce::String& error,
                           LoadProgress progress, bool anyPlaybackRate) {
    error.clear();
    juce::int64 first = 0;
    juce::int64 last = 0;
    if (!clip.audio || clip.audio->numChannels() < 1 || clip.audio->numSamples() < 1 ||
        minimumSamples < 1 || !std::isfinite(clip.audio->sampleRate) ||
        clip.audio->sampleRate <= 0 || !clipSampleRange(clip, first, last)) {
        error = "Select a valid clip and minimum silence duration.";
        return false;
    }

    const auto& audio = *clip.audio;
    const int count = audio.numSamples();
    const int taps = SincResampler::supportTaps(audio.sampleRate, sessionRate);
    const double stride = std::ceil(audio.sampleRate / sessionRate);
    if (taps == 0 || !std::isfinite(clip.offset) || clip.offset < 0 || !std::isfinite(stride) ||
        stride > 1000000) {
        error = "Invalid source geometry.";
        return false;
    }

    // At any output rate the sinc kernel has at most 4096 taps. Add one
    // project-frame stride on either side to cover positions between samples.
    const int margin = anyPlaybackRate ? 2048 + static_cast<int>(stride) + 1 : 0;
    const auto canonical = [](double value) {
        const double frame = value * sessionRate;
        const double nearest = std::round(frame);
        return std::isfinite(frame) && std::abs(frame - nearest) < 1.0e-7 ? nearest / sessionRate
                                                                          : value;
    };
    const double start = canonical(clip.start);
    const bool sameRate = std::abs(audio.sampleRate - sessionRate) < 1.0e-9;

    int left = 0;
    int right = 0;
    int nonzero = 0;
    int nonfinite = 0;
    bool initialized = false;
    const auto adjust = [&](int index, int direction) {
        for (int channel = 0; channel < audio.numChannels(); ++channel) {
            const float value = audio.channelData(channel)[index];
            if (!std::isfinite(value)) {
                nonfinite += direction;
            } else if (std::fpclassify(value) != FP_ZERO) {
                nonzero += direction;
            }
        }
    };

    std::vector<QuietRegion> next;
    juce::int64 run = -1;
    for (juce::int64 frame = first; frame < last; ++frame) {
        if (((frame - first) & 4095) == 0 && progress &&
            !progress(static_cast<double>(frame - first) / static_cast<double>(last - first))) {
            error = "Silence search cancelled.";
            return false;
        }

        double position =
            (clip.offset + static_cast<double>(frame) / sessionRate - start) * audio.sampleRate;
        const double nearest = std::round(position);
        if (std::abs(position - nearest) < 1.0e-7) {
            position = nearest;
        }
        if (!std::isfinite(position) || position < 0 || position >= count) {
            error = "Clip extends beyond its source.";
            return false;
        }

        const int index = static_cast<int>(std::floor(position));
        const bool exact =
            sameRate && std::abs(position - nearest) < 1.0e-8 && nearest >= 0 && nearest < count;
        const int wantedLeft = anyPlaybackRate ? std::max(0, index - margin)
                               : exact         ? index
                                               : std::max(0, index - (taps / 2 - 1));
        const int wantedRight =
            anyPlaybackRate ? static_cast<int>(std::min<juce::int64>(
                                  count, static_cast<juce::int64>(index) + margin + 1))
            : exact         ? index + 1
                            : static_cast<int>(std::min<juce::int64>(
                          count, static_cast<juce::int64>(index) + taps / 2 + 1));

        if (!initialized) {
            left = right = wantedLeft;
            initialized = true;
        }
        while (left > wantedLeft) {
            adjust(--left, 1);
        }
        while (right < wantedRight) {
            adjust(right++, 1);
        }
        while (left < wantedLeft) {
            adjust(left++, -1);
        }
        while (right > wantedRight) {
            adjust(--right, -1);
        }

        if (nonfinite) {
            error = "Source contains a non-finite sample.";
            return false;
        }

        if (nonzero == 0) {
            if (run < 0) {
                run = frame;
            }
        } else if (run >= 0) {
            if (frame - run >= minimumSamples) {
                if (next.size() >= 5000) {
                    error = "More than 5,000 silent regions found; increase the minimum duration.";
                    return false;
                }
                next.push_back({run, frame});
            }
            run = -1;
        }
    }

    if (run >= 0 && last - run >= minimumSamples) {
        if (next.size() >= 5000) {
            error = "More than 5,000 silent regions found; increase the minimum duration.";
            return false;
        }
        next.push_back({run, last});
    }

    if (progress && !progress(1)) {
        error = "Silence search cancelled.";
        return false;
    }

    result = std::move(next);
    return true;
}

bool findQuietRegions(const Session& session, int channels, double thresholdDb,
                      juce::int64 minimumSamples, std::vector<QuietRegion>& result,
                      juce::String& error, LoadProgress progress) {
    error.clear();
    const auto total = projectEndSample(session);
    if (!std::isfinite(thresholdDb) || thresholdDb < -120 || thresholdDb > 0 ||
        minimumSamples < 1 || minimumSamples > 60 * 44100LL ||
        !validateMixRange(session, channels, {0, total}, error)) {
        if (error.isEmpty()) {
            error = "Invalid quiet-gap analysis settings.";
        }
        return false;
    }

    const auto keepGoing = [&](double fraction) {
        if (!progress || progress(fraction)) {
            return true;
        }
        error = "Quiet-gap search cancelled.";
        return false;
    };
    if (!keepGoing(0)) {
        return false;
    }

    const double threshold = std::pow(10.0, thresholdDb / 20.0);
    ResamplingContext context;
    session.prepareResampling(context, sessionRate);
    juce::AudioBuffer<float> buffer(channels, 4096);
    std::vector<QuietRegion> next;
    juce::int64 runStart = -1;

    for (juce::int64 position = 0; position < total; position += 4096) {
        if (!keepGoing(static_cast<double>(position) / static_cast<double>(total))) {
            return false;
        }

        const int count = static_cast<int>(std::min<juce::int64>(4096, total - position));
        buffer.clear();
        session.render(buffer, 0, count, static_cast<double>(position) / sessionRate, sessionRate,
                       channels, &context);

        for (int i = 0; i < count; ++i) {
            bool quiet = true;
            for (int channel = 0; channel < channels; ++channel) {
                const float sample = buffer.getSample(channel, i);
                if (!std::isfinite(sample)) {
                    error = "Mix contains a non-finite sample.";
                    return false;
                }
                if (std::abs(sample) > threshold) {
                    quiet = false;
                }
            }

            const auto frame = position + i;
            if (quiet && runStart < 0) {
                runStart = frame;
            } else if (!quiet && runStart >= 0) {
                if (runStart > 0 && frame - runStart >= minimumSamples) {
                    if (next.size() >= 5000) {
                        error = "More than 5,000 quiet gaps found; increase the minimum duration "
                                "or lower the threshold.";
                        return false;
                    }
                    next.push_back({runStart, frame});
                }
                runStart = -1;
            }
        }
    }

    if (!keepGoing(1)) {
        return false;
    }

    result = std::move(next);
    return true;
}

juce::String quietMarkerName(juce::int64 sample) {
    return "Track at " + formatProjectTime(sample);
}

bool quietMarkerSuggestions(const Session& session, const std::vector<QuietRegion>& regions,
                            juce::StringArray& lines, std::vector<bool>& occupied,
                            juce::String& error) {
    error.clear();
    const auto end = projectEndSample(session);
    if (end <= 0 || regions.size() > 5000) {
        error = "Invalid master or quiet-gap list.";
        return false;
    }

    std::vector<juce::int64> existing;
    for (const auto& marker : session.markers) {
        const double frame = marker.position * sessionRate;
        if (std::isfinite(frame) && frame >= 0 && frame < 9007199254740992.0) {
            existing.push_back(static_cast<juce::int64>(std::llround(frame)));
        }
    }
    std::sort(existing.begin(), existing.end());

    juce::StringArray nextLines;
    std::vector<bool> nextOccupied;
    nextOccupied.reserve(regions.size());
    juce::int64 previousEnd = 0;

    for (const auto& region : regions) {
        if (region.start <= 0 || region.end <= region.start || region.end >= end ||
            region.start < previousEnd) {
            error = "Invalid or overlapping quiet-gap range.";
            return false;
        }
        previousEnd = region.end;
        const auto match = std::lower_bound(existing.begin(), existing.end(), region.start);
        const bool hasMarker = match != existing.end() && *match < region.end;
        nextOccupied.push_back(hasMarker);
        if (!hasMarker) {
            const auto sample = region.start + (region.end - region.start) / 2;
            nextLines.add(juce::String(sample) + " " + quietMarkerName(sample));
        }
    }

    lines = std::move(nextLines);
    occupied = std::move(nextOccupied);
    return true;
}

bool parseQuietMarkerSuggestion(const juce::String& input, juce::int64 masterEnd,
                                juce::int64& sample, juce::String& name, juce::String& error) {
    error.clear();
    const auto line = input.trim();
    const auto separator = line.indexOfAnyOf(" \t");
    if (separator < 1) {
        error = "Enter a sample and a name.";
        return false;
    }

    const auto digits = line.substring(0, separator).toStdString();
    const auto nextName = line.substring(separator).trim();
    juce::int64 nextSample = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), nextSample);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() ||
        nextSample <= 0 || nextSample >= masterEnd || nextName.isEmpty() ||
        nextName.length() > 1024) {
        error = "Use a valid internal sample and a name up to 1,024 characters.";
        return false;
    }

    sample = nextSample;
    name = nextName;
    return true;
}

namespace {

std::set<juce::int64> occupiedMarkerSamples(const Session& session) {
    std::set<juce::int64> occupied;
    for (const auto& marker : session.markers) {
        const double frame = marker.position * sessionRate;
        if (std::isfinite(frame) && frame >= 0 && frame < 9007199254740992.0) {
            occupied.insert(static_cast<juce::int64>(std::llround(frame)));
        }
    }
    return occupied;
}

bool addMarkersAtClipBoundaries(Session& session, const std::vector<ClipReference>& clips,
                                bool ends, juce::String& error) {
    if (!validateClipGroupMove(session, clips, 0, 0, error)) {
        return false;
    }

    const auto masterEnd = projectEndSample(session);
    auto occupied = occupiedMarkerSamples(session);

    struct BoundaryName {
        int track;
        int clip;
        juce::String name;
    };

    std::map<juce::int64, BoundaryName> additions;
    for (const auto& ref : clips) {
        const auto& clip =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        juce::int64 first = 0;
        juce::int64 last = 0;
        if (!clipSampleRange(clip, first, last)) {
            error = "A selected clip contains no project samples.";
            return false;
        }

        const auto sample = ends ? last : first;
        if (sample >= masterEnd) {
            error = ends ? "A selected clip ends at the master end; markers must be inside the "
                           "master."
                         : "A selected clip contains no valid project-sample start.";
            return false;
        }
        if (occupied.count(sample) > 0) {
            error = ends ? "A selected clip end coincides with an existing marker."
                         : "A selected clip start coincides with an existing marker.";
            return false;
        }

        auto name = clipDisplayName(clip);
        if (name.trim().isEmpty()) {
            name = "Untitled";
        }

        auto found = additions.find(sample);
        if (found == additions.end() ||
            std::tie(ref.track, ref.clip) < std::tie(found->second.track, found->second.clip)) {
            additions.insert_or_assign(
                sample, BoundaryName{ref.track, ref.clip, ends ? "After " + name : name});
        }
    }

    for (auto& entry : additions) {
        session.markers.push_back({seconds(entry.first), std::move(entry.second.name)});
    }
    return true;
}

} // namespace

bool addSuggestedMarkers(Session& session, const juce::String& text, juce::String& error) {
    error.clear();
    const auto end = projectEndSample(session);
    if (end <= 0 || text.getNumBytesAsUTF8() > 1024 * 1024) {
        error = "Invalid master or suggestion list is too large.";
        return false;
    }

    auto occupied = occupiedMarkerSamples(session);
    std::vector<Marker> additions;
    juce::StringArray lines;
    lines.addLines(text);

    for (int row = 0; row < lines.size(); ++row) {
        const auto line = lines[row].trim();
        if (line.isEmpty()) {
            continue;
        }

        juce::int64 sample = 0;
        juce::String name;
        juce::String problem;
        if (!parseQuietMarkerSuggestion(line, end, sample, name, problem)) {
            error = "Line " + juce::String(row + 1) + ": " + problem;
            return false;
        }
        if (!occupied.insert(sample).second) {
            error =
                "Line " + juce::String(row + 1) + ": another marker already occupies that sample.";
            return false;
        }
        if (session.markers.size() + additions.size() >= 10000) {
            error = "The master cannot contain more than 10,000 markers.";
            return false;
        }
        additions.push_back({static_cast<double>(sample) / sessionRate, name});
    }

    if (additions.empty()) {
        error = "Enter at least one suggestion to add.";
        return false;
    }

    session.markers.insert(session.markers.end(), additions.begin(), additions.end());
    return true;
}

bool addMarkersAtClipStarts(Session& session, const std::vector<ClipReference>& clips,
                            juce::String& error) {
    return addMarkersAtClipBoundaries(session, clips, false, error);
}

bool addMarkersAtClipEnds(Session& session, const std::vector<ClipReference>& clips,
                          juce::String& error) {
    return addMarkersAtClipBoundaries(session, clips, true, error);
}

bool addMarkersFromClipCues(Session& session, ClipReference ref, juce::String& error) {
    if (!validateClipGroupMove(session, {ref}, 0, 0, error)) {
        return false;
    }

    const auto& clip =
        session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
    const auto& audio = *clip.audio;
    if (audio.sourceCueError.isNotEmpty()) {
        error = audio.sourceCueError;
        return false;
    }
    if (audio.sourceCues.empty()) {
        error = "The imported source contains no WAV cue points.";
        return false;
    }

    int firstSource = 0;
    int lastSource = 0;
    if (!waveformSampleRange(clip, clip.start, clip.start + clip.duration, firstSource,
                             lastSource)) {
        error = "The focused clip contains no source sample positions.";
        return false;
    }

    const auto masterEnd = projectEndSample(session);
    auto occupied = occupiedMarkerSamples(session);
    std::vector<Marker> additions;

    for (const auto& cue : audio.sourceCues) {
        if (cue.sample < 0 || cue.sample >= audio.numSamples()) {
            error = "A captured WAV cue is outside its source.";
            return false;
        }
        if (cue.sample < firstSource || cue.sample >= lastSource) {
            continue;
        }

        const double sourceTime = static_cast<double>(cue.sample) / audio.sampleRate;
        const double frame = (clip.start + sourceTime - clip.offset) * sessionRate;
        if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
            error = "A mapped WAV cue lies outside the precise timeline.";
            return false;
        }

        const auto sample = static_cast<juce::int64>(std::llround(frame));
        if (sample >= masterEnd || !occupied.insert(sample).second) {
            error = "Mapped WAV cues collide or fall outside the master.";
            return false;
        }
        additions.push_back({seconds(sample), cue.name});
    }

    if (additions.empty()) {
        error = "No WAV cues fall inside the focused clip's trim.";
        return false;
    }

    std::sort(additions.begin(), additions.end(),
              [](const auto& a, const auto& b) { return a.position < b.position; });
    session.markers.insert(session.markers.end(), std::make_move_iterator(additions.begin()),
                           std::make_move_iterator(additions.end()));
    return true;
}

MarkerImportReconciliation reconcileImportedMarkers(const std::vector<Marker>& existing,
                                                    std::vector<Marker>& imported) {
    MarkerImportReconciliation result;
    std::map<juce::int64, std::vector<std::size_t>> oldAt;
    std::map<juce::int64, std::vector<std::size_t>> newAt;
    std::map<juce::String, std::size_t> ids;
    std::vector<bool> matched(imported.size(), false);
    std::set<juce::String> retainedIds;

    for (std::size_t i = 0; i < existing.size(); ++i) {
        const auto& marker = existing[i];
        ++ids[marker.id];
        if (markerPositionOnSampleGrid(marker.position)) {
            oldAt[static_cast<juce::int64>(std::llround(marker.position * sessionRate))].push_back(
                i);
        }
    }

    for (std::size_t i = 0; i < imported.size(); ++i) {
        if (markerPositionOnSampleGrid(imported[i].position)) {
            newAt[static_cast<juce::int64>(std::llround(imported[i].position * sessionRate))]
                .push_back(i);
        }
    }

    for (const auto& entry : newAt) {
        const auto found = oldAt.find(entry.first);
        if (entry.second.size() != 1 || found == oldAt.end() || found->second.size() != 1) {
            continue;
        }
        const auto& old = existing[found->second.front()];
        if (old.id.isEmpty() || ids[old.id] != 1) {
            continue;
        }
        auto& next = imported[entry.second.front()];
        next.id = old.id;
        matched[entry.second.front()] = true;
        retainedIds.insert(old.id);
        ++result.retained;
    }

    for (std::size_t i = 0; i < imported.size(); ++i) {
        if (!matched[i]) {
            auto& id = imported[i].id;
            while (id.isEmpty() || !retainedIds.insert(id).second) {
                id = juce::Uuid().toString().removeCharacters("-").toLowerCase();
            }
        }
    }

    return result;
}

std::vector<MarkerInspection> inspectMarkers(const Session& session) {
    std::vector<MarkerInspection> rows;
    rows.reserve(session.markers.size());
    std::map<juce::int64, int> counts;
    const auto end = projectEndSample(session);

    for (int index = 0; index < static_cast<int>(session.markers.size()); ++index) {
        const auto& marker = session.markers[static_cast<size_t>(index)];
        const double frame = marker.position * sessionRate;
        MarkerInspection row;
        row.index = index;
        row.emptyName = marker.name.trim().isEmpty();
        row.validPosition = std::isfinite(frame) && frame >= 0 && frame < 9007199254740992.0;
        if (row.validPosition) {
            row.sample = static_cast<juce::int64>(std::llround(frame));
            row.outside = end <= 0 || row.sample >= end;
            row.offGrid = !markerPositionOnSampleGrid(marker.position);
            ++counts[row.sample];
        }
        rows.push_back(row);
    }

    for (auto& row : rows) {
        if (row.validPosition) {
            row.duplicate = counts[row.sample] > 1;
        }
    }

    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.validPosition != b.validPosition) {
            return a.validPosition;
        }
        if (a.validPosition && a.sample != b.sample) {
            return a.sample < b.sample;
        }
        return a.index < b.index;
    });

    return rows;
}

bool nameMarkersByTrackNumber(Session& session, juce::String& error) {
    error.clear();
    const auto rows = inspectMarkers(session);
    if (rows.empty()) {
        error = "There are no explicit markers to number.";
        return false;
    }

    for (const auto& row : rows) {
        if (!row.validPosition || row.outside || row.duplicate) {
            error = "Fix invalid, duplicate or out-of-range marker positions before numbering.";
            return false;
        }
    }

    int number = rows.front().sample == 0 ? 1 : 2;
    std::vector<std::pair<int, juce::String>> names;
    names.reserve(rows.size());
    bool changed = false;

    for (const auto& row : rows) {
        auto name = "Track " + juce::String(number++);
        if (session.markers[static_cast<size_t>(row.index)].name != name) {
            changed = true;
        }
        names.emplace_back(row.index, std::move(name));
    }

    if (!changed) {
        return false;
    }

    for (const auto& item : names) {
        session.markers[static_cast<size_t>(item.first)].name = item.second;
    }
    return true;
}

bool renameMarkerIndices(Session& session, const std::vector<int>& indices,
                         const std::vector<juce::String>& names, juce::String& error) {
    error.clear();
    if (indices.empty() || indices.size() != names.size()) {
        error = "Provide one name for every selected marker.";
        return false;
    }

    std::set<int> seen;
    bool changed = false;
    for (size_t i = 0; i < indices.size(); ++i) {
        const int index = indices[i];
        if (index < 0 || index >= static_cast<int>(session.markers.size()) ||
            !seen.insert(index).second) {
            error = "Invalid or duplicate marker selection.";
            return false;
        }
        if (names[i].trim().isEmpty() || names[i].containsAnyOf("\r\n")) {
            error = "Each marker needs a nonempty single-line name.";
            return false;
        }
        if (session.markers[static_cast<size_t>(index)].name != names[i]) {
            changed = true;
        }
    }

    if (!changed) {
        error = "Marker names are unchanged.";
        return false;
    }

    for (size_t i = 0; i < indices.size(); ++i) {
        session.markers[static_cast<size_t>(indices[i])].name = names[i];
    }
    return true;
}

bool setMarkerSamples(Session& session, const std::vector<int>& indices,
                      const std::vector<juce::int64>& samples, juce::String& error) {
    error.clear();
    if (indices.empty() || indices.size() != samples.size()) {
        error = "Provide one position for each selected marker.";
        return false;
    }

    const auto end = projectEndSample(session);
    std::set<int> selected;
    std::set<juce::int64> occupied;
    bool changed = false;

    for (size_t i = 0; i < indices.size(); ++i) {
        const int index = indices[i];
        const auto sample = samples[i];
        if (index < 0 || index >= static_cast<int>(session.markers.size()) ||
            !selected.insert(index).second) {
            error = "Invalid or duplicate marker selection.";
            return false;
        }
        if (sample < 0 || sample >= end || !occupied.insert(sample).second) {
            error = "Marker positions must be unique samples within the master.";
            return false;
        }
        if (!std::equal_to<double>{}(session.markers[static_cast<size_t>(index)].position,
                                     seconds(sample))) {
            changed = true;
        }
    }

    for (const auto& row : inspectMarkers(session)) {
        if (selected.count(row.index) != 0) {
            continue;
        }
        if (!row.validPosition || row.outside || row.offGrid) {
            error = "Repair unselected invalid or off-grid markers first.";
            return false;
        }
        if (!occupied.insert(row.sample).second) {
            error = "Marker positions must not collide with other markers.";
            return false;
        }
    }

    if (!changed) {
        error = "Marker positions are unchanged.";
        return false;
    }

    for (size_t i = 0; i < indices.size(); ++i) {
        session.markers[static_cast<size_t>(indices[i])].position = seconds(samples[i]);
    }
    return true;
}

bool shiftMarkers(Session& session, juce::int64 delta, juce::String& error) {
    return shiftMarkersInRange(session, 0, projectEndSample(session), delta, error);
}

bool shiftMarkersInRange(Session& session, juce::int64 first, juce::int64 last, juce::int64 delta,
                         juce::String& error) {
    error.clear();
    if (session.markers.empty()) {
        error = "There are no markers to shift.";
        return false;
    }
    if (delta == 0) {
        error = "Marker positions are unchanged.";
        return false;
    }

    const auto end = projectEndSample(session);
    if (first < 0 || last <= first || last > end) {
        error = "Invalid marker shift range.";
        return false;
    }

    const auto rows = inspectMarkers(session);
    juce::int64 previous = -1;
    bool any = false;
    for (const auto& row : rows) {
        if (!row.validPosition || row.outside || row.duplicate || row.offGrid) {
            error = "Repair invalid, duplicate, or off-grid markers before shifting.";
            return false;
        }
        const bool move = row.sample >= first && row.sample < last;
        if (move && (delta < -row.sample || delta > end - 1 - row.sample)) {
            error = "The shift would move a marker outside the master.";
            return false;
        }
        const auto sample = move ? row.sample + delta : row.sample;
        if (sample <= previous) {
            error = "The shift would collide with or cross another marker.";
            return false;
        }
        previous = sample;
        any |= move;
    }

    if (!any) {
        error = "There are no markers in the selected range.";
        return false;
    }

    for (const auto& row : rows) {
        if (row.sample >= first && row.sample < last) {
            session.markers[static_cast<size_t>(row.index)].position = seconds(row.sample + delta);
        }
    }
    return true;
}

bool shiftMarkerIndices(Session& session, const std::vector<int>& indices, juce::int64 delta,
                        juce::String& error) {
    error.clear();
    if (indices.empty()) {
        error = "Select at least one marker.";
        return false;
    }
    if (delta == 0) {
        error = "Marker positions are unchanged.";
        return false;
    }

    std::vector<bool> selected(session.markers.size(), false);
    for (const auto index : indices) {
        if (index < 0 || static_cast<size_t>(index) >= selected.size() ||
            selected[static_cast<size_t>(index)]) {
            error = "The marker selection is invalid or contains duplicates.";
            return false;
        }
        selected[static_cast<size_t>(index)] = true;
    }

    const auto end = projectEndSample(session);
    const auto rows = inspectMarkers(session);
    juce::int64 previous = -1;
    for (const auto& row : rows) {
        if (!row.validPosition || row.outside || row.duplicate || row.offGrid) {
            error = "Repair invalid, duplicate, or off-grid markers before shifting.";
            return false;
        }
        const bool move = selected[static_cast<size_t>(row.index)];
        if (move && (delta < -row.sample || delta > end - 1 - row.sample)) {
            error = "The shift would move a marker outside the master.";
            return false;
        }
        const auto sample = move ? row.sample + delta : row.sample;
        if (sample <= previous) {
            error = "The shift would collide with or cross another marker.";
            return false;
        }
        previous = sample;
    }

    for (const auto& row : rows) {
        if (selected[static_cast<size_t>(row.index)]) {
            session.markers[static_cast<size_t>(row.index)].position = seconds(row.sample + delta);
        }
    }
    return true;
}

bool snapMarkersToSamples(Session& session, juce::String& error) {
    error.clear();
    const auto rows = inspectMarkers(session);
    if (rows.empty()) {
        error = "There are no markers to repair.";
        return false;
    }

    bool changed = false;
    for (const auto& row : rows) {
        if (!row.validPosition || row.outside || row.duplicate) {
            error = "Repair invalid, duplicate or out-of-range marker positions before snapping.";
            return false;
        }
        changed |= row.offGrid;
    }

    if (!changed) {
        error = "Markers are already aligned to project samples.";
        return false;
    }

    for (const auto& row : rows) {
        if (row.offGrid) {
            session.markers[static_cast<size_t>(row.index)].position = seconds(row.sample);
        }
    }
    return true;
}
Clipboard copyRange(const Session& session, const TimeSelection& selection) {
    Clipboard clipboard;
    if (!validSelection(session, selection)) {
        return clipboard;
    }

    const double start = seconds(selection.start);
    const double end = seconds(selection.end);
    clipboard.lengthSamples = selection.end - selection.start;
    clipboard.tracks.resize(static_cast<size_t>(selection.lastTrack - selection.firstTrack + 1));

    for (int t = selection.firstTrack; t <= selection.lastTrack; ++t) {
        auto& dest = clipboard.tracks[static_cast<size_t>(t - selection.firstTrack)];
        for (const auto& clip : session.tracks[static_cast<size_t>(t)].clips) {
            const double clipEnd = clip.start + clip.duration;
            if (compareTimeToSample(clipEnd, selection.start) <= 0 ||
                compareTimeToSample(clip.start, selection.end) >= 0) {
                continue;
            }

            const double left =
                compareTimeToSample(clip.start, selection.start) < 0 ? start : clip.start;
            const double right = compareTimeToSample(clipEnd, selection.end) > 0 ? end : clipEnd;

            auto piece = clip;
            piece.start = timelineDifference(left, start);
            piece.offset += timelineDifference(left, clip.start);
            piece.duration = timelineDifference(right, left);
            if (piece.duration > 0) {
                dest.push_back(std::move(piece));
            }
        }
    }

    return clipboard;
}

bool copyIndependentTracks(const Session& session, const TimeSelection& selection,
                           const std::vector<int>& tracks, Clipboard& output, juce::String& error) {
    error.clear();
    if (!validSelection(session, selection) || selection.end > maxPreciseSample || tracks.empty()) {
        error = "Select a valid time range and at least one track.";
        return false;
    }

    std::set<int> chosen;
    for (int index : tracks) {
        if (index < 0 || index >= static_cast<int>(session.tracks.size()) ||
            !chosen.insert(index).second) {
            error = "Invalid or duplicate track selection.";
            return false;
        }
    }

    const int first = *chosen.begin();
    const int last = *chosen.rbegin();
    Clipboard copied;
    copied.lengthSamples = selection.end - selection.start;
    copied.tracks.resize(static_cast<size_t>(last - first + 1));

    for (int index : chosen) {
        auto part = copyRange(session, {selection.start, selection.end, index, index});
        if (!part.tracks.empty()) {
            copied.tracks[static_cast<size_t>(index - first)] = std::move(part.tracks[0]);
        }
    }

    output = std::move(copied);
    return true;
}

void finishRangeCut(TimeSelection& selection, int& selectedTrack, int& selectedClip,
                    int& selectedMarker, double& playhead, bool allTracks,
                    int trackCount) noexcept {
    if (!selection.hasRange() || trackCount <= 0 || selection.lastTrack >= trackCount) {
        return;
    }

    if (allTracks) {
        selection.firstTrack = 0;
        selection.lastTrack = trackCount - 1;
    }
    selectedTrack = selection.firstTrack;
    selectedClip = -1;
    selectedMarker = -1;
    playhead = seconds(selection.start);
    selection.end = selection.start;
}

bool preservesMarkerBounds(const Session& before, const Session& after, juce::String& error) {
    const auto oldEnd = projectEndSample(before);
    const auto newEnd = projectEndSample(after);
    if (newEnd >= oldEnd || before.markers.size() != after.markers.size()) {
        return true;
    }

    for (size_t i = 0; i < before.markers.size(); ++i) {
        const auto position = before.markers[i].position;
        const auto frame = position * sessionRate;
        if (std::memcmp(&position, &after.markers[i].position, sizeof(position)) != 0 ||
            !std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
            continue;
        }
        const auto sample = static_cast<juce::int64>(std::llround(frame));
        if (sample < oldEnd && sample >= newEnd) {
            error =
                "This edit would put a track marker beyond the master. Move or delete the marker "
                "first.";
            return false;
        }
    }

    return true;
}

bool preservesOnDiscAudio(const Session& before, const Session& after, juce::String& error) {
    struct LockedRef {
        int trackIndex = 0;
        const Clip* clip = nullptr;
    };
    auto collectLocked = [](const Session& s) {
        std::vector<LockedRef> refs;
        for (int ti = 0; ti < static_cast<int>(s.tracks.size()); ++ti) {
            for (const auto& clip : s.tracks[static_cast<size_t>(ti)].clips) {
                if (clip.lockedOnDisc) {
                    refs.push_back({ti, &clip});
                }
            }
        }
        std::sort(refs.begin(), refs.end(), [](const LockedRef& a, const LockedRef& b) {
            return std::tie(a.trackIndex, a.clip->start, a.clip->onDiscRegionId) <
                   std::tie(b.trackIndex, b.clip->start, b.clip->onDiscRegionId);
        });
        return refs;
    };

    const auto beforeLocked = collectLocked(before);
    const auto afterLocked = collectLocked(after);
    if (beforeLocked.empty() && afterLocked.empty()) {
        return true;
    }

    auto sameVal = [](const auto& a, const auto& b) noexcept {
        return std::memcmp(&a, &b, sizeof(a)) == 0;
    };

    if (beforeLocked.size() != afterLocked.size()) {
        error = "On-disc MiniDisc audio is locked (no re-encoding). Edit tracks, splits, order, "
                "titles, or flags in the MiniDisc workspace instead.";
        return false;
    }
    for (std::size_t i = 0; i < beforeLocked.size(); ++i) {
        const auto& bRef = beforeLocked[i];
        const auto& aRef = afterLocked[i];
        const auto& bClip = *bRef.clip;
        const auto& aClip = *aRef.clip;
        if (bRef.trackIndex != aRef.trackIndex || bClip.onDiscRegionId != aClip.onDiscRegionId ||
            bClip.lockedOnDisc != aClip.lockedOnDisc || !sameVal(bClip.start, aClip.start) ||
            !sameVal(bClip.offset, aClip.offset) || !sameVal(bClip.duration, aClip.duration) ||
            !sameVal(bClip.gain, aClip.gain) || !sameVal(bClip.fadeIn, aClip.fadeIn) ||
            !sameVal(bClip.fadeOut, aClip.fadeOut) ||
            !sameVal(bClip.fadeInStart, aClip.fadeInStart) ||
            !sameVal(bClip.fadeOutEnd, aClip.fadeOutEnd) ||
            bClip.fadeInShape != aClip.fadeInShape || bClip.fadeOutShape != aClip.fadeOutShape) {
            error = "On-disc MiniDisc audio is locked (no re-encoding). Edit tracks, splits, "
                    "order, titles, or flags in the MiniDisc workspace instead.";
            return false;
        }
    }

    for (const auto& track : after.tracks) {
        for (const auto& clip : track.clips) {
            if (clip.lockedOnDisc) {
                continue;
            }
            const double clipStart = clip.start;
            const double clipEnd = clip.start + clip.duration;
            for (const auto& lRef : afterLocked) {
                const double lockedStart = lRef.clip->start;
                const double lockedEnd = lRef.clip->start + lRef.clip->duration;
                if (clipStart < lockedEnd - 1e-9 && clipEnd > lockedStart + 1e-9) {
                    error = "Cannot overlap local audio with locked on-disc MiniDisc audio. Place "
                            "new audio after the on-disc regions.";
                    return false;
                }
            }
        }
    }

    return true;
}

void populateLoadedDiscSession(const MdDisc& loadedDisc, Session& session) {
    session = {};
    session.mdDisc = loadedDisc;

    Track track;
    track.name = "MiniDisc";

    std::vector<std::string> regionLabels(loadedDisc.loadedDisc.regions.size());
    for (const auto& tr : loadedDisc.tracks) {
        for (const auto& part : tr.parts) {
            const int rIdx = findMdOnDiscRegionForPart(loadedDisc.loadedDisc, part);
            if (rIdx >= 0 && regionLabels[static_cast<std::size_t>(rIdx)].empty()) {
                const auto title = tr.displayTitle();
                if (!title.empty()) {
                    regionLabels[static_cast<std::size_t>(rIdx)] = title;
                }
            }
        }
    }

    for (std::size_t i = 0; i < loadedDisc.loadedDisc.regions.size(); ++i) {
        const auto& reg = loadedDisc.loadedDisc.regions[i];
        const std::int64_t frames64 =
            std::max<std::int64_t>(0, reg.timelineEndSample - reg.timelineStartSample);
        if (frames64 <= 0 || frames64 > std::numeric_limits<int>::max()) {
            continue;
        }
        auto audio = std::make_shared<Audio>();
        audio->onDiscPlaceholder = true;
        audio->placeholderChannels = mdChannelsForCodec(reg.codec);
        audio->placeholderFrames = static_cast<int>(frames64);
        audio->sampleRate = sessionRate;

        Clip clip;
        clip.audio = std::move(audio);
        clip.start = static_cast<double>(reg.timelineStartSample) / sessionRate;
        clip.offset = 0.0;
        clip.duration = static_cast<double>(frames64) / sessionRate;
        clip.lockedOnDisc = true;
        clip.onDiscRegionId = reg.id;
        const auto& lbl = regionLabels[i];
        clip.name = lbl.empty() ? ("On-Disc Region " + juce::String(static_cast<int>(i + 1)))
                                : juce::String::fromUTF8(lbl.c_str());
        track.clips.push_back(std::move(clip));
    }

    session.tracks.push_back(std::move(track));
}

namespace {

bool applyRemoveRange(Session& session, const TimeSelection& selection) {
    if (!validSelection(session, selection)) {
        return false;
    }

    const double start = seconds(selection.start);
    const double end = seconds(selection.end);
    bool changed = false;

    for (int t = selection.firstTrack; t <= selection.lastTrack; ++t) {
        auto& clips = session.tracks[static_cast<size_t>(t)].clips;
        std::vector<Clip> kept;
        kept.reserve(clips.size() + 1);

        for (const auto& clip : clips) {
            const double clipEnd = clip.start + clip.duration;
            if (compareTimeToSample(clipEnd, selection.start) <= 0 ||
                compareTimeToSample(clip.start, selection.end) >= 0) {
                kept.push_back(clip);
                continue;
            }
            changed = true;
            if (compareTimeToSample(clip.start, selection.start) < 0) {
                auto left = clip;
                left.duration = timelineDifference(start, clip.start);
                if (left.duration > 0) {
                    kept.push_back(std::move(left));
                }
            }
            if (compareTimeToSample(clipEnd, selection.end) > 0) {
                auto right = clip;
                right.start = end;
                right.offset += timelineDifference(end, clip.start);
                right.duration = timelineDifference(clipEnd, end);
                if (right.duration > 0) {
                    kept.push_back(std::move(right));
                }
            }
        }
        clips = std::move(kept);
    }

    return changed;
}

bool applyTrimTracks(Session& candidate, const TimeSelection& selection) {
    const double start = seconds(selection.start);
    const double end = seconds(selection.end);
    bool changed = false;

    for (int t = selection.firstTrack; t <= selection.lastTrack; ++t) {
        auto& clips = candidate.tracks[static_cast<size_t>(t)].clips;
        std::vector<Clip> kept;
        kept.reserve(clips.size());

        for (const auto& clip : clips) {
            const double clipEnd = clip.start + clip.duration;
            if (compareTimeToSample(clipEnd, selection.start) <= 0 ||
                compareTimeToSample(clip.start, selection.end) >= 0) {
                changed = true;
                continue;
            }

            const bool trimLeft = compareTimeToSample(clip.start, selection.start) < 0;
            const bool trimRight = compareTimeToSample(clipEnd, selection.end) > 0;
            const double left = trimLeft ? start : clip.start;
            const double right = trimRight ? end : clipEnd;
            if (trimLeft || trimRight) {
                auto piece = clip;
                piece.start = left;
                piece.offset += timelineDifference(left, clip.start);
                piece.duration = timelineDifference(right, left);
                if (piece.duration > 0) {
                    kept.push_back(std::move(piece));
                }
                changed = true;
            } else {
                kept.push_back(clip);
            }
        }
        clips = std::move(kept);
    }

    return changed;
}

} // namespace

bool removeRange(Session& session, const TimeSelection& selection, juce::String* error) {
    if (error) {
        error->clear();
    }
    const auto previousEnd = projectEndSample(session);
    if (!applyRemoveRange(session, selection)) {
        return false;
    }

    keepMasterEnd(session, previousEnd);
    return true;
}

bool trimTracksToRange(Session& session, const TimeSelection& selection, juce::String& error) {
    error.clear();
    if (!validSelection(session, selection) || selection.end >= 9007199254740992LL) {
        error = "Select a valid time range.";
        return false;
    }

    auto candidate = session;
    if (!applyTrimTracks(candidate, selection)) {
        error = "The selected tracks have no audio outside the selection.";
        return false;
    }
    if (!preservesMarkerBounds(session, candidate, error)) {
        return false;
    }

    session = std::move(candidate);
    return true;
}

bool splitRange(Session& session, const TimeSelection& selection) {
    if (!validSelection(session, selection)) {
        return false;
    }

    bool changed = false;
    for (int t = selection.firstTrack; t <= selection.lastTrack; ++t) {
        auto& clips = session.tracks[static_cast<size_t>(t)].clips;
        std::vector<Clip> pieces;
        pieces.reserve(clips.size() + 2);

        for (const auto& clip : clips) {
            auto remaining = clip;
            for (const auto boundary : {selection.start, selection.end}) {
                Clip left;
                Clip right;
                if (!splitClipGeometry(remaining, boundary, left, right)) {
                    continue;
                }
                pieces.push_back(std::move(left));
                remaining = std::move(right);
                changed = true;
            }
            pieces.push_back(std::move(remaining));
        }
        clips = std::move(pieces);
    }

    return changed;
}

bool splitTracksAtSample(Session& session, TrackRange tracks, juce::int64 sample) {
    if (tracks.first < 0 || tracks.last < tracks.first ||
        tracks.last >= static_cast<int>(session.tracks.size()) || sample < 0 ||
        sample > maxPreciseSample) {
        return false;
    }

    bool changed = false;
    for (int t = tracks.first; t <= tracks.last; ++t) {
        auto& clips = session.tracks[static_cast<size_t>(t)].clips;
        std::vector<Clip> pieces;
        pieces.reserve(clips.size());

        for (const auto& clip : clips) {
            Clip left;
            Clip right;
            if (!splitClipGeometry(clip, sample, left, right)) {
                pieces.push_back(clip);
                continue;
            }
            pieces.push_back(std::move(left));
            pieces.push_back(std::move(right));
            changed = true;
        }
        clips = std::move(pieces);
    }

    return changed;
}

namespace {

bool splitTrackIndicesToNewTracks(Session& session, const TimeSelection& selection,
                                  const std::vector<int>& tracks, int& firstNewTrack) {
    if (!validSelection(session, selection) || tracks.empty()) {
        return false;
    }

    std::set<int> chosen;
    for (int track : tracks) {
        if (track < 0 || track >= static_cast<int>(session.tracks.size()) ||
            !chosen.insert(track).second) {
            return false;
        }
    }

    const double start = seconds(selection.start);
    const double end = seconds(selection.end);
    std::vector<Track> newTracks;
    newTracks.reserve(chosen.size());

    for (int t : chosen) {
        const auto& source = session.tracks[static_cast<size_t>(t)];
        auto track = source;
        track.clips.clear();

        for (const auto& clip : source.clips) {
            const double clipEnd = clip.start + clip.duration;
            if (compareTimeToSample(clipEnd, selection.start) <= 0 ||
                compareTimeToSample(clip.start, selection.end) >= 0) {
                continue;
            }
            const double left =
                compareTimeToSample(clip.start, selection.start) < 0 ? start : clip.start;
            const double right = compareTimeToSample(clipEnd, selection.end) > 0 ? end : clipEnd;

            auto piece = clip;
            piece.start = left;
            piece.offset += timelineDifference(left, clip.start);
            piece.duration = timelineDifference(right, left);
            if (piece.duration > 0) {
                track.clips.push_back(std::move(piece));
            }
        }

        if (!track.clips.empty()) {
            track.name += " (split)";
            newTracks.push_back(std::move(track));
        }
    }

    if (newTracks.empty()) {
        return false;
    }

    auto candidate = session;
    for (int t : chosen) {
        applyRemoveRange(candidate, {selection.start, selection.end, t, t});
    }

    const int first = static_cast<int>(candidate.tracks.size());
    for (auto& track : newTracks) {
        candidate.tracks.push_back(std::move(track));
    }

    session = std::move(candidate);
    firstNewTrack = first;
    return true;
}

} // namespace

bool splitRangeToNewTracks(Session& session, const TimeSelection& selection, int& firstNewTrack) {
    if (!validSelection(session, selection)) {
        return false;
    }

    std::vector<int> tracks;
    tracks.reserve(static_cast<size_t>(selection.lastTrack - selection.firstTrack + 1));
    for (int t = selection.firstTrack; t <= selection.lastTrack; ++t) {
        tracks.push_back(t);
    }

    return splitTrackIndicesToNewTracks(session, selection, tracks, firstNewTrack);
}

bool splitIndependentRangeToNewTracks(Session& session, const TimeSelection& selection,
                                      const std::vector<int>& tracks, int& firstNewTrack) {
    return splitTrackIndicesToNewTracks(session, selection, tracks, firstNewTrack);
}

namespace {

// A scoped ripple changes every selected clip after the edit point. Reject a
// partially scoped persistent group rather than silently moving its members
// relative to clips on other tracks. A group entirely before the edit is safe.
bool validateScopedRippleGroups(const Session& session, const std::set<int>& tracks,
                                juce::int64 start, juce::String& error) {
    std::set<juce::String> affected;
    for (int track : tracks) {
        for (const auto& clip : session.tracks[static_cast<size_t>(track)].clips) {
            if (clip.groupId.isNotEmpty() &&
                compareTimeToSample(clip.start + clip.duration, start) > 0) {
                affected.insert(clip.groupId);
            }
        }
    }
    if (affected.empty()) {
        return true;
    }

    for (size_t track = 0; track < session.tracks.size(); ++track) {
        if (tracks.count(static_cast<int>(track)) != 0) {
            continue;
        }
        for (const auto& clip : session.tracks[track].clips) {
            if (affected.count(clip.groupId) > 0) {
                error = "This edit would move only part of a clip group. Include its other tracks, "
                        "edit all tracks, or ungroup the clips first.";
                return false;
            }
        }
    }

    return true;
}

std::set<int> scopedTrackSet(const TimeSelection& selection) {
    std::set<int> tracks;
    for (int index = selection.firstTrack; index <= selection.lastTrack; ++index) {
        tracks.insert(index);
    }
    return tracks;
}

bool applyRipple(Session& session, const TimeSelection& selection, bool allTracks) {
    if (!validSelection(session, selection)) {
        return false;
    }

    const double start = seconds(selection.start);
    const double end = seconds(selection.end);
    const auto length = selection.end - selection.start;
    bool changed = false;

    if (allTracks && session.minimumEndSample > selection.start) {
        session.minimumEndSample =
            std::max(selection.start, session.minimumEndSample - (selection.end - selection.start));
        changed = true;
    }

    const int first = allTracks ? 0 : selection.firstTrack;
    const int last = allTracks ? static_cast<int>(session.tracks.size()) - 1 : selection.lastTrack;

    for (int t = first; t <= last; ++t) {
        auto& clips = session.tracks[static_cast<size_t>(t)].clips;
        std::vector<Clip> kept;
        kept.reserve(clips.size() + 1);

        for (const auto& clip : clips) {
            const double clipEnd = clip.start + clip.duration;
            if (compareTimeToSample(clipEnd, selection.start) <= 0) {
                kept.push_back(clip);
                continue;
            }
            if (compareTimeToSample(clip.start, selection.end) >= 0) {
                auto shifted = clip;
                shifted.start = shiftTimelineBySamples(clip.start, -length);
                kept.push_back(std::move(shifted));
                changed = true;
                continue;
            }
            changed = true;
            if (compareTimeToSample(clip.start, selection.start) < 0) {
                auto left = clip;
                left.duration = timelineDifference(start, clip.start);
                if (left.duration > 0) {
                    kept.push_back(std::move(left));
                }
            }
            if (compareTimeToSample(clipEnd, selection.end) > 0) {
                auto right = clip;
                right.start = start;
                right.offset += timelineDifference(end, clip.start);
                right.duration = timelineDifference(clipEnd, end);
                if (right.duration > 0) {
                    kept.push_back(std::move(right));
                }
            }
        }
        clips = std::move(kept);
    }

    if (allTracks) {
        for (auto& marker : session.markers) {
            const auto sample =
                static_cast<juce::int64>(std::llround(marker.position * sessionRate));
            if (sample >= selection.end) {
                marker.position = seconds(sample - (selection.end - selection.start));
                changed = true;
            } else if (sample > selection.start) {
                marker.position = seconds(selection.start);
                changed = true;
            }
        }
    }

    return changed;
}

} // namespace

bool rippleRemoveRange(Session& session, const TimeSelection& selection, bool allTracks,
                       juce::String* error) {
    if (error) {
        error->clear();
    }
    if (!validSelection(session, selection) || selection.end >= 9007199254740992LL) {
        if (error) {
            *error = "Select a valid time range.";
        }
        return false;
    }

    juce::String groupError;
    if (!allTracks && !validateScopedRippleGroups(session, scopedTrackSet(selection),
                                                  selection.start, groupError)) {
        if (error) {
            *error = groupError;
        }
        return false;
    }

    if (session.markers.empty()) {
        return applyRipple(session, selection, allTracks);
    }

    for (const auto& marker : session.markers) {
        const double frame = marker.position * sessionRate;
        if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
            if (error) {
                *error = "Fix invalid marker positions before closing the gap.";
            }
            return false;
        }
        if (allTracks && !markerPositionOnSampleGrid(marker.position)) {
            if (error) {
                *error = "Repair off-grid markers before closing the gap on all tracks.";
            }
            return false;
        }
    }

    auto candidate = session;
    if (!applyRipple(candidate, selection, allTracks)) {
        return false;
    }

    const auto total = fullWavRange(candidate).end;
    std::set<juce::int64> positions;
    for (const auto& marker : candidate.markers) {
        const auto sample = static_cast<juce::int64>(std::llround(marker.position * sessionRate));
        if (!positions.insert(sample).second) {
            if (error) {
                *error =
                    "Closing the gap would place two track markers at the same sample. Move or "
                    "delete a marker first.";
            }
            return false;
        }
        if (sample >= total) {
            if (error) {
                *error =
                    "Closing the gap would put a track marker beyond the master. Move or delete "
                    "it first.";
            }
            return false;
        }
    }

    session = std::move(candidate);
    return true;
}

bool rippleCutRange(Session& session, const TimeSelection& selection, bool allTracks,
                    Clipboard& clipboard, juce::String& error) {
    error.clear();
    if (!validSelection(session, selection)) {
        error = "Select a valid time range first.";
        return false;
    }

    auto range = selection;
    if (allTracks) {
        range.firstTrack = 0;
        range.lastTrack = static_cast<int>(session.tracks.size()) - 1;
    }

    auto copied = copyRange(session, range);
    auto candidate = session;
    if (!rippleRemoveRange(candidate, range, allTracks, &error)) {
        if (error.isEmpty()) {
            error = "No audio follows or intersects the selected range.";
        }
        return false;
    }

    session = std::move(candidate);
    clipboard = std::move(copied);
    return true;
}

namespace {

bool insertGapOnTrack(Track& track, juce::int64 first, juce::int64 last) {
    const auto length = last - first;
    const double start = seconds(first);
    bool changed = false;

    auto& clips = track.clips;
    std::vector<Clip> pieces;
    pieces.reserve(clips.size() + 1);

    for (const auto& clip : clips) {
        const double end = clip.start + clip.duration;
        if (compareTimeToSample(end, first) <= 0) {
            pieces.push_back(clip);
            continue;
        }
        changed = true;
        if (compareTimeToSample(clip.start, first) >= 0) {
            auto right = clip;
            right.start = shiftTimelineBySamples(clip.start, length);
            pieces.push_back(std::move(right));
            continue;
        }

        auto left = clip;
        left.duration = timelineDifference(start, clip.start);
        if (left.duration > 0) {
            pieces.push_back(std::move(left));
        }

        auto right = clip;
        right.start = seconds(last);
        right.offset += timelineDifference(start, clip.start);
        right.duration = timelineDifference(end, start);
        if (right.duration > 0) {
            pieces.push_back(std::move(right));
        }
    }

    clips = std::move(pieces);
    return changed;
}

bool insertGapImpl(Session& session, const TimeSelection& selection, bool allTracks,
                   bool allowEmpty, juce::String& error, bool* changedOut = nullptr) {
    error.clear();
    constexpr juce::int64 limit = 9007199254740992LL;
    if (!validSelection(session, selection) || selection.end >= limit) {
        error = "Select a valid time range.";
        return false;
    }

    const auto length = selection.end - selection.start;
    const auto oldEnd = projectEndSample(session);
    if ((!allowEmpty && oldEnd <= 0) || oldEnd >= limit - length) {
        error = "The inserted gap would exceed the precise timeline range.";
        return false;
    }

    if (!allTracks &&
        !validateScopedRippleGroups(session, scopedTrackSet(selection), selection.start, error)) {
        return false;
    }

    auto candidate = session;
    bool changed = false;
    if (allTracks && selection.start <= oldEnd) {
        candidate.minimumEndSample = oldEnd + length;
        changed = true;
    }

    const int first = allTracks ? 0 : selection.firstTrack;
    const int last =
        allTracks ? static_cast<int>(candidate.tracks.size()) - 1 : selection.lastTrack;
    for (int t = first; t <= last; ++t) {
        changed = insertGapOnTrack(candidate.tracks[static_cast<size_t>(t)], selection.start,
                                   selection.end) ||
                  changed;
    }

    if (!changed && !allowEmpty) {
        error = "No audio intersects or follows the insertion point on these tracks.";
        return false;
    }

    for (int t = first; t <= last; ++t) {
        for (const auto& clip : candidate.tracks[static_cast<size_t>(t)].clips) {
            if (!validTimelineGeometry(clip.start, clip.offset, clip.duration)) {
                error = "Inserted gap would place audio beyond the precise timeline range.";
                return false;
            }
        }
    }

    if (allTracks) {
        std::set<juce::int64> positions;
        for (auto& marker : candidate.markers) {
            const double frame = marker.position * sessionRate;
            if (!std::isfinite(frame) || frame < 0 || frame >= static_cast<double>(limit)) {
                error = "Fix invalid marker positions before inserting a gap on all tracks.";
                return false;
            }
            if (!markerPositionOnSampleGrid(marker.position)) {
                error = "Repair off-grid markers before inserting a gap on all tracks.";
                return false;
            }
            auto sample = static_cast<juce::int64>(std::llround(frame));
            if (sample >= oldEnd || !positions.insert(sample).second) {
                error =
                    "Fix duplicate or out-of-range markers before inserting a gap on all tracks.";
                return false;
            }
            if (sample >= selection.start) {
                marker.position = seconds(sample + length);
            }
        }
    }

    session = std::move(candidate);
    if (changedOut) {
        *changedOut = changed;
    }
    return true;
}

} // namespace

bool insertGap(Session& session, const TimeSelection& selection, bool allTracks,
               juce::String& error) {
    return insertGapImpl(session, selection, allTracks, false, error);
}

bool editIndependentTracks(Session& session, const TimeSelection& selection,
                           const std::vector<int>& tracks, IndependentRangeEdit action,
                           Clipboard& clipboard, juce::String& error) {
    error.clear();
    if (!validSelection(session, selection) || selection.end > maxPreciseSample || tracks.empty()) {
        error = "Select a valid time range and at least one track.";
        return false;
    }

    const bool close =
        action == IndependentRangeEdit::closeGap || action == IndependentRangeEdit::cutClose;
    const bool cut =
        action == IndependentRangeEdit::cutGap || action == IndependentRangeEdit::cutClose;
    const bool insert = action == IndependentRangeEdit::insertGap;
    const bool split = action == IndependentRangeEdit::splitBoundaries;
    const bool trim = action == IndependentRangeEdit::trimOutside;
    const bool duplicate = action == IndependentRangeEdit::duplicate;
    if (!close && !cut && !insert && !split && !trim && !duplicate &&
        action != IndependentRangeEdit::deleteGap) {
        error = "Invalid range action.";
        return false;
    }

    std::set<int> chosen;
    for (int index : tracks) {
        if (index < 0 || index >= static_cast<int>(session.tracks.size()) ||
            !chosen.insert(index).second) {
            error = "Invalid or duplicate track selection.";
            return false;
        }
    }

    if ((close || insert) && !validateScopedRippleGroups(session, chosen, selection.start, error)) {
        return false;
    }

    const auto length = selection.end - selection.start;
    const auto oldEnd = projectEndSample(session);
    if (insert && (oldEnd <= 0 || oldEnd >= 9007199254740992LL - length)) {
        error = "The inserted gap would exceed the precise timeline range.";
        return false;
    }

    if (close) {
        for (const auto& marker : session.markers) {
            const double frame = marker.position * sessionRate;
            if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
                error = "Fix invalid marker positions before closing the gap.";
                return false;
            }
        }
    }

    Clipboard copied;
    if (cut || duplicate) {
        if (!copyIndependentTracks(session, selection, tracks, copied, error)) {
            return false;
        }
    }

    auto candidate = session;
    bool changed = false;
    if (duplicate) {
        if (length > maxPreciseSample - selection.end) {
            error = "Duplicated selection would exceed the precise timeline range.";
            return false;
        }
        if (!pasteRange(candidate, copied, *chosen.begin(), selection.end, &error)) {
            return false;
        }
        session = std::move(candidate);
        return true;
    }

    for (int index : chosen) {
        const TimeSelection one{selection.start, selection.end, index, index};
        if (insert) {
            auto& track = candidate.tracks[static_cast<size_t>(index)];
            changed = insertGapOnTrack(track, selection.start, selection.end) || changed;
            for (const auto& clip : track.clips) {
                if (!validTimelineGeometry(clip.start, clip.offset, clip.duration)) {
                    error = "Inserted audio would exceed the precise timeline range.";
                    return false;
                }
            }
        } else if (split) {
            changed = splitRange(candidate, one) || changed;
        } else if (trim) {
            changed = applyTrimTracks(candidate, one) || changed;
        } else if (close) {
            changed = applyRipple(candidate, one, false) || changed;
        } else {
            changed = applyRemoveRange(candidate, one) || changed;
        }
    }

    if (!changed) {
        error = split  ? "No selected clips cross a selection boundary."
                : trim ? "The selected tracks have no audio outside the selection."
                : insert || close
                    ? "No audio intersects or follows the selected time on those tracks."
                    : "No audio intersects the selected time on those tracks.";
        return false;
    }

    if (close && !candidate.markers.empty()) {
        const auto end = projectEndSample(candidate);
        std::set<juce::int64> positions;
        for (const auto& marker : candidate.markers) {
            const auto sample =
                static_cast<juce::int64>(std::llround(marker.position * sessionRate));
            if (!positions.insert(sample).second) {
                error = "Closing the gap would leave duplicate track markers; repair them first.";
                return false;
            }
            if (sample >= end) {
                error =
                    "Closing the gap would put a track marker beyond the master. Move or delete "
                    "it first.";
                return false;
            }
        }
    }

    if (!close && !insert && !split && !trim) {
        keepMasterEnd(candidate, oldEnd);
    }
    if (!preservesMarkerBounds(session, candidate, error)) {
        return false;
    }

    session = std::move(candidate);
    if (cut) {
        clipboard = std::move(copied);
    }
    return true;
}

namespace {

bool validClipboardDestination(const Session& session, const Clipboard& clipboard, int targetTrack,
                               juce::int64 atSample) {
    return !clipboard.empty() && targetTrack >= 0 && atSample >= 0 &&
           atSample <= maxPreciseSample && clipboard.lengthSamples <= maxPreciseSample - atSample &&
           static_cast<size_t>(targetTrack) <= session.tracks.size() &&
           clipboard.tracks.size() <= session.tracks.size() - static_cast<size_t>(targetTrack);
}

bool pasteRangeImpl(Session& session, const Clipboard& clipboard, int targetTrack,
                    juce::int64 atSample, juce::String* error,
                    std::vector<ClipReference>* pastedRefs, bool forkGroups) {
    if (error) {
        error->clear();
    }
    if (!validClipboardDestination(session, clipboard, targetTrack, atSample)) {
        if (error) {
            *error = "Select a destination track with enough tracks for the copied audio.";
        }
        return false;
    }

    auto copies = clipboard.tracks;
    bool hasAudio = false;
    if (forkGroups) {
        std::vector<Clip*> copiedClips;
        for (auto& track : copies) {
            for (auto& clip : track) {
                copiedClips.push_back(&clip);
            }
        }
        juce::String groupError;
        if (!forkCopiedClipGroups(session, copiedClips, groupError)) {
            if (error) {
                *error = groupError;
            }
            return false;
        }
    }

    for (auto& track : copies) {
        for (auto& clip : track) {
            hasAudio = true;
            clip.start = shiftTimelineBySamples(clip.start, atSample);
            if (!validClipForSampleEdit(clip) ||
                !validTimelineGeometry(clip.start, clip.offset, clip.duration)) {
                if (error) {
                    *error = "Pasted audio would extend beyond the precise timeline range.";
                }
                return false;
            }
        }
    }

    const auto pasteEnd = atSample + clipboard.lengthSamples;
    const bool extends = pasteEnd > projectEndSample(session);
    if (!hasAudio && !extends) {
        if (error) {
            *error = "The copied range is silent and the destination is already within the master.";
        }
        return false;
    }

    std::vector<ClipReference> result;
    for (size_t t = 0; t < copies.size(); ++t) {
        auto& target = session.tracks[static_cast<size_t>(targetTrack) + t].clips;
        for (auto& clip : copies[t]) {
            if (pastedRefs) {
                result.push_back(
                    {targetTrack + static_cast<int>(t), static_cast<int>(target.size())});
            }
            target.push_back(std::move(clip));
        }
    }

    if (extends && pasteEnd > projectEndSample(session)) {
        session.minimumEndSample = pasteEnd;
    }
    if (pastedRefs) {
        *pastedRefs = std::move(result);
    }
    return true;
}

} // namespace

bool pasteRange(Session& session, const Clipboard& clipboard, int targetTrack, juce::int64 atSample,
                juce::String* error, std::vector<ClipReference>* pastedRefs) {
    return pasteRangeImpl(session, clipboard, targetTrack, atSample, error, pastedRefs, true);
}

bool pasteRangeReplacing(Session& session, const Clipboard& clipboard, int targetTrack,
                         juce::int64 atSample, juce::String& error,
                         std::vector<ClipReference>* pastedRefs) {
    error.clear();
    if (!validClipboardDestination(session, clipboard, targetTrack, atSample)) {
        error = "Select a destination with enough tracks and a valid timeline range.";
        return false;
    }

    auto candidate = session;
    const TimeSelection destination{atSample, atSample + clipboard.lengthSamples, targetTrack,
                                    targetTrack + static_cast<int>(clipboard.tracks.size()) - 1};
    const bool removed = applyRemoveRange(candidate, destination);
    const auto previousEnd = projectEndSample(session);
    std::vector<ClipReference> result;

    if (clipboard.hasAudio()) {
        if (!pasteRange(candidate, clipboard, targetTrack, atSample, &error,
                        pastedRefs ? &result : nullptr)) {
            return false;
        }
    } else if (!removed && atSample + clipboard.lengthSamples <= previousEnd) {
        error = "The destination is already silent in this range.";
        return false;
    }

    const auto retainedEnd = std::max(previousEnd, atSample + clipboard.lengthSamples);
    if (projectEndSample(candidate) < retainedEnd) {
        candidate.minimumEndSample = retainedEnd;
    }
    if (!preservesMarkerBounds(session, candidate, error)) {
        return false;
    }

    session = std::move(candidate);
    if (pastedRefs) {
        *pastedRefs = std::move(result);
    }
    return true;
}

bool pasteRangeInserting(Session& session, const Clipboard& clipboard, int targetTrack,
                         juce::int64 atSample, bool allTracks, juce::String& error,
                         std::vector<ClipReference>* pastedRefs) {
    error.clear();
    if (!validClipboardDestination(session, clipboard, targetTrack, atSample)) {
        error = "Select a destination with enough tracks and a valid timeline range.";
        return false;
    }

    auto candidate = session;
    const TimeSelection destination{atSample, atSample + clipboard.lengthSamples, targetTrack,
                                    targetTrack + static_cast<int>(clipboard.tracks.size()) - 1};
    bool shifted = false;
    if (!insertGapImpl(candidate, destination, allTracks, true, error, &shifted)) {
        return false;
    }

    std::vector<ClipReference> result;
    if (clipboard.hasAudio() || !shifted) {
        if (!pasteRange(candidate, clipboard, targetTrack, atSample, &error,
                        pastedRefs ? &result : nullptr)) {
            return false;
        }
    }

    session = std::move(candidate);
    if (pastedRefs) {
        *pastedRefs = std::move(result);
    }
    return true;
}

bool duplicateRange(Session& session, const TimeSelection& selection, juce::String& error) {
    error.clear();
    if (!validSelection(session, selection)) {
        error = "Select a valid time range first.";
        return false;
    }
    if (selection.end > maxPreciseSample ||
        selection.end - selection.start > maxPreciseSample - selection.end) {
        error = "Duplicated selection would exceed the precise timeline range.";
        return false;
    }

    const auto copied = copyRange(session, selection);
    auto candidate = session;
    if (!pasteRange(candidate, copied, selection.firstTrack, selection.end, &error)) {
        return false;
    }

    session = std::move(candidate);
    return true;
}

int rangeMoveLinkedGroups(const Session& session, const TimeSelection& selection,
                          const std::vector<int>& independentTracks) {
    if (!validSelection(session, selection)) {
        return 0;
    }

    std::set<int> chosen;
    if (independentTracks.empty()) {
        for (int track = selection.firstTrack; track <= selection.lastTrack; ++track) {
            chosen.insert(track);
        }
    } else {
        for (int track : independentTracks) {
            if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
                return 0;
            }
            chosen.insert(track);
        }
    }

    std::map<juce::String, std::pair<bool, bool>> groups;
    for (size_t track = 0; track < session.tracks.size(); ++track) {
        for (const auto& clip : session.tracks[track].clips) {
            if (clip.groupId.isEmpty() || !std::isfinite(clip.start) ||
                !std::isfinite(clip.duration) || clip.duration <= 0 ||
                !std::isfinite(clip.start + clip.duration)) {
                continue;
            }

            const bool selected = chosen.count(static_cast<int>(track)) > 0;
            const double clipEnd = clip.start + clip.duration;
            auto& group = groups[clip.groupId];
            if (selected && compareTimeToSample(clip.start, selection.end) < 0 &&
                compareTimeToSample(clipEnd, selection.start) > 0) {
                group.first = true;
            }
            if (!selected || compareTimeToSample(clip.start, selection.start) < 0 ||
                compareTimeToSample(clipEnd, selection.end) > 0) {
                group.second = true;
            }
        }
    }

    int count = 0;
    for (const auto& group : groups) {
        if (group.second.first && group.second.second) {
            ++count;
        }
    }
    return count;
}

bool moveRange(Session& session, const TimeSelection& selection, int targetTrack,
               juce::int64 atSample, juce::String& error) {
    error.clear();
    if (!validSelection(session, selection) || atSample < 0 || atSample > maxPreciseSample ||
        selection.end - selection.start > maxPreciseSample - atSample || targetTrack < 0 ||
        static_cast<size_t>(targetTrack) > session.tracks.size() ||
        static_cast<size_t>(selection.lastTrack - selection.firstTrack + 1) >
            session.tracks.size() - static_cast<size_t>(targetTrack)) {
        error = "Choose a valid destination time and enough existing tracks.";
        return false;
    }

    if (atSample == selection.start && targetTrack == selection.firstTrack) {
        error = "The selection is already at that destination.";
        return false;
    }

    const auto copied = copyRange(session, selection);
    if (!copied.hasAudio()) {
        error = "The selected range contains no audio to move.";
        return false;
    }

    auto candidate = session;
    if (!applyRemoveRange(candidate, selection) ||
        !pasteRangeImpl(candidate, copied, targetTrack, atSample, &error, nullptr, false)) {
        return false;
    }
    if (!preservesMarkerBounds(session, candidate, error)) {
        return false;
    }

    session = std::move(candidate);
    return true;
}

bool moveIndependentRange(Session& session, const TimeSelection& selection,
                          const std::vector<int>& tracks, int targetTrack, juce::int64 atSample,
                          juce::String& error) {
    Clipboard copied;
    if (!copyIndependentTracks(session, selection, tracks, copied, error)) {
        return false;
    }

    if (targetTrack < 0 || atSample < 0 || atSample > maxPreciseSample ||
        copied.lengthSamples > maxPreciseSample - atSample ||
        static_cast<size_t>(targetTrack) > session.tracks.size() ||
        copied.tracks.size() > session.tracks.size() - static_cast<size_t>(targetTrack)) {
        error = "Choose a valid destination time and enough existing tracks.";
        return false;
    }

    const int first = *std::min_element(tracks.begin(), tracks.end());
    if (atSample == selection.start && targetTrack == first) {
        error = "The selection is already at that destination.";
        return false;
    }
    if (!copied.hasAudio()) {
        error = "The selected range contains no audio to move.";
        return false;
    }

    auto candidate = session;
    bool removed = false;
    for (int track : tracks) {
        removed =
            applyRemoveRange(candidate, {selection.start, selection.end, track, track}) || removed;
    }
    if (!removed ||
        !pasteRangeImpl(candidate, copied, targetTrack, atSample, &error, nullptr, false)) {
        return false;
    }
    if (!preservesMarkerBounds(session, candidate, error)) {
        return false;
    }

    session = std::move(candidate);
    return true;
}

bool measureClipRawPeak(const Clip& clip, float& peakOut, juce::String& error) {
    error.clear();
    peakOut = 0.0f;
    if (!clip.audio || clip.audio->samples.getNumChannels() <= 0 ||
        clip.audio->samples.getNumSamples() <= 0 || !(clip.audio->sampleRate > 0) ||
        !std::isfinite(clip.offset) || !std::isfinite(clip.duration) || clip.duration <= 0) {
        error = "Clip has no valid audio.";
        return false;
    }

    const double rate = clip.audio->sampleRate;
    const int channels = clip.audio->samples.getNumChannels();
    const int frames = clip.audio->samples.getNumSamples();
    const juce::int64 first = std::clamp<juce::int64>(
        static_cast<juce::int64>(std::floor(clip.offset * rate)), 0, frames);
    const juce::int64 last = std::clamp<juce::int64>(
        static_cast<juce::int64>(std::ceil((clip.offset + clip.duration) * rate)), 0, frames);

    float peak = 0.0f;
    bool any = false;
    for (juce::int64 i = first; i < last; ++i) {
        const double t = static_cast<double>(i) / rate;
        if (t < clip.offset - 1e-12 || t >= clip.offset + clip.duration - 1e-12) {
            continue;
        }
        any = true;
        double fade = 1.0;
        if (clip.fadeIn > 0.0) {
            fade *= static_cast<double>(
                fadeCurveGain((t - clip.fadeInStart) / clip.fadeIn, clip.fadeInShape));
        }
        if (clip.fadeOut > 0.0) {
            fade *= static_cast<double>(
                fadeCurveGain((clip.fadeOutEnd - t) / clip.fadeOut, clip.fadeOutShape));
        }
        for (int ch = 0; ch < channels; ++ch) {
            const float sample = std::abs(clip.audio->samples.getSample(ch, static_cast<int>(i)));
            if (std::isfinite(sample)) {
                peak = std::max(peak, static_cast<float>(static_cast<double>(sample) * fade));
            }
        }
    }

    if (!any && first < frames) {
        const int idx = static_cast<int>(std::clamp<juce::int64>(first, 0, frames - 1));
        for (int ch = 0; ch < channels; ++ch) {
            const float sample = std::abs(clip.audio->samples.getSample(ch, idx));
            if (std::isfinite(sample)) {
                peak = std::max(peak, sample);
            }
        }
    }

    peakOut = peak;
    return true;
}

namespace {

bool uniqueClipRefs(const Session& session, const std::vector<ClipReference>& clips,
                    std::vector<ClipReference>& unique, juce::String& error) {
    unique.clear();
    std::set<std::pair<int, int>> seen;
    for (const auto& ref : clips) {
        if (ref.track < 0 || ref.track >= static_cast<int>(session.tracks.size()) || ref.clip < 0 ||
            ref.clip >=
                static_cast<int>(session.tracks[static_cast<size_t>(ref.track)].clips.size())) {
            error = "Invalid clip selection.";
            return false;
        }
        if (seen.emplace(ref.track, ref.clip).second) {
            unique.push_back(ref);
        }
    }

    if (unique.empty()) {
        error = "Select at least one clip.";
        return false;
    }
    return true;
}

} // namespace

bool amplifyClips(Session& session, const std::vector<ClipReference>& clips, double deltaDb,
                  juce::String& error) {
    error.clear();
    if (!std::isfinite(deltaDb) || deltaDb < -240.0 || deltaDb > 240.0) {
        error = "Amplify amount must be finite.";
        return false;
    }

    std::vector<ClipReference> unique;
    if (!uniqueClipRefs(session, clips, unique, error)) {
        return false;
    }

    const double factor = std::pow(10.0, deltaDb / 20.0);
    if (!std::isfinite(factor) || factor < 0.0) {
        error = "Invalid amplify factor.";
        return false;
    }

    std::vector<float> nextGains;
    nextGains.reserve(unique.size());
    for (const auto& ref : unique) {
        const double next = static_cast<double>(session.tracks[static_cast<size_t>(ref.track)]
                                                    .clips[static_cast<size_t>(ref.clip)]
                                                    .gain) *
                            factor;
        if (!std::isfinite(next) || next < 0.0) {
            error = "Invalid resulting clip gain.";
            return false;
        }
        if (next > static_cast<double>(maxLinearGain)) {
            error = "Resulting clip gain exceeds 16x (+24.08 dB) limit.";
            return false;
        }
        nextGains.push_back(static_cast<float>(next));
    }

    for (size_t i = 0; i < unique.size(); ++i) {
        session.tracks[static_cast<size_t>(unique[i].track)]
            .clips[static_cast<size_t>(unique[i].clip)]
            .gain = nextGains[i];
    }
    return true;
}

bool normalizeClips(Session& session, const std::vector<ClipReference>& clips, double targetPeakDb,
                    bool independent, juce::String& error) {
    error.clear();
    if (!std::isfinite(targetPeakDb) || targetPeakDb < -120.0 || targetPeakDb > 0.0) {
        error = "Target peak must be between -120 and 0 dBFS.";
        return false;
    }

    std::vector<ClipReference> unique;
    if (!uniqueClipRefs(session, clips, unique, error)) {
        return false;
    }

    const double targetLinear = std::pow(10.0, targetPeakDb / 20.0);
    std::vector<float> rawPeaks(unique.size(), 0.0f);
    for (size_t i = 0; i < unique.size(); ++i) {
        if (!measureClipRawPeak(session.tracks[static_cast<size_t>(unique[i].track)]
                                    .clips[static_cast<size_t>(unique[i].clip)],
                                rawPeaks[i], error)) {
            return false;
        }
    }

    std::vector<float> nextGains(unique.size(), 1.0f);
    if (!independent) {
        double maxCurrent = 0.0;
        for (size_t i = 0; i < unique.size(); ++i) {
            const auto& clip = session.tracks[static_cast<size_t>(unique[i].track)]
                                   .clips[static_cast<size_t>(unique[i].clip)];
            maxCurrent = std::max(maxCurrent, static_cast<double>(rawPeaks[i]) *
                                                  static_cast<double>(clip.gain));
        }
        if (!(maxCurrent > 0.0)) {
            error = "Selected audio is silent.";
            return false;
        }

        const double scale = targetLinear / maxCurrent;
        for (size_t i = 0; i < unique.size(); ++i) {
            const double next =
                static_cast<double>(session.tracks[static_cast<size_t>(unique[i].track)]
                                        .clips[static_cast<size_t>(unique[i].clip)]
                                        .gain) *
                scale;
            if (!std::isfinite(next) || next > static_cast<double>(maxLinearGain)) {
                error = "Normalizing requires clip gain above 16x (+24.08 dB).";
                return false;
            }
            nextGains[i] = static_cast<float>(next);
        }
    } else {
        bool anySound = false;
        for (size_t i = 0; i < unique.size(); ++i) {
            const auto& clip = session.tracks[static_cast<size_t>(unique[i].track)]
                                   .clips[static_cast<size_t>(unique[i].clip)];
            if (rawPeaks[i] > 0.0f) {
                anySound = true;
                const double next = targetLinear / static_cast<double>(rawPeaks[i]);
                if (!std::isfinite(next) || next > static_cast<double>(maxLinearGain)) {
                    error = "Normalizing requires clip gain above 16x (+24.08 dB).";
                    return false;
                }
                nextGains[i] = static_cast<float>(next);
            } else {
                nextGains[i] = clip.gain;
            }
        }
        if (!anySound) {
            error = "Selected audio is silent.";
            return false;
        }
    }

    for (size_t i = 0; i < unique.size(); ++i) {
        session.tracks[static_cast<size_t>(unique[i].track)]
            .clips[static_cast<size_t>(unique[i].clip)]
            .gain = nextGains[i];
    }
    return true;
}

} // namespace carta
