/**
 * @file MdWorkspace.cpp
 * Implementation of the docked MiniDisc authoring & NetMD recording workspace.
 *
 * UI / IPC & Coordinate Contract:
 * - Translates JUCE list/button/text-editor interactions into `disc ...` IPC
 *   commands via `onCommand` with optimistic-concurrency `revision` checks.
 * - Maintains synchronized selection across the Tab 1 logical track list, the
 *   selected track's UTOC part list, and Tab 2's target recording plan list.
 * - Renders 44.1 kHz timeline sample intervals alongside UTOC physical
 *   `cluster:sector.group` coordinates and cluster/slot budget indicators.
 * - All methods execute on the JUCE message thread.
 */

#include "MiniDisc/MdWorkspace.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace {

// ============================================================================
// Internal Row & Range Helpers
// ============================================================================

void paintAlternatingRowBackground(juce::Graphics& g, int row, int width, int height, bool selected,
                                   bool dark) {
    const auto rowBg =
        selected ? (dark ? juce::Colour(0xff3c3c3c) : juce::Colour(0xffdadada))
                 : ((row % 2 == 1) ? (dark ? juce::Colour(0xff2b2b2b) : juce::Colour(0xfff3f3f3))
                                   : juce::Colours::transparentBlack);

    if (!rowBg.isTransparent()) {
        g.setColour(rowBg);
        g.fillRect(0, 0, width, height);
    }
}

int countPlacementAudioRefs(const carta::MdPhysicalPlan& plan, std::size_t placementIdx) {
    if (placementIdx >= plan.placements.size()) {
        return 1;
    }
    const auto& me = plan.placements[placementIdx];
    if (me.isOnDisc) {
        const auto myS = me.startAddr.toLinearUnits();
        const auto myE = me.endAddr.toLinearUnits();
        int count = 0;
        for (const auto& other : plan.placements) {
            if (!other.isOnDisc) {
                continue;
            }
            const auto oS = other.startAddr.toLinearUnits();
            const auto oE = other.endAddr.toLinearUnits();
            if (oS <= myE && myS <= oE) {
                ++count;
            }
        }
        return std::max(1, count);
    }
    if (me.spanIndex >= 0) {
        const int myS = me.unitOffsetInSpan;
        const int myE = me.unitOffsetInSpan + me.unitCount;
        int count = 0;
        for (const auto& other : plan.placements) {
            if (other.isOnDisc || other.spanIndex != me.spanIndex) {
                continue;
            }
            const int oS = other.unitOffsetInSpan;
            const int oE = other.unitOffsetInSpan + other.unitCount;
            if (oS < myE && myS < oE) {
                ++count;
            }
        }
        return std::max(1, count);
    }
    return 1;
}

juce::String compactClusterLabel(int startCluster, int endCluster) {
    const auto enDash = juce::String::fromUTF8("\xe2\x80\x93");
    if (startCluster == endCluster) {
        return "C" + juce::String(startCluster);
    }
    return "C" + juce::String(startCluster) + enDash + juce::String(endCluster);
}

juce::String formatMegabytes(std::int64_t bytes) {
    if (bytes <= 0) {
        return "0 MB";
    }
    if (bytes < 1024LL * 1024LL) {
        return juce::String((bytes + 1023LL) / 1024LL) + " KB";
    }
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 100.0) {
        return juce::String(static_cast<juce::int64>(std::llround(mb))) + " MB";
    }
    return juce::String(mb, 1) + " MB";
}

bool resolveSelectedOrPlayheadRange(const carta::TimeSelection& sel, juce::int64 playhead,
                                    juce::int64 masterEnd, juce::int64& outStart,
                                    juce::int64& outEnd) noexcept {
    if (sel.hasRange()) {
        outStart = sel.start;
        outEnd = sel.end;
        return true;
    }
    if (masterEnd > 0) {
        outStart = (playhead >= 0 && playhead < masterEnd) ? playhead : 0;
        outEnd = masterEnd;
        return true;
    }
    return false;
}

} // namespace

// ============================================================================
// Nested ListBox Models & Keyboard Handling
// ============================================================================

MdWorkspace::ReorderableListBox::ReorderableListBox(const juce::String& name,
                                                    juce::ListBoxModel* modelRef)
    : juce::ListBox(name, modelRef) {
    addMouseListener(this, true);
}

MdWorkspace::ReorderableListBox::~ReorderableListBox() {
    removeMouseListener(this);
}

bool MdWorkspace::ReorderableListBox::isIgnoredChildComponent(
    const juce::Component* comp) const noexcept {
    if (comp == nullptr) {
        return false;
    }
    if (dynamic_cast<const juce::ScrollBar*>(comp) != nullptr ||
        comp->findParentComponentOfClass<juce::ScrollBar>() != nullptr) {
        return true;
    }
    if (dynamic_cast<const juce::TextEditor*>(comp) != nullptr ||
        comp->findParentComponentOfClass<juce::TextEditor>() != nullptr) {
        return true;
    }
    return false;
}

int MdWorkspace::ReorderableListBox::computeInsertBeforeRow(juce::Point<int> pt) {
    auto* m = getListBoxModel();
    const int totalRows = m ? m->getNumRows() : 0;
    if (totalRows <= 0) {
        return -1;
    }
    const int hitRow = getRowContainingPosition(pt.x, pt.y);
    if (hitRow >= 0 && hitRow < totalRows) {
        const auto pos = getRowPosition(hitRow, true);
        return juce::jlimit(0, totalRows, (pt.y >= pos.getCentreY()) ? (hitRow + 1) : hitRow);
    }
    const auto firstRect = getRowPosition(0, true);
    return (pt.y < firstRect.getY()) ? 0 : totalRows;
}

void MdWorkspace::ReorderableListBox::mouseDown(const juce::MouseEvent& event) {
    // Only handle child row events or direct events once; guard against duplicate callback when
    // originalComponent == this because juce::Component delivers its own mouse events natively too.
    if (event.originalComponent == this && event.eventComponent == this) {
        juce::ListBox::mouseDown(event);
    }
    if (!dragReorderEnabled || !onRowReorder || !event.mods.isLeftButtonDown() ||
        event.mods.isPopupMenu() || event.mods.isShiftDown() || event.mods.isCommandDown() ||
        event.mods.isCtrlDown() || event.mods.isAltDown() ||
        isIgnoredChildComponent(event.originalComponent)) {
        dragCandidate = false;
        dragActive = false;
        dragFromRow = -1;
        dragInsertRow = -1;
        return;
    }
    const auto pt = event.getEventRelativeTo(this).getPosition();
    const int row = getRowContainingPosition(pt.x, pt.y);
    if (row < 0 || (canStartDrag && !canStartDrag(row))) {
        dragCandidate = false;
        dragActive = false;
        dragFromRow = -1;
        dragInsertRow = -1;
        return;
    }
    dragCandidate = true;
    dragActive = false;
    dragFromRow = row;
    dragInsertRow = -1;
    dragStartY = pt.y;
}

void MdWorkspace::ReorderableListBox::mouseDrag(const juce::MouseEvent& event) {
    if (event.originalComponent == this && event.eventComponent == this) {
        juce::ListBox::mouseDrag(event);
    }
    if (!dragCandidate) {
        return;
    }
    const auto pt = event.getEventRelativeTo(this).getPosition();
    if (!dragActive && std::abs(pt.y - dragStartY) >= 6) {
        dragActive = true;
    }
    if (dragActive) {
        dragInsertRow = computeInsertBeforeRow(pt);
        repaint();
    }
}

void MdWorkspace::ReorderableListBox::mouseUp(const juce::MouseEvent& event) {
    if (event.originalComponent == this && event.eventComponent == this) {
        juce::ListBox::mouseUp(event);
    }
    if (dragCandidate) {
        const bool shouldCommit =
            dragActive && dragFromRow >= 0 && dragInsertRow >= 0 && onRowReorder;
        const int fromRow = dragFromRow;
        const int insertRow = dragInsertRow;
        dragCandidate = false;
        dragActive = false;
        dragFromRow = -1;
        dragInsertRow = -1;
        repaint();
        if (shouldCommit) {
            onRowReorder(fromRow, insertRow);
        }
    }
}

void MdWorkspace::ReorderableListBox::paintOverChildren(juce::Graphics& g) {
    juce::ListBox::paintOverChildren(g);
    if (!dragActive || dragInsertRow < 0) {
        return;
    }
    auto* m = getListBoxModel();
    const int totalRows = m ? m->getNumRows() : 0;
    if (totalRows <= 0) {
        return;
    }
    int lineY = 1;
    if (dragInsertRow < totalRows) {
        lineY = getRowPosition(dragInsertRow, true).getY();
    } else {
        lineY = getRowPosition(totalRows - 1, true).getBottom() - 2;
    }
    lineY = juce::jlimit(1, std::max(1, getHeight() - 3), lineY);
    const bool dark = juce::Desktop::getInstance().isDarkModeActive();
    const auto accent = dark ? juce::Colour(0xff5eead4) : juce::Colour(0xff0f766e);
    const int barW = std::max(16, getVisibleContentWidth() - 4);
    g.setColour(accent);
    g.fillRect(2, lineY, barW, 2);
    g.fillEllipse(0.0f, static_cast<float>(lineY - 2), 6.0f, 6.0f);
}

MdWorkspace::TrackListModel::TrackListModel(MdWorkspace& ownerRef) : owner(ownerRef) {
}

int MdWorkspace::TrackListModel::getNumRows() {
    return static_cast<int>(owner.trackRows.size());
}

void MdWorkspace::TrackListModel::paintListBoxItem(int row, juce::Graphics& g, int width,
                                                   int height, bool selected) {
    owner.paintTrackRow(row, g, width, height, selected);
}

void MdWorkspace::TrackListModel::selectedRowsChanged(int lastRowSelected) {
    owner.handleTrackSelectionChanged(lastRowSelected);
}

void MdWorkspace::TrackListModel::listBoxItemClicked(int row, const juce::MouseEvent& event) {
    owner.handleTrackClick(row, event);
}

void MdWorkspace::TrackListModel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) {
    owner.handleTrackDoubleClick(row);
}

void MdWorkspace::TrackListModel::returnKeyPressed(int row) {
    owner.handleTrackDoubleClick(row);
}

void MdWorkspace::TrackListModel::deleteKeyPressed(int) {
    owner.triggerRemoveSelectedTracks();
}

juce::String MdWorkspace::TrackListModel::getTooltipForRow(int row) {
    return owner.tooltipForTrackRow(row);
}

MdWorkspace::TrackListBox::TrackListBox(MdWorkspace& ownerRef, TrackListModel& modelRef)
    : ReorderableListBox("MiniDisc tracks", &modelRef), owner(ownerRef) {
}

bool MdWorkspace::TrackListBox::keyPressed(const juce::KeyPress& key) {
    if (key.isKeyCode(juce::KeyPress::F2Key)) {
        if (key.getModifiers().isShiftDown()) {
            if (owner.onEditTrackForms) {
                owner.onEditTrackForms();
            }
        } else {
            owner.focusTrackRename();
        }
        return true;
    }
    if (!key.getModifiers().isAnyModifierKeyDown() && key.isKeyCode(juce::KeyPress::spaceKey)) {
        owner.triggerPreviewSelected();
        return true;
    }
    if (key == juce::KeyPress('c', juce::ModifierKeys::commandModifier, 0)) {
        if (!owner.selectedTrackIndices0().empty()) {
            owner.triggerCopySelectedTracks();
            return true;
        }
    }
    if (key == juce::KeyPress('v', juce::ModifierKeys::commandModifier, 0)) {
        if (!owner.copiedTrackClipboard.empty()) {
            const auto sel = owner.selectedTrackIndices0();
            const int afterRow =
                !sel.empty() ? sel.back() : (static_cast<int>(owner.trackRows.size()) - 1);
            owner.triggerPasteTracksAfter(afterRow);
            return true;
        }
    }
    return juce::ListBox::keyPressed(key);
}

MdWorkspace::PlanListModel::PlanListModel(MdWorkspace& ownerRef) : owner(ownerRef) {
}

int MdWorkspace::PlanListModel::getNumRows() {
    return owner.planViewMode == "bank" ? static_cast<int>(owner.audioBankRows.size())
                                        : static_cast<int>(owner.planRows.size());
}

void MdWorkspace::PlanListModel::paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                                                  bool selected) {
    if (owner.planViewMode == "bank") {
        owner.paintAudioBankRow(row, g, width, height, selected);
    } else {
        owner.paintPlanRow(row, g, width, height, selected);
    }
}

void MdWorkspace::PlanListModel::selectedRowsChanged(int lastRowSelected) {
    owner.handlePlanSelectionChanged(lastRowSelected);
}

void MdWorkspace::PlanListModel::listBoxItemClicked(int row, const juce::MouseEvent& event) {
    owner.handlePlanClick(row, event);
}

void MdWorkspace::PlanListModel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) {
    owner.triggerOpenPlanRow(row);
}

void MdWorkspace::PlanListModel::returnKeyPressed(int row) {
    owner.triggerOpenPlanRow(row);
}

juce::String MdWorkspace::PlanListModel::getTooltipForRow(int row) {
    return owner.planViewMode == "bank" ? owner.tooltipForAudioBankRow(row)
                                        : owner.tooltipForPlanRow(row);
}

MdWorkspace::DeviceTrackListModel::DeviceTrackListModel(MdWorkspace& ownerRef) : owner(ownerRef) {
}

int MdWorkspace::DeviceTrackListModel::getNumRows() {
    if (owner.recordUiState.activeDeviceIsHiMd && owner.recordUiState.hasHiMdStatus) {
        return static_cast<int>(owner.recordUiState.hiMdStatus.tracks.size());
    }
    return static_cast<int>(owner.recordUiState.deviceStatus.tracks.size());
}

void MdWorkspace::DeviceTrackListModel::paintListBoxItem(int row, juce::Graphics& g, int width,
                                                         int height, bool selected) {
    owner.paintDeviceTrackRow(row, g, width, height, selected);
}

void MdWorkspace::DeviceTrackListModel::listBoxItemClicked(int row, const juce::MouseEvent& event) {
    owner.handleDeviceTrackClick(row, event);
}

void MdWorkspace::DeviceTrackListModel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) {
    if (owner.recordUiState.activeDeviceIsHiMd && owner.onDumpHiMdTrack && !owner.recordBusy) {
        owner.onDumpHiMdTrack(row);
    }
}

juce::String MdWorkspace::DeviceTrackListModel::getTooltipForRow(int row) {
    return owner.tooltipForDeviceTrackRow(row);
}

MdWorkspace::InlineTrackTitleEditor::InlineTrackTitleEditor(MdWorkspace& ownerRef)
    : juce::TextEditor("Inline MD track title"), owner(ownerRef) {
}

bool MdWorkspace::InlineTrackTitleEditor::keyPressed(const juce::KeyPress& key) {
    if (!key.getModifiers().isAnyModifierKeyDown()) {
        if (key.isKeyCode(juce::KeyPress::upKey)) {
            owner.navigateInlineTrackEdit(-1);
            return true;
        }
        if (key.isKeyCode(juce::KeyPress::downKey)) {
            owner.navigateInlineTrackEdit(1);
            return true;
        }
        if (key.isKeyCode(juce::KeyPress::returnKey)) {
            owner.commitInlineTrackEdit(true);
            owner.trackList.grabKeyboardFocus();
            return true;
        }
        if (key.isKeyCode(juce::KeyPress::escapeKey)) {
            owner.cancelInlineTrackEdit();
            owner.trackList.grabKeyboardFocus();
            return true;
        }
    }
    return juce::TextEditor::keyPressed(key);
}

// ============================================================================
// Construction & Public State / IPC Synchronization
// ============================================================================

MdWorkspace::MdWorkspace()
    : trackModel(*this), trackList(*this, trackModel), inlineTrackTitleEditor(*this),
      deviceTrackModel(*this), deviceTrackList("On-disc tracks", &deviceTrackModel),
      planModel(*this), planList("Recording plan", &planModel) {
    // --- Persistent Header Bar ---
    addAndMakeVisible(mdLabel);
    mdLabel.setText("MiniDisc", juce::dontSendNotification);
    mdLabel.setFont(juce::FontOptions(13.0f, juce::Font::bold));

    addAndMakeVisible(discTitleEditor);
    discTitleEditor.setTextToShowWhenEmpty("Disc title (ASCII)", juce::Colours::grey);
    discTitleEditor.setReturnKeyStartsNewLine(false);
    discTitleEditor.setEscapeAndReturnKeysConsumed(true);
    discTitleEditor.setTooltip(
        "Disc title written to UTOC Sector 1 (Enter commits, Escape reverts)");
    discTitleEditor.onReturnKey = [this] {
        commitDiscTitle();
        giveAwayKeyboardFocus();
    };
    discTitleEditor.onFocusLost = [this] { commitDiscTitle(); };
    discTitleEditor.onEscapeKey = [this] {
        discTitleEditor.setText(discBaselineTitle, juce::dontSendNotification);
        giveAwayKeyboardFocus();
    };

    addAndMakeVisible(discFormsButton);
    discFormsButton.setTooltip("Edit ASCII and Full-Width/Japanese disc titles and disc timestamp");
    discFormsButton.onClick = [this] {
        commitPendingEdits();
        if (onEditDiscFormsAndTimestamp) {
            onEditDiscFormsAndTimestamp();
        }
    };

    addAndMakeVisible(mediumBox);
    mediumBox.addItem("MD 60", 1);
    mediumBox.addItem("MD 74", 2);
    mediumBox.addItem("MD 80", 3);
    mediumBox.addItem("Hi-MD 300M", 4);
    mediumBox.addItem("Hi-MD 1GB", 5);
    mediumBox.setSelectedId(3, juce::dontSendNotification);
    mediumBox.setTooltip("Target MiniDisc / Hi-MD physical capacity");
    mediumBox.onChange = [this] {
        if (syncingControls) {
            return;
        }
        commitPendingEdits();
        const auto medium = carta::md_ui::menuIdToMedium(mediumBox.getSelectedId());
        if (onCommand) {
            onCommand({"disc", "medium", juce::String(carta::mdMediumToString(medium))}, revision);
        }
    };

    addAndMakeVisible(codecBox);
    codecBox.setTooltip("Default codec for tracks without a per-track override");
    codecBox.onChange = [this] {
        if (syncingControls) {
            return;
        }
        commitPendingEdits();
        const auto codec = carta::md_ui::menuIdToCodec(codecBox.getSelectedId());
        if (onCommand) {
            onCommand({"disc", "codec", juce::String(carta::mdCodecModeToString(codec))}, revision);
        }
    };

    addAndMakeVisible(autoTrackToggle);
    autoTrackToggle.setToggleState(true, juce::dontSendNotification);
    autoTrackToggle.setTooltip(
        "Automatically add or extend MiniDisc tracks when importing clips into the arrangement");
    autoTrackToggle.onClick = [this] {
        if (syncingControls) {
            return;
        }
        commitPendingEdits();
        if (onCommand) {
            onCommand({"disc", "auto-track", autoTrackToggle.getToggleState() ? "on" : "off"},
                      revision);
        }
    };

    addAndMakeVisible(closeButton);
    closeButton.setButtonText(juce::String::fromUTF8("\xc3\x97"));
    closeButton.setTooltip("Hide MiniDisc authoring panel");
    closeButton.onClick = [this] {
        commitPendingEdits();
        if (onClose) {
            onClose();
        }
    };

    // --- Tab & Sync Bar ---
    addAndMakeVisible(tracksTabButton);
    tracksTabButton.setClickingTogglesState(false);
    tracksTabButton.onClick = [this] { setActiveTab("tracks", true); };

    addAndMakeVisible(recordTabButton);
    recordTabButton.setClickingTogglesState(false);
    recordTabButton.onClick = [this] { setActiveTab("record", true); };

    addAndMakeVisible(syncClipsButton);
    syncClipsButton.setTooltip("Build clean MiniDisc tracks across selected clips or active layers "
                               "(handles crossfades & ignores background stems)");
    syncClipsButton.onClick = [this] {
        commitPendingEdits();
        if (onSyncFromClips) {
            onSyncFromClips();
        }
    };

    addAndMakeVisible(previewTrackButton);
    previewTrackButton.setTooltip("Audition the selected MD track in playback order (Space)");
    previewTrackButton.onClick = [this] { triggerPreviewSelected(false); };

    addAndMakeVisible(previewFromButton);
    previewFromButton.setTooltip(
        "Audition seamlessly from the selected MD track through the end of the disc");
    previewFromButton.onClick = [this] { triggerPreviewSelected(true); };

    addAndMakeVisible(reviewWavButton);
    reviewWavButton.setTooltip(
        "Export MiniDisc playback order with track cues as 32-bit float WAV");
    reviewWavButton.onClick = [this] {
        commitPendingEdits();
        if (onReviewWav) {
            onReviewWav();
        }
    };

    addAndMakeVisible(planHintLabel);
    planHintLabel.setFont(juce::FontOptions(11.5f));
    planHintLabel.setJustificationType(juce::Justification::centredRight);

    // --- Tab 1 Left: Action Bar & Play Order List ---
    addAndMakeVisible(insertTrackButton);
    insertTrackButton.setTooltip(
        "Insert a new Play Order track for the current time selection or playhead-to-end range");
    insertTrackButton.onClick = [this] { triggerInsertTrack(); };

    addAndMakeVisible(insertAliasButton);
    insertAliasButton.setTooltip(
        "Insert a Play Order alias pointing to an existing Audio Pool item (0 extra disc space)");
    insertAliasButton.onClick = [this] { showInsertAliasMenu(); };

    addAndMakeVisible(duplicateTrackButton);
    duplicateTrackButton.setTooltip(
        "Insert a Play Order alias of the selected track (shares physical audio on disc)");
    duplicateTrackButton.onClick = [this] { triggerDuplicateTrack(); };

    addAndMakeVisible(splitTrackButton);
    splitTrackButton.setTooltip("Split the selected MD track at the current playhead sample");
    splitTrackButton.onClick = [this] { triggerSplitTrack(); };

    addAndMakeVisible(moveUpButton);
    moveUpButton.setTooltip("Move selected MD track earlier in disc order");
    moveUpButton.onClick = [this] { triggerMoveTrack(-1); };

    addAndMakeVisible(moveDownButton);
    moveDownButton.setTooltip("Move selected MD track later in disc order");
    moveDownButton.onClick = [this] { triggerMoveTrack(1); };

    addAndMakeVisible(removeTrackButton);
    removeTrackButton.setTooltip("Remove selected MD track(s) (Delete/Backspace)");
    removeTrackButton.onClick = [this] { triggerRemoveSelectedTracks(); };

    addAndMakeVisible(groupMenuButton);
    groupMenuButton.setTooltip(
        "Create, edit, or clear MiniDisc groups over contiguous selected tracks");
    groupMenuButton.onClick = [this] { showGroupMenu(); };

    addAndMakeVisible(trackList);
    trackList.setRowHeight(26);
    trackList.setMultipleSelectionEnabled(true);
    trackList.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    trackList.dragReorderEnabled = true;
    trackList.canStartDrag = [this](int) { return inlineEditingRow < 0; };
    trackList.onRowReorder = [this](int fromRow, int insertBeforeRow) {
        handleTrackReorderDrag(fromRow, insertBeforeRow);
    };

    addChildComponent(inlineTrackTitleEditor);
    inlineTrackTitleEditor.setReturnKeyStartsNewLine(false);
    inlineTrackTitleEditor.setEscapeAndReturnKeysConsumed(true);
    inlineTrackTitleEditor.setFont(juce::FontOptions(12.2f));
    inlineTrackTitleEditor.setIndents(4, 2);
    inlineTrackTitleEditor.setTooltip(
        "Edit track title in place (Up/Down moves to prev/next track, Enter commits, Escape "
        "cancels)");
    inlineTrackTitleEditor.onReturnKey = [this] {
        commitInlineTrackEdit(true);
        trackList.grabKeyboardFocus();
    };
    inlineTrackTitleEditor.onEscapeKey = [this] {
        cancelInlineTrackEdit();
        trackList.grabKeyboardFocus();
    };
    inlineTrackTitleEditor.onFocusLost = [this] {
        if (!inlineTransitioning && inlineEditingRow >= 0) {
            commitInlineTrackEdit(true);
        }
    };

    // --- Tab 1 Right: Empty State ---
    addChildComponent(emptyTitleLabel);
    emptyTitleLabel.setText("No MiniDisc tracks yet", juce::dontSendNotification);
    emptyTitleLabel.setFont(juce::FontOptions(13.5f, juce::Font::bold));
    addChildComponent(emptyBodyLabel);
    emptyBodyLabel.setText("Import audio with Auto from Clips enabled, click Tracks from Clips, or "
                           "double-click and drag in the MD Tracks timeline lane.",
                           juce::dontSendNotification);
    emptyBodyLabel.setFont(juce::FontOptions(11.5f));
    emptyBodyLabel.setJustificationType(juce::Justification::topLeft);
    addChildComponent(emptySyncClipsButton);
    emptySyncClipsButton.onClick = [this] {
        if (onSyncFromClips) {
            onSyncFromClips();
        }
    };
    addChildComponent(emptyImportButton);
    emptyImportButton.onClick = [this] {
        if (onImportAudio) {
            onImportAudio();
        }
    };

    // --- Tab 1 Right: Single-Track Inspector ---
    addChildComponent(trackTitleLabel);
    trackTitleLabel.setText("Title", juce::dontSendNotification);
    trackTitleLabel.setFont(juce::FontOptions(11.5f));

    addChildComponent(trackTitleEditor);
    trackTitleEditor.setTextToShowWhenEmpty("Track title (ASCII)", juce::Colours::grey);
    trackTitleEditor.setReturnKeyStartsNewLine(false);
    trackTitleEditor.setEscapeAndReturnKeysConsumed(true);
    trackTitleEditor.setTooltip("Rename track (Enter commits, Escape reverts, F2 focuses)");
    trackTitleEditor.onReturnKey = [this] {
        commitTrackTitle();
        trackList.grabKeyboardFocus();
    };
    trackTitleEditor.onFocusLost = [this] { commitTrackTitle(); };
    trackTitleEditor.onEscapeKey = [this] {
        trackTitleEditor.setText(trackBaselineTitle, juce::dontSendNotification);
        trackList.grabKeyboardFocus();
    };

    addChildComponent(trackFormsButton);
    trackFormsButton.setTooltip("Edit ASCII and Full-Width/Japanese track titles (Shift+F2)");
    trackFormsButton.onClick = [this] {
        commitPendingEdits();
        if (onEditTrackForms) {
            onEditTrackForms();
        }
    };

    addChildComponent(trackFlagsButton);
    trackFlagsButton.setTooltip("Edit Write-Protect, SCMS copy bit, Emphasis, and track timestamp");
    trackFlagsButton.onClick = [this] {
        commitPendingEdits();
        if (onEditTrackFlags) {
            onEditTrackFlags();
        }
    };

    addChildComponent(trackCodecLabel);
    trackCodecLabel.setText("Codec", juce::dontSendNotification);
    trackCodecLabel.setFont(juce::FontOptions(11.5f));

    addChildComponent(trackCodecBox);
    syncCodecDropdownsForMedium(false);
    trackCodecBox.setTooltip(
        "Override encoding mode for this track only, or inherit the disc default");
    trackCodecBox.onChange = [this] {
        if (syncingControls) {
            return;
        }
        commitPendingEdits();
        const int row = singleSelectedTrackIndex();
        if (row < 0) {
            return;
        }
        const int selId = trackCodecBox.getSelectedId();
        const juce::String modeStr =
            selId <= 1
                ? "clear"
                : juce::String(carta::mdCodecModeToString(carta::md_ui::menuIdToCodec(selId - 1)));
        if (onCommand) {
            onCommand({"disc", "track", "flags", juce::String(row), modeStr, "keep", "keep", "keep",
                       "keep"},
                      revision);
        }
    };

    addChildComponent(trackGroupButton);
    trackGroupButton.setTooltip(
        "Assign this track to a MiniDisc group or remove it from its group");
    trackGroupButton.onClick = [this] { showGroupMenu(); };

    addChildComponent(audioSourceHeaderLabel);
    audioSourceHeaderLabel.setText("Audio Pool Source", juce::dontSendNotification);
    audioSourceHeaderLabel.setFont(juce::FontOptions(11.5f, juce::Font::bold));
    audioSourceHeaderLabel.setBorderSize(juce::BorderSize<int>(0));

    addChildComponent(seekAudioButton);
    seekAudioButton.setTooltip("Seek playhead and select this audio source's range on Layer 1");
    seekAudioButton.onClick = [this] { triggerSeekTrackRange(); };

    addChildComponent(setAudioSelButton);
    setAudioSelButton.setTooltip(
        "Update this audio source's timeline range to match the current time selection");
    setAudioSelButton.onClick = [this] { triggerSetTrackToSelection(); };

    addChildComponent(detachAliasButton);
    detachAliasButton.setTooltip(
        "Detach this Play Order entry from shared aliases into its own independent Audio Pool "
        "item");
    detachAliasButton.onClick = [this] { triggerMakeUnique(); };

    // --- Tab 1 Right: Multi-Selection Inspector ---
    addChildComponent(multiHeaderLabel);
    multiHeaderLabel.setFont(juce::FontOptions(12.5f, juce::Font::bold));
    addChildComponent(multiGroupButton);
    multiGroupButton.onClick = [this] { promptCreateGroupForSelection(); };
    addChildComponent(multiUngroupButton);
    multiUngroupButton.onClick = [this] { clearGroupsForSelection(); };
    addChildComponent(multiDuplicateButton);
    multiDuplicateButton.setTooltip(
        "Insert Play Order aliases after each selected track (shares existing audio)");
    multiDuplicateButton.onClick = [this] { triggerDuplicateTrack(); };
    addChildComponent(multiRemoveButton);
    multiRemoveButton.onClick = [this] { triggerRemoveSelectedTracks(); };
    addChildComponent(multiMergeButton);
    multiMergeButton.setTooltip(
        "Merge contiguous selected MiniDisc tracks sequentially into one track (T-MARK erase)");
    multiMergeButton.onClick = [this] { triggerMergeSelectedTracks(); };
    addChildComponent(multiCopyButton);
    multiCopyButton.setTooltip("Copy selected MD tracks so Cmd/Ctrl+V pastes shared-audio aliases");
    multiCopyButton.onClick = [this] { triggerCopySelectedTracks(); };

    // --- Tab 2: Device & Record ---
    addChildComponent(inspectUtocButton);
    inspectUtocButton.setTooltip("View raw UTOC fragment chain, title cells, and timestamps");
    inspectUtocButton.onClick = [this] {
        if (onInspectUtoc) {
            onInspectUtoc();
        }
    };

    addChildComponent(importUtocButton);
    importUtocButton.setTooltip("Import UTOC Sector 0/1/2/4 bundle or raw binary");
    importUtocButton.onClick = [this] {
        if (onImportUtoc) {
            onImportUtoc();
        }
    };

    addChildComponent(exportUtocButton);
    exportUtocButton.setTooltip("Export compiled UTOC sectors");
    exportUtocButton.onClick = [this] {
        if (onExportUtoc) {
            onExportUtoc();
        }
    };

    addChildComponent(deviceCardTitle);
    deviceCardTitle.setText("Recorder", juce::dontSendNotification);
    deviceCardTitle.setFont(juce::FontOptions(11.5f, juce::Font::bold));
    deviceCardTitle.setBorderSize(juce::BorderSize<int>(0));

    addChildComponent(recordDeviceBox);
    recordDeviceBox.setTooltip("Select connected NetMD or Hi-MD USB recorder");
    recordDeviceBox.onChange = [this] {
        if (!syncingControls && onRecordPreflight) {
            onRecordPreflight();
        }
    };

    addChildComponent(refreshRecordButton);
    refreshRecordButton.setTooltip(
        "Scan USB for connected NetMD / Hi-MD devices and inspect inserted disc");
    refreshRecordButton.onClick = [this] {
        if (onRefreshRecord) {
            onRefreshRecord();
        }
    };

    addChildComponent(recordBadgeLabel);
    recordBadgeLabel.setFont(juce::FontOptions(10.5f, juce::Font::bold));
    recordBadgeLabel.setJustificationType(juce::Justification::centredLeft);
    recordBadgeLabel.setBorderSize(juce::BorderSize<int>(0));

    addChildComponent(deviceTrackList);
    deviceTrackList.setRowHeight(20);
    deviceTrackList.setOutlineThickness(0);
    deviceTrackList.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);

    addChildComponent(loadDiscButton);
    loadDiscButton.setTooltip(
        "Load existing MiniDisc UTOC into editor for non-destructive editing");
    loadDiscButton.onClick = [this] {
        if (onLoadDisc) {
            onLoadDisc();
        }
    };

    addChildComponent(readDiscTocButton);
    readDiscTocButton.setTooltip("Read physical UTOC Sectors 0/1/2/4 from the inserted MiniDisc");
    readDiscTocButton.onClick = [this] {
        if (onReadDiscToc) {
            onReadDiscToc();
        }
    };

    addChildComponent(eraseDiscButton);
    eraseDiscButton.setTooltip(
        "Erase all tracks (and switch Hi-MD formatted discs back to standard MD)");
    eraseDiscButton.onClick = [this] {
        if (onEraseDisc) {
            onEraseDisc();
        }
    };

    addChildComponent(planCardTitle);
    planCardTitle.setText("Recording Plan", juce::dontSendNotification);
    planCardTitle.setFont(juce::FontOptions(11.5f, juce::Font::bold));
    planCardTitle.setBorderSize(juce::BorderSize<int>(0));

    addChildComponent(planModePlaybackButton);
    planModePlaybackButton.setClickingTogglesState(false);
    planModePlaybackButton.setTooltip(
        "Show logical playback track order mapped onto physical cluster ranges (drag to reorder)");
    planModePlaybackButton.onClick = [this] { setPlanViewMode("playback"); };

    addChildComponent(planModeBankButton);
    planModeBankButton.setClickingTogglesState(false);
    planModeBankButton.setTooltip(
        "Show physical Audio Bank regions & spans mapped back to referencing tracks");
    planModeBankButton.onClick = [this] { setPlanViewMode("bank"); };

    addChildComponent(utocSummaryLabel);
    utocSummaryLabel.setFont(juce::FontOptions(10.4f));
    utocSummaryLabel.setJustificationType(juce::Justification::centredRight);
    utocSummaryLabel.setBorderSize(juce::BorderSize<int>(0));

    addChildComponent(planList);
    planList.setRowHeight(22);
    planList.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    planList.dragReorderEnabled = true;
    planList.onRowReorder = [this](int fromRow, int insertBeforeRow) {
        handlePlanReorderDrag(fromRow, insertBeforeRow);
    };

    addChildComponent(recordStatusLabel);
    recordStatusLabel.setFont(juce::FontOptions(10.8f));
    recordStatusLabel.setJustificationType(juce::Justification::centredLeft);
    recordStatusLabel.setBorderSize(juce::BorderSize<int>(0));

    addChildComponent(writeUtocButton);
    writeUtocButton.setTooltip(
        "Update UTOC Sectors 0/1/2/4 on hardware without erasing recorded audio");
    writeUtocButton.onClick = [this] {
        if (onRecordWriteUtoc) {
            onRecordWriteUtoc();
        }
    };

    addChildComponent(recordDiscButton);
    recordDiscButton.setTooltip(
        "Sync edited MiniDisc UTOC and upload any new audio spans without erasing preserved "
        "on-disc audio");
    recordDiscButton.onClick = [this] {
        if (onRecordStart) {
            onRecordStart();
        }
    };

    setActiveTab("tracks", false);
    updateButtonStates();
}

