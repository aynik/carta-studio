/**
 * @file StudioMouseAndDrag.cpp
 * @brief Mouse, trackpad, and file drag-and-drop interaction handlers, plus
 *        live drag preview updates for `Studio`.
 *
 * Coordinate spaces and units:
 * - Timeline pointer X in `[area.getX() + labelWidth, area.getRight())` maps
 *   to seconds via `xToTime()` and 44.1 kHz half-open `[start, end)` project
 *   samples via `sampleAtX()` / `snappedPointerSample()`.
 * - Clip and persistent-group drags compute horizontal deltas using `dragX`,
 *   `dragViewStart`, and `dragSpan` captured on `mouseDown` so viewport
 *   edge-scrolling (`scrollClipDrag()`) shifts the view seamlessly mid-drag.
 * - Track header gain/balance drags convert horizontal pixel offsets into
 *   dB-clamped linear gain (`[0, carta::maxLinearGain]`, coarse snap at 0 dB)
 *   or stereo pan (`[-1, 1]`, coarse snap at center).
 *
 * Threading and locking invariants:
 * - All pointer handlers run on the JUCE message thread.
 * - `Studio::lock` protects `session`, `position`, and active audition flags
 *   shared with the audio callback. `mouseDown()` and `mouseUp()` release `lock`
 *   via `juce::ScopedUnlock` around candidate preparation, snap-target scans,
 *   and component-test hooks so real-time audio never blocks, then re-check
 *   `editGeneration` or swap committed state under `lock`.
 * - Group drag previews (`updateGroupDrag()`, `updateGroupEdgeDrag()`) mutate
 *   only message-thread preview state until commit on `mouseUp()`.
 */

#include "Studio/Studio.h"

namespace {

constexpr int kTimelineRulerHeight = 32;
constexpr int kClipEdgeHitPx = 7;
constexpr int kSnapBoundaryRadiusPx = 7;

namespace HeaderHit {
[[maybe_unused]] constexpr int none = 0;
constexpr int collapse = 1;
constexpr int mute = 2;
constexpr int solo = 3;
constexpr int gain = 4;
constexpr int balance = 5;
} // namespace HeaderHit

namespace MdRibbonMenuId {
constexpr int splitHere = 1;
constexpr int splitPlayhead = 2;
constexpr int merge = 3;
constexpr int duplicateShared = 4;
constexpr int auditionTrack = 5;
constexpr int auditionFromHere = 6;
constexpr int renameTrack = 7;
constexpr int removeTrack = 8;
constexpr int syncFromClips = 9;
} // namespace MdRibbonMenuId

} // namespace

// ============================================================================
// File Drag-and-Drop Import
// ============================================================================

bool Studio::isInterestedInFileDrag(const juce::StringArray& files) {
    return !files.isEmpty();
}

void Studio::fileDragEnter(const juce::StringArray&, int x, int y) {
    updateFileDropGuide(x, y);
}

void Studio::fileDragMove(const juce::StringArray&, int x, int y) {
    updateFileDropGuide(x, y);
}

void Studio::fileDragExit(const juce::StringArray&) {
    fileDropGuideVisible = false;
    fileDropTrack = -1;
    repaint();
}

void Studio::filesDropped(const juce::StringArray& files, int x, int y) {
    const auto area = timeline().withTrimmedTop(timelineHeaderHeight()).withTrimmedLeft(labelWidth);
    fileDropGuideVisible = false;
    fileDropTrack = -1;
    repaint();

    if (savePromptActive || juce::Component::getCurrentlyModalComponent() != nullptr) {
        status.setText("Finish the current dialog before importing audio.",
                       juce::dontSendNotification);
        return;
    }
    if (!area.contains(x, y)) {
        status.setText("Drop audio over a timeline layer.", juce::dontSendNotification);
        return;
    }

    const auto at = sampleAtX(x);
    int queued = 0;
    int skipped = 0;
    if (activeOpen) {
        for (const auto& path : files) {
            recordImportIssue(juce::File(path),
                              "Wait for project opening to finish or cancel it before importing.");
        }
        return;
    }

    const int droppedLayer = trackAt(y);
    const int targetTrack = droppedLayer >= 0 ? droppedLayer : -2;
    const juce::int64 dropStart = droppedLayer >= 0 ? at : -1;
    const int markerPolicy = -1;
    const juce::uint64 batch = (droppedLayer >= 0 && files.size() > 1) ? nextImportBatchId++ : 0;

    for (const auto& path : files) {
        const juce::File file(path);
        if (!file.existsAsFile()) {
            recordImportIssue(file, "Not an existing regular file.");
            ++skipped;
            continue;
        }
        if (importFile(file, dropStart, targetTrack, markerPolicy, false, batch)) {
            ++queued;
        } else {
            ++skipped;
        }
    }

    if (!queued) {
        status.setText("No files to import.", juce::dontSendNotification);
    } else if (skipped) {
        status.setText("Queued " + juce::String(queued) + " files; skipped " +
                           juce::String(skipped) + " non-files.",
                       juce::dontSendNotification);
    }
}

// ============================================================================
// MiniDisc Ribbon Context Menu & Drag Preview
// ============================================================================

void Studio::showMdRibbonContextMenu(const juce::MouseEvent& event,
                                     const MdRibbonPointerInfo& info) {
    const auto area = timeline();
    const juce::int64 clickSample =
        event.x >= area.getX() + labelWidth ? std::max<juce::int64>(0, sampleAtX(event.x)) : -1;
    const juce::int64 playheadSample =
        static_cast<juce::int64>(std::llround(position * carta::sessionRate));
    const bool hasTrack =
        info.trackIndex >= 0 && info.trackIndex < static_cast<int>(session.mdDisc.tracks.size());
    if (hasTrack) {
        const auto focusId = info.trackId;
        {
            const juce::ScopedUnlock unlock(lock);
            discWorkspace.selectTrackById(focusId, true);
        }
        repaint();
    }

    auto coversStrict = [&](int trackIdx, juce::int64 sample) {
        if (trackIdx < 0 || trackIdx >= static_cast<int>(session.mdDisc.tracks.size()) ||
            sample <= 0) {
            return false;
        }
        for (const auto& pt : session.mdDisc.tracks[static_cast<size_t>(trackIdx)].parts) {
            if (pt.startSample < sample && sample < pt.endSample) {
                return true;
            }
        }
        return false;
    };

    const bool canSplitHere = hasTrack && coversStrict(info.trackIndex, clickSample);
    const bool canSplitPlayhead = hasTrack && coversStrict(info.trackIndex, playheadSample);
    int mergeLeftIdx = -1;
    if (info.gesture == MdRibbonGesture::sharedSplit && hasTrack &&
        info.rightTrackIndex == info.trackIndex + 1) {
        mergeLeftIdx = info.trackIndex;
    } else if (hasTrack && info.trackIndex + 1 < static_cast<int>(session.mdDisc.tracks.size())) {
        mergeLeftIdx = info.trackIndex;
    }

    juce::PopupMenu menu;
    menu.addItem(MdRibbonMenuId::splitHere, "Split Track Here", canSplitHere);
    menu.addItem(MdRibbonMenuId::splitPlayhead, "Split Track at Playhead", canSplitPlayhead);
    menu.addItem(MdRibbonMenuId::merge,
                 info.gesture == MdRibbonGesture::sharedSplit ? "Merge Split"
                                                              : "Merge with Next Track",
                 mergeLeftIdx >= 0);
    menu.addItem(MdRibbonMenuId::duplicateShared, "Duplicate as Shared Track", hasTrack);
    menu.addSeparator();
    menu.addItem(MdRibbonMenuId::auditionTrack, "Audition Track", hasTrack);
    menu.addItem(MdRibbonMenuId::auditionFromHere, "Audition from Here", hasTrack);
    menu.addSeparator();
    menu.addItem(MdRibbonMenuId::renameTrack, "Rename Track...", hasTrack);
    menu.addItem(MdRibbonMenuId::removeTrack, "Remove Track", hasTrack);
    menu.addSeparator();
    menu.addItem(MdRibbonMenuId::syncFromClips, "Tracks from Clips", session.length() > 0);

    const auto revision = editGeneration;
    const int trackIdx = info.trackIndex;
    const auto trackId = info.trackId;
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(this).withTargetScreenArea(
            {event.getScreenX(), event.getScreenY(), 1, 1}),
        [safe = juce::Component::SafePointer<Studio>(this), revision, trackIdx, trackId,
         mergeLeftIdx, clickSample, playheadSample](int result) {
            if (safe == nullptr || result == 0) {
                return;
            }
            if (safe->editGeneration != revision) {
                safe->status.setText("The project changed; open the menu again.",
                                     juce::dontSendNotification);
                return;
            }
            switch (result) {
            case MdRibbonMenuId::splitHere:
                if (safe->discWorkspace.onCommand) {
                    safe->discWorkspace.onCommand({"disc", "track", "split", juce::String(trackIdx),
                                                   juce::String(clickSample)},
                                                  revision);
                }
                break;
            case MdRibbonMenuId::splitPlayhead:
                if (safe->discWorkspace.onCommand) {
                    safe->discWorkspace.onCommand({"disc", "track", "split", juce::String(trackIdx),
                                                   juce::String(playheadSample)},
                                                  revision);
                }
                break;
            case MdRibbonMenuId::merge:
                if (safe->discWorkspace.onCommand) {
                    safe->discWorkspace.onCommand(
                        {"disc", "track", "merge", juce::String(mergeLeftIdx)}, revision);
                }
                break;
            case MdRibbonMenuId::duplicateShared:
                if (safe->discWorkspace.onCommand) {
                    safe->discWorkspace.onCommand(
                        {"disc", "track", "duplicate", juce::String(trackIdx)}, revision);
                }
                break;
            case MdRibbonMenuId::auditionTrack:
                if (safe->discWorkspace.onCommand) {
                    safe->discWorkspace.onCommand({"disc", "preview", juce::String(trackIdx)},
                                                  revision);
                }
                break;
            case MdRibbonMenuId::auditionFromHere:
                if (safe->discWorkspace.onCommand) {
                    safe->discWorkspace.onCommand(
                        {"disc", "preview", "from", juce::String(trackIdx)}, revision);
                }
                break;
            case MdRibbonMenuId::renameTrack:
                if (!safe->discWorkspaceVisible) {
                    safe->setDiscWorkspaceVisible(true);
                }
                safe->discWorkspace.selectTrackById(trackId, true);
                safe->discWorkspace.focusTrackRename();
                break;
            case MdRibbonMenuId::removeTrack:
                if (safe->discWorkspace.onCommand) {
                    safe->discWorkspace.onCommand(
                        {"disc", "track", "remove", juce::String(trackIdx)}, revision);
                }
                break;
            case MdRibbonMenuId::syncFromClips:
                if (safe->discWorkspace.onCommand) {
                    safe->discWorkspace.onCommand({"disc", "sync", "clips"}, revision);
                }
                break;
            default:
                break;
            }
        });
}

void Studio::updateDraggedMdRibbon(const juce::ModifierKeys& mods) {
    if (dragMode != kDragMdRibbon) {
        return;
    }

    const auto area = timeline();
    const double span = viewLength();
    const int left = area.getX() + labelWidth;
    const int width = std::max(1, area.getWidth() - labelWidth);
    const int clampedX = std::clamp(mdDragPointer.x, left, std::max(left, area.getRight()));
    const bool allowSnap = snapMode != 0 && !mods.isShiftDown();
    const double radius =
        (allowSnap && snapMode == 2)
            ? static_cast<double>(kSnapBoundaryRadiusPx) * span / static_cast<double>(width)
            : 0.0;
    const auto& targets = (allowSnap && snapMode == 2) ? dragTargets : emptySnapTargets;

    carta::MdDisc nextDisc = mdDragBeforeDisc;
    std::string mdErr;
    bool ok = false;
    juce::String summary;
    snapGuideVisible = false;

    if (mdDragGesture == MdRibbonGesture::sharedSplit) {
        if (mdDragTrack0 < 0 || mdDragTrack0 >= static_cast<int>(mdDragBeforeDisc.tracks.size()) ||
            mdDragRightTrack0 < 0 ||
            mdDragRightTrack0 >= static_cast<int>(mdDragBeforeDisc.tracks.size())) {
            return;
        }
        const auto& leftParts = mdDragBeforeDisc.tracks[static_cast<size_t>(mdDragTrack0)].parts;
        const auto& rightParts =
            mdDragBeforeDisc.tracks[static_cast<size_t>(mdDragRightTrack0)].parts;
        if (mdDragPart0 < 0 || mdDragPart0 >= static_cast<int>(leftParts.size()) ||
            mdDragRightPart0 < 0 || mdDragRightPart0 >= static_cast<int>(rightParts.size())) {
            return;
        }
        const juce::int64 lo = leftParts[static_cast<size_t>(mdDragPart0)].startSample + 1;
        const juce::int64 hi = std::min<juce::int64>(
            mdDragMaxSample,
            static_cast<juce::int64>(rightParts[static_cast<size_t>(mdDragRightPart0)].endSample) -
                1);
        if (hi < lo) {
            return;
        }
        const double loTime = static_cast<double>(lo) / carta::sessionRate;
        const double hiTime = static_cast<double>(hi) / carta::sessionRate;
        const double rawTime = juce::jlimit(loTime, hiTime, xToTime(clampedX, area, span));
        juce::int64 sample = static_cast<juce::int64>(std::llround(rawTime * carta::sessionRate));
        if (allowSnap) {
            const auto snapped = carta::snapTimelinePosition(rawTime, loTime, hiTime, true, radius,
                                                             targets, 0.0, snapGridSamples);
            snapGuideVisible = snapped.hasGuide;
            snapGuideTime = snapped.guide;
            sample = static_cast<juce::int64>(std::llround(snapped.time * carta::sessionRate));
        }
        sample = std::clamp(sample, lo, hi);
        ok = carta::moveMdSharedBoundary(nextDisc, mdDragTrack0, mdDragPart0, mdDragRightTrack0,
                                         mdDragRightPart0, sample, mdDragMaxSample, mdErr);
        if (ok) {
            summary = "Shared MD split at sample " + juce::String(sample) +
                      (mods.isShiftDown() ? " (snap bypassed)" : "");
        }
    } else if (mdDragGesture == MdRibbonGesture::leftEdge ||
               mdDragGesture == MdRibbonGesture::rightEdge) {
        const bool rightEdge = (mdDragGesture == MdRibbonGesture::rightEdge);
        if (mdDragTrack0 < 0 || mdDragTrack0 >= static_cast<int>(mdDragBeforeDisc.tracks.size())) {
            return;
        }
        const auto& srcParts = mdDragBeforeDisc.tracks[static_cast<size_t>(mdDragTrack0)].parts;
        if (mdDragPart0 < 0 || mdDragPart0 >= static_cast<int>(srcParts.size())) {
            return;
        }
        const juce::int64 lo =
            rightEdge ? srcParts[static_cast<size_t>(mdDragPart0)].startSample + 1 : 0;
        const juce::int64 hi =
            rightEdge ? mdDragMaxSample
                      : std::max<juce::int64>(
                            0, srcParts[static_cast<size_t>(mdDragPart0)].endSample - 1);
        const double loTime = static_cast<double>(lo) / carta::sessionRate;
        const double hiTime = static_cast<double>(hi) / carta::sessionRate;
        const double rawTime = juce::jlimit(loTime, hiTime, xToTime(clampedX, area, span));
        juce::int64 sample = static_cast<juce::int64>(std::llround(rawTime * carta::sessionRate));
        if (allowSnap) {
            const auto snapped = carta::snapTimelinePosition(rawTime, loTime, hiTime, true, radius,
                                                             targets, 0.0, snapGridSamples);
            snapGuideVisible = snapped.hasGuide;
            snapGuideTime = snapped.guide;
            sample = static_cast<juce::int64>(std::llround(snapped.time * carta::sessionRate));
        }
        sample = std::clamp(sample, lo, hi);
        ok = carta::trimMdPartEdge(nextDisc, mdDragTrack0, mdDragPart0, rightEdge, sample,
                                   mdDragMaxSample, mdErr);
        if (ok) {
            summary = (rightEdge ? "MD track end at sample " : "MD track start at sample ") +
                      juce::String(sample);
        }
    } else if (mdDragGesture == MdRibbonGesture::slideBody ||
               mdDragGesture == MdRibbonGesture::duplicateAlias) {
        const juce::int64 len = mdDragOrigEnd - mdDragOrigStart;
        if (len <= 0 || mdDragMaxSample < len) {
            return;
        }
        const juce::int64 rawPointerSample = static_cast<juce::int64>(
            std::llround(xToTime(clampedX, area, span) * carta::sessionRate));
        const juce::int64 delta = rawPointerSample - mdDragAnchorSample;
        const juce::int64 maxStart = std::max<juce::int64>(0, mdDragMaxSample - len);
        juce::int64 newStart = std::clamp(mdDragOrigStart + delta, juce::int64{0}, maxStart);
        const double durSec = static_cast<double>(len) / carta::sessionRate;
        const double rawStartTime = static_cast<double>(newStart) / carta::sessionRate;
        const double maxStartTime = static_cast<double>(maxStart) / carta::sessionRate;
        if (allowSnap) {
            const auto snapped = carta::snapTimelinePosition(
                rawStartTime, 0.0, maxStartTime, true, radius, targets, durSec, snapGridSamples);
            snapGuideVisible = snapped.hasGuide;
            snapGuideTime = snapped.guide;
            newStart = std::clamp(
                static_cast<juce::int64>(std::llround(snapped.time * carta::sessionRate)),
                juce::int64{0}, maxStart);
        }
        if (mdDragGesture == MdRibbonGesture::slideBody) {
            ok = carta::slideMdPartRange(nextDisc, mdDragTrack0, mdDragPart0, newStart,
                                         mdDragMaxSample, mdErr);
            if (ok) {
                summary = "MD track moved to [" + juce::String(newStart) + ", " +
                          juce::String(newStart + len) + ")";
            }
        } else {
            if (mdDragTrack0 < 0 ||
                mdDragTrack0 >= static_cast<int>(mdDragBeforeDisc.tracks.size())) {
                return;
            }
            const auto& srcTrack = mdDragBeforeDisc.tracks[static_cast<size_t>(mdDragTrack0)];
            carta::MdTrack copyTrack = srcTrack;
            copyTrack.id = carta::makeMdTrackId();
            const auto baseTitle = srcTrack.halfWidthTitle.empty()
                                       ? ("Track " + std::to_string(mdDragTrack0 + 1))
                                       : srcTrack.halfWidthTitle;
            copyTrack.halfWidthTitle = "Copy of " + baseTitle;
            if (!copyTrack.fullWidthTitle.empty()) {
                copyTrack.fullWidthTitle = "Copy of " + copyTrack.fullWidthTitle;
            }
            copyTrack.parts = {carta::MdPart{newStart, newStart + len}};
            const auto insertedId = juce::String::fromUTF8(copyTrack.id.c_str());
            ok = carta::insertMdTrack(nextDisc, mdDragTrack0 + 2, std::move(copyTrack), mdErr);
            if (ok) {
                mdDragFocusTrackId = insertedId;
                summary = "Shared MD track duplicated at [" + juce::String(newStart) + ", " +
                          juce::String(newStart + len) + ")";
            }
        }
    }

    if (ok) {
        const bool equalToBefore = (nextDisc == mdDragBeforeDisc);
        session.mdDisc = std::move(nextDisc);
        mdDragChanged = !equalToBefore;
        if (mdDragChanged) {
            dragged = true;
        }
        if (summary.isNotEmpty()) {
            status.setText(summary, juce::dontSendNotification);
        }
        if (discWorkspaceVisible) {
            const auto sel = timeSelection;
            const double pos = position;
            const auto gen = editGeneration;
            const auto focusId = mdDragFocusTrackId;
            const juce::ScopedUnlock unlock(lock);
            discWorkspace.update(session, sel, pos, gen, focusId);
        }
        repaint();
    } else if (!mdErr.empty()) {
        status.setText(juce::String::fromUTF8(mdErr.c_str()), juce::dontSendNotification);
        repaint();
    }
}

