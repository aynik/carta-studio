/**
 * @file StudioEditingActions.cpp
 * @brief Timeline range/clipboard edits, marker manipulation, track/clip mix
 *        and geometry dialogs, undo/redo history traversal, background mix/gap/
 *        silence analyses, list/navigator dialogs, and audition previews for `Studio`.
 *
 * Coordinate spaces and units:
 * - Project timeline positions convert between seconds and 44.1 kHz half-open
 *   `[start, end)` project samples (`carta::sessionRate`), validated against
 *   `2^53` (`9007199254740992.0`) for exact IEEE-754 double sample precision.
 * - Linear gains span `[0, carta::maxLinearGain]` (max +24.08 dB), UI gain
 *   percentages span `[0, 1600]`, track heights clamp to `[72, 800]` px, and
 *   channel height weights use integers in `[1, 1000]`.
 *
 * Threading and locking invariants:
 * - Actions execute on the JUCE message thread and acquire `Studio::lock` when
 *   reading or mutating shared transport/playback state (`session`, `position`,
 *   `masterChannels`, `auditionRange`, `playing`, `auditionActive`,
 *   `cutPreviewSession`).
 * - Candidate session construction, marker bounds checks, snap enumeration, and
 *   `editHistory.enforceRetainedMediaBudget()` release `lock` via
 *   `juce::ScopedUnlock`, then verify `editGeneration` and selection state
 *   before swapping committed state under `lock`.
 * - `exportPool` and `levelPool` workers operate on immutable `carta::Session`
 *   snapshots copied under `lock` and report back via `MessageManager::callAsync`
 *   with `SafePointer<Studio>` and `editGeneration` staleness checks.
 */

#include "Studio/Studio.h"

namespace {

namespace IndependentRangeAction {
constexpr int cutGap = 4;
constexpr int cutClose = 5;
constexpr int deleteGap = 6;
constexpr int closeGap = 7;
constexpr int insertGap = 8;
constexpr int copy = 9;
constexpr int previewClose = 10;
constexpr int splitBoundaries = 11;
constexpr int trimOutside = 12;
constexpr int duplicate = 13;
} // namespace IndependentRangeAction

namespace TrackMixOp {
constexpr int mute = 0;
constexpr int unmute = 1;
constexpr int solo = 2;
constexpr int clearSolo = 3;
} // namespace TrackMixOp

namespace TrackFlagCommand {
constexpr int mute = 150;
constexpr int unmute = 151;
constexpr int solo = 152;
[[maybe_unused]] constexpr int clearSolo = 153;
} // namespace TrackFlagCommand

namespace IndependentTrackMenuId {
constexpr int mute = 273;
[[maybe_unused]] constexpr int unmute = 274;
[[maybe_unused]] constexpr int solo = 275;
constexpr int clearSolo = 276;
constexpr int copyTime = 277;
constexpr int cutGap = 278;
[[maybe_unused]] constexpr int cutClose = 279;
[[maybe_unused]] constexpr int deleteGap = 280;
[[maybe_unused]] constexpr int deleteClose = 281;
constexpr int insertGap = 282;
constexpr int previewClose = 283;
} // namespace IndependentTrackMenuId

namespace AlignClipCommand {
[[maybe_unused]] constexpr int startToPlayhead = 141;
constexpr int endToPlayhead = 142;
constexpr int startToSelStart = 143;
[[maybe_unused]] constexpr int startToSelEnd = 144;
constexpr int endToSelStart = 145;
[[maybe_unused]] constexpr int endToSelEnd = 146;
constexpr int startToMarker = 199;
constexpr int endToMarker = 200;
[[maybe_unused]] constexpr int startToPrevEdge = 201;
constexpr int startToNextEdge = 202;
constexpr int endToPrevEdge = 203;
constexpr int endToNextEdge = 204;
} // namespace AlignClipCommand

namespace ClipEdgeNudgeCommand {
constexpr int endEarlier = 156;
} // namespace ClipEdgeNudgeCommand

namespace TimelineClipAction {
constexpr int copy = 188;
constexpr int cut = 189;
constexpr int remove = 190;
constexpr int duplicate = 191;
constexpr int split = 192;
constexpr int move = 193;
constexpr int clear = 194;
constexpr int join = 291;
} // namespace TimelineClipAction

namespace ClipScopeCommand {
constexpr int allLayers = 206;
constexpr int inTimeSelection = 208;
} // namespace ClipScopeCommand

namespace AuditionBoundaryCommand {
constexpr int playhead = 76;
constexpr int selectionStart = 77;
constexpr int selectionEnd = 78;
} // namespace AuditionBoundaryCommand

namespace ShiftMarkersCommand {
constexpr int all = 132;
constexpr int fromPlayhead = 209;
constexpr int inSelection = 210;
} // namespace ShiftMarkersCommand

namespace SeekCommand {
constexpr int base = 161;
} // namespace SeekCommand

namespace ClipPortionCommand {
constexpr int full = 0;
constexpr int visible = 249;
constexpr int beforePlayhead = 250;
} // namespace ClipPortionCommand

namespace SelectBoundaryCommand {
constexpr int allBase = 116;
constexpr int scopedBase = 135;
constexpr int scopedEnd = 140;
} // namespace SelectBoundaryCommand

namespace TrackOperation {
constexpr int add = 0;
constexpr int duplicate = 1;
constexpr int remove = 2;
} // namespace TrackOperation

namespace FocusedClipAction {
constexpr int split = 0;
constexpr int remove = 1;
constexpr int duplicate = 2;
} // namespace FocusedClipAction

namespace PastePolicy {
constexpr int overlap = 0;
constexpr int replace = 1;
constexpr int insertDestination = 2;
constexpr int insertAll = 3;
} // namespace PastePolicy

namespace LevelScope {
[[maybe_unused]] constexpr int master = 0;
constexpr int selectionFull = 1;
constexpr int selectionTracks = 2;
constexpr int trackAtPlayhead = 3;
} // namespace LevelScope

namespace RippleScope {
constexpr int focusedTrack = 0;
constexpr int selectedTracks = 1;
constexpr int allTracks = 2;
} // namespace RippleScope

namespace CrossfadeScope {
constexpr int sameTrack = 0;
constexpr int acrossTracks = 1;
constexpr int selectedPair = 2;
} // namespace CrossfadeScope

} // namespace

// ============================================================================
// Range, Clipboard, Ripple & Edit Point Operations
// ============================================================================

bool Studio::duplicateTimeSelection() {
    if (timeSelection.hasRange() && !selectedTimelineTracks().empty()) {
        return applyIndependentTrackRange(editGeneration, timeSelection, selectedTimelineTracks(),
                                          IndependentRangeAction::duplicate);
    }

    const juce::ScopedLock guard(lock);
    if (dragMode != kDragNone) {
        status.setText("Finish or cancel the current drag first.", juce::dontSendNotification);
        return false;
    }

    juce::String error;
    const auto scope = timeSelection;
    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            return carta::duplicateRange(candidate, scope, error) &&
                   carta::preservesMarkerBounds(session, candidate, error);
        },
        [] {}, "Duplicate selection", error);
    status.setText(error.isNotEmpty() ? error
                   : changed ? "Duplicated selected audio after the selection; overlaps are summed."
                             : "No audio was duplicated.",
                   juce::dontSendNotification);
    return changed;
}

bool Studio::copySelection(bool cut) {
    if (timeSelection.hasRange() && !selectedTimelineTracks().empty()) {
        const auto tracks = selectedTimelineTracks();
        const auto revision = editGeneration;
        const auto selection = timeSelection;
        if (!cut) {
            return copyIndependentTrackRange(revision, selection, tracks);
        }
        carta::Clipboard probe;
        juce::String error;
        if (!carta::copyIndependentTracks(session, selection, tracks, probe, error)) {
            status.setText(error, juce::dontSendNotification);
            return false;
        }
        if (!probe.hasAudio()) {
            return copyIndependentTrackRange(revision, selection, tracks, &probe);
        }
        return applyIndependentTrackRange(revision, selection, tracks,
                                          IndependentRangeAction::cutGap);
    }

    const juce::ScopedLock guard(lock);
    const bool keepPlayback = selectionPlaybackContinues();
    const auto scope = timeSelection;
    carta::Clipboard copied;
    {
        const juce::ScopedUnlock unlock(lock);
        copied = carta::copyRange(session, scope);
    }
    if (copied.empty()) {
        status.setText("Select a time range to copy.", juce::dontSendNotification);
        return false;
    }

    const bool hasAudio = copied.hasAudio();
    bool pruned = false;
    if (cut && hasAudio) {
        juce::String error;
        if (!prepareRangeSessionEdit(
                [&](carta::Session& candidate) {
                    return carta::removeRange(candidate, scope, &error);
                },
                [&] {
                    clipboard = std::move(copied);
                    auto cutPosition = position;
                    carta::finishRangeCut(timeSelection, selectedTrack, selectedClip,
                                          selectedMarker, cutPosition, false,
                                          static_cast<int>(session.tracks.size()));
                    if (!keepPlayback) {
                        position = cutPosition;
                    }
                },
                "Cut selection", error)) {
            status.setText(error.isNotEmpty() ? error : "Cut had no effect.",
                           juce::dontSendNotification);
            return false;
        }
    } else {
        clipboard = std::move(copied);
        const juce::ScopedUnlock unlock(lock);
        editHistory.enforceRetainedMediaBudget(session, clipboard);
        pruned = editHistory.takeBudgetPruned();
    }

    status.setText(
        juce::String(!hasAudio ? "Copied a silent time range; pasting can extend the master or "
                                 "replace audio."
                     : cut     ? "Cut selection; its time remains as a gap."
                               : "Copied selection.") +
            (pruned ? " Older undo history was discarded to limit retained media." : ""),
        juce::dontSendNotification);
    repaint();
    return true;
}

bool Studio::trimSelectionTracks() {
    if (timeSelection.hasRange() && !selectedTimelineTracks().empty()) {
        return applyIndependentTrackRange(editGeneration, timeSelection, selectedTimelineTracks(),
                                          IndependentRangeAction::trimOutside);
    }

    const juce::ScopedLock guard(lock);
    if (dragMode != kDragNone) {
        status.setText("Finish or cancel the current drag first.", juce::dontSendNotification);
        return false;
    }

    juce::String error;
    const auto scope = timeSelection;
    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            return carta::trimTracksToRange(candidate, scope, error);
        },
        [&] { selectedClip = -1; }, "Trim outside selection", error);
    if (changed) {
        status.setText("Trimmed selected tracks outside the time selection.",
                       juce::dontSendNotification);
    } else {
        status.setText(error.isNotEmpty() ? error : "Trim had no effect.",
                       juce::dontSendNotification);
    }
    repaint();
    return changed;
}

void Studio::deleteSelection() {
    if (timeSelection.hasRange() && !selectedTimelineTracks().empty()) {
        applyIndependentTrackRange(editGeneration, timeSelection, selectedTimelineTracks(),
                                   IndependentRangeAction::deleteGap, true);
        return;
    }

    const juce::ScopedLock guard(lock);
    juce::String error;
    const auto scope = timeSelection;
    if (prepareRangeSessionEdit(
            [&](carta::Session& candidate) { return carta::removeRange(candidate, scope, &error); },
            [&] { timeSelection.end = timeSelection.start; }, "Delete selection", error)) {
        status.setText("Deleted selection; its time remains as a gap.", juce::dontSendNotification);
    } else {
        status.setText(error.isEmpty() ? "No audio in the selected range." : error,
                       juce::dontSendNotification);
    }
    repaint();
}

bool Studio::resolveRippleScope(bool focusedTrack, carta::TimeSelection& scope) {
    scope = timeSelection;
    if (!focusedTrack) {
        return true;
    }
    if (selectedTrack < 0 || selectedTrack >= static_cast<int>(session.tracks.size())) {
        status.setText("Focus a track first.", juce::dontSendNotification);
        return false;
    }
    scope.firstTrack = scope.lastTrack = selectedTrack;
    return true;
}

void Studio::rippleSelection(bool allTracks, bool focusedTrack) {
    if (!allTracks && !focusedTrack && timeSelection.hasRange() &&
        !selectedTimelineTracks().empty()) {
        applyIndependentTrackRange(editGeneration, timeSelection, selectedTimelineTracks(),
                                   IndependentRangeAction::closeGap, true);
        return;
    }

    const juce::ScopedLock guard(lock);
    const bool keepPlayback = selectionPlaybackContinues();
    carta::TimeSelection scope;
    if (!resolveRippleScope(focusedTrack, scope)) {
        return;
    }

    juce::String error;
    if (prepareRangeSessionEdit(
            [&](carta::Session& candidate) {
                return carta::rippleRemoveRange(candidate, scope, allTracks, &error);
            },
            [&] {
                if (!keepPlayback) {
                    position = static_cast<double>(scope.start) / carta::sessionRate;
                }
                timeSelection = scope;
                timeSelection.end = timeSelection.start;
                selectedClip = -1;
                selectedMarker = -1;
            },
            allTracks      ? "Delete and close gap on all tracks"
            : focusedTrack ? "Delete and close gap on focused track"
                           : "Delete and close gap on selected tracks",
            error)) {
        status.setText(
            allTracks ? "Closed the gap on all tracks; markers moved with the timeline."
            : focusedTrack
                ? "Closed the gap on the focused track; other tracks and markers stayed in place."
                : "Closed the gap on selected tracks; other tracks and markers stayed in place.",
            juce::dontSendNotification);
    } else {
        status.setText(error.isEmpty() ? "Nothing follows or intersects the selected range."
                                       : error,
                       juce::dontSendNotification);
    }
    repaint();
}

void Studio::rippleCutSelection(bool allTracks, bool focusedTrack) {
    if (!allTracks && !focusedTrack && timeSelection.hasRange() &&
        !selectedTimelineTracks().empty()) {
        applyIndependentTrackRange(editGeneration, timeSelection, selectedTimelineTracks(),
                                   IndependentRangeAction::cutClose);
        return;
    }

    const juce::ScopedLock guard(lock);
    const bool keepPlayback = selectionPlaybackContinues();
    juce::String error;
    carta::TimeSelection scope;
    if (!resolveRippleScope(focusedTrack, scope)) {
        return;
    }

    carta::Clipboard proposedClipboard;
    if (prepareRangeSessionEdit(
            [&](carta::Session& candidate) {
                return carta::rippleCutRange(candidate, scope, allTracks, proposedClipboard, error);
            },
            [&] {
                clipboard = std::move(proposedClipboard);
                timeSelection = scope;
                auto cutPosition = position;
                carta::finishRangeCut(timeSelection, selectedTrack, selectedClip, selectedMarker,
                                      cutPosition, allTracks,
                                      static_cast<int>(session.tracks.size()));
                if (!keepPlayback) {
                    position = cutPosition;
                }
            },
            allTracks      ? "Cut and close gap on all tracks"
            : focusedTrack ? "Cut and close gap on focused track"
                           : "Cut and close gap on selected tracks",
            error)) {
        status.setText(
            allTracks ? "Cut all tracks and closed the gap; later markers moved."
            : focusedTrack
                ? "Cut the focused track and closed the gap; other tracks and markers stayed in "
                  "place."
                : "Cut selected tracks and closed the gap; other tracks and markers stayed in "
                  "place.",
            juce::dontSendNotification);
    } else {
        status.setText(error.isEmpty() ? "Nothing was cut." : error, juce::dontSendNotification);
    }
    repaint();
}

void Studio::insertSelectionGap(bool allTracks, bool focusedTrack) {
    if (!allTracks && !focusedTrack && timeSelection.hasRange() &&
        !selectedTimelineTracks().empty()) {
        applyIndependentTrackRange(editGeneration, timeSelection, selectedTimelineTracks(),
                                   IndependentRangeAction::insertGap);
        return;
    }

    const juce::ScopedLock guard(lock);
    const bool keepPlayback = selectionPlaybackContinues();
    carta::TimeSelection scope;
    if (!resolveRippleScope(focusedTrack, scope)) {
        return;
    }

    juce::String error;
    if (prepareRangeSessionEdit(
            [&](carta::Session& candidate) {
                return carta::insertGap(candidate, scope, allTracks, error);
            },
            [&] {
                if (!keepPlayback) {
                    position = static_cast<double>(scope.start) / carta::sessionRate;
                }
                if (focusedTrack) {
                    timeSelection = scope;
                }
                if (allTracks) {
                    timeSelection.firstTrack = 0;
                    timeSelection.lastTrack = static_cast<int>(session.tracks.size()) - 1;
                }
                selectedClip = -1;
                selectedMarker = -1;
            },
            allTracks      ? "Insert gap on all tracks"
            : focusedTrack ? "Insert gap on focused track"
                           : "Insert gap on selected tracks",
            error)) {
        status.setText(
            allTracks ? "Inserted gap on all tracks; markers moved with the timeline."
            : focusedTrack
                ? "Inserted a gap on the focused track; other tracks and markers stayed in place."
                : "Inserted gap on selected tracks; other tracks and markers stayed in place.",
            juce::dontSendNotification);
    } else {
        status.setText(error, juce::dontSendNotification);
    }
    repaint();
}

bool Studio::addMarkerAtEditPoint(const juce::String& name) {
    if (!timeSelection.hasPoint() ||
        timeSelection.firstTrack >= static_cast<int>(session.tracks.size()) ||
        timeSelection.start >= carta::projectEndSample(session) || name.trim().isEmpty()) {
        status.setText("Place a valid edit point inside the master and provide a marker name.",
                       juce::dontSendNotification);
        return false;
    }

    const auto sample = timeSelection.start;
    juce::String error;
    int index = -1;
    const bool changed = mutateMarkers(
        [&](carta::Session& candidate) {
            candidate.markers.push_back({0, name});
            index = static_cast<int>(candidate.markers.size()) - 1;
            return carta::setMarkerSample(candidate, index, sample, error);
        },
        [&] { selectedMarker = index; }, "Add marker at edit point", error);
    status.setText(changed ? "Added track marker at edit point sample " + juce::String(sample) + "."
                   : error.isNotEmpty() ? error
                                        : "Marker unchanged.",
                   juce::dontSendNotification);
    return changed;
}

void Studio::setEditPointAtPlayhead() {
    double frame = 0;
    {
        const juce::ScopedLock guard(lock);
        frame = position * carta::sessionRate;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (editPointNavigationHook) {
        editPointNavigationHook();
    }
#endif
    const auto end = carta::projectEndSample(session);
    if (selectedTrack < 0 || selectedTrack >= static_cast<int>(session.tracks.size()) ||
        !std::isfinite(frame) || frame < 0 || frame > static_cast<double>(end)) {
        status.setText("Focus a track and place the playhead within the master first.",
                       juce::dontSendNotification);
        return;
    }

    const auto sample = static_cast<juce::int64>(std::llround(frame));
    timeSelection = {sample, sample, selectedTrack, selectedTrack};
    selectedClip = selectedMarker = -1;
    timelineClipSelection.clear();
    timelineTracks.clear();
    keyboardRange.active = false;
    keyboardTracks.active = false;
    status.setText("Edit point: sample " + juce::String(sample) + ", track " +
                       juce::String(selectedTrack + 1) + ".",
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    menuItemsChanged();
    repaint();
}

bool Studio::selectFromEditPoint(int action) {
    if (!timeSelection.hasPoint() ||
        timeSelection.firstTrack >= static_cast<int>(session.tracks.size()) || action < 0 ||
        action > 7) {
        status.setText("Place an edit point on a track first.", juce::dontSendNotification);
        return false;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (editPointNavigationHook) {
        editPointNavigationHook();
    }
#endif
    const auto track = timeSelection.firstTrack;
    const auto boundary = action >= 6 ? (action == 6 ? carta::SelectionBoundary::previousClip
                                                     : carta::SelectionBoundary::nextClip)
                                      : static_cast<carta::SelectionBoundary>(action);
    const carta::TrackRange scope =
        action >= 6 ? carta::TrackRange{} : carta::TrackRange{track, track};
    carta::TimeSelection result;
    if (!carta::selectToBoundary(session, timeSelection.start, boundary, result, scope)) {
        status.setText("No distinct boundary in that direction.", juce::dontSendNotification);
        return false;
    }

    result.firstTrack = result.lastTrack = track;
    {
        const juce::ScopedLock guard(lock);
        const bool keepPlayback = selectionPlaybackContinues();
        stopSpecialAudition();
        if (!keepPlayback) {
            playing = false;
            auditionActive = false;
            cutPreviewActive = false;
        }
    }
    if (!carta::activateTimeSelection(timeSelection, timelineClipSelection, result,
                                      static_cast<int>(session.tracks.size()))) {
        return false;
    }
    timelineTracks.clear();
    selectedTrack = track;
    selectedClip = selectedMarker = -1;
    keyboardRange.active = false;
    keyboardTracks.active = false;
    status.setText("Selected samples " + juce::String(result.start) + " to " +
                       juce::String(result.end) + " (end exclusive), track " +
                       juce::String(track + 1) + ".",
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    menuItemsChanged();
    repaint();
    return true;
}

bool Studio::selectGapAtEditPoint(int scope, const std::vector<int>& explicitTracks) {
    if (!timeSelection.hasPoint() ||
        timeSelection.firstTrack >= static_cast<int>(session.tracks.size())) {
        status.setText("Place an edit point on a track first.", juce::dontSendNotification);
        return false;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (editPointNavigationHook) {
        editPointNavigationHook();
    }
#endif
    const int track = timeSelection.firstTrack;
    carta::TimeSelection result;
    std::vector<int> tracks;
    if (scope == 0) {
        tracks = {track};
    } else if (scope == 1) {
        tracks = selectedTimelineTracks();
    } else if (scope == 2) {
        for (int i = 0; i < static_cast<int>(session.tracks.size()); ++i) {
            tracks.push_back(i);
        }
    } else if (scope == 3) {
        tracks = explicitTracks;
    }
    if (tracks.empty()) {
        status.setText("Select one or more tracks first.", juce::dontSendNotification);
        return false;
    }
    if (!carta::selectClipGapOnTracks(session, tracks, timeSelection.start, result)) {
        status.setText("The edit point is not inside a clip gap on every selected track.",
                       juce::dontSendNotification);
        return false;
    }

    {
        const juce::ScopedLock guard(lock);
        const bool keepPlayback = selectionPlaybackContinues();
        stopSpecialAudition();
        if (!keepPlayback) {
            playing = false;
            auditionActive = false;
            cutPreviewActive = false;
        }
    }
    if (!carta::activateTimeSelection(timeSelection, timelineClipSelection, result,
                                      static_cast<int>(session.tracks.size()))) {
        return false;
    }
    timelineTracks.clear();
    if (scope == 1 || scope == 3) {
        timelineTracks.insert(tracks.begin(), tracks.end());
        timelineTrackRevision = editGeneration;
    }
    selectedTrack =
        std::find(tracks.begin(), tracks.end(), track) != tracks.end() ? track : result.firstTrack;
    selectedClip = selectedMarker = -1;
    keyboardRange.active = false;
    keyboardTracks.active = false;

    const auto rows = scope == 0   ? "track " + juce::String(track + 1)
                      : scope == 2 ? juce::String("all tracks")
                                   : juce::String("independently selected tracks");
    status.setText("Selected clip gap: samples " + juce::String(result.start) + " to " +
                       juce::String(result.end) + " (end exclusive), " + rows + ".",
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    menuItemsChanged();
    repaint();
    return true;
}

bool Studio::navigateEditPoint(int action) {
    if (!timeSelection.hasPoint() ||
        timeSelection.firstTrack >= static_cast<int>(session.tracks.size()) || action < 0 ||
        action > 7) {
        status.setText("Place an edit point on a track first.", juce::dontSendNotification);
        return false;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (editPointNavigationHook) {
        editPointNavigationHook();
    }
#endif
    const auto boundary = action >= 6 ? (action == 6 ? carta::SelectionBoundary::previousClip
                                                     : carta::SelectionBoundary::nextClip)
                                      : static_cast<carta::SelectionBoundary>(action);
    const carta::TrackRange scope =
        action >= 6 ? carta::TrackRange{}
                    : carta::TrackRange{timeSelection.firstTrack, timeSelection.firstTrack};
    carta::TimeSelection result;
    const auto original = timeSelection.start;
    if (!carta::selectToBoundary(session, original, boundary, result, scope)) {
        status.setText("No distinct boundary in that direction.", juce::dontSendNotification);
        return false;
    }

    const auto sample = result.start == original ? result.end : result.start;
    timeSelection.start = timeSelection.end = sample;
    keyboardRange.active = false;
    const double at = static_cast<double>(sample) / carta::sessionRate;
    if (at < viewStart || at >= viewStart + viewLength()) {
        viewStart =
            juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()), at - viewLength() * 0.5);
    }
    status.setText("Edit point: sample " + juce::String(sample) + ", track " +
                       juce::String(timeSelection.firstTrack + 1) + ".",
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    repaint();
    return true;
}

void Studio::nudgeEditPoint(int direction) {
    if (!timeSelection.hasPoint() ||
        timeSelection.firstTrack >= static_cast<int>(session.tracks.size())) {
        status.setText("Place an edit point on a track first.", juce::dontSendNotification);
        return;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (editPointNavigationHook) {
        editPointNavigationHook();
    }
#endif
    const auto end = carta::projectEndSample(session);
    const auto sample = timeSelection.start;
    if ((direction != 1 && direction != -1) || sample > end || (direction < 0 && sample == 0) ||
        (direction > 0 && sample >= end)) {
        status.setText("Edit point cannot move farther in that direction.",
                       juce::dontSendNotification);
        return;
    }
    timeSelection.start += direction;
    timeSelection.end = timeSelection.start;
    keyboardRange.active = false;
    const double at = static_cast<double>(timeSelection.start) / carta::sessionRate;
    if (at < viewStart || at >= viewStart + viewLength()) {
        viewStart =
            juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()), at - viewLength() * 0.5);
    }
    status.setText("Edit point: sample " + juce::String(timeSelection.start) + ", track " +
                       juce::String(timeSelection.firstTrack + 1) + ".",
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    repaint();
}

// ============================================================================
// Transport Follow, Undo/Redo History, Saved Loop & Audition State
// ============================================================================

void Studio::setFollowMode(int newMode) {
    followMode = newMode;
    followSuspended = false;
    menuItemsChanged();
    status.setText(newMode == 0   ? "Playhead following disabled."
                   : newMode == 1 ? "Playhead following: page."
                                  : "Playhead following: pinned.",
                   juce::dontSendNotification);
}

void Studio::suspendFollowingForNavigation() {
    if (!playing.load(std::memory_order_relaxed) || followMode == 0 || followSuspended) {
        return;
    }
    followSuspended = true;
    menuItemsChanged();
    const juce::String message =
        "Playhead following paused for navigation. Choose View > Follow playhead > Resume "
        "following.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
}

void Studio::resumeFollowing() {
    if (followMode == 0) {
        return;
    }
    followSuspended = false;
    menuItemsChanged();
    status.setText("Playhead following resumed.", juce::dontSendNotification);
    repaint();
}

void Studio::pasteSelection(int policy, bool atPoint) {
    const juce::ScopedLock guard(lock);
    if (atPoint && (!timeSelection.hasPoint() ||
                    timeSelection.firstTrack >= static_cast<int>(session.tracks.size()) ||
                    timeSelection.start > carta::projectEndSample(session))) {
        status.setText("Place a valid edit point on a track first.", juce::dontSendNotification);
        return;
    }

    const int track = atPoint                    ? timeSelection.firstTrack
                      : timeSelection.hasRange() ? timeSelection.firstTrack
                                                 : selectedTrack;
    const auto frame = editPlayhead() * carta::sessionRate;
    if (!atPoint && (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0)) {
        status.setText("Invalid playhead position.", juce::dontSendNotification);
        return;
    }
    const auto at = atPoint ? timeSelection.start : static_cast<juce::int64>(std::llround(frame));

    juce::String error;
    std::vector<carta::ClipReference> pastedClips;
    carta::ClipSelection nextSelection;
    const auto nextGeneration = editGeneration + 1;
    const bool pasted = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            const bool result =
                policy == PastePolicy::replace
                    ? carta::pasteRangeReplacing(candidate, clipboard, track, at, error,
                                                 &pastedClips)
                : policy >= PastePolicy::insertDestination
                    ? carta::pasteRangeInserting(candidate, clipboard, track, at,
                                                 policy == PastePolicy::insertAll, error,
                                                 &pastedClips)
                    : carta::pasteRange(candidate, clipboard, track, at, &error, &pastedClips);
            if (result && clipboard.wholeClips && !pastedClips.empty()) {
                if (!nextSelection.restore(candidate, pastedClips, nextGeneration)) {
                    error = "Could not select the pasted clips.";
                    return false;
                }
#if CARTA_STUDIO_COMPONENT_TEST
                if (preparedSelectionHook) {
                    preparedSelectionHook();
                }
#endif
            }
            return result;
        },
        [&] {
            selectedClip = -1;
            timeSelection = {};
            if (clipboard.wholeClips && !pastedClips.empty()) {
                timelineClipSelection = std::move(nextSelection);
            }
        },
        policy == PastePolicy::replace             ? "Paste replacing audio"
        : policy == PastePolicy::insertDestination ? "Paste and insert on selected tracks"
        : policy == PastePolicy::insertAll         ? "Paste and insert on all tracks"
                                                   : "Paste clips",
        error);

    if (!pasted) {
        status.setText(error, juce::dontSendNotification);
        return;
    }
    if (clipboard.wholeClips && !pastedClips.empty()) {
        selectedTrack = pastedClips.front().track;
        selectedClip = pastedClips.front().clip;
        selectedMarker = -1;
        revealTrack(selectedTrack);
        menuItemsChanged();
    }

    const char* message =
        policy == PastePolicy::replace ? "Pasted at playhead, replacing audio in the copied span."
        : policy == PastePolicy::insertDestination
            ? "Pasted and inserted time on destination tracks; markers stayed in place."
        : policy == PastePolicy::insertAll
            ? "Pasted and inserted time on all tracks; later markers moved."
            : "Pasted at playhead; overlapping clips are mixed.";
    status.setText(
        atPoint ? "Pasted at edit point sample " + juce::String(at) + "; " +
                      juce::String(policy == PastePolicy::overlap ? "overlapping clips are mixed."
                                   : policy == PastePolicy::replace
                                       ? "audio in the copied span was replaced."
                                   : policy == PastePolicy::insertDestination
                                       ? "time was inserted on destination tracks."
                                       : "time was inserted on all tracks and markers moved.")
                : juce::String(message),
        juce::dontSendNotification);
    repaint();
}

carta::EditorState Studio::captureState() const {
    return {session,
            timeSelection,
            selectedTrack,
            selectedClip,
            selectedMarker,
            masterChannels,
            position,
            timelineClipSelection.get(editGeneration),
            selectedTimelineTracks()};
}

carta::EditorState Studio::captureSelectionState() {
    carta::EditorState state{{},
                             timeSelection,
                             selectedTrack,
                             selectedClip,
                             selectedMarker,
                             masterChannels,
                             position,
                             {},
                             {}};
    {
        const juce::ScopedUnlock unlock(lock);
        state.selectedClips = timelineClipSelection.get(editGeneration);
        state.selectedTracks = selectedTimelineTracks();
    }
    return state;
}

bool Studio::reorderCurrentTrack(int source, int destination) {
    const juce::ScopedLock guard(lock);
    auto before = captureSelectionState();
    const juce::ScopedUnlock unlock(lock);
    return reorderTrack(source, destination, std::move(before));
}

bool Studio::changeTracks(int operation, int source, const juce::String& name,
                          juce::String& error) {
    const juce::ScopedLock guard(lock);
    const int count = static_cast<int>(session.tracks.size());
    if (operation < TrackOperation::add || operation > TrackOperation::remove ||
        (operation != TrackOperation::add && (source < 0 || source >= count))) {
        error = "Invalid track index.";
        return false;
    }
    if (dragMode != kDragNone) {
        error = "Finish or cancel the current drag first.";
        return false;
    }

    carta::EditorState before;
    before.selection = timeSelection;
    {
        const juce::ScopedUnlock unlock(lock);
        before.selectedClips = timelineClipSelection.get(editGeneration);
        before.selectedTracks = selectedTimelineTracks();
    }
    stopSpecialAudition();

    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            if (operation == TrackOperation::add) {
                carta::Track track;
                track.name = name;
                candidate.tracks.push_back(std::move(track));
            } else if (operation == TrackOperation::duplicate) {
                auto copiedTrack = candidate.tracks[static_cast<size_t>(source)];
                copiedTrack.name += " copy";
                std::vector<carta::Clip*> copies;
                for (auto& clip : copiedTrack.clips) {
                    copies.push_back(&clip);
                }
                if (!carta::forkCopiedClipGroups(candidate, copies, error)) {
                    return false;
                }
                candidate.tracks.insert(candidate.tracks.begin() + source + 1,
                                        std::move(copiedTrack));
            } else {
                candidate.tracks.erase(candidate.tracks.begin() + source);
            }
            return carta::preservesMarkerBounds(session, candidate, error);
        },
        [&] {
            if (operation == TrackOperation::add) {
                selectedTrack = count;
                selectedClip = -1;
            } else if (operation == TrackOperation::duplicate) {
                selectedTrack = source + 1;
                selectedClip = -1;
            } else {
                if (selectedTrack == source) {
                    selectedTrack = -1;
                    selectedClip = -1;
                } else if (selectedTrack > source) {
                    --selectedTrack;
                }
                firstTrack = juce::jlimit(
                    0, std::max(0, static_cast<int>(session.tracks.size()) - 1), firstTrack);
            }
        },
        operation == TrackOperation::add         ? "Add track"
        : operation == TrackOperation::duplicate ? "Duplicate track"
                                                 : "Remove track",
        error);
    if (!changed) {
        return false;
    }

    restoreMappedTrackSelections(
        before,
        [=](int index) {
            if (operation == TrackOperation::duplicate && index > source) {
                return index + 1;
            }
            if (operation == TrackOperation::remove) {
                return index == source ? -1 : (index > source ? index - 1 : index);
            }
            return index;
        },
        count);
    if (selectedTrack >= 0) {
        revealTrack(selectedTrack);
    }
    return true;
}

bool Studio::reorderTrack(int source, int destination, carta::EditorState before) {
    const juce::ScopedLock guard(lock);
    const int count = static_cast<int>(session.tracks.size());
    if (dragMode != kDragNone || source < 0 || destination < 0 || source >= count ||
        destination >= count || source == destination) {
        return false;
    }

    const auto remap = [source, destination](int index) {
        if (index == source) {
            return destination;
        }
        if (source < destination && index > source && index <= destination) {
            return index - 1;
        }
        if (destination < source && index >= destination && index < source) {
            return index + 1;
        }
        return index;
    };

    const auto generation = editGeneration;
    auto selection = before.selection;
    std::set<int> rows;
    carta::ClipSelection nextSelection;
    carta::Session candidate;
    carta::PlaybackRange candidateRange;
    bool prepared = false;
    const int focusedClip = before.selectedTrack == source ? before.selectedClip : -1;
    const int previousMarker = before.selectedMarker;

    stopSpecialAudition();
    {
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (rangePreparationHook) {
            rangePreparationHook();
        }
#endif
        before.session = {};
        auto clips = before.selectedClips;
        for (auto& clip : clips) {
            clip.track = remap(clip.track);
        }
        for (int row : before.selectedTracks) {
            rows.insert(remap(row));
        }
        const bool explicitRows = !rows.empty();
        if (!explicitRows && selection.hasRange() && selection.lastTrack < count) {
            for (int row = selection.firstTrack; row <= selection.lastTrack; ++row) {
                rows.insert(remap(row));
            }
        }
        if (!rows.empty() && selection.hasRange()) {
            selection.firstTrack = *rows.begin();
            selection.lastTrack = *rows.rbegin();
            if (!explicitRows && static_cast<size_t>(selection.lastTrack - selection.firstTrack +
                                                     1) == rows.size()) {
                rows.clear();
            }
        } else if (selection.firstTrack >= 0 && selection.lastTrack >= selection.firstTrack &&
                   selection.lastTrack < count) {
            const int first = remap(selection.firstTrack);
            const int last = remap(selection.lastTrack);
            selection.firstTrack = std::min(first, last);
            selection.lastTrack = std::max(first, last);
        }

        candidate = session;
        auto track = std::move(candidate.tracks[static_cast<size_t>(source)]);
        candidate.tracks.erase(candidate.tracks.begin() + source);
        candidate.tracks.insert(candidate.tracks.begin() + destination, std::move(track));
        prepared = nextSelection.restore(candidate, clips, generation + 1);
        if (prepared) {
            candidateRange = carta::fullMasterPlaybackRange(candidate);
        }
    }

    if (!prepared || editGeneration != generation) {
        status.setText("Could not move the track; the project or selection changed.",
                       juce::dontSendNotification);
        return false;
    }

    std::swap(session, candidate);
    before.session = std::move(candidate);
    selectedTrack = destination;
    selectedClip = focusedClip;
    selectedMarker = previousMarker;
    timeSelection = selection;
    timelineClipSelection = std::move(nextSelection);
    timelineTracks = std::move(rows);
    timelineTrackRevision = generation + 1;
    keyboardRange.active = false;
    pushPreparedHistory(std::move(before), "Move track", candidateRange);
    revealTrack(destination);
    menuItemsChanged();
    repaint();

    const auto message = "Track moved to row " + juce::String(destination + 1) + ".";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    return true;
}

void Studio::applyStateFields(const carta::EditorState& state, bool keepTransport) {
    timeSelection = state.selection;
    selectedTrack = state.selectedTrack;
    selectedClip = state.selectedClip;
    selectedMarker = state.selectedMarker;
    if (!timelineClipSelection.restore(session, state.selectedClips, editGeneration)) {
        timelineClipSelection.clear();
    }
    timelineTracks.clear();
    for (int track : state.selectedTracks) {
        if (track >= 0 && track < static_cast<int>(session.tracks.size())) {
            timelineTracks.insert(track);
        }
    }
    timelineTrackRevision = editGeneration;
    masterChannels = state.masterChannels;
    if (!keepTransport) {
        position = state.position;
        playing = false;
        auditionActive = false;
    }
    mode.setSelectedId(masterChannels, juce::dontSendNotification);
    firstTrack =
        juce::jlimit(0, std::max(0, static_cast<int>(session.tracks.size()) - 1), firstTrack);
}

void Studio::applyState(carta::EditorState state) {
    session = std::move(state.session);
    applyStateFields(state);
}

size_t Studio::traverseHistory(bool redo, size_t steps, bool& pruned) {
    const juce::ScopedLock guard(lock);
    stopSpecialAudition();
    auto state = captureSelectionState();
    size_t moved = 0;
    const auto generation = editGeneration;
    const bool rangeValid = masterRangeValid && masterRangeRevision == generation;
    carta::PlaybackRange range = masterRange;
    bool allGeometryStable = true;
    {
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (historyTraversalSnapshotHook) {
            historyTraversalSnapshotHook();
        }
#endif
        state.session = session;
#if CARTA_STUDIO_COMPONENT_TEST
        if (historyTraversalPreparedHook) {
            historyTraversalPreparedHook();
        }
#endif
        while (moved < steps) {
            bool stepStable = false;
            if (!(redo ? editHistory.redo(state, &stepStable)
                       : editHistory.undo(state, &stepStable))) {
                break;
            }
            allGeometryStable = allGeometryStable && stepStable;
            ++moved;
        }
        if (moved && (!allGeometryStable || !rangeValid)) {
            range = carta::fullMasterPlaybackRange(state.session);
        }
    }
    if (!moved) {
        return 0;
    }

    const bool loopWasPlaying = savedLoopPlaying();
    const bool livePlayback = playing && (!auditionActive || loopWasPlaying);
    const double livePosition = position;
    const bool loopFits =
        !loopWasPlaying || static_cast<double>(savedLoop.end) / carta::sessionRate <= range.end;
    const bool keepPlayback = livePlayback && loopFits && std::isfinite(livePosition) &&
                              livePosition >= 0 && livePosition < range.end;
    if (!keepPlayback) {
        playing = false;
        auditionActive = false;
    }
    editGeneration += moved;
    if (allGeometryStable) {
        selectionMemory.retainAfterGeometryStableEdit(generation, editGeneration);
    }
    std::swap(session, state.session);
    applyStateFields(state, keepPlayback);
    if (livePlayback && !keepPlayback) {
        position = std::isfinite(livePosition)
                       ? juce::jlimit(0.0, std::max(0.0, range.end), livePosition)
                       : 0;
        if (!loopFits) {
            audioLoopInvalid.store(true);
        }
    }
    masterRange = range;
    masterRangeRevision = editGeneration;
    masterRangeValid = true;
    if (allGeometryStable && playbackIndex && playbackIndexRevision == generation) {
        playbackIndexRevision = editGeneration;
    }
    keyboardTracks.active = false;
    dirty = editHistory.isDirty() || pendingDurability || loopDirty();
    {
        const juce::ScopedUnlock unlock(lock);
        state.session = {};
        editHistory.enforceRetainedMediaBudget(session, clipboard);
        pruned = editHistory.takeBudgetPruned();
    }
    return moved;
}

void Studio::restoreHistory(bool redo) {
    const auto action = redo ? editHistory.redoAction() : editHistory.undoAction();
    bool pruned = false;
    const auto moved = traverseHistory(redo, 1, pruned);
#if CARTA_STUDIO_COMPONENT_TEST
    if (historyResultHook) {
        historyResultHook();
    }
#endif
    if (moved) {
        status.setText((redo ? juce::String("Redid ") : juce::String("Undid ")) + action + "." +
                           (pruned ? " Older history was discarded to limit memory." : ""),
                       juce::dontSendNotification);
        menuItemsChanged();
    }
    repaint();
}

void Studio::jumpHistory(const HistoryMenuCommand& command) {
#if CARTA_STUDIO_COMPONENT_TEST
    if (historyValidationHook) {
        historyValidationHook();
    }
#endif
    const auto targets = editHistory.targets(command.redo);
    if (command.generation != editGeneration || command.steps == 0 ||
        command.steps > targets.size() || targets[command.steps - 1].revision != command.revision) {
        status.setText("History changed; open the menu again.", juce::dontSendNotification);
        return;
    }

    bool pruned = false;
    const auto moved = traverseHistory(command.redo, command.steps, pruned);
#if CARTA_STUDIO_COMPONENT_TEST
    if (historyResultHook) {
        historyResultHook();
    }
#endif
    const auto message =
        juce::String(command.redo ? "Redid " : "Undid ") + juce::String(static_cast<int>(moved)) +
        (moved == 1 ? " edit." : " edits.") +
        (moved < command.steps ? " Remaining history was discarded by the memory limit."
         : pruned              ? " Older history was discarded to limit memory."
                               : "");
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
}

void Studio::setAuditionRange(carta::PlaybackRange range, bool saved) {
    discPreviewActive = false;
    auditionRange = range;
    savedLoopAuditionKind = saved;
}

bool Studio::savedLoopAudition() const {
    return savedLoopAuditionKind && auditionActive && auditionRange.loop && !cutPreviewActive &&
           !groupAuditionActive && auditionRange.tracks.first < 0 && auditionRange.onlyClip < 0 &&
           savedLoop.end > savedLoop.start &&
           std::equal_to<double>{}(auditionRange.start,
                                   static_cast<double>(savedLoop.start) / carta::sessionRate) &&
           std::equal_to<double>{}(auditionRange.end,
                                   static_cast<double>(savedLoop.end) / carta::sessionRate);
}

bool Studio::savedLoopPlaying() const {
    return playing && savedLoopAudition();
}

bool Studio::selectionPlaybackContinues() const {
    return playing && (!auditionActive || savedLoopAudition());
}

bool Studio::loopDirty() const noexcept {
    return !sameLoop(savedLoop, persistedLoop);
}

juce::String Studio::describeSavedLoop(bool loopPlaying) const {
    if (savedLoop.end <= savedLoop.start) {
        return "No saved loop region.";
    }
    return "Loop region samples " + juce::String(savedLoop.start) + " to " +
           juce::String(savedLoop.end) + ", end exclusive." +
           (savedLoop.end > carta::projectEndSample(session) ? " Outside the master duration."
            : loopPlaying                                    ? " Looping."
                                                             : "");
}

juce::String Studio::describeSavedLoop() const {
    bool loopPlaying = false;
    {
        const juce::ScopedLock guard(lock);
        loopPlaying = savedLoopPlaying();
    }
    return describeSavedLoop(loopPlaying);
}

void Studio::prepareSelectionFocus() {
    const bool keepPlayback = selectionPlaybackContinues();
    stopSpecialAudition();
    if (!keepPlayback) {
        playing = false;
        auditionActive = false;
    }
}

bool Studio::setSavedLoop(juce::int64 start, juce::int64 end, juce::String& error) {
#if CARTA_STUDIO_COMPONENT_TEST
    if (loopPreparationHook) {
        loopPreparationHook();
    }
#endif
    if (start < 0 || end <= start || end > carta::projectEndSample(session)) {
        error = "Choose a nonempty loop within the master duration.";
        return false;
    }

    bool changed = false;
    {
        const juce::ScopedLock guard(lock);
        const bool updateAudition = savedLoopAudition();
        changed = !sameLoop(savedLoop, {start, end});
        savedLoop = {start, end};
        if (updateAudition) {
            auditionRange.start = static_cast<double>(start) / carta::sessionRate;
            auditionRange.end = static_cast<double>(end) / carta::sessionRate;
            if (position < auditionRange.start || position >= auditionRange.end) {
                position = auditionRange.start;
            }
        }
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (loopPublicationHook) {
        loopPublicationHook();
    }
#endif
    if (changed) {
        dirty = editHistory.isDirty() || pendingDurability || loopDirty();
        lastEditTime = juce::Time::getMillisecondCounterHiRes();
    }
    status.setText("Loop region: samples " + juce::String(start) + " to " + juce::String(end) +
                       " (end exclusive).",
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    menuItemsChanged();
    repaint();
    return true;
}

bool Studio::clearSavedLoop() {
    {
        const juce::ScopedLock guard(lock);
        if (savedLoop.end <= savedLoop.start) {
            return false;
        }
        if (savedLoopAudition()) {
            playing = false;
            auditionActive = false;
        }
        savedLoop = {};
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (loopPublicationHook) {
        loopPublicationHook();
    }
#endif
    dirty = editHistory.isDirty() || pendingDurability || loopDirty();
    lastEditTime = juce::Time::getMillisecondCounterHiRes();
    status.setText("Loop region cleared.", juce::dontSendNotification);
    menuItemsChanged();
    repaint();
    return true;
}

bool Studio::startSavedLoop(juce::String& error) {
#if CARTA_STUDIO_COMPONENT_TEST
    if (loopPreparationHook) {
        loopPreparationHook();
    }
#endif
    if (savedLoop.end <= savedLoop.start || savedLoop.end > carta::projectEndSample(session)) {
        error = "Set a loop region within the current master duration.";
        return false;
    }
    {
        const juce::ScopedLock guard(lock);
        stopSpecialAudition();
        setAuditionRange({static_cast<double>(savedLoop.start) / carta::sessionRate,
                          static_cast<double>(savedLoop.end) / carta::sessionRate, true},
                         true);
        position = auditionRange.start;
        auditionActive = true;
        cutPreviewActive = false;
        groupAuditionActive = false;
        playing = true;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (loopPublicationHook) {
        loopPublicationHook();
    }
#endif
    status.setText("Looping saved region (full mix).", juce::dontSendNotification);
    announceNavigation(status.getText());
    repaint();
    return true;
}

bool Studio::setSnapGrid(const juce::String& text) {
    juce::int64 samples = 0;
    if (!carta::parseSampleOrTime(text, samples) || samples <= 0 || samples >= 9007199254740992LL) {
        return false;
    }
    snapGridSamples = samples;
    status.setText("Snap grid: " + juce::String(samples) + " project samples.",
                   juce::dontSendNotification);
    menuItemsChanged();
    return true;
}

void Studio::editSnapGrid() {
    const auto previous = snapGridSamples;
    showTextPrompt(
        "Custom snap interval",
        "Enter a positive interval in project samples, seconds (e.g. 0.25 s), or time (e.g. "
        "0:00.250).",
        "interval", "Interval", juce::String(previous), "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), previous](bool accepted,
                                                                      const juce::String& value) {
            if (safe == nullptr || !accepted) {
                return;
            }
            if (safe->snapGridSamples != previous) {
                safe->status.setText("Snap grid changed; reopen the editor.",
                                     juce::dontSendNotification);
                return;
            }
            if (!safe->setSnapGrid(value)) {
                safe->status.setText(
                    "Enter a positive grid interval in project samples, seconds or time.",
                    juce::dontSendNotification);
            }
        });
}

juce::String Studio::describeEditLockInterruptions() const {
    const auto count = editLockSilentBlocks.load(std::memory_order_relaxed);
    return "Edit-lock silent playback blocks: " + juce::String(static_cast<juce::int64>(count)) +
           ". Other device interruptions are not counted.";
}

void Studio::resetOutputPeaks() {
    {
        const juce::ScopedLock guard(lock);
        outputPeakHold.reset();
        displayPeakLeft = displayPeakRight = 0;
    }
    status.setText("Held playback output peaks reset.", juce::dontSendNotification);
}

void Studio::resetEditLockInterruptions() {
    audioLockContention.store(false, std::memory_order_relaxed);
    editLockSilentBlocks.store(0, std::memory_order_relaxed);
    const juce::String message = "Edit-lock interruption count reset.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    repaint();
}

void Studio::stopAtPlayhead() {
    bool preview = false;
    {
        const juce::ScopedLock guard(lock);
        preview = cutPreviewActive && auditionActive;
        if (preview) {
            position = cutPreviewOriginalPosition;
        }
        playing = false;
        auditionActive = false;
        cutPreviewActive = false;
        groupAuditionActive = false;
        independentTrackAudition = false;
        savedLoopAuditionKind = false;
        playbackClock.reset();
    }
    const juce::String message = preview ? "Preview stopped; returned to the original playhead."
                                         : "Stopped at the playhead.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
}

void Studio::stopSpecialAudition() {
    if (auditionActive &&
        (cutPreviewActive || groupAuditionActive || auditionRange.tracks.first >= 0)) {
        playing = false;
        if (cutPreviewActive) {
            position = cutPreviewOriginalPosition;
        }
        auditionActive = false;
        cutPreviewActive = false;
        groupAuditionActive = false;
        discPreviewActive = false;
    }
}

double Studio::editPlayhead() const noexcept {
    return auditionActive && cutPreviewActive ? cutPreviewOriginalPosition : position;
}

bool Studio::setMinimumMasterDuration(juce::uint64 revision, juce::int64 end, juce::String& error) {
    if (end < 0 || end >= 9007199254740992LL) {
        error = "Enter a valid nonnegative duration.";
        return false;
    }
    if (!dialogTargetCurrent(revision)) {
        error = "Project changed; reopen the duration editor.";
        return false;
    }
    const bool changed = mutateGeometryState(
        [&](carta::Session& candidate, double) {
            if (candidate.minimumEndSample == end) {
                error = "Master duration unchanged.";
                return false;
            }
            candidate.minimumEndSample = end;
            return true;
        },
        error, "Change master duration");
    if (!changed && error.isEmpty()) {
        error = "Master duration unchanged.";
    }
    return changed;
}

void Studio::editMinimumMasterDuration() {
    const auto revision = editGeneration;
    showTextPrompt(
        "Minimum master duration",
        "Enter samples, seconds (e.g. 90 s), or a time such as 1:30.000. The master includes "
        "silence up to this point; clips beyond it still play. Enter 0 to follow the last clip.",
        "duration", "Minimum duration", juce::String(session.minimumEndSample), "Set",
        [safe = juce::Component::SafePointer<Studio>(this), revision](bool accepted,
                                                                      const juce::String& value) {
            if (safe == nullptr || !accepted) {
                return;
            }
            if (!safe->dialogTargetCurrent(revision)) {
                safe->status.setText("Project changed; reopen the duration editor.",
                                     juce::dontSendNotification);
                return;
            }
            juce::int64 end = 0;
            juce::String error;
            if (!carta::parseSampleOrTime(value, end) || end < 0 || end >= 9007199254740992LL) {
                safe->status.setText("Enter a valid nonnegative duration.",
                                     juce::dontSendNotification);
                return;
            }
            const bool changed = safe->setMinimumMasterDuration(revision, end, error);
            safe->status.setText(error.isNotEmpty() ? error
                                 : changed          ? "Minimum master duration updated."
                                                    : "Master duration unchanged.",
                                 juce::dontSendNotification);
        });
}

bool Studio::dialogTargetCurrent(juce::uint64 revision) {
    const juce::ScopedLock guard(lock);
    return editGeneration == revision && dragMode == kDragNone;
}

void Studio::retainIndexStableSelections(juce::uint64 previousGeneration) {
    timelineClipSelection.retainAfterStableEdit(previousGeneration, editGeneration);
    if (timelineTrackRevision == previousGeneration) {
        timelineTrackRevision = editGeneration;
    }
    menuItemsChanged();
}

void Studio::pushHistory(carta::EditorState old, juce::String action, bool renderGeometryStable) {
    stopSpecialAudition();
    editHistory.record(std::move(old), std::move(action), renderGeometryStable);
    const auto previousGeneration = editGeneration++;
    if (renderGeometryStable) {
        selectionMemory.retainAfterGeometryStableEdit(previousGeneration, editGeneration);
    }
    if (renderGeometryStable && masterRangeValid && masterRangeRevision == previousGeneration) {
        masterRangeRevision = editGeneration;
    } else {
        refreshCommittedMasterRange();
    }
    if (renderGeometryStable && playbackIndex && playbackIndexRevision == previousGeneration) {
        playbackIndexRevision = editGeneration;
    }
    keyboardTracks.active = false;
    dirty = editHistory.isDirty() || pendingDurability || loopDirty();
    menuItemsChanged();
    editHistory.enforceRetainedMediaBudget(session, clipboard);
    if (editHistory.takeBudgetPruned()) {
        const auto generation = editGeneration;
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Studio>(this),
                                         generation] {
            if (safe != nullptr && safe->editGeneration == generation) {
                safe->status.setText("Undo history was reduced to limit retained media and memory.",
                                     juce::dontSendNotification);
            }
        });
    }
}

void Studio::pushPreparedHistory(carta::EditorState old, juce::String action,
                                 carta::PlaybackRange range, bool renderGeometryStable) {
    stopSpecialAudition();
    const auto previousGeneration = editGeneration++;
    masterRange = range;
    masterRangeRevision = editGeneration;
    masterRangeValid = true;
    if (renderGeometryStable) {
        selectionMemory.retainAfterGeometryStableEdit(previousGeneration, editGeneration);
    }
    if (renderGeometryStable && playbackIndex && playbackIndexRevision == previousGeneration) {
        playbackIndexRevision = editGeneration;
    }
    bool pruned = false;
    {
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (preparedHistoryHook) {
            preparedHistoryHook();
        }
#endif
        editHistory.record(std::move(old), std::move(action), renderGeometryStable);
        editHistory.enforceRetainedMediaBudget(session, clipboard);
        pruned = editHistory.takeBudgetPruned();
    }
    keyboardTracks.active = false;
    dirty = editHistory.isDirty() || pendingDurability || loopDirty();
    menuItemsChanged();
    if (pruned) {
        const auto generation = editGeneration;
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Studio>(this),
                                         generation] {
            if (safe != nullptr && safe->editGeneration == generation) {
                safe->status.setText("Undo history was reduced to limit retained media and memory.",
                                     juce::dontSendNotification);
            }
        });
    }
}

// ============================================================================
// Track Routing, Split/Join, Clip Focus & Selection Navigation
// ============================================================================

bool Studio::editStereoTrackRouting(int track, bool fold, juce::String& error) {
    return mutateMixState(
        [&](carta::Session& candidate) {
            return fold ? carta::foldStereoTrackToMono(candidate, track, error)
                        : carta::swapStereoSourceChannels(candidate, track, error);
        },
        error, fold ? "Fold track to mono" : "Swap source channels");
}

bool Studio::changeMasterChannels(int channels, juce::String& error) {
    const juce::ScopedLock guard(lock);
    error.clear();
    if (dragMode != kDragNone) {
        error = "Finish or cancel the current drag first.";
        return false;
    }
    if (channels != 1 && channels != 2) {
        error = "Choose mono or stereo.";
        return false;
    }
    if (masterChannels == channels) {
        return false;
    }
    const auto generation = editGeneration;
    const bool changed = prepareRangeSessionEdit([](carta::Session&) { return true; },
                                                 [&] {
                                                     masterChannels = channels;
                                                     timelineClipSelection.retainAfterStableEdit(
                                                         generation, generation + 1);
                                                     if (timelineTrackRevision == generation) {
                                                         timelineTrackRevision = generation + 1;
                                                     }
                                                 },
                                                 "Change master channels", error, true);
    repaint();
    return changed;
}

bool Studio::editStereoChannelTracks(int track, bool combine, juce::String& error) {
    const juce::ScopedLock guard(lock);
    error.clear();
    if (dragMode != kDragNone) {
        error = "Finish or cancel the current drag first.";
        return false;
    }
    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            const bool edited = combine ? carta::combineStereoChannelTracks(candidate, track, error)
                                        : carta::splitStereoTrackChannels(candidate, track, error);
            return edited && carta::preservesMarkerBounds(session, candidate, error);
        },
        [&] {
            selectedTrack = track;
            selectedClip = -1;
            timeSelection = {};
            timelineTracks.clear();
            timelineClipSelection.clear();
            keyboardRange.active = false;
            keyboardTracks.active = false;
            firstTrack = juce::jlimit(0, std::max(0, static_cast<int>(session.tracks.size()) - 1),
                                      firstTrack);
        },
        combine ? "Combine stereo channels" : "Split stereo channels", error);
    repaint();
    return changed;
}

bool Studio::editFocusedClip(int action, juce::int64 sample, juce::String& error) {
    const juce::ScopedLock guard(lock);
    error.clear();
    if (dragMode != kDragNone) {
        error = "Finish or cancel the current drag first.";
        return false;
    }
    if (action < FocusedClipAction::split || action > FocusedClipAction::duplicate ||
        !validSelection()) {
        error = "Select a clip first.";
        return false;
    }

    const auto generation = editGeneration;
    const int track = selectedTrack;
    int clip = selectedClip;
    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            const bool edited = action == FocusedClipAction::split
                                    ? carta::splitClipAtSample(candidate, track, clip, sample)
                                : action == FocusedClipAction::remove
                                    ? carta::deleteClipGroup(candidate, {{track, clip}}, error)
                                    : carta::duplicateClipAfter(candidate, track, clip, error);
            return edited && carta::preservesMarkerBounds(session, candidate, error);
        },
        [&] {
            selectedClip = action == FocusedClipAction::remove ? -1 : clip;
            timelineClipSelection.clear();
            if (timelineTrackRevision == generation) {
                timelineTrackRevision = generation + 1;
            }
        },
        action == FocusedClipAction::split    ? "Split clip"
        : action == FocusedClipAction::remove ? "Delete clip"
                                              : "Duplicate clip",
        error);
    if (!changed && error.isEmpty()) {
        error = action == FocusedClipAction::split
                    ? "The split must leave at least one project sample on either side."
                    : "The clip was not changed.";
    }
    repaint();
    return changed;
}

bool Studio::joinFocusedClip(juce::String& error) {
    const juce::ScopedLock guard(lock);
    error.clear();
    if (dragMode != kDragNone) {
        error = "Finish or cancel the current drag first.";
        return false;
    }

    const auto generation = editGeneration;
    const int track = selectedTrack;
    int clip = selectedClip;
    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            return carta::joinNextClip(candidate, track, clip, error) &&
                   carta::preservesMarkerBounds(session, candidate, error);
        },
        [&] {
            selectedClip = clip;
            timelineClipSelection.clear();
            if (timelineTrackRevision == generation) {
                timelineTrackRevision = generation + 1;
            }
        },
        "Join clips", error);
    if (!changed && error.isEmpty()) {
        error = "No compatible clip to join.";
    }
    repaint();
    return changed;
}

bool Studio::splitSelectionToNewTracks(juce::String& error) {
    const juce::ScopedLock guard(lock);
    error.clear();
    if (dragMode != kDragNone) {
        error = "Finish or cancel the current drag first.";
        return false;
    }

    const auto selection = timeSelection;
    const auto independent = selectedTimelineTracks();
    if (!selection.hasRange()) {
        error = "Select a time range first.";
        return false;
    }

    int first = -1;
    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            const bool moved = independent.empty()
                                   ? carta::splitRangeToNewTracks(candidate, selection, first)
                                   : carta::splitIndependentRangeToNewTracks(candidate, selection,
                                                                             independent, first);
            return moved && carta::preservesMarkerBounds(session, candidate, error);
        },
        [&] {
            selectedTrack = first;
            selectedClip = -1;
            firstTrack = first;
            timeSelection.firstTrack = first;
            timeSelection.lastTrack = static_cast<int>(session.tracks.size()) - 1;
            timelineTracks.clear();
            timelineClipSelection.clear();
            keyboardRange.active = false;
            keyboardTracks.active = false;
        },
        "Move selection to new tracks", error);
    if (!changed && error.isEmpty()) {
        error = "No audio in the selected range.";
    }
    repaint();
    return changed;
}

bool Studio::splitTracksAtPlayhead(bool all, juce::String& error) {
    const juce::ScopedLock guard(lock);
    error.clear();
    if (dragMode != kDragNone) {
        error = "Finish or cancel the current drag first.";
        return false;
    }

    const auto generation = editGeneration;
    const auto independent = all ? std::vector<int>{} : selectedTimelineTracks();
    const auto scope = all ? carta::TrackRange{0, static_cast<int>(session.tracks.size()) - 1}
                       : timeSelection.hasRange()
                           ? carta::TrackRange{timeSelection.firstTrack, timeSelection.lastTrack}
                           : carta::TrackRange{selectedTrack, selectedTrack};
    const double frame = editPlayhead() * carta::sessionRate;
    if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
        error = "Invalid playhead position.";
        return false;
    }

    const auto sample = static_cast<juce::int64>(std::llround(frame));
    const int focus = selectedTrack;
    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            bool didSplit = false;
            if (independent.empty()) {
                didSplit = carta::splitTracksAtSample(candidate, scope, sample);
            } else {
                for (int track : independent) {
                    didSplit =
                        carta::splitTracksAtSample(candidate, {track, track}, sample) || didSplit;
                }
            }
            return didSplit && carta::preservesMarkerBounds(session, candidate, error);
        },
        [&] {
            if (independent.empty() ? (focus >= scope.first && focus <= scope.last)
                                    : (std::find(independent.begin(), independent.end(), focus) !=
                                       independent.end())) {
                selectedClip = -1;
            }
            if (timelineTrackRevision == generation) {
                timelineTrackRevision = generation + 1;
            }
        },
        all ? "Split all tracks at playhead" : "Split selected tracks at playhead", error);
    if (!changed && error.isEmpty()) {
        error = "No clips cross the playhead on those tracks.";
    }
    repaint();
    return changed;
}

void Studio::splitFocusedAudio() {
    const juce::ScopedLock guard(lock);
    if (dragMode != kDragNone) {
        status.setText("Finish or cancel the current drag first.", juce::dontSendNotification);
        return;
    }

    const auto selection = timeSelection;
    const int track = selectedTrack;
    const int clip = selectedClip;
    const double frame = editPlayhead() * carta::sessionRate;
    juce::int64 sample = 0;
    if (!selection.hasRange() &&
        !(clip < 0 && selection.hasPoint() &&
          selection.firstTrack < static_cast<int>(session.tracks.size()))) {
        if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
            status.setText("Invalid playhead position.", juce::dontSendNotification);
            return;
        }
        sample = static_cast<juce::int64>(std::llround(frame));
    }

    juce::String error;
    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            const bool didSplit =
                selection.hasRange() ? carta::splitRange(candidate, selection)
                : clip < 0 && selection.hasPoint() &&
                        selection.firstTrack < static_cast<int>(candidate.tracks.size())
                    ? carta::splitTracksAtSample(
                          candidate, {selection.firstTrack, selection.firstTrack}, selection.start)
                    : carta::splitClipAtSample(candidate, track, clip, sample);
            return didSplit && carta::preservesMarkerBounds(session, candidate, error);
        },
        [&] {
            if (selection.hasRange() && track >= selection.firstTrack &&
                track <= selection.lastTrack) {
                selectedClip = -1;
            }
        },
        "Split audio", error);
    if (!changed && error.isNotEmpty()) {
        status.setText(error, juce::dontSendNotification);
    }
    repaint();
}

bool Studio::validSelection() const {
    return selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size()) &&
           selectedClip >= 0 &&
           selectedClip <
               static_cast<int>(session.tracks[static_cast<size_t>(selectedTrack)].clips.size());
}

void Studio::revealTrack(int track) {
    if (track < firstTrack) {
        firstTrack = track;
    }
    while (firstTrack < track && trackArea(track).getBottom() > timeline().getBottom()) {
        ++firstTrack;
    }
}

void Studio::announceNavigation(const juce::String& message) {
    const auto expected = status.getText();
    const auto serial = ++announcementSerial;
    juce::Timer::callAfterDelay(
        150, [safe = juce::Component::SafePointer<Studio>(this), message, expected, serial] {
            if (safe != nullptr && safe->announcementSerial == serial &&
                safe->status.getText() == expected) {
                juce::AccessibilityHandler::postAnnouncement(
                    message, juce::AccessibilityHandler::AnnouncementPriority::low);
            }
        });
}

bool Studio::ensureFocusedPersistentGroup() {
    // Focus and clip selection belong to the message thread. Playback only
    // reads the committed session, so group expansion does not need its lock.
    if (dragMode != kDragNone || !validSelection() ||
        session.tracks[static_cast<size_t>(selectedTrack)]
            .clips[static_cast<size_t>(selectedClip)]
            .groupId.isEmpty()) {
        return false;
    }

    const auto revision = editGeneration;
    const int track = selectedTrack;
    const int clip = selectedClip;
    const auto current = timelineClipSelection.get(revision);
    if (!current.empty() && !timelineClipSelection.contains(track, clip, revision)) {
        return false;
    }
    const std::vector<carta::ClipReference> requested =
        current.empty() ? std::vector<carta::ClipReference>{{track, clip}} : current;
#if CARTA_STUDIO_COMPONENT_TEST
    if (focusedGroupExpansionHook) {
        focusedGroupExpansionHook();
    }
#endif
    carta::ClipSelection expanded;
    const bool valid = expanded.selectExpanded(session, requested, revision);
    const auto& now = timelineClipSelection.get(editGeneration);
    if (revision != editGeneration || dragMode != kDragNone || selectedTrack != track ||
        selectedClip != clip || current.size() != now.size() ||
        !std::equal(current.begin(), current.end(), now.begin(), [](const auto& a, const auto& b) {
            return a.track == b.track && a.clip == b.clip;
        })) {
        status.setText("The clip selection changed; try again.", juce::dontSendNotification);
        return false;
    }
    if (!valid) {
        status.setText("Could not select the clip group.", juce::dontSendNotification);
        return false;
    }
    timelineClipSelection = std::move(expanded);
    menuItemsChanged();
    repaint();
    return true;
}

bool Studio::setPersistentClipGrouping(bool group) {
    juce::String error;
    if (dragMode != kDragNone) {
        status.setText("Finish the current drag first.", juce::dontSendNotification);
        return false;
    }

    auto requested = timelineClipSelection.get(editGeneration);
    if (requested.empty() && validSelection()) {
        requested.push_back({selectedTrack, selectedClip});
    }
    std::vector<carta::ClipReference> members;
    if (!carta::expandClipGroups(session, requested, members, error)) {
        status.setText(error, juce::dontSendNotification);
        return false;
    }

    const bool changed = mutateMixState(
        [&](carta::Session& candidate) {
            return group ? carta::groupClips(candidate, members, error)
                         : carta::ungroupClips(candidate, members, error);
        },
        error, group ? "Group clips" : "Ungroup clips");
    if (!changed) {
        status.setText(error.isNotEmpty() ? error : "Clip groups unchanged.",
                       juce::dontSendNotification);
        return false;
    }

    if (!timelineClipSelection.restore(session, members, editGeneration)) {
        timelineClipSelection.clear();
    }
    const auto message = juce::String(group ? "Grouped " : "Ungrouped ") +
                         juce::String(members.size()) +
                         (members.size() == 1 ? " clip." : " clips.");
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
    return true;
}

void Studio::toggleFocusedClipSelection() {
    const juce::ScopedLock guard(lock);
    if (dragMode != kDragNone || !validSelection()) {
        status.setText("Focus a clip before changing the clip selection.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        return;
    }
    if (!timelineClipSelection.toggleGroup(session, {selectedTrack, selectedClip},
                                           editGeneration)) {
        status.setText("Could not select the clip group.", juce::dontSendNotification);
        return;
    }

    const bool nowSelected =
        timelineClipSelection.contains(selectedTrack, selectedClip, editGeneration);
    const auto description = carta::describeClipSelection(timelineClipSelection, editGeneration,
                                                          selectedTrack, selectedClip);
    const auto name = carta::clipDisplayName(session.tracks[static_cast<size_t>(selectedTrack)]
                                                 .clips[static_cast<size_t>(selectedClip)]);
    const auto message = (nowSelected ? "Added " : "Removed ") + name +
                         (nowSelected ? " to clip selection. " : " from clip selection. ") +
                         description;
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
}

bool Studio::selectSourceClips(int scope) {
    const juce::ScopedLock guard(lock);
    if (dragMode != kDragNone || !validSelection()) {
        status.setText("Focus a clip before selecting clips using its source.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        return false;
    }
    if (scope == 2 && !timeSelection.hasRange()) {
        status.setText("Select a time range first; clip selection unchanged.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        return false;
    }

    const auto source = session.tracks[static_cast<size_t>(selectedTrack)]
                            .clips[static_cast<size_t>(selectedClip)]
                            .audio;
    if (!source) {
        status.setText("The focused clip has no source.", juce::dontSendNotification);
        return false;
    }

    const auto revision = editGeneration;
    const int focusTrack = selectedTrack;
    const int focusClip = selectedClip;
    const auto selection = timeSelection;
    const auto independent = selectedTimelineTracks();
    const auto previous = timelineClipSelection.get(revision);
    std::vector<int> rows;
    if (scope != 0 && !independent.empty()) {
        rows = independent;
    } else if (scope != 0 && selection.hasRange()) {
        for (int track = selection.firstTrack; track <= selection.lastTrack; ++track) {
            rows.push_back(track);
        }
    } else if (scope != 0) {
        rows.push_back(focusTrack);
    } else {
        for (int track = 0; track < static_cast<int>(session.tracks.size()); ++track) {
            rows.push_back(track);
        }
    }

    const auto unchanged = [&] {
        return editGeneration == revision && selectedTrack == focusTrack &&
               selectedClip == focusClip && timeSelection.start == selection.start &&
               timeSelection.end == selection.end &&
               timeSelection.firstTrack == selection.firstTrack &&
               timeSelection.lastTrack == selection.lastTrack &&
               selectedTimelineTracks() == independent &&
               sameClipReferences(timelineClipSelection.get(revision), previous);
    };

    carta::ClipSelection chosen;
    bool success = false;
    bool stale = false;
    {
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (scopeSelectionHook) {
            scopeSelectionHook();
        }
#endif
        stale = !unchanged();
        if (!stale) {
            success = chosen.selectSourceOnTracks(session, source, rows,
                                                  scope == 2 ? &selection : nullptr, revision);
        }
    }
    if (stale || !unchanged()) {
        status.setText("The project or selection changed; source clip selection was not applied.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        return false;
    }
    if (!success) {
        status.setText("No matching clips in that scope; clip selection unchanged.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        return false;
    }

    timelineClipSelection = std::move(chosen);
    const auto count = timelineClipSelection.get(editGeneration).size();
    const auto message = juce::String(count) + (count == 1 ? " clip selected" : " clips selected") +
                         " (including grouped clips). Time selection and playhead unchanged.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
    return true;
}

bool Studio::selectClipsInScope(int command) {
    const juce::ScopedLock guard(lock);
    if (dragMode != kDragNone) {
        status.setText("Finish the current drag before selecting clips.",
                       juce::dontSendNotification);
        return false;
    }

    carta::TrackRange tracks;
    if (command == ClipScopeCommand::allLayers) {
        tracks = {0, static_cast<int>(session.tracks.size()) - 1};
    } else if (timeSelection.hasRange()) {
        tracks = {timeSelection.firstTrack, timeSelection.lastTrack};
    } else {
        tracks = {selectedTrack, selectedTrack};
    }

    const auto revision = editGeneration;
    const int focusTrack = selectedTrack;
    const int focusClip = selectedClip;
    const auto selection = timeSelection;
    const auto* range = command == ClipScopeCommand::inTimeSelection ? &selection : nullptr;
    const auto selectedRows = selectedTimelineTracks();
    const auto independent =
        command == ClipScopeCommand::allLayers ? std::vector<int>{} : selectedRows;
    const auto previous = timelineClipSelection.get(revision);

    const auto unchanged = [&] {
        return editGeneration == revision && selectedTrack == focusTrack &&
               selectedClip == focusClip && timeSelection.start == selection.start &&
               timeSelection.end == selection.end &&
               timeSelection.firstTrack == selection.firstTrack &&
               timeSelection.lastTrack == selection.lastTrack &&
               selectedTimelineTracks() == selectedRows &&
               sameClipReferences(timelineClipSelection.get(revision), previous);
    };

    carta::ClipSelection chosen;
    bool found = false;
    bool stale = false;
    {
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (scopeSelectionHook) {
            scopeSelectionHook();
        }
#endif
        stale = !unchanged();
        if (!stale) {
            found = independent.empty()
                        ? chosen.selectTracks(session, tracks, range, revision)
                        : chosen.selectTrackIndices(session, independent, range, revision);
        }
    }
    if (stale || !unchanged()) {
        status.setText("The project or selection changed; clip selection was not applied.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        return false;
    }
    if (!found) {
        status.setText("No clips found in that scope; clip selection unchanged.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        return false;
    }

    timelineClipSelection = std::move(chosen);
    const auto& clips = timelineClipSelection.get(editGeneration);
    if (!timelineClipSelection.contains(selectedTrack, selectedClip, editGeneration)) {
        selectedTrack = clips.front().track;
        selectedClip = clips.front().clip;
        revealTrack(selectedTrack);
    }
    const auto message = juce::String(clips.size()) +
                         (clips.size() == 1 ? " clip selected. " : " clips selected. ") +
                         "Time selection unchanged.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
    return true;
}

void Studio::focusTrackWithoutSeeking(int track) {
    selectedTrack = track;
    selectedClip = -1;
    selectedMarker = -1;
    keyboardRange.active = false;
    keyboardTracks.active = false;
    revealTrack(track);
    const auto message = "Focused track " + juce::String(track + 1) + ": " +
                         session.tracks[static_cast<size_t>(track)].name +
                         ". Time and clip selections unchanged.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(
        carta::describeNavigationTarget(session, track, -1, &sourceChecks) + " " +
        carta::describeClipSelection(timelineClipSelection, editGeneration, track, -1));
    menuItemsChanged();
    repaint();
}

void Studio::focusAdjacentTrack(bool next) {
    const int track = selectedTrack;
    const auto revision = editGeneration;
#if CARTA_STUDIO_COMPONENT_TEST
    if (focusNavigationHook) {
        focusNavigationHook();
    }
#endif
    if (revision != editGeneration || track != selectedTrack) {
        status.setText("The project or track focus changed; navigation was not applied.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        return;
    }
    const int count = static_cast<int>(session.tracks.size());
    if (count == 0) {
        status.setText("There are no tracks.", juce::dontSendNotification);
        announceNavigation(status.getText());
        return;
    }
    const int target =
        track < 0 || track >= count ? (next ? 0 : count - 1) : track + (next ? 1 : -1);
    if (target < 0 || target >= count) {
        status.setText(next ? "No next track." : "No previous track.", juce::dontSendNotification);
        announceNavigation(status.getText());
        return;
    }
    focusTrackWithoutSeeking(target);
}

void Studio::selectAdjacentTrack(bool next) {
    {
        const juce::ScopedLock guard(lock);
        stopSpecialAudition();
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (focusNavigationHook) {
        focusNavigationHook();
    }
#endif
    const int count = static_cast<int>(session.tracks.size());
    if (count == 0) {
        status.setText("There are no tracks.", juce::dontSendNotification);
        announceNavigation(status.getText());
        return;
    }
    const int target = selectedTrack < 0 || selectedTrack >= count
                           ? (next ? 0 : count - 1)
                           : selectedTrack + (next ? 1 : -1);
    if (target < 0 || target >= count) {
        status.setText(next ? "No next track." : "No previous track.", juce::dontSendNotification);
        announceNavigation(status.getText());
        return;
    }

    selectedTrack = target;
    selectedClip = -1;
    selectedMarker = -1;
    revealTrack(target);
#if CARTA_STUDIO_COMPONENT_TEST
    if (focusDescriptionHook) {
        focusDescriptionHook();
    }
#endif
    status.setText("Track " + juce::String(target + 1) + " of " + juce::String(count) + ": " +
                       session.tracks[static_cast<size_t>(target)].name + ".",
                   juce::dontSendNotification);
    announceNavigation(
        carta::describeNavigationTarget(session, target, -1, &sourceChecks) + " " +
        carta::describeClipSelection(timelineClipSelection, editGeneration, target, -1));
    repaint();
}

void Studio::focusClipWithoutSeeking(int track, int clipIndex) {
    selectedTrack = track;
    selectedClip = clipIndex;
    selectedMarker = -1;
    keyboardRange.active = false;
    keyboardTracks.active = false;
    const auto& clip =
        session.tracks[static_cast<size_t>(track)].clips[static_cast<size_t>(clipIndex)];
    juce::int64 start = 0;
    juce::int64 end = 0;
    const double clipPosition = carta::clipSampleRange(clip, start, end)
                                    ? static_cast<double>(start) / carta::sessionRate
                                    : clip.start;
    revealTrack(track);
    if (std::isfinite(clipPosition) &&
        (clipPosition < viewStart || clipPosition >= viewStart + viewLength())) {
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()),
                                 clipPosition - viewLength() * 0.5);
        suspendFollowingForNavigation();
    }
    const auto message = "Focused " + carta::clipDisplayName(clip) + " on track " +
                         juce::String(track + 1) + ". Time and clip selections unchanged.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(
        carta::describeNavigationTarget(session, track, clipIndex, &sourceChecks) + " " +
        carta::describeClipSelection(timelineClipSelection, editGeneration, track, clipIndex));
    menuItemsChanged();
    repaint();
}

void Studio::focusAdjacentClip(bool next) {
    const int track = selectedTrack;
    const int clip = selectedClip;
    const auto revision = editGeneration;
    if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
        status.setText("Select a track first.", juce::dontSendNotification);
        announceNavigation(status.getText());
        return;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (focusNavigationHook) {
        focusNavigationHook();
    }
#endif
    if (revision != editGeneration || track != selectedTrack || clip != selectedClip) {
        status.setText("The project or clip focus changed; navigation was not applied.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        return;
    }
    int target = -1;
    if (!carta::adjacentClipIndex(session.tracks[static_cast<size_t>(track)], clip, next, target)) {
        status.setText(next ? "No next clip on this track." : "No previous clip on this track.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        return;
    }
    focusClipWithoutSeeking(track, target);
}

void Studio::selectAdjacentClip(bool next) {
    bool wasLive = false;
    {
        const juce::ScopedLock guard(lock);
        wasLive = selectionPlaybackContinues();
    }
    if (selectedTrack < 0 || selectedTrack >= static_cast<int>(session.tracks.size())) {
        status.setText("Select a track first.", juce::dontSendNotification);
        announceNavigation(status.getText());
        return;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (focusNavigationHook) {
        focusNavigationHook();
    }
#endif
    const auto& track = session.tracks[static_cast<size_t>(selectedTrack)];
    int target = -1;
    if (!carta::adjacentClipIndex(track, selectedClip, next, target)) {
        status.setText(next ? "No next clip on this track." : "No previous clip on this track.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        return;
    }

    const bool grouped = !timelineClipSelection.get(editGeneration).empty();
    const auto& clip = track.clips[static_cast<size_t>(target)];
    juce::int64 start = 0;
    juce::int64 end = 0;
    const double clipPosition = carta::clipSampleRange(clip, start, end)
                                    ? static_cast<double>(start) / carta::sessionRate
                                    : clip.start;
    {
        const juce::ScopedLock guard(lock);
        const bool keepPlayback = wasLive || selectionPlaybackContinues();
        stopSpecialAudition();
        if (!keepPlayback) {
            position = clipPosition;
            playing = false;
            auditionActive = false;
        }
    }
    const bool cleared = timeSelection.hasRange() && !grouped;
    selectedClip = target;
    selectedMarker = -1;
    if (cleared) {
        timeSelection = {};
    }
    revealTrack(selectedTrack);
    if (clipPosition < viewStart || clipPosition >= viewStart + viewLength()) {
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()),
                                 clipPosition - viewLength() * 0.5);
        suspendFollowingForNavigation();
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (focusDescriptionHook) {
        focusDescriptionHook();
    }
#endif
    status.setText(
        (grouped ? "Focused " : "Selected ") + carta::clipDisplayName(clip) + " on track " +
            juce::String(selectedTrack + 1) + (cleared ? ". Time selection cleared." : ".") +
            (grouped ? " " + carta::describeClipSelection(timelineClipSelection, editGeneration,
                                                          selectedTrack, target)
                     : ""),
        juce::dontSendNotification);
    announceNavigation(
        carta::describeNavigationTarget(session, selectedTrack, target, &sourceChecks) +
        (cleared ? " Time selection cleared." : "") + " " +
        carta::describeClipSelection(timelineClipSelection, editGeneration, selectedTrack, target));
    repaint();
}

// ============================================================================
// Zoom, Spectrogram, Layer Height & Clip Group Arrangement
// ============================================================================

double Studio::fullLength() const {
    return std::max(10.0, session.length() * 1.1);
}

double Studio::viewLength() const {
    const bool frozenDragSpan = dragMode == kDragClipMove || dragMode == kDragClipTrimLeft ||
                                dragMode == kDragClipTrimRight || dragMode == kDragGroupEdge ||
                                draggingMasterEnd;
    return frozenDragSpan ? dragSpan : fullLength() / zoom;
}

void Studio::setZoomView(double span, double start) {
    if (dragMode != kDragNone || !std::isfinite(span) || !std::isfinite(start)) {
        return;
    }
    const double full = fullLength();
    span = std::clamp(span, std::min(full, 16.0 / carta::sessionRate), full);
    start = std::clamp(start, 0.0, std::max(0.0, full - span));
    const double oldSpan = viewLength();
    if (std::abs(span - oldSpan) < 1e-12 && std::abs(start - viewStart) < 1e-12) {
        return;
    }
    suspendFollowingForNavigation();
    zoomMemory.remember({viewStart, oldSpan});
    zoom = full / span;
    viewStart = start;
    menuItemsChanged();
    repaint();
}

void Studio::zoomAround(double factor, double anchor, double ratio) {
    const double nextZoom =
        juce::jlimit(1.0, std::max(1.0, fullLength() / (16.0 / carta::sessionRate)), zoom * factor);
    const double span = fullLength() / nextZoom;
    setZoomView(span, anchor - ratio * span);
}

void Studio::zoomAt(double factor) {
    double anchor = 0;
    {
        const juce::ScopedLock guard(lock);
        anchor = position;
    }
    zoomAround(factor, anchor, 0.5);
}

void Studio::zoomToArrangement() {
    setZoomView(fullLength(), 0);
}

bool Studio::zoomToMasterTrack(int index, juce::String& error) {
    if (dragMode != kDragNone) {
        error = "Finish the current drag before changing zoom.";
        return false;
    }
    std::vector<carta::MasterTrack> tracks;
    if (!carta::getMasterTracks(session, tracks, error)) {
        return false;
    }
    if (index < 0 || index >= static_cast<int>(tracks.size())) {
        error = "Invalid master track index.";
        return false;
    }
    const auto& track = tracks[static_cast<size_t>(index)];
    const double start = static_cast<double>(track.range.start) / carta::sessionRate;
    const double end = static_cast<double>(track.range.end) / carta::sessionRate;
    const double span = std::clamp((end - start) * 1.1, 16.0 / carta::sessionRate, fullLength());
    setZoomView(span, (start + end - span) * 0.5);

    const auto message =
        "Zoomed to master track " + juce::String(index + 1) + ": " + track.name + ".";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    return true;
}

void Studio::zoomToMasterTrackAtPlayhead() {
    double sample = 0;
    {
        const juce::ScopedLock guard(lock);
        sample = position * carta::sessionRate;
    }
    juce::String error;
    carta::MasterTrack track;
    int index = 0;
    if (!std::isfinite(sample) || sample < 0 || sample >= 9007199254740992.0 ||
        !carta::masterTrackAtSample(session, static_cast<juce::int64>(std::floor(sample + 1.0e-7)),
                                    track, index, error) ||
        !zoomToMasterTrack(index, error)) {
        status.setText(error.isNotEmpty() ? error : "Place the playhead within the master.",
                       juce::dontSendNotification);
    }
}

void Studio::togglePreviousZoom() {
    if (dragMode != kDragNone) {
        status.setText("Finish the current drag before changing zoom.", juce::dontSendNotification);
        return;
    }
    carta::TimelineViewport view{viewStart, viewLength()};
    if (!zoomMemory.toggle(view, fullLength())) {
        status.setText("No previous zoom view.", juce::dontSendNotification);
        return;
    }
    suspendFollowingForNavigation();
    zoom = fullLength() / view.span;
    viewStart = view.start;
    const auto message = "Restored previous zoom view.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    repaint();
}

void Studio::zoomToSelected() {
    if (!validSelection()) {
        return;
    }
    const auto& clip =
        session.tracks[static_cast<size_t>(selectedTrack)].clips[static_cast<size_t>(selectedClip)];
    const double span = juce::jlimit(16.0 / carta::sessionRate, fullLength(), clip.duration * 1.1);
    setZoomView(span, clip.start + clip.duration * 0.5 - span * 0.5);
}

void Studio::zoomToSelectedClips() {
    const auto& clips = timelineClipSelection.get(editGeneration);
    if (clips.empty() || dragMode != kDragNone) {
        status.setText(clips.empty() ? "Select clips first."
                                     : "Finish the current drag before changing zoom.",
                       juce::dontSendNotification);
        return;
    }

    double first = std::numeric_limits<double>::infinity();
    double last = 0;
    int firstRow = std::numeric_limits<int>::max();
    int lastRow = -1;
    for (const auto& ref : clips) {
        if (ref.track < 0 || ref.track >= static_cast<int>(session.tracks.size()) || ref.clip < 0 ||
            ref.clip >=
                static_cast<int>(session.tracks[static_cast<size_t>(ref.track)].clips.size())) {
            status.setText("The clip selection changed; select clips again.",
                           juce::dontSendNotification);
            return;
        }
        const auto& clip =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        if (!std::isfinite(clip.start) || !std::isfinite(clip.duration) || clip.start < 0 ||
            clip.duration <= 0 || !std::isfinite(clip.start + clip.duration)) {
            status.setText("A selected clip has an invalid time range.",
                           juce::dontSendNotification);
            return;
        }
        first = std::min(first, clip.start);
        last = std::max(last, clip.start + clip.duration);
        firstRow = std::min(firstRow, ref.track);
        lastRow = std::max(lastRow, ref.track);
    }

    const double span = juce::jlimit(16.0 / carta::sessionRate, fullLength(), (last - first) * 1.1);
    setZoomView(span, first + (last - first) * 0.5 - span * 0.5);

    juce::int64 rowsHeight = 0;
    for (int row = firstRow; row <= lastRow; ++row) {
        rowsHeight += carta::trackDisplayHeight(session.tracks[static_cast<size_t>(row)]);
    }
    const bool rowsFit = rowsHeight <= std::max(0, timeline().getHeight() - timelineHeaderHeight());
    if (rowsFit) {
        firstTrack = firstRow;
    } else {
        const int focus =
            timelineClipSelection.contains(selectedTrack, selectedClip, editGeneration)
                ? selectedTrack
                : firstRow;
        revealTrack(focus);
    }

    const auto message =
        juce::String("Zoomed to ") + juce::String(clips.size()) +
        (clips.size() == 1 ? " selected clip." : " selected clips.") +
        (rowsFit ? "" : " Selected tracks do not all fit vertically; scroll to see the others.");
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    repaint();
}

void Studio::zoomToTimeSelection() {
    if (!timeSelection.hasRange()) {
        return;
    }
    const double start = static_cast<double>(timeSelection.start) / carta::sessionRate;
    const double end = static_cast<double>(timeSelection.end) / carta::sessionRate;
    const double span = juce::jlimit(16.0 / carta::sessionRate, fullLength(), (end - start) * 1.1);
    setZoomView(span, (start + end - span) * 0.5);
}

bool Studio::parseInteger(const juce::String& text, juce::int64& value) {
    const auto input = text.trim().toStdString();
    if (input.empty()) {
        return false;
    }
    const auto* begin = input.data();
    const auto* end = begin + input.size();
    if (*begin == '+') {
        ++begin;
        if (begin == end || *begin == '-') {
            return false;
        }
    }
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

void Studio::setSpectrumFftOrder(int order) {
    if (order < 10 || order > 13 || order == spectrumFftOrder) {
        return;
    }
    spectrumFftOrder = order;
    cancelPendingSpectrum();
    status.setText("Spectrogram FFT size: " + juce::String(1 << order) + " samples.",
                   juce::dontSendNotification);
    menuItemsChanged();
    repaint();
}

void Studio::setSpectrumFloor(int floor) {
    if (floor < -120 || floor > -40 || floor % 20 != 0 || floor == spectrumFloorDb) {
        return;
    }
    spectrumFloorDb = floor;
    cancelPendingSpectrum();
    status.setText("Spectrogram level range: " + juce::String(floor) +
                       " to 0 dB. Source audio is unchanged.",
                   juce::dontSendNotification);
    menuItemsChanged();
    repaint();
}

void Studio::setSpectrumWindow(carta::SpectrumWindow window) {
    if (window == spectrumWindow) {
        return;
    }
    spectrumWindow = window;
    cancelPendingSpectrum();
    status.setText("Spectrogram window: " +
                       juce::String(window == carta::SpectrumWindow::hann      ? "Hann"
                                    : window == carta::SpectrumWindow::hamming ? "Hamming"
                                                                               : "Blackman") +
                       ". Source audio is unchanged.",
                   juce::dontSendNotification);
    menuItemsChanged();
    repaint();
}

void Studio::setSpectrumPreset(int preset) {
    if (preset < 0 || preset > 3) {
        return;
    }
    spectrumBand = static_cast<carta::SpectrumBand>(preset);
    cancelPendingSpectrum();
    status.setText("Spectrogram frequency range: " +
                       juce::String(preset == 0   ? "full"
                                    : preset == 1 ? "bass"
                                    : preset == 2 ? "voice"
                                                  : "treble") +
                       " (limited by source Nyquist).",
                   juce::dontSendNotification);
    menuItemsChanged();
    repaint();
}

bool Studio::setCustomSpectrumRange(int low, int high) {
    if (low < 1 || high <= low || high > 192000) {
        return false;
    }
    if (spectrumBand == carta::SpectrumBand::custom && spectrumCustomLow == low &&
        spectrumCustomHigh == high) {
        return true;
    }
    spectrumCustomLow = low;
    spectrumCustomHigh = high;
    spectrumBand = carta::SpectrumBand::custom;
    cancelPendingSpectrum();
    status.setText("Spectrogram frequency range: " + juce::String(low) + utf8Str("–") +
                       juce::String(high) + " Hz. Frequencies above source Nyquist are blank.",
                   juce::dontSendNotification);
    menuItemsChanged();
    repaint();
    return true;
}

void Studio::editCustomSpectrumRange() {
    const auto previousBand = spectrumBand;
    const int previousLow = spectrumCustomLow;
    const int previousHigh = spectrumCustomHigh;
    showTextFieldsPrompt(
        "Spectrogram frequency range",
        utf8Str("Enter integer frequencies in Hz (1–192000), with the lower limit below the "
                "upper limit. Frequencies above a source's Nyquist are blank."),
        {{"low", "Low Hz", juce::String(previousLow)},
         {"high", "High Hz", juce::String(previousHigh)}},
        "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), previousBand, previousLow,
         previousHigh](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted || values.size() != 2) {
                return;
            }
            if (safe->spectrumBand != previousBand || safe->spectrumCustomLow != previousLow ||
                safe->spectrumCustomHigh != previousHigh) {
                safe->status.setText("Spectrogram settings changed; reopen the editor.",
                                     juce::dontSendNotification);
                return;
            }
            juce::int64 low = 0;
            juce::int64 high = 0;
            if (!parseInteger(values[0], low) || !parseInteger(values[1], high) || low < 1 ||
                high <= low || high > 192000 ||
                !safe->setCustomSpectrumRange(static_cast<int>(low), static_cast<int>(high))) {
                safe->status.setText(
                    utf8Str("Enter integer frequencies with 1 ≤ low < high ≤ 192000 Hz."),
                    juce::dontSendNotification);
            }
        });
}

bool Studio::setChannelHeights(int index, juce::uint64 revision, const juce::String& text,
                               juce::String& error) {
    error.clear();
    if (dragMode != kDragNone || editGeneration != revision || index < 0 ||
        index >= static_cast<int>(session.tracks.size())) {
        error = "Project changed; choose the track again.";
        return false;
    }

    const auto input = text.trim();
    std::vector<int> weights;
    if (!input.equalsIgnoreCase("equal")) {
        if (input.length() > 8192) {
            error = "Channel height list is too long.";
            return false;
        }
        const auto bytes = input.toStdString();
        size_t start = 0;
        while (start <= bytes.size()) {
            const auto end = bytes.find(',', start);
            const auto part =
                bytes.substr(start, end == std::string::npos ? std::string::npos : end - start);
            juce::int64 value = 0;
            if (!parseInteger(juce::String::fromUTF8(part.c_str()), value) || value < 1 ||
                value > 1000 || weights.size() >= 1024) {
                error = "Enter comma-separated integer weights from 1 to 1000, or equal.";
                return false;
            }
            weights.push_back(static_cast<int>(value));
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        const auto count = carta::maxTrackChannels(session.tracks[static_cast<size_t>(index)]);
        if (count < 2 || static_cast<int>(weights.size()) != count) {
            error = "Enter one weight for each of the track's " + juce::String(count) +
                    " source channels.";
            return false;
        }
    }

    auto& track = session.tracks[static_cast<size_t>(index)];
    if (track.channelHeightWeights == weights) {
        return true;
    }
    if (!mutateMixState(
            [&](carta::Session& candidate) {
                candidate.tracks[static_cast<size_t>(index)].channelHeightWeights = weights;
                return true;
            },
            error, "Change channel heights")) {
        if (error.isEmpty()) {
            error = "Could not change channel heights.";
        }
        return false;
    }
    status.setText(weights.empty() ? "Channel heights are equal." : "Channel heights updated.",
                   juce::dontSendNotification);
    return true;
}

void Studio::editChannelHeights() {
    if (selectedTrack < 0 || selectedTrack >= static_cast<int>(session.tracks.size())) {
        return;
    }
    const int index = selectedTrack;
    const int channels = carta::maxTrackChannels(session.tracks[static_cast<size_t>(index)]);
    const auto revision = editGeneration;
    const auto& weights = session.tracks[static_cast<size_t>(index)].channelHeightWeights;
    juce::StringArray values;
    for (int weight : weights) {
        values.add(juce::String(weight));
    }
    showTextPrompt(
        "Channel heights",
        utf8Str("Enter one relative weight per source channel (1–1000), separated by commas. Use "
                "equal for equal heights. A track displaying one isolated channel uses its full "
                "height. This changes the display only. Current source channels: ") +
            juce::String(channels) + ".",
        "weights", "Relative heights", weights.empty() ? "equal" : values.joinIntoString(","),
        "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), index,
         revision](bool accepted, const juce::String& value) {
            if (safe == nullptr || !accepted) {
                return;
            }
            juce::String error;
            if (!safe->setChannelHeights(index, revision, value, error)) {
                safe->status.setText(error, juce::dontSendNotification);
            }
        });
}

void Studio::nudgeTrackHeight(int direction) {
    const int index = selectedTrack;
    if (index < 0 || index >= static_cast<int>(session.tracks.size()) ||
        (direction != -1 && direction != 1)) {
        status.setText("Focus a layer first.", juce::dontSendNotification);
        return;
    }

    juce::String error;
    bool expanded = false;
    const bool changed = mutateMixState(
        [&](carta::Session& candidate) {
            auto& track = candidate.tracks[static_cast<size_t>(index)];
            if (track.collapsed) {
                track.collapsed = false;
                expanded = true;
                return true;
            }
            const int next = juce::jlimit(72, 800, track.height + direction * 16);
            if (next == track.height) {
                return false;
            }
            track.height = next;
            return true;
        },
        error, "Change track height");
    if (!changed) {
        const auto message = error.isNotEmpty() ? error : "Layer height is already at the limit.";
        status.setText(message, juce::dontSendNotification);
        announceNavigation(message);
        return;
    }

    revealTrack(index);
    const auto message =
        (expanded ? juce::String("Expanded layer. Height ") : juce::String("Layer height ")) +
        juce::String(session.tracks[static_cast<size_t>(index)].height) + " pixels.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    repaint();
}

void Studio::editTrackHeight() {
    if (selectedTrack < 0 || selectedTrack >= static_cast<int>(session.tracks.size())) {
        return;
    }
    const int index = selectedTrack;
    const auto revision = editGeneration;
    showTextPrompt("Layer height", utf8Str("Height in pixels (72–800)."), "height", "Height",
                   juce::String(session.tracks[static_cast<size_t>(index)].height), "Apply",
                   [safe = juce::Component::SafePointer<Studio>(this), index,
                    revision](bool accepted, const juce::String& value) {
                       if (safe == nullptr || !accepted) {
                           return;
                       }
                       if (!safe->dialogTargetCurrent(revision)) {
                           safe->status.setText("Project changed; reopen the editor.",
                                                juce::dontSendNotification);
                           return;
                       }
                       juce::int64 height = 0;
                       if (!parseInteger(value, height) || height < 72 || height > 800) {
                           safe->status.setText("Enter a layer height from 72 to 800 pixels.",
                                                juce::dontSendNotification);
                           return;
                       }
                       bool exists = true;
                       juce::String error;
                       const bool changed = safe->mutateMixState(
                           [&](carta::Session& candidate) {
                               if (index < 0 ||
                                   index >= static_cast<int>(candidate.tracks.size())) {
                                   exists = false;
                                   return false;
                               }
                               auto& track = candidate.tracks[static_cast<size_t>(index)];
                               if (track.height == height && !track.collapsed) {
                                   return false;
                               }
                               track.height = static_cast<int>(height);
                               track.collapsed = false;
                               return true;
                           },
                           error, "Change track height");
                       safe->status.setText(!exists              ? "The layer no longer exists."
                                            : changed            ? "Layer height updated."
                                            : error.isNotEmpty() ? error
                                                                 : "Layer height unchanged.",
                                            juce::dontSendNotification);
                   });
}

void Studio::nudgeClipGroup(int direction) {
    const juce::ScopedLock guard(lock);
    if (dragMode != kDragNone) {
        status.setText("Finish the current drag before nudging clips.", juce::dontSendNotification);
        return;
    }
    const auto generation = editGeneration;
    const auto clips = timelineClipSelection.get(generation);
    if (clips.empty() || (direction != -1 && direction != 1)) {
        status.setText("Select clips first.", juce::dontSendNotification);
        return;
    }

    stopSpecialAudition();
    juce::String error;
    carta::Session candidate;
    carta::PlaybackRange candidateRange;
    bool prepared = false;
    {
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (groupKeyboardPreparationHook) {
            groupKeyboardPreparationHook();
        }
#endif
        prepared = carta::prepareClipGroupMove(session, candidate, clips, direction, 0, error);
        if (prepared) {
            candidateRange = carta::fullMasterPlaybackRange(candidate);
        }
    }
    if (prepared && generation != editGeneration) {
        prepared = false;
        error = "Project changed during the edit.";
    }
    if (!prepared) {
        status.setText(error, juce::dontSendNotification);
        return;
    }

    carta::EditorState before{{},
                              timeSelection,
                              selectedTrack,
                              selectedClip,
                              selectedMarker,
                              masterChannels,
                              position,
                              timelineClipSelection.get(editGeneration),
                              selectedTimelineTracks()};
    std::swap(session, candidate);
    before.session = std::move(candidate);
    pushPreparedHistory(std::move(before), "Nudge clips", candidateRange);
    // Horizontal group moves leave all clip indices and their order intact.
    timelineClipSelection.retainAfterStableEdit(generation, editGeneration);
    const auto message = juce::String("Nudged ") + juce::String(clips.size()) +
                         (clips.size() == 1 ? " clip " : " clips ") +
                         (direction < 0 ? "left" : "right") + " one sample.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    repaint();
}

bool Studio::sequenceSelectedClips(bool distribute) {
    const juce::ScopedLock guard(lock);
    if (dragMode != kDragNone) {
        status.setText("Finish the current drag before arranging clips.",
                       juce::dontSendNotification);
        return false;
    }

    const auto generation = editGeneration;
    const auto current = timelineClipSelection.get(generation);
    std::vector<carta::ClipReference> clips;
    juce::String error;
    bool expanded = false;
    {
        const juce::ScopedUnlock unlock(lock);
        expanded = carta::expandClipGroups(session, current, clips, error);
    }
    if (!expanded) {
        status.setText(error, juce::dontSendNotification);
        return false;
    }

    stopSpecialAudition();
    carta::Session candidate;
    carta::PlaybackRange candidateRange;
    bool prepared = false;
    {
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (groupKeyboardPreparationHook) {
            groupKeyboardPreparationHook();
        }
#endif
        prepared = distribute ? carta::prepareDistributeClipGroup(session, candidate, clips, error)
                              : carta::prepareSequenceClipGroup(session, candidate, clips, error);
        if (prepared) {
            candidateRange = carta::fullMasterPlaybackRange(candidate);
        }
    }
    if (prepared && generation != editGeneration) {
        prepared = false;
        error = "Project changed during the edit.";
    }
    if (!prepared) {
        status.setText(error, juce::dontSendNotification);
        announceNavigation(error);
        return false;
    }

    carta::EditorState before{{},
                              timeSelection,
                              selectedTrack,
                              selectedClip,
                              selectedMarker,
                              masterChannels,
                              position,
                              timelineClipSelection.get(editGeneration),
                              selectedTimelineTracks()};
    std::swap(session, candidate);
    before.session = std::move(candidate);
    pushPreparedHistory(std::move(before), distribute ? "Distribute clips" : "Sequence clips",
                        candidateRange);
    retainIndexStableSelections(generation);
    if (!timelineClipSelection.restore(session, clips, editGeneration)) {
        timelineClipSelection.clear();
    }
    const auto message = juce::String(distribute ? "Distributed " : "Sequenced ") +
                         juce::String(clips.size()) +
                         " clips; overlaps with unselected clips are mixed.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
    return true;
}

bool Studio::addClipBoundaryMarkers(bool ends) {
    if (dragMode != kDragNone) {
        status.setText("Finish the current drag before adding markers.",
                       juce::dontSendNotification);
        return false;
    }
    const auto generation = editGeneration;
    auto clips = timelineClipSelection.get(generation);
    if (clips.empty() && validSelection()) {
        clips.push_back({selectedTrack, selectedClip});
    }
    juce::String error;
    const auto firstMarker = session.markers.size();
    size_t added = 0;
    const bool changed = mutateMarkers(
        [&](carta::Session& candidate) {
            if (!(ends ? carta::addMarkersAtClipEnds(candidate, clips, error)
                       : carta::addMarkersAtClipStarts(candidate, clips, error))) {
                return false;
            }
            added = candidate.markers.size() - firstMarker;
            return true;
        },
        [&] { selectedMarker = static_cast<int>(firstMarker); },
        ends ? "Add clip-end markers" : "Add clip-start markers", error);
    if (!changed) {
        status.setText(error, juce::dontSendNotification);
        announceNavigation(error);
        return false;
    }
    const auto message =
        "Added " + juce::String(added) +
        (added == 1 ? " marker at selected clip " : " markers at selected clip ") +
        (ends ? (added == 1 ? "end." : "ends.") : (added == 1 ? "start." : "starts."));
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
    return true;
}

void Studio::addClipSourceCues() {
    if (dragMode != kDragNone || !validSelection()) {
        status.setText("Focus a clip before importing its WAV cues.", juce::dontSendNotification);
        return;
    }
    const auto oldCount = session.markers.size();
    const carta::ClipReference clip{selectedTrack, selectedClip};
    size_t added = 0;
    juce::String error;
    const bool changed = mutateMarkers(
        [&](carta::Session& candidate) {
            if (!carta::addMarkersFromClipCues(candidate, clip, error)) {
                return false;
            }
            added = candidate.markers.size() - oldCount;
            return true;
        },
        [&] { selectedMarker = static_cast<int>(oldCount); }, "Add WAV cue markers", error);
    if (!changed) {
        status.setText(error, juce::dontSendNotification);
        announceNavigation(error);
        return;
    }
    const auto message = "Added " + juce::String(added) +
                         (added == 1 ? " WAV cue as a marker." : " WAV cues as markers.");
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
}

void Studio::nudgeClip(int direction) {
    if (ensureFocusedPersistentGroup()) {
        nudgeClipGroup(direction);
        return;
    }
    juce::String error;
    const bool changed = mutateGeometryState(
        [&](carta::Session& candidate, double) {
            if (!validSelection() || (direction != -1 && direction != 1)) {
                error = "Select a clip first.";
                return false;
            }
            auto& clip = candidate.tracks[static_cast<size_t>(selectedTrack)]
                             .clips[static_cast<size_t>(selectedClip)];
            return carta::alignClipEdgeToTime(
                clip, false, carta::shiftTimelineBySamples(clip.start, direction), error);
        },
        error, "Nudge clip");
    status.setText(changed           ? "Moved clip by one project sample."
                   : error.isEmpty() ? "Clip was not moved."
                                     : error,
                   juce::dontSendNotification);
}

// ============================================================================
// Mix/Gain/Pan, Clip Geometry & Fade Shape Dialogs
// ============================================================================

std::vector<int> Studio::effectiveMixTracks() const {
    auto tracks = selectedTimelineTracks();
    if (!tracks.empty()) {
        return tracks;
    }
    if (timeSelection.hasRange() &&
        timeSelection.lastTrack < static_cast<int>(session.tracks.size())) {
        for (int i = timeSelection.firstTrack; i <= timeSelection.lastTrack; ++i) {
            tracks.push_back(i);
        }
    } else if (selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size())) {
        tracks.push_back(selectedTrack);
    }
    return tracks;
}

bool Studio::parseTrackGainDb(const juce::String& input, double& gain, bool limitGain) {
    if (input.trim().equalsIgnoreCase("-inf")) {
        gain = 0;
        return true;
    }
    double db = 0;
    if (!parseNumber(input, db)) {
        return false;
    }
    const double value = std::pow(10.0, db / 20.0);
    if (!std::isfinite(value) || value <= 0 || (limitGain && value > carta::maxLinearGain) ||
        (limitGain && std::equal_to<float>{}(static_cast<float>(value), 0.0f))) {
        return false;
    }
    gain = value;
    return true;
}

bool Studio::setTrackGains(juce::uint64 revision, const std::vector<int>& tracks, double value,
                           bool& changed, juce::String& error) {
    error.clear();
    changed = false;
    if (dragMode != kDragNone || editGeneration != revision) {
        error = "Project changed; select tracks again.";
        return false;
    }
    if (tracks.empty() || !std::isfinite(value) || value < 0 || value > carta::maxLinearGain) {
        error = "Select tracks and enter a gain from 0 to 1600%.";
        return false;
    }

    const float next = static_cast<float>(value);
    std::set<int> seen;
    for (const int index : tracks) {
        if (index < 0 || index >= static_cast<int>(session.tracks.size()) ||
            !seen.insert(index).second) {
            changed = false;
            error = "Invalid track selection.";
            return false;
        }
        changed = changed ||
                  !std::equal_to<float>{}(session.tracks[static_cast<size_t>(index)].gain, next);
    }
    if (!changed) {
        return true;
    }

    const bool applied = mutateMixState(
        [&](carta::Session& candidate) {
            if (editGeneration != revision) {
                error = "Project changed; select tracks again.";
                return false;
            }
            for (const int index : tracks) {
                candidate.tracks[static_cast<size_t>(index)].gain = next;
            }
            return true;
        },
        error, "Set selected track gains");
    if (!applied) {
        changed = false;
        if (error.isEmpty()) {
            error = "Could not set track gains.";
        }
    }
    return applied;
}

bool Studio::setTrackBalances(juce::uint64 revision, const std::vector<int>& tracks, double value,
                              bool& changed, juce::String& error) {
    error.clear();
    changed = false;
    if (dragMode != kDragNone || editGeneration != revision) {
        error = "Project changed; select tracks again.";
        return false;
    }
    if (tracks.empty() || !std::isfinite(value) || value < -1 || value > 1) {
        error = "Select tracks and enter a balance from -100% to 100%.";
        return false;
    }

    const float next = static_cast<float>(value);
    std::set<int> seen;
    for (const int index : tracks) {
        if (index < 0 || index >= static_cast<int>(session.tracks.size()) ||
            !seen.insert(index).second) {
            changed = false;
            error = "Invalid track selection.";
            return false;
        }
        changed = changed ||
                  !std::equal_to<float>{}(session.tracks[static_cast<size_t>(index)].pan, next);
    }
    if (!changed) {
        return true;
    }

    const bool applied = mutateMixState(
        [&](carta::Session& candidate) {
            if (editGeneration != revision) {
                error = "Project changed; select tracks again.";
                return false;
            }
            for (const int index : tracks) {
                candidate.tracks[static_cast<size_t>(index)].pan = next;
            }
            return true;
        },
        error, "Set selected track balances");
    if (!applied) {
        changed = false;
        if (error.isEmpty()) {
            error = "Could not set track balances.";
        }
    }
    return applied;
}

void Studio::setSelectedTrackBalanceDialog() {
    const auto tracks = effectiveMixTracks();
    if (tracks.empty()) {
        status.setText("Select tracks first.", juce::dontSendNotification);
        return;
    }
    const auto revision = editGeneration;
    showTextPrompt(
        "Set selected track balance",
        "Set every selected track to the same balance: -100% left, 0% center, 100% right.",
        "balance", "Balance (%)", {}, "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), tracks,
         revision](bool accepted, const juce::String& value) {
            if (safe == nullptr || !accepted) {
                return;
            }
            double percent = 0;
            bool changed = false;
            juce::String error;
            if (!parseNumber(value, percent) || percent < -100 || percent > 100) {
                error = "Enter a balance from -100% to 100%.";
            } else if (safe->setTrackBalances(revision, tracks, percent / 100.0, changed, error)) {
                safe->status.setText(changed ? "Set selected track balances."
                                             : "Selected track balances are unchanged.",
                                     juce::dontSendNotification);
                return;
            }
            safe->status.setText(error, juce::dontSendNotification);
        });
}

void Studio::setSelectedTrackGainDialog() {
    const auto tracks = effectiveMixTracks();
    if (tracks.empty()) {
        status.setText("Select tracks first.", juce::dontSendNotification);
        return;
    }
    const auto revision = editGeneration;
    showTextPrompt(
        "Set selected track gains",
        "Set every selected track to the same gain, replacing its current gain.", "gain",
        "Gain (%)", {}, "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), tracks,
         revision](bool accepted, const juce::String& value) {
            if (safe == nullptr || !accepted) {
                return;
            }
            double percent = 0;
            bool changed = false;
            juce::String error;
            if (!parseNumber(value, percent) || percent < 0 || percent > 1600) {
                error = "Enter a gain from 0 to 1600%.";
            } else if (safe->setTrackGains(revision, tracks, percent / 100.0, changed, error)) {
                safe->status.setText(changed ? "Set selected track gains."
                                             : "Selected track gains are unchanged.",
                                     juce::dontSendNotification);
                return;
            }
            safe->status.setText(error, juce::dontSendNotification);
        });
}

void Studio::setSelectedTrackGainDbDialog() {
    const auto tracks = effectiveMixTracks();
    if (tracks.empty()) {
        status.setText("Select tracks first.", juce::dontSendNotification);
        return;
    }
    const auto revision = editGeneration;
    showTextPrompt(
        "Set selected track gain in dB",
        "Set every selected track to the same gain. Enter -inf for silence; the maximum linear "
        "gain is 16 (about +24.08 dB).",
        "gainDb", "Gain (dB)", {}, "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), tracks,
         revision](bool accepted, const juce::String& value) {
            if (safe == nullptr || !accepted) {
                return;
            }
            double gain = 0;
            bool changed = false;
            juce::String error;
            if (!parseTrackGainDb(value, gain)) {
                error = "Enter -inf or a dB value producing a gain above zero and at most 16.";
            } else if (safe->setTrackGains(revision, tracks, gain, changed, error)) {
                safe->status.setText(changed ? "Set selected track gains."
                                             : "Selected track gains are unchanged.",
                                     juce::dontSendNotification);
                return;
            }
            safe->status.setText(error, juce::dontSendNotification);
        });
}

bool Studio::scaleTrackGains(juce::uint64 revision, const std::vector<int>& tracks, double factor,
                             juce::String& error) {
    error.clear();
    if (dragMode != kDragNone || editGeneration != revision) {
        error = "Project changed; select tracks again.";
        return false;
    }
    if (tracks.empty() || !std::isfinite(factor) || factor < 0) {
        error = "Select tracks and enter a nonnegative finite gain factor.";
        return false;
    }
    return mutateMixState(
        [&](carta::Session& candidate) {
            if (editGeneration != revision) {
                error = "Project changed; select tracks again.";
                return false;
            }
            std::set<int> seen;
            std::vector<float> gains;
            gains.reserve(tracks.size());
            bool changed = false;
            for (int i : tracks) {
                if (i < 0 || i >= static_cast<int>(candidate.tracks.size()) ||
                    !seen.insert(i).second) {
                    error = "Invalid track selection.";
                    return false;
                }
                const auto old = candidate.tracks[static_cast<size_t>(i)].gain;
                const double value = static_cast<double>(old) * factor;
                if (!std::isfinite(value) || value < 0 || value > carta::maxLinearGain) {
                    error =
                        "The scaled gain would exceed 1600% on at least one track; no gains were "
                        "changed.";
                    return false;
                }
                const auto next = static_cast<float>(value);
                if (old > 0 && factor > 0 && std::equal_to<float>{}(next, 0.0f)) {
                    error =
                        "The scaled gain would be too small to represent; no gains were changed.";
                    return false;
                }
                gains.push_back(next);
                changed = changed || !std::equal_to<float>{}(old, next);
            }
            if (!changed) {
                error = "Selected track gains are unchanged.";
                return false;
            }
            for (size_t i = 0; i < tracks.size(); ++i) {
                candidate.tracks[static_cast<size_t>(tracks[i])].gain = gains[i];
            }
            return true;
        },
        error, "Scale selected track gains");
}

void Studio::editSelectedTrackGain() {
    const auto tracks = effectiveMixTracks();
    if (tracks.empty()) {
        status.setText("Select tracks first.", juce::dontSendNotification);
        return;
    }
    const auto revision = editGeneration;
    showTextPrompt(
        "Scale selected track gains",
        "Multiply each selected track's gain by this percentage. The edit is rejected if any "
        "result exceeds 1600%.",
        "factor", "Gain multiplier (%)", "100", "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), tracks,
         revision](bool accepted, const juce::String& value) {
            if (safe == nullptr || !accepted) {
                return;
            }
            double percent = 0;
            juce::String error;
            if (!parseNumber(value, percent) || percent < 0 || percent > 1600) {
                error = "Enter a gain multiplier from 0 to 1600%.";
            } else if (safe->scaleTrackGains(revision, tracks, percent / 100.0, error)) {
                safe->status.setText("Scaled selected track gains.", juce::dontSendNotification);
                return;
            }
            safe->status.setText(error, juce::dontSendNotification);
        });
}

void Studio::adjustSelectedTrackGainDbDialog() {
    const auto tracks = effectiveMixTracks();
    if (tracks.empty()) {
        status.setText("Select tracks first.", juce::dontSendNotification);
        return;
    }
    const auto revision = editGeneration;
    showTextPrompt(
        "Adjust selected track gain in dB",
        "Add the same dB adjustment to each selected track, preserving their relative levels. "
        "Enter -inf for silence; no result may exceed 1600%.",
        "gainDb", "Adjustment (dB)", {}, "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), tracks,
         revision](bool accepted, const juce::String& value) {
            if (safe == nullptr || !accepted) {
                return;
            }
            double factor = 0;
            juce::String error;
            if (!parseTrackGainDb(value, factor, false)) {
                error = "Enter -inf or a finite dB adjustment.";
            } else if (safe->scaleTrackGains(revision, tracks, factor, error)) {
                safe->status.setText("Adjusted selected track gains.", juce::dontSendNotification);
                return;
            }
            safe->status.setText(error, juce::dontSendNotification);
        });
}

void Studio::setSelectedTrackFlag(int command) {
    if (!selectedTimelineTracks().empty()) {
        applyTrackMixSelection(editGeneration, selectedTimelineTracks(),
                               command - TrackFlagCommand::mute);
        return;
    }

    juce::String error;
    const bool changed = mutateMixState(
        [&](carta::Session& candidate) {
            carta::TrackRange range;
            if (timeSelection.hasRange()) {
                range = {timeSelection.firstTrack, timeSelection.lastTrack};
            } else if (selectedTrack >= 0 &&
                       selectedTrack < static_cast<int>(candidate.tracks.size())) {
                range = {selectedTrack, selectedTrack};
            } else {
                error = "Select a track or time range first.";
                return false;
            }
            if (range.first < 0 || range.last >= static_cast<int>(candidate.tracks.size())) {
                error = "The selected track range is no longer valid.";
                return false;
            }
            const auto flag = command <= TrackFlagCommand::unmute ? carta::TrackFlag::mute
                                                                  : carta::TrackFlag::solo;
            return carta::setTrackFlag(candidate, range, flag,
                                       command == TrackFlagCommand::mute ||
                                           command == TrackFlagCommand::solo);
        },
        error, "Change selected track flags");

    const auto message = command == TrackFlagCommand::mute     ? "Muted selected rows."
                         : command == TrackFlagCommand::unmute ? "Unmuted selected rows."
                         : command == TrackFlagCommand::solo
                             ? "Soloed selected rows; existing solos remain active."
                             : "Cleared solo on selected rows.";
    status.setText(error.isNotEmpty() ? error
                   : changed          ? message
                                      : "Selected rows were already in that state.",
                   juce::dontSendNotification);
}

void Studio::alignSelectedClip(int command) {
    const bool grouped = validSelection() && session.tracks[static_cast<size_t>(selectedTrack)]
                                                 .clips[static_cast<size_t>(selectedClip)]
                                                 .groupId.isNotEmpty();
    if (grouped && !ensureFocusedPersistentGroup()) {
        status.setText("Select the grouped clip before aligning it.", juce::dontSendNotification);
        return;
    }

    const auto group =
        grouped ? timelineClipSelection.get(editGeneration) : std::vector<carta::ClipReference>{};
    juce::String error;
    const bool changed = mutateGeometryState(
        [&](carta::Session& candidate, double editPosition) {
            if (!validSelection()) {
                error = "Select a clip first.";
                return false;
            }
            auto& clip = candidate.tracks[static_cast<size_t>(selectedTrack)]
                             .clips[static_cast<size_t>(selectedClip)];
            const auto align = [&](bool endEdge, double target) {
                if (grouped) {
                    return carta::alignClipGroupEdgeToTime(
                        candidate, group, {selectedTrack, selectedClip}, endEdge, target, error);
                }
                auto aligned = clip;
                if (!carta::alignClipEdgeToTime(aligned, endEdge, target, error)) {
                    return false;
                }
                if (sameClipGeometry(clip, aligned)) {
                    return false;
                }
                clip = std::move(aligned);
                return true;
            };

            if (command >= AlignClipCommand::startToMarker) {
                double target = 0;
                const bool endEdge = command == AlignClipCommand::endToMarker ||
                                     command >= AlignClipCommand::endToPrevEdge;
                if (command <= AlignClipCommand::endToMarker) {
                    if (selectedMarker < 0 ||
                        selectedMarker >= static_cast<int>(candidate.markers.size())) {
                        error = "Select a marker first.";
                        return false;
                    }
                    target = candidate.markers[static_cast<size_t>(selectedMarker)].position;
                } else if (!(grouped ? carta::adjacentClipEdgeExcluding(
                                           candidate, {selectedTrack, selectedClip}, endEdge,
                                           command == AlignClipCommand::startToNextEdge ||
                                               command == AlignClipCommand::endToNextEdge,
                                           group, target)
                                     : carta::adjacentClipEdge(
                                           candidate, {selectedTrack, selectedClip}, endEdge,
                                           command == AlignClipCommand::startToNextEdge ||
                                               command == AlignClipCommand::endToNextEdge,
                                           target))) {
                    error = (command == AlignClipCommand::startToNextEdge ||
                             command == AlignClipCommand::endToNextEdge)
                                ? "No next clip edge."
                                : "No previous clip edge.";
                    return false;
                }
                return align(endEdge, target);
            }

            juce::int64 target = 0;
            if (command <= AlignClipCommand::endToPlayhead) {
                const double frame = editPosition * carta::sessionRate;
                if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
                    error = "Invalid playhead position.";
                    return false;
                }
                target = static_cast<juce::int64>(std::llround(frame));
            } else {
                if (!timeSelection.hasRange()) {
                    error = "Select a time range first.";
                    return false;
                }
                target = (command == AlignClipCommand::startToSelStart ||
                          command == AlignClipCommand::endToSelStart)
                             ? timeSelection.start
                             : timeSelection.end;
            }
            return align(command == AlignClipCommand::endToPlayhead ||
                             command >= AlignClipCommand::endToSelStart,
                         static_cast<double>(target) / carta::sessionRate);
        },
        error, grouped ? "Align grouped clips" : "Align clip");

    status.setText(error.isNotEmpty() ? error
                   : changed          ? (grouped ? "Aligned grouped clips; overlaps are summed."
                                                 : "Aligned clip; overlaps are summed.")
                                      : "Clip is already aligned.",
                   juce::dontSendNotification);
}

void Studio::nudgeSelectedClipEdge(int command) {
    ensureFocusedPersistentGroup();
    juce::String error;
    const auto generation = editGeneration;
    const auto group = timelineClipSelection.get(generation);
    const bool changed = mutateGeometryState(
        [&](carta::Session& candidate, double) {
            const bool endEdge = command >= ClipEdgeNudgeCommand::endEarlier;
            const int delta = command % 2 == 0 ? -1 : 1;
            if (!group.empty()) {
                return carta::nudgeClipGroupEdges(candidate, group, endEdge, delta, error);
            }
            if (!validSelection()) {
                error = "Select a clip first.";
                return false;
            }
            auto& clip = candidate.tracks[static_cast<size_t>(selectedTrack)]
                             .clips[static_cast<size_t>(selectedClip)];
            return carta::nudgeClipEdge(clip, endEdge, delta, error);
        },
        error, group.empty() ? "Adjust clip edge" : "Adjust selected clip edges");
    status.setText(error.isNotEmpty() ? error
                   : changed
                       ? (group.empty() ? "Adjusted clip edge by one project sample."
                                        : "Adjusted selected clip edges by one project sample.")
                       : "Clip edge unchanged.",
                   juce::dontSendNotification);
}

bool Studio::trimSelectedEdgesToPlayhead(bool endEdge) {
    juce::String error;
    std::vector<carta::ClipReference> clips;
    if (!carta::expandClipGroups(session, timelineClipSelection.get(editGeneration), clips,
                                 error)) {
        status.setText(error, juce::dontSendNotification);
        return false;
    }

    const carta::ClipReference focus{selectedTrack, selectedClip};
    const bool changed = mutateGeometryState(
        [&](carta::Session& candidate, double editPosition) {
            const double frame = editPosition * carta::sessionRate;
            if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
                error = "Invalid playhead position.";
                return false;
            }
            return carta::trimClipUnitsToSample(candidate, clips, focus, endEdge,
                                                static_cast<juce::int64>(std::llround(frame)),
                                                error);
        },
        error, endEdge ? "Trim selected ends to playhead" : "Trim selected starts to playhead");

    if (changed && !timelineClipSelection.restore(session, clips, editGeneration)) {
        timelineClipSelection.clear();
    }
    const auto message =
        changed ? juce::String(endEdge ? "Trimmed selected clip and group end anchors to the "
                                         "playhead."
                                       : "Trimmed selected clip and group start anchors to the "
                                         "playhead.")
                : error;
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
    return changed;
}

bool Studio::slipClipAt(int track, int index, juce::uint64 generation, juce::int64 samples,
                        bool requireFocus, juce::String& error) {
    const juce::ScopedLock guard(lock);
    error.clear();
    if (dragMode != kDragNone) {
        error = "Finish or cancel the current drag first.";
        return false;
    }
    if (generation != editGeneration || track < 0 ||
        track >= static_cast<int>(session.tracks.size()) || index < 0 ||
        index >= static_cast<int>(session.tracks[static_cast<size_t>(track)].clips.size()) ||
        (requireFocus && (selectedTrack != track || selectedClip != index))) {
        error = "The clip changed; select it again.";
        return false;
    }

    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            return carta::slipClipGroup(candidate, {{track, index}}, samples, error);
        },
        [&] {
            timelineClipSelection.retainAfterStableEdit(generation, generation + 1);
            if (timelineTrackRevision == generation) {
                timelineTrackRevision = generation + 1;
            }
        },
        "Slip source audio", error);
    if (changed) {
        status.setText("Slipped source audio by " + juce::String(samples) + " samples.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        menuItemsChanged();
        repaint();
    }
    return changed;
}

void Studio::editSlipFocusedClip() {
    if (!validSelection()) {
        status.setText("Select a clip first.", juce::dontSendNotification);
        return;
    }
    const int track = selectedTrack;
    const int index = selectedClip;
    const auto generation = editGeneration;
    const bool grouped = session.tracks[static_cast<size_t>(track)]
                             .clips[static_cast<size_t>(index)]
                             .groupId.isNotEmpty();
    showTextPrompt(
        "Slip source audio",
        grouped
            ? "Enter a signed project-sample offset. The entire clip group moves within its fixed "
              "boundaries; fades stay in place."
            : "Enter a signed project-sample offset. The clip boundaries and fades stay in place.",
        "samples", "Sample offset", "0", "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), track, index,
         generation](bool accepted, const juce::String& value) {
            if (safe == nullptr || !accepted) {
                return;
            }
            juce::int64 samples = 0;
            juce::String error;
            if (!parseInteger(value, samples)) {
                safe->status.setText("Enter a signed integer sample offset.",
                                     juce::dontSendNotification);
                return;
            }
            if (!safe->slipClipAt(track, index, generation, samples, true, error)) {
                safe->status.setText(error, juce::dontSendNotification);
            }
        });
}

bool Studio::slipSelectedClips(const std::vector<carta::ClipReference>& clips,
                               juce::uint64 generation, juce::int64 samples) {
    juce::String error;
    bool changed = false;
    {
        const juce::ScopedLock guard(lock);
        const auto& current = timelineClipSelection.get(editGeneration);
        if (dragMode != kDragNone) {
            error = "Finish or cancel the current drag first.";
        } else if (generation != editGeneration || clips.empty() ||
                   clips.size() != current.size() ||
                   !std::equal(clips.begin(), clips.end(), current.begin(),
                               [](const auto& a, const auto& b) {
                                   return a.track == b.track && a.clip == b.clip;
                               })) {
            error = "The clip selection changed; reopen the editor.";
        } else {
            changed = prepareRangeSessionEdit(
                [&](carta::Session& candidate) {
                    return carta::slipClipGroup(candidate, clips, samples, error);
                },
                [&] {
                    timelineClipSelection.retainAfterStableEdit(generation, generation + 1);
                    if (timelineTrackRevision == generation) {
                        timelineTrackRevision = generation + 1;
                    }
                },
                "Slip source audio", error);
        }
    }

    const auto message =
        changed ? juce::String("Slipped source audio by ") + juce::String(samples) + " samples."
                : error;
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
    return changed;
}

void Studio::editSlipSelectedClips() {
    const auto generation = editGeneration;
    const auto clips = timelineClipSelection.get(generation);
    if (clips.empty()) {
        status.setText("Select clips first.", juce::dontSendNotification);
        return;
    }
    showTextPrompt("Slip source audio",
                   "Enter a signed project-sample offset. Clip boundaries and fades stay in place.",
                   "samples", "Sample offset", "0", "Apply",
                   [safe = juce::Component::SafePointer<Studio>(this), clips,
                    generation](bool accepted, const juce::String& value) {
                       if (safe == nullptr || !accepted) {
                           return;
                       }
                       juce::int64 samples = 0;
                       if (!parseInteger(value, samples)) {
                           safe->status.setText("Enter a signed integer sample offset.",
                                                juce::dontSendNotification);
                           return;
                       }
                       safe->slipSelectedClips(clips, generation, samples);
                   });
}

bool Studio::adjustSelectedClipEdges(const std::vector<carta::ClipReference>& clips,
                                     juce::uint64 generation, bool endEdge, juce::int64 samples) {
    juce::String error;
    bool changed = false;
    {
        const juce::ScopedLock guard(lock);
        if (dragMode != kDragNone) {
            status.setText("Finish or cancel the current drag first.", juce::dontSendNotification);
            return false;
        }
        stopSpecialAudition();
        const auto& current = timelineClipSelection.get(editGeneration);
        if (generation != editGeneration || clips.empty() || clips.size() != current.size() ||
            !std::equal(clips.begin(), clips.end(), current.begin(),
                        [](const auto& a, const auto& b) {
                            return a.track == b.track && a.clip == b.clip;
                        })) {
            error = "The clip selection changed; reopen the editor.";
        } else if (samples == 0) {
            error = "Enter a nonzero sample offset.";
        } else {
            carta::Session candidate;
            carta::PlaybackRange candidateRange;
            {
                const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
                if (groupTrimCommitPreparationHook) {
                    groupTrimCommitPreparationHook();
                }
#endif
                candidate = session;
                changed = carta::nudgeClipGroupEdges(candidate, clips, endEdge, samples, error) &&
                          carta::preservesOnDiscAudio(session, candidate, error) &&
                          carta::preservesMarkerBounds(session, candidate, error);
                if (changed) {
                    candidateRange = carta::fullMasterPlaybackRange(candidate);
                }
            }
            if (changed && generation != editGeneration) {
                changed = false;
                error = "Project changed during the edit.";
            }
            if (changed) {
                carta::EditorState before{{},
                                          timeSelection,
                                          selectedTrack,
                                          selectedClip,
                                          selectedMarker,
                                          masterChannels,
                                          position,
                                          timelineClipSelection.get(editGeneration),
                                          selectedTimelineTracks()};
                std::swap(session, candidate);
                before.session = std::move(candidate);
                pushPreparedHistory(std::move(before), "Adjust selected clip edges",
                                    candidateRange);
                timelineClipSelection.retainAfterStableEdit(generation, editGeneration);
            } else {
                const juce::ScopedUnlock unlock(lock);
                candidate = {};
            }
        }
    }

    const auto message = changed ? "Adjusted selected clip " +
                                       juce::String(endEdge ? "ends" : "starts") + " by " +
                                       juce::String(samples) + " samples."
                                 : error;
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
    return changed;
}

bool Studio::trimSelectedGroupRange(const std::vector<carta::ClipReference>& clips,
                                    juce::uint64 generation, carta::ClipReference anchor,
                                    bool setStart, juce::int64 start, bool setEnd,
                                    juce::int64 end) {
    juce::String error;
    const bool changed = mutateGeometryState(
        [&](carta::Session& candidate, double) {
            const auto& current = timelineClipSelection.get(editGeneration);
            if (generation != editGeneration || clips.empty() || clips.size() != current.size() ||
                !std::equal(clips.begin(), clips.end(), current.begin(),
                            [](const auto& a, const auto& b) {
                                return a.track == b.track && a.clip == b.clip;
                            })) {
                error = "The clip selection changed; reopen the editor.";
                return false;
            }
            return carta::trimClipGroupToAnchorRange(candidate, clips, anchor, setStart, start,
                                                     setEnd, end, error);
        },
        error, "Trim grouped clips");
    const auto message =
        changed ? juce::String("Trimmed selected clip edges relative to the focused clip.") : error;
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
    return changed;
}

bool Studio::alignSelectedGroupSample(const std::vector<carta::ClipReference>& clips,
                                      juce::uint64 generation, carta::ClipReference anchor,
                                      bool endEdge, juce::int64 sample) {
    juce::String error;
    const bool changed = mutateGeometryState(
        [&](carta::Session& candidate, double) {
            const auto& current = timelineClipSelection.get(editGeneration);
            if (generation != editGeneration || clips.empty() || clips.size() != current.size() ||
                !std::equal(clips.begin(), clips.end(), current.begin(),
                            [](const auto& a, const auto& b) {
                                return a.track == b.track && a.clip == b.clip;
                            })) {
                error = "The clip selection changed; reopen the editor.";
                return false;
            }
            return carta::alignClipGroupEdgeToTime(candidate, clips, anchor, endEdge,
                                                   static_cast<double>(sample) / carta::sessionRate,
                                                   error);
        },
        error, "Move grouped clips");
    const auto message =
        changed ? juce::String("Moved selected clips using the focused clip as anchor.") : error;
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
    return changed;
}

void Studio::editSelectedClipEdges(bool endEdge) {
    const auto generation = editGeneration;
    const auto clips = timelineClipSelection.get(generation);
    if (clips.empty()) {
        status.setText("Select clips first.", juce::dontSendNotification);
        return;
    }
    showTextPrompt(
        endEdge ? "Move selected clip ends" : "Move selected clip starts",
        "Enter a signed project-sample offset. Every selected edge moves by the same amount; "
        "source and marker bounds apply to the whole edit.",
        "samples", "Sample offset", "0", "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), clips, generation,
         endEdge](bool accepted, const juce::String& value) {
            if (safe == nullptr || !accepted) {
                return;
            }
            juce::int64 samples = 0;
            if (!parseInteger(value, samples)) {
                safe->status.setText("Enter a signed integer sample offset.",
                                     juce::dontSendNotification);
                return;
            }
            safe->adjustSelectedClipEdges(clips, generation, endEdge, samples);
        });
}

bool Studio::sameClipGeometry(const carta::Clip& a, const carta::Clip& b) {
    return a.audio == b.audio && std::memcmp(&a.start, &b.start, sizeof(double)) == 0 &&
           std::memcmp(&a.offset, &b.offset, sizeof(double)) == 0 &&
           std::memcmp(&a.duration, &b.duration, sizeof(double)) == 0;
}

bool Studio::renameClipTo(int track, int index, juce::uint64 revision, const juce::String& text,
                          juce::String& error) {
    const auto name = text.trim();
    if (name.length() > 1024) {
        error = "Clip name must be at most 1024 characters.";
        return false;
    }
    if (!dialogTargetCurrent(revision)) {
        error = "Project changed; select the clip again.";
        return false;
    }
    const bool changed = mutateMixState(
        [&](carta::Session& candidate) {
            if (track < 0 || track >= static_cast<int>(candidate.tracks.size()) || index < 0 ||
                index >=
                    static_cast<int>(candidate.tracks[static_cast<size_t>(track)].clips.size())) {
                error = "The clip changed; select it again.";
                return false;
            }
            auto& clip =
                candidate.tracks[static_cast<size_t>(track)].clips[static_cast<size_t>(index)];
            if (clip.name == name) {
                error = "Clip name unchanged.";
                return false;
            }
            clip.name = name;
            return true;
        },
        error, "Rename clip");
    if (changed) {
        status.setText(name.isEmpty() ? "Clip now uses its source filename."
                                      : "Renamed clip: " + name + ".",
                       juce::dontSendNotification);
    }
    return changed;
}

void Studio::renameClip() {
    if (!validSelection()) {
        status.setText("Select a clip first.", juce::dontSendNotification);
        return;
    }
    const int track = selectedTrack;
    const int index = selectedClip;
    const auto original =
        session.tracks[static_cast<size_t>(track)].clips[static_cast<size_t>(index)];
    const auto revision = editGeneration;
    showTextPrompt("Rename clip",
                   "Leave the name empty to display the source filename: " +
                       original.audio->file.getFileName(),
                   "name", "Clip name", original.name, "Rename",
                   [safe = juce::Component::SafePointer<Studio>(this), track, index,
                    revision](bool accepted, const juce::String& value) {
                       if (safe == nullptr || !accepted) {
                           return;
                       }
                       if (!safe->dialogTargetCurrent(revision)) {
                           safe->status.setText("Project changed; reopen the editor.",
                                                juce::dontSendNotification);
                           return;
                       }
                       juce::String error;
                       if (!safe->renameClipTo(track, index, revision, value, error)) {
                           safe->status.setText(error, juce::dontSendNotification);
                       }
                   });
}

bool Studio::editSingleClipSamples(int track, int index, juce::uint64 revision, bool trim,
                                   bool setStart, juce::int64 start, bool setEnd, juce::int64 end,
                                   juce::String& error) {
    if (!dialogTargetCurrent(revision)) {
        error = "Project changed; select the clip again.";
        return false;
    }
    const bool changed = mutateGeometryState(
        [&](carta::Session& candidate, double) {
            if (track < 0 || track >= static_cast<int>(candidate.tracks.size()) || index < 0 ||
                index >=
                    static_cast<int>(candidate.tracks[static_cast<size_t>(track)].clips.size())) {
                error = "The clip changed; select it again.";
                return false;
            }
            auto& target =
                candidate.tracks[static_cast<size_t>(track)].clips[static_cast<size_t>(index)];
            if (target.groupId.isNotEmpty()) {
                error = "Clip belongs to a group; use the grouped clip commands.";
                return false;
            }
            if (trim) {
                return carta::trimClipGroupToAnchorRange(candidate, {{track, index}},
                                                         {track, index}, setStart, start, setEnd,
                                                         end, error);
            }
            auto edited = target;
            if (!carta::moveClipToSample(edited, start, error)) {
                return false;
            }
            if (sameClipGeometry(edited, target)) {
                error = "Clip position unchanged.";
                return false;
            }
            target = std::move(edited);
            return true;
        },
        error, trim ? "Trim clip" : "Move clip");
    if (!changed && error.isEmpty()) {
        error = "Clip unchanged.";
    }
    return changed;
}

void Studio::editClipSamples(bool trim) {
    if (!validSelection()) {
        return;
    }
    const bool grouped = session.tracks[static_cast<size_t>(selectedTrack)]
                             .clips[static_cast<size_t>(selectedClip)]
                             .groupId.isNotEmpty();
    if (grouped && !ensureFocusedPersistentGroup()) {
        status.setText("Select the grouped clip before editing its position.",
                       juce::dontSendNotification);
        return;
    }

    const int track = selectedTrack;
    const int index = selectedClip;
    const auto original =
        session.tracks[static_cast<size_t>(track)].clips[static_cast<size_t>(index)];
    const double start = original.start * carta::sessionRate;
    const double end = (original.start + original.duration) * carta::sessionRate;
    if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || end >= 9007199254740992.0) {
        status.setText("Clip position is outside the precise sample range.",
                       juce::dontSendNotification);
        return;
    }

    const auto revision = editGeneration;
    const auto selectedClips =
        grouped ? timelineClipSelection.get(revision) : std::vector<carta::ClipReference>{};
    const auto startText = juce::String(static_cast<juce::int64>(std::llround(start)));
    const auto endText = juce::String(static_cast<juce::int64>(std::llround(end)));
    const auto title = grouped ? (trim ? "Trim grouped clips" : "Move grouped clips")
                       : trim  ? "Trim clip"
                               : "Move clip";
    const auto instructions =
        grouped ? (trim ? "Set the focused clip's boundaries. The same edge movements apply to "
                          "every selected clip; unchanged fields retain their exact positions."
                        : "Set the focused clip's start. All selected clips move by the same "
                          "amount; an unchanged field retains its exact position.")
        : trim  ? "Enter samples, seconds (e.g. 1.5 s), or mm:ss.sss. End is exclusive; source "
                  "alignment is preserved."
                : "Enter samples, seconds (e.g. 1.5 s), or mm:ss.sss for the clip start. Source "
                  "audio and duration are unchanged.";

    std::vector<carta::TextPromptField> fields{{"start", "Start", startText}};
    if (trim) {
        fields.push_back({"end", "End (exclusive)", endText});
    }
    showTextFieldsPrompt(
        title, instructions, std::move(fields), "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), track, index, trim, revision, grouped,
         selectedClips, startText, endText](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted || values.size() != (trim ? 2 : 1)) {
                return;
            }
            if (!safe->dialogTargetCurrent(revision)) {
                safe->status.setText("Project changed; reopen the editor.",
                                     juce::dontSendNotification);
                return;
            }
            juce::int64 a = 0;
            juce::int64 b = 0;
            juce::String error;
            if (!carta::parseSampleOrTime(values[0], a) ||
                (trim && !carta::parseSampleOrTime(values[1], b))) {
                safe->status.setText(
                    "Enter samples, seconds (e.g. 1.5 s), or a time such as 1:02.500.",
                    juce::dontSendNotification);
                return;
            }
            if (grouped) {
                const bool changeStart = values[0].trim() != startText;
                if (!trim) {
                    if (!changeStart) {
                        safe->status.setText("Clip position unchanged.",
                                             juce::dontSendNotification);
                        return;
                    }
                    safe->alignSelectedGroupSample(selectedClips, revision, {track, index}, false,
                                                   a);
                    return;
                }
                const bool changeEnd = values[1].trim() != endText;
                if (!changeStart && !changeEnd) {
                    safe->status.setText("Clip boundaries unchanged.", juce::dontSendNotification);
                    return;
                }
                safe->trimSelectedGroupRange(selectedClips, revision, {track, index}, changeStart,
                                             a, changeEnd, b);
                return;
            }

            const bool changeStart = values[0].trim() != startText;
            const bool changeEnd = trim && values[1].trim() != endText;
            if (!changeStart && !changeEnd) {
                safe->status.setText("Clip unchanged.", juce::dontSendNotification);
                return;
            }
            const bool changed = safe->editSingleClipSamples(track, index, revision, trim,
                                                             changeStart, a, changeEnd, b, error);
            safe->status.setText(
                changed ? (trim ? "Trimmed clip to sample range." : "Moved clip to sample.")
                        : error,
                juce::dontSendNotification);
        });
}

bool Studio::moveTimeSelectionTo(juce::uint64 revision, const carta::TimeSelection& original,
                                 const std::vector<int>& independent, int target, juce::int64 start,
                                 juce::String& error) {
    const juce::ScopedLock guard(lock);
    error.clear();
    const auto current = timeSelection;
    if (dragMode != kDragNone || editGeneration != revision ||
        selectedTimelineTracks() != independent || current.start != original.start ||
        current.end != original.end || current.firstTrack != original.firstTrack ||
        current.lastTrack != original.lastTrack || !original.hasRange()) {
        error = "The selection or project changed; select a range and try again.";
        return false;
    }

    const bool keepPlayback = selectionPlaybackContinues();
    int linkedGroups = 0;
    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            linkedGroups = carta::rangeMoveLinkedGroups(candidate, original, independent);
            return independent.empty()
                       ? carta::moveRange(candidate, original, target, start, error)
                       : carta::moveIndependentRange(candidate, original, independent, target,
                                                     start, error);
        },
        [&] {
            const int span = independent.empty() ? original.lastTrack - original.firstTrack
                                                 : independent.back() - independent.front();
            timeSelection = {start, start + (original.end - original.start), target, target + span};
            selectedTrack = target;
            selectedClip = -1;
            selectedMarker = -1;
            timelineClipSelection.clear();
            timelineTracks.clear();
            if (!independent.empty()) {
                for (int track : independent) {
                    timelineTracks.insert(target + track - independent.front());
                }
            }
            timelineTrackRevision = revision + 1;
            keyboardRange.active = false;
            keyboardTracks.active = false;
            if (!keepPlayback) {
                stopSpecialAudition();
                playing = false;
                auditionActive = false;
                cutPreviewActive = false;
                position = static_cast<double>(start) / carta::sessionRate;
            }
            revealTrack(target);
        },
        "Move selected time", error);

    if (changed) {
        status.setText(
            juce::String("Moved selected audio; destination overlaps are mixed and markers "
                         "stayed in place.") +
                (linkedGroups > 0 ? " Moved audio remains grouped with audio outside the original "
                                    "selection."
                                  : ""),
            juce::dontSendNotification);
        announceNavigation(status.getText());
        repaint();
    } else if (error.isEmpty()) {
        error = "No audio was moved.";
    }
    return changed;
}

void Studio::moveTimeSelection() {
    const auto original = timeSelection;
    const auto revision = editGeneration;
    const auto independent = selectedTimelineTracks();
    if (!original.hasRange()) {
        return;
    }
    const int linkedGroups = carta::rangeMoveLinkedGroups(session, original, independent);
    const juce::String warning =
        linkedGroups > 0
            ? "\n\nMoved audio will remain grouped with audio outside the selected time or "
              "tracks. Ungroup clips first to move them independently."
            : "";
    showTextFieldsPrompt(
        "Move selected time",
        "Enter a destination sample or time and first track. Existing audio overlaps and sums; "
        "markers stay in place." +
            warning,
        {{"start", "Destination start", juce::String(original.start)},
         {"track", "Destination first track",
          juce::String((independent.empty() ? original.firstTrack : independent.front()) + 1)}},
        "Move",
        [safe = juce::Component::SafePointer<Studio>(this), original, revision,
         independent](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted || values.size() != 2) {
                return;
            }
            juce::int64 start = 0;
            juce::int64 trackNumber = 0;
            juce::String error;
            if (!carta::parseSampleOrTime(values[0], start) ||
                !parseInteger(values[1], trackNumber) || trackNumber < 1 ||
                trackNumber > std::numeric_limits<int>::max()) {
                safe->status.setText("Enter a valid destination sample or time and track number.",
                                     juce::dontSendNotification);
                return;
            }
            if (!safe->moveTimeSelectionTo(revision, original, independent,
                                           static_cast<int>(trackNumber - 1), start, error)) {
                safe->status.setText(error, juce::dontSendNotification);
            }
        });
}

void Studio::editTimeSelection(bool byLength) {
    if (session.tracks.empty()) {
        return;
    }
    const bool hasRange = timeSelection.hasRange();
    const auto maximum = static_cast<juce::int64>(std::ceil(fullLength() * carta::sessionRate));
    const auto start =
        hasRange ? timeSelection.start
                 : std::min(maximum,
                            static_cast<juce::int64>(std::llround(position * carta::sessionRate)));
    const auto end = hasRange ? timeSelection.end : std::min(maximum, start + 1);
    const auto first = hasRange ? timeSelection.firstTrack : std::max(0, selectedTrack);
    const auto last = hasRange ? timeSelection.lastTrack : first;
    const auto revision = editGeneration;

    showTextFieldsPrompt(
        "Set time selection",
        byLength ? "Enter samples, seconds (e.g. 1.5 s), or mm:ss.sss for start and length. Track "
                   "numbers start at 1."
                 : "Enter samples, seconds (e.g. 1.5 s), or mm:ss.sss. End is exclusive; track "
                   "numbers start at 1.",
        {{"start", "Start", juce::String(start)},
         {"end", byLength ? "Length" : "End (exclusive)",
          juce::String(byLength ? end - start : end)},
         {"first", "First track", juce::String(first + 1)},
         {"last", "Last track", juce::String(last + 1)}},
        "Select",
        [safe = juce::Component::SafePointer<Studio>(this), byLength,
         revision](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted || values.size() != 4) {
                return;
            }
            juce::int64 a = 0;
            juce::int64 b = 0;
            juce::int64 firstNumber = 0;
            juce::int64 lastNumber = 0;
            const juce::ScopedLock guard(safe->lock);
            if (safe->editGeneration != revision || safe->dragMode != kDragNone) {
                safe->status.setText("Project changed; reopen the selection editor.",
                                     juce::dontSendNotification);
                return;
            }
            const auto limit =
                static_cast<juce::int64>(std::ceil(safe->fullLength() * carta::sessionRate));
            if (!carta::parseSampleOrTime(values[0], a) ||
                !carta::parseSampleOrTime(values[1], b) || !parseInteger(values[2], firstNumber) ||
                !parseInteger(values[3], lastNumber) || firstNumber < 1 ||
                lastNumber < firstNumber ||
                lastNumber > static_cast<juce::int64>(safe->session.tracks.size())) {
                safe->status.setText(
                    "Invalid selection. Use samples, seconds with s, or mm:ss.sss and valid track "
                    "numbers.",
                    juce::dontSendNotification);
                return;
            }
            if (!byLength && (a < 0 || b <= a || b > limit)) {
                safe->status.setText(
                    "Invalid selection. Use sample positions within the timeline and valid track "
                    "numbers.",
                    juce::dontSendNotification);
                return;
            }
            const auto length = byLength ? b : b - a;
            carta::TimeSelection selected;
            if (!carta::selectionFromLength(
                    a, length, static_cast<int>(firstNumber - 1), static_cast<int>(lastNumber - 1),
                    limit, static_cast<int>(safe->session.tracks.size()), selected)) {
                safe->status.setText(
                    "Invalid selection. Use a positive length within the timeline and valid track "
                    "numbers.",
                    juce::dontSendNotification);
                return;
            }
            carta::activateTimeSelection(safe->timeSelection, safe->timelineClipSelection, selected,
                                         static_cast<int>(safe->session.tracks.size()));
            safe->timelineTracks.clear();
            safe->keyboardRange.active = false;
            safe->keyboardTracks.active = false;
            safe->selectedTrack = static_cast<int>(firstNumber - 1);
            safe->selectedClip = -1;
            safe->selectedMarker = -1;
            if (!safe->selectionPlaybackContinues()) {
                safe->position = static_cast<double>(a) / carta::sessionRate;
                safe->auditionActive = false;
            }
            safe->status.setText("Time selection set in samples.", juce::dontSendNotification);
            safe->menuItemsChanged();
            safe->repaint();
        });
}

bool Studio::parseNumber(const juce::String& text, double& value) {
    const auto utf8 = text.trim().toStdString();
    if (utf8.empty()) {
        return false;
    }
    char* end = nullptr;
    value = std::strtod(utf8.c_str(), &end);
    return end != utf8.c_str() && *end == '\0' && std::isfinite(value);
}

bool Studio::setSelectedClipEnvelopes(const std::vector<carta::ClipReference>& clips,
                                      juce::uint64 generation, double gain, double fadeIn,
                                      double fadeOut) {
    juce::String error;
    bool unchanged = false;
    const bool changed = mutateMixState(
        [&](carta::Session& candidate) {
            const auto& current = timelineClipSelection.get(editGeneration);
            if (generation != editGeneration || clips.empty() || current.size() != clips.size() ||
                !std::equal(clips.begin(), clips.end(), current.begin(),
                            [](const auto& a, const auto& b) {
                                return a.track == b.track && a.clip == b.clip;
                            })) {
                error = "The clip selection changed; reopen the editor.";
                return false;
            }
            if (!std::isfinite(gain) || !std::isfinite(fadeIn) || !std::isfinite(fadeOut) ||
                gain > carta::maxLinearGain || (gain < 0 && !std::equal_to<double>{}(gain, -1)) ||
                (fadeIn < 0 && !std::equal_to<double>{}(fadeIn, -1)) ||
                (fadeOut < 0 && !std::equal_to<double>{}(fadeOut, -1))) {
                error = "Invalid gain or fade duration.";
                return false;
            }
            bool different = false;
            for (const auto& ref : clips) {
                if (ref.track < 0 || ref.track >= static_cast<int>(candidate.tracks.size()) ||
                    ref.clip < 0 ||
                    ref.clip >=
                        static_cast<int>(
                            candidate.tracks[static_cast<size_t>(ref.track)].clips.size())) {
                    error = "A selected clip no longer exists.";
                    return false;
                }
                const auto& clip = candidate.tracks[static_cast<size_t>(ref.track)]
                                       .clips[static_cast<size_t>(ref.clip)];
                if ((fadeIn >= 0 && fadeIn > clip.duration) ||
                    (fadeOut >= 0 && fadeOut > clip.duration)) {
                    error = "Each fade must fit within every selected clip.";
                    return false;
                }
                different |=
                    (gain >= 0 && !std::equal_to<float>{}(clip.gain, static_cast<float>(gain))) ||
                    (fadeIn >= 0 && (!std::equal_to<double>{}(clip.fadeIn, fadeIn) ||
                                     !std::equal_to<double>{}(clip.fadeInStart, clip.offset))) ||
                    (fadeOut >= 0 &&
                     (!std::equal_to<double>{}(clip.fadeOut, fadeOut) ||
                      !std::equal_to<double>{}(clip.fadeOutEnd, clip.offset + clip.duration)));
            }
            if (!different) {
                unchanged = true;
                return false;
            }
            for (const auto& ref : clips) {
                auto& clip = candidate.tracks[static_cast<size_t>(ref.track)]
                                 .clips[static_cast<size_t>(ref.clip)];
                if (gain >= 0) {
                    clip.gain = static_cast<float>(gain);
                }
                if (fadeIn >= 0) {
                    clip.fadeIn = fadeIn;
                    clip.fadeInStart = clip.offset;
                }
                if (fadeOut >= 0) {
                    clip.fadeOut = fadeOut;
                    clip.fadeOutEnd = clip.offset + clip.duration;
                }
            }
            return true;
        },
        error, "Set selected clip gain and fades");

    status.setText(changed     ? "Updated gain and fades on selected clips."
                   : unchanged ? "Selected clip envelopes unchanged."
                               : error,
                   juce::dontSendNotification);
    menuItemsChanged();
    repaint();
    return changed;
}

void Studio::editSelectedClipEnvelopes() {
    const auto generation = editGeneration;
    const auto clips = timelineClipSelection.get(generation);
    if (clips.empty()) {
        status.setText("Select clips first.", juce::dontSendNotification);
        return;
    }
    showTextFieldsPrompt(
        "Selected clip gain and fades",
        "Leave a field empty to keep each clip's current value. Fades anchor to each clip edge "
        "and must fit every selected clip.",
        {{"gain", "Gain (%)", {}},
         {"in", "Fade in (seconds)", {}},
         {"out", "Fade out (seconds)", {}}},
        "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), clips,
         generation](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted || values.size() != 3) {
                return;
            }
            double gain = -1;
            double fadeIn = -1;
            double fadeOut = -1;
            const auto gainText = values[0].trim();
            const auto inText = values[1].trim();
            const auto outText = values[2].trim();
            if ((gainText.isNotEmpty() &&
                 (!parseNumber(gainText, gain) || gain < 0 || gain > 1600)) ||
                (inText.isNotEmpty() && (!parseNumber(inText, fadeIn) || fadeIn < 0)) ||
                (outText.isNotEmpty() && (!parseNumber(outText, fadeOut) || fadeOut < 0))) {
                safe->status.setText("Enter a gain from 0 to 1600% and nonnegative fade durations.",
                                     juce::dontSendNotification);
                return;
            }
            if (gain >= 0) {
                gain *= 0.01;
            }
            safe->setSelectedClipEnvelopes(clips, generation, gain, fadeIn, fadeOut);
        });
}

std::vector<carta::ClipReference> Studio::activeFadeShapeTargets() const {
    const auto selected = timelineClipSelection.get(editGeneration);
    if (!selected.empty()) {
        return selected;
    }
    if (validSelection()) {
        return {{selectedTrack, selectedClip}};
    }
    return {};
}

bool Studio::setTargetClipFadeShapes(bool setIn, bool setOut, carta::FadeShape shape,
                                     juce::String& error) {
    error.clear();
    const auto targets = activeFadeShapeTargets();
    if (targets.empty()) {
        error = "Select one or more clips first.";
        return false;
    }

    bool anyDifferent = false;
    for (const auto& ref : targets) {
        if (ref.track < 0 || ref.track >= static_cast<int>(session.tracks.size()) || ref.clip < 0 ||
            ref.clip >=
                static_cast<int>(session.tracks[static_cast<size_t>(ref.track)].clips.size())) {
            error = "Invalid clip selection.";
            return false;
        }
        const auto& c =
            session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)];
        if ((setIn && c.fadeInShape != shape) || (setOut && c.fadeOutShape != shape)) {
            anyDifferent = true;
        }
    }
    if (!anyDifferent) {
        return true;
    }

    return mutateMixState(
        [&](carta::Session& candidate) {
            for (const auto& ref : targets) {
                auto& c = candidate.tracks[static_cast<size_t>(ref.track)]
                              .clips[static_cast<size_t>(ref.clip)];
                if (setIn) {
                    c.fadeInShape = shape;
                }
                if (setOut) {
                    c.fadeOutShape = shape;
                }
            }
            return true;
        },
        error, "Change fade curve shape");
}

bool Studio::applyClipMixOrRangeGainEdit(bool normalize, double value, bool independent,
                                         const juce::String& actionName, juce::String& error) {
    const juce::ScopedLock guard(lock);
    error.clear();
    if (dragMode != kDragNone) {
        error = "Finish or cancel the current drag first.";
        return false;
    }

    const auto generation = editGeneration;
    const auto selectedClips = timelineClipSelection.get(generation);
    if (!selectedClips.empty()) {
        return mutateMixState(
            [&](carta::Session& candidate) {
                return normalize ? carta::normalizeClips(candidate, selectedClips, value,
                                                         independent, error)
                                 : carta::amplifyClips(candidate, selectedClips, value, error);
            },
            error, actionName);
    }

    if (timeSelection.hasRange()) {
        const auto independentTracks = selectedTimelineTracks();
        std::vector<int> scopeTracks;
        if (!independentTracks.empty()) {
            scopeTracks = independentTracks;
        } else {
            for (int t = std::max(0, timeSelection.firstTrack);
                 t <=
                 std::min(static_cast<int>(session.tracks.size()) - 1, timeSelection.lastTrack);
                 ++t) {
                scopeTracks.push_back(t);
            }
        }
        const auto rangeStart = timeSelection.start;
        const auto rangeEnd = timeSelection.end;
        const auto focusTrack = selectedTrack;
        const bool changed = prepareRangeSessionEdit(
            [&](carta::Session& candidate) {
                std::vector<carta::ClipReference> targets;
                for (int t : scopeTracks) {
                    if (t < 0 || t >= static_cast<int>(candidate.tracks.size())) {
                        continue;
                    }
                    carta::splitTracksAtSample(candidate, {t, t}, rangeStart);
                    carta::splitTracksAtSample(candidate, {t, t}, rangeEnd);
                    const auto& clips = candidate.tracks[static_cast<size_t>(t)].clips;
                    for (int c = 0; c < static_cast<int>(clips.size()); ++c) {
                        juce::int64 cs = 0;
                        juce::int64 ce = 0;
                        if (carta::clipSampleRange(clips[static_cast<size_t>(c)], cs, ce) &&
                            cs >= rangeStart && ce <= rangeEnd) {
                            targets.push_back({t, c});
                        }
                    }
                }
                if (targets.empty()) {
                    error = normalize ? "No audio in selection to normalize."
                                      : "No audio in selection to amplify.";
                    return false;
                }
                return (normalize
                            ? carta::normalizeClips(candidate, targets, value, independent, error)
                            : carta::amplifyClips(candidate, targets, value, error)) &&
                       carta::preservesMarkerBounds(session, candidate, error);
            },
            [&] {
                if (std::find(scopeTracks.begin(), scopeTracks.end(), focusTrack) !=
                    scopeTracks.end()) {
                    selectedClip = -1;
                }
                if (timelineTrackRevision == generation) {
                    timelineTrackRevision = generation + 1;
                }
            },
            actionName, error);
        repaint();
        return changed;
    }

    if (validSelection()) {
        const std::vector<carta::ClipReference> single{{selectedTrack, selectedClip}};
        return mutateMixState(
            [&](carta::Session& candidate) {
                return normalize
                           ? carta::normalizeClips(candidate, single, value, independent, error)
                           : carta::amplifyClips(candidate, single, value, error);
            },
            error, actionName);
    }

    error = "Select clips, a time range, or a focused clip first.";
    return false;
}

void Studio::showAmplifyDialog() {
    juce::String initial = "0.0";
    if (validSelection()) {
        const auto& clip = session.tracks[static_cast<size_t>(selectedTrack)]
                               .clips[static_cast<size_t>(selectedClip)];
        float rawPeak = 0.0f;
        juce::String ignored;
        if (carta::measureClipRawPeak(clip, rawPeak, ignored) && rawPeak * clip.gain > 0.0f) {
            const double headroom =
                -20.0 * std::log10(static_cast<double>(rawPeak) * static_cast<double>(clip.gain));
            if (std::isfinite(headroom)) {
                initial = juce::String(headroom, 2);
            }
        }
    }

    const auto revision = editGeneration;
    showTextPrompt("Amplify", "Amplification amount in dB (maximum clip gain is +24.08 dB):", "db",
                   "Gain change (dB)", initial, "Apply",
                   [safe = juce::Component::SafePointer<Studio>(this),
                    revision](bool accepted, const juce::String& text) {
                       if (safe == nullptr || !accepted) {
                           return;
                       }
                       if (safe->editGeneration != revision) {
                           safe->status.setText("Project changed; reopen Amplify.",
                                                juce::dontSendNotification);
                           return;
                       }
                       double db = 0.0;
                       juce::String error;
                       if (!parseNumber(text.trim(), db)) {
                           safe->status.setText("Enter a finite amplification in dB.",
                                                juce::dontSendNotification);
                           return;
                       }
                       if (safe->applyClipMixOrRangeGainEdit(false, db, false, "Amplify", error)) {
                           safe->status.setText("Amplified selection.", juce::dontSendNotification);
                       } else {
                           safe->status.setText(error, juce::dontSendNotification);
                       }
                   });
}

void Studio::showNormalizeDialog() {
    const auto revision = editGeneration;
    showTextFieldsChoicePrompt(
        "Normalize", "Normalize peak amplitude to target dBFS (-120 to 0):",
        {{"targetDb", "Target peak (dBFS)", "-1.0"}}, "Normalize (Linked)", "Normalize Independent",
        [safe = juce::Component::SafePointer<Studio>(this),
         revision](int choice, const juce::StringArray& values) {
            if (safe == nullptr || choice == 0) {
                return;
            }
            if (safe->editGeneration != revision) {
                safe->status.setText("Project changed; reopen Normalize.",
                                     juce::dontSendNotification);
                return;
            }
            double targetDb = -1.0;
            juce::String error;
            if (values.size() != 1 || !parseNumber(values[0].trim(), targetDb)) {
                safe->status.setText("Enter a valid target peak in dBFS.",
                                     juce::dontSendNotification);
                return;
            }
            const bool independent = (choice == 2);
            if (safe->applyClipMixOrRangeGainEdit(true, targetDb, independent, "Normalize",
                                                  error)) {
                safe->status.setText(independent ? "Normalized clips independently."
                                                 : "Normalized selection.",
                                     juce::dontSendNotification);
            } else {
                safe->status.setText(error, juce::dontSendNotification);
            }
        });
}

void Studio::editClipEnvelope() {
    if (!validSelection()) {
        return;
    }
    const int track = selectedTrack;
    const int index = selectedClip;
    const auto& clip = session.tracks[static_cast<size_t>(track)].clips[static_cast<size_t>(index)];
    const auto revision = editGeneration;
    const auto originalGain = clip.gain;
    const auto shownGain = juce::String(clip.gain * 100.0f, 1);
    const auto shownIn = juce::String(clip.fadeIn, 6);
    const auto shownOut = juce::String(clip.fadeOut, 6);

    showTextFieldsPrompt(
        "Clip gain and fades",
        "Gain is linear; changing a fade anchors it to the current clip edge.",
        {{"gain", "Gain (%)", shownGain},
         {"in", "Fade in (seconds)", shownIn},
         {"out", "Fade out (seconds)", shownOut}},
        "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), track, index, originalGain, shownGain,
         shownIn, shownOut, revision](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted || values.size() != 3) {
                return;
            }
            if (!safe->dialogTargetCurrent(revision)) {
                safe->status.setText("Project changed; reopen the editor.",
                                     juce::dontSendNotification);
                return;
            }
            double in = 0;
            double out = 0;
            carta::EditedFloat gain;
            if (!carta::parseEditedFloat(values[0], shownGain, originalGain, 0, 1600, 0.01, gain) ||
                !parseNumber(values[1], in) || !parseNumber(values[2], out)) {
                safe->status.setText("Invalid clip gain or fade duration.",
                                     juce::dontSendNotification);
                return;
            }
            bool unchanged = false;
            juce::String error;
            const bool applied = safe->mutateMixState(
                [&](carta::Session& candidate) {
                    if (track < 0 || track >= static_cast<int>(candidate.tracks.size())) {
                        return false;
                    }
                    auto& clips = candidate.tracks[static_cast<size_t>(track)].clips;
                    if (index < 0 || index >= static_cast<int>(clips.size())) {
                        return false;
                    }
                    auto& target = clips[static_cast<size_t>(index)];
                    const bool changeIn = std::abs(in - shownIn.getDoubleValue()) > 1.0e-12;
                    const bool changeOut = std::abs(out - shownOut.getDoubleValue()) > 1.0e-12;
                    if ((changeIn && (in < 0 || in > target.duration)) ||
                        (changeOut && (out < 0 || out > target.duration))) {
                        return false;
                    }
                    if (!gain.edited && !changeIn && !changeOut) {
                        unchanged = true;
                        return false;
                    }
                    if (gain.edited) {
                        target.gain = gain.value;
                    }
                    if (changeIn) {
                        target.fadeIn = in;
                        target.fadeInStart = target.offset;
                    }
                    if (changeOut) {
                        target.fadeOut = out;
                        target.fadeOutEnd = target.offset + target.duration;
                    }
                    return true;
                },
                error, "Change clip gain and fades");
            safe->status.setText(applied              ? "Clip gain and fades updated."
                                 : error.isNotEmpty() ? error
                                 : unchanged          ? "Clip gain and fades unchanged."
                                                      : "Fades must fit within the clip.",
                                 juce::dontSendNotification);
        });
}

bool Studio::applyTrackRoutingMatrix(int index, int count, juce::uint64 revision,
                                     std::vector<carta::StereoRoute> matrix, bool fieldsEdited,
                                     juce::String& error) {
    if (revision != editGeneration || index < 0 ||
        index >= static_cast<int>(session.tracks.size()) ||
        carta::maxTrackChannels(session.tracks[static_cast<size_t>(index)]) != count ||
        static_cast<int>(matrix.size()) != count) {
        error = "Track channels changed; reopen routing.";
        return false;
    }
    if (!fieldsEdited &&
        carta::clipRoutingConfirmed(session.tracks[static_cast<size_t>(index)], count)) {
        error = "Track routing unchanged.";
        return false;
    }
    return mutateMixState(
        [&](carta::Session& candidate) {
            candidate.tracks[static_cast<size_t>(index)].routing = std::move(matrix);
            return true;
        },
        error, "Change track routing");
}

void Studio::editTrackRouting() {
    if (selectedTrack < 0 || selectedTrack >= static_cast<int>(session.tracks.size())) {
        return;
    }
    const int index = selectedTrack;
    const auto& track = session.tracks[static_cast<size_t>(index)];
    const int count = carta::maxTrackChannels(track);
    if (count <= 0) {
        status.setText("Add audio to the layer before setting routing.",
                       juce::dontSendNotification);
        return;
    }
    if (count > 4096) {
        status.setText("Routing supports up to 4096 source channels per layer.",
                       juce::dontSendNotification);
        return;
    }

    juce::StringArray left;
    juce::StringArray right;
    std::vector<carta::StereoRoute> originalRoutes;
    originalRoutes.reserve(static_cast<size_t>(count));
    for (int channel = 0; channel < count; ++channel) {
        const auto route = static_cast<size_t>(channel) < track.routing.size()
                               ? track.routing[static_cast<size_t>(channel)]
                               : carta::defaultChannelRoute(channel, count);
        originalRoutes.push_back(route);
        left.add(juce::String(route.left, 9));
        right.add(juce::String(route.right, 9));
    }

    const auto revision = editGeneration;
    const bool previewChanges = carta::routingMatrixChangesPreview(track, originalRoutes);
    const bool offerSharedMatrix = previewChanges && carta::clipRoutingConfirmed(track, count);
    juce::String instructions =
        "Enter one comma-separated coefficient per source channel, in source order. 0 is silence; "
        "1 is unity. This matrix applies to every clip on the layer.";
    if (previewChanges) {
        instructions +=
            "\n\nThe suggested matrix changes the current preview for some clips because their "
            "channel counts differ or their routing is incomplete. Review the values before "
            "applying.";
    }
    if (offerSharedMatrix) {
        instructions +=
            " Apply without edits keeps the current per-clip defaults; use Apply shared matrix to "
            "adopt these values.";
    }

    showTextFieldsChoicePrompt(
        "Layer channel routing", instructions,
        {{"left", "To left", left.joinIntoString(", ")},
         {"right", "To right", right.joinIntoString(", ")}},
        "Apply", offerSharedMatrix ? "Apply shared matrix" : "",
        [safe = juce::Component::SafePointer<Studio>(this), index, count, revision, left, right,
         originalRoutes](int answer, const juce::StringArray& values) {
            if (safe == nullptr || (answer != 1 && answer != 2)) {
                return;
            }
            if (!safe->dialogTargetCurrent(revision)) {
                safe->status.setText("Project changed; reopen the editor.",
                                     juce::dontSendNotification);
                return;
            }
            juce::StringArray l;
            juce::StringArray r;
            l.addTokens(values[0], ",", {});
            r.addTokens(values[1], ",", {});
            std::vector<carta::StereoRoute> matrix;
            matrix.reserve(static_cast<size_t>(count));
            if (l.size() != count || r.size() != count) {
                safe->status.setText("Enter exactly " + juce::String(count) +
                                         " coefficients for each output.",
                                     juce::dontSendNotification);
                return;
            }
            bool fieldsEdited = false;
            for (int i = 0; i < count; ++i) {
                carta::EditedFloat a;
                carta::EditedFloat b;
                const auto& original = originalRoutes[static_cast<size_t>(i)];
                if (!carta::parseEditedFloat(l[i], left[i], original.left, -2, 2, 1, a) ||
                    !carta::parseEditedFloat(r[i], right[i], original.right, -2, 2, 1, b)) {
                    safe->status.setText("Routing coefficients must be numbers from -2 to 2.",
                                         juce::dontSendNotification);
                    return;
                }
                matrix.push_back({a.value, b.value});
                fieldsEdited |= a.edited || b.edited;
            }
            juce::String error;
            const bool applied = safe->applyTrackRoutingMatrix(
                index, count, revision, std::move(matrix), fieldsEdited || answer == 2, error);
            safe->status.setText(applied ? "Layer channel routing updated." : error,
                                 juce::dontSendNotification);
        });
}

void Studio::editTrackMix() {
    if (selectedTrack < 0 || selectedTrack >= static_cast<int>(session.tracks.size())) {
        return;
    }
    const int index = selectedTrack;
    const auto& track = session.tracks[static_cast<size_t>(index)];
    const auto revision = editGeneration;
    const auto originalGain = track.gain;
    const auto originalPan = track.pan;
    const auto shownGain = juce::String(track.gain * 100.0f, 1);
    const auto shownPan = juce::String(track.pan * 100.0f, 1);

    showTextFieldsPrompt(
        "Layer gain and balance", utf8Str("Gain: 0–1600%. Balance: -100 (left) to 100 (right)."),
        {{"gain", "Gain (%)", shownGain}, {"pan", "Balance", shownPan}}, "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), index, originalGain, originalPan,
         shownGain, shownPan, revision](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted || values.size() != 2) {
                return;
            }
            if (!safe->dialogTargetCurrent(revision)) {
                safe->status.setText("Project changed; reopen the editor.",
                                     juce::dontSendNotification);
                return;
            }
            carta::EditedFloat gain;
            carta::EditedFloat pan;
            if (!carta::parseEditedFloat(values[0], shownGain, originalGain, 0, 1600, 0.01, gain) ||
                !carta::parseEditedFloat(values[1], shownPan, originalPan, -100, 100, 0.01, pan)) {
                safe->status.setText("Invalid gain or balance; layer was not changed.",
                                     juce::dontSendNotification);
                return;
            }
            juce::String error;
            const bool changed = safe->mutateMixState(
                [&](carta::Session& candidate) {
                    if (index < 0 || index >= static_cast<int>(candidate.tracks.size())) {
                        return false;
                    }
                    auto& target = candidate.tracks[static_cast<size_t>(index)];
                    if (!gain.edited && !pan.edited) {
                        return false;
                    }
                    if (gain.edited) {
                        target.gain = gain.value;
                    }
                    if (pan.edited) {
                        target.pan = pan.value;
                    }
                    return true;
                },
                error, "Change track gain and balance");
            safe->status.setText(error.isNotEmpty() ? error
                                 : changed          ? "Layer gain and balance updated."
                                                    : "Layer gain and balance unchanged.",
                                 juce::dontSendNotification);
        });
}

bool Studio::renameTrackTo(int index, const juce::String& name, juce::String& error) {
    const auto trimmed = name.trim();
    if (trimmed.isEmpty() || trimmed.containsAnyOf("\r\n")) {
        error = "Layer name must be nonempty and single-line.";
        return false;
    }
    return mutateMixState(
        [&](carta::Session& candidate) {
            if (index < 0 || index >= static_cast<int>(candidate.tracks.size())) {
                error = "Invalid layer index.";
                return false;
            }
            auto& track = candidate.tracks[static_cast<size_t>(index)];
            if (track.name == trimmed) {
                return false;
            }
            track.name = trimmed;
            return true;
        },
        error, "Rename track");
}

void Studio::renameTrack() {
    if (selectedTrack < 0 || selectedTrack >= static_cast<int>(session.tracks.size())) {
        return;
    }
    const int index = selectedTrack;
    const auto revision = editGeneration;
    showTextPrompt("Rename layer", "Layer name", "name", "Layer name",
                   session.tracks[static_cast<size_t>(index)].name, "Rename",
                   [safe = juce::Component::SafePointer<Studio>(this), index,
                    revision](bool accepted, const juce::String& value) {
                       if (safe == nullptr || !accepted) {
                           return;
                       }
                       if (!safe->dialogTargetCurrent(revision)) {
                           safe->status.setText("Project changed; reopen the editor.",
                                                juce::dontSendNotification);
                           return;
                       }
                       juce::String error;
                       const bool changed = safe->renameTrackTo(index, value, error);
                       safe->status.setText(error.isNotEmpty() ? error
                                            : changed          ? "Layer renamed."
                                                               : "Layer name unchanged.",
                                            juce::dontSendNotification);
                   });
}

// ============================================================================
// Render / Bounce & Clip Consolidation
// ============================================================================

void Studio::configureQuietGapSearch() {
    if (activeLevelAnalysis || !pendingImports.empty()) {
        status.setText("Wait for audio analysis and imports to finish.",
                       juce::dontSendNotification);
        return;
    }
    const auto revision = editGeneration;
    const int channels = masterChannels;
    showTextFieldsPrompt(
        "Suggest markers from quiet gaps",
        "Search the rendered master for internal quiet regions. Suggestions are reviewed before "
        "adding markers.",
        {{"threshold", "Maximum level (dBFS)", "-50"},
         {"duration", "Minimum duration (seconds)", "0.75"}},
        "Search",
        [safe = juce::Component::SafePointer<Studio>(this), revision,
         channels](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted) {
                return;
            }
            double threshold = 0;
            double duration = 0;
            if (!parseNumber(values[0], threshold) || !parseNumber(values[1], duration) ||
                threshold < -120 || threshold > 0 || duration < 0.1 || duration > 60) {
                safe->status.setText(
                    "Use a threshold from -120 to 0 dBFS and a duration from 0.1 to 60 seconds.",
                    juce::dontSendNotification);
                return;
            }
            safe->startQuietGapSearch(
                threshold, static_cast<juce::int64>(std::llround(duration * carta::sessionRate)),
                revision, channels);
        });
}

bool Studio::sameClipReferences(const std::vector<carta::ClipReference>& left,
                                const std::vector<carta::ClipReference>& right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](const auto& a, const auto& b) {
               return a.track == b.track && a.clip == b.clip;
           });
}

bool Studio::startSelectedTrackRender(const juce::File& target, bool ipc, juce::String& error,
                                      RenderScope scope, std::vector<int> explicitTracks,
                                      bool allowRoutingChange) {
    error.clear();
    const bool pair = scope == RenderScope::stereoPair;
    const bool fullTracks = scope == RenderScope::tracks || pair;
    const bool consolidate = scope == RenderScope::consolidate;
    const bool clips = scope == RenderScope::clips || consolidate;

    if (activeExport || !pendingImports.empty()) {
        error = "Wait for the active export and imports to finish.";
        return false;
    }
    if (dragMode != kDragNone) {
        error = "Finish the current drag first.";
        return false;
    }
    if (!target.hasFileExtension("wav") || target.exists() ||
        !target.getParentDirectory().isDirectory()) {
        error = "Choose a new WAV file in an existing folder.";
        return false;
    }
    if (!validateRetainedMedia(target, error)) {
        return false;
    }
    if (activeSave && carta::pathsReferToSameFile(activeSave->file, target)) {
        error = "Choose an output other than the project being saved.";
        return false;
    }

    auto task = std::make_shared<ExportTask>();
    carta::TimeSelection selection;
    carta::WavRange range;
    std::vector<int> independent;
    std::vector<int> chosen;
    std::vector<carta::ClipReference> rawClips;
    std::vector<carta::ClipReference> expandedClips;
    int channels = 0;
    int expectedMaster = 0;
    int consolidationTrack = -1;
    int focusedTrack = -1;
    int focusedClip = -1;
    double outputRate = 0;
    juce::uint64 revision = 0;
    juce::File protectedProject;
#if CARTA_STUDIO_COMPONENT_TEST
    task->verificationHook = consolidationVerificationHook;
#endif
    {
        const juce::ScopedLock guard(lock);
        selection = timeSelection;
        focusedTrack = selectedTrack;
        focusedClip = selectedClip;
        if (clips) {
            rawClips = timelineClipSelection.get(editGeneration);
            auto requested = rawClips;
            if (requested.empty()) {
                if (!validSelection()) {
                    error = "Select at least one clip to render.";
                    return false;
                }
                requested.push_back({focusedTrack, focusedClip});
            }
            if (!carta::expandClipGroups(session, requested, expandedClips, error)) {
                return false;
            }
        } else {
            if (!fullTracks && (!selection.hasRange() ||
                                selection.lastTrack >= static_cast<int>(session.tracks.size()))) {
                error = "Select time on the tracks to render first.";
                return false;
            }
            independent = selectedTimelineTracks();
            chosen = explicitTracks.empty() ? independent : explicitTracks;
            if (chosen.empty()) {
                if (fullTracks && !selection.hasRange()) {
                    if (focusedTrack < 0 ||
                        focusedTrack >= static_cast<int>(session.tracks.size())) {
                        error = "Focus or select tracks to render first.";
                        return false;
                    }
                    chosen.push_back(focusedTrack);
                } else {
                    for (int track = selection.firstTrack; track <= selection.lastTrack; ++track) {
                        chosen.push_back(track);
                    }
                }
            }
        }
        expectedMaster = masterChannels;
        channels = pair ? 2 : masterChannels;
        revision = editGeneration;
        outputRate = deviceRate;
        protectedProject = projectFile;
        copyWorkerSnapshot(task->snapshot);
    }

    if (!clips) {
        range = fullTracks ? carta::fullWavRange(task->snapshot)
                           : carta::WavRange{selection.start, selection.end};
    }
    if (pair) {
        if (chosen.size() != 2) {
            error = "Select exactly two mono tracks; the upper track becomes left and the lower "
                    "becomes right.";
            return false;
        }
        carta::Session render;
        if (!carta::prepareMonoStereoPair(task->snapshot, chosen[0], chosen[1], render, error,
                                          &range.start, &range.end)) {
            return false;
        }
        task->snapshot = std::move(render);
    } else if (consolidate) {
        carta::Session render;
        if (!carta::prepareClipConsolidation(task->snapshot, expandedClips, render,
                                             consolidationTrack, channels, range.start, range.end,
                                             error)) {
            return false;
        }
        task->verificationSnapshot = std::move(task->snapshot);
        task->snapshot = std::move(render);
    } else if (clips) {
        if (!carta::isolateClipGroup(task->snapshot, expandedClips, range.start, range.end,
                                     error)) {
            return false;
        }
    } else if (!carta::limitToTrackIndices(task->snapshot, chosen)) {
        error = "Selected tracks are no longer valid.";
        return false;
    }

    const carta::WavOptions options{carta::WavEncoding::float32, false, false,
                                    consolidate && channels > 2};
    if (!carta::validateWavRange(task->snapshot, channels, range, error, options) ||
        !carta::validateOutputDestination(task->snapshot, target, protectedProject, error)) {
        return false;
    }

    task->target = target;
    task->ipc = ipc;
    task->noReplace = true;
    task->renderTracks = true;
    task->phase.store(1);
    activeExport = task;

    if (ipc) {
        lastIpcExportHasGeneration = false;
        auto* info = new juce::DynamicObject();
        info->setProperty("kind", pair          ? "stereo-pair"
                                  : consolidate ? "consolidate-clips"
                                  : clips       ? "render-clips"
                                  : fullTracks  ? "render-tracks"
                                                : "render-selected");
        if (consolidate) {
            info->setProperty("routing_change_allowed", allowRoutingChange);
        }
        info->setProperty("state", "running");
        info->setProperty("phase", "writing");
        info->setProperty("progress", 0.0);
        info->setProperty("cancellation_requested", false);
        info->setProperty("file_written", false);
        info->setProperty("path", target.getFullPathName());
        info->setProperty("start_sample", range.start);
        info->setProperty("end_sample", range.end);
        lastIpcExport = juce::var(info);
    }

    status.setText(pair          ? "Rendering mono tracks as stereo..."
                   : consolidate ? "Consolidating selected clips to a new WAV..."
                   : clips       ? "Rendering selected clips to a new WAV..."
                                 : "Rendering selected tracks to a new WAV...",
                   juce::dontSendNotification);
    menuItemsChanged();
    resized();

    exportPool.addJob([safe = juce::Component::SafePointer<Studio>(this), task, selection,
                       independent, rawClips, expandedClips, focusedTrack, focusedClip,
                       consolidationTrack, fullTracks, clips, consolidate, pair, ipc, channels,
                       expectedMaster, revision, outputRate, protectedProject, range, options,
                       allowRoutingChange] {
        bool written = false;
        bool clipped = false;
        auto directorySync = carta::DirectorySyncStatus::unsupported;
        juce::String failure;
        std::shared_ptr<carta::Audio> audio;
        std::shared_ptr<const carta::SincResampler> kernel;
        try {
            written = carta::exportWavRange(
                task->snapshot, channels, range, task->target, clipped, failure, protectedProject,
                [task, consolidate](double p) {
                    task->progress.store(p * (consolidate ? 0.6 : 0.7));
                    return !task->cancelled.load();
                },
                options, true, &directorySync);
            if (written && directorySync == carta::DirectorySyncStatus::unconfirmed) {
                failure =
                    "WAV was written, but directory synchronization failed; no project media was "
                    "added.";
            }
            if (written && directorySync != carta::DirectorySyncStatus::unconfirmed &&
                !task->cancelled.load()) {
                task->phase.store(3);
                juce::AudioFormatManager formats;
                formats.registerBasicFormats();
                audio =
                    carta::loadAudio(formats, task->target, failure, [task, consolidate](double p) {
                        task->progress.store(consolidate ? 0.6 + p * 0.2 : 0.7 + p * 0.3);
                        return !task->cancelled.load();
                    });
                if (audio && consolidate && !task->cancelled.load()) {
                    task->phase.store(4);
#if CARTA_STUDIO_COMPONENT_TEST
                    if (task->verificationHook) {
                        task->verificationHook();
                    }
#endif
                    if (!carta::verifyClipConsolidation(
                            task->verificationSnapshot, expandedClips, audio, range.start,
                            range.end, failure,
                            [task](double p) {
                                task->progress.store(0.8 + p * 0.2);
                                return !task->cancelled.load();
                            },
                            allowRoutingChange)) {
                        audio.reset();
                    }
                }
                if (audio && !task->cancelled.load() && std::isfinite(outputRate) &&
                    outputRate > 0) {
                    kernel = std::make_shared<carta::SincResampler>(audio->sampleRate, outputRate);
                }
            }
        } catch (const std::exception& e) {
            failure = "Track render failed: " + juce::String(e.what());
        } catch (...) {
            failure = "Track render failed.";
        }

        juce::MessageManager::callAsync([safe, task, selection, independent, rawClips,
                                         expandedClips, focusedTrack, focusedClip,
                                         consolidationTrack, fullTracks, clips, consolidate, pair,
                                         ipc, expectedMaster, revision, range, audio, kernel,
                                         failure, written, directorySync] {
            if (safe == nullptr || safe->activeExport != task) {
                return;
            }
            const bool cancelled = task->cancelled.load();
            juce::String problem = failure;
            bool added = false;
            {
                const juce::ScopedLock guard(safe->lock);
                const auto current = safe->timeSelection;
                const bool timeChanged =
                    !clips && !fullTracks &&
                    (current.start != selection.start || current.end != selection.end ||
                     current.firstTrack != selection.firstTrack ||
                     current.lastTrack != selection.lastTrack);
                const bool rangeRowsChanged =
                    fullTracks && independent.empty() && selection.hasRange() &&
                    (!current.hasRange() || current.firstTrack != selection.firstTrack ||
                     current.lastTrack != selection.lastTrack);
                const bool tracksChanged =
                    !clips && !(pair && ipc) &&
                    (safe->selectedTimelineTracks() != independent || rangeRowsChanged ||
                     (fullTracks && independent.empty() && !selection.hasRange() &&
                      (current.hasRange() || safe->selectedTrack != focusedTrack)));
                const bool clipsChanged =
                    clips &&
                    (!sameClipReferences(rawClips,
                                         safe->timelineClipSelection.get(safe->editGeneration)) ||
                     (rawClips.empty() &&
                      (safe->selectedTrack != focusedTrack || safe->selectedClip != focusedClip)));

                if (!cancelled && audio &&
                    (safe->editGeneration != revision || safe->masterChannels != expectedMaster ||
                     safe->dragMode != kDragNone || timeChanged || tracksChanged || clipsChanged)) {
                    problem =
                        consolidate
                            ? "Project or selection changed while rendering; clips were not "
                              "replaced."
                            : "Project or selection changed while rendering; no track was added.";
                } else if (!cancelled && audio) {
                    safe->stopSpecialAudition();
                    const auto before = safe->captureSelectionState();
                    const auto generation = safe->editGeneration;
                    int replacement = -1;
                    carta::ClipSelection nextClips;
                    std::set<int> nextTracks;
                    added = safe->prepareRangeSessionEdit(
                        [&](carta::Session& candidate) {
                            if (consolidate) {
                                if (!carta::replaceClipsWithConsolidation(
                                        candidate, expandedClips, audio, range.start, range.end,
                                        replacement, problem)) {
                                    return false;
                                }
                            } else {
                                if (!carta::addImportedAudioTrack(candidate, audio, range.start)) {
                                    return false;
                                }
                                candidate.tracks.back().muted = true;
                            }
                            if (!carta::preservesOnDiscAudio(safe->session, candidate, problem) ||
                                !carta::preservesMarkerBounds(safe->session, candidate, problem)) {
                                return false;
                            }
                            const auto selected =
                                consolidate ? std::vector<carta::ClipReference>{{consolidationTrack,
                                                                                 replacement}}
                                            : before.selectedClips;
                            if (!nextClips.restore(candidate, selected, generation + 1)) {
                                if (consolidate) {
                                    problem = "Could not select the consolidated clip.";
                                    return false;
                                }
                                nextClips.clear();
                            }
                            nextTracks.insert(before.selectedTracks.begin(),
                                              before.selectedTracks.end());
                            return true;
                        },
                        [&] {
                            safe->resampling.registerPrepared(audio->sampleRate, safe->deviceRate,
                                                              kernel);
                            safe->timelineClipSelection = std::move(nextClips);
                            safe->timelineTracks = std::move(nextTracks);
                            safe->timelineTrackRevision = generation + 1;
                            safe->keyboardRange.active = false;
                            if (consolidate) {
                                safe->selectedTrack = consolidationTrack;
                                safe->selectedClip = replacement;
                            }
                        },
                        pair          ? "Render mono tracks as stereo"
                        : consolidate ? "Consolidate clips"
                        : clips       ? "Render selected clips"
                                      : "Render selected tracks",
                        problem);
                    if (!added && problem.isEmpty()) {
                        problem = consolidate ? "Could not replace the selected clips."
                                              : "Could not add the rendered track.";
                    }
                    if (added && consolidate && task->ipc) {
                        if (auto* result = safe->lastIpcExport.getDynamicObject()) {
                            result->setProperty("clip_index", replacement);
                        }
                    }
                }
            }

            if (!added && problem.isEmpty()) {
                problem =
                    cancelled ? "Track render cancelled." : "Could not read the rendered WAV.";
            }
            if (written && !added) {
                problem += " The rendered WAV remains at " + task->target.getFullPathName() + ".";
            }
            if (task->ipc) {
                if (auto* result = safe->lastIpcExport.getDynamicObject()) {
                    result->setProperty("state", added       ? "complete"
                                                 : cancelled ? "cancelled"
                                                             : "failed");
                    result->setProperty("phase", "finished");
                    result->setProperty(
                        "progress", added ? 1.0 : juce::jlimit(0.0, 1.0, task->progress.load()));
                    result->setProperty("cancellation_requested", cancelled);
                    result->setProperty("file_written", written);
                    if (written) {
                        result->setProperty(
                            "directory_sync",
                            directorySync == carta::DirectorySyncStatus::confirmed ? "confirmed"
                            : directorySync == carta::DirectorySyncStatus::unconfirmed
                                ? "unconfirmed"
                                : "unsupported");
                    }
                    if (added) {
                        result->setProperty(
                            "track_index", consolidate
                                               ? consolidationTrack
                                               : static_cast<int>(safe->session.tracks.size()) - 1);
                    } else {
                        result->setProperty("error", problem);
                    }
                }
            }
            safe->activeExport.reset();
            safe->menuItemsChanged();
            safe->resized();
            safe->repaint();
            safe->status.setText(
                added ? (consolidate
                             ? "Consolidated clips using " + task->target.getFullPathName() +
                                   ". Undo restores the originals."
                             : "Added muted rendered track from " + task->target.getFullPathName() +
                                   ". Unmute it when needed; originals are unchanged.")
                      : problem,
                juce::dontSendNotification);
        });
    });
    return true;
}

void Studio::confirmConsolidationRoutingChange() {
    const auto revision = editGeneration;
    const auto selected = timelineClipSelection.get(revision);
    const int track = selectedTrack;
    const int clip = selectedClip;
    const int channels = masterChannels;
    showGuardedStudioAlert(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle("Consolidate with routing changes")
            .withMessage(
                "Mixed mono and stereo clips can sound different if you later change or clear "
                "track routing. The current mono and stereo mix will still be checked before "
                "clips are replaced. Undo restores the originals. Continue?")
            .withButton("Continue")
            .withButton("Cancel")
            .withAssociatedComponent(this),
        [safe = juce::Component::SafePointer<Studio>(this), revision, selected, track, clip,
         channels](int answer) {
            if (safe == nullptr || answer != 0) {
                return;
            }
            if (safe->editGeneration != revision || safe->dragMode != kDragNone ||
                safe->masterChannels != channels || safe->selectedTrack != track ||
                safe->selectedClip != clip ||
                !sameClipReferences(selected,
                                    safe->timelineClipSelection.get(safe->editGeneration))) {
                safe->status.setText("Project or selection changed; choose consolidation again.",
                                     juce::dontSendNotification);
                return;
            }
            safe->chooseSelectedTrackRender(RenderScope::consolidate, true);
        });
}

void Studio::chooseSelectedTrackRender(RenderScope scope, bool allowRoutingChange) {
    const bool pair = scope == RenderScope::stereoPair;
    const bool fullTracks = scope == RenderScope::tracks || pair;
    const bool consolidate = scope == RenderScope::consolidate;
    const bool clips = scope == RenderScope::clips || consolidate;

    if ((!fullTracks && !clips && !timeSelection.hasRange()) ||
        (clips && timelineClipSelection.get(editGeneration).empty() && !validSelection()) ||
        activeExport || !pendingImports.empty()) {
        status.setText("Select audio and wait for exports and imports to finish.",
                       juce::dontSendNotification);
        return;
    }

    const auto revision = editGeneration;
    const auto selection = timeSelection;
    const auto rows = selectedTimelineTracks();
    const auto rawClips = timelineClipSelection.get(editGeneration);
    const auto channels = masterChannels;
    const int focus = selectedTrack;
    const int focusClip = selectedClip;
    if (pair && !(rows.size() == 2 || (rows.empty() && selection.hasRange() &&
                                       selection.lastTrack == selection.firstTrack + 1))) {
        status.setText(
            "Select exactly two mono tracks; the upper becomes left and the lower right.",
            juce::dontSendNotification);
        return;
    }

    launchStudioChooser(
        pair          ? "Render mono tracks as stereo (upper = left)"
        : consolidate ? "Consolidate clips to a new WAV"
        : clips       ? "Render selected clips to a new WAV"
        : fullTracks  ? "Render selected tracks to a new WAV"
                      : "Render selected time to a new WAV",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory)
            .getChildFile("Carta render.wav"),
        "*.wav", juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<Studio>(this), revision, selection, rows, rawClips,
         channels, focus, focusClip, fullTracks, clips, scope,
         allowRoutingChange](const StudioFileSelection& dialog) {
            if (safe == nullptr) {
                return;
            }
            const auto file = dialog.getResult();
            if (file == juce::File{}) {
                return;
            }
            const auto current = safe->timeSelection;
            const bool timeChanged =
                !clips && !fullTracks &&
                (current.start != selection.start || current.end != selection.end ||
                 current.firstTrack != selection.firstTrack ||
                 current.lastTrack != selection.lastTrack);
            const bool rangeRowsChanged =
                fullTracks && rows.empty() && selection.hasRange() &&
                (!current.hasRange() || current.firstTrack != selection.firstTrack ||
                 current.lastTrack != selection.lastTrack);
            const bool tracksChanged =
                !clips && (safe->selectedTimelineTracks() != rows || rangeRowsChanged ||
                           (fullTracks && rows.empty() && !selection.hasRange() &&
                            (current.hasRange() || safe->selectedTrack != focus)));
            const bool clipsChanged =
                clips && (!sameClipReferences(
                              rawClips, safe->timelineClipSelection.get(safe->editGeneration)) ||
                          (rawClips.empty() &&
                           (safe->selectedTrack != focus || safe->selectedClip != focusClip)));
            if (safe->editGeneration != revision || safe->masterChannels != channels ||
                timeChanged || tracksChanged || clipsChanged) {
                safe->status.setText(
                    "Project or selection changed; choose the render destination again.",
                    juce::dontSendNotification);
                return;
            }
            juce::String error;
            if (!safe->startSelectedTrackRender(file, false, error, scope, {},
                                                allowRoutingChange)) {
                safe->status.setText(error, juce::dontSendNotification);
            }
        });
}

// ============================================================================
// Level, Quiet-Gap, Source-Silence & Zero-Crossing Background Analysis
// ============================================================================

bool Studio::ipcAnalysisStale() const {
    if (editGeneration != lastIpcAnalysisGeneration || masterChannels != lastIpcAnalysisChannels) {
        return true;
    }
    if (lastIpcAnalysisScope == "zero-crossings" &&
        static_cast<bool>(lastIpcAnalysis["stale_on_completion"])) {
        return true;
    }
    if (lastIpcAnalysisScope != "selection" && lastIpcAnalysisScope != "tracks" &&
        lastIpcAnalysisScope != "zero-crossings") {
        return false;
    }
    if (!timeSelection.hasRange() || timeSelection.start != lastIpcAnalysisSelection.start ||
        timeSelection.end != lastIpcAnalysisSelection.end) {
        return true;
    }
    if (lastIpcAnalysisScope == "tracks" || lastIpcAnalysisScope == "zero-crossings") {
        return timeSelection.firstTrack != lastIpcAnalysisSelection.firstTrack ||
               timeSelection.lastTrack != lastIpcAnalysisSelection.lastTrack ||
               selectedTimelineTracks() != lastIpcAnalysisTracks ||
               (lastIpcAnalysisScope == "zero-crossings" &&
                !sameClipReferences(timelineClipSelection.get(editGeneration),
                                    lastIpcAnalysisClips));
    }
    return false;
}

bool Studio::startIpcLevelAnalysis(const juce::String& scope, juce::int64 index,
                                   juce::String& error) {
    auto task = std::make_shared<LevelAnalysisTask>();
    carta::WavRange range;
    carta::TimeSelection captured;
    std::vector<int> independent;
    std::vector<int> actualTracks;
    int channels = 0;
    {
        const juce::ScopedLock guard(lock);
        task->generation = editGeneration;
        channels = masterChannels;
        captured = timeSelection;
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (analysisSnapshotHook) {
            analysisSnapshotHook();
        }
#endif
        task->snapshot = session;
        if (scope == "selection" || scope == "tracks") {
            if (!captured.hasRange() ||
                captured.lastTrack >= static_cast<int>(session.tracks.size())) {
                error = "Select a valid time range first.";
                return false;
            }
            range = {captured.start, captured.end};
            if (scope == "tracks") {
                independent = selectedTimelineTracks();
                actualTracks = independent;
                if (actualTracks.empty()) {
                    for (int track = captured.firstTrack; track <= captured.lastTrack; ++track) {
                        actualTracks.push_back(track);
                    }
                }
                if (!carta::limitToTrackIndices(task->snapshot, actualTracks)) {
                    error = "Selected tracks are no longer valid.";
                    return false;
                }
            }
        } else if (scope == "track") {
            std::vector<carta::MasterTrack> tracks;
            if (!carta::getMasterTracks(session, tracks, error)) {
                return false;
            }
            if (index < 0 || index >= static_cast<juce::int64>(tracks.size())) {
                error = "Invalid master track index.";
                return false;
            }
            range = tracks[static_cast<size_t>(index)].range;
        } else {
            range = carta::fullWavRange(session);
        }
    }

    if (!carta::validateMixRange(task->snapshot, channels, range, error)) {
        return false;
    }
    task->ipc = true;
    task->progressLabel = "Levels";
    activeLevelAnalysis = task;
    lastIpcAnalysisGeneration = task->generation;
    lastIpcAnalysisChannels = channels;
    lastIpcAnalysisScope = scope;
    lastIpcAnalysisSelection = captured;
    lastIpcAnalysisTracks = independent;
    lastIpcSuggestions.clear();
    lastIpcSuggestionCount = 0;

    auto* info = new juce::DynamicObject();
    info->setProperty("kind", "levels");
    info->setProperty("scope", scope);
    info->setProperty("state", "running");
    info->setProperty("progress", 0.0);
    info->setProperty("cancellation_requested", false);
    info->setProperty("stale", false);
    info->setProperty("channels", channels);
    info->setProperty("start_sample", range.start);
    info->setProperty("end_sample", range.end);
    if (scope == "track") {
        info->setProperty("track_index", index);
    }
    if (scope == "tracks") {
        juce::Array<juce::var> rows;
        for (int track : actualTracks) {
            rows.add(track);
        }
        info->setProperty("track_indices", juce::var(rows));
    }
    lastIpcAnalysis = juce::var(info);
    status.setText("Analyzing mix levels...", juce::dontSendNotification);
    menuItemsChanged();
    resized();

    levelPool.addJob([safe = juce::Component::SafePointer<Studio>(this), task, channels, range] {
        carta::MixLevels levels;
        juce::String failure;
        bool ok = false;
        try {
            ok = carta::scanMixLevels(task->snapshot, channels, range, levels, failure,
                                      [task](double progress) {
                                          task->progress.store(progress);
                                          return !task->cancelled.load();
                                      });
        } catch (const std::exception& e) {
            failure = "Level analysis failed: " + juce::String(e.what());
        } catch (...) {
            failure = "Level analysis failed.";
        }

        juce::MessageManager::callAsync([safe, task, channels, levels, failure, ok] {
            if (safe == nullptr || safe->activeLevelAnalysis != task) {
                return;
            }
            const bool cancelled = task->cancelled.load();
            safe->activeLevelAnalysis.reset();
            safe->menuItemsChanged();
            safe->resized();
            if (auto* result = safe->lastIpcAnalysis.getDynamicObject()) {
                result->setProperty("state", cancelled ? "cancelled" : ok ? "complete" : "failed");
                result->setProperty(
                    "progress",
                    ok && !cancelled ? 1.0 : juce::jlimit(0.0, 1.0, task->progress.load()));
                result->setProperty("cancellation_requested", cancelled);
                result->setProperty("stale", safe->ipcAnalysisStale());
                if (ok && !cancelled) {
                    juce::Array<juce::var> outputs;
                    for (int channel = 0; channel < channels; ++channel) {
                        auto* output = new juce::DynamicObject();
                        output->setProperty(
                            "channel", channels == 1 ? "mono" : (channel == 0 ? "left" : "right"));
                        output->setProperty("peak", static_cast<double>(levels.peak[channel]));
                        output->setProperty("peak_sample",
                                            levels.peakSample[channel] < 0
                                                ? juce::var()
                                                : juce::var(levels.peakSample[channel]));
                        output->setProperty("rms", levels.rms[channel]);
                        output->setProperty("above_full_scale", levels.aboveFullScale[channel]);
                        output->setProperty("first_above_full_scale",
                                            levels.firstAboveFullScale[channel] < 0
                                                ? juce::var()
                                                : juce::var(levels.firstAboveFullScale[channel]));
                        outputs.add(juce::var(output));
                    }
                    result->setProperty("outputs", juce::var(outputs));
                } else {
                    result->setProperty(
                        "error", cancelled ? juce::String("Level analysis cancelled.") : failure);
                }
            }
            safe->status.setText(cancelled ? "Level analysis cancelled."
                                 : ok      ? "Level analysis complete."
                                           : failure,
                                 juce::dontSendNotification);
        });
    });
    return true;
}

bool Studio::selectQuietRegion(juce::int64 start, juce::int64 end, juce::uint64 revision,
                               int channels, juce::String& error) {
    const juce::ScopedLock guard(lock);
    error.clear();
    if (editGeneration != revision || masterChannels != channels || dragMode != kDragNone) {
        error = "Project changed; scan quiet gaps again.";
        return false;
    }
    if (session.tracks.empty() || start < 0 || end <= start ||
        end > carta::projectEndSample(session)) {
        error = "The quiet gap is no longer valid.";
        return false;
    }

    const carta::TimeSelection range{start, end, 0, static_cast<int>(session.tracks.size()) - 1};
    const bool keepPlayback = selectionPlaybackContinues();
    stopSpecialAudition();
    if (!keepPlayback) {
        playing = false;
        auditionActive = false;
        cutPreviewActive = false;
    }
    if (!carta::activateTimeSelection(timeSelection, timelineClipSelection, range,
                                      static_cast<int>(session.tracks.size()))) {
        error = "The quiet gap could not be selected.";
        return false;
    }
    timelineTracks.clear();
    selectedTrack = 0;
    selectedClip = selectedMarker = -1;
    keyboardRange.active = false;
    keyboardTracks.active = false;
    const auto message = "Selected quiet master-mix gap: samples " + juce::String(start) + " to " +
                         juce::String(end) + " (end exclusive), all tracks.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    revealTrack(0);
    menuItemsChanged();
    repaint();
    return true;
}

bool Studio::selectClipSilenceRegion(juce::int64 start, juce::int64 end, int track,
                                     juce::uint64 revision, int channels, juce::String& error) {
    const juce::ScopedLock guard(lock);
    error.clear();
    if (editGeneration != revision || masterChannels != channels || dragMode != kDragNone) {
        error = "Project changed; scan source silence again.";
        return false;
    }
    if (track < 0 || track >= static_cast<int>(session.tracks.size()) || start < 0 ||
        end <= start || end > carta::projectEndSample(session)) {
        error = "The source-silence range is no longer valid.";
        return false;
    }

    const carta::TimeSelection range{start, end, track, track};
    const bool keepPlayback = selectionPlaybackContinues();
    stopSpecialAudition();
    if (!keepPlayback) {
        playing = false;
        auditionActive = false;
        cutPreviewActive = false;
    }
    if (!carta::activateTimeSelection(timeSelection, timelineClipSelection, range,
                                      static_cast<int>(session.tracks.size()))) {
        error = "The source-silence range could not be selected.";
        return false;
    }
    timelineTracks.clear();
    selectedTrack = track;
    selectedClip = selectedMarker = -1;
    keyboardRange.active = false;
    keyboardTracks.active = false;
    const auto message = "Selected source silence on track " + juce::String(track + 1) +
                         ", samples " + juce::String(start) + " to " + juce::String(end) +
                         " (end exclusive). Range edits include overlapping clips.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    revealTrack(track);
    menuItemsChanged();
    repaint();
    return true;
}

bool Studio::detachScannedSilences(int track, int clip,
                                   const std::vector<carta::QuietRegion>& ranges,
                                   juce::uint64 revision, int channels, juce::String& error) {
    const juce::ScopedLock guard(lock);
    error.clear();
    if (editGeneration != revision || masterChannels != channels || dragMode != kDragNone) {
        error = "Project changed; scan source silence again.";
        return false;
    }
    if (ranges.empty()) {
        error = "No gaps have a conservative detach interior.";
        return false;
    }

    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            return carta::detachClipRanges(candidate, track, clip, ranges, error);
        },
        [&] {
            selectedTrack = track;
            selectedClip = selectedMarker = -1;
            timelineClipSelection.clear();
            keyboardRange.active = false;
            keyboardTracks.active = false;
        },
        "Detach clip silence", error);
    if (!changed) {
        return false;
    }
    if (timelineTrackRevision == revision) {
        timelineTrackRevision = editGeneration;
    }
    const auto message =
        "Detached " + juce::String(ranges.size()) +
        (ranges.size() == 1 ? " silent clip interior." : " silent clip interiors.") +
        " Other clips and markers are unchanged.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    revealTrack(track);
    menuItemsChanged();
    repaint();
    return true;
}

bool Studio::detachScannedSilence(int track, int clip, juce::int64 start, juce::int64 end,
                                  juce::uint64 revision, int channels, juce::String& error) {
    if (end <= start) {
        error = "This gap has no conservative detach interior.";
        return false;
    }
    return detachScannedSilences(track, clip, {{start, end}}, revision, channels, error);
}

void Studio::configureClipSilenceSearch() {
    if (!validSelection() || activeLevelAnalysis || !pendingImports.empty() || activeOpen) {
        status.setText("Focus a clip and finish the current analysis or import first.",
                       juce::dontSendNotification);
        return;
    }
    const int track = selectedTrack;
    const int clip = selectedClip;
    const auto revision = editGeneration;
    showTextPrompt(
        "Find silence in clip",
        "Search every source channel for digital silence. Selecting a result selects time on this "
        "track, including overlapping clips.",
        "duration", "Minimum duration (seconds)", "0.1", "Search",
        [safe = juce::Component::SafePointer<Studio>(this), revision, track,
         clip](bool accepted, const juce::String& value) {
            if (safe == nullptr || !accepted) {
                return;
            }
            double duration = 0;
            if (!parseNumber(value, duration) || duration < 1.0 / carta::sessionRate ||
                duration > 60) {
                safe->status.setText("Minimum duration must be one sample to 60 seconds.",
                                     juce::dontSendNotification);
                return;
            }
            if (safe->editGeneration != revision || safe->selectedTrack != track ||
                safe->selectedClip != clip) {
                safe->status.setText("Project or focused clip changed; start the search again.",
                                     juce::dontSendNotification);
                return;
            }
            juce::String error;
            if (!safe->startClipSilenceAnalysis(
                    track, clip,
                    static_cast<juce::int64>(std::llround(duration * carta::sessionRate)), false,
                    error)) {
                safe->status.setText(error, juce::dontSendNotification);
            }
        });
}

bool Studio::startClipSilenceAnalysis(int track, int clip, juce::int64 minimum, bool ipc,
                                      juce::String& error) {
    if (activeLevelAnalysis || !pendingImports.empty() || activeOpen || dragMode != kDragNone ||
        track < 0 || track >= static_cast<int>(session.tracks.size()) || clip < 0 ||
        clip >= static_cast<int>(session.tracks[static_cast<size_t>(track)].clips.size()) ||
        minimum < 1 || minimum > 60 * 44100LL) {
        error = "The clip or analysis settings are no longer valid.";
        return false;
    }

    auto task = std::make_shared<LevelAnalysisTask>();
    int channels = 0;
    {
        const juce::ScopedLock guard(lock);
        task->generation = editGeneration;
        channels = masterChannels;
        const juce::ScopedUnlock unlock(lock);
        task->snapshot = session;
    }
    task->ipc = ipc;
    task->progressLabel = "Source silence";
    activeLevelAnalysis = task;
    if (ipc) {
        lastIpcAnalysisGeneration = task->generation;
        lastIpcAnalysisChannels = channels;
        lastIpcAnalysisScope = "source-silence";
        lastIpcSuggestions.clear();
        lastIpcSuggestionCount = 0;
        auto* info = new juce::DynamicObject();
        info->setProperty("kind", "source-silence");
        info->setProperty("state", "running");
        info->setProperty("progress", 0.0);
        info->setProperty("stale", false);
        info->setProperty("track", track);
        info->setProperty("clip", clip);
        info->setProperty("minimum_samples", minimum);
        lastIpcAnalysis = juce::var(info);
    }

    status.setText("Searching source channels for silence...", juce::dontSendNotification);
    menuItemsChanged();
    resized();

    levelPool.addJob([safe = juce::Component::SafePointer<Studio>(this), task, track, clip, minimum,
                      channels, ipc] {
        std::vector<carta::QuietRegion> regions;
        std::vector<carta::QuietRegion> safeRegions;
        juce::String failure;
        bool ok = false;
        try {
            const auto& target =
                task->snapshot.tracks[static_cast<size_t>(track)].clips[static_cast<size_t>(clip)];
            ok = carta::findClipSourceSilence(target, minimum, regions, failure,
                                              [task](double progress) {
                                                  task->progress.store(progress * 0.5);
                                                  return !task->cancelled.load();
                                              });
            if (ok && !regions.empty()) {
                ok = carta::findClipSourceSilence(
                    target, 1, safeRegions, failure,
                    [task](double progress) {
                        task->progress.store(0.5 + progress * 0.5);
                        return !task->cancelled.load();
                    },
                    true);
            }
        } catch (const std::exception& e) {
            failure = "Source-silence search failed: " + juce::String(e.what());
        } catch (...) {
            failure = "Source-silence search failed.";
        }

        juce::MessageManager::callAsync([safe, task, foundRegions = std::move(regions),
                                         foundSafe = std::move(safeRegions), failure, ok, track,
                                         clip, channels, ipc] {
            if (safe == nullptr || safe->activeLevelAnalysis != task) {
                return;
            }
            const bool cancelled = task->cancelled.load();
            const bool stale =
                safe->editGeneration != task->generation || safe->masterChannels != channels;
            safe->activeLevelAnalysis.reset();
            safe->menuItemsChanged();
            safe->resized();
            if (ipc) {
                if (auto* result = safe->lastIpcAnalysis.getDynamicObject()) {
                    result->setProperty("state", cancelled ? "cancelled"
                                                 : ok      ? "complete"
                                                           : "failed");
                    result->setProperty(
                        "progress",
                        ok && !cancelled ? 1.0 : juce::jlimit(0.0, 1.0, task->progress.load()));
                    result->setProperty("cancellation_requested", cancelled);
                    result->setProperty("stale", stale);
                    if (ok && !cancelled) {
                        juce::Array<juce::var> gaps;
                        for (const auto& region : foundRegions) {
                            auto* gap = new juce::DynamicObject();
                            gap->setProperty("start_sample", region.start);
                            gap->setProperty("end_sample", region.end);
                            const auto interior = detachableSilence(region, foundSafe);
                            if (interior.end > interior.start) {
                                gap->setProperty("detach_start_sample", interior.start);
                                gap->setProperty("detach_end_sample", interior.end);
                            }
                            gaps.add(juce::var(gap));
                        }
                        result->setProperty("gaps", juce::var(gaps));
                    } else {
                        result->setProperty(
                            "error",
                            cancelled ? juce::String("Source-silence search cancelled.") : failure);
                    }
                }
            }
            safe->status.setText(cancelled ? "Source-silence search cancelled."
                                 : !ok     ? failure
                                 : stale   ? "Project changed; scan source silence again."
                                           : "Source-silence search complete.",
                                 juce::dontSendNotification);
            if (!ipc && ok && !cancelled && !stale) {
                safe->showClipSilenceList(foundRegions, foundSafe, track, clip, task->generation,
                                          channels);
            }
        });
    });
    return true;
}

void Studio::showClipSilenceList(const std::vector<carta::QuietRegion>& regions,
                                 const std::vector<carta::QuietRegion>& safeRegions, int track,
                                 int clip, juce::uint64 revision, int channels) {
    if (regions.empty()) {
        status.setText("No source silence matched the minimum duration.",
                       juce::dontSendNotification);
        return;
    }

    std::vector<QuietGapList::Row> rows;
    for (const auto& region : regions) {
        rows.push_back({region, false, detachableSilence(region, safeRegions)});
    }

    const auto activate = [safe = juce::Component::SafePointer<Studio>(this), rows, track, clip,
                           revision, channels](int row, QuietGapAction action) -> juce::String {
        if (safe == nullptr || (action != QuietGapAction::detachAll &&
                                (row < 0 || row >= static_cast<int>(rows.size())))) {
            return "The source-silence range is no longer available.";
        }
        const juce::ScopedLock guard(safe->lock);
        if (safe->editGeneration != revision || safe->masterChannels != channels ||
            safe->dragMode != kDragNone || track >= static_cast<int>(safe->session.tracks.size()) ||
            clip >=
                static_cast<int>(safe->session.tracks[static_cast<size_t>(track)].clips.size())) {
            return "Project changed; scan source silence again.";
        }
        if (action == QuietGapAction::detachAll) {
            std::vector<carta::QuietRegion> interiors;
            for (const auto& entry : rows) {
                if (entry.detachable.end > entry.detachable.start) {
                    interiors.push_back(entry.detachable);
                }
            }
            juce::String problem;
            const juce::ScopedUnlock unlock(safe->lock);
            return safe->detachScannedSilences(track, clip, interiors, revision, channels, problem)
                       ? juce::String()
                       : problem;
        }
        const auto gap = rows[static_cast<size_t>(row)].range;
        if (action == QuietGapAction::detach) {
            const auto interior = rows[static_cast<size_t>(row)].detachable;
            juce::String problem;
            const juce::ScopedUnlock unlock(safe->lock);
            return safe->detachScannedSilence(track, clip, interior.start, interior.end, revision,
                                              channels, problem)
                       ? juce::String()
                       : problem;
        }
        if (action != QuietGapAction::select) {
            const juce::ScopedUnlock unlock(safe->lock);
            if (!safe->requireAudioOutput() ||
                !safe->startBoundaryAudition(quietGapAuditionPoint(gap, action))) {
                return safe->status.getText();
            }
            return {};
        }
        juce::String problem;
        return safe->selectClipSilenceRegion(gap.start, gap.end, track, revision, channels, problem)
                   ? juce::String()
                   : problem;
    };

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Detected source silence";
    auto* content = new QuietGapList(std::move(rows), activate, {}, true);
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.resizable = true;
    options.useNativeTitleBar = true;
    options.dialogBackgroundColour = juce::Desktop::getInstance().isDarkModeActive()
                                         ? juce::Colour(0xff252525)
                                         : juce::Colour(0xffeeeeee);
    options.launchAsync();
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<QuietGapList>(content)] {
        if (safe != nullptr) {
            safe->focusList();
        }
    });
}

juce::String Studio::selectQuietSuggestion(const juce::String& line,
                                           const std::vector<carta::QuietRegion>& gaps,
                                           juce::uint64 revision, int channels) {
    const juce::ScopedLock guard(lock);
    if (editGeneration != revision || masterChannels != channels || dragMode != kDragNone) {
        return "Project changed; scan quiet gaps again.";
    }
    juce::int64 sample = 0;
    juce::String name;
    juce::String problem;
    if (!carta::parseQuietMarkerSuggestion(line, carta::projectEndSample(session), sample, name,
                                           problem)) {
        return problem;
    }
    const auto region = std::find_if(gaps.begin(), gaps.end(), [sample](const auto& gap) {
        return sample >= gap.start && sample < gap.end;
    });
    if (region == gaps.end()) {
        return "The line's sample is not inside a detected quiet gap.";
    }
    return selectQuietRegion(region->start, region->end, revision, channels, problem)
               ? juce::String()
               : problem;
}

void Studio::showQuietGapList(const std::vector<QuietGapList::Row>& rows, juce::uint64 revision,
                              int channels, std::function<void()> selected) {
    if (rows.empty()) {
        status.setText("No internal quiet gaps matched the settings.", juce::dontSendNotification);
        return;
    }
    {
        const juce::ScopedLock guard(lock);
        if (editGeneration != revision || masterChannels != channels || dragMode != kDragNone) {
            status.setText("Project changed; scan quiet gaps again.", juce::dontSendNotification);
            return;
        }
    }

    const auto activate = [safe = juce::Component::SafePointer<Studio>(this), rows, revision,
                           channels](int row, QuietGapAction action) -> juce::String {
        if (safe == nullptr || row < 0 || row >= static_cast<int>(rows.size())) {
            return "The quiet gap is no longer available.";
        }
        const juce::ScopedLock guard(safe->lock);
        if (safe->editGeneration != revision || safe->masterChannels != channels ||
            safe->dragMode != kDragNone) {
            return "Project changed; scan quiet gaps again.";
        }
        const auto& gap = rows[static_cast<size_t>(row)].range;
        if (action != QuietGapAction::select) {
            const auto point = quietGapAuditionPoint(gap, action);
            const juce::ScopedUnlock unlock(safe->lock);
            if (!safe->requireAudioOutput() || !safe->startBoundaryAudition(point)) {
                return safe->status.getText();
            }
            return {};
        }
        juce::String problem;
        return safe->selectQuietRegion(gap.start, gap.end, revision, channels, problem)
                   ? juce::String()
                   : problem;
    };

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Detected master-mix gaps";
    auto* content = new QuietGapList(rows, activate, std::move(selected));
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.resizable = true;
    options.useNativeTitleBar = true;
    options.dialogBackgroundColour = juce::Desktop::getInstance().isDarkModeActive()
                                         ? juce::Colour(0xff252525)
                                         : juce::Colour(0xffeeeeee);
    options.launchAsync();
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<QuietGapList>(content)] {
        if (safe != nullptr) {
            safe->focusList();
        }
    });
}

bool Studio::startIpcQuietAnalysis(double threshold, juce::int64 minimum, juce::String& error) {
    auto task = std::make_shared<LevelAnalysisTask>();
    int channels = 0;
    {
        const juce::ScopedLock guard(lock);
        task->generation = editGeneration;
        channels = masterChannels;
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (analysisSnapshotHook) {
            analysisSnapshotHook();
        }
#endif
        task->snapshot = session;
    }

    if (!carta::validateMixRange(task->snapshot, channels, carta::fullWavRange(task->snapshot),
                                 error)) {
        return false;
    }

    task->ipc = true;
    task->progressLabel = "Quiet gaps";
    activeLevelAnalysis = task;
    lastIpcAnalysisGeneration = task->generation;
    lastIpcAnalysisChannels = channels;
    lastIpcAnalysisScope = "quiet";
    lastIpcSuggestions.clear();
    lastIpcSuggestionCount = 0;

    auto* info = new juce::DynamicObject();
    info->setProperty("kind", "quiet");
    info->setProperty("state", "running");
    info->setProperty("progress", 0.0);
    info->setProperty("cancellation_requested", false);
    info->setProperty("stale", false);
    info->setProperty("threshold_db", threshold);
    info->setProperty("minimum_samples", minimum);
    info->setProperty("channels", channels);
    lastIpcAnalysis = juce::var(info);

    status.setText("Searching master mix for quiet gaps...", juce::dontSendNotification);
    menuItemsChanged();
    resized();

    levelPool.addJob([safe = juce::Component::SafePointer<Studio>(this), task, threshold, minimum,
                      channels] {
        std::vector<carta::QuietRegion> regions;
        juce::String failure;
        bool ok = false;
        juce::StringArray lines;
        std::vector<bool> occupied;
        try {
            ok = carta::findQuietRegions(task->snapshot, channels, threshold, minimum, regions,
                                         failure, [task](double progress) {
                                             task->progress.store(progress);
                                             return !task->cancelled.load();
                                         });
        } catch (const std::exception& e) {
            failure = "Quiet-gap search failed: " + juce::String(e.what());
        } catch (...) {
            failure = "Quiet-gap search failed.";
        }

        if (ok) {
            try {
                ok = carta::quietMarkerSuggestions(task->snapshot, regions, lines, occupied,
                                                   failure);
            } catch (const std::exception& e) {
                ok = false;
                failure = "Preparing suggestions failed: " + juce::String(e.what());
            } catch (...) {
                ok = false;
                failure = "Preparing suggestions failed.";
            }
        }

        juce::MessageManager::callAsync([safe, task, foundRegions = std::move(regions),
                                         suggestionLines = std::move(lines),
                                         markerFlags = std::move(occupied), failure, ok, channels] {
            if (safe == nullptr || safe->activeLevelAnalysis != task) {
                return;
            }
            const bool cancelled = task->cancelled.load();
            safe->activeLevelAnalysis.reset();
            safe->menuItemsChanged();
            safe->resized();
            if (auto* result = safe->lastIpcAnalysis.getDynamicObject()) {
                result->setProperty("state", cancelled ? "cancelled" : ok ? "complete" : "failed");
                result->setProperty(
                    "progress",
                    ok && !cancelled ? 1.0 : juce::jlimit(0.0, 1.0, task->progress.load()));
                result->setProperty("cancellation_requested", cancelled);
                result->setProperty("stale", safe->editGeneration != task->generation ||
                                                 safe->masterChannels != channels);
                if (ok && !cancelled) {
                    juce::Array<juce::var> gaps;
                    for (size_t i = 0; i < foundRegions.size(); ++i) {
                        const auto& region = foundRegions[i];
                        auto* gap = new juce::DynamicObject();
                        gap->setProperty("start_sample", region.start);
                        gap->setProperty("end_sample", region.end);
                        const auto midpoint = region.start + (region.end - region.start) / 2;
                        gap->setProperty("suggested_sample", midpoint);
                        gap->setProperty("suggested_name", carta::quietMarkerName(midpoint));
                        gap->setProperty("existing_marker", static_cast<bool>(markerFlags[i]));
                        gaps.add(juce::var(gap));
                    }
                    result->setProperty("gaps", juce::var(gaps));
                    safe->lastIpcSuggestions = suggestionLines.joinIntoString("\n");
                    safe->lastIpcSuggestionCount = suggestionLines.size();
                    result->setProperty("suggestion_count", safe->lastIpcSuggestionCount);
                } else {
                    result->setProperty(
                        "error", cancelled ? juce::String("Quiet-gap search cancelled.") : failure);
                }
            }
            safe->status.setText(cancelled ? "Quiet-gap search cancelled."
                                 : ok      ? "Quiet-gap analysis complete."
                                           : failure,
                                 juce::dontSendNotification);
        });
    });
    return true;
}

void Studio::startQuietGapSearch(double threshold, juce::int64 duration, juce::uint64 revision,
                                 int channels) {
    if (activeLevelAnalysis || !pendingImports.empty()) {
        status.setText("Wait for audio analysis and imports to finish.",
                       juce::dontSendNotification);
        return;
    }

    auto task = std::make_shared<LevelAnalysisTask>();
    {
        const juce::ScopedLock guard(lock);
        if (editGeneration != revision || masterChannels != channels || dragMode != kDragNone) {
            status.setText("Project changed; reopen the quiet-gap search.",
                           juce::dontSendNotification);
            return;
        }
        task->generation = revision;
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (analysisSnapshotHook) {
            analysisSnapshotHook();
        }
#endif
        task->snapshot = session;
    }

    task->progressLabel = "Quiet gaps";
    activeLevelAnalysis = task;
    status.setText("Searching master mix for quiet gaps...", juce::dontSendNotification);
    menuItemsChanged();
    resized();

    levelPool.addJob([safe = juce::Component::SafePointer<Studio>(this), task, threshold, duration,
                      channels] {
        std::vector<carta::QuietRegion> regions;
        juce::String error;
        bool ok = false;
        try {
            ok = carta::findQuietRegions(task->snapshot, channels, threshold, duration, regions,
                                         error, [task](double p) {
                                             task->progress.store(p);
                                             return !task->cancelled.load();
                                         });
        } catch (const std::exception& e) {
            error = "Quiet-gap search failed: " + juce::String(e.what());
        } catch (...) {
            error = "Quiet-gap search failed.";
        }

        juce::MessageManager::callAsync([safe, task, foundRegions = std::move(regions), error, ok,
                                         channels]() mutable {
            if (safe == nullptr || safe->activeLevelAnalysis != task) {
                return;
            }
            safe->activeLevelAnalysis.reset();
            safe->resized();
            safe->menuItemsChanged();
            if (!ok || task->cancelled.load()) {
                safe->status.setText(task->cancelled.load() ? "Quiet-gap search cancelled." : error,
                                     juce::dontSendNotification);
                return;
            }
            {
                const juce::ScopedLock guard(safe->lock);
                if (safe->editGeneration != task->generation || safe->masterChannels != channels ||
                    safe->dragMode != kDragNone) {
                    safe->status.setText("Project changed; run the quiet-gap search again.",
                                         juce::dontSendNotification);
                    return;
                }
            }
            juce::StringArray lines;
            std::vector<bool> occupied;
            juce::String suggestionError;
            if (!carta::quietMarkerSuggestions(task->snapshot, foundRegions, lines, occupied,
                                               suggestionError)) {
                safe->status.setText(suggestionError, juce::dontSendNotification);
                return;
            }
            const auto skipped =
                static_cast<int>(std::count(occupied.begin(), occupied.end(), true));
            if (foundRegions.empty()) {
                safe->status.setText("No internal quiet gaps matched the settings.",
                                     juce::dontSendNotification);
                return;
            }

            std::vector<QuietGapList::Row> gapRows;
            gapRows.reserve(foundRegions.size());
            for (size_t i = 0; i < foundRegions.size(); ++i) {
                gapRows.push_back({foundRegions[i], occupied[i]});
            }
            const auto browse = [safe, gapRows, expectedRevision = task->generation,
                                 channels](std::function<void()> selected) {
                if (safe != nullptr) {
                    safe->showQuietGapList(gapRows, expectedRevision, channels,
                                           std::move(selected));
                }
            };
            if (lines.isEmpty()) {
                safe->status.setText(
                    "All detected quiet gaps already contain markers. Browse the gaps to "
                    "audition or select them.",
                    juce::dontSendNotification);
                browse({});
                return;
            }

            const auto accept = [safe, expectedRevision = task->generation,
                                 channels](const juce::String& text) -> juce::String {
                if (safe == nullptr) {
                    return "The editor has closed.";
                }
                juce::String message;
                const bool added = safe->mutateMarkers(
                    [&](carta::Session& candidate) {
                        if (safe->editGeneration != expectedRevision ||
                            safe->masterChannels != channels || safe->dragMode != kDragNone) {
                            message =
                                "Project changed; close this window and run the search again.";
                            return false;
                        }
                        return carta::addSuggestedMarkers(candidate, text, message);
                    },
                    [] {}, "Add suggested markers", message);
                if (!added) {
                    return message.isNotEmpty() ? message : juce::String("No markers were added.");
                }
                safe->status.setText("Added suggested track markers; undo removes them together.",
                                     juce::dontSendNotification);
                return {};
            };

            const auto audition = [safe, expectedRevision = task->generation,
                                   channels](const juce::String& line) -> juce::String {
                if (safe == nullptr) {
                    return "The editor has closed.";
                }
                const juce::ScopedLock guard(safe->lock);
                if (safe->editGeneration != expectedRevision || safe->masterChannels != channels ||
                    safe->dragMode != kDragNone) {
                    return "Project changed; close this window and run the search again.";
                }
                const juce::ScopedUnlock unlock(safe->lock);
                juce::int64 sample = 0;
                juce::String name;
                juce::String parseError;
                if (!carta::parseQuietMarkerSuggestion(line, carta::projectEndSample(safe->session),
                                                       sample, name, parseError)) {
                    return parseError;
                }
                if (!safe->requireAudioOutput() || !safe->startBoundaryAudition(sample)) {
                    return safe->status.getText();
                }
                return {};
            };

            const auto select =
                [safe, expectedRevision = task->generation, channels,
                 detectedGaps = std::move(foundRegions)](const juce::String& line) -> juce::String {
                if (safe == nullptr) {
                    return "The editor has closed.";
                }
                return safe->selectQuietSuggestion(line, detectedGaps, expectedRevision, channels);
            };

            juce::DialogWindow::LaunchOptions options;
            options.dialogTitle = "Review suggested markers";
            auto* content = new QuietMarkerSuggestions(lines.joinIntoString("\n"), accept, audition,
                                                       select, browse);
            options.content.setOwned(content);
            options.componentToCentreAround = safe.getComponent();
            options.resizable = true;
            options.useNativeTitleBar = true;
            options.dialogBackgroundColour = juce::Desktop::getInstance().isDarkModeActive()
                                                 ? juce::Colour(0xff252525)
                                                 : juce::Colour(0xffeeeeee);
            options.launchAsync();
            safe->status.setText("Review " + juce::String(lines.size()) + " suggested markers; " +
                                     juce::String(skipped) +
                                     " detected gaps already contain markers.",
                                 juce::dontSendNotification);
            juce::MessageManager::callAsync(
                [safeContent = juce::Component::SafePointer<QuietMarkerSuggestions>(content)] {
                    if (safeContent != nullptr) {
                        safeContent->focusEditor();
                    }
                });
        });
    });
}

bool Studio::findSelectionZeroCrossings(bool requireAllChannels, bool ipc) {
    if (activeLevelAnalysis) {
        status.setText("Another audio analysis is running.", juce::dontSendNotification);
        return false;
    }
    if (!pendingImports.empty()) {
        status.setText("Wait for imports to finish or cancel them before searching.",
                       juce::dontSendNotification);
        return false;
    }

    auto task = std::make_shared<LevelAnalysisTask>();
    carta::TimeSelection original;
    std::vector<carta::ClipReference> originalGroup;
    std::vector<int> originalTracks;
    int channels = 0;
    {
        const juce::ScopedLock guard(lock);
        if (!timeSelection.hasRange() ||
            timeSelection.lastTrack >= static_cast<int>(session.tracks.size())) {
            status.setText("Select a valid time range first.", juce::dontSendNotification);
            return false;
        }
        original = timeSelection;
        originalGroup = timelineClipSelection.get(editGeneration);
        originalTracks = selectedTimelineTracks();
        channels = masterChannels;
        task->generation = editGeneration;
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (analysisSnapshotHook) {
            analysisSnapshotHook();
        }
#endif
        task->snapshot = session;
    }

    if (!(originalTracks.empty()
              ? carta::limitToTracks(task->snapshot, original.firstTrack, original.lastTrack)
              : carta::limitToTrackIndices(task->snapshot, originalTracks))) {
        status.setText("Selected tracks are no longer valid.", juce::dontSendNotification);
        return false;
    }

    task->ipc = ipc;
    if (ipc) {
        lastIpcAnalysisGeneration = task->generation;
        lastIpcAnalysisChannels = channels;
        lastIpcAnalysisScope = "zero-crossings";
        lastIpcAnalysisSelection = original;
        lastIpcAnalysisTracks = originalTracks;
        lastIpcAnalysisClips = originalGroup;
        auto* info = new juce::DynamicObject();
        info->setProperty("kind", "zero-crossings");
        info->setProperty("state", "running");
        info->setProperty("mode", requireAllChannels ? "all" : "any");
        info->setProperty("radius_samples", 441);
        info->setProperty("progress", 0.0);
        info->setProperty("stale", false);
        info->setProperty("cancellation_requested", false);
        lastIpcAnalysis = juce::var(info);
    }

    task->progressLabel = "Zero crossings";
    activeLevelAnalysis = task;
    status.setText("Searching selected-track mix for zero crossings...",
                   juce::dontSendNotification);
    menuItemsChanged();
    resized();

    levelPool.addJob([safe = juce::Component::SafePointer<Studio>(this), task, original,
                      originalGroup, originalTracks, channels, requireAllChannels] {
        carta::TimeSelection result;
        juce::String error;
        bool ok = false;
        try {
            ok = carta::snapSelectionToZeroCrossings(
                task->snapshot, channels, original, 441, result, error,
                [task](double p) {
                    task->progress.store(p);
                    return !task->cancelled.load();
                },
                requireAllChannels);
        } catch (const std::exception& e) {
            error = "Zero-crossing search failed: " + juce::String(e.what());
        } catch (...) {
            error = "Zero-crossing search failed.";
        }

        juce::MessageManager::callAsync([safe, task, original, originalGroup, originalTracks,
                                         result, error, ok, channels] {
            if (safe == nullptr || safe->activeLevelAnalysis != task) {
                return;
            }
            safe->activeLevelAnalysis.reset();
            safe->resized();
            safe->menuItemsChanged();
            auto finish = [&](const juce::String& state, const juce::String& message,
                              bool stale = false) {
                if (task->ipc) {
                    if (auto* info = safe->lastIpcAnalysis.getDynamicObject()) {
                        info->setProperty("state", state);
                        info->setProperty("error", message);
                        info->setProperty("stale_on_completion", stale);
                        info->setProperty("cancellation_requested", task->cancelled.load());
                    }
                }
                safe->status.setText(message, juce::dontSendNotification);
            };
            if (!ok || task->cancelled.load()) {
                finish(task->cancelled.load() ? "cancelled" : "failed",
                       task->cancelled.load() ? "Zero-crossing search cancelled." : error);
                return;
            }
            const juce::ScopedLock guard(safe->lock);
            const auto current = safe->timeSelection;
            const auto& group = safe->timelineClipSelection.get(task->generation);
            const bool sameGroup = group.size() == originalGroup.size() &&
                                   std::equal(group.begin(), group.end(), originalGroup.begin(),
                                              [](const auto& a, const auto& b) {
                                                  return a.track == b.track && a.clip == b.clip;
                                              });
            if (safe->editGeneration != task->generation || safe->masterChannels != channels ||
                safe->dragMode != kDragNone || !sameGroup ||
                safe->selectedTimelineTracks() != originalTracks ||
                current.start != original.start || current.end != original.end ||
                current.firstTrack != original.firstTrack ||
                current.lastTrack != original.lastTrack) {
                finish("failed",
                       "Project or selection changed; run the zero-crossing search again.", true);
                return;
            }
            if (!carta::activateTimeSelection(safe->timeSelection, safe->timelineClipSelection,
                                              result,
                                              static_cast<int>(safe->session.tracks.size()))) {
                finish("failed", "Selection is no longer valid.", true);
                return;
            }
            if (task->ipc) {
                safe->lastIpcAnalysisSelection = result;
                safe->lastIpcAnalysisClips.clear();
                if (auto* info = safe->lastIpcAnalysis.getDynamicObject()) {
                    info->setProperty("state", "complete");
                    info->setProperty("progress", 1.0);
                    info->setProperty("start_sample", result.start);
                    info->setProperty("end_sample", result.end);
                }
            }
            safe->keyboardRange.active = false;
            safe->keyboardTracks.active = false;
            safe->status.setText("Zero-crossing selection: samples " + juce::String(result.start) +
                                     " to " + juce::String(result.end) + " (end exclusive).",
                                 juce::dontSendNotification);
            safe->announceNavigation(safe->status.getText());
            safe->menuItemsChanged();
            safe->repaint();
        });
    });
    return true;
}

bool Studio::locateLevelSample(juce::int64 sample, juce::uint64 revision, int channels,
                               const juce::String& label, juce::String& error) {
    error.clear();
    if (sample < 0 || sample >= carta::projectEndSample(session)) {
        error = "The analyzed sample is no longer within the master.";
        return false;
    }
    {
        const juce::ScopedLock guard(lock);
        if (editGeneration != revision || masterChannels != channels || dragMode != kDragNone) {
            error = "The project changed; run level analysis again.";
            return false;
        }
        stopSpecialAudition();
        playing = false;
        auditionActive = false;
        cutPreviewActive = false;
        position = static_cast<double>(sample) / carta::sessionRate;
        selectedMarker = -1;
    }
    const double destination = static_cast<double>(sample) / carta::sessionRate;
    if (destination < viewStart || destination >= viewStart + viewLength()) {
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()),
                                 destination - viewLength() * 0.5);
    }
    status.setText(label + " at project sample " + juce::String(sample) + ".",
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    repaint();
    return true;
}

bool Studio::levelScopeMatches(int scope, const carta::TimeSelection& captured,
                               const std::vector<int>& tracks) const {
    if (scope != LevelScope::selectionFull && scope != LevelScope::selectionTracks) {
        return true;
    }
    if (!timeSelection.hasRange() || timeSelection.start != captured.start ||
        timeSelection.end != captured.end) {
        return false;
    }
    return scope != LevelScope::selectionTracks ||
           (timeSelection.firstTrack == captured.firstTrack &&
            timeSelection.lastTrack == captured.lastTrack && selectedTimelineTracks() == tracks);
}

void Studio::analyzeLevels(int scope) {
    if (activeLevelAnalysis) {
        status.setText("Level analysis is already running.", juce::dontSendNotification);
        return;
    }
    if (!pendingImports.empty()) {
        status.setText("Wait for imports to finish or cancel them before analyzing levels.",
                       juce::dontSendNotification);
        return;
    }

    carta::Session snapshot;
    carta::WavRange range;
    carta::TimeSelection capturedSelection;
    std::vector<int> capturedTracks;
    int channels = 0;
    juce::uint64 generation = 0;
    juce::String label;
    juce::String error;
    {
        const juce::ScopedLock guard(lock);
        if ((scope == LevelScope::selectionFull || scope == LevelScope::selectionTracks) &&
            !timeSelection.hasRange()) {
            status.setText("Select a time range first.", juce::dontSendNotification);
            return;
        }
        channels = masterChannels;
        generation = editGeneration;
        capturedSelection = timeSelection;
        if (scope == LevelScope::selectionTracks) {
            capturedTracks = selectedTimelineTracks();
        }
        const double capturedPosition = position;
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (analysisSnapshotHook) {
            analysisSnapshotHook();
        }
#endif
        snapshot = session;
        if (scope == LevelScope::selectionFull || scope == LevelScope::selectionTracks) {
            range = {capturedSelection.start, capturedSelection.end};
            label = scope == LevelScope::selectionFull
                        ? "Full mix over selection"
                        : "Selected tracks " + juce::String(capturedSelection.firstTrack + 1) +
                              utf8Str("–") + juce::String(capturedSelection.lastTrack + 1) +
                              " over selection";
            if (scope == LevelScope::selectionTracks) {
                if (!capturedTracks.empty()) {
                    juce::StringArray numbers;
                    for (int track : capturedTracks) {
                        numbers.add(juce::String(track + 1));
                    }
                    label =
                        "Independent tracks " + numbers.joinIntoString(", ") + " over selection";
                }
                if (!(capturedTracks.empty()
                          ? carta::limitToTracks(snapshot, capturedSelection.firstTrack,
                                                 capturedSelection.lastTrack)
                          : carta::limitToTrackIndices(snapshot, capturedTracks))) {
                    status.setText("Selected tracks are no longer valid.",
                                   juce::dontSendNotification);
                    return;
                }
            }
        } else if (scope == LevelScope::trackAtPlayhead) {
            const double frame = capturedPosition * carta::sessionRate;
            carta::MasterTrack track;
            int index = 0;
            if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0 ||
                !carta::masterTrackAtSample(snapshot,
                                            static_cast<juce::int64>(std::floor(frame + 1.0e-7)),
                                            track, index, error)) {
                status.setText(error.isEmpty() ? "Place the playhead within the master." : error,
                               juce::dontSendNotification);
                return;
            }
            range = track.range;
            label = "Master track " + juce::String(index + 1) + ": " + track.name;
        } else {
            range = carta::fullWavRange(session);
            label = "Master mix";
        }
    }

    if (!carta::validateMixRange(snapshot, channels, range, error)) {
        status.setText(error, juce::dontSendNotification);
        return;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (analysisPreparedRangeHook) {
        analysisPreparedRangeHook(range);
    }
#endif
    auto task = std::make_shared<LevelAnalysisTask>();
    task->generation = generation;
    task->snapshot = std::move(snapshot);
    activeLevelAnalysis = task;
    status.setText("Analyzing snapshot: " + label + ".", juce::dontSendNotification);
    menuItemsChanged();
    resized();

    levelPool.addJob([safe = juce::Component::SafePointer<Studio>(this), task, channels, range,
                      label, scope, capturedSelection, capturedTracks] {
        carta::MixLevels levels;
        juce::String scanError;
        bool ok = false;
        try {
            ok = carta::scanMixLevels(task->snapshot, channels, range, levels, scanError,
                                      [task](double p) {
                                          task->progress.store(p);
                                          return !task->cancelled.load();
                                      });
        } catch (const std::exception& e) {
            scanError = "Level analysis failed: " + juce::String(e.what());
        } catch (...) {
            scanError = "Level analysis failed.";
        }

        juce::MessageManager::callAsync([safe, task, levels, scanError, ok, channels, range, label,
                                         scope, capturedSelection, capturedTracks] {
            if (safe == nullptr || safe->activeLevelAnalysis != task) {
                return;
            }
            safe->activeLevelAnalysis.reset();
            safe->menuItemsChanged();
            safe->resized();
            if (!ok || task->cancelled.load()) {
                safe->status.setText(task->cancelled.load() ? "Level analysis cancelled."
                                                            : scanError,
                                     juce::dontSendNotification);
                return;
            }

            const auto db = [](double value) {
                return value > 0 ? juce::String(20.0 * std::log10(value), 2) + " dBFS"
                                 : juce::String(utf8Str("−∞ dBFS"));
            };
            juce::String text = label + utf8Str(" · samples [") + juce::String(range.start) + ", " +
                                juce::String(range.end) + ")\n";
            juce::int64 firstOver = -1;
            juce::int64 peakAt = -1;
            float maximumPeak = 0;
            for (int ch = 0; ch < channels; ++ch) {
                text += "\n" + juce::String(channels == 1 ? "Mono" : (ch == 0 ? "Left" : "Right")) +
                        ": sample peak " + db(levels.peak[ch]) +
                        (levels.peakSample[ch] >= 0
                             ? " at sample " + juce::String(levels.peakSample[ch])
                             : juce::String()) +
                        ", RMS " + db(levels.rms[ch]);
                if (levels.peakSample[ch] >= 0 &&
                    (peakAt < 0 || levels.peak[ch] > maximumPeak ||
                     (std::equal_to<float>{}(levels.peak[ch], maximumPeak) &&
                      levels.peakSample[ch] < peakAt))) {
                    peakAt = levels.peakSample[ch];
                    maximumPeak = levels.peak[ch];
                }
                if (levels.aboveFullScale[ch] > 0) {
                    text += "\n  " + juce::String(levels.aboveFullScale[ch]) +
                            " samples above 0 dBFS; first at sample " +
                            juce::String(levels.firstAboveFullScale[ch]) + ".";
                    if (firstOver < 0 || levels.firstAboveFullScale[ch] < firstOver) {
                        firstOver = levels.firstAboveFullScale[ch];
                    }
                }
            }
            text += "\n\nMeasured at 44.1 kHz before WAV quantization or dither. These are sample "
                    "peaks, not true peaks or loudness.";
            const bool stale = safe->editGeneration != task->generation ||
                               safe->masterChannels != channels ||
                               !safe->levelScopeMatches(scope, capturedSelection, capturedTracks);
            if (stale) {
                text += "\n\nThe project, master channels or analysis selection changed; these "
                        "values describe the earlier snapshot.";
            }
            safe->status.setText("Level analysis complete.", juce::dontSendNotification);
            if (peakAt < 0 || stale) {
                safe->showGuardedStudioAlert(juce::MessageBoxOptions()
                                                 .withIconType(juce::MessageBoxIconType::InfoIcon)
                                                 .withTitle("Mix levels")
                                                 .withMessage(text)
                                                 .withButton("OK")
                                                 .withAssociatedComponent(safe.getComponent()));
            } else {
                safe->showGuardedStudioAlert(
                    levelResultsOptions(safe.getComponent(), text, firstOver >= 0),
                    [safe, task, channels, firstOver, peakAt, scope, capturedSelection,
                     capturedTracks](int answer) {
                        if (safe == nullptr || (answer != 1 && (answer != 2 || firstOver < 0))) {
                            return;
                        }
                        juce::String navigationError;
                        const auto sample = answer == 2 ? firstOver : peakAt;
                        if (!safe->levelScopeMatches(scope, capturedSelection, capturedTracks)) {
                            safe->status.setText(
                                "The analysis selection changed; run level analysis again.",
                                juce::dontSendNotification);
                            return;
                        }
                        if (!safe->locateLevelSample(sample, task->generation, channels,
                                                     answer == 2 ? "First sample above 0 dBFS"
                                                                 : "Sample peak",
                                                     navigationError)) {
                            safe->status.setText(navigationError, juce::dontSendNotification);
                        }
                    });
            }
        });
    });
}

// ============================================================================
// Clip Group Actions, Navigators, Overlap, Marker & Master Track Lists
// ============================================================================

bool Studio::performTimelineClipAction(int action, juce::int64 explicitSplitSample) {
    std::vector<carta::ClipReference> clips;
    juce::uint64 generation = 0;
    {
        const juce::ScopedLock guard(lock);
        generation = editGeneration;
        clips = timelineClipSelection.get(generation);
    }
    if (clips.empty()) {
        status.setText("Select clip titles with Command/Control+Shift-click first.",
                       juce::dontSendNotification);
        return false;
    }
    if (action == TimelineClipAction::clear) {
        timelineClipSelection.clear();
        status.setText("Clip selection cleared.", juce::dontSendNotification);
        menuItemsChanged();
        repaint();
        return true;
    }
    if (action == TimelineClipAction::move) {
        moveClipGroupDialog(std::move(clips), generation, true);
        return true;
    }
    if (action == TimelineClipAction::copy) {
        const juce::ScopedLock guard(lock);
        juce::String error;
        carta::Clipboard copied;
        if (generation != editGeneration || dragMode != kDragNone) {
            status.setText("The clip selection changed.", juce::dontSendNotification);
            return false;
        }
        bool copiedSuccessfully = false;
        {
            const juce::ScopedUnlock unlock(lock);
            copiedSuccessfully = carta::copyClipGroup(session, clips, copied, error);
        }
        if (!copiedSuccessfully) {
            status.setText(error.isEmpty() ? "The clip selection changed." : error,
                           juce::dontSendNotification);
            return false;
        }
        clipboard = std::move(copied);
        bool pruned = false;
        {
            const juce::ScopedUnlock unlock(lock);
            editHistory.enforceRetainedMediaBudget(session, clipboard);
            pruned = editHistory.takeBudgetPruned();
        }
        status.setText(
            juce::String("Copied selected clips; paste at the playhead.") +
                (pruned ? " Older undo history was discarded to limit retained media." : ""),
            juce::dontSendNotification);
        return true;
    }

    const juce::ScopedLock guard(lock);
    if (generation != editGeneration || dragMode != kDragNone) {
        status.setText("The clip selection changed.", juce::dontSendNotification);
        return false;
    }

    juce::String error;
    juce::int64 splitSample = -1;
    std::vector<carta::ClipReference> duplicates;
    std::vector<carta::ClipReference> mapped;
    std::vector<carta::ClipReference> resultRefs;
    std::vector<std::vector<carta::ClipReference>> splitPieces;
    carta::Clipboard nextClipboard;
    carta::ClipSelection nextSelection;

    const auto focused = std::find_if(clips.begin(), clips.end(), [this](const auto& ref) {
        return ref.track == selectedTrack && ref.clip == selectedClip;
    });
    const auto focusedIndex = focused == clips.end()
                                  ? size_t{0}
                                  : static_cast<size_t>(std::distance(clips.begin(), focused));
    if (action == TimelineClipAction::split) {
        const auto frame = explicitSplitSample >= 0 ? static_cast<double>(explicitSplitSample)
                                                    : editPlayhead() * carta::sessionRate;
        if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
            status.setText("Invalid playhead position.", juce::dontSendNotification);
            return false;
        }
        splitSample = static_cast<juce::int64>(std::llround(frame));
    }

    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            bool ok = false;
            if (action == TimelineClipAction::cut) {
                ok = carta::cutClipGroup(candidate, clips, nextClipboard, error);
            } else if (action == TimelineClipAction::remove) {
                ok = carta::deleteClipGroup(candidate, clips, error);
            } else if (action == TimelineClipAction::duplicate) {
                ok = carta::duplicateClipGroup(candidate, clips, error, &duplicates);
            } else if (action == TimelineClipAction::split) {
                ok = carta::splitClipGroupAtSample(candidate, clips, splitSample, error,
                                                   &splitPieces);
            } else if (action == TimelineClipAction::join) {
                ok = carta::joinSelectedClipChains(candidate, clips, error, &mapped);
            }
            if (!ok || !carta::preservesMarkerBounds(session, candidate, error)) {
                return false;
            }
            if (action == TimelineClipAction::duplicate) {
                resultRefs = duplicates;
            } else if (action == TimelineClipAction::split) {
                for (const auto& group : splitPieces) {
                    for (const auto& ref : group) {
                        resultRefs.push_back(ref);
                    }
                }
            } else if (action == TimelineClipAction::join) {
                std::set<std::pair<int, int>> seen;
                for (const auto& ref : mapped) {
                    if (seen.emplace(ref.track, ref.clip).second) {
                        resultRefs.push_back(ref);
                    }
                }
            }
            if (!resultRefs.empty() &&
                !nextSelection.restore(candidate, resultRefs, generation + 1)) {
                error = "Could not select the resulting clips.";
                return false;
            }
            return true;
        },
        [&] {
            if (action == TimelineClipAction::cut) {
                clipboard = std::move(nextClipboard);
            }
            if (action == TimelineClipAction::duplicate) {
                selectedTrack = duplicates[focusedIndex].track;
                selectedClip = duplicates[focusedIndex].clip;
            } else if (action == TimelineClipAction::split) {
                selectedTrack = splitPieces[focusedIndex][0].track;
                selectedClip = splitPieces[focusedIndex][0].clip;
            } else if (action == TimelineClipAction::join) {
                selectedTrack = mapped[focusedIndex].track;
                selectedClip = mapped[focusedIndex].clip;
                selectedMarker = -1;
            } else {
                selectedClip = -1;
            }
            if (action != TimelineClipAction::join) {
                keyboardRange.active = false;
                keyboardTracks.active = false;
            }
            if (!resultRefs.empty()) {
                timelineClipSelection = std::move(nextSelection);
            }
        },
        action == TimelineClipAction::cut         ? "Cut clips"
        : action == TimelineClipAction::remove    ? "Delete clips"
        : action == TimelineClipAction::duplicate ? "Duplicate clips"
        : action == TimelineClipAction::split     ? "Split clips"
                                                  : "Join selected clips",
        error);

    if (changed && (action == TimelineClipAction::duplicate ||
                    action == TimelineClipAction::split || action == TimelineClipAction::join)) {
        if (action == TimelineClipAction::join && timelineTrackRevision == generation) {
            timelineTrackRevision = editGeneration;
        }
        revealTrack(selectedTrack);
    }
    status.setText(
        changed ? (action == TimelineClipAction::cut
                       ? juce::String("Cut selected clips; markers stayed in place.")
                   : action == TimelineClipAction::remove
                       ? juce::String("Deleted selected clips; markers stayed in place.")
                   : action == TimelineClipAction::duplicate
                       ? juce::String("Duplicated selected clips after their span; overlaps are "
                                      "mixed.")
                   : action == TimelineClipAction::join
                       ? juce::String("Joined compatible selected clips without rendering.")
                       : "Split selected clips at sample " + juce::String(splitSample) + ".")
                : error,
        juce::dontSendNotification);
    menuItemsChanged();
    return changed;
}

bool Studio::transformBrowserClipGroup(const std::vector<carta::ClipReference>& clips,
                                       juce::uint64 generation, bool isSplit) {
    const juce::ScopedLock guard(lock);
    juce::String error;
    juce::int64 splitSample = -1;
    std::vector<carta::ClipReference> duplicates;
    std::vector<carta::ClipReference> resultRefs;
    std::vector<std::vector<carta::ClipReference>> pieces;
    carta::ClipSelection nextSelection;

    if (clips.empty()) {
        status.setText("Select clip rows only.", juce::dontSendNotification);
        return false;
    }
    if (editGeneration != generation || dragMode != kDragNone) {
        status.setText("Project changed; reopen the track and clip browser.",
                       juce::dontSendNotification);
        return false;
    }

    const auto focused = std::find_if(clips.begin(), clips.end(), [this](const auto& ref) {
        return ref.track == selectedTrack && ref.clip == selectedClip;
    });
    const auto focusedIndex = focused == clips.end()
                                  ? size_t{0}
                                  : static_cast<size_t>(std::distance(clips.begin(), focused));
    if (isSplit) {
        const auto frame = editPlayhead() * carta::sessionRate;
        if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
            status.setText("Invalid playhead position.", juce::dontSendNotification);
            return false;
        }
        splitSample = static_cast<juce::int64>(std::llround(frame));
    }

    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            if (isSplit) {
                if (!carta::splitClipGroupAtSample(candidate, clips, splitSample, error, &pieces)) {
                    return false;
                }
                for (const auto& group : pieces) {
                    for (const auto& ref : group) {
                        resultRefs.push_back(ref);
                    }
                }
            } else {
                if (!carta::duplicateClipGroup(candidate, clips, error, &duplicates)) {
                    return false;
                }
                resultRefs = duplicates;
            }
            if (!carta::preservesMarkerBounds(session, candidate, error)) {
                return false;
            }
            if (!nextSelection.restore(candidate, resultRefs, generation + 1)) {
                error = "Could not select the resulting clips.";
                return false;
            }
            return true;
        },
        [&] {
            const auto focus = isSplit ? pieces[focusedIndex][0] : duplicates[focusedIndex];
            selectedTrack = focus.track;
            selectedClip = focus.clip;
            selectedMarker = -1;
            keyboardRange.active = false;
            keyboardTracks.active = false;
            timelineClipSelection = std::move(nextSelection);
        },
        isSplit ? "Split clips" : "Duplicate clips", error);

    if (changed) {
        revealTrack(selectedTrack);
        menuItemsChanged();
    }
    status.setText(changed
                       ? (isSplit ? "Split selected clips at sample " + juce::String(splitSample) +
                                        "; time selection and markers stayed in place."
                                  : juce::String("Duplicated selected clips after their span; "
                                                 "overlaps are mixed and markers stayed in place."))
                       : error,
                   juce::dontSendNotification);
    repaint();
    return changed;
}

bool Studio::moveSelectedClipGroup(const std::vector<carta::ClipReference>& clips,
                                   juce::uint64 generation, juce::int64 samples, juce::int64 tracks,
                                   bool requireTimelineSelection) {
    const juce::ScopedLock guard(lock);
    juce::String error;
    bool needsRouting = false;
    std::vector<carta::ClipReference> moved;
    carta::ClipSelection nextSelection;
    const int focusedTrack = selectedTrack;
    const int focusedClip = selectedClip;
    const auto anchor = std::find_if(clips.begin(), clips.end(), [&](const auto& ref) {
        return ref.track == focusedTrack && ref.clip == focusedClip;
    });
    const auto anchorIndex = anchor == clips.end()
                                 ? size_t{0}
                                 : static_cast<size_t>(std::distance(clips.begin(), anchor));

    if (editGeneration != generation || dragMode != kDragNone) {
        status.setText("Project changed; reopen the move dialog.", juce::dontSendNotification);
        return false;
    }
    if (requireTimelineSelection) {
        const auto& current = timelineClipSelection.get(generation);
        const bool same = current.size() == clips.size() &&
                          std::equal(current.begin(), current.end(), clips.begin(),
                                     [](const auto& a, const auto& b) {
                                         return a.track == b.track && a.clip == b.clip;
                                     });
        if (!same) {
            status.setText("Clip selection changed; reopen the move dialog.",
                           juce::dontSendNotification);
            return false;
        }
    }

    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            if (!carta::moveClipGroup(candidate, clips, samples, tracks, error, &moved)) {
                return false;
            }
            std::set<int> destinations;
            for (const auto& clip : moved) {
                destinations.insert(clip.track);
            }
            for (int index : destinations) {
                const auto& destination = candidate.tracks[static_cast<size_t>(index)];
                if (!carta::clipRoutingConfirmed(destination,
                                                 carta::maxTrackChannels(destination))) {
                    needsRouting = true;
                }
            }
            if (!nextSelection.restore(candidate, moved, generation + 1)) {
                error = "Could not select the moved clips.";
                return false;
            }
            return true;
        },
        [&] {
            selectedTrack = moved[anchorIndex].track;
            selectedClip = moved[anchorIndex].clip;
            selectedMarker = -1;
            keyboardRange.active = false;
            keyboardTracks.active = false;
            timelineClipSelection = std::move(nextSelection);
        },
        "Move clips", error);

    if (changed) {
        revealTrack(selectedTrack);
        menuItemsChanged();
    }
    status.setText(
        changed ? juce::String(
                      "Moved selected clips together; destination overlaps are mixed and markers "
                      "stayed in place.") +
                      (needsRouting ? " Confirm destination routing before export." : "")
                : error,
        juce::dontSendNotification);
    repaint();
    return changed;
}

void Studio::moveClipGroupDialog(std::vector<carta::ClipReference> clips, juce::uint64 generation,
                                 bool requireTimelineSelection) {
    if (editGeneration != generation || dragMode != kDragNone || clips.empty()) {
        status.setText("Project changed; reopen the track and clip browser.",
                       juce::dontSendNotification);
        return;
    }
    showTextFieldsPrompt(
        "Move selected clips",
        "Enter signed project-sample and track offsets. Existing overlaps sum; destination tracks "
        "must exist. Markers remain fixed.",
        {{"samples", "Sample offset", "0"}, {"tracks", "Track offset", "0"}}, "Move",
        [safe = juce::Component::SafePointer<Studio>(this), selectedClips = std::move(clips),
         generation, requireTimelineSelection](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted || values.size() != 2) {
                return;
            }
            juce::int64 samples = 0;
            juce::int64 tracks = 0;
            if (!parseInteger(values[0], samples) || !parseInteger(values[1], tracks)) {
                safe->status.setText("Enter signed integer sample and track offsets.",
                                     juce::dontSendNotification);
                return;
            }
            safe->moveSelectedClipGroup(selectedClips, generation, samples, tracks,
                                        requireTimelineSelection);
        });
}

bool Studio::expandBrowserClipTargets(std::vector<carta::ClipReference>& clips,
                                      juce::uint64 generation) {
    const juce::ScopedLock guard(lock);
    juce::String error;
    std::vector<carta::ClipReference> expanded;
    if (editGeneration != generation || dragMode != kDragNone) {
        status.setText("Project changed; reopen the track and clip browser.",
                       juce::dontSendNotification);
        return false;
    }
    bool valid = false;
    {
        const juce::ScopedUnlock unlock(lock);
        valid = carta::expandClipGroups(session, clips, expanded, error);
    }
    if (!valid) {
        status.setText(error, juce::dontSendNotification);
        return false;
    }
    clips = std::move(expanded);
    return true;
}

bool Studio::removeBrowserClipGroup(const std::vector<carta::ClipReference>& clips,
                                    juce::uint64 generation, bool cut) {
    const juce::ScopedLock guard(lock);
    juce::String error;
    carta::Clipboard nextClipboard;
    if (clips.empty() || editGeneration != generation || dragMode != kDragNone) {
        status.setText("Project changed; reopen the track and clip browser.",
                       juce::dontSendNotification);
        return false;
    }

    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            return (cut ? carta::cutClipGroup(candidate, clips, nextClipboard, error)
                        : carta::deleteClipGroup(candidate, clips, error)) &&
                   carta::preservesMarkerBounds(session, candidate, error);
        },
        [&] {
            if (cut) {
                clipboard = std::move(nextClipboard);
            } else {
                selectedTrack = clips.front().track;
                timeSelection = {};
            }
            selectedClip = -1;
            selectedMarker = -1;
            keyboardRange.active = false;
            keyboardTracks.active = false;
        },
        cut ? "Cut clips" : "Delete clips", error);

    if (changed) {
        revealTrack(selectedTrack);
    }
    status.setText(changed
                       ? (cut ? juce::String("Cut selected clips; their time and markers stayed "
                                             "in place.")
                              : juce::String("Deleted selected clips; markers stayed in place."))
                       : error,
                   juce::dontSendNotification);
    return changed;
}

void Studio::showTrackClipNavigator() {
    carta::Session snapshot;
    int track = -1;
    int clip = -1;
    juce::uint64 generation = 0;
    {
        const juce::ScopedLock guard(lock);
        track = selectedTrack;
        clip = selectedClip;
        generation = editGeneration;
        const juce::ScopedUnlock unlock(lock);
        snapshot = session;
    }

    auto rows = carta::makeNavigationRows(snapshot, &sourceChecks);
    if (rows.empty()) {
        status.setText("There are no layers to browse.", juce::dontSendNotification);
        return;
    }

    int selected = 0;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        if (rows[static_cast<size_t>(i)].track == track &&
            rows[static_cast<size_t>(i)].clip == clip) {
            selected = i;
            break;
        }
    }

    auto activate = [safe = juce::Component::SafePointer<Studio>(this), rows,
                     generation](std::vector<int> selectedRows, int action) {
        if (safe == nullptr || selectedRows.empty()) {
            return;
        }
        if (action >= 2 && action <= 7) {
            std::vector<carta::ClipReference> clips;
            clips.reserve(selectedRows.size());
            for (const int index : selectedRows) {
                if (index < 0 || index >= static_cast<int>(rows.size()) ||
                    rows[static_cast<size_t>(index)].clip < 0) {
                    safe->status.setText("Select clip rows only.", juce::dontSendNotification);
                    return;
                }
                const auto& target = rows[static_cast<size_t>(index)];
                clips.push_back({target.track, target.clip});
            }
            juce::MessageManager::callAsync([safe, selectedClips = std::move(clips), generation,
                                             action]() mutable {
                if (safe == nullptr) {
                    return;
                }
                if (!safe->expandBrowserClipTargets(selectedClips, generation)) {
                    return;
                }
                if (action == 2) {
                    safe->moveClipGroupDialog(std::move(selectedClips), generation);
                    return;
                }
                if (action == 3 || action == 5) {
                    safe->transformBrowserClipGroup(selectedClips, generation, action == 5);
                    return;
                }
                if (action == 6) {
                    const juce::ScopedLock guard(safe->lock);
                    juce::String error;
                    carta::Clipboard copied;
                    if (safe->editGeneration != generation || safe->dragMode != kDragNone) {
                        safe->status.setText("Project changed; reopen the layer and clip browser.",
                                             juce::dontSendNotification);
                        return;
                    }
                    bool valid = false;
                    {
                        const juce::ScopedUnlock unlock(safe->lock);
                        valid = carta::copyClipGroup(safe->session, selectedClips, copied, error);
                    }
                    if (!valid) {
                        safe->status.setText(error, juce::dontSendNotification);
                        return;
                    }
                    safe->clipboard = std::move(copied);
                    bool pruned = false;
                    {
                        const juce::ScopedUnlock unlock(safe->lock);
                        safe->editHistory.enforceRetainedMediaBudget(safe->session,
                                                                     safe->clipboard);
                        pruned = safe->editHistory.takeBudgetPruned();
                    }
                    safe->status.setText(
                        juce::String("Copied selected clips; paste uses the playhead and "
                                     "destination layer.") +
                            (pruned ? " Older undo history was discarded to limit retained "
                                      "media."
                                    : ""),
                        juce::dontSendNotification);
                    return;
                }
                safe->removeBrowserClipGroup(selectedClips, generation, action == 7);
            });
            return;
        }

        const int row = selectedRows.front();
        if (row < 0 || row >= static_cast<int>(rows.size())) {
            return;
        }
        const juce::ScopedLock guard(safe->lock);
        const auto& target = rows[static_cast<size_t>(row)];
        if (safe->editGeneration != generation || safe->dragMode != kDragNone || target.track < 0 ||
            target.track >= static_cast<int>(safe->session.tracks.size())) {
            safe->status.setText("Project changed; reopen the layer and clip browser.",
                                 juce::dontSendNotification);
            return;
        }
        const auto& clips = safe->session.tracks[static_cast<size_t>(target.track)].clips;
        if (target.clip >= static_cast<int>(clips.size())) {
            safe->status.setText("Clip changed; reopen the layer and clip browser.",
                                 juce::dontSendNotification);
            return;
        }
        juce::int64 first = 0;
        juce::int64 last = 0;
        if (action == 1 &&
            (target.clip < 0 ||
             !carta::clipSampleRange(clips[static_cast<size_t>(target.clip)], first, last))) {
            safe->status.setText("This clip contains no project sample positions.",
                                 juce::dontSendNotification);
            return;
        }

        const bool keepPlayback = safe->selectionPlaybackContinues();
        safe->stopSpecialAudition();
        safe->selectedTrack = target.track;
        safe->selectedClip = target.clip;
        safe->selectedMarker = -1;
        safe->timeSelection = {};
        safe->keyboardRange.active = false;
        safe->keyboardTracks.active = false;
        safe->revealTrack(target.track);
        if (target.clip >= 0) {
            const auto& item = clips[static_cast<size_t>(target.clip)];
            const double clipPosition = carta::clipSampleRange(item, first, last)
                                            ? static_cast<double>(first) / carta::sessionRate
                                            : item.start;
            if (action == 1) {
                carta::activateTimeSelection(safe->timeSelection, safe->timelineClipSelection,
                                             {first, last, target.track, target.track},
                                             static_cast<int>(safe->session.tracks.size()));
                safe->timelineTracks.clear();
            }
            if (!keepPlayback) {
                safe->position = clipPosition;
                safe->playing = false;
                safe->auditionActive = false;
            }
            if (clipPosition < safe->viewStart ||
                clipPosition >= safe->viewStart + safe->viewLength()) {
                safe->viewStart =
                    juce::jlimit(0.0, std::max(0.0, safe->fullLength() - safe->viewLength()),
                                 clipPosition - safe->viewLength() * 0.5);
                safe->suspendFollowingForNavigation();
            }
        }
        const auto message = action == 1 ? "Selected clip time: samples " + juce::String(first) +
                                               " to " + juce::String(last) +
                                               " (end exclusive); overlapping clips are included."
                                         : target.description;
        safe->status.setText(message, juce::dontSendNotification);
        safe->announceNavigation(message);
        safe->menuItemsChanged();
        safe->repaint();
        juce::MessageManager::callAsync([safe] {
            if (safe != nullptr) {
                safe->grabKeyboardFocus();
            }
        });
    };

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Layers and clips";
    auto* content = new TrackClipNavigator(std::move(rows), selected, std::move(activate));
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.resizable = true;
    options.useNativeTitleBar = true;
    options.dialogBackgroundColour = juce::Desktop::getInstance().isDarkModeActive()
                                         ? juce::Colour(0xff252525)
                                         : juce::Colour(0xffeeeeee);
    options.launchAsync();
    juce::MessageManager::callAsync(
        [safe = juce::Component::SafePointer<TrackClipNavigator>(content)] {
            if (safe != nullptr) {
                safe->focusList();
            }
        });
}

bool Studio::applyTrackMixSelection(juce::uint64 revision, const std::vector<int>& indices,
                                    int action) {
    juce::String error;
    bool valid = false;
    const auto flag = action < TrackMixOp::solo ? carta::TrackFlag::mute : carta::TrackFlag::solo;
    const bool value = action == TrackMixOp::mute || action == TrackMixOp::solo;
    const juce::String label = action == TrackMixOp::mute     ? "Mute tracks"
                               : action == TrackMixOp::unmute ? "Unmute tracks"
                               : action == TrackMixOp::solo   ? "Solo tracks"
                                                              : "Clear track solos";
    const bool changed = mutateMixState(
        [&](carta::Session& candidate) {
            if (revision != editGeneration || action < TrackMixOp::mute ||
                action > TrackMixOp::clearSolo || indices.empty()) {
                error = "Track selection changed; reopen the track list.";
                return false;
            }
            std::set<int> seen;
            for (int index : indices) {
                if (index < 0 || index >= static_cast<int>(candidate.tracks.size()) ||
                    !seen.insert(index).second) {
                    error = "Invalid track selection; reopen the track list.";
                    return false;
                }
            }
            valid = true;
            return carta::setTrackFlags(candidate, indices, flag, value);
        },
        error, label);
    if (!valid || error.isNotEmpty()) {
        status.setText(error.isEmpty() ? "Could not update tracks." : error,
                       juce::dontSendNotification);
        return false;
    }
    status.setText(changed
                       ? label + " applied to " + juce::String(static_cast<int>(indices.size())) +
                             " selected tracks."
                       : "Selected tracks were already in that state.",
                   juce::dontSendNotification);
    return true;
}

bool Studio::applyIndependentTrackRange(juce::uint64 revision, carta::TimeSelection captured,
                                        const std::vector<int>& indices, int action,
                                        bool finishStandard) {
    const juce::ScopedLock guard(lock);
    juce::String error;
    const bool retainTracks = timelineTrackRevision == revision && !timelineTracks.empty();
    const auto kind =
        action == IndependentRangeAction::cutGap      ? carta::IndependentRangeEdit::cutGap
        : action == IndependentRangeAction::cutClose  ? carta::IndependentRangeEdit::cutClose
        : action == IndependentRangeAction::deleteGap ? carta::IndependentRangeEdit::deleteGap
        : action == IndependentRangeAction::closeGap  ? carta::IndependentRangeEdit::closeGap
        : action == IndependentRangeAction::insertGap ? carta::IndependentRangeEdit::insertGap
        : action == IndependentRangeAction::splitBoundaries
            ? carta::IndependentRangeEdit::splitBoundaries
        : action == IndependentRangeAction::trimOutside ? carta::IndependentRangeEdit::trimOutside
                                                        : carta::IndependentRangeEdit::duplicate;
    const juce::String label =
        action == IndependentRangeAction::cutGap      ? "Cut time on selected tracks"
        : action == IndependentRangeAction::cutClose  ? "Cut and close time on selected tracks"
        : action == IndependentRangeAction::deleteGap ? "Delete time on selected tracks"
        : action == IndependentRangeAction::closeGap  ? "Close gap on selected tracks"
        : action == IndependentRangeAction::insertGap ? "Insert gap on selected tracks"
        : action == IndependentRangeAction::splitBoundaries
            ? "Split selected tracks at selection boundaries"
        : action == IndependentRangeAction::trimOutside ? "Trim selected tracks outside selection"
                                                        : "Duplicate time on selected tracks";
    if (dragMode != kDragNone) {
        status.setText("Finish or cancel the current drag first.", juce::dontSendNotification);
        return false;
    }

    const bool keepPlayback = selectionPlaybackContinues();
    const bool supportedAction =
        (action >= IndependentRangeAction::cutGap && action <= IndependentRangeAction::insertGap) ||
        (action >= IndependentRangeAction::splitBoundaries &&
         action <= IndependentRangeAction::duplicate);
    if (revision != editGeneration || !supportedAction || !captured.hasRange() ||
        timeSelection.start != captured.start || timeSelection.end != captured.end ||
        timeSelection.firstTrack != captured.firstTrack ||
        timeSelection.lastTrack != captured.lastTrack) {
        status.setText("Project or time selection changed; reopen the track list.",
                       juce::dontSendNotification);
        return false;
    }

    carta::Clipboard next;
    const bool changed = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            return carta::editIndependentTracks(candidate, captured, indices, kind, next, error) &&
                   carta::preservesMarkerBounds(session, candidate, error);
        },
        [&] {
            if (action == IndependentRangeAction::cutGap ||
                action == IndependentRangeAction::cutClose) {
                clipboard = std::move(next);
                timeSelection = {};
                selectedTrack = *std::min_element(indices.begin(), indices.end());
                selectedClip = -1;
                selectedMarker = -1;
                if (!keepPlayback) {
                    position = static_cast<double>(captured.start) / carta::sessionRate;
                }
                keyboardRange.active = false;
                keyboardTracks.active = false;
                timelineClipSelection.clear();
            } else if (action == IndependentRangeAction::deleteGap ||
                       action == IndependentRangeAction::closeGap ||
                       action == IndependentRangeAction::insertGap ||
                       action == IndependentRangeAction::splitBoundaries ||
                       action == IndependentRangeAction::trimOutside) {
                selectedClip = -1;
                selectedMarker = -1;
            }
            if (finishStandard && (action == IndependentRangeAction::deleteGap ||
                                   action == IndependentRangeAction::closeGap)) {
                if (action == IndependentRangeAction::closeGap && !keepPlayback) {
                    position = static_cast<double>(captured.start) / carta::sessionRate;
                }
                timeSelection.end = timeSelection.start;
            }
        },
        label, error);

    if (changed && retainTracks) {
        timelineTrackRevision = editGeneration;
        menuItemsChanged();
    }
    status.setText(changed              ? label + "; other tracks and markers remain in place."
                   : error.isNotEmpty() ? error
                                        : "No audio was changed.",
                   juce::dontSendNotification);
    return changed;
}

bool Studio::independentRangeCurrent(juce::uint64 revision,
                                     const carta::TimeSelection& captured) const {
    return dragMode == kDragNone && revision == editGeneration && captured.hasRange() &&
           timeSelection.start == captured.start && timeSelection.end == captured.end &&
           timeSelection.firstTrack == captured.firstTrack &&
           timeSelection.lastTrack == captured.lastTrack;
}

bool Studio::copyIndependentTrackRange(juce::uint64 revision, const carta::TimeSelection& captured,
                                       const std::vector<int>& indices,
                                       carta::Clipboard* prepared) {
    const juce::ScopedLock guard(lock);
    if (!independentRangeCurrent(revision, captured)) {
        status.setText("Project or time selection changed; reopen the track list.",
                       juce::dontSendNotification);
        return false;
    }
    carta::Clipboard copied;
    juce::String error;
    bool copiedSuccessfully = false;
    if (prepared != nullptr) {
        copied = std::move(*prepared);
        copiedSuccessfully = true;
    } else {
        const juce::ScopedUnlock unlock(lock);
        copiedSuccessfully =
            carta::copyIndependentTracks(session, captured, indices, copied, error);
    }
    if (!copiedSuccessfully) {
        status.setText(error, juce::dontSendNotification);
        return false;
    }
    clipboard = std::move(copied);
    bool pruned = false;
    {
        const juce::ScopedUnlock unlock(lock);
        editHistory.enforceRetainedMediaBudget(session, clipboard);
        pruned = editHistory.takeBudgetPruned();
    }
    status.setText(juce::String(clipboard.hasAudio() ? "Copied time from selected tracks."
                                                     : "Copied silent time from selected tracks.") +
                       (pruned ? " Older undo history was discarded to limit retained media." : ""),
                   juce::dontSendNotification);
    menuItemsChanged();
    return true;
}

bool Studio::previewIndependentTrackRange(juce::uint64 revision,
                                          const carta::TimeSelection& captured,
                                          const std::vector<int>& indices) {
    return startRippleAudition(RippleScope::selectedTracks, revision, captured, indices);
}

void Studio::performTimelineTrackAction(int id) {
    const auto indices = selectedTimelineTracks();
    if (indices.empty()) {
        status.setText("Select independent tracks first.", juce::dontSendNotification);
        return;
    }
    const auto revision = editGeneration;
    const auto selection = timeSelection;
    if (id >= IndependentTrackMenuId::mute && id <= IndependentTrackMenuId::clearSolo) {
        applyTrackMixSelection(revision, indices, id - IndependentTrackMenuId::mute);
    } else if (id == IndependentTrackMenuId::copyTime) {
        copyIndependentTrackRange(revision, selection, indices);
    } else if (id >= IndependentTrackMenuId::cutGap && id <= IndependentTrackMenuId::insertGap) {
        applyIndependentTrackRange(revision, selection, indices,
                                   id - IndependentTrackMenuId::cutGap +
                                       IndependentRangeAction::cutGap);
    } else if (id == IndependentTrackMenuId::previewClose) {
        previewIndependentTrackRange(revision, selection, indices);
    }
    menuItemsChanged();
    repaint();
}

void Studio::showTrackMixList(std::vector<int> selected) {
    std::vector<TrackMixList::Row> rows;
    juce::uint64 revision = 0;
    carta::TimeSelection captured;
    {
        const juce::ScopedLock guard(lock);
        revision = editGeneration;
        for (const auto& track : session.tracks) {
            rows.push_back({track.name, track.muted, track.solo});
        }
        if (selected.empty()) {
            selected = selectedTimelineTracks();
        }
        if (selected.empty() && selectedTrack >= 0 &&
            selectedTrack < static_cast<int>(rows.size())) {
            selected.push_back(selectedTrack);
        }
        captured = timeSelection;
    }
    if (rows.empty()) {
        status.setText("There are no layers.", juce::dontSendNotification);
        return;
    }

    const auto action = [safe = juce::Component::SafePointer<Studio>(this), revision,
                         captured](std::vector<int> indices, int command) {
        juce::MessageManager::callAsync([safe, revision, captured,
                                         trackIndices = std::move(indices), command] {
            if (safe == nullptr) {
                return;
            }
            if (command == IndependentRangeAction::previewClose) {
                safe->previewIndependentTrackRange(revision, captured, trackIndices);
                return;
            }
            const bool ok =
                command == IndependentRangeAction::copy
                    ? safe->copyIndependentTrackRange(revision, captured, trackIndices)
                : command < 4
                    ? safe->applyTrackMixSelection(revision, trackIndices, command)
                    : safe->applyIndependentTrackRange(revision, captured, trackIndices, command);
            if (!ok) {
                return;
            }
            safe->showTrackMixList(trackIndices);
        });
    };

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = captured.hasRange()
                              ? utf8Str("Edit or mix layers · samples ") +
                                    juce::String(captured.start) + utf8Str("–") +
                                    juce::String(captured.end) + " (exclusive)"
                              : juce::String(utf8Str("Edit or mix layers · no time selection"));
    auto* content =
        new TrackMixList(std::move(rows), std::move(selected), action, captured.hasRange());
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.resizable = true;
    options.useNativeTitleBar = true;
    options.dialogBackgroundColour = juce::Desktop::getInstance().isDarkModeActive()
                                         ? juce::Colour(0xff252525)
                                         : juce::Colour(0xffeeeeee);
    options.launchAsync();
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<TrackMixList>(content)] {
        if (safe != nullptr) {
            safe->focusList();
        }
    });
}

bool Studio::renameMasterTrack(int index, const juce::String& name, juce::uint64 generation,
                               bool& changed, juce::String& error) {
    changed = false;
    error.clear();
    if (name.trim().isEmpty() || name.containsAnyOf("\r\n")) {
        error = "Track name must be nonempty and single-line.";
        return false;
    }
    const bool applied = mutateMarkers(
        [&](carta::Session& candidate) {
            if (editGeneration != generation) {
                error = "Project changed; reopen the track list.";
                return false;
            }
            std::vector<carta::MasterTrack> tracks;
            if (!carta::getMasterTracks(candidate, tracks, error)) {
                return false;
            }
            if (index < 0 || index >= static_cast<int>(tracks.size())) {
                error = "Invalid master track index.";
                return false;
            }
            const auto& track = tracks[static_cast<size_t>(index)];
            if (track.name == name) {
                return false;
            }
            for (int markerIndex = 0; markerIndex < static_cast<int>(candidate.markers.size());
                 ++markerIndex) {
                const auto sample = static_cast<juce::int64>(
                    std::llround(candidate.markers[static_cast<size_t>(markerIndex)].position *
                                 carta::sessionRate));
                if (sample == track.range.start) {
                    return carta::renameMarkerIndices(candidate, {markerIndex}, {name}, error);
                }
            }
            if (track.range.start != 0) {
                error = "Track marker is missing; reopen the track list.";
                return false;
            }
            candidate.markers.push_back({0, name});
            return true;
        },
        [] {}, "Rename master track", error);
    changed = applied;
    return applied || error.isEmpty();
}

void Studio::renameMasterTrackDialog(int index, juce::uint64 generation,
                                     const juce::String& currentName) {
    if (editGeneration != generation || dragMode != kDragNone) {
        status.setText("Project changed; reopen the track list.", juce::dontSendNotification);
        return;
    }
    showTextPrompt("Rename master track", "Enter a name for this marker-defined track.", "name",
                   "Name", currentName, "Rename",
                   [safe = juce::Component::SafePointer<Studio>(this), index,
                    generation](bool accepted, const juce::String& value) {
                       if (safe == nullptr || !accepted) {
                           return;
                       }
                       juce::String error;
                       bool changed = false;
                       if (!safe->renameMasterTrack(index, value, generation, changed, error)) {
                           safe->status.setText(error, juce::dontSendNotification);
                       } else {
                           safe->status.setText(changed ? "Master track renamed."
                                                        : "Master track name unchanged.",
                                                juce::dontSendNotification);
                       }
                   });
}

void Studio::showMasterTracks() {
    std::vector<carta::MasterTrack> tracks;
    juce::String error;
    int selected = 0;
    juce::uint64 generation = 0;
    double capturedPosition = 0;
    {
        const juce::ScopedLock guard(lock);
        capturedPosition = position;
        generation = editGeneration;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif
    if (!carta::getMasterTracks(session, tracks, error)) {
        status.setText(error + " Use Markers > Marker list and repair.",
                       juce::dontSendNotification);
        return;
    }

    const double capturedFrame = capturedPosition * carta::sessionRate;
    if (std::isfinite(capturedFrame) && capturedFrame >= 0 && capturedFrame < 9007199254740992.0) {
        const auto at = static_cast<juce::int64>(std::floor(capturedFrame + 1.0e-7));
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (at >= tracks[i].range.start && at < tracks[i].range.end) {
                selected = static_cast<int>(i);
                break;
            }
        }
    }

    const auto activate = [safe = juce::Component::SafePointer<Studio>(this), tracks,
                           generation](int row, int action) {
        if (safe == nullptr || row < 0 || row >= static_cast<int>(tracks.size())) {
            return;
        }
        if (safe->editGeneration != generation || safe->dragMode != kDragNone) {
            safe->status.setText("Project changed; reopen the track list.",
                                 juce::dontSendNotification);
            return;
        }
        if (action == 5 || action == 6) {
            juce::String selectionError;
            if (!safe->selectMasterTrackIndex(row, action == 6, generation, selectionError)) {
                safe->status.setText(selectionError, juce::dontSendNotification);
            }
            return;
        }
        if (action == 4) {
            const auto name = tracks[static_cast<size_t>(row)].name;
            juce::MessageManager::callAsync([safe, row, generation, name] {
                if (safe != nullptr) {
                    safe->renameMasterTrackDialog(row, generation, name);
                }
            });
            return;
        }
        if (action == 3) {
            juce::String zoomError;
            if (!safe->zoomToMasterTrack(row, zoomError)) {
                safe->status.setText(zoomError, juce::dontSendNotification);
            }
            return;
        }
        if (action != 0 && !safe->requireAudioOutput()) {
            return;
        }

        const juce::ScopedLock guard(safe->lock);
        const auto sample = tracks[static_cast<size_t>(row)].range.start;
        safe->playing = false;
        safe->position = static_cast<double>(sample) / carta::sessionRate;
        safe->auditionActive = false;
        safe->cutPreviewActive = false;
        safe->selectedMarker = -1;
        for (int i = 0; i < static_cast<int>(safe->session.markers.size()); ++i) {
            const auto markerSample =
                safe->session.markers[static_cast<size_t>(i)].position * carta::sessionRate;
            if (std::isfinite(markerSample) &&
                std::floor(markerSample + 0.5) == static_cast<double>(sample)) {
                safe->selectedMarker = i;
                break;
            }
        }
        if (safe->position < safe->viewStart ||
            safe->position >= safe->viewStart + safe->viewLength()) {
            safe->viewStart =
                juce::jlimit(0.0, std::max(0.0, safe->fullLength() - safe->viewLength()),
                             safe->position - safe->viewLength() * 0.5);
        }
        safe->status.setText("Track " + juce::String(row + 1) + ": " +
                                 tracks[static_cast<size_t>(row)].name + ", start sample " +
                                 juce::String(sample) + ".",
                             juce::dontSendNotification);
        if (action == 1) {
            const juce::ScopedUnlock unlock(safe->lock);
            safe->auditionBoundary(AuditionBoundaryCommand::playhead);
        } else if (action == 2) {
            const auto range = tracks[static_cast<size_t>(row)].range;
            safe->setAuditionRange({static_cast<double>(range.start) / carta::sessionRate,
                                    static_cast<double>(range.end) / carta::sessionRate, false});
            safe->groupAuditionActive = false;
            safe->auditionActive = true;
            safe->playing = true;
            safe->status.setText("Playing master track " + juce::String(row + 1) + ": " +
                                     tracks[static_cast<size_t>(row)].name + ".",
                                 juce::dontSendNotification);
        }
        safe->repaint();
    };

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Master tracks";
    auto* content = new MasterTrackList(tracks, selected, activate);
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.resizable = true;
    options.useNativeTitleBar = true;
    options.dialogBackgroundColour = juce::Desktop::getInstance().isDarkModeActive()
                                         ? juce::Colour(0xff252525)
                                         : juce::Colour(0xffeeeeee);
    options.launchAsync();
    juce::MessageManager::callAsync(
        [safe = juce::Component::SafePointer<MasterTrackList>(content)] {
            if (safe != nullptr) {
                safe->focusList();
            }
        });
}

void Studio::showOverlapList() {
    std::vector<OverlapList::Row> rows;
    juce::uint64 revision = 0;
    int selected = 0;
    carta::Session snapshot;
    juce::int64 sample = 0;
    {
        const juce::ScopedLock guard(lock);
        revision = editGeneration;
        snapshot = session;
        const double frame = position * carta::sessionRate;
        if (std::isfinite(frame) && frame >= 0 && frame < 9007199254740992.0) {
            sample = static_cast<juce::int64>(std::llround(frame));
        }
    }

    const auto regions = carta::overlapRegions(snapshot);
    bool found = false;
    for (const auto& region : regions) {
        if (!found && region.end > sample) {
            selected = static_cast<int>(rows.size());
            found = true;
        }
        auto name = snapshot.tracks[static_cast<size_t>(region.track)].name;
        rows.push_back({region, name.trim().isEmpty() ? juce::String("unnamed") : name});
    }
    if (rows.empty()) {
        status.setText("No same-layer clip overlaps contain project samples.",
                       juce::dontSendNotification);
        return;
    }

    auto activate = [safe = juce::Component::SafePointer<Studio>(this), rows, revision](int row) {
        if (safe == nullptr || row < 0 || row >= static_cast<int>(rows.size())) {
            return;
        }
        const juce::ScopedLock guard(safe->lock);
        if (safe->editGeneration != revision || safe->dragMode != kDragNone) {
            safe->status.setText("Project changed; reopen the overlap list.",
                                 juce::dontSendNotification);
            return;
        }
        const auto& target = rows[static_cast<size_t>(row)].range;
        if (target.track < 0 || target.track >= static_cast<int>(safe->session.tracks.size())) {
            return;
        }
        safe->stopSpecialAudition();
        safe->playing = false;
        safe->auditionActive = false;
        safe->cutPreviewActive = false;
        safe->selectedTrack = target.track;
        safe->selectedClip = -1;
        safe->selectedMarker = -1;
        carta::activateTimeSelection(safe->timeSelection, safe->timelineClipSelection,
                                     {target.start, target.end, target.track, target.track},
                                     static_cast<int>(safe->session.tracks.size()));
        safe->timelineTracks.clear();
        safe->keyboardRange.active = false;
        safe->keyboardTracks.active = false;
        safe->position = static_cast<double>(target.start) / carta::sessionRate;
        safe->revealTrack(target.track);
        if (safe->position < safe->viewStart ||
            safe->position >= safe->viewStart + safe->viewLength()) {
            safe->viewStart =
                juce::jlimit(0.0, std::max(0.0, safe->fullLength() - safe->viewLength()),
                             safe->position - safe->viewLength() * 0.5);
        }
        safe->status.setText("Selected overlap on layer " + juce::String(target.track + 1) +
                                 ", samples " + juce::String(target.start) + " to " +
                                 juce::String(target.end) +
                                 " (end exclusive). Signals sum when audible.",
                             juce::dontSendNotification);
        safe->announceNavigation(safe->status.getText());
        safe->menuItemsChanged();
        safe->repaint();
    };

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Clip overlaps";
    auto* content = new OverlapList(rows, selected, activate);
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.resizable = true;
    options.useNativeTitleBar = true;
    options.dialogBackgroundColour = juce::Desktop::getInstance().isDarkModeActive()
                                         ? juce::Colour(0xff252525)
                                         : juce::Colour(0xffeeeeee);
    options.launchAsync();
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<OverlapList>(content)] {
        if (safe != nullptr) {
            safe->focusList();
        }
    });
}

void Studio::performMarkerListAction(juce::uint64 generation, int markerIndex, int action, int row,
                                     bool reopen) {
    {
        const juce::ScopedLock guard(lock);
        if (editGeneration != generation || dragMode != kDragNone ||
            selectedMarker != markerIndex || markerIndex < 0 ||
            markerIndex >= static_cast<int>(session.markers.size())) {
            status.setText("Marker changed; reopen the marker list.", juce::dontSendNotification);
            return;
        }
    }
    if (action == 1) {
        renameMarker(reopen);
    } else if (action == 2) {
        editMarkerPosition(reopen);
    } else if (action == 3) {
        deleteMarkerListRows(generation, {markerIndex}, row, reopen);
    }
}

void Studio::deleteMarkerListRows(juce::uint64 generation, const std::vector<int>& indices,
                                  int anchorRow, bool reopen) {
    juce::String error;
    const bool deleted = mutateMarkers(
        [&](carta::Session& candidate) {
            if (editGeneration != generation || indices.empty() ||
                selectedMarker != indices.front()) {
                error = "Marker selection changed; reopen the marker list.";
                return false;
            }
            return carta::deleteMarkers(candidate, indices, error);
        },
        [&] { selectedMarker = -1; }, indices.size() == 1 ? "Delete marker" : "Delete markers",
        error);
    if (!deleted) {
        status.setText(error, juce::dontSendNotification);
        return;
    }

    const auto remaining = carta::inspectMarkers(session);
    if (!remaining.empty()) {
        selectedMarker =
            remaining[std::min(static_cast<size_t>(std::max(0, anchorRow)), remaining.size() - 1)]
                .index;
    }
    status.setText("Deleted " + juce::String(static_cast<int>(indices.size())) +
                       (indices.size() == 1 ? " marker." : " markers."),
                   juce::dontSendNotification);
    if (reopen) {
        reopenMarkerList();
    }
}

bool Studio::shiftMarkerListRows(juce::uint64 generation, const std::vector<int>& indices,
                                 juce::int64 delta, juce::String& error) {
    return mutateMarkers(
        [&](carta::Session& candidate) {
            if (editGeneration != generation || indices.empty() ||
                selectedMarker != indices.front()) {
                error = "Marker selection changed; reopen the marker list.";
                return false;
            }
            return carta::shiftMarkerIndices(candidate, indices, delta, error);
        },
        [] {}, "Shift selected markers", error);
}

bool Studio::renameMarkerListRows(juce::uint64 generation, const std::vector<int>& indices,
                                  const std::vector<juce::String>& names, juce::String& error) {
    return mutateMarkers(
        [&](carta::Session& candidate) {
            if (editGeneration != generation || indices.empty() ||
                selectedMarker != indices.front()) {
                error = "Marker selection changed; reopen the marker list.";
                return false;
            }
            return carta::renameMarkerIndices(candidate, indices, names, error);
        },
        [] {}, "Rename selected markers", error);
}

bool Studio::positionMarkerListRows(juce::uint64 generation, const std::vector<int>& indices,
                                    const std::vector<juce::int64>& samples, juce::String& error) {
    bool keepPlayback = false;
    {
        const juce::ScopedLock guard(lock);
        keepPlayback = selectionPlaybackContinues();
    }
    return mutateMarkers(
        [&](carta::Session& candidate) {
            if (editGeneration != generation || indices.empty() ||
                selectedMarker != indices.front()) {
                error = "Marker selection changed; reopen the marker list.";
                return false;
            }
            return carta::setMarkerSamples(candidate, indices, samples, error);
        },
        [&] {
            if (!keepPlayback) {
                position = static_cast<double>(samples.front()) / carta::sessionRate;
            }
        },
        "Move selected markers", error);
}

void Studio::positionMarkerListDialog(juce::uint64 generation, std::vector<int> indices) {
    juce::StringArray positions;
    {
        const juce::ScopedLock guard(lock);
        if (editGeneration != generation || dragMode != kDragNone || indices.empty() ||
            selectedMarker != indices.front()) {
            status.setText("Marker selection changed; reopen the marker list.",
                           juce::dontSendNotification);
            return;
        }
        const auto rows = carta::inspectMarkers(session);
        for (int index : indices) {
            auto found = std::find_if(rows.begin(), rows.end(),
                                      [index](const auto& row) { return row.index == index; });
            if (found == rows.end()) {
                status.setText("Marker selection changed; reopen the marker list.",
                               juce::dontSendNotification);
                return;
            }
            positions.add(found->validPosition ? juce::String(found->sample) : juce::String());
        }
    }

    const auto revision = std::make_shared<juce::uint64>(generation);
    const auto accept = [safe = juce::Component::SafePointer<Studio>(this), indices,
                         revision](const juce::String& text) -> juce::String {
        if (safe == nullptr) {
            return "The project is closed.";
        }
        const auto lines = juce::StringArray::fromLines(text);
        if (lines.size() != static_cast<int>(indices.size())) {
            return "Enter one position per selected marker.";
        }
        std::vector<juce::int64> samples;
        samples.reserve(indices.size());
        for (const auto& line : lines) {
            juce::int64 sample = 0;
            if (!carta::parseSampleOrTime(line, sample)) {
                return "Enter valid samples or times for every marker.";
            }
            samples.push_back(sample);
        }
        juce::String error;
        if (!safe->positionMarkerListRows(*revision, indices, samples, error)) {
            return error;
        }
        *revision = safe->editGeneration;
        safe->status.setText("Moved selected markers; undo restores their positions.",
                             juce::dontSendNotification);
        return {};
    };

    const auto closed = [safe = juce::Component::SafePointer<Studio>(this), indices, revision] {
        if (safe != nullptr) {
            safe->reopenMarkerList(indices, *revision);
        }
    };

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Set selected marker positions";
    auto* content = new MarkerNamesEditor(positions.joinIntoString("\n"),
                                          static_cast<int>(indices.size()), accept, closed, true);
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.resizable = true;
    options.useNativeTitleBar = true;
    options.dialogBackgroundColour = juce::Desktop::getInstance().isDarkModeActive()
                                         ? juce::Colour(0xff252525)
                                         : juce::Colour(0xffeeeeee);
    options.launchAsync();
    juce::MessageManager::callAsync(
        [safe = juce::Component::SafePointer<MarkerNamesEditor>(content)] {
            if (safe != nullptr) {
                safe->focusEditor();
            }
        });
}

void Studio::renameMarkerListDialog(juce::uint64 generation, std::vector<int> indices) {
    juce::StringArray names;
    {
        const juce::ScopedLock guard(lock);
        if (editGeneration != generation || dragMode != kDragNone || indices.empty() ||
            selectedMarker != indices.front()) {
            status.setText("Marker selection changed; reopen the marker list.",
                           juce::dontSendNotification);
            return;
        }
        for (int index : indices) {
            if (index < 0 || index >= static_cast<int>(session.markers.size())) {
                status.setText("Marker selection changed; reopen the marker list.",
                               juce::dontSendNotification);
                return;
            }
            const auto& name = session.markers[static_cast<size_t>(index)].name;
            if (name.containsAnyOf("\r\n")) {
                status.setText(
                    "A selected name contains a line break; rename it individually first.",
                    juce::dontSendNotification);
                reopenMarkerList(indices, generation);
                return;
            }
            names.add(name);
        }
    }

    const auto revision = std::make_shared<juce::uint64>(generation);
    const auto accept = [safe = juce::Component::SafePointer<Studio>(this), indices,
                         revision](const juce::String& text) -> juce::String {
        if (safe == nullptr) {
            return "The project is closed.";
        }
        const auto lines = juce::StringArray::fromLines(text);
        if (lines.size() != static_cast<int>(indices.size())) {
            return "Enter exactly one nonempty name per selected marker.";
        }
        std::vector<juce::String> next;
        next.reserve(indices.size());
        for (const auto& line : lines) {
            next.push_back(line);
        }
        juce::String error;
        if (!safe->renameMarkerListRows(*revision, indices, next, error)) {
            return error.isNotEmpty() ? error : "Could not rename the selected markers.";
        }
        *revision = safe->editGeneration;
        safe->status.setText("Renamed " + juce::String(static_cast<int>(indices.size())) +
                                 " markers. Undo restores their names.",
                             juce::dontSendNotification);
        return {};
    };

    const auto closed = [safe = juce::Component::SafePointer<Studio>(this), indices, revision] {
        if (safe != nullptr) {
            safe->reopenMarkerList(indices, *revision);
        }
    };

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Rename selected markers";
    auto* content = new MarkerNamesEditor(names.joinIntoString("\n"),
                                          static_cast<int>(indices.size()), accept, closed);
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.resizable = true;
    options.useNativeTitleBar = true;
    options.dialogBackgroundColour = juce::Desktop::getInstance().isDarkModeActive()
                                         ? juce::Colour(0xff252525)
                                         : juce::Colour(0xffeeeeee);
    options.launchAsync();
    juce::MessageManager::callAsync(
        [safe = juce::Component::SafePointer<MarkerNamesEditor>(content)] {
            if (safe != nullptr) {
                safe->focusEditor();
            }
        });
}

void Studio::shiftMarkerListDialog(juce::uint64 generation, std::vector<int> indices) {
    {
        const juce::ScopedLock guard(lock);
        if (editGeneration != generation || dragMode != kDragNone || indices.empty() ||
            selectedMarker != indices.front()) {
            status.setText("Marker selection changed; reopen the marker list.",
                           juce::dontSendNotification);
            return;
        }
    }
    showTextPrompt(
        "Shift selected markers",
        "Enter a signed offset in project samples. Markers cannot cross or collide with stationary "
        "markers.",
        "offset", "Sample offset", "0", "Shift",
        [safe = juce::Component::SafePointer<Studio>(this), generation,
         markerIndices = std::move(indices)](bool accepted, const juce::String& value) {
            if (safe == nullptr) {
                return;
            }
            bool keepSelection = safe->editGeneration == generation;
            if (accepted) {
                juce::int64 delta = 0;
                juce::String error;
                if (!parseInteger(value, delta)) {
                    error = "Enter an integer sample offset.";
                } else {
                    bool keepPlayback = false;
                    {
                        const juce::ScopedLock guard(safe->lock);
                        keepPlayback = safe->selectionPlaybackContinues();
                    }
                    if (safe->shiftMarkerListRows(generation, markerIndices, delta, error)) {
                        keepSelection = true;
                        if (!keepPlayback) {
                            const juce::ScopedLock guard(safe->lock);
                            safe->position =
                                safe->session.markers[static_cast<size_t>(markerIndices.front())]
                                    .position;
                        }
                        safe->status.setText(
                            "Shifted " + juce::String(static_cast<int>(markerIndices.size())) +
                                " selected markers by " + juce::String(delta) + " samples.",
                            juce::dontSendNotification);
                    }
                }
                if (error.isNotEmpty()) {
                    safe->status.setText(error, juce::dontSendNotification);
                }
            }
            safe->reopenMarkerList(keepSelection ? markerIndices : std::vector<int>{},
                                   safe->editGeneration);
        });
}

bool Studio::resolveMarkerListSelection(const std::vector<MarkerList::Row>& rows,
                                        const std::vector<int>& selectedRows, int anchorRow,
                                        std::vector<int>& indices, int& currentAnchor,
                                        juce::String& error) const {
    if (selectedRows.empty() || anchorRow < 0 || anchorRow >= static_cast<int>(rows.size())) {
        error = "Marker selection changed; reopen the marker list.";
        return false;
    }

    const auto current = carta::inspectMarkers(session);
    std::map<juce::String, int> byId;
    std::vector<carta::MarkerInspection> byIndex(session.markers.size());
    for (const auto& info : current) {
        const auto& entryMarker = session.markers[static_cast<size_t>(info.index)];
        if (entryMarker.id.isEmpty() || !byId.emplace(entryMarker.id, info.index).second) {
            error = "Marker identities are invalid; reopen the marker list.";
            return false;
        }
        byIndex[static_cast<size_t>(info.index)] = info;
    }

    std::vector<int> resolved;
    resolved.reserve(selectedRows.size());
    std::set<int> seen;
    for (int row : selectedRows) {
        if (row < 0 || row >= static_cast<int>(rows.size())) {
            error = "Marker selection changed; reopen the marker list.";
            return false;
        }
        const auto& previous = rows[static_cast<size_t>(row)];
        const auto found = byId.find(previous.id);
        if (found == byId.end() || !seen.insert(found->second).second) {
            error = "A selected marker no longer exists; reopen the marker list.";
            return false;
        }
        const auto& entryMarker = session.markers[static_cast<size_t>(found->second)];
        const auto& info = byIndex[static_cast<size_t>(found->second)];
        const auto& old = previous.info;
        if (entryMarker.name != previous.name ||
            std::memcmp(&entryMarker.position, &previous.position, sizeof(double)) != 0 ||
            info.validPosition != old.validPosition || info.outside != old.outside ||
            info.duplicate != old.duplicate || info.emptyName != old.emptyName ||
            info.offGrid != old.offGrid) {
            error = "A selected marker changed; reopen the marker list.";
            return false;
        }
        resolved.push_back(found->second);
    }

    const auto anchor = byId.find(rows[static_cast<size_t>(anchorRow)].id);
    if (anchor == byId.end()) {
        error = "The focused marker no longer exists; reopen the marker list.";
        return false;
    }
    const auto foundRow = std::find_if(current.begin(), current.end(), [&](const auto& info) {
        return info.index == anchor->second;
    });
    if (foundRow == current.end()) {
        error = "The focused marker no longer exists; reopen the marker list.";
        return false;
    }
    currentAnchor = static_cast<int>(std::distance(current.begin(), foundRow));
    indices = std::move(resolved);
    return true;
}

void Studio::showMarkerList(std::vector<int> selectedIndices) {
    std::vector<MarkerList::Row> rows;
    int selected = 0;
    const auto inspected = carta::inspectMarkers(session);
    for (const auto& info : inspected) {
        if (info.index == selectedMarker) {
            selected = static_cast<int>(rows.size());
        }
        const auto& entryMarker = session.markers[static_cast<size_t>(info.index)];
        rows.push_back({info, entryMarker.name, entryMarker.id, entryMarker.position});
    }
    if (rows.empty()) {
        status.setText("There are no markers.", juce::dontSendNotification);
        return;
    }

    const auto activate = [safe = juce::Component::SafePointer<Studio>(this),
                           rows](std::vector<int> selectedRows, int action, int row) {
        if (safe == nullptr || selectedRows.empty() || row < 0 ||
            row >= static_cast<int>(rows.size())) {
            return;
        }
        const juce::ScopedLock guard(safe->lock);
        if (safe->dragMode != kDragNone) {
            safe->status.setText("Finish the current drag first.", juce::dontSendNotification);
            return;
        }
        std::vector<int> indices;
        int currentRow = 0;
        juce::String error;
        bool resolved = false;
        {
            const juce::ScopedUnlock unlock(safe->lock);
            resolved = safe->resolveMarkerListSelection(rows, selectedRows, row, indices,
                                                        currentRow, error);
        }
        if (!resolved) {
            safe->status.setText(error, juce::dontSendNotification);
            return;
        }

        const auto generation = safe->editGeneration;
        const auto& entry = rows[static_cast<size_t>(selectedRows.front())];
        const auto end = carta::projectEndSample(safe->session);
        if (action == 6 && (!entry.info.validPosition || entry.info.outside ||
                            entry.info.sample < 0 || entry.info.sample >= end)) {
            safe->status.setText(
                "Marker is outside the master; repair its position before auditioning.",
                juce::dontSendNotification);
            return;
        }
        if (action == 6 && !safe->requireAudioOutput()) {
            return;
        }

        const bool keepPlayback = action != 6 && safe->selectionPlaybackContinues();
        safe->stopSpecialAudition();
        safe->selectedMarker = indices.front();
        const double markerPosition =
            entry.info.validPosition
                ? static_cast<double>(std::min(entry.info.sample, end)) / carta::sessionRate
                : 0;
        if (!keepPlayback) {
            safe->playing = false;
            safe->auditionActive = false;
            safe->position = markerPosition;
        }
        if (markerPosition < safe->viewStart ||
            markerPosition >= safe->viewStart + safe->viewLength()) {
            safe->viewStart =
                juce::jlimit(0.0, std::max(0.0, safe->fullLength() - safe->viewLength()),
                             markerPosition - safe->viewLength() * 0.5);
            safe->suspendFollowingForNavigation();
        }

        const auto label =
            entry.name.trim().isEmpty() ? juce::String("unnamed marker") : entry.name;
        safe->status.setText("Selected " + label +
                                 ". Use the Markers menu to rename, move or delete it.",
                             juce::dontSendNotification);
        safe->repaint();
        if (action == 6) {
            const juce::ScopedUnlock unlock(safe->lock);
            safe->auditionBoundary(AuditionBoundaryCommand::playhead);
        }
        if ((action >= 1 && action <= 5) || action == 7) {
            juce::MessageManager::callAsync([safe, generation, markerIndices = std::move(indices),
                                             action, anchor = currentRow] {
                if (safe == nullptr) {
                    return;
                }
                if (action == 3) {
                    safe->deleteMarkerListRows(generation, markerIndices, anchor);
                } else if (action == 4) {
                    safe->shiftMarkerListDialog(generation, markerIndices);
                } else if (action == 5) {
                    safe->renameMarkerListDialog(generation, markerIndices);
                } else if (action == 7) {
                    safe->positionMarkerListDialog(generation, markerIndices);
                } else {
                    safe->performMarkerListAction(generation, markerIndices.front(), action,
                                                  anchor);
                }
            });
        }
    };

    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Track markers";
    auto* content = new MarkerList(rows, selected, activate);
    options.content.setOwned(content);
    if (!selectedIndices.empty()) {
        std::set<int> wanted(selectedIndices.begin(), selectedIndices.end());
        std::vector<int> selectedRows;
        for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
            if (wanted.count(rows[static_cast<size_t>(row)].info.index) > 0) {
                selectedRows.push_back(row);
            }
        }
        content->selectRows(selectedRows);
    }
    options.componentToCentreAround = this;
    options.resizable = true;
    options.useNativeTitleBar = true;
    options.dialogBackgroundColour = juce::Desktop::getInstance().isDarkModeActive()
                                         ? juce::Colour(0xff252525)
                                         : juce::Colour(0xffeeeeee);
    options.launchAsync();
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MarkerList>(content)] {
        if (safe != nullptr) {
            safe->focusList();
        }
    });
}

// ============================================================================
// Selection, Boundary, Clip, Ripple & Crossfade Audition Previews
// ============================================================================

bool Studio::startSelectionAudition(bool scoped, bool loop, bool context) {
    const juce::ScopedLock guard(lock);
    if (dragMode != kDragNone || !timeSelection.hasRange()) {
        status.setText("Select a valid time range within the master first.",
                       juce::dontSendNotification);
        return false;
    }

    const auto revision = editGeneration;
    const auto selection = timeSelection;
    const auto before = auditionBefore;
    const auto after = auditionAfter;
    std::vector<int> independent;
    carta::Session candidate;
    carta::PlaybackRange range;
    juce::String error;
    bool valid = false;
    bool tracksUnchanged = false;
    {
        const juce::ScopedUnlock unlock(lock);
        if (scoped) {
            independent = selectedTimelineTracks();
        }
#if CARTA_STUDIO_COMPONENT_TEST
        if (selectionAuditionPreparationHook) {
            selectionAuditionPreparationHook();
        }
#endif
        const auto end = carta::projectEndSample(session);
        if (selection.lastTrack < static_cast<int>(session.tracks.size()) && selection.end <= end) {
            valid = context ? carta::selectionPreviewRange(selection.start, selection.end, before,
                                                           after, end, range)
                            : true;
            if (!context) {
                range = {static_cast<double>(selection.start) / carta::sessionRate,
                         static_cast<double>(selection.end) / carta::sessionRate, loop};
            }
            if (valid && !independent.empty()) {
                auto expanded = selection;
                expanded.start =
                    static_cast<juce::int64>(std::llround(range.start * carta::sessionRate));
                expanded.end =
                    static_cast<juce::int64>(std::llround(range.end * carta::sessionRate));
                valid = carta::prepareIndependentTrackAudition(session, expanded, independent, loop,
                                                               candidate, range, error);
            } else if (valid && scoped) {
                range.tracks = {selection.firstTrack, selection.lastTrack};
            }
        }
        tracksUnchanged = !scoped || selectedTimelineTracks() == independent;
    }

    if (!independentRangeCurrent(revision, selection) || !tracksUnchanged ||
        (context && (before != auditionBefore || after != auditionAfter))) {
        {
            const juce::ScopedUnlock unlock(lock);
            candidate = {};
        }
        status.setText("The project or selection changed; audition again.",
                       juce::dontSendNotification);
        return false;
    }
    if (!valid) {
        {
            const juce::ScopedUnlock unlock(lock);
            candidate = {};
        }
        status.setText(error.isNotEmpty() ? error
                                          : "Select a valid time range within the master first.",
                       juce::dontSendNotification);
        return false;
    }

    stopSpecialAudition();
    std::swap(cutPreviewSession, candidate);
    setAuditionRange(range);
    position = range.start;
    auditionActive = true;
    cutPreviewActive = false;
    groupAuditionActive = !independent.empty();
    independentTrackAudition = !independent.empty();
    playing = true;
    {
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (selectionAuditionDisposalHook) {
            selectionAuditionDisposalHook();
        }
#endif
        candidate = {};
    }

    status.setText(context  ? (scoped ? "Playing selected tracks with context."
                                      : "Playing the full mix with selection context.")
                   : scoped ? (loop ? "Looping selected tracks within the time selection."
                                    : "Playing selected tracks within the time selection.")
                            : (loop ? "Looping the full mix within the time selection."
                                    : "Playing the full mix within the time selection."),
                   juce::dontSendNotification);
    repaint();
    return true;
}

bool Studio::startSelectionContextAudition(bool scoped) {
    return startSelectionAudition(scoped, false, true);
}

bool Studio::startBoundaryAudition(juce::int64 point, int requiredMarker, int requiredPointTrack) {
    const auto revision = editGeneration;
    const auto before = auditionBefore;
    const auto after = auditionAfter;
    carta::PlaybackRange range;
#if CARTA_STUDIO_COMPONENT_TEST
    if (boundaryPreparationHook) {
        boundaryPreparationHook();
    }
#endif
    if (!carta::boundaryPreviewRange(point, before, after, carta::projectEndSample(session),
                                     range)) {
        status.setText("No audio time available within the audition context.",
                       juce::dontSendNotification);
        return false;
    }

    bool stale = false;
    {
        const juce::ScopedLock guard(lock);
        stale = revision != editGeneration || before != auditionBefore || after != auditionAfter ||
                dragMode != kDragNone ||
                (requiredMarker >= 0 && selectedMarker != requiredMarker) ||
                (requiredPointTrack >= 0 &&
                 (!timeSelection.hasPoint() || timeSelection.firstTrack != requiredPointTrack ||
                  timeSelection.start != point));
        if (!stale) {
            setAuditionRange(range);
            position = range.start;
            auditionActive = true;
            cutPreviewActive = false;
            groupAuditionActive = false;
            playing = true;
        }
    }
    if (stale) {
        status.setText("Project, selection or audition context changed; try again.",
                       juce::dontSendNotification);
        return false;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (boundaryPublicationHook) {
        boundaryPublicationHook();
    }
#endif
    status.setText("Auditioning the full mix around sample " + juce::String(point) + ".",
                   juce::dontSendNotification);
    repaint();
    return true;
}

bool Studio::auditionFocusedMarker() {
    if (selectedMarker < 0 || selectedMarker >= static_cast<int>(session.markers.size())) {
        status.setText("Select a marker first.", juce::dontSendNotification);
        announceNavigation(status.getText());
        return false;
    }
    const double frame =
        session.markers[static_cast<size_t>(selectedMarker)].position * carta::sessionRate;
    const auto end = carta::projectEndSample(session);
    if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0 ||
        frame >= static_cast<double>(end) || std::llround(frame) >= end) {
        status.setText("Marker is outside the master; repair its position before auditioning.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        return false;
    }
    if (!requireAudioOutput()) {
        announceNavigation(status.getText());
        return false;
    }
    const bool started =
        startBoundaryAudition(static_cast<juce::int64>(std::llround(frame)), selectedMarker);
    announceNavigation(status.getText());
    return started;
}

bool Studio::auditionEditPoint() {
    if (dragMode != kDragNone || !timeSelection.hasPoint() ||
        timeSelection.firstTrack >= static_cast<int>(session.tracks.size())) {
        status.setText("Place an edit point on a track first.", juce::dontSendNotification);
        return false;
    }
    if (!requireAudioOutput()) {
        return false;
    }
    return startBoundaryAudition(timeSelection.start, -1, timeSelection.firstTrack);
}

void Studio::auditionBoundary(int which) {
    if (!requireAudioOutput()) {
        return;
    }
    double capturedPosition = 0;
    {
        const juce::ScopedLock guard(lock);
        capturedPosition = position;
    }
    const auto end = carta::projectEndSample(session);
    if (end <= 0 || (which != AuditionBoundaryCommand::playhead && !timeSelection.hasRange())) {
        return;
    }
    const auto point =
        which == AuditionBoundaryCommand::selectionStart ? timeSelection.start
        : which == AuditionBoundaryCommand::selectionEnd
            ? timeSelection.end
            : static_cast<juce::int64>(std::llround(juce::jlimit(
                  0.0, static_cast<double>(end), capturedPosition * carta::sessionRate)));
    startBoundaryAudition(point);
}

void Studio::auditionClip(bool loop) {
    if (!requireAudioOutput()) {
        return;
    }
    const juce::ScopedLock guard(lock);
    if (!validSelection()) {
        status.setText("Select a clip first.", juce::dontSendNotification);
        return;
    }
    const auto& clip =
        session.tracks[static_cast<size_t>(selectedTrack)].clips[static_cast<size_t>(selectedClip)];
    juce::int64 start = 0;
    juce::int64 end = 0;
    if (!carta::clipSampleRange(clip, start, end)) {
        status.setText("This clip contains no project sample positions.",
                       juce::dontSendNotification);
        return;
    }
    setAuditionRange({static_cast<double>(start) / carta::sessionRate,
                      static_cast<double>(end) / carta::sessionRate,
                      loop,
                      {selectedTrack, selectedTrack},
                      selectedClip});
    position = auditionRange.start;
    auditionActive = true;
    cutPreviewActive = false;
    groupAuditionActive = false;
    playing = true;
    status.setText(
        juce::String(loop ? "Looping clip: " : "Playing clip: ") + carta::clipDisplayName(clip) +
            ". Other clips are excluded." +
            (session.tracks[static_cast<size_t>(selectedTrack)].muted ? " Track is muted." : ""),
        juce::dontSendNotification);
    repaint();
}

bool Studio::auditionClipGroup(bool loop) {
    if (!requireAudioOutput()) {
        return false;
    }
    const juce::ScopedLock guard(lock);
    if (dragMode != kDragNone) {
        status.setText("Finish or cancel the current drag first.", juce::dontSendNotification);
        return false;
    }

    const auto revision = editGeneration;
    std::vector<carta::ClipReference> clips;
    carta::Session candidate;
    carta::PlaybackRange range;
    juce::String error;
    bool valid = false;
    bool selectionUnchanged = false;
    {
        const juce::ScopedUnlock unlock(lock);
        clips = timelineClipSelection.get(revision);
#if CARTA_STUDIO_COMPONENT_TEST
        if (clipAuditionPreparationHook) {
            clipAuditionPreparationHook();
        }
#endif
        valid = carta::prepareClipGroupAudition(session, clips, loop, candidate, range, error);
        const auto& current = timelineClipSelection.get(editGeneration);
        selectionUnchanged = clips.size() == current.size() &&
                             std::equal(clips.begin(), clips.end(), current.begin(),
                                        [](const auto& a, const auto& b) {
                                            return a.track == b.track && a.clip == b.clip;
                                        });
    }

    if (revision != editGeneration || dragMode != kDragNone || !selectionUnchanged) {
        {
            const juce::ScopedUnlock unlock(lock);
            candidate = {};
        }
        status.setText("The project or clip selection changed; audition again.",
                       juce::dontSendNotification);
        return false;
    }
    if (!valid) {
        {
            const juce::ScopedUnlock unlock(lock);
            candidate = {};
        }
        status.setText(error, juce::dontSendNotification);
        return false;
    }

    std::swap(cutPreviewSession, candidate);
    setAuditionRange(range);
    position = range.start;
    cutPreviewActive = false;
    groupAuditionActive = true;
    independentTrackAudition = false;
    auditionActive = true;
    playing = true;
    {
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (clipAuditionDisposalHook) {
            clipAuditionDisposalHook();
        }
#endif
        candidate = {};
    }
    status.setText(juce::String(loop ? "Looping " : "Playing ") + juce::String(clips.size()) +
                       " selected clips; other clips are excluded.",
                   juce::dontSendNotification);
    repaint();
    return true;
}

bool Studio::prepareRippleAudition(bool allTracks, bool focusedTrack, carta::Session& candidate,
                                   carta::PlaybackRange& range, juce::String& error) {
    carta::TimeSelection scope;
    if (!resolveRippleScope(focusedTrack, scope)) {
        error = "Focus a track first.";
        return false;
    }
    return carta::prepareRipplePreview(session, scope, allTracks, auditionBefore, auditionAfter,
                                       candidate, range, error);
}

bool Studio::startRippleAudition(int scope, juce::uint64 revision,
                                 const carta::TimeSelection& captured,
                                 const std::vector<int>& indices) {
    if (!requireAudioOutput()) {
        return false;
    }
    const juce::ScopedLock guard(lock);
    if (!independentRangeCurrent(revision, captured)) {
        status.setText("Project or time selection changed; select a range and try again.",
                       juce::dontSendNotification);
        return false;
    }

    auto selection = captured;
    const int focus = selectedTrack;
    if (scope == RippleScope::focusedTrack) {
        if (focus < 0 || focus >= static_cast<int>(session.tracks.size())) {
            status.setText("Focus a track first.", juce::dontSendNotification);
            return false;
        }
        selection.firstTrack = selection.lastTrack = focus;
    }

    carta::Session candidate;
    carta::PlaybackRange range;
    juce::String error;
    bool valid = false;
    {
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (ripplePreviewPreparationHook) {
            ripplePreviewPreparationHook();
        }
#endif
        valid = scope == RippleScope::selectedTracks && !indices.empty()
                    ? carta::prepareIndependentRipplePreview(session, selection, indices,
                                                             auditionBefore, auditionAfter,
                                                             candidate, range, error)
                    : carta::prepareRipplePreview(session, selection,
                                                  scope == RippleScope::allTracks, auditionBefore,
                                                  auditionAfter, candidate, range, error);
    }

    if (!valid || !independentRangeCurrent(revision, captured) ||
        (scope == RippleScope::focusedTrack && selectedTrack != focus)) {
        status.setText(valid ? "Project or time selection changed; preview the gap again."
                       : error.isNotEmpty() ? error
                                            : "Could not preview gap closure.",
                       juce::dontSendNotification);
        return false;
    }

    stopSpecialAudition();
    std::swap(cutPreviewSession, candidate);
    setAuditionRange(range);
    cutPreviewOriginalPosition = position;
    position = range.start;
    cutPreviewActive = true;
    groupAuditionActive = false;
    auditionActive = true;
    playing = true;
    {
        const juce::ScopedUnlock unlock(lock);
        candidate = {};
    }
    status.setText(
        scope == RippleScope::allTracks
            ? "Previewing all-track gap closure; no edit applied. Playhead shows the proposed "
              "timeline."
        : scope == RippleScope::focusedTrack
            ? "Previewing focused-track gap closure; no edit applied. Playhead shows the proposed "
              "timeline."
            : "Previewing selected-track gap closure; no edit applied. Playhead shows the proposed "
              "timeline.",
        juce::dontSendNotification);
    repaint();
    return true;
}

void Studio::auditionRipple(bool allTracks, bool focusedTrack) {
    startRippleAudition(allTracks      ? RippleScope::allTracks
                        : focusedTrack ? RippleScope::focusedTrack
                                       : RippleScope::selectedTracks,
                        editGeneration, timeSelection, selectedTimelineTracks());
}

bool Studio::crossfadeSelectedPair() {
    const auto clips = timelineClipSelection.get(editGeneration);
    if (clips.size() != 2) {
        status.setText("Select exactly two clips to crossfade.", juce::dontSendNotification);
        return false;
    }
    juce::String error;
    const bool changed = mutateMixState(
        [&](carta::Session& candidate) {
            return carta::crossfadeClipPair(candidate, clips[0], clips[1], error);
        },
        error, "Crossfade selected clips");
    status.setText(changed ? "Crossfaded the selected clips; other overlaps remain unchanged."
                           : error,
                   juce::dontSendNotification);
    return changed;
}

bool Studio::startCrossfadeAudition(int scope) {
    if (!requireAudioOutput()) {
        return false;
    }
    const juce::ScopedLock guard(lock);
    if (dragMode != kDragNone) {
        status.setText("Finish the current drag before previewing.", juce::dontSendNotification);
        return false;
    }

    const auto clips = timelineClipSelection.get(editGeneration);
    const int track = selectedTrack;
    const int clip = selectedClip;
    const auto generation = editGeneration;
    if (scope == CrossfadeScope::selectedPair && clips.size() != 2) {
        status.setText("Select exactly two clips to crossfade.", juce::dontSendNotification);
        return false;
    }

    carta::Session candidate;
    carta::PlaybackRange range;
    juce::String error;
    bool valid = false;
    {
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (crossfadePreviewPreparationHook) {
            crossfadePreviewPreparationHook();
        }
#endif
        valid = scope == CrossfadeScope::selectedPair
                    ? carta::prepareSelectedCrossfadePreview(session, clips[0], clips[1],
                                                             auditionBefore, auditionAfter,
                                                             candidate, range, error)
                    : carta::prepareCrossfadePreview(
                          session, track, clip, scope == CrossfadeScope::acrossTracks,
                          auditionBefore, auditionAfter, candidate, range, error);
    }

    if (!valid || generation != editGeneration) {
        status.setText(valid ? "Project changed; preview the crossfade again." : error,
                       juce::dontSendNotification);
        return false;
    }

    stopSpecialAudition();
    std::swap(cutPreviewSession, candidate);
    setAuditionRange(range);
    cutPreviewOriginalPosition = position;
    position = range.start;
    cutPreviewActive = true;
    groupAuditionActive = false;
    auditionActive = true;
    playing = true;
    {
        const juce::ScopedUnlock unlock(lock);
        candidate = {};
    }
    status.setText(scope == CrossfadeScope::selectedPair
                       ? "Previewing the selected crossfade in the full mix; no edit applied."
                       : "Previewing the linear crossfade in the full mix; no edit applied.",
                   juce::dontSendNotification);
    repaint();
    return true;
}

void Studio::auditionSelectedCrossfade() {
    startCrossfadeAudition(CrossfadeScope::selectedPair);
}

void Studio::auditionCrossfade(bool acrossTracks) {
    startCrossfadeAudition(acrossTracks ? CrossfadeScope::acrossTracks : CrossfadeScope::sameTrack);
}

bool Studio::setAuditionContext(juce::int64 before, juce::int64 after, juce::String& error) {
    if (before < 0 || after < 0 || before > 60 * 44100 || after > 60 * 44100 ||
        (before == 0 && after == 0)) {
        error =
            "Invalid audition context; enter nonnegative sample counts up to 60 seconds, with at "
            "least one nonzero.";
        return false;
    }
    auditionBefore = before;
    auditionAfter = after;
    error.clear();
    return true;
}

void Studio::editAuditionContext() {
    const auto originalBefore = auditionBefore;
    const auto originalAfter = auditionAfter;
    showTextFieldsPrompt(
        "Audition context",
        "Set the time before and after a boundary in project samples (44,100 samples per second). "
        "Both cannot be zero; maximum 60 seconds each.",
        {{"before", "Before (samples)", juce::String(originalBefore)},
         {"after", "After (samples)", juce::String(originalAfter)}},
        "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), originalBefore,
         originalAfter](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted || values.size() != 2) {
                return;
            }
            if (safe->auditionBefore != originalBefore || safe->auditionAfter != originalAfter) {
                safe->status.setText("Audition context changed; reopen the editor.",
                                     juce::dontSendNotification);
                return;
            }
            juce::int64 before = 0;
            juce::int64 after = 0;
            juce::String error;
            if (!parseInteger(values[0], before) || !parseInteger(values[1], after) ||
                !safe->setAuditionContext(before, after, error)) {
                safe->status.setText(
                    error.isNotEmpty() ? error : "Enter audition context in project samples.",
                    juce::dontSendNotification);
                return;
            }
            safe->status.setText("Audition context updated.", juce::dontSendNotification);
        });
}

// ============================================================================
// Marker Editing, Playhead Seeking & Selection Memory / Boundaries
// ============================================================================

bool Studio::deleteMarkersInSelection(int& removed, juce::String& error) {
    removed = 0;
    error.clear();
    if (dragMode != kDragNone || !timeSelection.hasRange()) {
        error = "Select a time range before deleting markers.";
        return false;
    }

    const auto range = timeSelection;
    const auto revision = editGeneration;
    const int focus = selectedMarker;
    std::vector<int> indices;
    for (size_t i = 0; i < session.markers.size(); ++i) {
        double sample = session.markers[i].position * carta::sessionRate;
        if (!std::isfinite(sample)) {
            continue;
        }
        const double rounded = std::round(sample);
        if (std::abs(sample - rounded) < 1e-7) {
            sample = rounded;
        }
        if (sample >= static_cast<double>(range.start) && sample < static_cast<double>(range.end)) {
            indices.push_back(static_cast<int>(i));
        }
    }
    if (indices.empty()) {
        error = "There are no markers in the time selection.";
        return false;
    }

    const bool changed = mutateMarkers(
        [&](carta::Session& candidate) {
            if (editGeneration != revision || selectedMarker != focus ||
                timeSelection.start != range.start || timeSelection.end != range.end ||
                timeSelection.firstTrack != range.firstTrack ||
                timeSelection.lastTrack != range.lastTrack) {
                error = "Project or selection changed; try again.";
                return false;
            }
            return carta::deleteMarkers(candidate, indices, error);
        },
        [&] {
            if (focus >= 0) {
                if (std::binary_search(indices.begin(), indices.end(), focus)) {
                    selectedMarker = -1;
                } else {
                    selectedMarker =
                        focus -
                        static_cast<int>(std::lower_bound(indices.begin(), indices.end(), focus) -
                                         indices.begin());
                }
            }
        },
        "Delete markers in selection", error);
    if (!changed) {
        return false;
    }
    removed = static_cast<int>(indices.size());
    const auto message = "Deleted " + juce::String(removed) +
                         (removed == 1 ? " marker" : " markers") + " in samples [" +
                         juce::String(range.start) + ", " + juce::String(range.end) + ").";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    return true;
}

void Studio::shiftMarkersDialog(int command) {
    juce::uint64 revision = 0;
    juce::int64 first = 0;
    juce::int64 last = 0;
    {
        const juce::ScopedLock guard(lock);
        if (session.markers.empty()) {
            return;
        }
        revision = editGeneration;
        last = carta::projectEndSample(session);
        if (command == ShiftMarkersCommand::fromPlayhead) {
            const auto frame = position * carta::sessionRate;
            if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
                status.setText("Invalid playhead position.", juce::dontSendNotification);
                return;
            }
            first = static_cast<juce::int64>(std::llround(frame));
        } else if (command == ShiftMarkersCommand::inSelection) {
            if (!timeSelection.hasRange()) {
                status.setText("Select a time range first.", juce::dontSendNotification);
                return;
            }
            first = timeSelection.start;
            last = std::min(timeSelection.end, last);
        }
        if (first >= last) {
            status.setText("No markers exist in that time range.", juce::dontSendNotification);
            return;
        }
    }

    const auto scope = command == ShiftMarkersCommand::all ? juce::String("all explicit markers")
                       : command == ShiftMarkersCommand::fromPlayhead
                           ? juce::String("markers at or after the playhead")
                           : juce::String("markers in the time selection");
    showTextPrompt(
        "Shift markers",
        "Shift " + scope + " in samples [" + juce::String(first) + ", " + juce::String(last) +
            "). Negative offsets move earlier. Markers cannot cross or collide.",
        "offset", "Sample offset", "0", "Shift",
        [safe = juce::Component::SafePointer<Studio>(this), revision, first, last,
         scope](bool accepted, const juce::String& value) {
            if (safe == nullptr || !accepted) {
                return;
            }
            juce::int64 delta = 0;
            juce::String error;
            if (!parseInteger(value, delta)) {
                safe->status.setText("Enter an integer sample offset.", juce::dontSendNotification);
                return;
            }
            const bool moved = safe->mutateMarkers(
                [&](carta::Session& candidate) {
                    if (safe->editGeneration != revision) {
                        error = "Project changed; open the marker shift again.";
                        return false;
                    }
                    return carta::shiftMarkersInRange(candidate, first, last, delta, error);
                },
                [] {}, "Shift markers", error);
            safe->status.setText(
                moved ? "Shifted " + scope + " by " + juce::String(delta) + " samples." : error,
                juce::dontSendNotification);
        });
}

void Studio::reopenMarkerList(std::vector<int> selectedIndices, juce::uint64 revision) {
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Studio>(this),
                                     restoreIndices = std::move(selectedIndices),
                                     revision]() mutable {
        if (safe != nullptr && safe->dragMode == kDragNone && !safe->session.markers.empty()) {
            if (!restoreIndices.empty() && safe->editGeneration != revision) {
                restoreIndices.clear();
            }
            safe->showMarkerList(std::move(restoreIndices));
        }
    });
}

void Studio::editMarkerPosition(bool reopenList) {
    if (selectedMarker < 0 || selectedMarker >= static_cast<int>(session.markers.size())) {
        return;
    }
    const int index = selectedMarker;
    const double markerSample =
        session.markers[static_cast<size_t>(index)].position * carta::sessionRate;
    const bool valid =
        std::isfinite(markerSample) && markerSample >= 0 && markerSample < 9007199254740992.0;
    const auto initial =
        valid ? juce::String(static_cast<juce::int64>(std::llround(markerSample))) : juce::String();
    const auto revision = editGeneration;

    showTextPrompt("Set marker position",
                   "Enter samples, seconds (e.g. 1.5 s), or mm:ss.sss for the track start.",
                   "sample", "Start", initial, "Move",
                   [safe = juce::Component::SafePointer<Studio>(this), index, revision,
                    reopenList](bool accepted, const juce::String& value) {
                       if (safe != nullptr && reopenList) {
                           safe->reopenMarkerList();
                       }
                       if (safe == nullptr || !accepted) {
                           return;
                       }
                       if (!safe->dialogTargetCurrent(revision)) {
                           safe->status.setText("Project changed; reopen the editor.",
                                                juce::dontSendNotification);
                           return;
                       }
                       juce::int64 target = 0;
                       juce::String error;
                       if (!carta::parseSampleOrTime(value, target)) {
                           safe->status.setText(
                               "Enter samples, seconds (e.g. 1.5 s), or a time such as 1:02.500.",
                               juce::dontSendNotification);
                           return;
                       }
                       bool keepPlayback = false;
                       {
                           const juce::ScopedLock guard(safe->lock);
                           keepPlayback = safe->selectionPlaybackContinues();
                       }
                       const bool moved = safe->mutateMarkers(
                           [&](carta::Session& candidate) {
                               return carta::setMarkerSample(candidate, index, target, error);
                           },
                           [&] {
                               if (!keepPlayback) {
                                   safe->position =
                                       static_cast<double>(target) / carta::sessionRate;
                                   safe->auditionActive = false;
                               }
                           },
                           "Move marker", error);
                       if (!moved) {
                           safe->status.setText(error, juce::dontSendNotification);
                           return;
                       }
                       safe->status.setText("Marker moved to sample " + juce::String(target) + ".",
                                            juce::dontSendNotification);
                   });
}

bool Studio::nudgeMarker(int index, int direction, juce::String& error) {
    error.clear();
    if (index < 0 || index >= static_cast<int>(session.markers.size()) ||
        (direction != -1 && direction != 1)) {
        error = "Select a marker first.";
        return false;
    }

    const double original = session.markers[static_cast<size_t>(index)].position;
    bool follow = false;
    {
        const juce::ScopedLock guard(lock);
        follow = !playing && std::isfinite(original) &&
                 std::abs(position - original) < 0.25 / carta::sessionRate;
    }
    if (!mutateMarkers(
            [&](carta::Session& candidate) {
                return carta::shiftMarkerIndices(candidate, {index}, direction, error);
            },
            [&] {
                if (follow) {
                    position = session.markers[static_cast<size_t>(index)].position;
                }
            },
            "Nudge marker", error)) {
        if (error.isEmpty()) {
            error = "Could not move marker.";
        }
        return false;
    }

    const auto sample = static_cast<juce::int64>(
        std::llround(session.markers[static_cast<size_t>(index)].position * carta::sessionRate));
    const auto message = "Marker " + session.markers[static_cast<size_t>(index)].name +
                         " moved to sample " + juce::String(sample) + ".";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    repaint();
    return true;
}

void Studio::navigateMarker(bool next) {
    double origin = 0;
    juce::uint64 generation = 0;
    {
        const juce::ScopedLock guard(lock);
        origin = auditionActive && cutPreviewActive ? cutPreviewOriginalPosition : position;
        generation = editGeneration;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif
    const auto masterEnd = carta::projectEndSample(session);
    int hit = -1;
    double best =
        next ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
    for (int i = 0; i < static_cast<int>(session.markers.size()); ++i) {
        const double value = session.markers[static_cast<size_t>(i)].position;
        const double frame = value * carta::sessionRate;
        if (!std::isfinite(frame) || frame < 0 || frame >= static_cast<double>(masterEnd)) {
            continue;
        }
        if ((next && value > origin + 0.5 / carta::sessionRate && value < best) ||
            (!next && value < origin - 0.5 / carta::sessionRate && value > best)) {
            best = value;
            hit = i;
        }
    }
    if (hit < 0) {
        status.setText(next ? "No next marker." : "No previous marker.",
                       juce::dontSendNotification);
        announceNavigation(status.getText());
        return;
    }
    {
        const juce::ScopedLock guard(lock);
        if (generation != editGeneration) {
            status.setText("The project changed; try again.", juce::dontSendNotification);
            return;
        }
        stopSpecialAudition();
        selectedMarker = hit;
        position = best;
        auditionActive = false;
        keyboardRange.active = false;
    }
    if (best < viewStart || best >= viewStart + viewLength()) {
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()),
                                 best - viewLength() * 0.5);
    }
    const auto& name = session.markers[static_cast<size_t>(hit)].name;
    status.setText("Marker: " + (name.trim().isEmpty() ? juce::String("unnamed") : name),
                   juce::dontSendNotification);
    announceNavigation(status.getText() + ". " +
                       carta::describeMarkerPosition(session, hit, masterEnd));
    repaint();
}

void Studio::seekTimeline(int command) {
    double destination = 0;
    juce::uint64 generation = 0;
    {
        const juce::ScopedLock guard(lock);
        destination = auditionActive && cutPreviewActive ? cutPreviewOriginalPosition : position;
        generation = editGeneration;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif
    const auto end = carta::projectEndSample(session);
    const auto seek = static_cast<carta::PlayheadSeek>(command - SeekCommand::base);
    if (!carta::seekPlayhead(destination, end, seek)) {
        status.setText("Import audio before moving the playhead.", juce::dontSendNotification);
        return;
    }
    {
        const juce::ScopedLock guard(lock);
        if (generation != editGeneration) {
            status.setText("The project changed; try again.", juce::dontSendNotification);
            return;
        }
        stopSpecialAudition();
        position = destination;
        auditionActive = false;
        cutPreviewActive = false;
        selectedMarker = -1;
        keyboardRange.active = false;
    }
    if (destination < viewStart || destination >= viewStart + viewLength()) {
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()),
                                 destination - viewLength() * 0.5);
    }
    status.setText(
        "Playhead at sample " +
            juce::String(static_cast<juce::int64>(std::llround(destination * carta::sessionRate))) +
            ".",
        juce::dontSendNotification);
    announceNavigation(status.getText());
    repaint();
}

void Studio::navigateClipBoundary(bool next, bool allTracks) {
    double origin = 0;
    int track = -1;
    juce::uint64 generation = 0;
    {
        const juce::ScopedLock guard(lock);
        if (!allTracks &&
            (selectedTrack < 0 || selectedTrack >= static_cast<int>(session.tracks.size()))) {
            status.setText(
                "Select a track first, or use Shift with the boundary shortcut for all tracks.",
                juce::dontSendNotification);
            return;
        }
        track = allTracks ? -1 : selectedTrack;
        origin = auditionActive && cutPreviewActive ? cutPreviewOriginalPosition : position;
        generation = editGeneration;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif
    juce::int64 target = 0;
    if (!carta::adjacentClipBoundary(session, track, origin, next, target)) {
        status.setText(next ? "No next clip boundary." : "No previous clip boundary.",
                       juce::dontSendNotification);
        return;
    }
    {
        const juce::ScopedLock guard(lock);
        if (generation != editGeneration || (!allTracks && selectedTrack != track)) {
            status.setText("The project or focused track changed; try again.",
                           juce::dontSendNotification);
            return;
        }
        stopSpecialAudition();
        position = static_cast<double>(target) / carta::sessionRate;
        auditionActive = false;
        selectedMarker = -1;
        keyboardRange.active = false;
    }
    const double destination = static_cast<double>(target) / carta::sessionRate;
    if (destination < viewStart || destination >= viewStart + viewLength()) {
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()),
                                 destination - viewLength() * 0.5);
    }
    status.setText("Clip boundary at sample " + juce::String(target) +
                       (allTracks ? " (all tracks)." : " (selected track)."),
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    repaint();
}

bool Studio::selectSelectedClipSpan() {
    const auto generation = editGeneration;
    const auto clips = timelineClipSelection.get(generation);
    if (dragMode != kDragNone || clips.empty()) {
        status.setText("Select clips before selecting their time span.",
                       juce::dontSendNotification);
        return false;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (scopeSelectionHook) {
        scopeSelectionHook();
    }
#endif
    juce::int64 first = std::numeric_limits<juce::int64>::max();
    juce::int64 last = 0;
    std::set<int> tracks;
    for (const auto& ref : clips) {
        if (ref.track < 0 || ref.track >= static_cast<int>(session.tracks.size()) || ref.clip < 0 ||
            ref.clip >=
                static_cast<int>(session.tracks[static_cast<size_t>(ref.track)].clips.size())) {
            status.setText("Clip selection is no longer valid.", juce::dontSendNotification);
            return false;
        }
        juce::int64 a = 0;
        juce::int64 b = 0;
        if (!carta::clipSampleRange(
                session.tracks[static_cast<size_t>(ref.track)].clips[static_cast<size_t>(ref.clip)],
                a, b)) {
            status.setText("Each selected clip must contain a project sample; selection unchanged.",
                           juce::dontSendNotification);
            return false;
        }
        first = std::min(first, a);
        last = std::max(last, b);
        tracks.insert(ref.track);
    }

    const carta::TimeSelection range{first, last, *tracks.begin(), *tracks.rbegin()};
    {
        const juce::ScopedLock guard(lock);
        const auto& current = timelineClipSelection.get(editGeneration);
        if (generation != editGeneration || current.size() != clips.size() ||
            !std::equal(current.begin(), current.end(), clips.begin(),
                        [](const auto& a, const auto& b) {
                            return a.track == b.track && a.clip == b.clip;
                        })) {
            status.setText("The clip selection changed; try again.", juce::dontSendNotification);
            return false;
        }
        const bool keepPlayback = selectionPlaybackContinues();
        stopSpecialAudition();
        if (!carta::activateTimeSelection(timeSelection, timelineClipSelection, range,
                                          static_cast<int>(session.tracks.size()))) {
            status.setText("Could not select the clip span.", juce::dontSendNotification);
            return false;
        }
        timelineTracks = std::move(tracks);
        timelineTrackRevision = editGeneration;
        if (timelineTracks.count(selectedTrack) == 0) {
            selectedTrack = *timelineTracks.begin();
        }
        selectedClip = -1;
        selectedMarker = -1;
        keyboardRange.active = false;
        keyboardTracks.active = false;
        if (!keepPlayback) {
            position = static_cast<double>(first) / carta::sessionRate;
            auditionActive = false;
        }
    }

    const double start = static_cast<double>(first) / carta::sessionRate;
    if (start < viewStart || start >= viewStart + viewLength()) {
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()),
                                 start - viewLength() * 0.5);
    }
    const auto message = "Selected samples [" + juce::String(first) + ", " + juce::String(last) +
                         ") on " + juce::String(static_cast<int>(timelineTracks.size())) +
                         " clip tracks; other audio and gaps in that span are included.";
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
    return true;
}

bool Studio::selectClipTime(int portion) {
    const juce::ScopedLock guard(lock);
    if (!validSelection()) {
        status.setText("Focus a clip first.", juce::dontSendNotification);
        return false;
    }

    const auto& clip =
        session.tracks[static_cast<size_t>(selectedTrack)].clips[static_cast<size_t>(selectedClip)];
    juce::int64 start = 0;
    juce::int64 end = 0;
    bool valid = false;
    if (portion == ClipPortionCommand::full) {
        valid = carta::clipSampleRange(clip, start, end);
    } else {
        const double begin = portion == ClipPortionCommand::visible          ? viewStart
                             : portion == ClipPortionCommand::beforePlayhead ? 0.0
                                                                             : position;
        const double finish = portion == ClipPortionCommand::visible ? viewStart + viewLength()
                              : portion == ClipPortionCommand::beforePlayhead ? position
                                                                              : fullLength();
        valid = carta::clipSampleRangeWithin(clip, begin, finish, start, end);
    }
    if (!valid) {
        status.setText("No clip samples in the requested portion; selection unchanged.",
                       juce::dontSendNotification);
        return false;
    }

    const bool keepPlayback = selectionPlaybackContinues();
    stopSpecialAudition();
    carta::activateTimeSelection(timeSelection, timelineClipSelection,
                                 {start, end, selectedTrack, selectedTrack},
                                 static_cast<int>(session.tracks.size()));
    timelineTracks.clear();
    if (!keepPlayback) {
        position = static_cast<double>(start) / carta::sessionRate;
        auditionActive = false;
    }
    keyboardRange.active = false;
    keyboardTracks.active = false;
    status.setText("Selected clip time: samples " + juce::String(start) + " to " +
                       juce::String(end) + " (end exclusive); overlapping clips are included.",
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    menuItemsChanged();
    repaint();
    return true;
}

bool Studio::selectMasterTrackIndex(int index, bool scoped, juce::uint64 generation,
                                    juce::String& error) {
    error.clear();
    if (dragMode != kDragNone || editGeneration != generation) {
        error = "Project changed; reopen the track list.";
        return false;
    }
    std::vector<carta::MasterTrack> tracks;
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif
    if (!carta::getMasterTracks(session, tracks, error)) {
        return false;
    }
    if (dragMode != kDragNone || editGeneration != generation) {
        error = "Project changed; reopen the track list.";
        return false;
    }
    if (index < 0 || index >= static_cast<int>(tracks.size())) {
        error = "Invalid master track index.";
        return false;
    }

    const int count = static_cast<int>(session.tracks.size());
    if (count == 0) {
        error = "Add an audio track before selecting time.";
        return false;
    }
    carta::TrackRange scope{0, count - 1};
    if (scoped) {
        const auto independent = selectedTimelineTracks();
        if (!independent.empty()) {
            scope = {independent.front(), independent.back()};
        } else if (timeSelection.hasRange() && timeSelection.lastTrack < count) {
            scope = {timeSelection.firstTrack, timeSelection.lastTrack};
        } else if (selectedTrack >= 0 && selectedTrack < count) {
            scope = {selectedTrack, selectedTrack};
        } else {
            error = "Select a track first.";
            return false;
        }
    }

    const auto range = tracks[static_cast<size_t>(index)].range;
    const carta::TimeSelection selected{range.start, range.end, scope.first, scope.last};
    {
        const juce::ScopedLock guard(lock);
        const bool keepPlayback = selectionPlaybackContinues();
        stopSpecialAudition();
        if (!keepPlayback) {
            playing = false;
            auditionActive = false;
            cutPreviewActive = false;
        }
    }
    if (!carta::activateTimeSelection(timeSelection, timelineClipSelection, selected, count)) {
        error = "Could not select this master track.";
        return false;
    }
    if (!scoped) {
        timelineTracks.clear();
    }
    selectedTrack = scope.first;
    selectedClip = selectedMarker = -1;
    keyboardRange.active = false;
    keyboardTracks.active = false;

    const auto message = "Selected master track " + juce::String(index + 1) + ": samples " +
                         juce::String(range.start) + " to " + juce::String(range.end) +
                         " (end exclusive) " + (scoped ? "on selected tracks." : "on all tracks.");
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
    return true;
}

void Studio::selectMasterTrack(bool scoped) {
    carta::TrackRange scope;
    if (scoped) {
        const auto independent = selectedTimelineTracks();
        if (!independent.empty()) {
            scope = {independent.front(), independent.back()};
        } else if (timeSelection.hasRange() &&
                   timeSelection.lastTrack < static_cast<int>(session.tracks.size())) {
            scope = {timeSelection.firstTrack, timeSelection.lastTrack};
        } else if (selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size())) {
            scope = {selectedTrack, selectedTrack};
        } else {
            status.setText("Select a track first.", juce::dontSendNotification);
            return;
        }
    }

    double frame = 0;
    {
        const juce::ScopedLock guard(lock);
        frame = position * carta::sessionRate;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif
    if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
        status.setText("Place the playhead within the master.", juce::dontSendNotification);
        return;
    }
    carta::TimeSelection selected;
    juce::String error;
    if (!carta::selectMasterTrackAtSample(session,
                                          static_cast<juce::int64>(std::floor(frame + 1.0e-7)),
                                          selected, error, scope)) {
        status.setText(error, juce::dontSendNotification);
        return;
    }
    {
        const juce::ScopedLock guard(lock);
        const bool keepPlayback = selectionPlaybackContinues();
        stopSpecialAudition();
        if (!keepPlayback) {
            playing = false;
            auditionActive = false;
            cutPreviewActive = false;
        }
    }
    if (!scoped) {
        timelineTracks.clear();
    }
    carta::activateTimeSelection(timeSelection, timelineClipSelection, selected,
                                 static_cast<int>(session.tracks.size()));
    selectedTrack = selected.firstTrack;
    selectedClip = -1;
    selectedMarker = -1;
    keyboardRange.active = false;
    keyboardTracks.active = false;
    status.setText("Selected master track: samples " + juce::String(selected.start) + " to " +
                       juce::String(selected.end) + " (end exclusive) " +
                       (scoped ? "on selected tracks." : "on all tracks."),
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    menuItemsChanged();
    repaint();
}

void Studio::selectToBoundary(int id) {
    double frame = 0;
    {
        const juce::ScopedLock guard(lock);
        stopSpecialAudition();
        frame = position * carta::sessionRate;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif
    const auto end = carta::projectEndSample(session);
    if (end <= 0 || !std::isfinite(frame)) {
        status.setText("Import audio before making a boundary selection.",
                       juce::dontSendNotification);
        return;
    }
    const auto cursor =
        static_cast<juce::int64>(std::llround(juce::jlimit(0.0, static_cast<double>(end), frame)));
    const bool scoped =
        id >= SelectBoundaryCommand::scopedBase && id <= SelectBoundaryCommand::scopedEnd;
    const auto boundary = static_cast<carta::SelectionBoundary>(
        id - (scoped ? SelectBoundaryCommand::scopedBase : SelectBoundaryCommand::allBase));
    carta::TrackRange scope;
    const auto independent = scoped ? selectedTimelineTracks() : std::vector<int>{};
    if (scoped) {
        if (!independent.empty()) {
            scope = {independent.front(), independent.back()};
        } else if (timeSelection.hasRange() &&
                   timeSelection.lastTrack < static_cast<int>(session.tracks.size())) {
            scope = {timeSelection.firstTrack, timeSelection.lastTrack};
        } else if (selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size())) {
            scope = {selectedTrack, selectedTrack};
        } else {
            status.setText("Select a track first.", juce::dontSendNotification);
            return;
        }
    }

    carta::TimeSelection selected;
    if (!(independent.empty() ? carta::selectToBoundary(session, cursor, boundary, selected, scope)
                              : carta::selectToBoundaryOnTracks(session, cursor, boundary,
                                                                independent, selected))) {
        status.setText("No distinct boundary in that direction.", juce::dontSendNotification);
        return;
    }
    {
        const juce::ScopedLock guard(lock);
        if (!selectionPlaybackContinues()) {
            playing = false;
            auditionActive = false;
            cutPreviewActive = false;
            position = static_cast<double>(cursor) / carta::sessionRate;
        }
    }
    if (!scoped) {
        timelineTracks.clear();
    }
    carta::activateTimeSelection(timeSelection, timelineClipSelection, selected,
                                 static_cast<int>(session.tracks.size()));
    selectedTrack = selected.firstTrack;
    selectedClip = -1;
    selectedMarker = -1;
    keyboardRange.active = false;
    keyboardTracks.active = false;
    status.setText("Selected samples " + juce::String(selected.start) + " to " +
                       juce::String(selected.end) + " (end exclusive) " +
                       (scoped ? "on selected tracks." : "on all tracks."),
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    menuItemsChanged();
    repaint();
}

void Studio::adjustTimeSelection(carta::SelectionPart part, int direction) {
    {
        const juce::ScopedLock guard(lock);
        stopSpecialAudition();
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif
    const double limit = std::ceil(fullLength() * carta::sessionRate);
    if (!std::isfinite(limit) || limit <= 0 || limit >= 9007199254740992.0 ||
        !carta::nudgeTimeSelection(timeSelection, part, direction,
                                   static_cast<juce::int64>(limit))) {
        status.setText(timeSelection.hasRange() ? "Selection cannot move farther in that direction."
                                                : "Select a time range first.",
                       juce::dontSendNotification);
        return;
    }
    timelineClipSelection.clear();
    menuItemsChanged();
    selectedMarker = -1;
    const auto point = part == carta::SelectionPart::end ? timeSelection.end : timeSelection.start;
    double displayPosition = 0;
    {
        const juce::ScopedLock guard(lock);
        const bool keepPlayback = selectionPlaybackContinues();
        if (!keepPlayback) {
            if (auditionActive) {
                playing = false;
            }
            auditionActive = false;
            position = static_cast<double>(point) / carta::sessionRate;
        }
        displayPosition = position;
    }
    if (displayPosition < viewStart || displayPosition >= viewStart + viewLength()) {
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()),
                                 displayPosition - viewLength() * 0.5);
    }
    status.setText("Selection: samples " + juce::String(timeSelection.start) + " to " +
                       juce::String(timeSelection.end) + " (end exclusive).",
                   juce::dontSendNotification);
    repaint();
}

void Studio::markLiveSelection(bool markIn) {
    double frame = 0;
    bool temporaryPreview = false;
    {
        const juce::ScopedLock guard(lock);
        temporaryPreview = auditionActive && (cutPreviewActive || groupAuditionActive);
        frame = position * carta::sessionRate;
    }
    if (temporaryPreview) {
        status.setText("Finish the temporary edit or clip-group preview before marking a range.",
                       juce::dontSendNotification);
        return;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif
    const auto end = carta::projectEndSample(session);
    if (end <= 0 || end >= 9007199254740992LL || !std::isfinite(frame)) {
        status.setText("No valid playhead position to mark.", juce::dontSendNotification);
        return;
    }
    const auto sample =
        static_cast<juce::int64>(std::llround(std::clamp(frame, 0.0, static_cast<double>(end))));
    if (markIn) {
        const int count = static_cast<int>(session.tracks.size());
        const auto independent = selectedTimelineTracks();
        if (!independent.empty()) {
            liveInFirstTrack = independent.front();
            liveInLastTrack = independent.back();
        } else if (timeSelection.hasRange() && timeSelection.lastTrack < count) {
            liveInFirstTrack = timeSelection.firstTrack;
            liveInLastTrack = timeSelection.lastTrack;
        } else if (selectedTrack >= 0 && selectedTrack < count) {
            liveInFirstTrack = liveInLastTrack = selectedTrack;
        } else {
            status.setText("Focus a track or select a track range before marking in.",
                           juce::dontSendNotification);
            return;
        }
        liveInTracks = {independent.begin(), independent.end()};
        liveInSample = sample;
        liveInGeneration = editGeneration;
        liveInPresent = true;
        status.setText("Marked in at sample " + juce::String(sample) + ". Press O to mark out.",
                       juce::dontSendNotification);
    } else {
        if (!liveInPresent || liveInGeneration != editGeneration) {
            liveInPresent = false;
            status.setText("Mark in again; the project changed or no in-point is set.",
                           juce::dontSendNotification);
            menuItemsChanged();
            repaint();
            return;
        }
        if (sample == liveInSample) {
            status.setText("In and out are the same sample; mark out at a different position.",
                           juce::dontSendNotification);
            return;
        }
        const carta::TimeSelection selection{std::min(sample, liveInSample),
                                             std::max(sample, liveInSample), liveInFirstTrack,
                                             liveInLastTrack};
        if (!carta::activateTimeSelection(timeSelection, timelineClipSelection, selection,
                                          static_cast<int>(session.tracks.size()))) {
            liveInPresent = false;
            status.setText("Marked tracks are no longer available; mark in again.",
                           juce::dontSendNotification);
            return;
        }
        timelineTracks = liveInTracks;
        timelineTrackRevision = editGeneration;
        liveInPresent = false;
        keyboardRange.active = false;
        keyboardTracks.active = false;
        selectedClip = -1;
        selectedMarker = -1;
        const auto scope = timelineTracks.empty()
                               ? "tracks " + juce::String(selection.firstTrack + 1) + " to " +
                                     juce::String(selection.lastTrack + 1) + "."
                               : describeTimelineTracks();
        status.setText("Selected samples " + juce::String(selection.start) + " to " +
                           juce::String(selection.end) + " (end exclusive), " + scope,
                       juce::dontSendNotification);
    }
    announceNavigation(status.getText());
    menuItemsChanged();
    repaint();
}

void Studio::extendSelection(int direction) {
    bool keepPlayback = false;
    double selectionPosition = 0;
    {
        const juce::ScopedLock guard(lock);
        stopSpecialAudition();
        keepPlayback = selectionPlaybackContinues();
        selectionPosition = position;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif
    const double limit = std::ceil(fullLength() * carta::sessionRate);
    if (!std::isfinite(limit) || limit <= 0 || limit >= 9007199254740992.0 ||
        !carta::extendTimeSelection(timeSelection, selectionPosition, selectedTrack,
                                    static_cast<int>(session.tracks.size()), direction,
                                    static_cast<juce::int64>(limit), keyboardRange, keepPlayback)) {
        status.setText(selectedTrack < 0 && !timeSelection.hasRange()
                           ? "Select a track first."
                           : "Selection cannot extend farther in that direction.",
                       juce::dontSendNotification);
        return;
    }
    timelineClipSelection.clear();
    menuItemsChanged();
    double displayPosition = 0;
    {
        const juce::ScopedLock guard(lock);
        if (!keepPlayback) {
            if (!keyboardRange.independent) {
                position = selectionPosition;
            }
            playing = false;
            auditionActive = false;
        }
        displayPosition = position;
    }
    selectedTrack = timeSelection.firstTrack;
    selectedClip = -1;
    selectedMarker = -1;
    if (displayPosition < viewStart || displayPosition >= viewStart + viewLength()) {
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()),
                                 displayPosition - viewLength() * 0.5);
    }
    status.setText(timeSelection.hasRange()
                       ? "Selection: samples " + juce::String(timeSelection.start) + " to " +
                             juce::String(timeSelection.end) + " (end exclusive)."
                       : "Selection collapsed at sample " + juce::String(keyboardRange.focus) + ".",
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    repaint();
}

void Studio::extendSelectionTracks(int direction) {
    const juce::ScopedLock guard(lock);
    stopSpecialAudition();
    auto nextSelection = timeSelection;
    auto nextGesture = keyboardTracks;
    const auto independent = selectedTimelineTracks();
    if (!independent.empty() && nextSelection.hasRange()) {
        nextSelection.firstTrack = independent.front();
        nextSelection.lastTrack = independent.back();
        nextGesture.active = false;
    }
    if (!carta::extendSelectionTracks(nextSelection, selectedTrack,
                                      static_cast<int>(session.tracks.size()), direction,
                                      nextGesture)) {
        status.setText(timeSelection.hasRange()
                           ? "Selection cannot extend farther in that direction."
                           : "Select a time range first.",
                       juce::dontSendNotification);
        return;
    }
    timeSelection = nextSelection;
    keyboardTracks = nextGesture;
    timelineTracks.clear();
    timelineClipSelection.clear();
    menuItemsChanged();
    if (!selectionPlaybackContinues()) {
        playing = false;
        auditionActive = false;
    }
    selectedTrack = keyboardTracks.focus;
    selectedClip = -1;
    selectedMarker = -1;
    revealTrack(selectedTrack);
    status.setText("Selection: tracks " + juce::String(timeSelection.firstTrack + 1) + " to " +
                       juce::String(timeSelection.lastTrack + 1) + "; samples " +
                       juce::String(timeSelection.start) + " to " +
                       juce::String(timeSelection.end) + " (end exclusive).",
                   juce::dontSendNotification);
    announceNavigation(status.getText());
    repaint();
}

bool Studio::clearRememberedSelection() {
    const juce::ScopedLock guard(lock);
    if (!selectionMemory.present) {
        status.setText("No remembered selection to clear.", juce::dontSendNotification);
        return false;
    }
    selectionMemory = {};
    rememberedTimelineTracks.clear();
    menuItemsChanged();
    status.setText("Remembered selection cleared.", juce::dontSendNotification);
    return true;
}

bool Studio::rememberSelection(bool restore) {
    const juce::ScopedLock guard(lock);
    const int count = static_cast<int>(session.tracks.size());
    if (!restore) {
        if (!selectionMemory.remember(timeSelection, count, editGeneration)) {
            status.setText("Select a time range first.", juce::dontSendNotification);
            return false;
        }
        rememberedTimelineTracks.clear();
        for (int track : selectedTimelineTracks()) {
            rememberedTimelineTracks.insert(track);
        }
        menuItemsChanged();
        status.setText("Remembered the time selection; some project edits invalidate it.",
                       juce::dontSendNotification);
        return true;
    }

    auto restored = timeSelection;
    if (!selectionMemory.restore(restored, count, editGeneration)) {
        status.setText("No current remembered selection; remember it again after editing.",
                       juce::dontSendNotification);
        return false;
    }
    stopSpecialAudition();
    timeSelection = restored;
    timelineTracks = rememberedTimelineTracks;
    timelineTrackRevision = editGeneration;
    timelineClipSelection.clear();
    menuItemsChanged();
    const bool keepPlayback = selectionPlaybackContinues();
    if (!keepPlayback) {
        playing = false;
        auditionActive = false;
        cutPreviewActive = false;
        position = static_cast<double>(timeSelection.start) / carta::sessionRate;
    }
    selectedTrack = timeSelection.firstTrack;
    selectedClip = -1;
    selectedMarker = -1;
    keyboardRange.active = false;
    keyboardTracks.active = false;
    revealTrack(selectedTrack);
    if (position < viewStart || position >= viewStart + viewLength()) {
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()),
                                 position - viewLength() * 0.5);
    }
    const auto scope = timelineTracks.empty()
                           ? "tracks " + juce::String(timeSelection.firstTrack + 1) + " to " +
                                 juce::String(timeSelection.lastTrack + 1) + "."
                           : describeTimelineTracks();
    status.setText("Restored samples " + juce::String(timeSelection.start) + " to " +
                       juce::String(timeSelection.end) + " (end exclusive), " + scope,
                   juce::dontSendNotification);
    repaint();
    return true;
}

// ============================================================================
// Native & Modal Dialog Launchers + Marker Rename
// ============================================================================

void Studio::launchStudioChooser(const juce::String& title, const juce::File& initial,
                                 const juce::String& filters, int flags,
                                 std::function<void(const StudioFileSelection&)> callback) {
    if (savePromptActive || juce::Component::getCurrentlyModalComponent() != nullptr) {
        status.setText("Finish the current dialog first.", juce::dontSendNotification);
        if (callback) {
            callback({});
        }
        return;
    }
    savePromptActive = true;
    auto completed = [safe = juce::Component::SafePointer<Studio>(this),
                      done = std::move(callback)](juce::Array<juce::File> files) {
        if (safe == nullptr) {
            return;
        }
        safe->savePromptActive = false;
        if (done) {
            done({std::move(files)});
        }
    };
#if JUCE_LINUX && CARTA_STUDIO_GTK_PROMPTS && !CARTA_STUDIO_COMPONENT_TEST
    if (!juce::FileChooser::isPlatformDialogAvailable() &&
        carta::showGtkFileChooser(this, title, initial, filters, flags, completed)) {
        return;
    }
#endif
    chooser = std::make_unique<juce::FileChooser>(title, initial, filters);
    chooser->launchAsync(flags,
                         [done = std::move(completed)](const juce::FileChooser& fileChooser) {
                             done(fileChooser.getResults());
                         });
}

void Studio::showTextFieldsChoicePrompt(
    const juce::String& title, const juce::String& message,
    std::vector<carta::TextPromptField> fields, const juce::String& accept,
    const juce::String& alternative, std::function<void(int, const juce::StringArray&)> callback) {
    if (savePromptActive || juce::Component::getCurrentlyModalComponent() != nullptr) {
        status.setText("Finish the current dialog first.", juce::dontSendNotification);
        return;
    }
    if (fields.empty() || fields.size() > 8) {
        status.setText("This editor has an unsupported number of fields.",
                       juce::dontSendNotification);
        return;
    }
    savePromptActive = true;
    auto completed = [safe = juce::Component::SafePointer<Studio>(this),
                      done = std::move(callback)](int choice, juce::StringArray values) {
        if (safe == nullptr) {
            return;
        }
        safe->savePromptActive = false;
        if (done) {
            done(choice, values);
        }
    };
#if JUCE_MAC && !CARTA_STUDIO_COMPONENT_TEST
    carta::showNativeTextFieldsPrompt(this, title, message, fields, accept, alternative,
                                      std::move(completed));
#else
#if JUCE_LINUX && CARTA_STUDIO_GTK_PROMPTS && !CARTA_STUDIO_COMPONENT_TEST
    if (carta::showGtkTextFieldsPrompt(this, title, message, fields, accept, alternative,
                                       completed)) {
        return;
    }
#endif
    auto* dialog = new juce::AlertWindow(title, message, juce::MessageBoxIconType::NoIcon);
    for (const auto& field : fields) {
        dialog->addTextEditor(field.key, field.initial, field.label);
    }
    dialog->addButton(accept, 1);
    if (alternative.isNotEmpty()) {
        dialog->addButton(alternative, 2);
    }
    dialog->addButton("Cancel", 0);
    dialog->enterModalState(
        true,
        juce::ModalCallbackFunction::create([dialog, specs = std::move(fields),
                                             hasAlternative = alternative.isNotEmpty(),
                                             done = std::move(completed)](int answer) {
            const int choice = answer == 1 ? 1 : answer == 2 && hasAlternative ? 2 : 0;
            juce::StringArray values;
            if (choice != 0) {
                for (const auto& field : specs) {
                    values.add(dialog->getTextEditorContents(field.key));
                }
            }
            done(choice, std::move(values));
        }),
        true);
#endif
}

void Studio::showTextFieldsPrompt(const juce::String& title, const juce::String& message,
                                  std::vector<carta::TextPromptField> fields,
                                  const juce::String& accept,
                                  std::function<void(bool, const juce::StringArray&)> callback) {
    showTextFieldsChoicePrompt(
        title, message, std::move(fields), accept, {},
        [done = std::move(callback)](int choice, const juce::StringArray& values) {
            if (done) {
                done(choice == 1, values);
            }
        });
}

void Studio::showTextPrompt(const juce::String& title, const juce::String& message,
                            const juce::String& key, const juce::String& label,
                            const juce::String& initial, const juce::String& accept,
                            std::function<void(bool, const juce::String&)> callback) {
    if (savePromptActive || juce::Component::getCurrentlyModalComponent() != nullptr) {
        status.setText("Finish the current dialog first.", juce::dontSendNotification);
        return;
    }
    savePromptActive = true;
    auto completed = [safe = juce::Component::SafePointer<Studio>(this),
                      done = std::move(callback)](bool accepted, juce::String value) {
        if (safe == nullptr) {
            return;
        }
        safe->savePromptActive = false;
        if (done) {
            done(accepted, value);
        }
    };
#if JUCE_MAC && !CARTA_STUDIO_COMPONENT_TEST
    juce::ignoreUnused(key);
    carta::showNativeTextPrompt(this, title, message, label, initial, accept, std::move(completed));
#else
#if JUCE_LINUX && CARTA_STUDIO_GTK_PROMPTS && !CARTA_STUDIO_COMPONENT_TEST
    if (carta::showGtkTextFieldsPrompt(this, title, message, {{key, label, initial}}, accept, {},
                                       [completed](int choice, juce::StringArray values) {
                                           completed(choice == 1, choice == 1 && values.size() == 1
                                                                      ? values[0]
                                                                      : juce::String{});
                                       })) {
        return;
    }
#endif
    auto* dialog = new juce::AlertWindow(title, message, juce::MessageBoxIconType::NoIcon);
    dialog->addTextEditor(key, initial, label);
    dialog->addButton(accept, 1);
    dialog->addButton("Cancel", 0);
    dialog->enterModalState(
        true,
        juce::ModalCallbackFunction::create([dialog, key, done = std::move(completed)](int answer) {
            done(answer == 1, answer == 1 ? dialog->getTextEditorContents(key) : juce::String{});
        }),
        true);
#endif
}

void Studio::renameMarker(bool reopenList) {
    if (selectedMarker < 0 || selectedMarker >= static_cast<int>(session.markers.size())) {
        return;
    }
    const auto revision = editGeneration;
    const int index = selectedMarker;
    showTextPrompt("Rename marker", "Track name", "name", "Track name",
                   session.markers[static_cast<size_t>(index)].name, "Rename",
                   [safe = juce::Component::SafePointer<Studio>(this), index, revision,
                    reopenList](bool accepted, const juce::String& value) {
                       if (safe != nullptr && reopenList) {
                           safe->reopenMarkerList();
                       }
                       if (safe == nullptr || !accepted) {
                           return;
                       }
                       if (!safe->dialogTargetCurrent(revision)) {
                           safe->status.setText("Project changed; reopen the editor.",
                                                juce::dontSendNotification);
                           return;
                       }
                       const auto name = value.trim();
                       juce::String error;
                       const bool changed = safe->mutateMarkers(
                           [&](carta::Session& candidate) {
                               return carta::renameMarkerIndices(candidate, {index}, {name}, error);
                           },
                           [] {}, "Rename marker", error);
                       const juce::String message = changed              ? "Marker renamed."
                                                    : error.isNotEmpty() ? error
                                                                         : "Marker unchanged.";
                       safe->status.setText(message, juce::dontSendNotification);
                       safe->announceNavigation(message);
                   });
}