void MdWorkspace::syncCodecDropdownsForMedium(bool isHiMd) {
    const int desiredKind = isHiMd ? 1 : 0;
    if (lastCodecMenuKind == desiredKind) {
        return;
    }
    const bool prevSync = syncingControls;
    syncingControls = true;

    codecBox.clear(juce::dontSendNotification);
    if (isHiMd) {
        codecBox.addItem("Hi-SP", 5);
        codecBox.addItem("Hi-LP", 6);
        codecBox.addItem("LP2", 3);
        codecBox.addItem("LP4", 4);
        codecBox.addItem("Linear PCM", 7);
    } else {
        codecBox.addItem("SP Stereo", 1);
        codecBox.addItem("SP Mono", 2);
        codecBox.addItem("LP2", 3);
        codecBox.addItem("LP4", 4);
    }

    trackCodecBox.clear(juce::dontSendNotification);
    trackCodecBox.addItem("Default (Inherited)", 1);
    if (isHiMd) {
        trackCodecBox.addItem("Hi-SP", 6);
        trackCodecBox.addItem("Hi-LP", 7);
        trackCodecBox.addItem("LP2", 4);
        trackCodecBox.addItem("LP4", 5);
        trackCodecBox.addItem("Linear PCM", 8);
    } else {
        trackCodecBox.addItem("SP Stereo", 2);
        trackCodecBox.addItem("SP Mono", 3);
        trackCodecBox.addItem("LP2", 4);
        trackCodecBox.addItem("LP4", 5);
    }

    lastCodecMenuKind = desiredKind;
    syncingControls = prevSync;
}

juce::String MdWorkspace::getTooltip() {
    return lastHoverTooltip;
}

void MdWorkspace::syncAppearance() {
    updateTabVisibility();
    deviceTrackList.repaint();
    planList.repaint();
    repaint();
}

juce::String MdWorkspace::currentView() const noexcept {
    return activeTab;
}

bool MdWorkspace::selectView(const juce::String& view, juce::String& error) {
    const auto norm = view.trim().toLowerCase();
    if (norm == "tracks" || norm == "record" || norm == "map") {
        setActiveTab(norm, false);
        return true;
    }
    if (norm == "bank" || norm == "playback") {
        setPlanViewMode(norm);
        setActiveTab("record", false);
        return true;
    }
    error = "Disc workspace view must be tracks, record, bank, or playback.";
    return false;
}

void MdWorkspace::setActiveTab(const juce::String& tab, bool notify) {
    const auto norm = tab.trim().toLowerCase();
    const auto next = (norm == "record" || norm == "map" || norm == "bank" || norm == "playback")
                          ? juce::String("record")
                          : juce::String("tracks");
    const bool changed = next != activeTab;
    if (changed && next != "tracks" && inlineEditingRow >= 0) {
        commitInlineTrackEdit(true);
    }
    activeTab = next;
    updateTabVisibility();
    resized();
    repaint();
    if (changed && notify && onTabChanged) {
        onTabChanged(activeTab);
    }
}

juce::String MdWorkspace::currentPlanViewMode() const noexcept {
    return planViewMode;
}

void MdWorkspace::setPlanViewMode(const juce::String& mode) {
    const auto norm = mode.trim().toLowerCase();
    const auto next = (norm == "bank") ? juce::String("bank") : juce::String("playback");
    planViewMode = next;
    planList.dragReorderEnabled = (planViewMode == "playback");
    syncingControls = true;
    planList.updateContent();
    if (planViewMode == "playback") {
        const auto selected = selectedTrackIndices0();
        if (!selected.empty() && selected.front() < static_cast<int>(planRows.size())) {
            planList.selectRow(selected.front(), false, true);
        } else {
            planList.deselectAllRows();
        }
    } else {
        planList.deselectAllRows();
    }
    syncingControls = false;
    updateTabVisibility();
    planList.repaint();
    repaint();
}

bool MdWorkspace::hasPendingProgramEdits() const noexcept {
    return false;
}

void MdWorkspace::commitPendingEdits() {
    commitInlineTrackEdit(true);
    commitDiscTitle();
    commitTrackTitle();
}

void MdWorkspace::update(const carta::Session& session, const carta::TimeSelection& selection,
                         double playheadSec, juce::uint64 generation,
                         const juce::String& preferredTrackId) {
    const auto prevPrimaryId = preferredTrackId.isNotEmpty() ? preferredTrackId : primaryTrackId();

    discState = session.mdDisc;
    timeSelection = selection;
    playheadSample = static_cast<juce::int64>(
        std::llround(std::max(0.0, playheadSec) * static_cast<double>(carta::kMdSampleRate)));
    masterEndSample = carta::projectEndSample(session);
    revision = generation;

    const bool isHiMd = carta::isHiMdMedium(discState.medium);
    std::string planErr;
    planValid = carta::planMdPhysicalAudio(discState, physicalPlan, planErr);
    planErrorText = planErr;

    if (isHiMd) {
        utocValid = true;
        utocErrorText.clear();
        budget = {};
        hiMdTotalBytes =
            (recordUiState.activeDeviceIsHiMd && recordUiState.hasHiMdStatus &&
             recordUiState.hiMdStatus.fsTotalBytes > 0)
                ? recordUiState.hiMdStatus.fsTotalBytes
                : carta::hiMdMediumCapacityBytes(discState.medium);
        hiMdUsedBytes = physicalPlan.hiMdUniqueBytes +
                        (physicalPlan.uniqueAudioTrackCount > 0
                             ? static_cast<std::int64_t>(physicalPlan.uniqueAudioTrackCount) *
                                       32768LL +
                                   65536LL
                             : 0LL);
    } else {
        hiMdUsedBytes = 0;
        hiMdTotalBytes = 0;
        carta::UtocDiscState compiledUtoc;
        std::string utocErr;
        utocValid = carta::compileMdDiscToUtoc(discState, compiledUtoc, utocErr);
        utocErrorText = utocErr;
        const int physTotal =
            discState.loadedDisc.active ? discState.loadedDisc.physicalTotalClusters() : 0;
        budget = utocValid ? carta::computeUtocBudget(
                                 compiledUtoc, carta::mdMediumToString(discState.medium), physTotal)
                           : carta::UtocBudget{};
    }

    // Build per-placement reference counts so aliased/shared parts across both newly uploaded
    // spans and loaded on-disc regions are identified accurately without conflating coalesced
    // sequential neighbors.
    std::vector<int> placementRefCounts(physicalPlan.placements.size(), 1);
    std::map<std::pair<int, int>, std::size_t> placementIndexByTrackPart;
    std::vector<std::vector<std::size_t>> placementsByTrack(discState.tracks.size());
    for (std::size_t pi = 0; pi < physicalPlan.placements.size(); ++pi) {
        const auto& pl = physicalPlan.placements[pi];
        placementRefCounts[pi] = countPlacementAudioRefs(physicalPlan, pi);
        placementIndexByTrackPart.emplace(std::make_pair(pl.trackIndex, pl.partIndex), pi);
        if (pl.trackIndex >= 0 && pl.trackIndex < static_cast<int>(placementsByTrack.size())) {
            placementsByTrack[static_cast<std::size_t>(pl.trackIndex)].push_back(pi);
        }
    }

    // Build deduplicated audio-bank timeline entries from all session layer clips (+ any loaded
    // disc regions not represented by clips).
    audioBankClipEntries.clear();
    {
        std::set<std::pair<juce::int64, juce::int64>> seenClipRanges;
        for (const auto& layer : session.tracks) {
            const auto layerLabel = layer.name;
            for (const auto& clip : layer.clips) {
                juce::int64 s0 = 0;
                juce::int64 e0 = 0;
                if (!carta::clipSampleRange(clip, s0, e0) || e0 <= s0) {
                    continue;
                }
                if (!seenClipRanges.insert({s0, e0}).second) {
                    continue;
                }
                BankClipEntry entry;
                entry.label = carta::clipDisplayName(clip).replaceCharacters("\r\n", "  ").trim();
                entry.layerName = layerLabel;
                entry.startSample = s0;
                entry.endSample = e0;
                entry.codec = discState.defaultCodec;
                if (discState.loadedDisc.active) {
                    int rIdx = -1;
                    if (!clip.onDiscRegionId.empty()) {
                        for (std::size_t ri = 0; ri < discState.loadedDisc.regions.size(); ++ri) {
                            if (discState.loadedDisc.regions[ri].id == clip.onDiscRegionId) {
                                rIdx = static_cast<int>(ri);
                                break;
                            }
                        }
                    }
                    if (rIdx < 0) {
                        rIdx = carta::findMdOnDiscRegionForPart(discState.loadedDisc, {s0, e0});
                    }
                    if (rIdx >= 0 &&
                        rIdx < static_cast<int>(discState.loadedDisc.regions.size())) {
                        const auto& reg =
                            discState.loadedDisc.regions[static_cast<std::size_t>(rIdx)];
                        entry.isOnDisc = true;
                        entry.onDiscRegionIndex = rIdx;
                        entry.startCluster = reg.physStart.cluster;
                        entry.endCluster = reg.physEnd.cluster;
                        entry.codec = reg.codec;
                    }
                }
                if (entry.label.isEmpty()) {
                    entry.label = entry.isOnDisc
                                      ? ("Region #" + juce::String(entry.onDiscRegionIndex + 1))
                                      : carta::md_ui::formatClock(s0);
                }
                audioBankClipEntries.push_back(std::move(entry));
            }
        }
        std::sort(audioBankClipEntries.begin(), audioBankClipEntries.end(),
                  [](const BankClipEntry& a, const BankClipEntry& b) {
                      return std::tie(a.startSample, a.endSample) <
                             std::tie(b.startSample, b.endSample);
                  });
    }

    std::map<std::string, int> uniqueSlotByAudioKey;
    trackRows.clear();
    trackRows.reserve(discState.tracks.size());
    for (std::size_t i = 0; i < discState.tracks.size(); ++i) {
        const auto& t = discState.tracks[i];
        const std::string audioKey = carta::effectiveMdAudioId(t);
        if (!uniqueSlotByAudioKey.count(audioKey)) {
            uniqueSlotByAudioKey.emplace(audioKey, static_cast<int>(uniqueSlotByAudioKey.size()) + 1);
        }
        TrackRow row;
        row.id = juce::String::fromUTF8(t.id.c_str());
        row.audioId = juce::String::fromUTF8(audioKey.c_str());
        row.index0 = static_cast<int>(i);
        const auto titleStr = t.displayTitle();
        row.title = titleStr.empty() ? ("Track " + juce::String(static_cast<int>(i + 1)))
                                     : juce::String::fromUTF8(titleStr.c_str());
        row.fullWidthTitle = juce::String::fromUTF8(t.fullWidthTitle.c_str());
        row.artist = juce::String::fromUTF8(t.artist.c_str());
        row.album = juce::String::fromUTF8(t.album.c_str());
        row.effectiveCodec = carta::effectiveTrackCodec(discState, t);
        row.hasCodecOverride = t.codecOverride.has_value();
        row.writeProtected = t.writeProtected;
        row.scms = t.scmsCopyProtected;
        row.emphasis = t.emphasis;
        row.hasTimestamp = !t.timestamp.empty();
        row.partCount = static_cast<int>(t.parts.size());
        row.aliasRefCount = carta::countMdAudioRefs(discState, audioKey);
        row.totalSamples = t.totalSamples();
        if (!t.parts.empty()) {
            row.firstStartSample = t.parts.front().startSample;
            row.firstEndSample = t.parts.front().endSample;
        }
        const int oneBased = static_cast<int>(i) + 1;
        for (std::size_t g = 0; g < discState.groups.size(); ++g) {
            const auto& grp = discState.groups[g];
            if (oneBased >= grp.startTrack && oneBased <= grp.endTrack) {
                row.groupColorIndex = static_cast<int>(g);
                row.groupName =
                    !grp.halfWidthTitle.empty()
                        ? grp.halfWidthTitle
                        : (!grp.fullWidthTitle.empty() ? grp.fullWidthTitle
                                                       : ("Grp " + std::to_string(g + 1)));
                break;
            }
        }
        row.maxSharedRefs = row.aliasRefCount;
        row.onDisc = carta::mdTrackUsesOnDiscAudio(discState, t);
        row.forceGapBefore = t.forceGapBefore;
        for (const std::size_t pi : placementsByTrack[i]) {
            row.maxSharedRefs = std::max(row.maxSharedRefs, placementRefCounts[pi]);
        }
        row.sharesPhysicalSpan = row.maxSharedRefs > 1;
        trackRows.push_back(std::move(row));
    }

    planRows.clear();
    planRows.reserve(trackRows.size());
    for (std::size_t i = 0; i < trackRows.size(); ++i) {
        const auto& tr = trackRows[i];
        PlanRow pr;
        pr.trackIndex = tr.index0;
        pr.id = tr.id;
        pr.audioId = tr.audioId;
        pr.title = tr.title;
        pr.groupName = tr.groupName;
        pr.groupColorIndex = tr.groupColorIndex;
        pr.codec = tr.effectiveCodec;
        pr.partCount = tr.partCount;
        pr.aliasRefCount = tr.aliasRefCount;
        pr.uniqueAudioSlot = uniqueSlotByAudioKey[tr.audioId.toStdString()];
        pr.totalSamples = tr.totalSamples;
        pr.sharesPhysicalSpan = tr.sharesPhysicalSpan;
        pr.onDisc = tr.onDisc;
        pr.forceGapBefore = tr.forceGapBefore;
        if (i > 0) {
            const auto& prevTr = trackRows[i - 1];
            pr.gappedBefore = tr.forceGapBefore ||
                              (tr.effectiveCodec != prevTr.effectiveCodec) ||
                              (tr.onDisc != prevTr.onDisc);
        }
        int odMinClu = 0;
        int odMaxClu = 0;
        bool hasOd = false;
        std::vector<juce::String> hops;
        for (const std::size_t pi : placementsByTrack[i]) {
            const auto& place = physicalPlan.placements[pi];
            if (place.isOnDisc) {
                if (!hasOd) {
                    odMinClu = place.startAddr.cluster;
                    odMaxClu = place.endAddr.cluster;
                    hasOd = true;
                } else {
                    odMinClu = std::min(odMinClu, static_cast<int>(place.startAddr.cluster));
                    odMaxClu = std::max(odMaxClu, static_cast<int>(place.endAddr.cluster));
                }
                pr.spanRefCount = std::max(pr.spanRefCount, placementRefCounts[pi]);
                hops.push_back(
                    compactClusterLabel(place.startAddr.cluster, place.endAddr.cluster));
            } else if (place.spanIndex >= 0 &&
                       static_cast<std::size_t>(place.spanIndex) < physicalPlan.spans.size()) {
                const auto& sp = physicalPlan.spans[static_cast<std::size_t>(place.spanIndex)];
                if (pr.primarySpanIndex < 0) {
                    pr.primarySpanIndex = place.spanIndex;
                    pr.spanClusterStart = sp.startCluster;
                    pr.spanClusterEnd = sp.startCluster + std::max(0, sp.clusterCount - 1);
                }
                pr.spanRefCount = std::max(pr.spanRefCount, placementRefCounts[pi]);
                const int pStartC =
                    sp.startCluster + place.unitOffsetInSpan / carta::kMdUnitsPerCluster;
                const int pEndC =
                    sp.startCluster +
                    std::max(0, place.unitOffsetInSpan + place.unitCount - 1) /
                        carta::kMdUnitsPerCluster;
                hops.push_back(compactClusterLabel(pStartC, pEndC));
            }
        }
        if (hasOd && pr.primarySpanIndex < 0) {
            pr.spanClusterStart = odMinClu;
            pr.spanClusterEnd = odMaxClu;
        }
        if (hops.size() > 1) {
            const auto arrow = juce::String::fromUTF8(" \xe2\x86\x92 ");
            pr.partHopsText = juce::String(static_cast<int>(hops.size())) + "p: ";
            if (hops.size() <= 3) {
                pr.partHopsText += hops[0];
                for (std::size_t h = 1; h < hops.size(); ++h) {
                    pr.partHopsText += arrow + hops[h];
                }
            } else {
                pr.partHopsText += hops[0] + arrow + hops[1] + arrow +
                                   juce::String::fromUTF8("\xe2\x80\xa6") + arrow + hops.back();
            }
        }
        planRows.push_back(std::move(pr));
    }

    // Build inverse Audio Bank table: on-disc regions first, then 1 row per unique local Audio Pool
    // item identically across Standard MD (NetMD) and Hi-MD.
    audioBankRows.clear();
    if (discState.loadedDisc.active) {
        for (std::size_t ri = 0; ri < discState.loadedDisc.regions.size(); ++ri) {
            const auto& reg = discState.loadedDisc.regions[ri];
            AudioBankRow row;
            row.isOnDisc = true;
            row.onDiscRegionIndex = static_cast<int>(ri);
            row.codec = reg.codec;
            row.startCluster = reg.physStart.cluster;
            row.endCluster = reg.physEnd.cluster;
            row.clusterCount =
                isHiMd ? 0 : std::max(1, row.endCluster - row.startCluster + 1);
            row.unitCount = reg.unitCount;
            row.timelineStartSample = reg.timelineStartSample;
            row.timelineEndSample = reg.timelineEndSample;
            row.hiMdBytes =
                isHiMd ? carta::estimateHiMdTrackBytes(
                             reg.codec,
                             std::max<std::int64_t>(0, reg.timelineEndSample -
                                                           reg.timelineStartSample))
                       : 0;

            std::set<int> uniqTracks;
            if (isHiMd) {
                for (std::size_t ti = 0; ti < discState.tracks.size(); ++ti) {
                    for (const auto& part : discState.tracks[ti].parts) {
                        if (carta::findMdOnDiscRegionForPart(discState.loadedDisc, part) ==
                            static_cast<int>(ri)) {
                            ++row.totalPartRefs;
                            uniqTracks.insert(static_cast<int>(ti));
                        }
                    }
                }
            } else {
                for (const auto& place : physicalPlan.placements) {
                    if (place.isOnDisc && place.onDiscRegionIndex == static_cast<int>(ri)) {
                        ++row.totalPartRefs;
                        if (place.trackIndex >= 0 &&
                            place.trackIndex < static_cast<int>(trackRows.size())) {
                            uniqTracks.insert(place.trackIndex);
                        }
                    }
                }
            }
            row.referencingTrackIndices0.assign(uniqTracks.begin(), uniqTracks.end());
            row.isReclaimed = row.referencingTrackIndices0.empty();

            if (!row.referencingTrackIndices0.empty()) {
                const int firstTrIdx = row.referencingTrackIndices0.front();
                const auto& firstTr = trackRows[static_cast<std::size_t>(firstTrIdx)];
                row.audioId = firstTr.audioId;
                row.uniqueAudioSlot = uniqueSlotByAudioKey[row.audioId.toStdString()];
                row.forceGapBefore = firstTr.forceGapBefore;
                if (firstTrIdx > 0) {
                    const auto& prevTr = trackRows[static_cast<std::size_t>(firstTrIdx - 1)];
                    row.gappedBefore = firstTr.forceGapBefore ||
                                       (firstTr.effectiveCodec != prevTr.effectiveCodec) ||
                                       (firstTr.onDisc != prevTr.onDisc);
                }
            }

            for (const auto& clipEntry : audioBankClipEntries) {
                if (clipEntry.onDiscRegionIndex == static_cast<int>(ri) &&
                    clipEntry.label.isNotEmpty()) {
                    row.label = clipEntry.label;
                    break;
                }
            }
            if (row.label.isEmpty() && !row.referencingTrackIndices0.empty()) {
                row.label = trackRows[static_cast<std::size_t>(row.referencingTrackIndices0.front())]
                                .title;
            }
            if (row.label.isEmpty()) {
                row.label = "On-Disc Region #" + juce::String(static_cast<int>(ri + 1));
            }
            audioBankRows.push_back(std::move(row));
        }
    }

    std::set<std::string> seenLocalAudioKeys;
    for (std::size_t ti = 0; ti < discState.tracks.size(); ++ti) {
        const auto& rawTr = discState.tracks[ti];
        const auto& trMeta = trackRows[ti];
        const std::string key = trMeta.audioId.toStdString();

        bool hasLocalPart = !discState.loadedDisc.active || !trMeta.onDisc;
        std::int64_t localMinS = std::numeric_limits<std::int64_t>::max();
        std::int64_t localMaxE = 0;
        std::int64_t localTotalSamples = 0;
        int minCluster = 0;
        int maxCluster = 0;
        int localUnitSum = 0;
        int firstSpanIdx = -1;

        if (isHiMd) {
            for (const auto& part : rawTr.parts) {
                const bool pOnDisc =
                    discState.loadedDisc.active &&
                    carta::findMdOnDiscRegionForPart(discState.loadedDisc, part) >= 0;
                if (!pOnDisc) {
                    hasLocalPart = true;
                    localMinS = std::min(localMinS, part.startSample);
                    localMaxE = std::max(localMaxE, part.endSample);
                    localTotalSamples += part.durationSamples();
                }
            }
        } else {
            for (const std::size_t pi : placementsByTrack[ti]) {
                const auto& place = physicalPlan.placements[pi];
                if (!place.isOnDisc) {
                    hasLocalPart = true;
                    const int sc = place.startAddr.cluster;
                    const int ec = place.endAddr.cluster;
                    if (minCluster == 0 || sc < minCluster) {
                        minCluster = sc;
                    }
                    maxCluster = std::max(maxCluster, ec);
                    localUnitSum += place.unitCount;
                    if (firstSpanIdx < 0) {
                        firstSpanIdx = place.spanIndex;
                    }
                    if (place.partIndex >= 0 &&
                        place.partIndex < static_cast<int>(rawTr.parts.size())) {
                        const auto& pt = rawTr.parts[static_cast<std::size_t>(place.partIndex)];
                        localMinS = std::min(localMinS, pt.startSample);
                        localMaxE = std::max(localMaxE, pt.endSample);
                        localTotalSamples += pt.durationSamples();
                    }
                }
            }
        }

        if (!hasLocalPart || !seenLocalAudioKeys.insert(key).second) {
            continue;
        }
        if (localMinS == std::numeric_limits<std::int64_t>::max() && !rawTr.parts.empty()) {
            localMinS = rawTr.parts.front().startSample;
            localMaxE = rawTr.parts.back().endSample;
            localTotalSamples = rawTr.totalSamples();
        }

        AudioBankRow row;
        row.isOnDisc = false;
        row.spanIndex = firstSpanIdx;
        row.audioId = trMeta.audioId;
        row.uniqueAudioSlot = uniqueSlotByAudioKey[key];
        row.codec = trMeta.effectiveCodec;
        row.startCluster = minCluster;
        row.endCluster = maxCluster;
        row.unitCount = localUnitSum;
        row.clusterCount =
            isHiMd ? 0
                   : (localUnitSum > 0
                          ? std::max(1, (localUnitSum + carta::kMdUnitsPerCluster - 1) /
                                            carta::kMdUnitsPerCluster)
                          : 0);
        row.timelineStartSample = localMinS;
        row.timelineEndSample = localMaxE;
        row.hiMdBytes = isHiMd ? carta::estimateHiMdTrackBytes(row.codec, localTotalSamples) : 0;
        row.forceGapBefore = trMeta.forceGapBefore;
        if (ti > 0) {
            const auto& prevMeta = trackRows[ti - 1];
            row.gappedBefore = trMeta.forceGapBefore ||
                               (trMeta.effectiveCodec != prevMeta.effectiveCodec) ||
                               (trMeta.onDisc != prevMeta.onDisc);
        }

        for (std::size_t k = 0; k < trackRows.size(); ++k) {
            if (trackRows[k].audioId == trMeta.audioId) {
                row.referencingTrackIndices0.push_back(static_cast<int>(k));
                row.totalPartRefs += trackRows[k].partCount;
            }
        }

        std::int64_t bestOverlap = 0;
        for (const auto& clipEntry : audioBankClipEntries) {
            if (clipEntry.isOnDisc || clipEntry.label.isEmpty()) {
                continue;
            }
            if (clipEntry.startSample == row.timelineStartSample &&
                clipEntry.endSample == row.timelineEndSample) {
                row.label = clipEntry.label;
                break;
            }
            const auto ov = std::min<std::int64_t>(clipEntry.endSample, row.timelineEndSample) -
                            std::max<std::int64_t>(clipEntry.startSample, row.timelineStartSample);
            if (ov > bestOverlap) {
                bestOverlap = ov;
                row.label = clipEntry.label;
            }
        }
        if (row.label.isEmpty()) {
            row.label = trMeta.title;
        }
        if (row.label.isEmpty()) {
            row.label = "Audio Pool #" + juce::String(row.uniqueAudioSlot);
        }
        audioBankRows.push_back(std::move(row));
    }

    std::sort(audioBankRows.begin(), audioBankRows.end(),
              [](const AudioBankRow& a, const AudioBankRow& b) {
                  if (a.isOnDisc != b.isOnDisc) {
                      return a.isOnDisc;
                  }
                  if (a.startCluster != b.startCluster) {
                      return a.startCluster < b.startCluster;
                  }
                  return a.timelineStartSample < b.timelineStartSample;
              });

    syncingControls = true;
    if (lastCodecMenuKind != (isHiMd ? 1 : 0)) {
        syncCodecDropdownsForMedium(isHiMd);
    }
    discTitleEditor.setTextToShowWhenEmpty(isHiMd ? "Disc title" : "Disc title (ASCII)",
                                           juce::Colours::grey);
    discTitleEditor.setTooltip(
        isHiMd ? "Hi-MD disc title (Enter commits, Escape reverts)"
               : "Disc title written to UTOC Sector 1 (Enter commits, Escape reverts)");
    discBaselineTitle = juce::String::fromUTF8(discState.halfWidthTitle.c_str());
    if (!discTitleEditor.hasKeyboardFocus(true)) {
        discTitleEditor.setText(discBaselineTitle, juce::dontSendNotification);
    }
    mediumBox.setSelectedId(carta::md_ui::mediumToMenuId(discState.medium),
                            juce::dontSendNotification);
    codecBox.setSelectedId(carta::md_ui::codecToMenuId(discState.defaultCodec),
                           juce::dontSendNotification);
    autoTrackToggle.setToggleState(discState.autoTrackOnImport, juce::dontSendNotification);
    syncingControls = false;

    // Preserve selection by stable track ID when possible.
    int targetRow = -1;
    if (prevPrimaryId.isNotEmpty()) {
        for (int i = 0; i < static_cast<int>(trackRows.size()); ++i) {
            if (trackRows[static_cast<std::size_t>(i)].id == prevPrimaryId) {
                targetRow = i;
                break;
            }
        }
    }
    if (targetRow < 0 && !trackRows.empty()) {
        targetRow = 0;
    }

    syncingControls = true;
    trackList.updateContent();
    planList.updateContent();
    if (targetRow >= 0) {
        trackList.selectRow(targetRow, false, true);
        if (planViewMode == "playback") {
            planList.selectRow(targetRow, false, true);
        }
    } else {
        trackList.deselectAllRows();
        if (planViewMode == "playback") {
            planList.deselectAllRows();
        }
    }
    if (planViewMode == "bank" &&
        planList.getSelectedRow() >= static_cast<int>(audioBankRows.size())) {
        planList.deselectAllRows();
    }
    syncingControls = false;

    syncTrackInspectorFields();
    refreshPlanSummaryLabels();
    if (inlineEditingRow >= 0 && !inlineTransitioning) {
        if (inlineEditingRow >= static_cast<int>(trackRows.size()) || activeTab != "tracks") {
            cancelInlineTrackEdit();
        } else {
            updateInlineTrackEditorBounds();
        }
    }
    updateTabVisibility();
    updateButtonStates();
    planList.repaint();
    repaint();
}