// ============================================================================
// Primary Mouse Event Dispatch (Down / Drag / Up / DoubleClick / Wheel)
// ============================================================================

void Studio::mouseDown(const juce::MouseEvent& e) {
    grabKeyboardFocus();
    const juce::ScopedLock guard(lock);
    auto area = timeline();

    // 1. MiniDisc track ribbon interactions (context menu, double-click, drag).
    if (mdRibbonArea().contains(e.getPosition())) {
        if (e.x < area.getX() + labelWidth) {
            if (!e.mods.isPopupMenu() && !e.mods.isAnyModifierKeyDown()) {
                clearAudioSelection(false);
            }
            return;
        }
        const auto info = classifyMdRibbonPointer(e.getPosition(), e.mods);
        if (e.mods.isPopupMenu()) {
            showMdRibbonContextMenu(e, info);
            return;
        }
        if (e.getNumberOfClicks() >= 2) {
            if (info.trackIndex >= 0) {
                {
                    const juce::ScopedUnlock unlock(lock);
                    discWorkspace.selectTrackById(info.trackId, true);
                }
                status.setText("Focused MD Track " + juce::String(info.trackIndex + 1) + ": " +
                                   info.title,
                               juce::dontSendNotification);
                if (discWorkspace.onSeekPart) {
                    const auto trackId = info.trackId;
                    const int partIdx = std::max(0, info.partIndex);
                    const auto rev = editGeneration;
                    {
                        const juce::ScopedUnlock unlock(lock);
                        discWorkspace.onSeekPart(trackId, partIdx, rev);
                    }
                }
                repaint();
            } else if (info.gesture == MdRibbonGesture::emptySpace) {
                const juce::int64 clickSample = std::max<juce::int64>(0, sampleAtX(e.x));
                juce::int64 gapStart = 0;
                juce::int64 gapEnd = carta::projectEndSample(session);
                for (const auto& tr : session.mdDisc.tracks) {
                    for (const auto& pt : tr.parts) {
                        gapEnd = std::max<juce::int64>(gapEnd, pt.endSample);
                    }
                }
                for (const auto& tr : session.mdDisc.tracks) {
                    for (const auto& pt : tr.parts) {
                        if (pt.endSample <= clickSample) {
                            gapStart = std::max<juce::int64>(gapStart, pt.endSample);
                        }
                        if (pt.startSample >= clickSample) {
                            gapEnd = std::min<juce::int64>(gapEnd, pt.startSample);
                        }
                    }
                }

                juce::int64 addStart = gapStart;
                juce::int64 addEnd = gapEnd;
                juce::String newTitle;
                for (const auto& layer : session.tracks) {
                    for (const auto& clip : layer.clips) {
                        juce::int64 cs = 0;
                        juce::int64 ce = 0;
                        if (!carta::clipSampleRange(clip, cs, ce)) {
                            continue;
                        }
                        if (cs <= clickSample && clickSample < ce) {
                            const auto clippedStart = std::max<juce::int64>(cs, gapStart);
                            const auto clippedEnd = std::min<juce::int64>(ce, gapEnd);
                            if (clippedEnd > clippedStart) {
                                addStart = clippedStart;
                                addEnd = clippedEnd;
                                newTitle = carta::clipDisplayName(clip).trim();
                                break;
                            }
                        }
                    }
                    if (newTitle.isNotEmpty()) {
                        break;
                    }
                }

                if (addEnd > addStart) {
                    int insIdx = 0;
                    for (const auto& tr : session.mdDisc.tracks) {
                        if (!tr.parts.empty() && tr.parts.front().startSample < addStart) {
                            ++insIdx;
                        } else {
                            break;
                        }
                    }
                    if (newTitle.isEmpty()) {
                        newTitle = "Track " + juce::String(insIdx + 1);
                    }
                    const auto rev = editGeneration;
                    const juce::ScopedUnlock unlock(lock);
                    if (discWorkspace.onCommand) {
                        discWorkspace.onCommand({"disc", "track", "add", juce::String(addStart),
                                                 juce::String(addEnd), newTitle,
                                                 juce::String(insIdx)},
                                                rev);
                    }
                }
            }
            return;
        }

        if (info.gesture == MdRibbonGesture::emptySpace || info.trackIndex < 0 ||
            info.partIndex < 0) {
            if (!e.mods.isAnyModifierKeyDown()) {
                clearAudioSelection(false);
            }
            if (!selectionPlaybackContinues()) {
                juce::int64 maxEnd = carta::projectEndSample(session);
                for (const auto& tr : session.mdDisc.tracks) {
                    for (const auto& pt : tr.parts) {
                        maxEnd = std::max<juce::int64>(maxEnd, pt.endSample);
                    }
                }
                position = static_cast<double>(std::clamp<juce::int64>(
                               sampleAtX(e.x), 0, std::max<juce::int64>(0, maxEnd))) /
                           carta::sessionRate;
                auditionActive = false;
                repaint();
            }
            return;
        }

        {
            const juce::ScopedUnlock unlock(lock);
            discWorkspace.selectTrackById(info.trackId, true);
        }
        dragBeforeState = captureSelectionState();
        {
            const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
            if (rangePreparationHook) {
                rangePreparationHook();
            }
#endif
            dragBeforeState.session = session;
        }
        mdDragBeforeDisc = session.mdDisc;
        mdDragGesture = info.gesture;
        mdDragTrack0 = info.trackIndex;
        mdDragPart0 = info.partIndex;
        mdDragRightTrack0 = info.rightTrackIndex;
        mdDragRightPart0 = info.rightPartIndex;
        mdDragFocusTrackId = info.trackId;
        mdDragChanged = false;
        mdDragPointer = e.getPosition();
        mdDragAnchorSample = sampleAtX(e.x);
        dragX = e.x;
        const auto& origPart = session.mdDisc.tracks[static_cast<size_t>(mdDragTrack0)]
                                   .parts[static_cast<size_t>(mdDragPart0)];
        mdDragOrigStart = origPart.startSample;
        mdDragOrigEnd = origPart.endSample;

        juce::int64 maxEnd = carta::projectEndSample(session);
        for (const auto& tr : session.mdDisc.tracks) {
            for (const auto& pt : tr.parts) {
                maxEnd = std::max<juce::int64>(maxEnd, pt.endSample);
            }
        }
        mdDragMaxSample = std::max<juce::int64>(1, maxEnd);
        mdDragActionLabel = mdDragGesture == MdRibbonGesture::sharedSplit ? "Move MD track split"
                            : (mdDragGesture == MdRibbonGesture::leftEdge ||
                               mdDragGesture == MdRibbonGesture::rightEdge)
                                ? "Trim MD track boundary"
                            : mdDragGesture == MdRibbonGesture::duplicateAlias
                                ? "Duplicate MD track"
                                : "Slide MD track";
        {
            // Exclude the boundary/part being dragged so it does not snap to itself.
            carta::Session snapSession;
            snapSession.tracks = session.tracks;
            snapSession.markers = session.markers;
            snapSession.mdDisc = mdDragBeforeDisc;
            if (mdDragGesture == MdRibbonGesture::sharedSplit && mdDragTrack0 >= 0 &&
                mdDragRightTrack0 >= 0) {
                auto& lParts = snapSession.mdDisc.tracks[static_cast<size_t>(mdDragTrack0)].parts;
                auto& rParts =
                    snapSession.mdDisc.tracks[static_cast<size_t>(mdDragRightTrack0)].parts;
                lParts[static_cast<size_t>(mdDragPart0)].endSample =
                    rParts[static_cast<size_t>(mdDragRightPart0)].endSample;
                rParts.erase(rParts.begin() + mdDragRightPart0);
            } else if (mdDragGesture == MdRibbonGesture::leftEdge ||
                       mdDragGesture == MdRibbonGesture::rightEdge ||
                       mdDragGesture == MdRibbonGesture::slideBody) {
                auto& parts = snapSession.mdDisc.tracks[static_cast<size_t>(mdDragTrack0)].parts;
                parts.erase(parts.begin() + mdDragPart0);
            }
            const auto snapSel = timeSelection;
            const double snapPos = position;
            const juce::ScopedUnlock unlock(lock);
            dragTargets = carta::timelineSnapTargets(snapSession, snapSel, snapPos, -1, -1, true,
                                                     true, true, snapOptions);
        }

        dragMode = kDragMdRibbon;
        dragged = false;
        snapGuideVisible = false;
        status.setText(
            mdDragGesture == MdRibbonGesture::sharedSplit
                ? "Drag shared MD track split; hold Shift to bypass snap, Escape cancels."
            : mdDragGesture == MdRibbonGesture::duplicateAlias
                ? "Drag to duplicate shared MD track; Shift bypasses snap, Escape cancels."
            : mdDragGesture == MdRibbonGesture::slideBody
                ? "Drag to move MD track; Option/Alt duplicates, Shift bypasses snap, Escape "
                  "cancels."
                : "Drag MD track boundary; Shift bypasses snap, Escape cancels.",
            juce::dontSendNotification);
        repaint();
        return;
    }

    if (!area.contains(e.getPosition())) {
        if (!e.mods.isPopupMenu() && !e.mods.isAnyModifierKeyDown()) {
            clearAudioSelection(false);
        }
        return;
    }

    // 2. Reset transient gesture state before classifying timeline targets.
    selectionClipSnapshotValid = false;
    keyboardTracks.active = false;
    keyboardRange.active = false;
    draggingFade = false;
    draggingMasterEnd = false;
    selectionPointerDragging = false;
    clipPointerDragging = false;
    playheadPointerDragging = false;
    copyGestureCandidate = false;
    groupDragCopy = false;
    selectionVerticalDirection = 0;
    selectionVerticalElapsed = 0;
    clipVerticalDirection = 0;
    clipVerticalElapsed = 0;
    clipDropTrack = -1;
    if (e.getNumberOfClicks() == 1) {
        doubleClickTrack = selectedTrack;
        doubleClickClip = selectedClip;
        doubleClickRevision = editGeneration;
    }

    // 3. Command/Control+Shift+drag across clip bodies starts marquee clip selection.
    if (!e.mods.isPopupMenu() && e.mods.isCommandDown() && e.mods.isShiftDown() &&
        !e.mods.isAltDown() && e.x >= area.getX() + labelWidth &&
        e.y >= area.getY() + timelineHeaderHeight()) {
        const auto revision = editGeneration;
        bool body = false;
        double anchorTime = 0;
        juce::int64 anchorY = 0;
        {
            const juce::ScopedUnlock unlock(lock);
            const int track = trackAt(e.y);
            if (track >= 0 && e.y >= trackArea(track).getY() + 4 + titleHeight) {
#if CARTA_STUDIO_COMPONENT_TEST
                if (scopeSelectionHook) {
                    scopeSelectionHook();
                }
#endif
                body = true;
                anchorTime = xToTime(e.x);
                anchorY = marqueeDocumentY(e.y);
            }
        }
        if (revision != editGeneration || dragMode != kDragNone) {
            status.setText("The project changed; start the selection again.",
                           juce::dontSendNotification);
            return;
        }
        if (body) {
            marqueeStart = e.getPosition();
            marqueeEnd = marqueeStart;
            marqueeAnchorTime = anchorTime;
            marqueeAnchorY = anchorY;
            marqueeRevision = revision;
            marqueePreview.clear();
            marqueePreviewValid = false;
            marqueeScrollClock = juce::Time::getMillisecondCounterHiRes();
            marqueeVerticalElapsed = 0;
            marqueeVerticalDirection = 0;
            dragMode = kDragMarquee;
            dragged = false;
            status.setText("Drag across clip bodies to select clips; Escape cancels.",
                           juce::dontSendNotification);
            repaint();
            return;
        }
    }

    stopSpecialAudition();
    selectionBefore = timeSelection;
    selectionBeforePosition = position;
    selectionGesturePlayback = selectionPlaybackContinues();
    selectionBeforeTrack = selectedTrack;
    selectionBeforeClip = selectedClip;
    selectionBeforeMarker = selectedMarker;

    if (e.mods.isPopupMenu()) {
        popupPending = true;
        dragMode = kDragNone;
        dragged = false;
        return;
    }

    // 4. Scale-gutter channel-height splitter drag.
    if (!e.mods.isAnyModifierKeyDown()) {
        const auto boundary = channelBoundaryAt(e.getPosition());
        if (boundary.track >= 0) {
            const auto& track = session.tracks[static_cast<size_t>(boundary.track)];
            channelDragTrack = boundary.track;
            channelDragLane = boundary.lane;
            channelDragStartY = e.y;
            channelDragPairPixels = boundary.pairBottom - boundary.pairTop;
            channelDragOriginal = track.channelHeightWeights;
            const int count = carta::maxTrackChannels(track);
            channelDragScaled = static_cast<int>(channelDragOriginal.size()) == count &&
                                        carta::validChannelHeightWeights(channelDragOriginal)
                                    ? channelDragOriginal
                                    : std::vector<int>(static_cast<size_t>(count), 1);

            int common = 0;
            for (int weight : channelDragScaled) {
                common = std::gcd(common, weight);
            }
            for (auto& weight : channelDragScaled) {
                weight /= common;
            }
            const int largest =
                *std::max_element(channelDragScaled.begin(), channelDragScaled.end());
            if (largest <= 500) {
                const int scale = 500 / largest;
                for (auto& weight : channelDragScaled) {
                    weight *= scale;
                }
            } else {
                for (auto& weight : channelDragScaled) {
                    weight = std::max(1, static_cast<int>(std::lround(static_cast<double>(weight) *
                                                                      500 / largest)));
                }
            }

            dragBeforeState = captureSelectionState();
            {
                const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
                if (rangePreparationHook) {
                    rangePreparationHook();
                }
#endif
                dragBeforeState.session = session;
            }
            dragMode = kDragChannelHeight;
            dragged = false;
            status.setText("Drag channel boundary; Escape cancels.", juce::dontSendNotification);
            return;
        }
    }

    // 5. Command/Control+Shift+click on track header or clip title toggles independent selection.
    if (e.mods.isCommandDown() && e.mods.isShiftDown() && !e.mods.isAltDown() &&
        e.x < area.getX() + trackLabelWidth && e.y >= area.getY() + timelineHeaderHeight() &&
        trackResizeAt(e.getPosition()) < 0) {
        const int track = trackAt(e.y);
        if (track >= 0) {
            toggleTimelineTrack(track);
            return;
        }
    }

    if (e.mods.isCommandDown() && e.mods.isShiftDown() && !e.mods.isAltDown() &&
        e.y >= area.getY() + timelineHeaderHeight()) {
        const auto revision = editGeneration;
        int track = -1;
        int clip = -1;
        bool valid = false;
        carta::ClipSelection selection;
        {
            const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
            if (scopeSelectionHook) {
                scopeSelectionHook();
            }
#endif
            track = trackAt(e.y);
            if (track >= 0 && track < static_cast<int>(session.tracks.size())) {
                const auto row = trackArea(track);
                if (e.y >= row.getY() + 4 && e.y < row.getY() + 4 + titleHeight) {
                    for (const auto& region : titleRegions(track)) {
                        if (region.bounds.contains(e.x)) {
                            clip = region.clip;
                            break;
                        }
                    }
                    if (clip >= 0) {
                        selection = timelineClipSelection;
                        valid = selection.toggleGroup(session, {track, clip}, revision);
                    }
                }
            }
        }
        if (revision != editGeneration || dragMode != kDragNone) {
            status.setText("The project changed; select the clip again.",
                           juce::dontSendNotification);
            return;
        }
        if (clip >= 0) {
            if (!valid) {
                status.setText("Could not select the clip group.", juce::dontSendNotification);
                return;
            }
            timelineClipSelection = std::move(selection);
            const auto& chosen = timelineClipSelection.get(revision);
            if (chosen.empty()) {
                selectedTrack = track;
                selectedClip = -1;
            } else if (timelineClipSelection.contains(track, clip, revision)) {
                selectedTrack = track;
                selectedClip = clip;
            } else {
                selectedTrack = chosen.back().track;
                selectedClip = chosen.back().clip;
            }
            selectedMarker = -1;
            dragMode = kDragNone;
            dragged = false;
            const auto message = juce::String(chosen.size()) +
                                 (chosen.size() == 1 ? " clip selected. " : " clips selected. ") +
                                 "Use Edit > Selected clips for group actions.";
            status.setText(message, juce::dontSendNotification);
            announceNavigation(message);
            menuItemsChanged();
            repaint();
            return;
        }
    }

    // 6. Ruler bar interactions: loop handles, master end, markers, playhead seek.
    if (e.y < area.getY() + kTimelineRulerHeight) {
        if (e.x < area.getX() + labelWidth) {
            if (!e.mods.isPopupMenu() && !e.mods.isAnyModifierKeyDown()) {
                clearAudioSelection(false);
            }
            return;
        }

        const int loopEdge = (!e.mods.isAltDown() && !e.mods.isCommandDown())
                                 ? savedLoopHandleAt(e.getPosition())
                                 : 0;
        if (loopEdge != 0) {
            loopDragOriginal = savedLoop;
            loopDragPreview = savedLoop;
            loopDragEnd = loopEdge > 0;
            loopPointerDragging = false;
            const auto snapSelection = timeSelection;
            const auto snapPosition = position;
            {
                const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
                if (rangePreparationHook) {
                    rangePreparationHook();
                }
#endif
                dragTargets = carta::timelineSnapTargets(session, snapSelection, snapPosition, -1,
                                                         -1, true, true, true, snapOptions);
            }
            dragMode = kDragSavedLoop;
            dragged = false;
            status.setText("Drag loop boundary; Shift bypasses snapping and Escape cancels.",
                           juce::dontSendNotification);
            repaint();
            return;
        }

        if (!savedLoopAudition()) {
            auditionActive = false;
        }
        const int previousMarker = selectedMarker;
        selectedMarker = -1;
        if (masterEndHandleAt(e.getPosition())) {
            dragBeforeState = captureSelectionState();
            const double snapPosition = position;
            const auto snapSelection = timeSelection;
            {
                const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
                if (rangePreparationHook) {
                    rangePreparationHook();
                }
#endif
                dragBefore = session;
                dragBeforeState.session = dragBefore;
                dragFullLength = fullLength();
                dragSpan = dragFullLength / zoom;
                auto audioOnly = dragBefore;
                audioOnly.minimumEndSample = 0;
                dragMasterFloor = carta::projectEndSample(audioOnly);
                dragMasterOriginalEnd = carta::projectEndSample(dragBefore);
                for (const auto& boundary : dragBefore.markers) {
                    const double frame = boundary.position * carta::sessionRate;
                    if (std::isfinite(frame) && frame >= 0 &&
                        frame < static_cast<double>(dragMasterOriginalEnd)) {
                        const auto sample = static_cast<juce::int64>(std::llround(frame));
                        if (sample < dragMasterOriginalEnd) {
                            dragMasterFloor = std::max(dragMasterFloor, sample + 1);
                        }
                    }
                }
                dragTargets = carta::timelineSnapTargets(dragBefore, snapSelection, snapPosition,
                                                         -1, -1, true, true, false, snapOptions);
            }
            draggingMasterEnd = true;
            dragMode = kDragMarkerOrMasterEnd;
            dragged = false;
            status.setText("Drag the master end; clips and markers stay in place. Escape cancels.",
                           juce::dontSendNotification);
            repaint();
            return;
        }

        int nearby = 0;
        selectedMarker = markerHitAt(e.x, e.mods.isAltDown(), previousMarker, &nearby);
        if (selectedMarker >= 0) {
            dragMode = kDragMarkerOrMasterEnd;
            dragged = false;
            dragBeforeState = captureSelectionState();
            const double snapPosition = position;
            {
                const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
                if (rangePreparationHook) {
                    rangePreparationHook();
                }
#endif
                dragBefore = session;
                dragBeforeState.session = dragBefore;
                markerDragMasterEnd = carta::projectEndSample(session);
                dragTargets = carta::timelineSnapTargets(session, timeSelection, snapPosition, -1,
                                                         -1, false, true, true, snapOptions);
            }
            if (!selectionGesturePlayback) {
                position = session.markers[static_cast<size_t>(selectedMarker)].position;
            }
            const auto& name = session.markers[static_cast<size_t>(selectedMarker)].name;
            const auto message =
                "Marker: " + (name.trim().isEmpty() ? juce::String("unnamed") : name) +
                (nearby > 1 ? " (" + juce::String(nearby) + " nearby; Option/Alt-click cycles)."
                            : ".");
            status.setText(message, juce::dontSendNotification);
            announceNavigation(message);
        } else {
            auditionActive = false;
            playheadBefore = position;
            dragMode = kDragPlayhead;
            dragged = false;
            const auto snapSelection = timeSelection;
            double target = 0;
            {
                const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
                if (rangePreparationHook) {
                    rangePreparationHook();
                }
#endif
                dragTargets = carta::timelineSnapTargets(session, snapSelection,
                                                         std::numeric_limits<double>::quiet_NaN(),
                                                         -1, -1, true, true, true, snapOptions);
                target = static_cast<double>(snappedPointerSample(e.x, !e.mods.isShiftDown())) /
                         carta::sessionRate;
            }
            position = target;
        }
        repaint();
        return;
    }

    // 7. Persistent clip group gesture preparation & group move/edge drags.
    if (!e.mods.isCommandDown() && !e.mods.isAltDown() && trackResizeAt(e.getPosition()) < 0) {
        // Selection expansion is message-thread-owned and only reads the session.
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (groupSelectionHook) {
            groupSelectionHook();
        }
#endif
        preparePersistentGroupGesture(e.getPosition());
    }

    if (!e.mods.isCommandDown() && !e.mods.isAltDown() &&
        !timelineClipSelection.get(editGeneration).empty()) {
        int track = -1;
        int hit = -1;
        bool edgeHit = false;
        bool endEdge = false;
        std::vector<carta::ClipReference> chosen;
        {
            // Selection and geometry are message-thread-owned. Neither
            // scanning selected edges nor laying out titles changes audio.
            const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
            if (groupHitTestHook) {
                groupHitTestHook();
            }
#endif
            track = trackAt(e.y);
            if (track >= 0 && track < static_cast<int>(session.tracks.size())) {
                const auto& selected = timelineClipSelection.get(editGeneration);
                for (auto it = selected.rbegin(); it != selected.rend(); ++it) {
                    if (it->track == track) {
                        const auto rect = clipRect(session.tracks[static_cast<size_t>(track)]
                                                       .clips[static_cast<size_t>(it->clip)],
                                                   track);
                        if (rect.contains(e.getPosition()) &&
                            (e.x < rect.getX() + kClipEdgeHitPx ||
                             e.x > rect.getRight() - kClipEdgeHitPx)) {
                            hit = it->clip;
                            edgeHit = true;
                            endEdge = e.x > rect.getRight() - kClipEdgeHitPx;
                            break;
                        }
                    }
                }
                const auto row = trackArea(track);
                if (hit < 0 && e.y >= row.getY() + 4 && e.y < row.getY() + 4 + titleHeight) {
                    for (const auto& region : titleRegions(track)) {
                        if (region.bounds.contains(e.x) &&
                            timelineClipSelection.contains(track, region.clip, editGeneration)) {
                            hit = region.clip;
                            break;
                        }
                    }
                }
                if (hit >= 0) {
                    chosen = selected;
                }
            }
        }

        if (hit >= 0) {
            groupDragClips = std::move(chosen);
            groupDragGeneration = editGeneration;
            groupEdgeEnd = endEdge;
            groupDragAnchorTrack = track;
            groupDragAnchorClip = hit;
            const auto snapSelection = timeSelection;
            const auto snapPosition = position;
            bool prepared = false;
            std::vector<double> targets;
            double span = 0;
            double length = 0;
            {
                const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
                if (groupPreparationHook) {
                    groupPreparationHook();
                }
#endif
                if (edgeHit) {
                    prepared =
                        groupEdgePlan.prepare(session, groupDragClips, groupDragError) &&
                        groupEdgeMarkerPreview.prepare(session, groupDragClips, groupDragError);
                } else {
                    prepared = groupDragPreview.prepare(session, groupDragClips, groupDragError) &&
                               clipOverlapPreview.prepare(session, groupDragClips, groupDragError);
                }
                if (prepared) {
                    targets = carta::clipGroupSnapTargets(session, snapSelection, snapPosition,
                                                          groupDragClips, snapOptions);
                    span = viewLength();
                    length = fullLength();
                }
            }
            if (!prepared) {
                if (edgeHit) {
                    groupEdgePlan.clear();
                }
                groupDragClips.clear();
                restoreGestureClipSelection();
                status.setText(groupDragError, juce::dontSendNotification);
                return;
            }

            selectedTrack = track;
            selectedClip = hit;
            selectedMarker = -1;
            groupDragOriginal =
                session.tracks[static_cast<size_t>(track)].clips[static_cast<size_t>(hit)];
            groupDragSamples = 0;
            groupDragValid = true;
            groupDragError.clear();
            if (edgeHit) {
                groupEdgePreview.clear();
            } else {
                groupDragTracks = 0;
                groupDragOverlaps = false;
            }
            dragSpan = span;
            dragViewStart = viewStart;
            dragFullLength = length;
            dragX = e.x;
            dragTargets = std::move(targets);
            dragMode = edgeHit ? kDragGroupEdge : kDragGroupMove;
            dragged = false;
            status.setText(edgeHit ? "Drag selected clip edges together; Escape cancels."
                                   : "Drag selected clips; overlaps sum. Hold Shift to disable "
                                     "boundary snapping.",
                           juce::dontSendNotification);
            repaint();
            return;
        }
    }

    // A range gesture takes clip-group focus, but Escape must restore it.
    auto previousClipSelection = std::move(timelineClipSelection);
    timelineClipSelection = {};

    // 8. Track row resize and track header controls (collapse/M/S/gain/balance/reorder).
    const int resizeTrack = trackResizeAt(e.getPosition());
    if (resizeTrack >= 0) {
        timelineClipSelection = std::move(previousClipSelection);
        resizedTrack = resizeTrack;
        resizeStartY = e.y;
        resizeStartHeight = session.tracks[static_cast<size_t>(resizeTrack)].height;
        dragBeforeState = captureSelectionState();
        {
            const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
            if (rangePreparationHook) {
                rangePreparationHook();
            }
#endif
            dragBeforeState.session = session;
        }
        dragMode = kDragTrackHeight;
        dragged = false;
        return;
    }

    if (e.x < area.getX() + trackLabelWidth && !e.mods.isAnyModifierKeyDown()) {
        const int track = trackAt(e.y);
        if (track >= 0) {
            const int headerHit = trackHeaderHit(e.getPosition());
            if (headerHit >= HeaderHit::collapse && headerHit <= HeaderHit::solo) {
                toggleHeaderTrackControl(track, headerHit);
                return;
            }
            if (headerHit == HeaderHit::gain || headerHit == HeaderHit::balance) {
                dragMode = kDragHeaderControl;
                headerDragTrack = track;
                headerDragGain = (headerHit == HeaderHit::gain);
                headerDragAnchor = e.getPosition();
                headerDragMoved = false;
                dragged = false;
                const auto& tr = session.tracks[static_cast<size_t>(track)];
                headerDragStartValue = headerDragValue = headerDragGain ? tr.gain : tr.pan;
                setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
                status.setText(
                    headerDragGain
                        ? "Drag left/right to adjust track gain; double-click resets to 0 dB; "
                          "Escape cancels."
                        : "Drag left/right to adjust track balance; double-click centers; Escape "
                          "cancels.",
                    juce::dontSendNotification);
                repaint();
                return;
            }

            timelineClipSelection = std::move(previousClipSelection);
            dragBeforeState = captureSelectionState();
            trackDragSource = track;
            trackDragSlot = track;
            trackDragPointer = e.getPosition();
            trackDragMoved = false;
            trackDragClock = juce::Time::getMillisecondCounterHiRes();
            trackDragScrollElapsed = 0;
            trackDragScrollDirection = 0;
            selectedTrack = track;
            selectedClip = -1;
            timeSelection = {};
            timelineClipSelection.clear();
            dragMode = kDragTrackReorder;
            status.setText("Drag the track header to reorder; Escape cancels.",
                           juce::dontSendNotification);
            repaint();
            return;
        }
    }

    // 9. Fade handles, time-selection edges, clip body time selection, or clip move/trim.
    const int fade = fadeHandleAt(e.getPosition());
    if (fade != 0 && !e.mods.isAltDown() && !e.mods.isCommandDown()) {
        selectionBeforeClips = std::move(previousClipSelection);
        selectionClipSnapshotValid = true;
        dragOriginal = session.tracks[static_cast<size_t>(selectedTrack)]
                           .clips[static_cast<size_t>(selectedClip)];
        dragBeforeState = captureSelectionState();
        dragViewStart = viewStart;
        dragX = e.x;
        {
            const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
            if (singleClipPreparationHook) {
                singleClipPreparationHook();
            }
#endif
            dragBefore = session;
            dragBeforeState.session = dragBefore;
            dragFullLength = fullLength();
            dragSpan = dragFullLength / zoom;
        }
        draggingFade = true;
        dragMode = fade < 0 ? kDragClipTrimLeft : kDragClipTrimRight;
        dragged = false;
        status.setText("Drag fade handle; Escape cancels.", juce::dontSendNotification);
        return;
    }

    const int t = trackAt(e.y);
    if (t < 0) {
        timelineClipSelection = std::move(previousClipSelection);
        if (!e.mods.isPopupMenu() && !e.mods.isAnyModifierKeyDown()) {
            clearAudioSelection(false);
        }
        return;
    }

    selectionBeforeTracks.clear();
    for (int track : selectedTimelineTracks()) {
        selectionBeforeTracks.insert(track);
    }

    const int selectionEdge = e.mods.isAltDown() ? 0 : selectionEdgeAt(e.getPosition());
    if (selectionEdge != 0) {
        selectionBeforeClips = std::move(previousClipSelection);
        selectionClipSnapshotValid = true;
        selectionResizeBase = selectionBefore;
        if (!selectionBeforeTracks.empty()) {
            selectionResizeBase.firstTrack = *selectionBeforeTracks.begin();
            selectionResizeBase.lastTrack = *selectionBeforeTracks.rbegin();
        }
        dragMode = kDragTimeSelectionResize;
        selectionResizeLeft = selectionEdge < 0;
        selectedTrack = t;
        selectedClip = -1;
        const double snapPosition = position;
        {
            const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
            if (rangePreparationHook) {
                rangePreparationHook();
            }
#endif
            dragTargets = carta::timelineSnapTargets(session, {}, snapPosition, -1, -1, true, false,
                                                     true, snapOptions);
        }
        dragged = false;
        status.setText("Drag the selection edge; crossing reverses the range.",
                       juce::dontSendNotification);
        repaint();
        return;
    }

    const int previousTrack = selectedTrack;
    const int previousClip = selectedClip;
    selectedTrack = t;
    selectedClip = -1;
    dragMode = kDragNone;
    auto& clips = session.tracks[static_cast<size_t>(t)].clips;

    // The clip body creates a time selection; title strips move clips.
    // Edge handles retain priority so trimming stays possible.
    bool edge = false;
    bool title = false;
    int bodyHit = -1;
    int titleHit = -1;
    std::vector<int> hits;
    {
        // The arrangement and view are owned by the message thread;
        // playback only reads them while this geometry is calculated.
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (clipHitTestHook) {
            clipHitTestHook();
        }
#endif
        for (int i = static_cast<int>(clips.size()) - 1; i >= 0; --i) {
            const auto rect = clipRect(clips[static_cast<size_t>(i)], t);
            if (!rect.contains(e.getPosition())) {
                continue;
            }
            hits.push_back(i);
            if (bodyHit < 0 || (previousTrack == t && previousClip == i)) {
                bodyHit = i;
            }
            if (e.y < rect.getY() + titleHeight) {
                title = true;
            }
        }
        // Match the focused/topmost clip that the cursor and subsequent
        // drag target, even when another clip's edge is obscured.
        if (bodyHit >= 0) {
            const auto rect = clipRect(clips[static_cast<size_t>(bodyHit)], t);
            edge = e.x < rect.getX() + kClipEdgeHitPx || e.x > rect.getRight() - kClipEdgeHitPx;
            if (e.y < clipRect(clips.front(), t).getY() + titleHeight) {
                for (const auto& region : titleRegions(t)) {
                    if (region.bounds.contains(e.x)) {
                        titleHit = region.clip;
                        break;
                    }
                }
            }
        }
    }

    if (e.x >= area.getX() + labelWidth && !title && !edge && !e.mods.isAltDown()) {
        const bool keepPlayback = selectionPlaybackContinues();
        if (!keepPlayback) {
            auditionActive = false;
        }
        const double snapPosition = position;
        {
            const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
            if (rangePreparationHook) {
                rangePreparationHook();
            }
#endif
            dragTargets = carta::timelineSnapTargets(session, {}, snapPosition, -1, -1, true, false,
                                                     true, snapOptions);
            selectionAnchorSample = snappedPointerSample(e.x, !e.mods.isShiftDown());
        }
        selectionAnchorTrack = t;
        selectionBeforeClips = std::move(previousClipSelection);
        selectionClipSnapshotValid = true;
        timelineTracks.clear();
        timeSelection = {selectionAnchorSample, selectionAnchorSample, t, t};
        if (!keepPlayback) {
            position = static_cast<double>(selectionAnchorSample) / carta::sessionRate;
        }
        dragMode = kDragTimeSelection;
        dragged = false;
        status.setText("Edit point: sample " + juce::String(selectionAnchorSample) +
                           ". Drag to select time; split uses this point.",
                       juce::dontSendNotification);
        repaint();
        return;
    }

    dragSnapSelection = timeSelection;
    if (!e.mods.isCommandDown()) {
        timeSelection = {};
    }

    if (!hits.empty()) {
        int chosen = hits.front();
        const bool clickedTitle = !e.mods.isAltDown() && titleHit >= 0;
        if (clickedTitle) {
            chosen = titleHit;
        }
        if (e.mods.isAltDown() && previousTrack == t) {
            const auto it = std::find(hits.begin(), hits.end(), previousClip);
            if (it != hits.end()) {
                chosen = *(std::next(it) == hits.end() ? hits.begin() : std::next(it));
            }
        } else if (!clickedTitle && previousTrack == t &&
                   std::find(hits.begin(), hits.end(), previousClip) != hits.end()) {
            chosen = previousClip;
        }

        const int i = chosen;
        selectionBeforeClips = std::move(previousClipSelection);
        selectionClipSnapshotValid = true;
        selectedClip = i;
        const auto rect = clipRect(clips[static_cast<size_t>(i)], t);
        dragMode = e.x < rect.getX() + kClipEdgeHitPx       ? kDragClipTrimLeft
                   : e.x > rect.getRight() - kClipEdgeHitPx ? kDragClipTrimRight
                                                            : kDragClipMove;
        copyGestureAnchor = titleHit;
        const auto anchorRect = titleHit >= 0 ? clipRect(clips[static_cast<size_t>(titleHit)], t)
                                              : juce::Rectangle<int>{};
        bool copyModifier = e.mods.isAltDown();
#if JUCE_LINUX
        // Many Linux window managers intercept Alt-drag to move a window.
        // Control-drag offers a gesture the application can receive; the
        // Control+Shift title-selection gesture keeps its existing meaning.
        copyModifier = copyModifier || (e.mods.isCtrlDown() && !e.mods.isShiftDown());
#endif
        copyGestureCandidate = copyModifier && titleHit >= 0 &&
                               e.x >= anchorRect.getX() + kClipEdgeHitPx &&
                               e.x <= anchorRect.getRight() - kClipEdgeHitPx;
        if (copyGestureCandidate) {
            dragMode = kDragClipMove;
        }
        dragOriginal = clips[static_cast<size_t>(i)];
        dragX = e.x;
        dragViewStart = viewStart;
        dragged = false;
        dragBeforeState = captureSelectionState();
        clipOverlapReady = false;
        {
            const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
            if (singleClipPreparationHook) {
                singleClipPreparationHook();
            }
#endif
            dragFullLength = fullLength();
            dragSpan = dragFullLength / zoom;
            dragBefore = session;
            dragBeforeState.session = dragBefore;
            prepareClipDragEnd();
            dragTargets = carta::timelineSnapTargets(dragBefore, dragSnapSelection,
                                                     dragBeforeState.position, selectedTrack,
                                                     selectedClip, true, true, false, snapOptions);
            if (dragMode == kDragClipMove) {
                juce::String error;
                clipOverlapReady =
                    clipOverlapPreview.prepare(dragBefore, {{selectedTrack, selectedClip}}, error);
            }
        }
        status.setText("Selected " + carta::clipDisplayName(clips[static_cast<size_t>(i)]) +
                           ". Double-click to zoom; drag track edge to resize.",
                       juce::dontSendNotification);
    }

    if (selectedClip < 0 && e.x >= timeline().getX() + labelWidth) {
        position = xToTime(e.x);
    }
    repaint();
}