void MdWorkspace::updateRecordSummary(const RecordUiSnapshot& snapshot) {
    recordUiState = snapshot;
    recordBusy = snapshot.busy || snapshot.progress.active;
    syncingControls = true;
    recordDeviceIds.clear();
    recordDeviceIsHiMd.clear();
    recordDeviceBox.clear(juce::dontSendNotification);
    int selectedMenuId = 0;
    int nextId = 1;
    for (const auto& d : snapshot.devices) {
        recordDeviceIds.push_back(d.id);
        recordDeviceIsHiMd.push_back(false);
        const auto devName = juce::String::fromUTF8(d.name.c_str());
        const auto devId = juce::String::fromUTF8(d.id.c_str());
        recordDeviceBox.addItem(devName + " [" + devId + "]", nextId);
        if (!snapshot.activeDeviceIsHiMd && !snapshot.activeDeviceId.empty() &&
            d.id == snapshot.activeDeviceId) {
            selectedMenuId = nextId;
        }
        ++nextId;
    }
    for (const auto& d : snapshot.hiMdDevices) {
        recordDeviceIds.push_back(d.id);
        recordDeviceIsHiMd.push_back(true);
        const auto devName = juce::String::fromUTF8(d.name.c_str());
        const auto devId = juce::String::fromUTF8(d.id.c_str());
        recordDeviceBox.addItem(devName + juce::String::fromUTF8(" [Hi-MD \xc2\xb7 ") + devId + "]",
                                nextId);
        if (snapshot.activeDeviceIsHiMd && !snapshot.activeDeviceId.empty() &&
            d.id == snapshot.activeDeviceId) {
            selectedMenuId = nextId;
        }
        ++nextId;
    }
    if (recordDeviceIds.empty()) {
        recordDeviceBox.addItem("No NetMD / Hi-MD device", 1);
        recordDeviceBox.setSelectedId(1, juce::dontSendNotification);
        recordDeviceBox.setEnabled(false);
    } else {
        recordDeviceBox.setEnabled(!recordBusy);
        recordDeviceBox.setSelectedId(selectedMenuId > 0 ? selectedMenuId : 1,
                                      juce::dontSendNotification);
    }
    syncingControls = false;

    if (snapshot.activeDeviceIsHiMd) {
        loadDiscButton.setButtonText("Load Disc");
        loadDiscButton.setTooltip("Load Hi-MD disc tracks and groups into editor");
        readDiscTocButton.setButtonText("Dump Track...");
        readDiscTocButton.setTooltip("Extract selected on-disc Hi-MD track as .oma or .wav");
        eraseDiscButton.setButtonText("Wipe Disc...");
        eraseDiscButton.setTooltip("Wipe tracks or reformat Hi-MD FAT16 filesystem");
        writeUtocButton.setButtonText("Disc Tools...");
        writeUtocButton.setTooltip("Rename Hi-MD disc/tracks, reorder, delete, or sync groups");
        recordDiscButton.setButtonText("Record Hi-MD...");
        recordDiscButton.setTooltip("Encode and upload authored tracks to Hi-MD disc");
    } else {
        loadDiscButton.setButtonText("Load Disc");
        loadDiscButton.setTooltip(
            "Load existing MiniDisc UTOC into editor for non-destructive editing");
        readDiscTocButton.setButtonText("Read Disc TOC...");
        readDiscTocButton.setTooltip(
            "Read physical UTOC Sectors 0/1/2/4 from the inserted MiniDisc");
        eraseDiscButton.setButtonText("Erase Disc...");
        eraseDiscButton.setTooltip(
            "Erase all tracks (and switch Hi-MD formatted discs back to standard MD)");
        writeUtocButton.setButtonText("Write UTOC Only...");
        writeUtocButton.setTooltip(
            "Update UTOC Sectors 0/1/2/4 on hardware without erasing recorded audio");
        recordDiscButton.setButtonText("Record MiniDisc...");
        recordDiscButton.setTooltip(
            "Sync edited MiniDisc UTOC and upload any new audio spans without erasing preserved "
            "on-disc audio");
    }

    utocSummaryLabel.setText(snapshot.utocSummaryText, juce::dontSendNotification);
    recordBadgeLabel.setText(snapshot.badgeText, juce::dontSendNotification);
    recordBadgeLabel.setTooltip(snapshot.badgeText);
    recordStatusLabel.setText(snapshot.statusText, juce::dontSendNotification);
    deviceTrackList.updateContent();
    updateTabVisibility();
    deviceTrackList.repaint();
    if (snapshot.activeDeviceIsHiMd && snapshot.hiMdNeedsElevation) {
        refreshRecordButton.setButtonText("Unlock...");
        refreshRecordButton.setTooltip(
            "Authorize administrator access once for this session to inspect and record Hi-MD");
    } else {
        refreshRecordButton.setButtonText("Refresh");
        refreshRecordButton.setTooltip(
            "Scan USB for connected NetMD / Hi-MD devices and inspect inserted disc");
    }
    refreshRecordButton.setEnabled(!recordBusy);
    loadDiscButton.setEnabled(!recordBusy && snapshot.canLoadDisc);
    readDiscTocButton.setEnabled(
        !recordBusy && (snapshot.activeDeviceIsHiMd ? snapshot.canDumpTrack : snapshot.canReadToc));
    eraseDiscButton.setEnabled(!recordBusy && snapshot.canErase);
    writeUtocButton.setEnabled(
        !recordBusy &&
        (snapshot.activeDeviceIsHiMd ? snapshot.canManageHiMd : snapshot.canWriteUtoc));
    recordDiscButton.setEnabled(!recordBusy && snapshot.canRecord);
    repaint();
}

juce::String MdWorkspace::selectedRecordDeviceId() const {
    const int idx = recordDeviceBox.getSelectedId() - 1;
    if (idx >= 0 && idx < static_cast<int>(recordDeviceIds.size())) {
        return juce::String::fromUTF8(recordDeviceIds[static_cast<std::size_t>(idx)].c_str());
    }
    return {};
}

bool MdWorkspace::selectedRecordDeviceIsHiMd() const noexcept {
    const int idx = recordDeviceBox.getSelectedId() - 1;
    if (idx >= 0 && idx < static_cast<int>(recordDeviceIsHiMd.size())) {
        return recordDeviceIsHiMd[static_cast<std::size_t>(idx)];
    }
    return false;
}

int MdWorkspace::selectedDeviceTrackIndex0() const noexcept {
    const int row = deviceTrackList.getSelectedRow();
    if (recordUiState.activeDeviceIsHiMd && recordUiState.hasHiMdStatus) {
        return (row >= 0 && row < static_cast<int>(recordUiState.hiMdStatus.tracks.size())) ? row
                                                                                           : -1;
    }
    return (row >= 0 && row < static_cast<int>(recordUiState.deviceStatus.tracks.size())) ? row
                                                                                          : -1;
}

void MdWorkspace::handleDeviceTrackClick(int row, const juce::MouseEvent& event) {
    if (!recordUiState.activeDeviceIsHiMd || !recordUiState.hasHiMdStatus || recordBusy) {
        return;
    }
    if (row < 0 || row >= static_cast<int>(recordUiState.hiMdStatus.tracks.size())) {
        return;
    }
    if (!event.mods.isPopupMenu()) {
        return;
    }
    deviceTrackList.selectRow(row, false, true);
    const int nTracks = static_cast<int>(recordUiState.hiMdStatus.tracks.size());
    juce::PopupMenu menu;
    menu.addItem(1, "Dump Track...");
    menu.addItem(2, "Edit Track Metadata...");
    menu.addSeparator();
    menu.addItem(3, "Move Track Up", row > 0);
    menu.addItem(4, "Move Track Down", row + 1 < nTracks);
    menu.addSeparator();
    menu.addItem(5, "Delete Track...");
    menu.showMenuAsync(
        juce::PopupMenu::Options(),
        [safe = juce::Component::SafePointer<MdWorkspace>(this), row](int choice) {
            if (safe == nullptr || choice <= 0 || safe->recordBusy) {
                return;
            }
            if (choice == 1 && safe->onDumpHiMdTrack) {
                safe->onDumpHiMdTrack(row);
            } else if (choice == 2 && safe->onRenameHiMdTrack) {
                safe->onRenameHiMdTrack(row);
            } else if (choice == 3 && safe->onMoveHiMdTrack) {
                safe->onMoveHiMdTrack(row, -1);
            } else if (choice == 4 && safe->onMoveHiMdTrack) {
                safe->onMoveHiMdTrack(row, 1);
            } else if (choice == 5 && safe->onDeleteHiMdTrack) {
                safe->onDeleteHiMdTrack(row);
            }
        });
}