void Studio::mouseDrag(const juce::MouseEvent& e) {
    if (dragMode == kDragMarquee) {
        marqueeEnd = e.getPosition();
        dragged = dragged || marqueeStart.getDistanceFrom(marqueeEnd) >= 3.0;
        updateMarqueePreview();
        repaint();
        return;
    }

    if (dragMode == kDragClipMove && copyGestureCandidate) {
        if (e.getDistanceFromDragStart() < 4) {
            return;
        }
        const juce::ScopedLock guard(lock);
        const int track = selectedTrack;
        const int clip = copyGestureAnchor;
        const auto revision = editGeneration;
        std::vector<carta::ClipReference> requested;
        carta::ClipSelection expanded;
        bool prepared = false;
        std::vector<double> targets;
        double span = 0;
        double length = 0;
        {
            const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
            if (groupSelectionHook) {
                groupSelectionHook();
            }
#endif
            if (selectionBeforeClips.contains(track, clip, revision)) {
                requested = selectionBeforeClips.get(revision);
            } else {
                requested.push_back({track, clip});
            }
            prepared = expanded.selectExpanded(session, requested, revision);
#if CARTA_STUDIO_COMPONENT_TEST
            if (groupPreparationHook) {
                groupPreparationHook();
            }
#endif
            if (prepared) {
                groupDragClips = expanded.get(revision);
                prepared = groupDragPreview.prepare(session, groupDragClips, groupDragError) &&
                           clipOverlapPreview.prepare(session, groupDragClips, groupDragError);
            }
            if (prepared) {
                targets =
                    carta::clipGroupSnapTargets(session, dragSnapSelection, selectionBeforePosition,
                                                groupDragClips, snapOptions);
                span = dragSpan;
                length = dragFullLength;
            }
        }
        if (!prepared || revision != editGeneration) {
            groupDragClips.clear();
            timeSelection = selectionBefore;
            selectedTrack = selectionBeforeTrack;
            selectedClip = selectionBeforeClip;
            selectedMarker = selectionBeforeMarker;
            restoreGestureClipSelection();
            dragMode = kDragNone;
            copyGestureCandidate = false;
            status.setText(groupDragError.isNotEmpty()
                               ? groupDragError
                               : "The clip selection changed; try copying again.",
                           juce::dontSendNotification);
            return;
        }

        timelineClipSelection = std::move(expanded);
        selectedClip = clip;
        groupDragGeneration = revision;
        groupDragAnchorTrack = track;
        groupDragAnchorClip = clip;
        groupDragOriginal =
            session.tracks[static_cast<size_t>(track)].clips[static_cast<size_t>(clip)];
        groupDragSamples = 0;
        groupDragTracks = 0;
        groupDragValid = true;
        groupDragOverlaps = false;
        groupDragError.clear();
        dragSpan = span;
        dragFullLength = length;
        dragTargets = std::move(targets);
        dragMode = kDragGroupMove;
        groupDragCopy = true;
        copyGestureCandidate = false;
        {
            const juce::ScopedUnlock unlock(lock);
            dragBefore = {};
            dragBeforeState = {};
        }
    }

    // Group gestures only update message-thread-owned preview geometry.
    // The committed session remains unchanged until mouse-up.
    if (dragMode == kDragGroupMove || dragMode == kDragGroupEdge) {
        if (!clipPointerDragging) {
            clipScrollClock = juce::Time::getMillisecondCounterHiRes();
        }
        clipPointerDragging = true;
        clipPointer = e.getPosition();
        clipSnapAllowed = !e.mods.isShiftDown();
        if (dragMode == kDragGroupMove) {
            updateGroupDrag();
        } else {
            updateGroupEdgeDrag();
        }
        return;
    }

    // Marker geometry, snapping and validation belong to the message
    // thread; publish just the validated position under the audio lock.
    if (dragMode == kDragMarkerOrMasterEnd && !draggingMasterEnd && selectedMarker >= 0 &&
        selectedMarker < static_cast<int>(session.markers.size())) {
#if CARTA_STUDIO_COMPONENT_TEST
        if (rangePreparationHook) {
            rangePreparationHook();
        }
#endif
        const auto sample = snappedPointerSample(e.x, !e.mods.isShiftDown());
        juce::String error;
        const bool valid = carta::validateMarkerSample(session, selectedMarker, sample,
                                                       markerDragMasterEnd, error);
        if (valid) {
            const double markerPosition = static_cast<double>(sample) / carta::sessionRate;
            const juce::ScopedLock guard(lock);
            if (!selectionGesturePlayback && !playing) {
                position = markerPosition;
            }
            session.markers[static_cast<size_t>(selectedMarker)].position = markerPosition;
            dragged = std::abs(markerPosition -
                               dragBefore.markers[static_cast<size_t>(selectedMarker)].position) >
                      1.0e-12;
        } else {
            status.setText(error, juce::dontSendNotification);
        }
        repaint();
        return;
    }

    // These previews only touch message-thread-owned view and selection state.
    if (dragMode == kDragTrackReorder) {
        trackDragPointer = e.getPosition();
        if (e.getDistanceFromDragStart() > 4) {
            trackDragMoved = true;
        }
        updateDraggedTrack();
        return;
    }

    if (dragMode == kDragSavedLoop) {
        if (!loopPointerDragging) {
            loopScrollClock = juce::Time::getMillisecondCounterHiRes();
        }
        loopPointerDragging = true;
        loopPointer = e.getPosition();
        loopSnapAllowed = !e.mods.isShiftDown();
        updateDraggedLoop();
        return;
    }

    if (dragMode == kDragTimeSelection || dragMode == kDragTimeSelectionResize) {
        if (!selectionPointerDragging) {
            selectionScrollClock = juce::Time::getMillisecondCounterHiRes();
        }
        selectionPointerDragging = true;
        selectionPointer = e.getPosition();
        selectionSnapAllowed = !e.mods.isShiftDown();
        updateDraggedSelection();
        return;
    }

    const juce::ScopedLock guard(lock);
    if (dragMode == kDragHeaderControl) {
        if (headerDragTrack >= 0 && headerDragTrack < static_cast<int>(session.tracks.size())) {
            const int dx = e.x - headerDragAnchor.x;
            if (std::abs(dx) >= 2) {
                headerDragMoved = true;
            }
            if (headerDragMoved) {
                const bool fine = e.mods.isShiftDown();
                headerDragValue = headerDragGain
                                      ? headerDraggedGain(headerDragStartValue, dx, fine)
                                      : headerDraggedBalance(headerDragStartValue, dx, fine);
                auto& tr = session.tracks[static_cast<size_t>(headerDragTrack)];
                if (headerDragGain) {
                    tr.gain = headerDragValue;
                } else {
                    tr.pan = headerDragValue;
                }
                dragged = true;
                status.setText((headerDragGain
                                    ? "Track gain: " + formatHeaderGainDb(headerDragValue)
                                    : "Track balance: " + formatHeaderBalance(headerDragValue)) +
                                   " (Escape cancels)",
                               juce::dontSendNotification);
                repaint();
            }
        }
        return;
    }

    if (dragMode == kDragMdRibbon) {
        dragged = dragged || std::abs(e.x - dragX) >= 1 || e.getDistanceFromDragStart() >= 2;
        mdDragPointer = e.getPosition();
        updateDraggedMdRibbon(e.mods);
        return;
    }

    if (dragMode == kDragChannelHeight) {
        updateDraggedChannelHeight(e.y);
        return;
    }

    if (dragMode == kDragPlayhead) {
        if (!playheadPointerDragging) {
            playheadScrollClock = juce::Time::getMillisecondCounterHiRes();
        }
        playheadPointerDragging = true;
        playheadPointer = e.getPosition();
        playheadSnapAllowed = !e.mods.isShiftDown();
        updateDraggedPlayhead();
        return;
    }

    if (dragMode == kDragTrackHeight && resizedTrack >= 0 &&
        resizedTrack < static_cast<int>(session.tracks.size())) {
        auto& h = session.tracks[static_cast<size_t>(resizedTrack)].height;
        h = juce::jlimit(72, 800, resizeStartHeight + e.y - resizeStartY);
        dragged = (h != resizeStartHeight);
        repaint();
        return;
    }

    if (dragMode == kDragMarkerOrMasterEnd && draggingMasterEnd) {
        if (!clipPointerDragging) {
            clipScrollClock = juce::Time::getMillisecondCounterHiRes();
        }
        clipPointerDragging = true;
        clipPointer = e.getPosition();
        clipSnapAllowed = !e.mods.isShiftDown();
        updateDraggedMasterEnd();
        return;
    }

    const bool singleClipDrag = dragMode == kDragClipMove || dragMode == kDragClipTrimLeft ||
                                dragMode == kDragClipTrimRight;
    if (!singleClipDrag || !validSelection()) {
        return;
    }
    if (!draggingFade && session.tracks[static_cast<size_t>(selectedTrack)]
                             .clips[static_cast<size_t>(selectedClip)]
                             .groupId.isNotEmpty()) {
        status.setText("Drag a grouped clip without Alt/Option to move or trim the group together.",
                       juce::dontSendNotification);
        return;
    }
    if (!clipPointerDragging) {
        clipScrollClock = juce::Time::getMillisecondCounterHiRes();
    }
    clipPointerDragging = true;
    clipPointer = e.getPosition();
    clipSnapAllowed = !e.mods.isShiftDown();
    updateDraggedClip();
    updateClipDropTarget();
}

void Studio::mouseUp(const juce::MouseEvent& e) {
    if (dragMode == kDragMarquee) {
        marqueeEnd = e.getPosition();
        dragged = dragged || marqueeStart.getDistanceFrom(marqueeEnd) >= 3.0;
        updateMarqueePreview();
        const bool valid = marqueePreviewValid && marqueeRevision == editGeneration;
        dragMode = kDragNone;
        dragged = false;
        if (valid) {
            timelineClipSelection = std::move(marqueePreview);
            const auto& clips = timelineClipSelection.get(editGeneration);
            if (!timelineClipSelection.contains(selectedTrack, selectedClip, editGeneration)) {
                selectedTrack = clips.front().track;
                selectedClip = clips.front().clip;
            }
            selectedMarker = -1;
            const auto message = juce::String(clips.size()) +
                                 (clips.size() == 1 ? " clip selected." : " clips selected.") +
                                 " Time selection and playhead unchanged.";
            status.setText(message, juce::dontSendNotification);
            announceNavigation(message);
            menuItemsChanged();
        } else {
            status.setText("No clips selected; selection unchanged.", juce::dontSendNotification);
        }
        marqueePreview.clear();
        marqueePreviewValid = false;
        updateCursor(e.getPosition());
        repaint();
        return;
    }

    // Group previews only read the committed session and are owned by the
    // message thread, including the final pointer update on release.
    if ((dragMode == kDragGroupMove || dragMode == kDragGroupEdge) && clipPointerDragging) {
        clipPointer = e.getPosition();
        if (dragMode == kDragGroupMove) {
            updateGroupDrag();
        } else {
            updateGroupEdgeDrag();
        }
    }

    if ((dragMode == kDragTimeSelection || dragMode == kDragTimeSelectionResize) && !dragged) {
        {
            const juce::ScopedLock guard(lock);
            dragMode = kDragNone;
            draggingFade = false;
            draggingMasterEnd = false;
            dragged = false;
            selectionPointerDragging = false;
            clipPointerDragging = false;
            playheadPointerDragging = false;
            clipDropTrack = -1;
            resizedTrack = -1;
            snapGuideVisible = false;
            dirty = editHistory.isDirty() || pendingDurability || loopDirty();
        }
#if CARTA_STUDIO_COMPONENT_TEST
        if (navigationPreviewHook) {
            navigationPreviewHook();
        }
#endif
        if (selectionClipSnapshotValid) {
            selectionBeforeClips.clear();
            selectionClipSnapshotValid = false;
        }
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()), viewStart);
        updateCursor(e.getPosition());
        repaint();
        return;
    }

    const juce::ScopedLock guard(lock);
    if (dragMode == kDragChannelHeight) {
        updateDraggedChannelHeight(e.y);
        const auto previous = editGeneration;
        dragMode = kDragNone;
        if (dragged) {
            auto range = masterRange;
            if (!masterRangeValid || masterRangeRevision != previous) {
                const juce::ScopedUnlock unlock(lock);
                range = carta::fullMasterPlaybackRange(session);
            }
            pushPreparedHistory(std::move(dragBeforeState), "Change channel heights", range, true);
            retainIndexStableSelections(previous);
            status.setText("Channel heights updated.", juce::dontSendNotification);
        } else {
            status.setText("Channel heights unchanged.", juce::dontSendNotification);
        }
        {
            const juce::ScopedUnlock unlock(lock);
            dragBeforeState = {};
        }
        channelDragTrack = channelDragLane = -1;
        channelDragOriginal.clear();
        channelDragScaled.clear();
        dragged = false;
        dirty = editHistory.isDirty() || pendingDurability || loopDirty();
        updateCursor(e.getPosition());
        menuItemsChanged();
        repaint();
        return;
    }

    if (dragMode == kDragTrackHeight) {
        const auto previous = editGeneration;
        dragMode = kDragNone;
        if (dragged) {
            auto range = masterRange;
            if (!masterRangeValid || masterRangeRevision != previous) {
                const juce::ScopedUnlock unlock(lock);
                range = carta::fullMasterPlaybackRange(session);
            }
            pushPreparedHistory(std::move(dragBeforeState), "Resize track", range, true);
            retainIndexStableSelections(previous);
        }
        {
            const juce::ScopedUnlock unlock(lock);
            dragBeforeState = {};
        }
        resizedTrack = -1;
        dragged = false;
        dirty = editHistory.isDirty() || pendingDurability || loopDirty();
        updateCursor(e.getPosition());
        menuItemsChanged();
        repaint();
        return;
    }

    if (popupPending) {
        popupPending = false;
        if (!showMarkerContextMenu(e) && !showTrackContextMenu(e) && !showSelectionContextMenu(e)) {
            const juce::ScopedUnlock unlock(lock);
            showClipContextMenu(e);
        }
        return;
    }

    if (dragMode == kDragTrackReorder) {
        trackDragPointer = e.getPosition();
        updateDraggedTrack();
        const int source = trackDragSource;
        const int slot = trackDragSlot;
        const int destination = slot > source ? slot - 1 : slot;
        dragMode = kDragNone;
        if (trackDragMoved && slot >= 0 && destination != source) {
            bool moved = false;
            {
                const juce::ScopedUnlock unlock(lock);
                moved = reorderTrack(source, destination, dragBeforeState);
            }
            if (!moved) {
                restoreTrackDragSelection();
            }
        } else if (trackDragMoved && slot < 0) {
            restoreTrackDragSelection();
            status.setText("Layer move cancelled: drop inside the timeline.",
                           juce::dontSendNotification);
        }
        dragBeforeState = {};
        trackDragSource = trackDragSlot = -1;
        trackDragMoved = false;
        updateCursor(e.getPosition());
        menuItemsChanged();
        repaint();
        return;
    }

    if (dragMode == kDragHeaderControl) {
        const int track = headerDragTrack;
        const bool isGain = headerDragGain;
        const bool moved = headerDragMoved;
        const float startVal = headerDragStartValue;
        const float endVal = headerDragValue;
        if (track >= 0 && track < static_cast<int>(session.tracks.size())) {
            if (isGain) {
                session.tracks[static_cast<size_t>(track)].gain = startVal;
            } else {
                session.tracks[static_cast<size_t>(track)].pan = startVal;
            }
        }
        dragMode = kDragNone;
        headerDragTrack = -1;
        headerDragMoved = false;
        dragged = false;
        if (moved && track >= 0 && track < static_cast<int>(session.tracks.size()) &&
            std::abs(endVal - startVal) > 1e-6f) {
            bool changed = false;
            juce::String error;
            if (isGain) {
                if (setTrackGains(editGeneration, {track}, static_cast<double>(endVal), changed,
                                  error)) {
                    status.setText("Track gain " + formatHeaderGainDb(endVal) + ".",
                                   juce::dontSendNotification);
                } else {
                    status.setText(error, juce::dontSendNotification);
                }
            } else {
                if (setTrackBalances(editGeneration, {track}, static_cast<double>(endVal), changed,
                                     error)) {
                    status.setText("Track balance " + formatHeaderBalance(endVal) + ".",
                                   juce::dontSendNotification);
                } else {
                    status.setText(error, juce::dontSendNotification);
                }
            }
        }
        updateCursor(e.getPosition());
        menuItemsChanged();
        repaint();
        return;
    }

    if (dragMode == kDragMdRibbon) {
        const auto previous = editGeneration;
        const auto focusId = mdDragFocusTrackId;
        const auto action = mdDragActionLabel.isNotEmpty() ? mdDragActionLabel
                                                           : juce::String("Edit MD track boundary");
        const bool changed = dragged && mdDragChanged;
        if (changed) {
            auto range = masterRange;
            if (!masterRangeValid || masterRangeRevision != previous) {
                const juce::ScopedUnlock unlock(lock);
                range = carta::fullMasterPlaybackRange(session);
            }
            pushPreparedHistory(std::move(dragBeforeState), action, range, true);
            retainIndexStableSelections(previous);
        } else {
            session.mdDisc = std::move(mdDragBeforeDisc);
            status.setText("MD track boundary unchanged.", juce::dontSendNotification);
        }
        {
            const juce::ScopedUnlock unlock(lock);
            dragBeforeState = {};
        }
        mdDragBeforeDisc = {};
        dragMode = kDragNone;
        mdDragGesture = MdRibbonGesture::none;
        mdDragTrack0 = mdDragPart0 = mdDragRightTrack0 = mdDragRightPart0 = -1;
        mdDragChanged = false;
        mdDragFocusTrackId = {};
        mdDragActionLabel = {};
        dragged = false;
        snapGuideVisible = false;
        dirty = editHistory.isDirty() || pendingDurability || loopDirty();
        {
            const juce::ScopedUnlock unlock(lock);
            refreshDiscWorkspace(true, focusId);
        }
        updateCursor(e.getPosition());
        menuItemsChanged();
        repaint();
        return;
    }

    if (dragMode == kDragSavedLoop) {
        const auto result = loopDragPreview;
        const bool changed =
            result.start != loopDragOriginal.start || result.end != loopDragOriginal.end;
        dragMode = kDragNone;
        loopPointerDragging = false;
        snapGuideVisible = false;
        dragged = false;
        if (changed) {
            juce::String error;
            if (!setSavedLoop(result.start, result.end, error)) {
                status.setText(error, juce::dontSendNotification);
            }
        } else {
            status.setText("Loop region unchanged.", juce::dontSendNotification);
        }
        updateCursor(e.getPosition());
        repaint();
        return;
    }

    if (dragMode == kDragGroupMove) {
        finishGroupDrag(e.getPosition());
        return;
    }
    if (dragMode == kDragGroupEdge) {
        finishGroupEdgeDrag(e.getPosition());
        return;
    }

    if (dragMode == kDragMarkerOrMasterEnd && !draggingMasterEnd) {
        if (dragged) {
            const auto generation = editGeneration;
            auto range = masterRange;
            if (!masterRangeValid || masterRangeRevision != generation) {
                const juce::ScopedUnlock unlock(lock);
                range = carta::fullMasterPlaybackRange(session);
            }
            timelineClipSelection.retainAfterStableEdit(generation, generation + 1);
            if (timelineTrackRevision == generation) {
                timelineTrackRevision = generation + 1;
            }
            pushPreparedHistory(std::move(dragBeforeState), "Move marker", range, true);
        }
        {
            const juce::ScopedUnlock unlock(lock);
            dragBefore = {};
            dragBeforeState = {};
        }
        dragMode = kDragNone;
        dragged = false;
        snapGuideVisible = false;
        dirty = editHistory.isDirty() || pendingDurability || loopDirty();
        updateCursor(e.getPosition());
        menuItemsChanged();
        repaint();
        return;
    }

    if (dragMode == kDragMarkerOrMasterEnd && draggingMasterEnd) {
        if (dragged) {
            juce::String error;
            bool allowed = false;
            {
                const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
                if (singleClipCommitHook) {
                    singleClipCommitHook();
                }
#endif
                allowed = carta::preservesMarkerBounds(dragBefore, session, error);
            }
            if (allowed) {
                const auto previous = editGeneration;
                pushPreparedHistory(std::move(dragBeforeState), "Change master duration",
                                    dragMasterRange);
                retainIndexStableSelections(previous);
            } else {
                std::swap(session, dragBeforeState.session);
                applyStateFields(dragBeforeState);
                status.setText(error, juce::dontSendNotification);
            }
        }
        {
            const juce::ScopedUnlock unlock(lock);
            dragBefore = {};
            dragBeforeState = {};
        }
        dragMode = kDragNone;
        draggingMasterEnd = false;
        clipPointerDragging = false;
        dragged = false;
        snapGuideVisible = false;
        dirty = editHistory.isDirty() || pendingDurability || loopDirty();
        {
            const juce::ScopedUnlock unlock(lock);
            viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()), viewStart);
            updateCursor(e.getPosition());
            repaint();
        }
        menuItemsChanged();
        return;
    }

    const int crossTrack = dragMode == kDragClipMove && clipPointerDragging && validSelection()
                               ? clipDropTrackAt(e.getPosition())
                               : -1;
    const bool singleClipDrag = dragMode == kDragClipMove || dragMode == kDragClipTrimLeft ||
                                dragMode == kDragClipTrimRight;
    if ((dragged || crossTrack >= 0) && singleClipDrag) {
        juce::String error;
        bool allowed = false;
        carta::PlaybackRange range;
        const bool stable = draggingFade;
        const auto generation = editGeneration;
        carta::Session candidate;
        int destinationClip = selectedClip;
        bool routing = false;
        {
            const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
            if (singleClipCommitHook) {
                singleClipCommitHook();
            }
#endif
            const carta::Session* proposed = &session;
            if (crossTrack >= 0) {
                candidate = session;
                if (carta::moveClipToTrack(candidate, selectedTrack, destinationClip, crossTrack)) {
                    proposed = &candidate;
                    const auto& track = candidate.tracks[static_cast<size_t>(crossTrack)];
                    routing = !carta::clipRoutingConfirmed(track, carta::maxTrackChannels(track));
#if CARTA_STUDIO_COMPONENT_TEST
                    if (singleClipCandidateHook) {
                        singleClipCandidateHook();
                    }
#endif
                } else {
                    proposed = nullptr;
                    error = "Could not move the clip to that track.";
                }
            }
            allowed =
                proposed && (stable || carta::preservesMarkerBounds(dragBefore, *proposed, error));
            if (allowed) {
                range = stable && masterRangeValid && masterRangeRevision == generation
                            ? masterRange
                            : carta::fullMasterPlaybackRange(*proposed);
            }
        }
        if (allowed) {
            if (crossTrack >= 0) {
                std::swap(session, candidate);
                selectedTrack = crossTrack;
                selectedClip = destinationClip;
                dragged = true;
            }
            if (!stable) {
                dragMasterRange = range;
            }
            pushPreparedHistory(std::move(dragBeforeState),
                                draggingFade                ? "Adjust clip fade"
                                : dragMode == kDragClipMove ? "Move clip"
                                                            : "Trim clip",
                                range, stable);
            if (routing) {
                status.setText(
                    "Clip moved. Confirm the destination track's multichannel routing before "
                    "export.",
                    juce::dontSendNotification);
            }
        } else {
            std::swap(session, dragBeforeState.session);
            applyStateFields(dragBeforeState);
            status.setText(error, juce::dontSendNotification);
        }
        {
            const juce::ScopedUnlock unlock(lock);
            candidate = {};
            dragBefore = {};
            dragBeforeState = {};
        }
        if (selectionClipSnapshotValid) {
            selectionBeforeClips.clear();
            selectionClipSnapshotValid = false;
        }
        dragMode = kDragNone;
        draggingFade = false;
        clipPointerDragging = false;
        clipDropTrack = -1;
        dragged = false;
        snapGuideVisible = false;
        dirty = editHistory.isDirty() || pendingDurability || loopDirty();
        {
            const juce::ScopedUnlock unlock(lock);
            viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()), viewStart);
            updateCursor(e.getPosition());
            repaint();
        }
        return;
    }

    if (!dragged && singleClipDrag) {
        const juce::ScopedUnlock unlock(lock);
        dragBefore = {};
        dragBeforeState = {};
        if (dragMode == kDragClipMove && !e.mods.isAnyModifierKeyDown()) {
            selectClipTime(0);
        }
    }

    if (dragged) {
        juce::String error;
        const bool renderGeometryStable =
            draggingFade || dragMode == kDragTrackHeight ||
            (dragMode == kDragMarkerOrMasterEnd && !draggingMasterEnd);
        if (renderGeometryStable || carta::preservesMarkerBounds(dragBefore, session, error)) {
            const auto previous = editGeneration;
            pushHistory(std::move(dragBeforeState),
                        draggingMasterEnd           ? "Change master duration"
                        : draggingFade              ? "Adjust clip fade"
                        : dragMode == kDragClipMove ? "Move clip"
                        : (dragMode == kDragClipTrimLeft || dragMode == kDragClipTrimRight)
                            ? "Trim clip"
                        : dragMode == kDragMarkerOrMasterEnd ? "Move marker"
                                                             : "Resize track",
                        renderGeometryStable);
            if (dragMode == kDragMarkerOrMasterEnd || dragMode == kDragTrackHeight) {
                retainIndexStableSelections(previous);
            }
        } else {
            applyState(std::move(dragBeforeState));
            status.setText(error, juce::dontSendNotification);
        }
    }

    if (selectionClipSnapshotValid) {
        selectionBeforeClips.clear();
        selectionClipSnapshotValid = false;
    }
    dragMode = kDragNone;
    draggingFade = false;
    draggingMasterEnd = false;
    selectionPointerDragging = false;
    clipPointerDragging = false;
    playheadPointerDragging = false;
    clipDropTrack = -1;
    resizedTrack = -1;
    dragged = false;
    snapGuideVisible = false;
    dirty = editHistory.isDirty() || pendingDurability || loopDirty();
    viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()), viewStart);
    updateCursor(e.getPosition());
    repaint();
}