std::vector<int> MdWorkspace::selectedTrackIndices0() const {
    std::vector<int> out;
    const auto selected = trackList.getSelectedRows();
    for (int i = 0; i < selected.size(); ++i) {
        const int row = selected[i];
        if (row >= 0 && row < static_cast<int>(trackRows.size())) {
            out.push_back(row);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

int MdWorkspace::singleSelectedTrackIndex() const {
    const auto selected = selectedTrackIndices0();
    return selected.size() == 1 ? selected.front() : -1;
}

juce::String MdWorkspace::primaryTrackId() const {
    const int row = trackList.getLastRowSelected();
    if (row >= 0 && row < static_cast<int>(trackRows.size())) {
        return trackRows[static_cast<std::size_t>(row)].id;
    }
    const auto all = selectedTrackIndices0();
    if (!all.empty()) {
        return trackRows[static_cast<std::size_t>(all.front())].id;
    }
    return {};
}

void MdWorkspace::selectTrackById(const juce::String& trackId, bool scrollToRow) {
    for (int i = 0; i < static_cast<int>(trackRows.size()); ++i) {
        if (trackRows[static_cast<std::size_t>(i)].id == trackId) {
            trackList.selectRow(i, false, true);
            if (scrollToRow) {
                trackList.scrollToEnsureRowIsOnscreen(i);
            }
            return;
        }
    }
}

void MdWorkspace::focusTrackRename() {
    if (currentView() != "tracks") {
        juce::String ignored;
        selectView("tracks", ignored);
    }
    const int row = singleSelectedTrackIndex();
    if (row >= 0) {
        beginInlineTrackEdit(row);
    }
}

// ============================================================================
// JUCE Component Lifecycle (paint, resized, mouse*)
// ============================================================================

void MdWorkspace::paint(juce::Graphics& g) {
    const bool dark = juce::Desktop::getInstance().isDarkModeActive();
    const auto panelBg = dark ? juce::Colour(0xff1f1f1f) : juce::Colour(0xffeeeeee);
    const auto headerBg = dark ? juce::Colour(0xff282828) : juce::Colour(0xfff7f7f7);
    const auto border = dark ? juce::Colour(0xff3c3c3c) : juce::Colour(0xffcfcfcf);
    const auto textPrimary = dark ? juce::Colour(0xffe0e0e0) : juce::Colour(0xff262626);
    const auto textMuted = dark ? juce::Colour(0xff9e9e9e) : juce::Colour(0xff666666);
    const auto tabAccent = dark ? juce::Colour(0xffd0d0d0) : juce::Colour(0xff444444);

    g.fillAll(panelBg);

    // Top resize divider bar
    g.setColour(border);
    g.fillRect(0, 0, getWidth(), 1);
    g.setColour(textMuted.withAlpha(0.35f));
    g.fillRoundedRectangle(static_cast<float>(getWidth() / 2 - 20), 2.0f, 40.0f, 3.0f, 1.5f);

    // Persistent header bar background
    g.setColour(headerBg);
    g.fillRect(0, 6, getWidth(), 34);
    g.setColour(border);
    g.fillRect(0, 40, getWidth(), 1);

    // Sub-header tab bar background
    g.setColour(dark ? juce::Colour(0xff232323) : juce::Colour(0xffe8e8e8));
    g.fillRect(0, 41, getWidth(), 30);
    g.setColour(border);
    g.fillRect(0, 71, getWidth(), 1);

    // Active tab indicator underline
    const auto activeTabBounds =
        (activeTab == "record") ? recordTabButton.getBounds() : tracksTabButton.getBounds();
    if (!activeTabBounds.isEmpty()) {
        g.setColour(tabAccent);
        g.fillRect(activeTabBounds.getX() + 4, 69, std::max(0, activeTabBounds.getWidth() - 8), 2);
    }

    // UTOC / Hi-MD capacity budget pill in header
    if (!budgetPillBounds.isEmpty()) {
        const bool isHiMd = carta::isHiMdMedium(discState.medium);
        const bool over =
            isHiMd ? (!planValid || (hiMdTotalBytes > 0 && hiMdUsedBytes > hiMdTotalBytes))
                   : (!utocValid || !budget.fits() || !planValid);
        const auto pillFill = over ? (dark ? juce::Colour(0xff4a2326) : juce::Colour(0xfffee2e2))
                                   : (dark ? juce::Colour(0xff303030) : juce::Colour(0xffeaeaea));
        const auto pillStroke = over ? (dark ? juce::Colour(0xffd9534f) : juce::Colour(0xffb91c1c))
                                     : (dark ? juce::Colour(0xff555555) : juce::Colour(0xffb8b8b8));
        g.setColour(pillFill);
        g.fillRoundedRectangle(budgetPillBounds.toFloat(), 4.0f);
        g.setColour(pillStroke);
        g.drawRoundedRectangle(budgetPillBounds.toFloat().reduced(0.5f), 4.0f, 1.0f);
        g.setColour(over ? pillStroke.brighter(dark ? 0.3f : 0.0f) : textPrimary);
        g.setFont(juce::FontOptions(11.3f, juce::Font::bold));
        g.drawText(budgetPillText, budgetPillBounds.reduced(8, 0), juce::Justification::centred,
                   true);
    }

    if (activeTab == "tracks") {
        carta::md_ui::paintPanelCard(g, leftCardBounds, dark);
        carta::md_ui::paintPanelCard(g, rightCardBounds, dark);

        if (!trackHeaderBounds.isEmpty()) {
            g.setColour(dark ? juce::Colour(0xff2e2e2e) : juce::Colour(0xffebebeb));
            g.fillRect(trackHeaderBounds);
            g.setColour(border);
            g.fillRect(trackHeaderBounds.getX(), trackHeaderBounds.getBottom() - 1,
                       trackHeaderBounds.getWidth(), 1);
            g.setColour(textMuted);
            g.setFont(juce::FontOptions(10.8f, juce::Font::bold));
            const int x = trackHeaderBounds.getX();
            const int y = trackHeaderBounds.getY();
            const int h = trackHeaderBounds.getHeight();
            const int w = trackHeaderBounds.getWidth();
            g.drawText("#", x + 8, y, 24, h, juce::Justification::centredLeft, false);
            g.drawText("Group", x + 36, y, 76, h, juce::Justification::centredLeft, false);
            const int rightCols = 166;
            g.drawText("Track Title & Attributes", x + 118, y, std::max(40, w - 118 - rightCols), h,
                       juce::Justification::centredLeft, true);
            g.drawText("Codec", x + w - 160, y, 52, h, juce::Justification::centred, false);
            g.drawText("Uses", x + w - 104, y, 36, h, juce::Justification::centred, false);
            g.drawText("Dur", x + w - 66, y, 56, h, juce::Justification::centredRight, false);
        }
        if (selectedTrackIndices0().size() == 1 && !audioSourceCardBounds.isEmpty()) {
            paintAudioSourceCard(g, dark);
        }
    } else {
        const bool devConnected =
            recordUiState.activeDeviceIsHiMd
                ? (recordUiState.hasHiMdStatus && !recordUiState.hiMdStatus.deviceId.empty())
                : (recordUiState.hasDeviceStatus && !recordUiState.deviceStatus.deviceId.empty());
        carta::md_ui::paintPanelCard(g, deviceCardBounds, dark, devConnected);
        carta::md_ui::paintPanelCard(g, planCardBounds, dark);
        paintPhysicalDiscMeter(g, deviceDiscMeterBounds, dark);
        const auto devListBounds = deviceTrackList.getBounds();
        if (!devListBounds.isEmpty()) {
            g.setColour(dark ? juce::Colour(0xff1c1c1c) : juce::Colour(0xfff4f4f4));
            g.fillRoundedRectangle(devListBounds.toFloat(), 4.0f);
            g.setColour(border);
            g.drawRoundedRectangle(devListBounds.toFloat().reduced(0.5f), 4.0f, 1.0f);
            if (!deviceTrackList.isVisible()) {
                juce::String emptyMsg = "Connect a NetMD / Hi-MD recorder and click Refresh.";
                if (devConnected) {
                    if (recordUiState.activeDeviceIsHiMd) {
                        emptyMsg = "Blank Hi-MD disc — 0 tracks on device";
                    } else {
                        const auto& st = recordUiState.deviceStatus;
                        if (!st.discPresent) {
                            emptyMsg = "No disc loaded in recorder";
                        } else if (st.hiMdFormatted) {
                            emptyMsg = "Hi-MD formatted disc — select Hi-MD device or erase";
                        } else if (st.trackCount > 0 && st.tracks.empty()) {
                            emptyMsg = juce::String(st.trackCount) +
                                       " tracks on disc — click Load Disc to view & edit";
                        } else {
                            emptyMsg = "Blank disc — 0 tracks on device";
                        }
                    }
                }
                g.setColour(textMuted);
                g.setFont(juce::FontOptions(10.8f));
                g.drawText(emptyMsg, devListBounds.reduced(8, 2), juce::Justification::centred,
                           true);
            }
        }
        paintTargetCapacityBar(g, targetBarBounds, dark);
        const bool planListEmpty =
            planViewMode == "bank" ? audioBankRows.empty() : planRows.empty();
        if (planListEmpty && !planList.getBounds().isEmpty()) {
            g.setColour(textMuted);
            g.setFont(juce::FontOptions(11.3f));
            g.drawText(
                planViewMode == "bank"
                    ? "Load a disc or import timeline audio to view physical Audio Bank regions."
                    : "Add MiniDisc tracks (Play Order -> Tracks from Clips) to plan recording.",
                planList.getBounds().reduced(8, 0), juce::Justification::centred, true);
        }
        if (recordBusy || recordUiState.progress.active) {
            paintPlanProgressBar(g, planProgressBounds, dark);
        }
    }
}

void MdWorkspace::resized() {
    const int w = getWidth();
    const int h = getHeight();
    if (w <= 0 || h <= 0) {
        return;
    }

    // Row 1: Persistent Header (y=8..38)
    int x = 10;
    const int r1y = 10;
    const int r1h = 26;
    mdLabel.setBounds(x, r1y, 62, r1h);
    x += 66;

    const int closeW = 28;
    const int rightReserve = closeW + 10;
    const int remainingForControls = std::max(200, w - x - rightReserve);
    const int pillW = juce::jlimit(165, 290, remainingForControls / 4);
    const int controlsAvail = std::max(120, remainingForControls - pillW - 10);

    const int titleW = juce::jlimit(90, 190, controlsAvail * 24 / 100);
    const int formsW = 92;
    const int medW = 96;
    const int codecW = 112;
    const int autoW = 122;
    closeButton.setBounds(w - 10 - closeW, r1y, closeW, r1h);
    budgetPillBounds = {w - 10 - closeW - 8 - pillW, r1y + 1, pillW, r1h - 2};

    discTitleEditor.setBounds(x, r1y, titleW, r1h);
    x += titleW + 6;
    discFormsButton.setBounds(x, r1y, formsW, r1h);
    x += formsW + 6;
    mediumBox.setBounds(x, r1y, medW, r1h);
    x += medW + 6;
    codecBox.setBounds(x, r1y, codecW, r1h);
    x += codecW + 6;
    autoTrackToggle.setBounds(x, r1y, std::min(autoW, std::max(0, budgetPillBounds.getX() - x - 6)),
                              r1h);

    // Row 2: Mode Tabs & Sync Strip (y=42..68)
    int tx = 10;
    const int r2y = 43;
    const int r2h = 24;
    tracksTabButton.setBounds(tx, r2y, 114, r2h);
    tx += 118;
    recordTabButton.setBounds(tx, r2y, 128, r2h);
    tx += 140;
    if (activeTab == "tracks") {
        syncClipsButton.setBounds(tx, r2y, 132, r2h);
        tx += 138;
        previewTrackButton.setBounds(tx, r2y, 72, r2h);
        tx += 76;
        previewFromButton.setBounds(tx, r2y, 82, r2h);
        tx += 86;
        reviewWavButton.setBounds(tx, r2y, 96, r2h);
        tx += 104;
    } else {
        inspectUtocButton.setBounds(tx, r2y, 110, r2h);
        tx += 116;
        importUtocButton.setBounds(tx, r2y, 104, r2h);
        tx += 110;
        exportUtocButton.setBounds(tx, r2y, 104, r2h);
        tx += 112;
    }
    planHintLabel.setBounds(tx, r2y, std::max(0, w - tx - 10), r2h);

    const auto body = juce::Rectangle<int>(8, 74, std::max(0, w - 16), std::max(0, h - 79));
    if (activeTab == "tracks") {
        deviceDiscMeterBounds = {};
        deviceTrackList.setBounds({});
        targetBarBounds = {};
        planProgressBounds = {};
        const int gap = 8;
        const int leftW =
            juce::jlimit(320, std::max(320, body.getWidth() - 280), (body.getWidth() * 58) / 100);
        leftCardBounds = {body.getX(), body.getY(), std::min(body.getWidth(), leftW),
                          body.getHeight()};
        rightCardBounds = {leftCardBounds.getRight() + gap, body.getY(),
                           std::max(0, body.getRight() - leftCardBounds.getRight() - gap),
                           body.getHeight()};

        // Left Card internals
        auto leftInner = leftCardBounds.reduced(6, 6);
        auto actionRow = leftInner.removeFromTop(24);
        int ax = actionRow.getX();
        const bool compactBar = actionRow.getWidth() < 540;
        const int btnGap = compactBar ? 3 : 4;
        const int insertW = compactBar ? 52 : 60;
        const int aliasPoolW = compactBar ? 62 : 70;
        const int dupW = compactBar ? 42 : 52;
        const int splitW = compactBar ? 40 : 48;
        const int upW = compactBar ? 30 : 36;
        const int dnW = compactBar ? 30 : 36;
        const int remW = compactBar ? 54 : 62;
        const int grpW = compactBar ? 74 : 78;
        insertTrackButton.setBounds(ax, actionRow.getY(), insertW, 24);
        ax += insertW + btnGap;
        insertAliasButton.setBounds(ax, actionRow.getY(), aliasPoolW, 24);
        ax += aliasPoolW + btnGap;
        duplicateTrackButton.setBounds(ax, actionRow.getY(), dupW, 24);
        ax += dupW + btnGap;
        splitTrackButton.setBounds(ax, actionRow.getY(), splitW, 24);
        ax += splitW + btnGap;
        moveUpButton.setBounds(ax, actionRow.getY(), upW, 24);
        ax += upW + btnGap;
        moveDownButton.setBounds(ax, actionRow.getY(), dnW, 24);
        ax += dnW + btnGap;
        removeTrackButton.setBounds(ax, actionRow.getY(), remW, 24);
        ax += remW + btnGap;
        groupMenuButton.setBounds(ax, actionRow.getY(),
                                  std::min(grpW, std::max(44, actionRow.getRight() - ax)), 24);

        leftInner.removeFromTop(4);
        trackHeaderBounds = leftInner.removeFromTop(20);
        trackList.setBounds(leftInner);

        // Right Card internals
        auto rightInner = rightCardBounds.reduced(8, 6);
        layoutRightInspector(rightInner);
        if (inlineEditingRow >= 0) {
            updateInlineTrackEditorBounds();
        }
    } else {
        const int gap = 8;
        const int leftW =
            juce::jlimit(280, std::max(280, body.getWidth() - 360), (body.getWidth() * 40) / 100);
        deviceCardBounds = {body.getX(), body.getY(), std::min(body.getWidth(), leftW),
                            body.getHeight()};
        planCardBounds = {deviceCardBounds.getRight() + gap, body.getY(),
                          std::max(0, body.getRight() - deviceCardBounds.getRight() - gap),
                          body.getHeight()};
        layoutDeviceCard(deviceCardBounds.reduced(8, 6));
        layoutPlanCard(planCardBounds.reduced(8, 6));
        updateTabVisibility();
    }
}

void MdWorkspace::mouseMove(const juce::MouseEvent& event) {
    setMouseCursor(event.y < 6 ? juce::MouseCursor::UpDownResizeCursor
                               : juce::MouseCursor::NormalCursor);
    updatePanelHoverTooltip(event.getPosition());
}

void MdWorkspace::mouseDown(const juce::MouseEvent& event) {
    if (event.y < 6 && event.mods.isLeftButtonDown()) {
        resizingPanel = true;
        resizeStartScreenY = event.getScreenY();
    }
}

void MdWorkspace::mouseDrag(const juce::MouseEvent& event) {
    if (resizingPanel && onResize) {
        onResize(event.getScreenY() - resizeStartScreenY);
    }
}

void MdWorkspace::mouseUp(const juce::MouseEvent&) {
    if (resizingPanel) {
        resizingPanel = false;
        if (onResizeEnd) {
            onResizeEnd();
        }
    }
}

// ============================================================================
// Layout & Visibility Helpers
// ============================================================================

void MdWorkspace::layoutRightInspector(juce::Rectangle<int> area) {
    if (area.getWidth() <= 0 || area.getHeight() <= 0) {
        return;
    }

    auto emptyArea = area;
    emptyTitleLabel.setBounds(emptyArea.removeFromTop(22));
    emptyArea.removeFromTop(4);
    emptyBodyLabel.setBounds(emptyArea.removeFromTop(38));
    emptyArea.removeFromTop(8);
    auto emptyBtns = emptyArea.removeFromTop(24);
    emptySyncClipsButton.setBounds(emptyBtns.removeFromLeft(136));
    emptyBtns.removeFromLeft(6);
    emptyImportButton.setBounds(emptyBtns.removeFromLeft(108));

    auto multiArea = area;
    multiHeaderLabel.setBounds(multiArea.removeFromTop(24));
    multiArea.removeFromTop(8);
    auto multiRow1 = multiArea.removeFromTop(24);
    multiGroupButton.setBounds(multiRow1.removeFromLeft(130));
    multiRow1.removeFromLeft(6);
    multiUngroupButton.setBounds(multiRow1.removeFromLeft(90));
    multiArea.removeFromTop(6);
    auto multiRow2 = multiArea.removeFromTop(24);
    multiDuplicateButton.setBounds(multiRow2.removeFromLeft(130));
    multiRow2.removeFromLeft(6);
    multiRemoveButton.setBounds(multiRow2.removeFromLeft(110));
    multiArea.removeFromTop(6);
    auto multiRow3 = multiArea.removeFromTop(24);
    multiMergeButton.setBounds(multiRow3.removeFromLeft(130));
    multiRow3.removeFromLeft(6);
    multiCopyButton.setBounds(multiRow3.removeFromLeft(110));

    auto singleArea = area;
    auto rowA = singleArea.removeFromTop(22);
    trackTitleLabel.setBounds(rowA.removeFromLeft(34));
    const int formsW = 64;
    const int flagsW = 62;
    trackFlagsButton.setBounds(rowA.removeFromRight(flagsW));
    rowA.removeFromRight(4);
    trackFormsButton.setBounds(rowA.removeFromRight(formsW));
    rowA.removeFromRight(4);
    trackTitleEditor.setBounds(rowA);

    singleArea.removeFromTop(4);
    auto rowB = singleArea.removeFromTop(22);
    trackCodecLabel.setBounds(rowB.removeFromLeft(42));
    const int codecW = std::min(140, std::max(96, rowB.getWidth() / 2));
    trackCodecBox.setBounds(rowB.removeFromLeft(codecW));
    rowB.removeFromLeft(6);
    trackGroupButton.setBounds(rowB);

    singleArea.removeFromTop(8);
    audioSourceCardBounds = singleArea;
    auto srcInner = audioSourceCardBounds.reduced(8, 7);
    audioSourceHeaderLabel.setBounds(srcInner.removeFromTop(20));
    auto srcBtnRow = srcInner.removeFromBottom(24);
    const bool narrowSrc = srcBtnRow.getWidth() < 260;
    const int seekW = narrowSrc ? 54 : 66;
    const int setW = narrowSrc ? 74 : 88;
    const int uniqW = narrowSrc ? 88 : 102;
    seekAudioButton.setBounds(srcBtnRow.removeFromLeft(seekW));
    srcBtnRow.removeFromLeft(4);
    setAudioSelButton.setBounds(srcBtnRow.removeFromLeft(setW));
    srcBtnRow.removeFromLeft(4);
    detachAliasButton.setBounds(srcBtnRow.removeFromLeft(
        std::min(uniqW, std::max(40, srcBtnRow.getWidth()))));
}

void MdWorkspace::layoutDeviceCard(juce::Rectangle<int> area) {
    if (area.getWidth() <= 0 || area.getHeight() <= 0) {
        deviceDiscMeterBounds = {};
        deviceTrackList.setBounds({});
        return;
    }
    auto topRow = area.removeFromTop(22);
    deviceCardTitle.setBounds(topRow.removeFromLeft(60));
    topRow.removeFromLeft(4);
    refreshRecordButton.setBounds(topRow.removeFromRight(68));
    topRow.removeFromRight(4);
    recordDeviceBox.setBounds(topRow);

    area.removeFromTop(4);
    recordBadgeLabel.setBounds(area.removeFromTop(16));
    area.removeFromTop(4);

    auto btnRow = area.removeFromBottom(22);
    area.removeFromBottom(4);
    const int btnW = std::max(0, (btnRow.getWidth() - 8) / 3);
    loadDiscButton.setBounds(btnRow.removeFromLeft(btnW));
    btnRow.removeFromLeft(4);
    readDiscTocButton.setBounds(btnRow.removeFromLeft(btnW));
    btnRow.removeFromLeft(4);
    eraseDiscButton.setBounds(btnRow);

    if (area.getHeight() >= 82) {
        deviceDiscMeterBounds = area.removeFromTop(54);
        area.removeFromTop(4);
        deviceTrackList.setBounds(area);
    } else {
        deviceDiscMeterBounds = area;
        deviceTrackList.setBounds({});
    }
}

void MdWorkspace::layoutPlanCard(juce::Rectangle<int> area) {
    if (area.getWidth() <= 0 || area.getHeight() <= 0) {
        targetBarBounds = {};
        planProgressBounds = {};
        return;
    }
    auto topRow = area.removeFromTop(20);
    planCardTitle.setBounds(topRow.removeFromLeft(90));
    topRow.removeFromLeft(4);
    planModePlaybackButton.setBounds(topRow.removeFromLeft(64));
    topRow.removeFromLeft(2);
    planModeBankButton.setBounds(topRow.removeFromLeft(76));
    topRow.removeFromLeft(6);
    utocSummaryLabel.setBounds(topRow);

    area.removeFromTop(4);
    targetBarBounds = area.removeFromTop(22);
    area.removeFromTop(4);

    auto btnRow = area.removeFromBottom(22);
    area.removeFromBottom(4);
    const int recW = std::min(190, std::max(140, (btnRow.getWidth() * 44) / 100));
    const int tocW = std::min(144, std::max(110, btnRow.getWidth() - recW - 6));
    recordDiscButton.setBounds(btnRow.removeFromRight(recW));
    btnRow.removeFromRight(6);
    writeUtocButton.setBounds(btnRow.removeFromRight(tocW));

    planProgressBounds = area.removeFromBottom(20);
    recordStatusLabel.setBounds(planProgressBounds);
    area.removeFromBottom(4);

    planList.setBounds(area);
}

void MdWorkspace::updateTabVisibility() {
    const bool isTracks = activeTab == "tracks";
    const bool isRecord = !isTracks;
    const bool isHiMd = carta::isHiMdMedium(discState.medium);
    const auto selected = selectedTrackIndices0();
    const bool noneSelected = isTracks && selected.empty();
    const bool oneSelected = isTracks && selected.size() == 1;
    const bool multiSelected = isTracks && selected.size() > 1;

    syncClipsButton.setVisible(isTracks);
    previewTrackButton.setVisible(isTracks);
    previewFromButton.setVisible(isTracks);
    reviewWavButton.setVisible(isTracks);
    inspectUtocButton.setVisible(isRecord && !isHiMd);
    importUtocButton.setVisible(isRecord && !isHiMd);
    exportUtocButton.setVisible(isRecord && !isHiMd);
    inspectUtocButton.setEnabled(!isHiMd);
    importUtocButton.setEnabled(!isHiMd);
    exportUtocButton.setEnabled(!isHiMd);

    insertTrackButton.setVisible(isTracks);
    insertAliasButton.setVisible(isTracks);
    duplicateTrackButton.setVisible(isTracks);
    splitTrackButton.setVisible(isTracks);
    moveUpButton.setVisible(isTracks);
    moveDownButton.setVisible(isTracks);
    removeTrackButton.setVisible(isTracks);
    groupMenuButton.setVisible(isTracks);
    trackList.setVisible(isTracks);

    emptyTitleLabel.setVisible(noneSelected);
    emptyBodyLabel.setVisible(noneSelected);
    emptySyncClipsButton.setVisible(noneSelected);
    emptyImportButton.setVisible(noneSelected);

    trackTitleLabel.setVisible(oneSelected);
    trackTitleEditor.setVisible(oneSelected);
    trackFormsButton.setVisible(oneSelected);
    trackFlagsButton.setVisible(oneSelected);
    trackCodecLabel.setVisible(oneSelected);
    trackCodecBox.setVisible(oneSelected);
    trackGroupButton.setVisible(oneSelected);
    audioSourceHeaderLabel.setVisible(oneSelected);
    seekAudioButton.setVisible(oneSelected);
    setAudioSelButton.setVisible(oneSelected);
    detachAliasButton.setVisible(oneSelected);

    multiHeaderLabel.setVisible(multiSelected);
    multiGroupButton.setVisible(multiSelected);
    multiUngroupButton.setVisible(multiSelected);
    multiDuplicateButton.setVisible(multiSelected);
    multiRemoveButton.setVisible(multiSelected);
    multiMergeButton.setVisible(multiSelected);
    multiCopyButton.setVisible(multiSelected);

    deviceCardTitle.setVisible(isRecord);
    recordDeviceBox.setVisible(isRecord);
    refreshRecordButton.setVisible(isRecord);
    recordBadgeLabel.setVisible(isRecord);
    const bool showDeviceTracks =
        isRecord && !deviceTrackList.getBounds().isEmpty() &&
        ((recordUiState.activeDeviceIsHiMd && recordUiState.hasHiMdStatus &&
          !recordUiState.hiMdStatus.tracks.empty()) ||
         (!recordUiState.activeDeviceIsHiMd && recordUiState.hasDeviceStatus &&
          recordUiState.deviceStatus.discPresent && !recordUiState.deviceStatus.hiMdFormatted &&
          !recordUiState.deviceStatus.tracks.empty()));
    deviceTrackList.setVisible(showDeviceTracks);
    loadDiscButton.setVisible(isRecord);
    readDiscTocButton.setVisible(isRecord);
    eraseDiscButton.setVisible(isRecord);
    planCardTitle.setVisible(isRecord);
    planModePlaybackButton.setVisible(isRecord);
    planModeBankButton.setVisible(isRecord);
    utocSummaryLabel.setVisible(isRecord);
    planList.setVisible(isRecord);
    recordStatusLabel.setVisible(isRecord && !recordBusy);
    writeUtocButton.setVisible(isRecord);
    recordDiscButton.setVisible(isRecord);
    if (!isTracks && inlineEditingRow >= 0) {
        cancelInlineTrackEdit();
    }

    const auto baseBtnCol = getLookAndFeel().findColour(juce::TextButton::buttonColourId);
    tracksTabButton.setColour(juce::TextButton::buttonColourId,
                              baseBtnCol.withAlpha(isTracks ? 1.0f : 0.65f));
    recordTabButton.setColour(juce::TextButton::buttonColourId,
                              baseBtnCol.withAlpha(isRecord ? 1.0f : 0.65f));
    planModePlaybackButton.setColour(juce::TextButton::buttonColourId,
                                     baseBtnCol.withAlpha(planViewMode == "playback" ? 1.0f : 0.62f));
    planModeBankButton.setColour(juce::TextButton::buttonColourId,
                                 baseBtnCol.withAlpha(planViewMode == "bank" ? 1.0f : 0.62f));
}

void MdWorkspace::refreshPlanSummaryLabels() {
    if (carta::isHiMdMedium(discState.medium)) {
        const std::int64_t capBytes =
            hiMdTotalBytes > 0 ? hiMdTotalBytes : carta::hiMdMediumCapacityBytes(discState.medium);
        const int pct =
            capBytes > 0
                ? static_cast<int>(std::llround((static_cast<double>(hiMdUsedBytes) * 100.0) /
                                                static_cast<double>(capBytes)))
                : 0;
        const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
        const juce::String uniqPart =
            physicalPlan.aliasedTrackCount > 0
                ? (" (" + juce::String(physicalPlan.uniqueAudioTrackCount) + " uniq)")
                : juce::String();
        budgetPillText = juce::String(static_cast<int>(discState.tracks.size())) + "/2047 trk" +
                         uniqPart + dot + juce::String(static_cast<int>(discState.groups.size())) +
                         " grp" + dot + formatMegabytes(hiMdUsedBytes) + " / " +
                         formatMegabytes(capBytes) + " (" + juce::String(pct) + "%)";

        if (!planValid) {
            planHintLabel.setText(juce::String::fromUTF8(planErrorText.c_str()),
                                  juce::dontSendNotification);
        } else if (physicalPlan.hiMdSavedBytes > 0) {
            planHintLabel.setText("Hi-MD Play Order" + dot +
                                      juce::String(physicalPlan.uniqueAudioTrackCount) +
                                      " unique audio items" + dot + "saved " +
                                      formatMegabytes(physicalPlan.hiMdSavedBytes) + " by aliases",
                                  juce::dontSendNotification);
        } else if (!trackRows.empty()) {
            planHintLabel.setText("Hi-MD Play Order" + dot +
                                      juce::String(static_cast<int>(trackRows.size())) +
                                      " track(s)" + dot + formatMegabytes(hiMdUsedBytes) + " / " +
                                      formatMegabytes(capBytes),
                                  juce::dontSendNotification);
        } else {
            planHintLabel.setText(
                "Hi-MD Play Order: alias Audio Pool tracks or switch to Device & Record for "
                "Hi-SP / Hi-LP / LP2 / LP4 / PCM",
                juce::dontSendNotification);
        }
        return;
    }

    const auto totalClusters =
        std::max(1, discState.loadedDisc.active ? discState.loadedDisc.physicalTotalClusters()
                                                : carta::mdTotalClusters(discState.medium));
    const int pct =
        static_cast<int>(std::llround((static_cast<double>(physicalPlan.usedClusters) * 100.0) /
                                      static_cast<double>(totalClusters)));
    budgetPillText = juce::String(static_cast<int>(discState.tracks.size())) +
                     juce::String::fromUTF8("/255 trk \xc2\xb7 ") +
                     juce::String(static_cast<int>(discState.groups.size())) +
                     juce::String::fromUTF8(" grp \xc2\xb7 ") +
                     carta::md_ui::formatClockWhole(physicalPlan.logicalPlaySamples) + " (" +
                     juce::String(pct) + "%)";

    if (!utocValid) {
        planHintLabel.setText(juce::String::fromUTF8(utocErrorText.c_str()),
                              juce::dontSendNotification);
    } else if (!planValid) {
        planHintLabel.setText(juce::String::fromUTF8(planErrorText.c_str()),
                              juce::dontSendNotification);
    } else if (physicalPlan.savedClusters > 0) {
        planHintLabel.setText(juce::String(physicalPlan.uniqueAudioTrackCount) + " unique / " +
                                  juce::String(physicalPlan.playOrderTrackCount) +
                                  juce::String::fromUTF8(" Play Order \xc2\xb7 ") +
                                  juce::String(physicalPlan.savedClusters) +
                                  " clusters saved by aliases",
                              juce::dontSendNotification);
    } else if (!trackRows.empty()) {
        planHintLabel.setText(juce::String(static_cast<int>(physicalPlan.spans.size())) +
                                  juce::String::fromUTF8(" physical spans \xc2\xb7 ") +
                                  juce::String(physicalPlan.usedClusters) + "/" +
                                  juce::String(totalClusters) + " clusters",
                              juce::dontSendNotification);
    } else {
        planHintLabel.setText(
            "Drag splits or edges in the MD Tracks lane; Play Order aliases share physical audio",
            juce::dontSendNotification);
    }
}

void MdWorkspace::syncTrackInspectorFields() {
    const bool isHiMd = carta::isHiMdMedium(discState.medium);
    const auto selected = selectedTrackIndices0();
    if (selected.size() == 1) {
        const int row = selected.front();
        const auto& track = discState.tracks[static_cast<std::size_t>(row)];

        syncingControls = true;
        const auto titleStr = track.halfWidthTitle;
        trackBaselineTitle = juce::String::fromUTF8(titleStr.c_str());
        trackTitleEditor.setTextToShowWhenEmpty(isHiMd ? "Track title" : "Track title (ASCII)",
                                                juce::Colours::grey);
        if (!trackTitleEditor.hasKeyboardFocus(true)) {
            trackTitleEditor.setText(trackBaselineTitle, juce::dontSendNotification);
        }
        trackFormsButton.setButtonText(isHiMd ? "Meta..." : "Forms...");
        trackFormsButton.setTooltip(
            isHiMd ? "Edit Hi-MD title, artist, and album metadata (Shift+F2)"
                   : "Edit ASCII and Full-Width/Japanese track titles (Shift+F2)");
        trackCodecBox.setSelectedId(
            track.codecOverride ? (carta::md_ui::codecToMenuId(*track.codecOverride) + 1) : 1,
            juce::dontSendNotification);
        const bool onDiscTrack = carta::mdTrackUsesOnDiscAudio(discState, track);
        trackCodecBox.setEnabled(!onDiscTrack);
        trackCodecBox.setTooltip(
            onDiscTrack
                ? "Encoding mode is fixed for existing on-disc audio (re-encoding not supported "
                  "in non-destructive mode)"
                : "Override encoding mode for this audio item, or inherit the disc default");
        const auto& rowMeta = trackRows[static_cast<std::size_t>(row)];
        trackGroupButton.setButtonText(rowMeta.groupName.empty()
                                           ? "Group: None..."
                                           : "Grp: " +
                                                 juce::String::fromUTF8(rowMeta.groupName.c_str()));
        const juce::String poolLabel =
            rowMeta.aliasRefCount > 1
                ? ("Audio Pool Source  [" + juce::String(rowMeta.aliasRefCount) + " Uses]")
                : juce::String("Audio Pool Source  (Unique)");
        audioSourceHeaderLabel.setText(poolLabel, juce::dontSendNotification);
        syncingControls = false;
    } else if (selected.size() > 1) {
        multiHeaderLabel.setText(
            juce::String(static_cast<int>(selected.size())) + " tracks selected (Tracks " +
                juce::String(selected.front() + 1) + juce::String::fromUTF8("\xe2\x80\x93") +
                juce::String(selected.back() + 1) + ")",
            juce::dontSendNotification);
    }
}

void MdWorkspace::updateButtonStates() {
    const auto selected = selectedTrackIndices0();
    const int row = selected.size() == 1 ? selected.front() : -1;
    const bool hasSel = timeSelection.hasRange();
    const bool hasAudio = masterEndSample > 0;
    const bool contiguous = !selected.empty() && (selected.back() - selected.front() + 1 ==
                                                  static_cast<int>(selected.size()));

    insertTrackButton.setEnabled(hasSel || hasAudio);
    insertAliasButton.setEnabled(!trackRows.empty());
    duplicateTrackButton.setEnabled(!selected.empty());
    bool canSplit = false;
    if (row >= 0 && trackRows[static_cast<std::size_t>(row)].aliasRefCount <= 1) {
        const auto& tr = discState.tracks[static_cast<std::size_t>(row)];
        for (const auto& p : tr.parts) {
            if (playheadSample > p.startSample && playheadSample < p.endSample) {
                canSplit = true;
                break;
            }
        }
    }
    splitTrackButton.setEnabled(canSplit);
    moveUpButton.setEnabled(contiguous && selected.front() > 0);
    moveDownButton.setEnabled(contiguous &&
                              selected.back() + 1 < static_cast<int>(trackRows.size()));
    removeTrackButton.setEnabled(!selected.empty());
    groupMenuButton.setEnabled(!trackRows.empty());

    previewTrackButton.setEnabled(row >= 0);
    previewFromButton.setEnabled(row >= 0);
    reviewWavButton.setEnabled(!trackRows.empty());

    const bool onDiscSingle = row >= 0 && trackRows[static_cast<std::size_t>(row)].onDisc;
    seekAudioButton.setEnabled(row >= 0);
    setAudioSelButton.setEnabled(row >= 0 && hasSel && !onDiscSingle);
    detachAliasButton.setEnabled(
        row >= 0 && trackRows[static_cast<std::size_t>(row)].aliasRefCount > 1);

    multiGroupButton.setEnabled(contiguous);
    multiUngroupButton.setEnabled(!selected.empty());
    multiDuplicateButton.setEnabled(!selected.empty());
    multiRemoveButton.setEnabled(!selected.empty());
    multiMergeButton.setEnabled(contiguous && selected.size() >= 2);
    multiCopyButton.setEnabled(!selected.empty());
}

// ============================================================================
// Selection, Commit, & Command Dispatch Triggers
// ============================================================================

void MdWorkspace::handleTrackSelectionChanged(int lastRowSelected) {
    if (syncingControls) {
        return;
    }
    syncingControls = true;
    if (planViewMode == "playback") {
        if (lastRowSelected >= 0 && lastRowSelected < static_cast<int>(planRows.size())) {
            planList.selectRow(lastRowSelected, false, true);
        } else {
            planList.deselectAllRows();
        }
    }
    syncingControls = false;
    syncTrackInspectorFields();
    updateTabVisibility();
    updateButtonStates();
    resized();
    planList.repaint();
    repaint();
    if (onTrackSelectionChanged && lastRowSelected >= 0 &&
        lastRowSelected < static_cast<int>(trackRows.size())) {
        onTrackSelectionChanged(trackRows[static_cast<std::size_t>(lastRowSelected)].id);
    }
}

void MdWorkspace::handlePlanSelectionChanged(int lastRowSelected) {
    if (syncingControls) {
        return;
    }
    if (planViewMode == "bank") {
        if (lastRowSelected < 0 || lastRowSelected >= static_cast<int>(audioBankRows.size())) {
            return;
        }
        const auto& br = audioBankRows[static_cast<std::size_t>(lastRowSelected)];
        if (onSeekRange && br.timelineEndSample > br.timelineStartSample) {
            onSeekRange(br.timelineStartSample, br.timelineEndSample, revision);
        }
        repaint();
        return;
    }
    if (lastRowSelected < 0 || lastRowSelected >= static_cast<int>(trackRows.size())) {
        return;
    }
    syncingControls = true;
    trackList.selectRow(lastRowSelected, false, true);
    trackList.scrollToEnsureRowIsOnscreen(lastRowSelected);
    syncingControls = false;
    syncTrackInspectorFields();
    updateButtonStates();
    repaint();
    if (onTrackSelectionChanged) {
        onTrackSelectionChanged(trackRows[static_cast<std::size_t>(lastRowSelected)].id);
    }
}

void MdWorkspace::handlePlanClick(int row, const juce::MouseEvent& event) {
    if (event.mods.isPopupMenu()) {
        const int numRows = planViewMode == "bank" ? static_cast<int>(audioBankRows.size())
                                                   : static_cast<int>(planRows.size());
        if (row < 0 || row >= numRows) {
            return;
        }
        if (!planList.isRowSelected(row)) {
            planList.selectRow(row, false, true);
        }
        if (planViewMode == "bank") {
            showAudioBankContextMenu(row);
        } else {
            showPlanContextMenu(row);
        }
    }
}

void MdWorkspace::triggerOpenPlanRow(int row) {
    if (planViewMode == "bank") {
        if (row < 0 || row >= static_cast<int>(audioBankRows.size())) {
            return;
        }
        const auto& br = audioBankRows[static_cast<std::size_t>(row)];
        if (onSeekRange && br.timelineEndSample > br.timelineStartSample) {
            onSeekRange(br.timelineStartSample, br.timelineEndSample, revision);
        }
        if (!br.referencingTrackIndices0.empty()) {
            const int trIdx = br.referencingTrackIndices0.front();
            if (trIdx >= 0 && trIdx < static_cast<int>(trackRows.size())) {
                trackList.selectRow(trIdx, false, true);
            }
        }
        return;
    }
    if (row < 0 || row >= static_cast<int>(trackRows.size())) {
        return;
    }
    trackList.selectRow(row, false, true);
    if (onSeekPart && !discState.tracks[static_cast<std::size_t>(row)].parts.empty()) {
        onSeekPart(trackRows[static_cast<std::size_t>(row)].id, 0, revision);
    }
    setActiveTab("tracks", true);
}

void MdWorkspace::handleTrackClick(int row, const juce::MouseEvent& event) {
    if (row < 0 || row >= static_cast<int>(trackRows.size())) {
        return;
    }
    if (event.mods.isPopupMenu()) {
        if (!trackList.isRowSelected(row)) {
            trackList.selectRow(row, false, true);
        }
        showTrackContextMenu(row);
    }
}

void MdWorkspace::handleTrackDoubleClick(int row) {
    if (row < 0 || row >= static_cast<int>(trackRows.size())) {
        return;
    }
    beginInlineTrackEdit(row);
}

void MdWorkspace::beginInlineTrackEdit(int row) {
    if (activeTab != "tracks" || row < 0 || row >= static_cast<int>(trackRows.size()) ||
        row >= static_cast<int>(discState.tracks.size())) {
        return;
    }
    if (inlineEditingRow >= 0 && inlineEditingRow != row) {
        inlineTransitioning = true;
        commitInlineTrackEdit(false);
        inlineTransitioning = false;
        if (row >= static_cast<int>(trackRows.size()) ||
            row >= static_cast<int>(discState.tracks.size())) {
            cancelInlineTrackEdit();
            return;
        }
    }

    trackList.selectRow(row, false, true);
    trackList.scrollToEnsureRowIsOnscreen(row);
    inlineEditingRow = row;
    inlineBaselineTitle = juce::String::fromUTF8(
        discState.tracks[static_cast<std::size_t>(row)].halfWidthTitle.c_str());
    inlineTrackTitleEditor.setText(inlineBaselineTitle, juce::dontSendNotification);
    updateInlineTrackEditorBounds();
    inlineTrackTitleEditor.setVisible(true);
    inlineTrackTitleEditor.toFront(true);
    inlineTrackTitleEditor.grabKeyboardFocus();
    inlineTrackTitleEditor.selectAll();
    trackList.repaintRow(row);
}

void MdWorkspace::commitInlineTrackEdit(bool closeEditor) {
    if (inlineEditingRow < 0) {
        return;
    }
    const int row = inlineEditingRow;
    const auto next = inlineTrackTitleEditor.getText();
    const auto base = inlineBaselineTitle;
    if (closeEditor) {
        inlineEditingRow = -1;
        inlineTrackTitleEditor.setVisible(false);
        trackList.repaintRow(row);
    }
    if (row < 0 || row >= static_cast<int>(trackRows.size())) {
        return;
    }
    if (next != base && onCommand) {
        if (onCommand({"disc", "track", "rename", juce::String(row), next}, revision)) {
            if (!closeEditor) {
                inlineBaselineTitle = next;
            }
        }
    }
}

void MdWorkspace::cancelInlineTrackEdit() {
    if (inlineEditingRow < 0) {
        inlineTrackTitleEditor.setVisible(false);
        return;
    }
    const int prevRow = inlineEditingRow;
    inlineEditingRow = -1;
    inlineTrackTitleEditor.setVisible(false);
    trackList.repaintRow(prevRow);
}

void MdWorkspace::navigateInlineTrackEdit(int rowDelta) {
    if (inlineEditingRow < 0 || trackRows.empty()) {
        return;
    }
    const int currentRow = inlineEditingRow;
    const int maxRow = static_cast<int>(trackRows.size()) - 1;
    const int targetRow = std::clamp(currentRow + rowDelta, 0, maxRow);
    inlineTransitioning = true;
    commitInlineTrackEdit(false);
    inlineTransitioning = false;
    beginInlineTrackEdit(targetRow);
}

void MdWorkspace::updateInlineTrackEditorBounds() {
    if (inlineEditingRow < 0 || inlineEditingRow >= static_cast<int>(trackRows.size())) {
        inlineTrackTitleEditor.setBounds({});
        return;
    }
    const auto rowInList = trackList.getRowPosition(inlineEditingRow, true);
    const int rowW =
        rowInList.getWidth() > 0 ? rowInList.getWidth() : trackList.getVisibleContentWidth();
    const int editorX = trackList.getX() + 112;
    const int editorW = std::max(60, rowW - 112 - 166);
    const int editorH = std::max(18, rowInList.getHeight() - 4);
    inlineTrackTitleEditor.setBounds(editorX, trackList.getY() + rowInList.getY() + 2, editorW,
                                     editorH);
}

void MdWorkspace::commitDiscTitle() {
    if (syncingControls) {
        return;
    }
    const auto next = discTitleEditor.getText();
    if (next == discBaselineTitle) {
        return;
    }
    if (onCommand && onCommand({"disc", "title", next}, revision)) {
        discBaselineTitle = next;
    }
}

void MdWorkspace::commitTrackTitle() {
    if (syncingControls) {
        return;
    }
    const int row = singleSelectedTrackIndex();
    if (row < 0) {
        return;
    }
    const auto next = trackTitleEditor.getText();
    if (next == trackBaselineTitle) {
        return;
    }
    if (onCommand && onCommand({"disc", "track", "rename", juce::String(row), next}, revision)) {
        trackBaselineTitle = next;
    }
}

void MdWorkspace::triggerInsertTrack() {
    commitPendingEdits();
    juce::int64 start = 0;
    juce::int64 end = 0;
    if (!resolveSelectedOrPlayheadRange(timeSelection, playheadSample, masterEndSample, start,
                                        end)) {
        if (onFeedback) {
            onFeedback("Import audio or create a time selection before inserting a track.");
        }
        return;
    }
    const int selRow = singleSelectedTrackIndex();
    const int insertIdx = selRow >= 0 ? (selRow + 1) : static_cast<int>(trackRows.size());
    const juce::String defaultName = "Track " + juce::String(insertIdx + 1);
    if (onCommand) {
        onCommand({"disc", "track", "add", juce::String(start), juce::String(end), defaultName,
                   juce::String(insertIdx)},
                  revision);
    }
}

void MdWorkspace::triggerDuplicateTrack() {
    commitPendingEdits();
    const auto selected = selectedTrackIndices0();
    if (selected.empty() || !onCommand) {
        return;
    }
    if (selected.size() == 1) {
        onCommand({"disc", "track", "duplicate", juce::String(selected.front())}, revision);
        return;
    }
    // Reverse order preserves earlier indices during batch duplication.
    for (auto it = selected.rbegin(); it != selected.rend(); ++it) {
        if (!onCommand({"disc", "track", "duplicate", juce::String(*it)}, revision)) {
            break;
        }
    }
}

void MdWorkspace::triggerSplitTrack() {
    commitPendingEdits();
    const int row = singleSelectedTrackIndex();
    if (row < 0 || !onCommand) {
        return;
    }
    onCommand({"disc", "track", "split", juce::String(row), juce::String(playheadSample)},
              revision);
}

void MdWorkspace::triggerMoveTrack(int delta) {
    commitPendingEdits();
    const auto selected = selectedTrackIndices0();
    if (selected.empty() || !onCommand || delta == 0) {
        return;
    }
    if (selected.size() == 1) {
        const int row = selected.front();
        const int dest = row + delta;
        if (dest < 0 || dest >= static_cast<int>(trackRows.size())) {
            return;
        }
        onCommand({"disc", "track", "move", juce::String(row), juce::String(dest)}, revision);
        return;
    }
    const int fromStart = selected.front();
    const int fromEnd = selected.back();
    const int blockLen = fromEnd - fromStart + 1;
    if (blockLen != static_cast<int>(selected.size())) {
        if (onFeedback) {
            onFeedback("Select a single track or contiguous block of tracks to move.");
        }
        return;
    }
    const int destStart = fromStart + delta;
    if (destStart < 0 || destStart + blockLen > static_cast<int>(trackRows.size())) {
        return;
    }
    if (onCommand({"disc", "track", "move-block", juce::String(fromStart), juce::String(fromEnd),
                   juce::String(destStart)},
                  revision)) {
        syncingControls = true;
        trackList.deselectAllRows();
        for (int r = destStart; r < destStart + blockLen; ++r) {
            trackList.selectRow(r, false, false);
        }
        if (planViewMode == "playback") {
            planList.deselectAllRows();
            for (int r = destStart; r < destStart + blockLen; ++r) {
                planList.selectRow(r, false, false);
            }
        }
        syncingControls = false;
        syncTrackInspectorFields();
        updateTabVisibility();
        updateButtonStates();
        repaint();
    }
}

void MdWorkspace::triggerCopySelectedTracks() {
    commitPendingEdits();
    const auto selected = selectedTrackIndices0();
    if (selected.empty()) {
        return;
    }
    copiedTrackClipboard.clear();
    copiedTrackClipboard.reserve(selected.size());
    for (const int idx : selected) {
        if (idx >= 0 && idx < static_cast<int>(discState.tracks.size())) {
            copiedTrackClipboard.push_back(discState.tracks[static_cast<std::size_t>(idx)]);
        }
    }
    if (onFeedback && !copiedTrackClipboard.empty()) {
        onFeedback("Copied " + juce::String(static_cast<int>(copiedTrackClipboard.size())) +
                   (copiedTrackClipboard.size() == 1 ? " MD track" : " MD tracks") +
                   " (paste with Cmd/Ctrl+V for Play Order aliases).");
    }
}

void MdWorkspace::triggerPasteTracksAfter(int afterRow) {
    commitPendingEdits();
    if (copiedTrackClipboard.empty() || !onCommand) {
        return;
    }
    int dest = juce::jlimit(0, static_cast<int>(discState.tracks.size()), afterRow + 1);
    const auto snapshot = copiedTrackClipboard;
    int pastedCount = 0;
    for (const auto& tr : snapshot) {
        if (tr.parts.empty()) {
            continue;
        }
        const std::string audioKey = carta::effectiveMdAudioId(tr);
        const bool poolAlive =
            !audioKey.empty() && carta::countMdAudioRefs(discState, audioKey) > 0;
        bool ok = false;
        if (poolAlive) {
            ok = onCommand({"disc", "track", "alias-audio",
                            juce::String::fromUTF8(audioKey.c_str()), juce::String(dest)},
                           revision);
        } else {
            const auto title = tr.halfWidthTitle.empty()
                                   ? juce::String::fromUTF8(tr.fullWidthTitle.c_str())
                                   : juce::String::fromUTF8(tr.halfWidthTitle.c_str());
            const auto codecStr =
                tr.codecOverride ? juce::String(carta::mdCodecModeToString(*tr.codecOverride))
                                 : juce::String("default");
            ok = onCommand({"disc", "track", "add", juce::String(tr.parts.front().startSample),
                            juce::String(tr.parts.back().endSample),
                            title.isNotEmpty() ? title : ("Track " + juce::String(dest + 1)),
                            juce::String(dest), codecStr},
                           revision);
        }
        if (!ok) {
            break;
        }
        ++dest;
        ++pastedCount;
    }
    if (onFeedback && pastedCount > 0) {
        onFeedback("Pasted " + juce::String(pastedCount) +
                   (pastedCount == 1 ? " Play Order alias." : " Play Order aliases."));
    }
}

void MdWorkspace::promptMoveTrackToPosition(int row) {
    commitPendingEdits();
    const int count = static_cast<int>(trackRows.size());
    if (row < 0 || row >= count || !onCommand) {
        return;
    }
    auto* dialog = new juce::AlertWindow(
        "Move Track to Position",
        "Enter destination track number (1 to " + juce::String(count) + "):",
        juce::MessageBoxIconType::NoIcon, this);
    dialog->addTextEditor("pos", juce::String(row + 1), "Position:");
    dialog->addButton("Move", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safe = juce::Component::SafePointer<MdWorkspace>(this),
             safeDialog = juce::Component::SafePointer<juce::AlertWindow>(dialog),
             row](int answer) {
                if (safe == nullptr || safeDialog == nullptr || answer != 1 || !safe->onCommand) {
                    return;
                }
                const auto raw = safeDialog->getTextEditorContents("pos").trim();
                const int maxCount = static_cast<int>(safe->trackRows.size());
                const int pos1 = raw.getIntValue();
                if (raw.isEmpty() || !raw.containsOnly("0123456789") || pos1 < 1 ||
                    pos1 > maxCount) {
                    if (safe->onFeedback) {
                        safe->onFeedback("Track position must be between 1 and " +
                                         juce::String(maxCount) + ".");
                    }
                    return;
                }
                const int dest0 = pos1 - 1;
                if (dest0 != row) {
                    safe->onCommand(
                        {"disc", "track", "move", juce::String(row), juce::String(dest0)},
                        safe->revision);
                }
            }),
        true);
}

void MdWorkspace::handleTrackReorderDrag(int fromRow, int insertBeforeRow) {
    commitPendingEdits();
    const int count = static_cast<int>(trackRows.size());
    if (fromRow < 0 || fromRow >= count || !onCommand) {
        return;
    }
    const auto selected = selectedTrackIndices0();
    const bool contiguous = !selected.empty() && (selected.back() - selected.front() + 1 ==
                                                  static_cast<int>(selected.size()));
    if (contiguous && selected.size() > 1 && fromRow >= selected.front() &&
        fromRow <= selected.back()) {
        const int blockStart = selected.front();
        const int blockEnd = selected.back();
        const int blockLen = blockEnd - blockStart + 1;
        int destStart = blockStart;
        if (insertBeforeRow <= blockStart) {
            destStart = insertBeforeRow;
        } else if (insertBeforeRow > blockEnd + 1) {
            destStart = insertBeforeRow - blockLen;
        } else {
            return;
        }
        destStart = juce::jlimit(0, count - blockLen, destStart);
        if (destStart == blockStart) {
            return;
        }
        if (onCommand({"disc", "track", "move-block", juce::String(blockStart),
                       juce::String(blockEnd), juce::String(destStart)},
                      revision)) {
            syncingControls = true;
            trackList.deselectAllRows();
            for (int r = destStart; r < destStart + blockLen; ++r) {
                trackList.selectRow(r, false, false);
            }
            if (planViewMode == "playback") {
                planList.deselectAllRows();
                for (int r = destStart; r < destStart + blockLen; ++r) {
                    planList.selectRow(r, false, false);
                }
            }
            syncingControls = false;
            syncTrackInspectorFields();
            updateTabVisibility();
            updateButtonStates();
            repaint();
        }
        return;
    }

    int dest = (insertBeforeRow > fromRow) ? (insertBeforeRow - 1) : insertBeforeRow;
    dest = juce::jlimit(0, count - 1, dest);
    if (dest != fromRow) {
        onCommand({"disc", "track", "move", juce::String(fromRow), juce::String(dest)}, revision);
    }
}

void MdWorkspace::handlePlanReorderDrag(int fromRow, int insertBeforeRow) {
    if (planViewMode != "playback") {
        return;
    }
    commitPendingEdits();
    const int count = static_cast<int>(planRows.size());
    if (fromRow < 0 || fromRow >= count || !onCommand) {
        return;
    }
    int dest = (insertBeforeRow > fromRow) ? (insertBeforeRow - 1) : insertBeforeRow;
    dest = juce::jlimit(0, count - 1, dest);
    if (dest != fromRow) {
        onCommand({"disc", "track", "move", juce::String(fromRow), juce::String(dest)}, revision);
    }
}

void MdWorkspace::triggerRemoveSelectedTracks() {
    commitPendingEdits();
    const auto selected = selectedTrackIndices0();
    if (selected.empty() || !onCommand) {
        return;
    }
    if (selected.size() == 1) {
        onCommand({"disc", "track", "remove", juce::String(selected.front())}, revision);
        return;
    }
    // Reverse order preserves lower indices across batch deletion.
    for (auto it = selected.rbegin(); it != selected.rend(); ++it) {
        if (!onCommand({"disc", "track", "remove", juce::String(*it)}, revision)) {
            break;
        }
    }
}

void MdWorkspace::triggerMergeSelectedTracks() {
    commitPendingEdits();
    const auto selected = selectedTrackIndices0();
    if (selected.size() < 2 || !onCommand) {
        return;
    }
    if (selected.back() - selected.front() + 1 != static_cast<int>(selected.size())) {
        return;
    }
    for (int idx = selected.back() - 1; idx >= selected.front(); --idx) {
        if (!onCommand({"disc", "track", "merge", juce::String(idx)}, revision)) {
            break;
        }
    }
}

void MdWorkspace::triggerPreviewSelected(bool throughEnd) {
    commitPendingEdits();
    const int row = singleSelectedTrackIndex();
    if (row < 0 || !onPreviewTrack) {
        return;
    }
    onPreviewTrack(trackRows[static_cast<std::size_t>(row)].id, throughEnd);
}

void MdWorkspace::triggerSeekTrackRange() {
    const int row = singleSelectedTrackIndex();
    if (row < 0 || row >= static_cast<int>(trackRows.size())) {
        return;
    }
    if (onSeekPart) {
        onSeekPart(trackRows[static_cast<std::size_t>(row)].id, 0, revision);
    }
}

void MdWorkspace::triggerSetTrackToSelection() {
    commitPendingEdits();
    const int row = singleSelectedTrackIndex();
    if (row < 0 || !timeSelection.hasRange() || !onCommand) {
        return;
    }
    onCommand({"disc", "track", "range", juce::String(row), juce::String(timeSelection.start),
               juce::String(timeSelection.end)},
              revision);
}

void MdWorkspace::triggerMakeUnique() {
    commitPendingEdits();
    const int row = singleSelectedTrackIndex();
    if (row < 0 || !onCommand) {
        return;
    }
    onCommand({"disc", "track", "detach", juce::String(row)}, revision);
}

// ============================================================================
// Popup Menus & Group Assignment Dialog
// ============================================================================

void MdWorkspace::populateAudioPoolSubmenu(juce::PopupMenu& menu, int baseId, int chunkSize) const {
    struct PoolChoice {
        int srcTrackIdx = 0;
        juce::String label;
    };
    std::vector<PoolChoice> pool;
    std::set<std::string> seenKeys;
    const auto enDash = juce::String::fromUTF8("\xe2\x80\x93");
    for (int i = 0; i < static_cast<int>(trackRows.size()); ++i) {
        const auto& tr = trackRows[static_cast<std::size_t>(i)];
        const std::string key = tr.audioId.toStdString();
        if (!seenKeys.insert(key).second) {
            continue;
        }
        juce::String label =
            tr.title + " (" + carta::md_ui::codecShortLabel(tr.effectiveCodec) + ", " +
            carta::md_ui::formatClock(tr.firstStartSample) + enDash +
            carta::md_ui::formatClock(tr.firstEndSample) + ")";
        if (tr.aliasRefCount > 1) {
            label += " [x" + juce::String(tr.aliasRefCount) + "]";
        }
        pool.push_back({i, label});
    }

    const int total = static_cast<int>(pool.size());
    if (total <= 0) {
        return;
    }
    const int pageLen = std::max(1, chunkSize);
    if (total <= pageLen) {
        for (int i = 0; i < total; ++i) {
            menu.addItem(baseId + pool[static_cast<std::size_t>(i)].srcTrackIdx,
                         pool[static_cast<std::size_t>(i)].label);
        }
        return;
    }
    for (int start = 0; start < total; start += pageLen) {
        const int end = std::min(total, start + pageLen);
        juce::PopupMenu sub;
        for (int i = start; i < end; ++i) {
            sub.addItem(baseId + pool[static_cast<std::size_t>(i)].srcTrackIdx,
                        pool[static_cast<std::size_t>(i)].label);
        }
        menu.addSubMenu("Audio " + juce::String(start + 1) + enDash + juce::String(end), sub);
    }
}

void MdWorkspace::showInsertAliasMenu() {
    commitPendingEdits();
    if (trackRows.empty() || !onCommand) {
        return;
    }
    const auto sel = selectedTrackIndices0();
    const int insertDest = !sel.empty() ? (sel.back() + 1) : static_cast<int>(trackRows.size());
    juce::PopupMenu menu;
    populateAudioPoolSubmenu(menu, 100, 30);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&insertAliasButton),
        [safe = juce::Component::SafePointer<MdWorkspace>(this), insertDest,
         rev = revision](int choice) {
            if (safe == nullptr || choice < 100 || !safe->onCommand) {
                return;
            }
            const int srcIdx = choice - 100;
            if (srcIdx < 0 || srcIdx >= static_cast<int>(safe->trackRows.size())) {
                return;
            }
            safe->onCommand({"disc", "track", "alias", juce::String(srcIdx),
                             juce::String(insertDest)},
                            rev);
        });
}