void Studio::mouseMove(const juce::MouseEvent& e) {
    updateCursor(e.getPosition());
}

void Studio::mouseExit(const juce::MouseEvent&) {
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void Studio::mouseDoubleClick(const juce::MouseEvent& event) {
    if (event.x < timeline().getX() + trackLabelWidth &&
        event.y >= timeline().getY() + timelineHeaderHeight()) {
        int hitTrack = -1;
        const int hit = trackHeaderHit(event.getPosition(), &hitTrack);
        if (hit == HeaderHit::gain || hit == HeaderHit::balance) {
            bool changed = false;
            juce::String error;
            if (hit == HeaderHit::gain) {
                if (setTrackGains(editGeneration, {hitTrack}, 1.0, changed, error)) {
                    status.setText(changed ? "Reset track gain to 0.0 dB."
                                           : "Track gain is already 0.0 dB.",
                                   juce::dontSendNotification);
                }
            } else {
                if (setTrackBalances(editGeneration, {hitTrack}, 0.0, changed, error)) {
                    status.setText(changed ? "Centered track balance."
                                           : "Track balance is already centered.",
                                   juce::dontSendNotification);
                }
            }
            repaint();
            return;
        }
    }

    if (event.y < timeline().getY() + timelineHeaderHeight()) {
        return;
    }
    const int track = trackAt(event.y);
    if (track < 0) {
        return;
    }

    const auto& clips = session.tracks[static_cast<size_t>(track)].clips;
    if (!clips.empty() && event.y < clipRect(clips.front(), track).getY() + titleHeight) {
        for (const auto& region : titleRegions(track)) {
            if (region.bounds.contains(event.x)) {
                selectedTrack = track;
                selectedClip = region.clip;
                timeSelection = {};
                dragMode = kDragNone;
                zoomToSelected();
                return;
            }
        }
    }

    if (doubleClickRevision == editGeneration && doubleClickTrack == track &&
        doubleClickClip >= 0 && doubleClickClip < static_cast<int>(clips.size()) &&
        clipRect(clips[static_cast<size_t>(doubleClickClip)], track)
            .contains(event.getPosition())) {
        selectedTrack = track;
        selectedClip = doubleClickClip;
        timeSelection = {};
        dragMode = kDragNone;
        zoomToSelected();
        return;
    }

    if (selectedTrack == track && validSelection() &&
        clipRect(clips[static_cast<size_t>(selectedClip)], track).contains(event.getPosition())) {
        timeSelection = {};
        dragMode = kDragNone;
        zoomToSelected();
        return;
    }

    for (int i = static_cast<int>(clips.size()) - 1; i >= 0; --i) {
        if (clipRect(clips[static_cast<size_t>(i)], track).contains(event.getPosition())) {
            selectedTrack = track;
            selectedClip = i;
            timeSelection = {};
            dragMode = kDragNone;
            zoomToSelected();
            return;
        }
    }

    const auto body = timeline().withTrimmedTop(timelineHeaderHeight()).withTrimmedLeft(labelWidth);
    if (body.contains(event.getPosition()) && !event.mods.isAnyModifierKeyDown() &&
        !session.tracks[static_cast<size_t>(track)].collapsed) {
        const auto sample = sampleAtX(event.x);
        carta::TimeSelection gap;
        if (!carta::selectClipGap(session, track, sample, gap)) {
            return;
        }
        // The second mouse-down may have started a point-selection gesture.
        // Finish it before selecting the gap so mouse-up cannot restore it.
        dragMode = kDragNone;
        dragged = false;
        selectionPointerDragging = false;
        selectionClipSnapshotValid = false;
        selectionBeforeClips.clear();
        timeSelection = {sample, sample, track, track};
        selectedTrack = track;
        selectedClip = selectedMarker = -1;
        selectGapAtEditPoint();
    }
}

void Studio::mouseMagnify(const juce::MouseEvent& event, float scaleFactor) {
    if (dragMode != kDragNone || !std::isfinite(scaleFactor) || scaleFactor <= 0.0f ||
        !timeline().contains(event.getPosition())) {
        return;
    }
    const auto area = timeline();
    const double ratio = juce::jlimit(0.0, 1.0,
                                      static_cast<double>(event.x - area.getX() - labelWidth) /
                                          std::max(1, area.getWidth() - labelWidth));
    zoomAround(static_cast<double>(scaleFactor), viewStart + ratio * viewLength(), ratio);
    repaint();
}

void Studio::mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) {
    // View state and the committed session are owned by the message thread;
    // playback reads the session but never changes navigation state.
    if (dragMode != kDragNone || !std::isfinite(wheel.deltaX) || !std::isfinite(wheel.deltaY)) {
        smoothVerticalRemainder = 0;
        return;
    }

    if (event.mods.isCommandDown() && std::abs(wheel.deltaY) > 0.0001f) {
        smoothVerticalRemainder = 0;
        const double ratio =
            juce::jlimit(0.0, 1.0,
                         static_cast<double>(event.x - timeline().getX() - labelWidth) /
                             std::max(1, timeline().getWidth() - labelWidth));
        const double steps = wheel.isSmooth
                                 ? std::clamp(static_cast<double>(wheel.deltaY) * 10.0, -4.0, 4.0)
                                 : (wheel.deltaY > 0 ? 1.0 : -1.0);
        zoomAround(std::pow(1.5, steps), xToTime(event.x), ratio);
    } else if (event.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY)) {
        smoothVerticalRemainder = 0;
        const float delta = event.mods.isShiftDown() ? wheel.deltaY : wheel.deltaX;
        const double oldStart = viewStart;
        viewStart =
            juce::jlimit(0.0, std::max(0.0, std::max(10.0, session.length() * 1.1) - viewLength()),
                         viewStart - static_cast<double>(delta) * viewLength());
        if (std::abs(viewStart - oldStart) > 1e-12) {
            suspendFollowingForNavigation();
        }
    } else if (std::abs(wheel.deltaY) > 0.0001f) {
        const int last = std::max(0, static_cast<int>(session.tracks.size()) - 1);
        int steps = wheel.deltaY < 0 ? 1 : -1;
        if (wheel.isSmooth) {
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (now - lastSmoothVerticalWheelTime > 500) {
                smoothVerticalRemainder = 0;
            }
            lastSmoothVerticalWheelTime = now;
            smoothVerticalRemainder = std::clamp(
                smoothVerticalRemainder - static_cast<double>(wheel.deltaY), -10.0, 10.0);
            steps = static_cast<int>(smoothVerticalRemainder / 0.1);
            smoothVerticalRemainder -= static_cast<double>(steps) * 0.1;
        } else {
            smoothVerticalRemainder = 0;
        }
        firstTrack = juce::jlimit(0, last, firstTrack + steps);
        if ((firstTrack == 0 && smoothVerticalRemainder < 0) ||
            (firstTrack == last && smoothVerticalRemainder > 0)) {
            smoothVerticalRemainder = 0;
        }
    }
    repaint();
}