void MdWorkspace::populateAudioBankSubmenu(juce::PopupMenu& menu, int baseId, int chunkSize) const {
    const int total = static_cast<int>(audioBankClipEntries.size());
    if (total <= 0) {
        return;
    }
    const auto enDash = juce::String::fromUTF8("\xe2\x80\x93");
    auto addItem = [&](juce::PopupMenu& target, int index) {
        const auto& entry = audioBankClipEntries[static_cast<std::size_t>(index)];
        juce::String itemLabel =
            entry.label + " (" + carta::md_ui::formatClock(entry.startSample) + enDash +
            carta::md_ui::formatClock(entry.endSample) + ")";
        if (entry.isOnDisc) {
            itemLabel += " [" + compactClusterLabel(entry.startCluster, entry.endCluster) + "]";
        }
        target.addItem(baseId + index, itemLabel);
    };

    const int pageLen = std::max(1, chunkSize);
    if (total <= pageLen) {
        for (int i = 0; i < total; ++i) {
            addItem(menu, i);
        }
        return;
    }
    for (int start = 0; start < total; start += pageLen) {
        const int end = std::min(total, start + pageLen);
        juce::PopupMenu sub;
        for (int i = start; i < end; ++i) {
            addItem(sub, i);
        }
        menu.addSubMenu("Bank " + juce::String(start + 1) + enDash + juce::String(end), sub);
    }
}

void MdWorkspace::showTrackContextMenu(int row) {
    if (row < 0 || row >= static_cast<int>(trackRows.size())) {
        return;
    }
    const bool isAliased = trackRows[static_cast<std::size_t>(row)].aliasRefCount > 1;
    juce::PopupMenu menu;
    menu.addItem(1, "Rename Audio Item...");
    menu.addItem(2, "ASCII & Full-Width Titles...");
    menu.addItem(3, "Flags & Timestamp...");
    menu.addSeparator();
    menu.addItem(4, "Insert Play Order Alias After");
    menu.addItem(14, "Make Unique (Detach Alias)", isAliased);
    menu.addItem(11, "Copy Track(s)");
    menu.addItem(12, "Paste Alias(es) After", !copiedTrackClipboard.empty());
    menu.addItem(10, "Move Track to Position...", trackRows.size() > 1);
    menu.addItem(8, "Merge with Next Track",
                 !isAliased && row + 1 < static_cast<int>(trackRows.size()));
    menu.addItem(15, "Force Gapped Break Before Track",
                 row > 0 && !trackRows[static_cast<std::size_t>(row)].onDisc,
                 trackRows[static_cast<std::size_t>(row)].forceGapBefore);
    menu.addItem(5, "Remove Track");
    {
        menu.addSeparator();
        juce::PopupMenu poolSub;
        populateAudioPoolSubmenu(poolSub, 2000, 30);
        menu.addSubMenu("Insert Alias After from Audio Pool", poolSub);
    }
    if (!audioBankClipEntries.empty()) {
        juce::PopupMenu bankSub;
        populateAudioBankSubmenu(bankSub, 1000, 30);
        menu.addSubMenu("Insert Standalone Track from Clips", bankSub);
    }
    menu.addSeparator();
    menu.addItem(6, "Preview Track");
    menu.addItem(7, "Preview From Here to End");
    menu.addItem(13, "Show in Audio Bank (Physical Map)", !audioBankRows.empty());

    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&trackList),
        [safe = juce::Component::SafePointer<MdWorkspace>(this), row](int choice) {
            if (safe == nullptr || choice <= 0 || row < 0 ||
                row >= static_cast<int>(safe->trackRows.size())) {
                return;
            }
            if (choice >= 2000 && safe->onCommand) {
                const int srcIdx = choice - 2000;
                if (srcIdx >= 0 && srcIdx < static_cast<int>(safe->trackRows.size())) {
                    safe->onCommand({"disc", "track", "alias", juce::String(srcIdx),
                                     juce::String(row + 1)},
                                    safe->revision);
                }
                return;
            }
            if (choice >= 1000 && safe->onCommand) {
                const int entryIdx = choice - 1000;
                if (entryIdx >= 0 &&
                    entryIdx < static_cast<int>(safe->audioBankClipEntries.size())) {
                    const auto& entry =
                        safe->audioBankClipEntries[static_cast<std::size_t>(entryIdx)];
                    const auto label = entry.label.isNotEmpty()
                                           ? entry.label
                                           : ("Track " + juce::String(row + 2));
                    const auto codecStr =
                        (entry.isOnDisc && entry.codec != safe->discState.defaultCodec)
                            ? juce::String(carta::mdCodecModeToString(entry.codec))
                            : juce::String("default");
                    safe->onCommand({"disc", "track", "add", juce::String(entry.startSample),
                                     juce::String(entry.endSample), label,
                                     juce::String(row + 1), codecStr},
                                    safe->revision);
                }
                return;
            }

            const bool multiCapable = (choice == 4 || choice == 5 || choice == 11 || choice == 12);
            if (!multiCapable || !safe->trackList.isRowSelected(row)) {
                safe->trackList.selectRow(row, false, true);
            }

            if (choice == 1) {
                safe->focusTrackRename();
            } else if (choice == 2 && safe->onEditTrackForms) {
                safe->onEditTrackForms();
            } else if (choice == 3 && safe->onEditTrackFlags) {
                safe->onEditTrackFlags();
            } else if (choice == 4) {
                safe->triggerDuplicateTrack();
            } else if (choice == 14) {
                safe->triggerMakeUnique();
            } else if (choice == 11) {
                safe->triggerCopySelectedTracks();
            } else if (choice == 12) {
                const auto sel = safe->selectedTrackIndices0();
                const int after = !sel.empty() ? sel.back() : row;
                safe->triggerPasteTracksAfter(after);
            } else if (choice == 10) {
                safe->promptMoveTrackToPosition(row);
            } else if (choice == 8 && safe->onCommand) {
                safe->onCommand({"disc", "track", "merge", juce::String(row)}, safe->revision);
            } else if (choice == 15 && safe->onCommand) {
                const bool forced = safe->trackRows[static_cast<std::size_t>(row)].forceGapBefore;
                safe->onCommand({"disc", "track", "gap", juce::String(row), forced ? "off" : "on"},
                                safe->revision);
            } else if (choice == 5) {
                safe->triggerRemoveSelectedTracks();
            } else if (choice == 6) {
                safe->triggerPreviewSelected(false);
            } else if (choice == 7) {
                safe->triggerPreviewSelected(true);
            } else if (choice == 13) {
                int bankTarget = -1;
                for (int bi = 0; bi < static_cast<int>(safe->audioBankRows.size()); ++bi) {
                    const auto& refs =
                        safe->audioBankRows[static_cast<std::size_t>(bi)].referencingTrackIndices0;
                    if (std::find(refs.begin(), refs.end(), row) != refs.end()) {
                        bankTarget = bi;
                        break;
                    }
                }
                safe->setPlanViewMode("bank");
                safe->setActiveTab("record", true);
                if (bankTarget >= 0 &&
                    bankTarget < static_cast<int>(safe->audioBankRows.size())) {
                    safe->planList.selectRow(bankTarget, false, true);
                    safe->planList.scrollToEnsureRowIsOnscreen(bankTarget);
                }
            }
        });
}

void MdWorkspace::showPlanContextMenu(int row) {
    if (row < 0 || row >= static_cast<int>(planRows.size())) {
        return;
    }
    juce::PopupMenu menu;
    menu.addItem(1, "Edit Track in Tracks Tab");
    menu.addItem(2, "Duplicate Track (Shared Audio)");
    menu.addItem(3, "Move Track to Position...", planRows.size() > 1);
    menu.addSeparator();
    menu.addItem(4, "Preview Track");
    menu.addItem(5, "Show Audio in Audio Bank", !audioBankRows.empty());
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&planList),
        [safe = juce::Component::SafePointer<MdWorkspace>(this), row](int choice) {
            if (safe == nullptr || choice <= 0 || row < 0 ||
                row >= static_cast<int>(safe->planRows.size())) {
                return;
            }
            if (choice == 1) {
                safe->triggerOpenPlanRow(row);
            } else if (choice == 2 && safe->onCommand) {
                safe->onCommand({"disc", "track", "duplicate", juce::String(row)}, safe->revision);
            } else if (choice == 3) {
                safe->promptMoveTrackToPosition(row);
            } else if (choice == 4 && safe->onPreviewTrack) {
                safe->onPreviewTrack(safe->planRows[static_cast<std::size_t>(row)].id, false);
            } else if (choice == 5) {
                int bankTarget = -1;
                for (int bi = 0; bi < static_cast<int>(safe->audioBankRows.size()); ++bi) {
                    const auto& refs =
                        safe->audioBankRows[static_cast<std::size_t>(bi)].referencingTrackIndices0;
                    if (std::find(refs.begin(), refs.end(), row) != refs.end()) {
                        bankTarget = bi;
                        break;
                    }
                }
                safe->setPlanViewMode("bank");
                if (bankTarget >= 0 &&
                    bankTarget < static_cast<int>(safe->audioBankRows.size())) {
                    safe->planList.selectRow(bankTarget, false, true);
                    safe->planList.scrollToEnsureRowIsOnscreen(bankTarget);
                }
            }
        });
}

void MdWorkspace::showAudioBankContextMenu(int row) {
    if (row < 0 || row >= static_cast<int>(audioBankRows.size())) {
        return;
    }
    const auto& br = audioBankRows[static_cast<std::size_t>(row)];
    const bool hasRange = br.timelineEndSample > br.timelineStartSample;
    const bool hasRefTrack = !br.referencingTrackIndices0.empty();

    juce::PopupMenu menu;
    menu.addItem(1, "Seek Audio on Timeline", hasRange && onSeekRange != nullptr);
    menu.addItem(2,
                 hasRefTrack ? "Insert Play Order Alias (at End)"
                             : "Add New Standalone Track from This Audio",
                 (hasRefTrack || hasRange) && onCommand != nullptr);

    if (!br.referencingTrackIndices0.empty()) {
        menu.addSeparator();
        juce::PopupMenu jumpMenu;
        const int refTotal = static_cast<int>(br.referencingTrackIndices0.size());
        const auto enDash = juce::String::fromUTF8("\xe2\x80\x93");
        auto addRefItem = [&](juce::PopupMenu& target, int k) {
            const int trIdx = br.referencingTrackIndices0[static_cast<std::size_t>(k)];
            if (trIdx >= 0 && trIdx < static_cast<int>(trackRows.size())) {
                target.addItem(
                    100 + k, "T" + juce::String(trIdx + 1) + ": " +
                                 trackRows[static_cast<std::size_t>(trIdx)].title);
            }
        };
        if (refTotal <= 30) {
            for (int k = 0; k < refTotal; ++k) {
                addRefItem(jumpMenu, k);
            }
        } else {
            for (int start = 0; start < refTotal; start += 30) {
                const int end = std::min(refTotal, start + 30);
                juce::PopupMenu sub;
                for (int k = start; k < end; ++k) {
                    addRefItem(sub, k);
                }
                jumpMenu.addSubMenu("Refs " + juce::String(start + 1) + enDash + juce::String(end),
                                    sub);
            }
        }
        menu.addSubMenu("Jump to Referencing Track", jumpMenu);
    }

    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&planList),
        [safe = juce::Component::SafePointer<MdWorkspace>(this), row](int choice) {
            if (safe == nullptr || choice <= 0 || row < 0 ||
                row >= static_cast<int>(safe->audioBankRows.size())) {
                return;
            }
            const auto& bankRow = safe->audioBankRows[static_cast<std::size_t>(row)];
            if (choice == 1) {
                if (safe->onSeekRange &&
                    bankRow.timelineEndSample > bankRow.timelineStartSample) {
                    safe->onSeekRange(bankRow.timelineStartSample, bankRow.timelineEndSample,
                                      safe->revision);
                }
            } else if (choice == 2 && safe->onCommand) {
                const int dest = static_cast<int>(safe->trackRows.size());
                if (!bankRow.referencingTrackIndices0.empty()) {
                    safe->onCommand({"disc", "track", "alias",
                                     juce::String(bankRow.referencingTrackIndices0.front()),
                                     juce::String(dest)},
                                    safe->revision);
                } else {
                    const auto label = bankRow.label.isNotEmpty()
                                           ? bankRow.label
                                           : ("Track " + juce::String(dest + 1));
                    const auto codecStr =
                        (bankRow.isOnDisc && bankRow.codec != safe->discState.defaultCodec)
                            ? juce::String(carta::mdCodecModeToString(bankRow.codec))
                            : juce::String("default");
                    safe->onCommand({"disc", "track", "add",
                                     juce::String(bankRow.timelineStartSample),
                                     juce::String(bankRow.timelineEndSample), label,
                                     juce::String(dest), codecStr},
                                    safe->revision);
                }
            } else if (choice >= 100) {
                const int k = choice - 100;
                if (k >= 0 && k < static_cast<int>(bankRow.referencingTrackIndices0.size())) {
                    const int trIdx = bankRow.referencingTrackIndices0[static_cast<std::size_t>(k)];
                    if (trIdx >= 0 && trIdx < static_cast<int>(safe->trackRows.size())) {
                        const auto trId = safe->trackRows[static_cast<std::size_t>(trIdx)].id;
                        safe->setActiveTab("tracks", true);
                        safe->selectTrackById(trId, true);
                    }
                }
            }
        });
}

void MdWorkspace::showGroupMenu() {
    commitPendingEdits();
    const auto selected = selectedTrackIndices0();
    const bool contiguous = !selected.empty() && (selected.back() - selected.front() + 1 ==
                                                  static_cast<int>(selected.size()));

    juce::PopupMenu menu;
    menu.addItem(1, "Create / Update Group from Selected Tracks...", contiguous);
    menu.addItem(2, "Remove Group(s) on Selected Tracks", !selected.empty());
    if (!discState.groups.empty()) {
        menu.addSeparator();
        for (std::size_t g = 0; g < discState.groups.size(); ++g) {
            const auto& grp = discState.groups[g];
            const auto label =
                "Ungroup \"" +
                juce::String::fromUTF8(
                    (grp.halfWidthTitle.empty() ? grp.fullWidthTitle : grp.halfWidthTitle)
                        .c_str()) +
                "\" (Tracks " + juce::String(grp.startTrack) +
                juce::String::fromUTF8("\xe2\x80\x93") + juce::String(grp.endTrack) + ")";
            menu.addItem(100 + static_cast<int>(g), label);
        }
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&groupMenuButton),
                       [safe = juce::Component::SafePointer<MdWorkspace>(this)](int choice) {
                           if (safe == nullptr || choice <= 0) {
                               return;
                           }
                           if (choice == 1) {
                               safe->promptCreateGroupForSelection();
                           } else if (choice == 2) {
                               safe->clearGroupsForSelection();
                           } else if (choice >= 100 && safe->onCommand) {
                               const std::size_t idx = static_cast<std::size_t>(choice - 100);
                               if (idx < safe->discState.groups.size()) {
                                   const auto& grp = safe->discState.groups[idx];
                                   safe->onCommand({"disc", "group", "clear",
                                                    juce::String(grp.startTrack),
                                                    juce::String(grp.endTrack)},
                                                   safe->revision);
                               }
                           }
                       });
}

void MdWorkspace::promptCreateGroupForSelection() {
    const auto selected = selectedTrackIndices0();
    if (selected.empty() ||
        (selected.back() - selected.front() + 1 != static_cast<int>(selected.size()))) {
        if (onFeedback) {
            onFeedback("Select contiguous MiniDisc tracks to assign a group.");
        }
        return;
    }
    const int startTrack1 = selected.front() + 1;
    const int endTrack1 = selected.back() + 1;
    const auto rev = revision;

    auto* dialog = new juce::AlertWindow("MiniDisc Group",
                                         "Assign Tracks " + juce::String(startTrack1) +
                                             juce::String::fromUTF8("\xe2\x80\x93") +
                                             juce::String(endTrack1) + " to a MiniDisc group.",
                                         juce::MessageBoxIconType::NoIcon, this);
    juce::String initialHalf =
        "Group " + juce::String(static_cast<int>(discState.groups.size() + 1));
    juce::String initialFull;
    for (const auto& grp : discState.groups) {
        if (!(endTrack1 < grp.startTrack || startTrack1 > grp.endTrack)) {
            if (!grp.halfWidthTitle.empty()) {
                initialHalf = juce::String::fromUTF8(grp.halfWidthTitle.c_str());
            }
            if (!grp.fullWidthTitle.empty()) {
                initialFull = juce::String::fromUTF8(grp.fullWidthTitle.c_str());
            }
            break;
        }
    }
    dialog->addTextEditor("half", initialHalf, "ASCII title:");
    dialog->addTextEditor("full", initialFull, "Full-width title (optional):");
    dialog->addButton("Apply", 1);
    dialog->addButton("Cancel", 0);
    dialog->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safe = juce::Component::SafePointer<MdWorkspace>(this),
             safeDialog = juce::Component::SafePointer<juce::AlertWindow>(dialog), startTrack1,
             endTrack1, rev](int answer) {
                if (safe != nullptr && safeDialog != nullptr && answer == 1 && safe->onCommand) {
                    const auto half = safeDialog->getTextEditorContents("half").trim();
                    const auto full = safeDialog->getTextEditorContents("full").trim();
                    safe->onCommand({"disc", "group", "set", juce::String(startTrack1),
                                     juce::String(endTrack1), half, full},
                                    rev);
                }
            }),
        true);
}

void MdWorkspace::clearGroupsForSelection() {
    const auto selected = selectedTrackIndices0();
    if (selected.empty() || !onCommand) {
        return;
    }
    onCommand({"disc", "group", "clear", juce::String(selected.front() + 1),
               juce::String(selected.back() + 1)},
              revision);
}

// ============================================================================
// Row Painting, Tooltips, & Capacity Meters
// ============================================================================

void MdWorkspace::paintTrackRow(int row, juce::Graphics& g, int width, int height,
                                bool selected) const {
    if (row < 0 || row >= static_cast<int>(trackRows.size())) {
        return;
    }
    const auto& tr = trackRows[static_cast<std::size_t>(row)];
    const bool dark = juce::Desktop::getInstance().isDarkModeActive();
    const auto textPrimary = dark ? juce::Colour(0xffe0e0e0) : juce::Colour(0xff262626);
    const auto textMuted = dark ? juce::Colour(0xff9e9e9e) : juce::Colour(0xff666666);
    paintAlternatingRowBackground(g, row, width, height, selected, dark);

    // Group stripe on far left
    if (tr.groupColorIndex >= 0) {
        g.setColour(carta::md_ui::groupColour(tr.groupColorIndex, dark));
        g.fillRect(0, 1, 4, height - 2);
    }

    // Track number
    g.setColour(textMuted);
    g.setFont(juce::FontOptions(11.3f, juce::Font::bold));
    g.drawText(juce::String::formatted("%02d", row + 1), 8, 0, 24, height,
               juce::Justification::centredLeft, false);

    // Group badge
    if (tr.groupColorIndex >= 0) {
        const auto gc = carta::md_ui::groupColour(tr.groupColorIndex, dark);
        const auto pill = juce::Rectangle<int>(36, 4, 72, height - 8);
        g.setColour(gc.withAlpha(dark ? 0.25f : 0.14f));
        g.fillRoundedRectangle(pill.toFloat(), 3.0f);
        g.setColour(dark ? gc.brighter(0.4f) : gc.darker(0.1f));
        g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
        g.drawText(juce::String::fromUTF8(tr.groupName.c_str()), pill.reduced(4, 0),
                   juce::Justification::centred, true);
    } else {
        g.setColour(textMuted.withAlpha(0.55f));
        g.setFont(juce::FontOptions(11.0f));
        g.drawText(juce::CharPointer_UTF8("\xe2\x80\x94"), 36, 0, 72, height,
                   juce::Justification::centred, false);
    }

    // Attributes suffix
    juce::StringArray attrs;
    if (discState.loadedDisc.active) {
        attrs.add(tr.onDisc ? "DISC" : "NEW");
    }
    if (tr.writeProtected) {
        attrs.add("WP");
    }
    if (tr.scms) {
        attrs.add("SCMS");
    }
    if (tr.emphasis) {
        attrs.add("EMP");
    }
    if (tr.fullWidthTitle.isNotEmpty()) {
        attrs.add("FW");
    }
    if (tr.partCount > 1) {
        attrs.add("FRAG " + juce::String(tr.partCount));
    }
    if (tr.forceGapBefore) {
        attrs.add("GAP");
    }
    const auto attrText = attrs.joinIntoString(" ");
    const int attrWidth =
        attrText.isNotEmpty() ? std::min(120, static_cast<int>(attrText.length()) * 7 + 12) : 0;

    const int rightReserve = 166;
    const int titleX = 114;
    const int titleAvail = std::max(30, width - titleX - rightReserve - attrWidth);
    if (!(inlineEditingRow == row && inlineTrackTitleEditor.isVisible())) {
        const juce::Font titleFont(
            juce::FontOptions(12.2f, selected ? juce::Font::bold : juce::Font::plain));
        g.setColour(textPrimary);
        g.setFont(titleFont);
        g.drawText(tr.title, titleX, 0, titleAvail, height, juce::Justification::centredLeft, true);
        if (tr.artist.isNotEmpty() || tr.album.isNotEmpty()) {
            juce::GlyphArrangement ga;
            ga.addLineOfText(titleFont, tr.title, 0.0f, 0.0f);
            const int titleW = std::min(
                titleAvail,
                static_cast<int>(std::ceil(ga.getBoundingBox(0, -1, false).getWidth())) + 2);
            if (titleAvail - titleW > 28) {
                juce::String meta = tr.artist;
                if (tr.album.isNotEmpty()) {
                    meta += (meta.isNotEmpty() ? juce::String::fromUTF8(" \xc2\xb7 ")
                                               : juce::String()) +
                            tr.album;
                }
                g.setColour(textMuted);
                g.setFont(juce::FontOptions(10.8f));
                g.drawText(juce::String::fromUTF8(" \xe2\x80\x94 ") + meta, titleX + titleW, 0,
                           titleAvail - titleW, height, juce::Justification::centredLeft, true);
            }
        }
    }

    if (attrWidth > 0) {
        g.setColour(textMuted);
        g.setFont(juce::FontOptions(9.8f, juce::Font::bold));
        g.drawText(attrText, titleX + titleAvail + 2, 0, attrWidth - 4, height,
                   juce::Justification::centredRight, true);
    }

    // Codec badge
    const auto codecCol = carta::md_ui::codecColour(tr.effectiveCodec, dark);
    const auto codecRect = juce::Rectangle<int>(width - 158, 4, 48, height - 8);
    g.setColour(codecCol.withAlpha(dark ? 0.28f : 0.14f));
    g.fillRoundedRectangle(codecRect.toFloat(), 3.0f);
    g.setColour(dark ? codecCol.brighter(0.35f) : codecCol);
    g.setFont(juce::FontOptions(10.3f, tr.hasCodecOverride ? juce::Font::bold : juce::Font::plain));
    g.drawText((tr.hasCodecOverride ? "*" : "") + carta::md_ui::codecShortLabel(tr.effectiveCodec),
               codecRect, juce::Justification::centred, false);

    // Play Order uses count
    const bool aliased = tr.aliasRefCount > 1;
    g.setColour(aliased ? (dark ? juce::Colour(0xff5eead4) : juce::Colour(0xff0f766e))
                        : textMuted);
    g.setFont(juce::FontOptions(11.3f, aliased ? juce::Font::bold : juce::Font::plain));
    g.drawText(aliased ? ("x" + juce::String(tr.aliasRefCount)) : juce::String("1"), width - 104,
               0, 34, height, juce::Justification::centred, false);

    // Duration
    g.setColour(textPrimary);
    g.setFont(juce::FontOptions(11.5f));
    g.drawText(carta::md_ui::formatClock(tr.totalSamples), width - 66, 0, 56, height,
               juce::Justification::centredRight, false);
}

void MdWorkspace::paintAudioSourceCard(juce::Graphics& g, bool dark) const {
    if (audioSourceCardBounds.isEmpty()) {
        return;
    }
    const int row = singleSelectedTrackIndex();
    if (row < 0 || row >= static_cast<int>(trackRows.size()) ||
        row >= static_cast<int>(discState.tracks.size())) {
        return;
    }

    const auto& tr = trackRows[static_cast<std::size_t>(row)];
    const auto& rawTrack = discState.tracks[static_cast<std::size_t>(row)];
    const bool isHiMd = carta::isHiMdMedium(discState.medium);
    const auto cardBg = dark ? juce::Colour(0xff1e1e1e) : juce::Colour(0xfff4f4f4);
    const auto cardBorder = dark ? juce::Colour(0xff3a3a3a) : juce::Colour(0xffd0d0d0);
    const auto textPrimary = dark ? juce::Colour(0xffe0e0e0) : juce::Colour(0xff262626);
    const auto textMuted = dark ? juce::Colour(0xff9e9e9e) : juce::Colour(0xff666666);
    const auto teal = dark ? juce::Colour(0xff5eead4) : juce::Colour(0xff0f766e);

    g.setColour(cardBg);
    g.fillRoundedRectangle(audioSourceCardBounds.toFloat(), 5.0f);
    g.setColour(cardBorder);
    g.drawRoundedRectangle(audioSourceCardBounds.toFloat().reduced(0.5f), 5.0f, 1.0f);

    auto body = audioSourceCardBounds.reduced(8, 7);
    body.removeFromTop(24);
    body.removeFromBottom(28);
    if (body.getHeight() < 16 || body.getWidth() < 40) {
        return;
    }

    const auto enDash = juce::String::fromUTF8(" \xe2\x80\x93 ");
    const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");

    const auto rangeText = carta::md_ui::formatClock(tr.firstStartSample) + enDash +
                           carta::md_ui::formatClock(
                               rawTrack.parts.empty() ? tr.firstEndSample
                                                      : rawTrack.parts.back().endSample) +
                           dot + "Dur " + carta::md_ui::formatClock(tr.totalSamples);
    g.setColour(textPrimary);
    g.setFont(juce::FontOptions(11.3f, juce::Font::bold));
    g.drawText(rangeText, body.removeFromTop(18), juce::Justification::centredLeft, true);

    juce::String physDetail;
    if (isHiMd) {
        const auto estBytes = carta::estimateHiMdTrackBytes(tr.effectiveCodec, tr.totalSamples);
        physDetail = (tr.onDisc ? "On-Disc Hi-MD Slot" : "Audio Pool") + dot +
                     carta::md_ui::codecLongLabel(tr.effectiveCodec) + dot +
                     formatMegabytes(estBytes);
    } else {
        int spanIdx = -1;
        bool isOnDisc = false;
        carta::MdPhysAddr startA{};
        carta::MdPhysAddr endA{};
        for (const auto& place : physicalPlan.placements) {
            if (place.trackIndex == row && place.partIndex == 0) {
                spanIdx = place.spanIndex;
                isOnDisc = place.isOnDisc;
                startA = place.startAddr;
                endA = place.endAddr;
                break;
            }
        }
        if (isOnDisc) {
            physDetail = "On-Disc UTOC " + carta::md_ui::formatAddr(startA) + ".." +
                         carta::md_ui::formatAddr(endA);
        } else if (spanIdx >= 0) {
            physDetail = "Span #" + juce::String(spanIdx + 1) + " (" +
                         carta::md_ui::formatAddr(startA) + ".." + carta::md_ui::formatAddr(endA) +
                         ")";
        } else {
            physDetail = "(unplanned audio span)";
        }
    }
    if (body.getHeight() >= 16) {
        g.setColour(textMuted);
        g.setFont(juce::FontOptions(10.6f));
        g.drawText(physDetail, body.removeFromTop(16), juce::Justification::centredLeft, true);
    }

    if (body.getHeight() >= 16) {
        const juce::String usageText =
            tr.aliasRefCount > 1
                ? ("Shared across " + juce::String(tr.aliasRefCount) +
                   " Play Order tracks (0 extra disc space for aliases)")
                : juce::String("Single unique audio item (0 aliases)");
        g.setColour(tr.aliasRefCount > 1 ? teal : textMuted);
        g.setFont(juce::FontOptions(10.5f, tr.aliasRefCount > 1 ? juce::Font::bold : juce::Font::plain));
        g.drawText(usageText, body.removeFromTop(16), juce::Justification::centredLeft, true);
    }

    if (rawTrack.parts.size() > 1 && body.getHeight() >= 16) {
        g.setColour(textMuted);
        g.setFont(juce::FontOptions(10.3f));
        g.drawText("Physical fragments: " + juce::String(static_cast<int>(rawTrack.parts.size())) +
                       " UTOC ranges (casually recorded MD)",
                   body.removeFromTop(16), juce::Justification::centredLeft, true);
    }
}

juce::String MdWorkspace::tooltipForTrackRow(int row) const {
    if (row < 0 || row >= static_cast<int>(trackRows.size())) {
        return {};
    }
    const auto& tr = trackRows[static_cast<std::size_t>(row)];
    juce::String tip = "Play Order #" + juce::String(row + 1) + ": " + tr.title;
    if (tr.fullWidthTitle.isNotEmpty()) {
        tip += " [" + tr.fullWidthTitle + "]";
    }
    if (tr.artist.isNotEmpty()) {
        tip += juce::String::fromUTF8(" \xc2\xb7 Artist: ") + tr.artist;
    }
    if (tr.album.isNotEmpty()) {
        tip += juce::String::fromUTF8(" \xc2\xb7 Album: ") + tr.album;
    }
    tip += juce::String::fromUTF8(" \xc2\xb7 ") + carta::md_ui::codecLongLabel(tr.effectiveCodec) +
           juce::String::fromUTF8(" \xc2\xb7 ") + carta::md_ui::formatClock(tr.totalSamples);
    if (tr.aliasRefCount > 1) {
        tip += juce::String::fromUTF8(" \xc2\xb7 Play Order alias (") +
               juce::String(tr.aliasRefCount) + " uses sharing 1 Audio Pool item)";
    }
    if (tr.partCount > 1) {
        tip += juce::String::fromUTF8(" \xc2\xb7 ") + juce::String(tr.partCount) +
               " physical fragments";
    }
    if (tr.forceGapBefore) {
        tip += juce::String::fromUTF8(" \xc2\xb7 Forced gapped break before track");
    }
    tip += juce::String::fromUTF8(" \xc2\xb7 Double-click to edit title in place");
    return tip;
}

juce::String MdWorkspace::formatSecMmSs(int totalSeconds) {
    const int s = std::max(0, totalSeconds);
    return juce::String::formatted("%02d:%02d", s / 60, s % 60);
}