// ============================================================================
// Live Drag Preview Updates & Commits
// ============================================================================

int Studio::clipDropTrackAt(juce::Point<int> point) const {
    const auto area = timeline().withTrimmedTop(timelineHeaderHeight());
    if (!area.contains(point)) {
        return -1;
    }
    const int track = trackAt(point.y);
    return track >= 0 && track < static_cast<int>(session.tracks.size()) && track != selectedTrack
               ? track
               : -1;
}

void Studio::updateClipDropTarget() {
    const juce::ScopedUnlock unlock(lock);
    const int target =
        dragMode == kDragClipMove && clipPointerDragging ? clipDropTrackAt(clipPointer) : -1;
    const bool targetChanged = target != clipDropTrack;
    clipDropTrack = target;
    if (dragMode == kDragClipMove && clipPointerDragging && validSelection()) {
        const int destination = target >= 0 ? target : selectedTrack;
        const auto& clip = session.tracks[static_cast<size_t>(selectedTrack)]
                               .clips[static_cast<size_t>(selectedClip)];
        const bool overlap =
            clipOverlapReady && clipOverlapPreview.overlapsSingle(clip.start, destination);
        const auto location = target >= 0
                                  ? "Drop on track " + juce::String(target + 1) + ": " +
                                        session.tracks[static_cast<size_t>(target)].name + ". "
                                  : juce::String("Move clip. ");
        status.setText(location + (clipOverlapReady
                                       ? (overlap ? "Destination overlaps another clip; signals "
                                                    "sum when audible."
                                                  : "No overlap with other clips.")
                                       : "Overlap preview unavailable."),
                       juce::dontSendNotification);
    }
    if (targetChanged) {
        repaint();
    }
}

void Studio::updateGroupEdgeDrag() {
    if (dragMode != kDragGroupEdge || !clipPointerDragging) {
        return;
    }
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    snapGuideVisible = false;
    groupDragValid = false;
    groupEdgePreview.clear();

    const auto area = timeline();
    const double delta = carta::clipDragDelta(clipPointer.x, dragX, area.getX() + labelWidth,
                                              area.getRight(), viewStart, dragViewStart, dragSpan);
    const double frames = delta * carta::sessionRate;

    if (groupDragGeneration != editGeneration) {
        groupDragError = "Project changed during the drag.";
    } else if (!std::isfinite(frames) || std::abs(frames) >= 9007199254740992.0) {
        groupDragError = "Trim exceeds the precise timeline range.";
    } else {
        groupDragSamples = static_cast<juce::int64>(std::llround(frames));
        const double anchor =
            groupDragOriginal.start + (groupEdgeEnd ? groupDragOriginal.duration : 0.0);
        if (clipSnapAllowed && snapMode == 2) {
            const double radius = static_cast<double>(kSnapBoundaryRadiusPx) * dragSpan /
                                  std::max(1, area.getWidth() - labelWidth);
            const auto snap =
                carta::snapClipGroupMove(anchor, 0, groupDragSamples, radius, dragTargets);
            groupDragSamples = snap.delta;
            snapGuideVisible = snap.hasGuide;
            snapGuideTime = snap.guide;
        }
        if (clipSnapAllowed && snapMode != 0 && snapGridSamples > 1 && !snapGuideVisible) {
            juce::int64 gridDelta = 0;
            if (carta::snapClipGroupToGrid(anchor, groupDragSamples, snapGridSamples, gridDelta)) {
                groupDragSamples = gridDelta;
                snapGuideVisible = true;
                snapGuideTime =
                    std::round((anchor * carta::sessionRate + static_cast<double>(gridDelta)) /
                               static_cast<double>(snapGridSamples)) *
                    static_cast<double>(snapGridSamples) / carta::sessionRate;
            }
        }
        groupDragValid =
            groupDragSamples == 0 || groupEdgePlan.calculate(groupEdgeEnd, groupDragSamples,
                                                             groupEdgePreview, groupDragError);
        if (groupDragValid && groupDragSamples != 0 &&
            !groupEdgeMarkerPreview.validate(groupEdgePreview, groupDragError)) {
            groupDragValid = false;
            groupEdgePreview.clear();
            snapGuideVisible = false;
        }
    }

    status.setText(groupDragValid ? "Trim selected clip edges: " + juce::String(groupDragSamples) +
                                        " samples. Hold Shift to disable snapping."
                                  : groupDragError,
                   juce::dontSendNotification);
    repaint();
}

void Studio::finishGroupEdgeDrag(juce::Point<int> point) {
    const auto clips = std::move(groupDragClips);
    const auto generation = groupDragGeneration;
    const auto samples = groupDragSamples;
    const bool valid = groupDragValid;
    const bool end = groupEdgeEnd;
    dragMode = kDragNone;
    clipPointerDragging = false;
    snapGuideVisible = false;
    dragged = false;
    {
        const juce::ScopedUnlock unlock(lock);
        groupDragClips.clear();
        groupEdgePreview.clear();
        groupEdgePlan.clear();
    }
    if (valid && samples != 0) {
        // adjustSelectedClipEdges owns its lock; do not retain mouseUp's
        // recursive acquisition while it prepares the candidate session.
        const juce::ScopedUnlock unlock(lock);
        adjustSelectedClipEdges(clips, generation, end, samples);
    } else if (!valid) {
        status.setText(groupDragError, juce::dontSendNotification);
    }
    {
        const juce::ScopedUnlock unlock(lock);
        selectionBeforeClips.clear();
        selectionClipSnapshotValid = false;
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()), viewStart);
        updateCursor(point);
        menuItemsChanged();
        repaint();
    }
}

void Studio::updateGroupDrag() {
    if (dragMode != kDragGroupMove || !clipPointerDragging) {
        return;
    }
    setMouseCursor(groupDragCopy ? juce::MouseCursor::CopyingCursor
                                 : juce::MouseCursor::DraggingHandCursor);
    const auto area = timeline();
    const int target = trackAt(clipPointer.y);
    groupDragValid = false;
    groupDragOverlaps = false;
    snapGuideVisible = false;

    if (groupDragGeneration != editGeneration) {
        groupDragError = "Project changed during the drag.";
    } else if (clipPointer.y < area.getY() + timelineHeaderHeight() ||
               clipPointer.y >= area.getBottom() || target < 0) {
        groupDragError = "Move the pointer over a destination track.";
    } else {
        groupDragTracks = target - groupDragAnchorTrack;
        const double delta =
            carta::clipDragDelta(clipPointer.x, dragX, area.getX() + labelWidth, area.getRight(),
                                 viewStart, dragViewStart, dragSpan);
        const double frames = delta * carta::sessionRate;
        if (!std::isfinite(frames) || std::abs(frames) >= 9007199254740992.0) {
            groupDragError = "Move exceeds the precise timeline range.";
        } else {
            groupDragSamples = static_cast<juce::int64>(std::llround(frames));
            if (clipSnapAllowed && snapMode == 2) {
                const double radius = static_cast<double>(kSnapBoundaryRadiusPx) * dragSpan /
                                      std::max(1, area.getWidth() - labelWidth);
                const auto snap =
                    carta::snapClipGroupMove(groupDragOriginal.start, groupDragOriginal.duration,
                                             groupDragSamples, radius, dragTargets);
                groupDragSamples = snap.delta;
                snapGuideVisible = snap.hasGuide;
                snapGuideTime = snap.guide;
            }
            if (clipSnapAllowed && snapMode != 0 && snapGridSamples > 1 && !snapGuideVisible) {
                juce::int64 gridDelta = 0;
                if (carta::snapClipGroupToGrid(groupDragOriginal.start, groupDragSamples,
                                               snapGridSamples, gridDelta)) {
                    groupDragSamples = gridDelta;
                    snapGuideVisible = true;
                    const auto frame = (groupDragOriginal.start * carta::sessionRate +
                                        static_cast<double>(gridDelta)) /
                                       static_cast<double>(snapGridSamples);
                    snapGuideTime = std::round(frame) * static_cast<double>(snapGridSamples) /
                                    carta::sessionRate;
                }
            }
            groupDragValid =
                groupDragPreview.validate(groupDragSamples, groupDragTracks, groupDragError);
            if (groupDragValid) {
                groupDragOverlaps = clipOverlapPreview.overlaps(groupDragSamples, groupDragTracks);
            }
        }
    }

    status.setText(groupDragValid
                       ? (groupDragCopy ? "Copy selected clips: " : "Move selected clips: ") +
                             juce::String(groupDragSamples) + " samples, " +
                             juce::String(groupDragTracks) + " tracks. " +
                             (groupDragCopy ? "Copies can overlap existing audio; signals sum."
                              : groupDragOverlaps
                                  ? "Destination overlaps unselected clips; signals sum when "
                                    "audible."
                                  : "No overlap with unselected clips.")
                       : groupDragError,
                   juce::dontSendNotification);
    repaint();
}

void Studio::finishGroupDrag(juce::Point<int> point) {
    if (clipPointerDragging && !timeline().withTrimmedTop(timelineHeaderHeight()).contains(point)) {
        groupDragValid = false;
        groupDragError = "Drop over a destination track to move the clips.";
    }

    if (clipPointerDragging && groupDragValid && (groupDragSamples != 0 || groupDragTracks != 0)) {
        juce::String error;
        std::vector<carta::ClipReference> movedRefs;
        std::vector<carta::ClipReference> historyClips;
        std::vector<int> historyTracks;
        carta::Session candidate;
        carta::ClipSelection nextSelection;
        carta::PlaybackRange candidateRange;
        bool prepared = false;
        bool routing = false;
        size_t anchorIndex = 0;

        if (groupDragGeneration != editGeneration) {
            error = "Project changed during the drag.";
        } else {
            // The message thread is the only writer of session. Build and
            // validate the candidate while playback reads the old project.
            const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
            if (groupCommitPreparationHook) {
                groupCommitPreparationHook();
            }
#endif
            prepared = groupDragCopy
                           ? carta::prepareClipGroupCopy(session, candidate, groupDragClips,
                                                         groupDragSamples, groupDragTracks, error,
                                                         &movedRefs)
                           : carta::prepareClipGroupMove(session, candidate, groupDragClips,
                                                         groupDragSamples, groupDragTracks, error,
                                                         &movedRefs);
            if (prepared) {
                prepared = nextSelection.restore(candidate, movedRefs, editGeneration + 1);
                if (!prepared) {
                    error = "Could not select the moved clips.";
                } else {
#if CARTA_STUDIO_COMPONENT_TEST
                    if (preparedSelectionHook) {
                        preparedSelectionHook();
                    }
#endif
                    std::set<int> destinations;
                    for (const auto& ref : groupDragClips) {
                        destinations.insert(ref.track + groupDragTracks);
                    }
                    for (int destination : destinations) {
                        const auto& track = candidate.tracks[static_cast<size_t>(destination)];
                        if (!carta::clipRoutingConfirmed(track, carta::maxTrackChannels(track))) {
                            routing = true;
                        }
                    }
                    const auto anchor = std::find_if(groupDragClips.begin(), groupDragClips.end(),
                                                     [this](const auto& ref) {
                                                         return ref.track == groupDragAnchorTrack &&
                                                                ref.clip == groupDragAnchorClip;
                                                     });
                    anchorIndex =
                        static_cast<size_t>(std::distance(groupDragClips.begin(), anchor));
                    candidateRange = carta::fullMasterPlaybackRange(candidate);
#if CARTA_STUDIO_COMPONENT_TEST
                    if (historySelectionSnapshotHook) {
                        historySelectionSnapshotHook();
                    }
#endif
                    historyClips = timelineClipSelection.get(editGeneration);
                    historyTracks = selectedTimelineTracks();
                }
            }
        }
        if (prepared && groupDragGeneration != editGeneration) {
            prepared = false;
            error = "Project changed during the drag.";
        }
        if (prepared) {
            carta::EditorState before{
                {},           timeSelection,           selectedTrack,
                selectedClip, selectedMarker,          masterChannels,
                position,     std::move(historyClips), std::move(historyTracks)};
            std::swap(session, candidate);
            before.session = std::move(candidate);
            timelineClipSelection = std::move(nextSelection);
            selectedTrack = groupDragAnchorTrack + groupDragTracks;
            selectedClip = anchorIndex < movedRefs.size() ? movedRefs[anchorIndex].clip : -1;
            pushPreparedHistory(std::move(before), groupDragCopy ? "Copy clips" : "Move clips",
                                candidateRange);
            status.setText(
                juce::String(groupDragCopy
                                 ? "Copied selected clips; overlaps are mixed and markers "
                                   "stayed in place."
                                 : "Moved selected clips; overlaps are mixed and markers stayed "
                                   "in place.") +
                    (routing ? " Confirm destination routing before export." : ""),
                juce::dontSendNotification);
        }
        if (error.isNotEmpty()) {
            status.setText(error, juce::dontSendNotification);
        }
    } else if (clipPointerDragging && !groupDragValid) {
        status.setText(groupDragError, juce::dontSendNotification);
    }

    dragMode = kDragNone;
    clipPointerDragging = false;
    snapGuideVisible = false;
    dragged = false;
    groupDragCopy = false;
    {
        const juce::ScopedUnlock unlock(lock);
        groupDragClips.clear();
        selectionBeforeClips.clear();
        selectionClipSnapshotValid = false;
        viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()), viewStart);
        updateCursor(point);
        menuItemsChanged();
        repaint();
    }
}

void Studio::updateDraggedMasterEnd() {
    if (dragMode != kDragMarkerOrMasterEnd || !draggingMasterEnd) {
        return;
    }
    // The caller holds the audio lock. Pointer calculations and status
    // updates only use message-thread state; publish the duration and
    // matching playback range together under the lock.
    const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
    if (masterEndPreviewHook) {
        masterEndPreviewHook();
    }
#endif
    const auto area = timeline();
    const int x = juce::jlimit(area.getX() + labelWidth, area.getRight(), clipPointer.x);
    const auto requested = snappedPointerSample(x, clipSnapAllowed);
    const auto sample = std::clamp(requested, dragMasterFloor, 9007199254740991LL);
    if (sample != requested) {
        snapGuideVisible = false;
    }
    const auto minimum = sample == dragMasterOriginalEnd ? dragBefore.minimumEndSample : sample;
    // dragMasterFloor already includes the original audio end; the chosen
    // minimum end therefore equals the resulting project end.
    const auto end = sample;
    {
        const juce::ScopedLock guard(lock);
        session.minimumEndSample = minimum;
        dragged = minimum != dragBefore.minimumEndSample;
        dragMasterRange = {0, static_cast<double>(end) / carta::sessionRate, false};
    }
    status.setText("Master end: sample " + juce::String(end) + " (exclusive).",
                   juce::dontSendNotification);
    repaint();
}

void Studio::updateDraggedClip() {
    const bool singleClipDrag = dragMode == kDragClipMove || dragMode == kDragClipTrimLeft ||
                                dragMode == kDragClipTrimRight;
    if (!singleClipDrag || !validSelection()) {
        return;
    }
    const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
    if (singleClipPreviewHook) {
        singleClipPreviewHook();
    }
#endif
    setMouseCursor(dragMode == kDragClipMove ? juce::MouseCursor::DraggingHandCursor
                                             : juce::MouseCursor::LeftRightResizeCursor);

    const auto area = timeline();
    const double delta = carta::clipDragDelta(clipPointer.x, dragX, area.getX() + labelWidth,
                                              area.getRight(), viewStart, dragViewStart, dragSpan);
    auto clip = dragOriginal;
    bool changed = false;
    const auto publish = [&] {
        const juce::ScopedLock guard(lock);
        std::swap(clip, session.tracks[static_cast<size_t>(selectedTrack)]
                            .clips[static_cast<size_t>(selectedClip)]);
        dragged = changed;
        if (changed && !draggingFade) {
            refreshClipDragRange();
        }
    };

    if (draggingFade) {
        snapGuideVisible = false;
        if (std::abs(delta) < 1.0e-12) {
            publish();
            repaint();
            return;
        }
        const bool in = dragMode == kDragClipTrimLeft;
        const double endpoint =
            in ? (dragOriginal.fadeIn > 0
                      ? dragOriginal.fadeInStart + dragOriginal.fadeIn - dragOriginal.offset
                      : 0.0)
               : (dragOriginal.fadeOut > 0
                      ? dragOriginal.fadeOutEnd - dragOriginal.fadeOut - dragOriginal.offset
                      : dragOriginal.duration);
        const double initial = std::clamp(endpoint, 0.0, dragOriginal.duration);
        const double duration =
            in ? std::clamp(initial + delta, 0.0, dragOriginal.duration)
               : dragOriginal.duration - std::clamp(initial + delta, 0.0, dragOriginal.duration);
        if (std::abs(duration - (in ? initial : dragOriginal.duration - initial)) < 1.0e-12) {
            publish();
            repaint();
            return;
        }
        if (carta::setClipFade(clip, in, duration)) {
            changed = in ? (std::abs(clip.fadeIn - dragOriginal.fadeIn) > 1.0e-12 ||
                            std::abs(clip.fadeInStart - dragOriginal.fadeInStart) > 1.0e-12)
                         : (std::abs(clip.fadeOut - dragOriginal.fadeOut) > 1.0e-12 ||
                            std::abs(clip.fadeOutEnd - dragOriginal.fadeOutEnd) > 1.0e-12);
        }
        if (!changed) {
            clip = dragOriginal;
        }
        publish();
        repaint();
        return;
    }

    const double minimum = std::min(1.0 / carta::sessionRate, dragOriginal.duration);
    const double oldEnd = dragOriginal.start + dragOriginal.duration;
    double lower = 0;
    double upper = 0;
    double proposed = 0;
    double trailing = 0;
    if (dragMode == kDragClipMove) {
        lower = 0;
        upper = 90000000000.0;
        proposed = dragOriginal.start + delta;
        trailing = dragOriginal.duration;
    } else if (dragMode == kDragClipTrimLeft) {
        lower = std::max(0.0, dragOriginal.start - dragOriginal.offset);
        upper = oldEnd - minimum;
        proposed = dragOriginal.start + delta;
    } else {
        lower = dragOriginal.start + minimum;
        upper = dragOriginal.start + dragOriginal.audio->length() - dragOriginal.offset;
        proposed = oldEnd + delta;
    }

    const bool snapping = snapMode != 0 && clipSnapAllowed;
    const auto& targets = (snapping && snapMode == 2) ? dragTargets : emptySnapTargets;
    const double radius = snapping && snapMode == 2
                              ? static_cast<double>(kSnapBoundaryRadiusPx) * dragSpan /
                                    std::max(1, area.getWidth() - labelWidth)
                              : 0.0;
    const auto snapped = carta::snapTimelinePosition(proposed, lower, upper, snapping, radius,
                                                     targets, trailing, snapGridSamples);
    snapGuideVisible = snapped.hasGuide;
    snapGuideTime = snapped.guide;

    const double originalEdge = dragMode == kDragClipTrimRight ? oldEnd : dragOriginal.start;
    if (std::abs(snapped.time - originalEdge) < 1.0e-12) {
        publish();
        repaint();
        return;
    }

    if (dragMode == kDragClipMove) {
        clip.start = snapped.time;
    } else {
        const auto gridSample = [](double time, juce::int64& sample) {
            const double frame = time * carta::sessionRate;
            const double rounded = std::round(frame);
            if (!std::isfinite(frame) || rounded < 0 || rounded >= 9007199254740992.0 ||
                std::abs(frame - rounded) >= 1.0e-7) {
                return false;
            }
            sample = static_cast<juce::int64>(rounded);
            return true;
        };
        juce::int64 first = 0;
        juce::int64 last = 0;
        const double newStart = dragMode == kDragClipTrimLeft ? snapped.time : dragOriginal.start;
        const double newEnd = dragMode == kDragClipTrimRight ? snapped.time : oldEnd;
        if (snapping && gridSample(newStart, first) && gridSample(newEnd, last)) {
            juce::String error;
            if (!carta::trimClipToSamples(clip, first, last, error)) {
                clip = dragOriginal;
                snapGuideVisible = false;
                publish();
                status.setText(error, juce::dontSendNotification);
                repaint();
                return;
            }
        } else if (dragMode == kDragClipTrimLeft) {
            const double shift = snapped.time - dragOriginal.start;
            clip.start = snapped.time;
            clip.offset = std::max(0.0, dragOriginal.offset + shift);
            clip.duration = dragOriginal.duration - shift;
        } else {
            clip.duration = std::min(snapped.time - dragOriginal.start,
                                     dragOriginal.audio->length() - dragOriginal.offset);
        }
    }

    if (!carta::validTimelineGeometry(clip.start, clip.offset, clip.duration)) {
        clip = dragOriginal;
        snapGuideVisible = false;
        publish();
        status.setText("Clip would extend beyond the precise timeline range.",
                       juce::dontSendNotification);
        repaint();
        return;
    }

    changed = !sameClipGeometry(clip, dragOriginal);
    if (changed && (dragMode == kDragClipTrimLeft || dragMode == kDragClipTrimRight)) {
        juce::int64 first = 0;
        juce::int64 last = 0;
        if (carta::clipSampleRange(clip, first, last)) {
            status.setText("Trim preview: project samples [" + juce::String(first) + ", " +
                               juce::String(last) + utf8Str(") — ") + juce::String(last - first) +
                               " samples. Escape cancels.",
                           juce::dontSendNotification);
        } else {
            status.setText(
                "Trim preview: no project samples fall inside this clip. Escape cancels.",
                juce::dontSendNotification);
        }
    }
    publish();
    repaint();
}