void MdWorkspace::updatePanelHoverTooltip(juce::Point<int> point) {
    if (budgetPillBounds.contains(point)) {
        if (carta::isHiMdMedium(discState.medium)) {
            const auto capBytes = hiMdTotalBytes > 0
                                      ? hiMdTotalBytes
                                      : carta::hiMdMediumCapacityBytes(discState.medium);
            lastHoverTooltip =
                "Hi-MD Disc Layout: " + juce::String(physicalPlan.uniqueAudioTrackCount) +
                " unique audio items across " + juce::String(physicalPlan.playOrderTrackCount) +
                " Play Order track(s), est. " + formatMegabytes(hiMdUsedBytes) + " / " +
                formatMegabytes(capBytes) +
                (discState.loadedDisc.active ? " (loaded Hi-MD disc)" : "") + ".";
        } else {
            lastHoverTooltip =
                utocValid
                    ? "UTOC Sectors: " + juce::String(budget.usedTracks) + "/" +
                          juce::String(budget.maxTracks) + " tracks, " +
                          juce::String(budget.usedFragments) + "/" +
                          juce::String(budget.maxFragments) + " fragments, " +
                          juce::String(budget.usedClusters) + "/" +
                          juce::String(budget.totalClusters) + " clusters, " +
                          juce::String(budget.usedHalfWidthCells) + "/" +
                          juce::String(budget.maxHalfWidthCells) + " ASCII title cells, " +
                          juce::String(budget.usedFullWidthCells) + "/" +
                          juce::String(budget.maxFullWidthCells) + " full-width cells."
                    : juce::String::fromUTF8(utocErrorText.c_str());
        }
    } else if (activeTab == "record" && targetBarBounds.contains(point)) {
        if (carta::isHiMdMedium(discState.medium)) {
            const auto capBytes =
                std::max<std::int64_t>(1, hiMdTotalBytes > 0
                                              ? hiMdTotalBytes
                                              : carta::hiMdMediumCapacityBytes(discState.medium));
            lastHoverTooltip =
                "Target Hi-MD Capacity: " + formatMegabytes(hiMdUsedBytes) + " / " +
                formatMegabytes(capBytes) + " (" +
                juce::String(physicalPlan.uniqueAudioTrackCount) + " unique audio item(s) across " +
                juce::String(physicalPlan.playOrderTrackCount) + " Play Order track(s)" +
                (physicalPlan.hiMdSavedBytes > 0
                     ? (", " + formatMegabytes(physicalPlan.hiMdSavedBytes) + " saved by aliases")
                     : juce::String()) +
                ").";
        } else {
            const int totalClu = std::max(1, discState.loadedDisc.active
                                                 ? discState.loadedDisc.physicalTotalClusters()
                                                 : carta::mdTotalClusters(discState.medium));
            if (discState.loadedDisc.active) {
                lastHoverTooltip =
                    "Target Disc Capacity: " + juce::String(physicalPlan.usedClusters) + "/" +
                    juce::String(totalClu) + " clusters (" +
                    juce::String(physicalPlan.onDiscClusters) + " on-disc + " +
                    juce::String(physicalPlan.newUploadClusters) + " new upload(s)" +
                    (physicalPlan.reclaimedClusters > 0
                         ? (", reclaim " + juce::String(physicalPlan.reclaimedClusters) +
                            " clusters")
                         : juce::String()) +
                    ").";
            } else {
                lastHoverTooltip =
                    "Target Disc Capacity: " + juce::String(physicalPlan.usedClusters) + "/" +
                    juce::String(totalClu) + " clusters across " +
                    juce::String(static_cast<int>(physicalPlan.spans.size())) +
                    " physical audio upload(s)" +
                    (physicalPlan.savedClusters > 0
                         ? (" (" + juce::String(physicalPlan.savedClusters) +
                            " clusters saved by aliasing)")
                         : juce::String()) +
                    ".";
            }
        }
    } else if (activeTab == "record" && deviceDiscMeterBounds.contains(point)) {
        if (recordUiState.activeDeviceIsHiMd && recordUiState.hasHiMdStatus) {
            const auto& st = recordUiState.hiMdStatus;
            const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
            lastHoverTooltip =
                st.fsTotalBytes > 0
                    ? ("Physical Hi-MD Disc: " +
                       (!st.discTitle.empty() ? juce::String::fromUTF8(st.discTitle.c_str())
                                              : juce::String("Untitled Hi-MD Disc")) +
                       " (" + juce::String(st.trackCount) + " tracks, used " +
                       formatMegabytes(st.fsUsedBytes) + dot + "free " +
                       formatMegabytes(st.fsFreeBytes) + ")")
                    : "No Hi-MD disc mounted on selected USB recorder.";
        } else if (!recordUiState.activeDeviceIsHiMd && recordUiState.hasDeviceStatus) {
            const auto& st = recordUiState.deviceStatus;
            lastHoverTooltip =
                st.discPresent
                    ? ("Physical Disc: " + juce::String::fromUTF8(st.discTitle.c_str()) + " (" +
                       juce::String(st.trackCount) + " tracks, used " +
                       formatSecMmSs(st.usedSeconds) + ", free " + formatSecMmSs(st.leftSeconds) +
                       ")")
                    : "No disc inserted in selected USB recorder.";
        } else {
            lastHoverTooltip.clear();
        }
    } else {
        lastHoverTooltip.clear();
    }
}

void MdWorkspace::paintPhysicalDiscMeter(juce::Graphics& g, juce::Rectangle<int> bounds,
                                         bool dark) const {
    if (bounds.isEmpty() || bounds.getHeight() < 24 || bounds.getWidth() < 80) {
        return;
    }
    const auto textPrimary = dark ? juce::Colour(0xffe0e0e0) : juce::Colour(0xff262626);
    const auto textMuted = dark ? juce::Colour(0xff9e9e9e) : juce::Colour(0xff666666);
    const auto wellBg = dark ? juce::Colour(0xff1c1c1c) : juce::Colour(0xffe6e6e6);
    const auto border = dark ? juce::Colour(0xff3c3c3c) : juce::Colour(0xffc8c8c8);

    g.setColour(dark ? juce::Colour(0xff212121) : juce::Colour(0xfff2f2f2));
    g.fillRoundedRectangle(bounds.toFloat(), 4.0f);
    g.setColour(border);
    g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 4.0f, 1.0f);

    auto area = bounds.reduced(8, 4);
    if (recordUiState.activeDeviceIsHiMd) {
        if (!recordUiState.hasHiMdStatus || recordUiState.hiMdStatus.deviceId.empty()) {
            g.setColour(textMuted);
            g.setFont(juce::FontOptions(11.0f));
            g.drawText("Connect a Sony NetMD / Hi-MD recorder via USB and click Refresh.", area,
                       juce::Justification::centredLeft, true);
            return;
        }
        const auto& st = recordUiState.hiMdStatus;
        auto headerRow = area.removeFromTop(16);
        const bool discPresent = st.fsTotalBytes > 0;
        juce::Colour stateCol = dark ? juce::Colour(0xff8a8a8a) : juce::Colour(0xff666666);
        juce::String stateLabel;
        if (!discPresent) {
            stateLabel = "NO DISC";
        } else {
            stateCol = dark ? juce::Colour(0xff8b5cf6) : juce::Colour(0xff6d28d9);
            stateLabel = st.trackCount == 0 ? juce::String("BLANK")
                                            : (juce::String(st.trackCount) + " TRK");
        }
        const int pillW = std::min(96, static_cast<int>(stateLabel.length()) * 7 + 16);
        const auto pillRect = headerRow.removeFromRight(pillW);
        g.setColour(stateCol.withAlpha(dark ? 0.28f : 0.16f));
        g.fillRoundedRectangle(pillRect.toFloat(), 3.0f);
        g.setColour(dark ? stateCol.brighter(0.35f) : stateCol);
        g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        g.drawText(stateLabel, pillRect, juce::Justification::centred, false);

        headerRow.removeFromRight(6);
        const juce::String titleText =
            !discPresent ? juce::String("No Hi-MD disc inserted")
                         : (!st.discTitle.empty() ? juce::String::fromUTF8(st.discTitle.c_str())
                                                  : juce::String("Untitled Hi-MD Disc"));
        g.setColour(textPrimary);
        g.setFont(juce::FontOptions(11.4f, juce::Font::bold));
        g.drawText(titleText, headerRow, juce::Justification::centredLeft, true);

        if (area.getHeight() >= 11) {
            area.removeFromTop(3);
            const auto barRect = area.removeFromTop(8);
            g.setColour(wellBg);
            g.fillRoundedRectangle(barRect.toFloat(), 3.0f);
            g.setColour(border);
            g.drawRoundedRectangle(barRect.toFloat().reduced(0.5f), 3.0f, 1.0f);

            if (discPresent && st.fsUsedBytes > 0) {
                const double frac =
                    juce::jlimit(0.0, 1.0,
                                 static_cast<double>(st.fsUsedBytes) /
                                     static_cast<double>(st.fsTotalBytes));
                const int fillW =
                    std::max(4, static_cast<int>(std::llround(barRect.getWidth() * frac)));
                g.setColour(stateCol.withAlpha(0.82f));
                g.fillRoundedRectangle(barRect.withWidth(fillW).reduced(1).toFloat(), 2.5f);
            }
            if (area.getHeight() >= 12) {
                area.removeFromTop(3);
                const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
                const juce::String sub =
                    !discPresent
                        ? juce::String("Insert a Hi-MD disc and click Refresh.")
                        : ("Used " + formatMegabytes(st.fsUsedBytes) + dot + "Free " +
                           formatMegabytes(st.fsFreeBytes) + dot + "Hi-MD FAT16");
                g.setColour(textMuted);
                g.setFont(juce::FontOptions(10.3f));
                g.drawText(sub, area, juce::Justification::centredLeft, true);
            }
        }
        return;
    }
    if (!recordUiState.hasDeviceStatus || recordUiState.deviceStatus.deviceId.empty()) {
        g.setColour(textMuted);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText("Connect a Sony NetMD / Hi-MD recorder via USB and click Refresh.", area,
                   juce::Justification::centredLeft, true);
        return;
    }

    const auto& st = recordUiState.deviceStatus;
    auto headerRow = area.removeFromTop(16);
    juce::String stateLabel;
    juce::Colour stateCol = dark ? juce::Colour(0xff8a8a8a) : juce::Colour(0xff666666);
    if (!st.discPresent) {
        stateLabel = "NO DISC";
    } else if (st.hiMdFormatted) {
        stateLabel = "Hi-MD Format";
        stateCol = dark ? juce::Colour(0xffcf872c) : juce::Colour(0xffb45309);
    } else if (st.writeProtected) {
        stateLabel = "LOCKED";
        stateCol = dark ? juce::Colour(0xffd9534f) : juce::Colour(0xffb91c1c);
    } else if (st.trackCount == 0) {
        stateLabel = "BLANK";
        stateCol = dark ? juce::Colour(0xff2d8f8b) : juce::Colour(0xff0f766e);
    } else {
        stateLabel = juce::String(st.trackCount) + (st.trackCount == 1 ? " TRK" : " TRK");
        stateCol = dark ? juce::Colour(0xff2d8f8b) : juce::Colour(0xff0f766e);
    }

    const int pillW = std::min(96, static_cast<int>(stateLabel.length()) * 7 + 16);
    const auto pillRect = headerRow.removeFromRight(pillW);
    g.setColour(stateCol.withAlpha(dark ? 0.28f : 0.16f));
    g.fillRoundedRectangle(pillRect.toFloat(), 3.0f);
    g.setColour(dark ? stateCol.brighter(0.35f) : stateCol);
    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
    g.drawText(stateLabel, pillRect, juce::Justification::centred, false);

    headerRow.removeFromRight(6);
    const juce::String titleText =
        !st.discPresent
            ? "No disc inserted"
            : (!st.discTitle.empty()
                   ? juce::String::fromUTF8(st.discTitle.c_str())
                   : (!st.fullWidthTitle.empty() ? juce::String::fromUTF8(st.fullWidthTitle.c_str())
                                                 : juce::String("Untitled Physical Disc")));
    g.setColour(textPrimary);
    g.setFont(juce::FontOptions(11.4f, juce::Font::bold));
    g.drawText(titleText, headerRow, juce::Justification::centredLeft, true);

    if (area.getHeight() >= 11) {
        area.removeFromTop(3);
        const auto barRect = area.removeFromTop(8);
        g.setColour(wellBg);
        g.fillRoundedRectangle(barRect.toFloat(), 3.0f);
        g.setColour(border);
        g.drawRoundedRectangle(barRect.toFloat().reduced(0.5f), 3.0f, 1.0f);

        const int totalSec =
            st.totalSeconds > 0 ? st.totalSeconds : (st.usedSeconds + st.leftSeconds);
        if (st.discPresent && st.hiMdFormatted) {
            g.setColour(stateCol.withAlpha(0.75f));
            g.fillRoundedRectangle(barRect.reduced(1).toFloat(), 2.5f);
        } else if (st.discPresent && totalSec > 0 && st.usedSeconds > 0) {
            const double frac = juce::jlimit(
                0.0, 1.0, static_cast<double>(st.usedSeconds) / static_cast<double>(totalSec));
            const int fillW =
                std::max(4, static_cast<int>(std::llround(barRect.getWidth() * frac)));
            g.setColour(stateCol.withAlpha(0.82f));
            g.fillRoundedRectangle(barRect.withWidth(fillW).reduced(1).toFloat(), 2.5f);
        }

        if (area.getHeight() >= 12) {
            area.removeFromTop(3);
            juce::String sub;
            if (!st.discPresent) {
                sub = "Insert a MiniDisc and click Refresh.";
            } else if (st.hiMdFormatted) {
                sub = "Hi-MD formatted (" + formatSecMmSs(st.usedSeconds) +
                      " used) · Erase Disc to format Standard MD";
            } else {
                const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
                sub = "Used " + formatSecMmSs(st.usedSeconds) + dot + "Free " +
                      formatSecMmSs(st.leftSeconds) + dot +
                      (st.writeProtected ? "Write-Protected" : "Writable MD");
            }
            g.setColour(textMuted);
            g.setFont(juce::FontOptions(10.3f));
            g.drawText(sub, area, juce::Justification::centredLeft, true);
        }
    }
}

void MdWorkspace::paintTargetCapacityBar(juce::Graphics& g, juce::Rectangle<int> bounds,
                                         bool dark) const {
    if (bounds.isEmpty() || bounds.getWidth() < 40 || bounds.getHeight() < 12) {
        return;
    }
    const auto textPrimary = dark ? juce::Colour(0xffe0e0e0) : juce::Colour(0xff262626);
    const auto textMuted = dark ? juce::Colour(0xff9e9e9e) : juce::Colour(0xff666666);
    const auto wellBg = dark ? juce::Colour(0xff1b1b1b) : juce::Colour(0xffe5e5e5);
    const auto border = dark ? juce::Colour(0xff3c3c3c) : juce::Colour(0xffc6c6c6);
    const bool isHiMd = carta::isHiMdMedium(discState.medium);
    const std::int64_t hiMdCap = std::max<std::int64_t>(
        1, hiMdTotalBytes > 0 ? hiMdTotalBytes : carta::hiMdMediumCapacityBytes(discState.medium));
    const bool over =
        isHiMd ? (hiMdUsedBytes > hiMdCap) : (!utocValid || !planValid || !budget.fits());

    g.setColour(wellBg);
    g.fillRoundedRectangle(bounds.toFloat(), 4.0f);

    if (isHiMd) {
        const auto inner = bounds.reduced(1);
        std::int64_t cursorBytes = 0;
        bool drewHiMdRows = false;
        int newSegIdx = 0;
        for (const auto& br : audioBankRows) {
            if (br.isReclaimed || br.hiMdBytes <= 0) {
                continue;
            }
            drewHiMdRows = true;
            const int x1 = inner.getX() +
                           static_cast<int>(std::llround((static_cast<double>(cursorBytes) /
                                                          static_cast<double>(hiMdCap)) *
                                                         inner.getWidth()));
            cursorBytes += br.hiMdBytes;
            const int x2Raw = inner.getX() +
                              static_cast<int>(std::llround((static_cast<double>(cursorBytes) /
                                                              static_cast<double>(hiMdCap)) *
                                                             inner.getWidth()));
            const int x2 = std::min(inner.getRight(), std::max(x1 + 3, x2Raw));
            if (x1 >= inner.getRight()) {
                break;
            }
            const auto segRect = juce::Rectangle<int>(x1, inner.getY(), x2 - x1, inner.getHeight());
            const auto col =
                over ? juce::Colour(0xffdc2626)
                     : (br.isOnDisc ? (dark ? juce::Colour(0xff0e7490) : juce::Colour(0xff0891b2))
                                    : carta::md_ui::codecColour(br.codec, dark));
            g.setColour(col.withAlpha(br.isOnDisc ? 0.75f : (dark ? 0.72f : 0.65f)));
            g.fillRect(segRect);
            if (x1 > inner.getX() && br.gappedBefore) {
                g.setColour(dark ? juce::Colour(0xfff59e0b) : juce::Colour(0xffb45309));
                g.fillRect(x1 - 1, inner.getY(), 2, inner.getHeight());
            }
            if (x2 < inner.getRight()) {
                g.setColour(wellBg.withAlpha(0.85f));
                g.fillRect(x2 - 1, inner.getY(), 1, inner.getHeight());
            }
            if (br.isOnDisc) {
                if (segRect.getWidth() >= 54) {
                    g.setColour(juce::Colours::white);
                    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                    g.drawText("ON-DISC", segRect.reduced(3, 0), juce::Justification::centred,
                               true);
                }
            } else {
                ++newSegIdx;
                if (segRect.getWidth() >= 48) {
                    g.setColour(dark ? juce::Colour(0xfff5f5f5) : juce::Colour(0xffffffff));
                    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                    g.drawText("A" + juce::String(newSegIdx) + " " +
                                   carta::md_ui::codecShortLabel(br.codec),
                               segRect.reduced(3, 0), juce::Justification::centred, true);
                }
            }
        }
        if (!drewHiMdRows) {
            for (std::size_t i = 0; i < physicalPlan.spans.size(); ++i) {
                const auto& sp = physicalPlan.spans[i];
                const auto codec = sp.codec;
                const auto trackBytes =
                    carta::estimateHiMdTrackBytes(codec, sp.endSample - sp.startSample);
                if (trackBytes <= 0) {
                    continue;
                }
                const int x1 = inner.getX() +
                               static_cast<int>(std::llround((static_cast<double>(cursorBytes) /
                                                              static_cast<double>(hiMdCap)) *
                                                             inner.getWidth()));
                cursorBytes += trackBytes;
                const int x2Raw = inner.getX() +
                                  static_cast<int>(std::llround((static_cast<double>(cursorBytes) /
                                                                  static_cast<double>(hiMdCap)) *
                                                                inner.getWidth()));
                const int x2 = std::min(inner.getRight(), std::max(x1 + 3, x2Raw));
                if (x1 >= inner.getRight()) {
                    break;
                }
                const auto segRect =
                    juce::Rectangle<int>(x1, inner.getY(), x2 - x1, inner.getHeight());
                const auto col =
                    over ? juce::Colour(0xffdc2626) : carta::md_ui::codecColour(codec, dark);
                g.setColour(col.withAlpha(dark ? 0.72f : 0.65f));
                g.fillRect(segRect);
                if (x2 < inner.getRight()) {
                    g.setColour(wellBg.withAlpha(0.85f));
                    g.fillRect(x2 - 1, inner.getY(), 1, inner.getHeight());
                }
                if (segRect.getWidth() >= 48) {
                    g.setColour(dark ? juce::Colour(0xfff5f5f5) : juce::Colour(0xffffffff));
                    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                    g.drawText("A" + juce::String(static_cast<int>(i + 1)) + " " +
                                   carta::md_ui::codecShortLabel(codec),
                               segRect.reduced(3, 0), juce::Justification::centred, true);
                }
            }
        }
        g.setColour(over ? juce::Colour(0xffdc2626) : border);
        g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 4.0f, 1.0f);

        const int pct = static_cast<int>(std::llround(
            (static_cast<double>(hiMdUsedBytes) * 100.0) / static_cast<double>(hiMdCap)));
        const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
        const auto capText =
            formatMegabytes(hiMdUsedBytes) + " / " + formatMegabytes(hiMdCap) + " (" +
            juce::String(pct) + "%)" +
            (discState.tracks.empty()
                 ? juce::String()
                 : (dot + juce::String(physicalPlan.uniqueAudioTrackCount) + " uniq / " +
                    juce::String(physicalPlan.playOrderTrackCount) + " trk"));
        g.setFont(juce::FontOptions(10.2f, juce::Font::bold));
        const int labelW =
            std::min(bounds.getWidth() - 12, static_cast<int>(capText.length()) * 6 + 16);
        const auto pill = juce::Rectangle<int>(bounds.getRight() - labelW - 4, bounds.getY() + 3,
                                               labelW, bounds.getHeight() - 6);
        g.setColour((dark ? juce::Colour(0xff1e1e1e) : juce::Colour(0xfff7f7f7)).withAlpha(0.84f));
        g.fillRoundedRectangle(pill.toFloat(), 3.0f);
        g.setColour(over ? (dark ? juce::Colour(0xfffca5a5) : juce::Colour(0xffb91c1c))
                         : textMuted.interpolatedWith(textPrimary, 0.7f));
        g.drawText(capText, pill.reduced(4, 0), juce::Justification::centredRight, true);
        return;
    }

    const int totalClu =
        std::max(1, discState.loadedDisc.active ? discState.loadedDisc.physicalTotalClusters()
                                                : carta::mdTotalClusters(discState.medium));
    const auto inner = bounds.reduced(1);
    int cursorClu = 0;
    std::int64_t totalOnDiscUnits = 0;
    std::int64_t totalNewUnits = 0;
    for (const auto& br : audioBankRows) {
        if (br.isReclaimed || br.unitCount <= 0) {
            continue;
        }
        if (br.isOnDisc) {
            totalOnDiscUnits += br.unitCount;
        } else {
            totalNewUnits += br.unitCount;
        }
    }
    if (discState.loadedDisc.active && physicalPlan.onDiscClusters > 0) {
        if (totalOnDiscUnits > 0) {
            double unitCursor = 0.0;
            for (const auto& br : audioBankRows) {
                if (!br.isOnDisc || br.isReclaimed || br.unitCount <= 0) {
                    continue;
                }
                const double startCluD = (unitCursor / static_cast<double>(totalOnDiscUnits)) *
                                         static_cast<double>(physicalPlan.onDiscClusters);
                unitCursor += static_cast<double>(br.unitCount);
                const double endCluD = (unitCursor / static_cast<double>(totalOnDiscUnits)) *
                                       static_cast<double>(physicalPlan.onDiscClusters);
                const int x1 = inner.getX() +
                               static_cast<int>(std::llround(
                                   (startCluD / static_cast<double>(totalClu)) * inner.getWidth()));
                const int x2Raw = inner.getX() +
                                  static_cast<int>(std::llround(
                                      (endCluD / static_cast<double>(totalClu)) * inner.getWidth()));
                const int x2 = std::min(inner.getRight(), std::max(x1 + 3, x2Raw));
                if (x1 >= inner.getRight()) {
                    break;
                }
                const auto onDiscRect =
                    juce::Rectangle<int>(x1, inner.getY(), x2 - x1, inner.getHeight());
                g.setColour((over ? juce::Colour(0xffdc2626)
                                  : (dark ? juce::Colour(0xff0e7490) : juce::Colour(0xff0891b2)))
                                .withAlpha(0.75f));
                g.fillRect(onDiscRect);
                if (x1 > inner.getX() && br.gappedBefore) {
                    g.setColour(dark ? juce::Colour(0xfff59e0b) : juce::Colour(0xffb45309));
                    g.fillRect(x1 - 1, inner.getY(), 2, inner.getHeight());
                }
                if (x2 < inner.getRight()) {
                    g.setColour(wellBg.withAlpha(0.85f));
                    g.fillRect(x2 - 1, inner.getY(), 1, inner.getHeight());
                }
                if (onDiscRect.getWidth() >= 54) {
                    g.setColour(juce::Colours::white);
                    g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                    g.drawText("ON-DISC", onDiscRect.reduced(3, 0), juce::Justification::centred,
                               true);
                }
            }
        } else {
            const int x1 = inner.getX();
            const int x2 = std::min(
                inner.getRight(),
                std::max(x1 + 3,
                         inner.getX() + static_cast<int>(std::llround(
                                            (static_cast<double>(physicalPlan.onDiscClusters) /
                                             static_cast<double>(totalClu)) *
                                            inner.getWidth()))));
            const auto onDiscRect =
                juce::Rectangle<int>(x1, inner.getY(), x2 - x1, inner.getHeight());
            g.setColour((over ? juce::Colour(0xffdc2626)
                              : (dark ? juce::Colour(0xff0e7490) : juce::Colour(0xff0891b2)))
                            .withAlpha(0.75f));
            g.fillRect(onDiscRect);
            if (x2 < inner.getRight()) {
                g.setColour(wellBg.withAlpha(0.85f));
                g.fillRect(x2 - 1, inner.getY(), 1, inner.getHeight());
            }
            if (onDiscRect.getWidth() >= 54) {
                g.setColour(juce::Colours::white);
                g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                g.drawText("ON-DISC", onDiscRect.reduced(3, 0), juce::Justification::centred, true);
            }
        }
        cursorClu = physicalPlan.onDiscClusters;
    }
    const int newCluBudget =
        physicalPlan.newUploadClusters > 0
            ? physicalPlan.newUploadClusters
            : (discState.loadedDisc.active ? 0 : physicalPlan.usedClusters);
    if (totalNewUnits > 0 && newCluBudget > 0) {
        double newUnitCursor = 0.0;
        int newSegIdx = 0;
        for (const auto& br : audioBankRows) {
            if (br.isOnDisc || br.isReclaimed || br.unitCount <= 0) {
                continue;
            }
            ++newSegIdx;
            const double startCluD =
                static_cast<double>(cursorClu) +
                (newUnitCursor / static_cast<double>(totalNewUnits)) *
                    static_cast<double>(newCluBudget);
            newUnitCursor += static_cast<double>(br.unitCount);
            const double endCluD =
                static_cast<double>(cursorClu) +
                (newUnitCursor / static_cast<double>(totalNewUnits)) *
                    static_cast<double>(newCluBudget);
            const int x1 = inner.getX() +
                           static_cast<int>(std::llround(
                               (startCluD / static_cast<double>(totalClu)) * inner.getWidth()));
            const int x2Raw = inner.getX() +
                              static_cast<int>(std::llround(
                                  (endCluD / static_cast<double>(totalClu)) * inner.getWidth()));
            const int x2 = std::min(inner.getRight(), std::max(x1 + 3, x2Raw));
            if (x1 >= inner.getRight()) {
                break;
            }
            const auto segRect = juce::Rectangle<int>(x1, inner.getY(), x2 - x1, inner.getHeight());
            const auto col =
                over ? juce::Colour(0xffdc2626) : carta::md_ui::codecColour(br.codec, dark);
            g.setColour(col.withAlpha(dark ? 0.72f : 0.65f));
            g.fillRect(segRect);
            if (x1 > inner.getX() && br.gappedBefore) {
                g.setColour(dark ? juce::Colour(0xfff59e0b) : juce::Colour(0xffb45309));
                g.fillRect(x1 - 1, inner.getY(), 2, inner.getHeight());
            }
            if (x2 < inner.getRight()) {
                g.setColour(wellBg.withAlpha(0.85f));
                g.fillRect(x2 - 1, inner.getY(), 1, inner.getHeight());
            }
            if (segRect.getWidth() >= 48) {
                g.setColour(dark ? juce::Colour(0xfff5f5f5) : juce::Colour(0xffffffff));
                g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                g.drawText("A" + juce::String(newSegIdx) + " " +
                               carta::md_ui::codecShortLabel(br.codec),
                           segRect.reduced(3, 0), juce::Justification::centred, true);
            }
        }
    } else {
        for (std::size_t i = 0; i < physicalPlan.spans.size(); ++i) {
            const auto& sp = physicalPlan.spans[i];
            if (sp.clusterCount <= 0) {
                continue;
            }
            const int startClu = discState.loadedDisc.active
                                     ? cursorClu
                                     : std::max<int>(0, sp.startCluster - carta::kMdFirstAudioCluster);
            cursorClu += sp.clusterCount;
            const int x1 = inner.getX() +
                           static_cast<int>(std::llround((static_cast<double>(startClu) /
                                                          static_cast<double>(totalClu)) *
                                                         inner.getWidth()));
            const int x2Raw = inner.getX() +
                              static_cast<int>(std::llround(
                                  (static_cast<double>(startClu + sp.clusterCount) /
                                   static_cast<double>(totalClu)) *
                                  inner.getWidth()));
            const int x2 = std::min(inner.getRight(), std::max(x1 + 3, x2Raw));
            if (x1 >= inner.getRight()) {
                break;
            }
            const auto segRect = juce::Rectangle<int>(x1, inner.getY(), x2 - x1, inner.getHeight());
            const auto col =
                over ? juce::Colour(0xffdc2626) : carta::md_ui::codecColour(sp.codec, dark);
            g.setColour(col.withAlpha(dark ? 0.72f : 0.65f));
            g.fillRect(segRect);
            if (x2 < inner.getRight()) {
                g.setColour(wellBg.withAlpha(0.85f));
                g.fillRect(x2 - 1, inner.getY(), 1, inner.getHeight());
            }
            if (segRect.getWidth() >= 48) {
                g.setColour(dark ? juce::Colour(0xfff5f5f5) : juce::Colour(0xffffffff));
                g.setFont(juce::FontOptions(10.0f, juce::Font::bold));
                g.drawText("A" + juce::String(static_cast<int>(i + 1)) + " " +
                               carta::md_ui::codecShortLabel(sp.codec),
                           segRect.reduced(3, 0), juce::Justification::centred, true);
            }
        }
    }

    g.setColour(over ? juce::Colour(0xffdc2626) : border);
    g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 4.0f, 1.0f);

    const int pct = static_cast<int>(std::llround(
        (static_cast<double>(physicalPlan.usedClusters) * 100.0) / static_cast<double>(totalClu)));
    const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
    const auto capText =
        juce::String(physicalPlan.usedClusters) + "/" + juce::String(totalClu) +
        " clusters (" + juce::String(pct) + "%)" +
        (discState.tracks.empty()
             ? juce::String()
             : (dot + juce::String(physicalPlan.uniqueAudioTrackCount) + " uniq / " +
                juce::String(physicalPlan.playOrderTrackCount) + " trk")) +
        ((discState.loadedDisc.active && physicalPlan.reclaimedClusters > 0)
             ? (dot + "reclaim " + juce::String(physicalPlan.reclaimedClusters))
             : juce::String());
    g.setColour(textPrimary);
    g.setFont(juce::FontOptions(10.2f, juce::Font::bold));
    const int labelW =
        std::min(bounds.getWidth() - 12, static_cast<int>(capText.length()) * 6 + 16);
    const auto pill = juce::Rectangle<int>(bounds.getRight() - labelW - 4, bounds.getY() + 3,
                                           labelW, bounds.getHeight() - 6);
    g.setColour((dark ? juce::Colour(0xff1e1e1e) : juce::Colour(0xfff7f7f7)).withAlpha(0.84f));
    g.fillRoundedRectangle(pill.toFloat(), 3.0f);
    g.setColour(over ? (dark ? juce::Colour(0xfffca5a5) : juce::Colour(0xffb91c1c))
                     : textMuted.interpolatedWith(textPrimary, 0.7f));
    g.drawText(capText, pill.reduced(4, 0), juce::Justification::centredRight, true);
}