void Studio::updateDraggedPlayhead() {
    if (dragMode != kDragPlayhead) {
        return;
    }
    // Caller holds the playback lock; only publishing the seek requires it.
    const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif
    const auto area = timeline();
    const int x =
        juce::jlimit(area.getX() + labelWidth, std::max(area.getX() + labelWidth, area.getRight()),
                     playheadPointer.x);
    const auto target =
        static_cast<double>(snappedPointerSample(x, playheadSnapAllowed)) / carta::sessionRate;
    {
        const juce::ScopedLock guard(lock);
        position = target;
    }
    repaint();
}

void Studio::updateDraggedLoop() {
    if (dragMode != kDragSavedLoop) {
        return;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif
    const auto area = timeline();
    const int x = juce::jlimit(area.getX() + labelWidth,
                               std::max(area.getX() + labelWidth, area.getRight()), loopPointer.x);
    const auto sample = juce::jlimit(juce::int64{0}, carta::projectEndSample(session),
                                     snappedPointerSample(x, loopSnapAllowed));
    loopDragPreview = loopDragOriginal;
    if (loopDragEnd) {
        loopDragPreview.end = std::max(loopDragOriginal.start + 1, sample);
    } else {
        loopDragPreview.start = std::min(loopDragOriginal.end - 1, sample);
    }
    status.setText("Loop preview: samples " + juce::String(loopDragPreview.start) + " to " +
                       juce::String(loopDragPreview.end) + " (end exclusive).",
                   juce::dontSendNotification);
    repaint();
}

void Studio::updateDraggedSelection() {
    if ((dragMode != kDragTimeSelection && dragMode != kDragTimeSelectionResize) ||
        session.tracks.empty()) {
        return;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif
    const auto area = timeline();
    const int x =
        juce::jlimit(area.getX() + labelWidth, std::max(area.getX() + labelWidth, area.getRight()),
                     selectionPointer.x);
    const auto current = snappedPointerSample(x, selectionSnapAllowed);
    const int y = juce::jlimit(area.getY() + timelineHeaderHeight(),
                               std::max(area.getY() + timelineHeaderHeight(), area.getBottom() - 1),
                               selectionPointer.y);
    const int hit = trackAt(y);
    const int track =
        juce::jlimit(firstTrack, std::max(firstTrack, static_cast<int>(session.tracks.size()) - 1),
                     hit < 0 ? static_cast<int>(session.tracks.size()) - 1 : hit);
    if (dragMode == kDragTimeSelection) {
        timeSelection = {
            std::min(selectionAnchorSample, current), std::max(selectionAnchorSample, current),
            std::min(selectionAnchorTrack, track), std::max(selectionAnchorTrack, track)};
    } else {
        timeSelection =
            carta::resizeTimeSelection(selectionResizeBase, selectionResizeLeft, current, track);
        // Horizontal resizing keeps the independent rows; changing the vertical
        // extent explicitly switches back to the contiguous range under the drag.
        timelineTracks = (timeSelection.firstTrack == selectionResizeBase.firstTrack &&
                          timeSelection.lastTrack == selectionResizeBase.lastTrack)
                             ? selectionBeforeTracks
                             : std::set<int>{};
    }
    status.setText("Selection: samples " + juce::String(timeSelection.start) + " to " +
                       juce::String(timeSelection.end) + " (end exclusive).",
                   juce::dontSendNotification);
    repaint();
}

void Studio::restoreGestureClipSelection() {
    if (selectionClipSnapshotValid) {
        timelineClipSelection = std::move(selectionBeforeClips);
        selectionClipSnapshotValid = false;
    }
}

void Studio::preparePersistentGroupGesture(juce::Point<int> point) {
    const auto area = timeline();
    if (point.x < area.getX() + labelWidth || fadeHandleAt(point) != 0) {
        return;
    }
    const int track = trackAt(point.y);
    if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
        return;
    }
    const auto& clips = session.tracks[static_cast<size_t>(track)].clips;
    int hit = -1;
    const auto atEdge = [&](int index) {
        const auto rect = clipRect(clips[static_cast<size_t>(index)], track);
        return rect.contains(point) && (point.x < rect.getX() + kClipEdgeHitPx ||
                                        point.x > rect.getRight() - kClipEdgeHitPx);
    };

    int bodyHit = -1;
    if (track == selectedTrack && selectedClip >= 0 &&
        selectedClip < static_cast<int>(clips.size()) &&
        clipRect(clips[static_cast<size_t>(selectedClip)], track).contains(point)) {
        bodyHit = selectedClip;
    }
    if (bodyHit < 0) {
        for (int index = static_cast<int>(clips.size()) - 1; index >= 0; --index) {
            if (clipRect(clips[static_cast<size_t>(index)], track).contains(point)) {
                bodyHit = index;
                break;
            }
        }
    }
    if (bodyHit >= 0 && atEdge(bodyHit)) {
        hit = bodyHit;
    }

    const auto row = trackArea(track);
    if (hit < 0 && point.y >= row.getY() + 4 && point.y < row.getY() + 4 + titleHeight) {
        for (const auto& region : titleRegions(track)) {
            if (region.bounds.contains(point.x)) {
                hit = region.clip;
                break;
            }
        }
    }
    if (hit < 0 || clips[static_cast<size_t>(hit)].groupId.isEmpty()) {
        return;
    }

    std::vector<carta::ClipReference> requested;
    if (timelineClipSelection.contains(track, hit, editGeneration)) {
        requested = timelineClipSelection.get(editGeneration);
    } else {
        requested.push_back({track, hit});
    }
    carta::ClipSelection expanded;
    if (!expanded.selectExpanded(session, requested, editGeneration)) {
        return;
    }
    selectionBeforeClips = timelineClipSelection;
    selectionClipSnapshotValid = true;
    timelineClipSelection = std::move(expanded);
}

void Studio::restoreTrackDragSelection() {
    timeSelection = dragBeforeState.selection;
    selectedTrack = dragBeforeState.selectedTrack;
    selectedClip = dragBeforeState.selectedClip;
    selectedMarker = dragBeforeState.selectedMarker;
    timelineClipSelection.restore(session, dragBeforeState.selectedClips, editGeneration);
    timelineTracks.clear();
    for (int track : dragBeforeState.selectedTracks) {
        timelineTracks.insert(track);
    }
    timelineTrackRevision = editGeneration;
}

void Studio::updateDraggedTrack() {
#if CARTA_STUDIO_COMPONENT_TEST
    if (navigationPreviewHook) {
        navigationPreviewHook();
    }
#endif
    const auto area = timeline();
    trackDragSlot = -1;
    if (trackDragMoved && trackDragPointer.x >= area.getX() &&
        trackDragPointer.x < area.getRight() &&
        trackDragPointer.y >= area.getY() + timelineHeaderHeight() &&
        trackDragPointer.y < area.getBottom()) {
        const auto visible = carta::visibleTrackRows(
            session, firstTrack, std::max(0, area.getHeight() - timelineHeaderHeight()));
        for (int track = visible.getStart(); track < visible.getEnd(); ++track) {
            const auto row = trackArea(track);
            if (trackDragPointer.y < row.getCentreY()) {
                trackDragSlot = track;
                break;
            }
            trackDragSlot = track + 1;
        }
    }
    repaint();
}

void Studio::updateDraggedChannelHeight(int y) {
    if (channelDragTrack < 0 || channelDragTrack >= static_cast<int>(session.tracks.size()) ||
        channelDragLane < 0 || channelDragLane + 1 >= static_cast<int>(channelDragScaled.size()) ||
        channelDragPairPixels < 8) {
        return;
    }
    const int lane = channelDragLane;
    const int left = channelDragScaled[static_cast<size_t>(lane)];
    const int right = channelDragScaled[static_cast<size_t>(lane + 1)];
    const int total = left + right;
    const auto delta = static_cast<juce::int64>(y) - channelDragStartY;
    const auto requested = static_cast<juce::int64>(left) +
                           static_cast<juce::int64>(std::llround(static_cast<double>(delta) *
                                                                 total / channelDragPairPixels));
    const int minPixels = std::min(6, channelDragPairPixels / 3);
    const int minimum = std::max(
        {1, total - 1000, (total * minPixels + channelDragPairPixels - 1) / channelDragPairPixels});
    const int maximum = std::min(
        {1000, total - 1, total * (channelDragPairPixels - minPixels) / channelDragPairPixels});
    if (minimum > maximum) {
        return;
    }
    const int next = static_cast<int>(std::clamp(requested, static_cast<juce::int64>(minimum),
                                                 static_cast<juce::int64>(maximum)));
    auto& weights = session.tracks[static_cast<size_t>(channelDragTrack)].channelHeightWeights;
    if (delta == 0 || next == left) {
        weights = channelDragOriginal;
    } else {
        weights = channelDragScaled;
        weights[static_cast<size_t>(lane)] = next;
        weights[static_cast<size_t>(lane + 1)] = total - next;
    }
    dragged = weights != channelDragOriginal;
    repaint();
}

// ============================================================================
// Track Header Control Formatting, Drag Math & Edge Auto-Scrolling
// ============================================================================

juce::String Studio::formatHeaderGainDb(float gain) {
    if (!(gain > 0.0f) || !std::isfinite(gain)) {
        return "-inf";
    }
    const double db = 20.0 * std::log10(static_cast<double>(gain));
    if (std::abs(db) < 0.05) {
        return "0.0dB";
    }
    return (db > 0.0 ? juce::String("+") : juce::String()) + juce::String(db, 1) + "dB";
}

juce::String Studio::formatHeaderBalance(float pan) {
    const int pct = juce::roundToInt(pan * 100.0f);
    if (pct == 0) {
        return "C";
    }
    return (pct < 0 ? juce::String("L") : juce::String("R")) + juce::String(std::abs(pct));
}

float Studio::headerDraggedGain(float startGain, int dx, bool fine) {
    const double startDb =
        startGain > 0.0f
            ? std::clamp(20.0 * std::log10(static_cast<double>(startGain)), -60.0, 24.1)
            : -60.0;
    double db = std::clamp(startDb + static_cast<double>(dx) * (fine ? 0.1 : 0.25), -60.0,
                           24.082399653118496);
    if (!fine && std::abs(db) < 0.25) {
        db = 0.0;
    }
    if (db <= -59.95) {
        return 0.0f;
    }
    return std::clamp(static_cast<float>(std::pow(10.0, db / 20.0)), 0.0f, carta::maxLinearGain);
}

float Studio::headerDraggedBalance(float startPan, int dx, bool fine) {
    float pan =
        std::clamp(startPan + static_cast<float>(dx) * (fine ? 0.005f : 0.015f), -1.0f, 1.0f);
    if (!fine && std::abs(pan) < 0.025f) {
        pan = 0.0f;
    }
    return pan;
}

void Studio::toggleHeaderTrackControl(int track, int hit) {
    if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
        return;
    }
    selectedTrack = track;
    juce::String error;
    if (hit == HeaderHit::collapse) {
        {
            const juce::ScopedLock guard(lock);
            stopSpecialAudition();
        }
        const bool changed = mutateMixState(
            [track](carta::Session& candidate) {
                if (track < 0 || track >= static_cast<int>(candidate.tracks.size())) {
                    return false;
                }
                auto& tr = candidate.tracks[static_cast<size_t>(track)];
                tr.collapsed = !tr.collapsed;
                return true;
            },
            error, "Toggle track collapse");
        if (changed) {
            revealTrack(track);
            menuItemsChanged();
            repaint();
            status.setText("Track display updated.", juce::dontSendNotification);
        } else if (error.isNotEmpty()) {
            status.setText(error, juce::dontSendNotification);
        }
        return;
    }

    const bool isMute = (hit == HeaderHit::mute);
    const bool changed = mutateMixState(
        [track, isMute](carta::Session& candidate) {
            if (track < 0 || track >= static_cast<int>(candidate.tracks.size())) {
                return false;
            }
            auto& tr = candidate.tracks[static_cast<size_t>(track)];
            if (isMute) {
                tr.muted = !tr.muted;
            } else {
                tr.solo = !tr.solo;
            }
            return true;
        },
        error, isMute ? "Toggle track mute" : "Toggle track solo");

    const auto message =
        changed
            ? "Track " + juce::String(track + 1) + " " +
                  session.tracks[static_cast<size_t>(track)].name + ": " +
                  (isMute
                       ? (session.tracks[static_cast<size_t>(track)].muted ? "muted." : "unmuted.")
                       : (session.tracks[static_cast<size_t>(track)].solo ? "solo on."
                                                                          : "solo off."))
            : error;
    status.setText(message, juce::dontSendNotification);
    if (changed) {
        announceNavigation(message);
    }
}

void Studio::scrollClipDrag() {
#if CARTA_STUDIO_COMPONENT_TEST
    if (scrollGeometryHook) {
        scrollGeometryHook();
    }
#endif
    const double now = juce::Time::getMillisecondCounterHiRes();
    const auto area = timeline();
    const double before = viewStart;
    const double elapsed = std::clamp((now - clipScrollClock) / 1000.0, 0.0, 0.1);
    viewStart = carta::selectionEdgeScroll(
        viewStart, dragSpan, std::max(dragFullLength, fullLength()), area.getX() + labelWidth,
        area.getRight(), clipPointer.x, elapsed);
    clipScrollClock = now;
    clipSnapAllowed = !juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();

    bool verticalMoved = false;
    const int count = static_cast<int>(session.tracks.size());
    if ((dragMode == kDragClipMove || dragMode == kDragGroupMove) && count > 0 && firstTrack >= 0 &&
        firstTrack < count) {
        const int direction = carta::selectionTrackScrollDirection(
            area.getY() + timelineHeaderHeight(), area.getBottom(), clipPointer.y, firstTrack,
            count, trackArea(count - 1).getBottom());
        if (direction != clipVerticalDirection) {
            clipVerticalDirection = direction;
            clipVerticalElapsed = 0;
        }
        if (direction != 0) {
            clipVerticalElapsed += elapsed;
            if (clipVerticalElapsed >= 0.2) {
                firstTrack += direction;
                clipVerticalElapsed -= 0.2;
                verticalMoved = true;
            }
        }
    }

    if (dragMode == kDragGroupMove && (verticalMoved || viewStart < before || viewStart > before)) {
        updateGroupDrag();
    } else if (dragMode == kDragGroupEdge && (viewStart < before || viewStart > before)) {
        updateGroupEdgeDrag();
    } else if (viewStart < before || viewStart > before) {
        const juce::ScopedLock guard(lock);
        if (draggingMasterEnd) {
            updateDraggedMasterEnd();
        } else {
            updateDraggedClip();
        }
    }

    if (dragMode == kDragClipMove && (verticalMoved || viewStart < before || viewStart > before)) {
        {
            const juce::ScopedLock guard(lock);
            updateClipDropTarget();
        }
        repaint();
    }
}