void MdWorkspace::paintPlanProgressBar(juce::Graphics& g, juce::Rectangle<int> bounds,
                                       bool dark) const {
    if (bounds.isEmpty() || bounds.getWidth() < 40) {
        return;
    }
    const auto wellBg = dark ? juce::Colour(0xff1b1b1b) : juce::Colour(0xffe4e4e4);
    const auto border = dark ? juce::Colour(0xff4a4a4a) : juce::Colour(0xffb8b8b8);
    const auto accent = dark ? juce::Colour(0xff2d8f8b) : juce::Colour(0xff0f766e);
    const double frac = juce::jlimit(0.0, 1.0, recordUiState.progress.progress);

    g.setColour(wellBg);
    g.fillRoundedRectangle(bounds.toFloat(), 4.0f);
    if (frac > 0.0) {
        const int fillW = std::max(4, static_cast<int>(std::llround(bounds.getWidth() * frac)));
        g.setColour(accent.withAlpha(dark ? 0.65f : 0.35f));
        g.fillRoundedRectangle(bounds.withWidth(fillW).toFloat(), 4.0f);
    }
    g.setColour(border);
    g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 4.0f, 1.0f);

    juce::String label = recordUiState.progress.phase.empty()
                             ? juce::String("Working...")
                             : juce::String::fromUTF8(recordUiState.progress.phase.c_str());
    label += " (" + juce::String(static_cast<int>(std::llround(frac * 100.0))) + "%)";
    if (recordUiState.progress.bytesTotal > 0) {
        label +=
            " " + juce::String(static_cast<juce::int64>(recordUiState.progress.bytesDone / 1024)) +
            "/" + juce::String(static_cast<juce::int64>(recordUiState.progress.bytesTotal / 1024)) +
            " KB";
    }
    g.setColour(dark ? juce::Colour(0xffeaeaea) : juce::Colour(0xff1f1f1f));
    g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
    g.drawText(label, bounds.reduced(8, 0), juce::Justification::centredLeft, true);
}

void MdWorkspace::paintPlanRow(int row, juce::Graphics& g, int width, int height,
                               bool selected) const {
    if (row < 0 || row >= static_cast<int>(planRows.size())) {
        return;
    }
    const auto& pr = planRows[static_cast<std::size_t>(row)];
    const bool dark = juce::Desktop::getInstance().isDarkModeActive();
    const auto textPrimary = dark ? juce::Colour(0xffe0e0e0) : juce::Colour(0xff262626);
    const auto textMuted = dark ? juce::Colour(0xff9e9e9e) : juce::Colour(0xff666666);
    paintAlternatingRowBackground(g, row, width, height, selected, dark);

    if (pr.groupColorIndex >= 0) {
        g.setColour(carta::md_ui::groupColour(pr.groupColorIndex, dark));
        g.fillRect(0, 2, 3, height - 4);
    }

    g.setColour(textMuted);
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText(juce::String::formatted("%02d", pr.trackIndex + 1), 8, 0, 24, height,
               juce::Justification::centredLeft, false);

    const auto codecCol = carta::md_ui::codecColour(pr.codec, dark);
    const auto codecRect = juce::Rectangle<int>(34, 4, 46, height - 8);
    g.setColour(codecCol.withAlpha(dark ? 0.28f : 0.14f));
    g.fillRoundedRectangle(codecRect.toFloat(), 3.0f);
    g.setColour(dark ? codecCol.brighter(0.35f) : codecCol);
    g.setFont(juce::FontOptions(9.8f, juce::Font::bold));
    g.drawText(carta::md_ui::codecShortLabel(pr.codec), codecRect, juce::Justification::centred,
               false);

    const int physW = std::min(240, std::max(130, (width * 40) / 100));
    const int durW = 54;
    const int titleX = 86;
    const int titleW = std::max(40, width - titleX - physW - durW - 12);

    juce::String titleText = pr.title;
    if (!pr.groupName.empty()) {
        titleText += "  [" + juce::String::fromUTF8(pr.groupName.c_str()) + "]";
    }
    g.setColour(textPrimary);
    g.setFont(juce::FontOptions(11.6f, selected ? juce::Font::bold : juce::Font::plain));
    g.drawText(titleText, titleX, 0, titleW, height, juce::Justification::centredLeft, true);

    g.setColour(textMuted);
    g.setFont(juce::FontOptions(11.0f));
    g.drawText(carta::md_ui::formatClock(pr.totalSamples), width - physW - durW - 8, 0, durW,
               height, juce::Justification::centredRight, false);

    juce::String physText;
    const auto midDot = juce::String::fromUTF8(" \xc2\xb7 ");
    const juce::String gapPrefix = pr.forceGapBefore ? ("GAP" + midDot) : juce::String();
    const juce::String sharedTag =
        pr.spanRefCount > 1 ? (" (x" + juce::String(pr.spanRefCount) + ")") : juce::String();
    if (carta::isHiMdMedium(discState.medium)) {
        const auto estBytes = carta::estimateHiMdTrackBytes(pr.codec, pr.totalSamples);
        const auto baseStr =
            (pr.onDisc ? juce::String("DISC ") : (discState.loadedDisc.active ? juce::String("NEW ")
                                                                              : juce::String())) +
            formatMegabytes(estBytes);
        if (pr.aliasRefCount > 1) {
            physText = gapPrefix + "Alias -> Pool #" + juce::String(pr.uniqueAudioSlot) + " (" +
                       baseStr + ") x" + juce::String(pr.aliasRefCount);
        } else {
            physText = gapPrefix + "Pool #" + juce::String(pr.uniqueAudioSlot) + midDot + baseStr;
        }
    } else if (pr.partHopsText.isNotEmpty()) {
        const juce::String tagPrefix =
            (pr.onDisc && pr.primarySpanIndex >= 0)
                ? "MIX "
                : (pr.onDisc ? "DISC " : (discState.loadedDisc.active ? "NEW " : ""));
        physText = gapPrefix + tagPrefix + pr.partHopsText + sharedTag;
    } else if (pr.onDisc || pr.primarySpanIndex >= 0) {
        const auto range = compactClusterLabel(pr.spanClusterStart, pr.spanClusterEnd);
        const juce::String tagPrefix =
            pr.onDisc ? "DISC " : (discState.loadedDisc.active ? "NEW " : "");
        const int refCount = std::max(pr.aliasRefCount, pr.spanRefCount);
        if (pr.aliasRefCount > 1 || pr.sharesPhysicalSpan) {
            physText = gapPrefix + "Alias -> Pool #" + juce::String(pr.uniqueAudioSlot) + " (" +
                       tagPrefix + range + ") x" + juce::String(refCount);
        } else {
            physText = gapPrefix + "Pool #" + juce::String(pr.uniqueAudioSlot) + midDot +
                       tagPrefix + range;
        }
    } else {
        physText = "(unplanned)";
    }
    const auto teal = dark ? juce::Colour(0xff5eead4) : juce::Colour(0xff0f766e);
    g.setColour((pr.sharesPhysicalSpan || pr.onDisc) ? teal : textMuted);
    g.setFont(juce::FontOptions(
        10.6f, (pr.sharesPhysicalSpan || pr.onDisc) ? juce::Font::bold : juce::Font::plain));
    g.drawText(physText, width - physW - 6, 0, physW, height, juce::Justification::centredRight,
               true);
}

juce::String MdWorkspace::tooltipForPlanRow(int row) const {
    if (row < 0 || row >= static_cast<int>(planRows.size())) {
        return {};
    }
    const auto& pr = planRows[static_cast<std::size_t>(row)];
    juce::String tip =
        "Track " + juce::String(pr.trackIndex + 1) + ": " + pr.title +
        juce::String::fromUTF8(" \xc2\xb7 ") + carta::md_ui::codecLongLabel(pr.codec) +
        juce::String::fromUTF8(" \xc2\xb7 ") + carta::md_ui::formatClock(pr.totalSamples) + " (" +
        juce::String(pr.partCount) + (pr.partCount == 1 ? " part)" : " parts)");
    if (carta::isHiMdMedium(discState.medium)) {
        const auto estBytes = carta::estimateHiMdTrackBytes(pr.codec, pr.totalSamples);
        tip += juce::String::fromUTF8(" \xc2\xb7 Est. Hi-MD size: ") + formatMegabytes(estBytes);
    } else if (pr.onDisc && pr.primarySpanIndex < 0) {
        tip += juce::String::fromUTF8(" \xc2\xb7 Preserved On-Disc Audio (Clusters ") +
               juce::String(pr.spanClusterStart) + ".." + juce::String(pr.spanClusterEnd) + ")";
    } else if (pr.primarySpanIndex >= 0) {
        tip += juce::String::fromUTF8(" \xc2\xb7 ") +
               (discState.loadedDisc.active ? "New Upload Span #" : "Physical Span #") +
               juce::String(pr.primarySpanIndex + 1) + " (Clusters " +
               juce::String(pr.spanClusterStart) + ".." + juce::String(pr.spanClusterEnd) + ")";
    }
    if (pr.partHopsText.isNotEmpty()) {
        tip += juce::String::fromUTF8(" \xc2\xb7 Clusters: ") + pr.partHopsText;
    }
    if (pr.spanRefCount > 1) {
        tip += juce::String::fromUTF8(" \xc2\xb7 Shared across ") +
               juce::String(pr.spanRefCount) + " part references";
    }
    return tip;
}

void MdWorkspace::paintAudioBankRow(int row, juce::Graphics& g, int width, int height,
                                    bool selected) const {
    if (row < 0 || row >= static_cast<int>(audioBankRows.size())) {
        return;
    }
    const auto& br = audioBankRows[static_cast<std::size_t>(row)];
    const bool dark = juce::Desktop::getInstance().isDarkModeActive();
    const auto textPrimary = dark ? juce::Colour(0xffe0e0e0) : juce::Colour(0xff262626);
    const auto textMuted = dark ? juce::Colour(0xff9e9e9e) : juce::Colour(0xff666666);
    paintAlternatingRowBackground(g, row, width, height, selected, dark);

    // Origin / Reclaim pill
    const auto statusCol =
        br.isReclaimed
            ? (dark ? juce::Colour(0xfff59e0b) : juce::Colour(0xffb45309))
            : (br.isOnDisc ? (dark ? juce::Colour(0xff5eead4) : juce::Colour(0xff0f766e))
                           : (dark ? juce::Colour(0xff60a5fa) : juce::Colour(0xff2563eb)));
    const auto statusRect = juce::Rectangle<int>(6, 4, 58, height - 8);
    g.setColour(statusCol.withAlpha(dark ? 0.24f : 0.14f));
    g.fillRoundedRectangle(statusRect.toFloat(), 3.0f);
    g.setColour(statusCol);
    g.setFont(juce::FontOptions(9.5f, juce::Font::bold));
    g.drawText(br.isReclaimed ? "RECLAIM" : (br.isOnDisc ? "ON-DISC" : "NEW"), statusRect,
               juce::Justification::centred, false);

    // Codec badge
    const auto codecCol = carta::md_ui::codecColour(br.codec, dark);
    const auto codecRect = juce::Rectangle<int>(68, 4, 46, height - 8);
    g.setColour(codecCol.withAlpha(dark ? 0.28f : 0.14f));
    g.fillRoundedRectangle(codecRect.toFloat(), 3.0f);
    g.setColour(dark ? codecCol.brighter(0.35f) : codecCol);
    g.setFont(juce::FontOptions(9.8f, juce::Font::bold));
    g.drawText(carta::md_ui::codecShortLabel(br.codec), codecRect, juce::Justification::centred,
               false);

    const int refsW = std::min(155, std::max(88, (width * 30) / 100));
    const int cluW = 86;
    const int labelX = 118;
    const int labelW = std::max(36, width - labelX - refsW - cluW - 18);

    g.setColour(textPrimary);
    g.setFont(juce::FontOptions(11.3f, selected ? juce::Font::bold : juce::Font::plain));
    g.drawText(br.label, labelX, 0, labelW, height, juce::Justification::centredLeft, true);

    const auto cluStr =
        carta::isHiMdMedium(discState.medium)
            ? formatMegabytes(br.hiMdBytes)
            : (br.startCluster > 0
                   ? (compactClusterLabel(br.startCluster, br.endCluster) + " (" +
                      juce::String(br.clusterCount) + " cl)")
                   : juce::String("(unplanned)"));
    g.setColour(textMuted);
    g.setFont(juce::FontOptions(10.4f));
    g.drawText(cluStr, width - refsW - cluW - 12, 0, cluW, height,
               juce::Justification::centredRight, true);

    juce::String refsStr;
    if (br.isReclaimed) {
        refsStr = "reclaims " + juce::String(br.clusterCount) + " cl";
    } else {
        const int nTrk = static_cast<int>(br.referencingTrackIndices0.size());
        refsStr = juce::String(nTrk) + " trk";
        if (br.totalPartRefs > nTrk) {
            refsStr += "/" + juce::String(br.totalPartRefs) + "p";
        }
        refsStr += ": ";
        const int shown = std::min(3, nTrk);
        for (int k = 0; k < shown; ++k) {
            if (k > 0) {
                refsStr += ", ";
            }
            refsStr += "T" + juce::String(br.referencingTrackIndices0[static_cast<std::size_t>(k)] +
                                          1);
        }
        if (nTrk > shown) {
            refsStr += juce::String::fromUTF8(", \xe2\x80\xa6");
        }
    }
    const auto refsCol = br.isReclaimed
                             ? (dark ? juce::Colour(0xfffbbf24) : juce::Colour(0xffb45309))
                             : (br.totalPartRefs > 1 ? statusCol : textMuted);
    g.setColour(refsCol);
    g.setFont(juce::FontOptions(10.4f, (br.totalPartRefs > 1 || br.isReclaimed) ? juce::Font::bold
                                                                               : juce::Font::plain));
    g.drawText(refsStr, width - refsW - 6, 0, refsW, height, juce::Justification::centredRight,
               true);
}

juce::String MdWorkspace::tooltipForAudioBankRow(int row) const {
    if (row < 0 || row >= static_cast<int>(audioBankRows.size())) {
        return {};
    }
    const auto& br = audioBankRows[static_cast<std::size_t>(row)];
    juce::String tip = br.isOnDisc ? ("On-Disc Region #" + juce::String(br.onDiscRegionIndex + 1))
                                   : ("Audio Pool Item #" + juce::String(br.spanIndex + 1));
    if (carta::isHiMdMedium(discState.medium)) {
        tip += ": " + br.label + juce::String::fromUTF8(" \xc2\xb7 ") +
               carta::md_ui::codecLongLabel(br.codec) + juce::String::fromUTF8(" \xc2\xb7 ") +
               formatMegabytes(br.hiMdBytes);
    } else {
        tip += ": " + br.label + juce::String::fromUTF8(" \xc2\xb7 ") +
               carta::md_ui::codecLongLabel(br.codec) +
               juce::String::fromUTF8(" \xc2\xb7 Clusters ") + juce::String(br.startCluster) +
               ".." + juce::String(br.endCluster) + " (" + juce::String(br.clusterCount) +
               (br.clusterCount == 1 ? " cluster)" : " clusters)");
    }
    if (br.timelineEndSample > br.timelineStartSample) {
        tip += juce::String::fromUTF8(" \xc2\xb7 ") +
               carta::md_ui::formatClock(br.timelineEndSample - br.timelineStartSample);
    }
    if (br.forceGapBefore) {
        tip += juce::String::fromUTF8(" \xc2\xb7 Forced gapped break before");
    } else if (br.gappedBefore) {
        tip += juce::String::fromUTF8(" \xc2\xb7 Gapped break before (codec/on-disc boundary)");
    }
    if (br.isReclaimed) {
        tip += juce::String::fromUTF8(" \xc2\xb7 Unreferenced — reclaims ") +
               juce::String(br.clusterCount) + " clusters on Write UTOC / Record";
    } else {
        juce::StringArray trNames;
        const int cap = std::min(12, static_cast<int>(br.referencingTrackIndices0.size()));
        for (int k = 0; k < cap; ++k) {
            const int trIdx = br.referencingTrackIndices0[static_cast<std::size_t>(k)];
            if (trIdx >= 0 && trIdx < static_cast<int>(trackRows.size())) {
                trNames.add("T" + juce::String(trIdx + 1) + " " +
                            trackRows[static_cast<std::size_t>(trIdx)].title);
            }
        }
        tip += juce::String::fromUTF8(" \xc2\xb7 Referenced by ") +
               juce::String(static_cast<int>(br.referencingTrackIndices0.size())) + " track(s) (" +
               juce::String(br.totalPartRefs) + " part refs): " + trNames.joinIntoString(", ");
        if (static_cast<int>(br.referencingTrackIndices0.size()) > cap) {
            tip += " ... +" +
                   juce::String(static_cast<int>(br.referencingTrackIndices0.size()) - cap) +
                   " more";
        }
    }
    return tip;
}

void MdWorkspace::paintDeviceTrackRow(int row, juce::Graphics& g, int width, int height,
                                      bool selected) const {
    const bool dark = juce::Desktop::getInstance().isDarkModeActive();
    const auto textPrimary = dark ? juce::Colour(0xffe0e0e0) : juce::Colour(0xff262626);
    const auto textMuted = dark ? juce::Colour(0xff9e9e9e) : juce::Colour(0xff666666);

    if (recordUiState.activeDeviceIsHiMd) {
        if (!recordUiState.hasHiMdStatus) {
            return;
        }
        const auto& tracks = recordUiState.hiMdStatus.tracks;
        if (row < 0 || row >= static_cast<int>(tracks.size())) {
            return;
        }
        const auto& tr = tracks[static_cast<std::size_t>(row)];
        paintAlternatingRowBackground(g, row, width, height, selected, dark);

        g.setColour(textMuted);
        g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
        g.drawText(juce::String::formatted("%02d", tr.index + 1), 6, 0, 22, height,
                   juce::Justification::centredLeft, false);

        const int durW = 42;
        const int codecW = 44;
        const int rightX = width - 6;
        g.setColour(textPrimary);
        g.setFont(juce::FontOptions(10.5f));
        g.drawText(formatSecMmSs(static_cast<int>(std::llround(tr.durationSeconds))), rightX - durW,
                   0, durW, height, juce::Justification::centredRight, false);

        const juce::String codecLabel =
            tr.encoding.empty() ? juce::String("Hi-SP")
                                : juce::String::fromUTF8(tr.encoding.c_str());
        carta::MdCodecMode codecMode = carta::MdCodecMode::hiSp;
        if (codecLabel.equalsIgnoreCase("PCM") || codecLabel.equalsIgnoreCase("LPCM")) {
            codecMode = carta::MdCodecMode::pcm;
        } else if (codecLabel.equalsIgnoreCase("Hi-LP") || codecLabel.equalsIgnoreCase("HILP")) {
            codecMode = carta::MdCodecMode::hiLp;
        } else if (codecLabel.equalsIgnoreCase("LP2")) {
            codecMode = carta::MdCodecMode::lp2;
        } else if (codecLabel.equalsIgnoreCase("LP4")) {
            codecMode = carta::MdCodecMode::lp4;
        }
        const auto codecCol = carta::md_ui::codecColour(codecMode, dark);
        const auto codecRect =
            juce::Rectangle<int>(rightX - durW - 4 - codecW, 3, codecW, height - 6);
        g.setColour(codecCol.withAlpha(dark ? 0.28f : 0.14f));
        g.fillRoundedRectangle(codecRect.toFloat(), 3.0f);
        g.setColour(dark ? codecCol.brighter(0.35f) : codecCol);
        g.setFont(juce::FontOptions(9.5f, juce::Font::bold));
        g.drawText(codecLabel, codecRect, juce::Justification::centred, false);

        const juce::String titleText =
            !tr.title.empty() ? juce::String::fromUTF8(tr.title.c_str())
                              : ("Track " + juce::String(tr.index + 1));
        const int titleX = 30;
        const int titleW = std::max(24, codecRect.getX() - titleX - 4);
        const juce::Font titleFont(juce::FontOptions(11.0f));
        g.setFont(titleFont);
        if (!tr.artist.empty()) {
            const juce::String artistSuffix =
                juce::String::fromUTF8(" \xe2\x80\x94 ") +
                juce::String::fromUTF8(tr.artist.c_str());
            juce::GlyphArrangement ga;
            ga.addLineOfText(titleFont, titleText, 0.0f, 0.0f);
            const int titlePx =
                static_cast<int>(std::ceil(ga.getBoundingBox(0, -1, false).getWidth())) + 2;
            const int drawTitleW = std::min(titleW, titlePx);
            g.setColour(textPrimary);
            g.drawText(titleText, titleX, 0, drawTitleW, height, juce::Justification::centredLeft,
                       true);
            const int remW = titleW - drawTitleW;
            if (remW >= 28) {
                g.setColour(textMuted);
                g.setFont(juce::FontOptions(10.3f));
                g.drawText(artistSuffix, titleX + drawTitleW, 0, remW, height,
                           juce::Justification::centredLeft, true);
            }
        } else {
            g.setColour(textPrimary);
            g.drawText(titleText, titleX, 0, titleW, height, juce::Justification::centredLeft, true);
        }
        return;
    }

    const auto& tracks = recordUiState.deviceStatus.tracks;
    if (row < 0 || row >= static_cast<int>(tracks.size())) {
        return;
    }
    const auto& tr = tracks[static_cast<std::size_t>(row)];
    paintAlternatingRowBackground(g, row, width, height, selected, dark);

    g.setColour(textMuted);
    g.setFont(juce::FontOptions(10.5f, juce::Font::bold));
    g.drawText(juce::String::formatted("%02d", tr.index + 1), 6, 0, 22, height,
               juce::Justification::centredLeft, false);

    const int durW = 42;
    const int codecW = 38;
    const int rightX = width - 6;
    g.setColour(textPrimary);
    g.setFont(juce::FontOptions(10.5f));
    g.drawText(formatSecMmSs(tr.durationSeconds), rightX - durW, 0, durW, height,
               juce::Justification::centredRight, false);

    const juce::String codecLabel =
        tr.codec.empty() ? juce::String("SP") : juce::String::fromUTF8(tr.codec.c_str());
    carta::MdCodecMode codecMode = carta::MdCodecMode::spStereo;
    if (codecLabel.equalsIgnoreCase("LP2")) {
        codecMode = carta::MdCodecMode::lp2;
    } else if (codecLabel.equalsIgnoreCase("LP4")) {
        codecMode = carta::MdCodecMode::lp4;
    } else if (codecLabel.equalsIgnoreCase("Mono")) {
        codecMode = carta::MdCodecMode::spMono;
    }
    const auto codecCol = carta::md_ui::codecColour(codecMode, dark);
    const auto codecRect = juce::Rectangle<int>(rightX - durW - 4 - codecW, 3, codecW, height - 6);
    g.setColour(codecCol.withAlpha(dark ? 0.28f : 0.14f));
    g.fillRoundedRectangle(codecRect.toFloat(), 3.0f);
    g.setColour(dark ? codecCol.brighter(0.35f) : codecCol);
    g.setFont(juce::FontOptions(9.5f, juce::Font::bold));
    g.drawText(codecLabel, codecRect, juce::Justification::centred, false);

    juce::String titleText;
    if (!tr.title.empty()) {
        titleText = juce::String::fromUTF8(tr.title.c_str());
    } else if (!tr.fullWidthTitle.empty()) {
        titleText = juce::String::fromUTF8(tr.fullWidthTitle.c_str());
    } else {
        titleText = "Track " + juce::String(tr.index + 1);
    }
    const int titleX = 30;
    const int titleW = std::max(24, codecRect.getX() - titleX - 4);
    g.setColour(textPrimary);
    g.setFont(juce::FontOptions(11.0f));
    g.drawText(titleText, titleX, 0, titleW, height, juce::Justification::centredLeft, true);
}

juce::String MdWorkspace::tooltipForDeviceTrackRow(int row) const {
    const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
    if (recordUiState.activeDeviceIsHiMd) {
        if (!recordUiState.hasHiMdStatus) {
            return {};
        }
        const auto& tracks = recordUiState.hiMdStatus.tracks;
        if (row < 0 || row >= static_cast<int>(tracks.size())) {
            return {};
        }
        const auto& tr = tracks[static_cast<std::size_t>(row)];
        juce::String tip = "Hi-MD Track " + juce::String::formatted("%02d", tr.index + 1) + ": " +
                           (!tr.title.empty() ? juce::String::fromUTF8(tr.title.c_str())
                                              : juce::String("(untitled)"));
        if (!tr.artist.empty()) {
            tip += dot + "Artist: " + juce::String::fromUTF8(tr.artist.c_str());
        }
        if (!tr.album.empty()) {
            tip += dot + "Album: " + juce::String::fromUTF8(tr.album.c_str());
        }
        const juce::String enc =
            tr.encoding.empty() ? juce::String("Hi-SP")
                                : juce::String::fromUTF8(tr.encoding.c_str());
        tip += dot + enc +
               (tr.bitrate > 0 ? (" (" + juce::String(tr.bitrate) + " kbps)") : juce::String()) +
               dot + formatSecMmSs(static_cast<int>(std::llround(tr.durationSeconds)));
        return tip;
    }

    const auto& tracks = recordUiState.deviceStatus.tracks;
    if (row < 0 || row >= static_cast<int>(tracks.size())) {
        return {};
    }
    const auto& tr = tracks[static_cast<std::size_t>(row)];
    juce::String tip = "Disc Track " + juce::String::formatted("%02d", tr.index + 1) + ": ";
    if (!tr.title.empty()) {
        tip += juce::String::fromUTF8(tr.title.c_str());
    } else if (!tr.fullWidthTitle.empty()) {
        tip += juce::String::fromUTF8(tr.fullWidthTitle.c_str());
    } else {
        tip += "(untitled)";
    }
    if (!tr.fullWidthTitle.empty() && !tr.title.empty()) {
        tip += " [" + juce::String::fromUTF8(tr.fullWidthTitle.c_str()) + "]";
    }
    tip += dot +
           (tr.codec.empty() ? juce::String("SP") : juce::String::fromUTF8(tr.codec.c_str())) +
           dot + formatSecMmSs(tr.durationSeconds);
    return tip;
}
