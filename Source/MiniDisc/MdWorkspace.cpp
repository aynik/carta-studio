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

std::vector<int> computeSpanRefCounts(const carta::MdPhysicalPlan& plan) {
    std::vector<int> refs(plan.spans.size(), 0);
    for (const auto& placement : plan.placements) {
        if (placement.spanIndex >= 0 &&
            static_cast<std::size_t>(placement.spanIndex) < refs.size()) {
            refs[static_cast<std::size_t>(placement.spanIndex)]++;
        }
    }
    return refs;
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
    : juce::ListBox("MiniDisc tracks", &modelRef), owner(ownerRef) {
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
    return juce::ListBox::keyPressed(key);
}

MdWorkspace::PartListModel::PartListModel(MdWorkspace& ownerRef) : owner(ownerRef) {
}

int MdWorkspace::PartListModel::getNumRows() {
    return static_cast<int>(owner.partRows.size());
}

void MdWorkspace::PartListModel::paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                                                  bool selected) {
    owner.paintPartRow(row, g, width, height, selected);
}

void MdWorkspace::PartListModel::selectedRowsChanged(int lastRowSelected) {
    owner.handlePartSelectionChanged(lastRowSelected);
}

void MdWorkspace::PartListModel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) {
    owner.triggerSeekPart(row);
}

void MdWorkspace::PartListModel::returnKeyPressed(int row) {
    owner.triggerSeekPart(row);
}

void MdWorkspace::PartListModel::deleteKeyPressed(int row) {
    owner.triggerDeletePart(row);
}

juce::String MdWorkspace::PartListModel::getTooltipForRow(int row) {
    return owner.tooltipForPartRow(row);
}

MdWorkspace::PlanListModel::PlanListModel(MdWorkspace& ownerRef) : owner(ownerRef) {
}

int MdWorkspace::PlanListModel::getNumRows() {
    return static_cast<int>(owner.planRows.size());
}

void MdWorkspace::PlanListModel::paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                                                  bool selected) {
    owner.paintPlanRow(row, g, width, height, selected);
}

void MdWorkspace::PlanListModel::selectedRowsChanged(int lastRowSelected) {
    owner.handlePlanSelectionChanged(lastRowSelected);
}

void MdWorkspace::PlanListModel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) {
    owner.triggerOpenPlanRow(row);
}

void MdWorkspace::PlanListModel::returnKeyPressed(int row) {
    owner.triggerOpenPlanRow(row);
}

juce::String MdWorkspace::PlanListModel::getTooltipForRow(int row) {
    return owner.tooltipForPlanRow(row);
}

MdWorkspace::DeviceTrackListModel::DeviceTrackListModel(MdWorkspace& ownerRef) : owner(ownerRef) {
}

int MdWorkspace::DeviceTrackListModel::getNumRows() {
    return static_cast<int>(owner.recordUiState.deviceStatus.tracks.size());
}

void MdWorkspace::DeviceTrackListModel::paintListBoxItem(int row, juce::Graphics& g, int width,
                                                         int height, bool selected) {
    owner.paintDeviceTrackRow(row, g, width, height, selected);
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
      partModel(*this), partsList("UTOC parts", &partModel), deviceTrackModel(*this),
      deviceTrackList("On-disc tracks", &deviceTrackModel), planModel(*this),
      planList("Recording plan", &planModel) {
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
    mediumBox.setSelectedId(3, juce::dontSendNotification);
    mediumBox.setTooltip("Target MiniDisc physical capacity");
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
    codecBox.addItem("SP Stereo", 1);
    codecBox.addItem("SP Mono", 2);
    codecBox.addItem("LP2", 3);
    codecBox.addItem("LP4", 4);
    codecBox.setSelectedId(1, juce::dontSendNotification);
    codecBox.setTooltip("Default ATRAC codec for tracks without a per-track override");
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

    // --- Tab 1 Left: Action Bar & Track List ---
    addAndMakeVisible(insertTrackButton);
    insertTrackButton.setTooltip(
        "Insert an MD track for the current time selection or playhead-to-end range");
    insertTrackButton.onClick = [this] { triggerInsertTrack(); };

    addAndMakeVisible(duplicateTrackButton);
    duplicateTrackButton.setTooltip(
        "Duplicate the selected MD track (shares physical clusters on disc)");
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
    trackCodecBox.addItem("Default (Inherited)", 1);
    trackCodecBox.addItem("SP Stereo", 2);
    trackCodecBox.addItem("SP Mono", 3);
    trackCodecBox.addItem("LP2", 4);
    trackCodecBox.addItem("LP4", 5);
    trackCodecBox.setSelectedId(1, juce::dontSendNotification);
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

    addChildComponent(partsHeaderLabel);
    partsHeaderLabel.setText("UTOC Parts (0)", juce::dontSendNotification);
    partsHeaderLabel.setFont(juce::FontOptions(11.5f, juce::Font::bold));

    addChildComponent(addPartButton);
    addPartButton.setTooltip(
        "Append timeline selection (or playhead-to-end) as a sequential part in this track");
    addPartButton.onClick = [this] { triggerAddPart(); };

    addChildComponent(setPartSelButton);
    setPartSelButton.setTooltip("Update the selected part to match the current timeline selection");
    setPartSelButton.onClick = [this] { triggerSetPartToSelection(); };

    addChildComponent(repeatPartButton);
    repeatPartButton.setTooltip(
        "Repeat the selected part sequentially (shares existing physical clusters)");
    repeatPartButton.onClick = [this] { showRepeatPartMenu(); };

    addChildComponent(delPartButton);
    delPartButton.setTooltip("Remove the selected part from this track");
    delPartButton.onClick = [this] { triggerDeletePart(partsList.getSelectedRow()); };

    addChildComponent(seekPartButton);
    seekPartButton.setTooltip("Seek playhead and select this part's time range on Layer 1");
    seekPartButton.onClick = [this] { triggerSeekPart(partsList.getSelectedRow()); };

    addChildComponent(partsList);
    partsList.setRowHeight(24);
    partsList.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);

    // --- Tab 1 Right: Multi-Selection Inspector ---
    addChildComponent(multiHeaderLabel);
    multiHeaderLabel.setFont(juce::FontOptions(12.5f, juce::Font::bold));
    addChildComponent(multiGroupButton);
    multiGroupButton.onClick = [this] { promptCreateGroupForSelection(); };
    addChildComponent(multiUngroupButton);
    multiUngroupButton.onClick = [this] { clearGroupsForSelection(); };
    addChildComponent(multiDuplicateButton);
    multiDuplicateButton.onClick = [this] { triggerDuplicateTrack(); };
    addChildComponent(multiRemoveButton);
    multiRemoveButton.onClick = [this] { triggerRemoveSelectedTracks(); };
    addChildComponent(multiMergeButton);
    multiMergeButton.setTooltip(
        "Merge contiguous selected MiniDisc tracks sequentially into one track (T-MARK erase)");
    multiMergeButton.onClick = [this] { triggerMergeSelectedTracks(); };

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
    recordDeviceBox.setTooltip("Select connected NetMD USB recorder");
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
    planCardTitle.setText("Target Disc Plan", juce::dontSendNotification);
    planCardTitle.setFont(juce::FontOptions(11.5f, juce::Font::bold));
    planCardTitle.setBorderSize(juce::BorderSize<int>(0));

    addChildComponent(utocSummaryLabel);
    utocSummaryLabel.setFont(juce::FontOptions(10.4f));
    utocSummaryLabel.setJustificationType(juce::Justification::centredRight);
    utocSummaryLabel.setBorderSize(juce::BorderSize<int>(0));

    addChildComponent(planList);
    planList.setRowHeight(22);
    planList.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);

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
    error = "Disc workspace view must be tracks or record.";
    return false;
}

void MdWorkspace::setActiveTab(const juce::String& tab, bool notify) {
    const auto norm = tab.trim().toLowerCase();
    const auto next =
        (norm == "record" || norm == "map") ? juce::String("record") : juce::String("tracks");
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
    const int prevPartRow = partsList.getSelectedRow();

    discState = session.mdDisc;
    timeSelection = selection;
    playheadSample = static_cast<juce::int64>(
        std::llround(std::max(0.0, playheadSec) * static_cast<double>(carta::kMdSampleRate)));
    masterEndSample = carta::projectEndSample(session);
    revision = generation;

    std::string planErr;
    planValid = carta::planMdPhysicalAudio(discState, physicalPlan, planErr);
    planErrorText = planErr;

    carta::UtocDiscState compiledUtoc;
    std::string utocErr;
    utocValid = carta::compileMdDiscToUtoc(discState, compiledUtoc, utocErr);
    utocErrorText = utocErr;
    const int physTotal =
        discState.loadedDisc.active ? discState.loadedDisc.physicalTotalClusters() : 0;
    budget = utocValid ? carta::computeUtocBudget(
                             compiledUtoc, carta::mdMediumToString(discState.medium), physTotal)
                       : carta::UtocBudget{};

    // Build per-span reference counts so aliased/shared parts are immediately identifiable.
    const auto spanRefCounts = computeSpanRefCounts(physicalPlan);

    trackRows.clear();
    trackRows.reserve(discState.tracks.size());
    for (std::size_t i = 0; i < discState.tracks.size(); ++i) {
        const auto& t = discState.tracks[i];
        TrackRow row;
        row.id = juce::String::fromUTF8(t.id.c_str());
        row.index0 = static_cast<int>(i);
        const auto titleStr = t.displayTitle();
        row.title = titleStr.empty() ? ("Track " + juce::String(static_cast<int>(i + 1)))
                                     : juce::String::fromUTF8(titleStr.c_str());
        row.fullWidthTitle = juce::String::fromUTF8(t.fullWidthTitle.c_str());
        row.effectiveCodec = carta::effectiveTrackCodec(discState, t);
        row.hasCodecOverride = t.codecOverride.has_value();
        row.writeProtected = t.writeProtected;
        row.scms = t.scmsCopyProtected;
        row.emphasis = t.emphasis;
        row.hasTimestamp = !t.timestamp.empty();
        row.partCount = static_cast<int>(t.parts.size());
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
        row.sharesPhysicalSpan = false;
        row.onDisc = carta::mdTrackUsesOnDiscAudio(discState, t);
        for (const auto& p : physicalPlan.placements) {
            if (p.trackIndex == static_cast<int>(i) && p.spanIndex >= 0 &&
                static_cast<std::size_t>(p.spanIndex) < spanRefCounts.size() &&
                spanRefCounts[static_cast<std::size_t>(p.spanIndex)] > 1) {
                row.sharesPhysicalSpan = true;
                break;
            }
        }
        trackRows.push_back(std::move(row));
    }

    planRows.clear();
    planRows.reserve(trackRows.size());
    for (std::size_t i = 0; i < trackRows.size(); ++i) {
        const auto& tr = trackRows[i];
        PlanRow pr;
        pr.trackIndex = tr.index0;
        pr.id = tr.id;
        pr.title = tr.title;
        pr.groupName = tr.groupName;
        pr.groupColorIndex = tr.groupColorIndex;
        pr.codec = tr.effectiveCodec;
        pr.partCount = tr.partCount;
        pr.totalSamples = tr.totalSamples;
        pr.sharesPhysicalSpan = tr.sharesPhysicalSpan;
        pr.onDisc = tr.onDisc;
        int odMinClu = 0;
        int odMaxClu = 0;
        bool hasOd = false;
        for (const auto& place : physicalPlan.placements) {
            if (place.trackIndex != static_cast<int>(i)) {
                continue;
            }
            if (place.isOnDisc) {
                if (!hasOd) {
                    odMinClu = place.startAddr.cluster;
                    odMaxClu = place.endAddr.cluster;
                    hasOd = true;
                } else {
                    odMinClu = std::min(odMinClu, static_cast<int>(place.startAddr.cluster));
                    odMaxClu = std::max(odMaxClu, static_cast<int>(place.endAddr.cluster));
                }
            } else if (pr.primarySpanIndex < 0 && place.spanIndex >= 0 &&
                       static_cast<std::size_t>(place.spanIndex) < physicalPlan.spans.size()) {
                const auto& sp = physicalPlan.spans[static_cast<std::size_t>(place.spanIndex)];
                pr.primarySpanIndex = place.spanIndex;
                pr.spanClusterStart = sp.startCluster;
                pr.spanClusterEnd = sp.startCluster + std::max(0, sp.clusterCount - 1);
                pr.spanRefCount = static_cast<std::size_t>(place.spanIndex) < spanRefCounts.size()
                                      ? spanRefCounts[static_cast<std::size_t>(place.spanIndex)]
                                      : 1;
            }
        }
        if (hasOd && pr.primarySpanIndex < 0) {
            pr.spanClusterStart = odMinClu;
            pr.spanClusterEnd = odMaxClu;
        }
        planRows.push_back(std::move(pr));
    }

    syncingControls = true;
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
        planList.selectRow(targetRow, false, true);
    } else {
        trackList.deselectAllRows();
        planList.deselectAllRows();
    }
    syncingControls = false;

    rebuildPartRows(prevPartRow);
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
    recordDeviceBox.clear(juce::dontSendNotification);
    int selectedMenuId = 0;
    for (std::size_t i = 0; i < snapshot.devices.size(); ++i) {
        const auto& d = snapshot.devices[i];
        const int menuId = static_cast<int>(i) + 1;
        recordDeviceIds.push_back(d.id);
        const auto devName = juce::String::fromUTF8(d.name.c_str());
        const auto devId = juce::String::fromUTF8(d.id.c_str());
        recordDeviceBox.addItem(devName + " [" + devId + "]", menuId);
        if (!snapshot.activeDeviceId.empty() && d.id == snapshot.activeDeviceId) {
            selectedMenuId = menuId;
        }
    }
    if (recordDeviceIds.empty()) {
        recordDeviceBox.addItem("No NetMD device", 1);
        recordDeviceBox.setSelectedId(1, juce::dontSendNotification);
        recordDeviceBox.setEnabled(false);
    } else {
        recordDeviceBox.setEnabled(!recordBusy);
        recordDeviceBox.setSelectedId(selectedMenuId > 0 ? selectedMenuId : 1,
                                      juce::dontSendNotification);
    }
    syncingControls = false;

    utocSummaryLabel.setText(snapshot.utocSummaryText, juce::dontSendNotification);
    recordBadgeLabel.setText(snapshot.badgeText, juce::dontSendNotification);
    recordBadgeLabel.setTooltip(snapshot.badgeText);
    recordStatusLabel.setText(snapshot.statusText, juce::dontSendNotification);
    deviceTrackList.updateContent();
    updateTabVisibility();
    deviceTrackList.repaint();
    refreshRecordButton.setEnabled(!recordBusy);
    loadDiscButton.setEnabled(!recordBusy && snapshot.canLoadDisc);
    readDiscTocButton.setEnabled(!recordBusy && snapshot.canReadToc);
    eraseDiscButton.setEnabled(!recordBusy && snapshot.canErase);
    writeUtocButton.setEnabled(!recordBusy && snapshot.canWriteUtoc);
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

    // UTOC budget pill in header
    if (!budgetPillBounds.isEmpty()) {
        const bool over = !utocValid || !budget.fits() || !planValid;
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
            g.drawText("Parts", x + w - 104, y, 36, h, juce::Justification::centred, false);
            g.drawText("Dur", x + w - 66, y, 56, h, juce::Justification::centredRight, false);
        }
    } else {
        const bool devConnected =
            recordUiState.hasDeviceStatus && !recordUiState.deviceStatus.deviceId.empty();
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
                const auto& st = recordUiState.deviceStatus;
                juce::String emptyMsg = "Connect a NetMD recorder and click Refresh.";
                if (devConnected) {
                    if (!st.discPresent) {
                        emptyMsg = "No disc loaded in recorder";
                    } else if (st.hiMdFormatted) {
                        emptyMsg = "Hi-MD formatted disc — erase for Standard MD";
                    } else if (st.trackCount > 0 && st.tracks.empty()) {
                        emptyMsg = juce::String(st.trackCount) +
                                   " tracks on disc — click Load Disc to view & edit";
                    } else {
                        emptyMsg = "Blank disc — 0 tracks on device";
                    }
                }
                g.setColour(textMuted);
                g.setFont(juce::FontOptions(10.8f));
                g.drawText(emptyMsg, devListBounds.reduced(8, 2), juce::Justification::centred,
                           true);
            }
        }
        paintTargetCapacityBar(g, targetBarBounds, dark);
        if (planRows.empty() && !planList.getBounds().isEmpty()) {
            g.setColour(textMuted);
            g.setFont(juce::FontOptions(11.3f));
            g.drawText("Add MiniDisc tracks (Tracks & UTOC -> Tracks from Clips) to plan physical "
                       "disc spans.",
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
    const int medW = 84;
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
        const int btnGap = 4;
        const int insertW = 68;
        const int dupW = 74;
        const int splitW = 54;
        const int upW = 42;
        const int dnW = 50;
        const int remW = 68;
        const int grpW = 78;
        insertTrackButton.setBounds(ax, actionRow.getY(), insertW, 24);
        ax += insertW + btnGap;
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
                                  std::min(grpW, std::max(50, actionRow.getRight() - ax)), 24);

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

    auto singleArea = area;
    auto rowA = singleArea.removeFromTop(24);
    trackTitleLabel.setBounds(rowA.removeFromLeft(34));
    const int formsW = 64;
    const int flagsW = 62;
    trackFlagsButton.setBounds(rowA.removeFromRight(flagsW));
    rowA.removeFromRight(4);
    trackFormsButton.setBounds(rowA.removeFromRight(formsW));
    rowA.removeFromRight(4);
    trackTitleEditor.setBounds(rowA);

    singleArea.removeFromTop(5);
    auto rowB = singleArea.removeFromTop(24);
    trackCodecLabel.setBounds(rowB.removeFromLeft(42));
    const int codecW = std::min(150, std::max(100, rowB.getWidth() / 2));
    trackCodecBox.setBounds(rowB.removeFromLeft(codecW));
    rowB.removeFromLeft(6);
    trackGroupButton.setBounds(rowB);

    singleArea.removeFromTop(6);
    auto rowC = singleArea.removeFromTop(24);
    const int seekW = 48;
    const int delW = 40;
    const int repW = 68;
    const int setW = 70;
    const int addW = 56;
    seekPartButton.setBounds(rowC.removeFromRight(seekW));
    rowC.removeFromRight(3);
    delPartButton.setBounds(rowC.removeFromRight(delW));
    rowC.removeFromRight(3);
    repeatPartButton.setBounds(rowC.removeFromRight(repW));
    rowC.removeFromRight(3);
    setPartSelButton.setBounds(rowC.removeFromRight(setW));
    rowC.removeFromRight(3);
    addPartButton.setBounds(rowC.removeFromRight(addW));
    rowC.removeFromRight(4);
    partsHeaderLabel.setBounds(rowC);

    singleArea.removeFromTop(4);
    partsList.setBounds(singleArea);
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
    refreshRecordButton.setBounds(topRow.removeFromRight(62));
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
    auto topRow = area.removeFromTop(16);
    planCardTitle.setBounds(topRow.removeFromLeft(116));
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
    const auto selected = selectedTrackIndices0();
    const bool noneSelected = isTracks && selected.empty();
    const bool oneSelected = isTracks && selected.size() == 1;
    const bool multiSelected = isTracks && selected.size() > 1;

    syncClipsButton.setVisible(isTracks);
    previewTrackButton.setVisible(isTracks);
    previewFromButton.setVisible(isTracks);
    reviewWavButton.setVisible(isTracks);
    inspectUtocButton.setVisible(isRecord);
    importUtocButton.setVisible(isRecord);
    exportUtocButton.setVisible(isRecord);

    insertTrackButton.setVisible(isTracks);
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
    partsHeaderLabel.setVisible(oneSelected);
    addPartButton.setVisible(oneSelected);
    setPartSelButton.setVisible(oneSelected);
    repeatPartButton.setVisible(oneSelected);
    delPartButton.setVisible(oneSelected);
    seekPartButton.setVisible(oneSelected);
    partsList.setVisible(oneSelected);

    multiHeaderLabel.setVisible(multiSelected);
    multiGroupButton.setVisible(multiSelected);
    multiUngroupButton.setVisible(multiSelected);
    multiDuplicateButton.setVisible(multiSelected);
    multiRemoveButton.setVisible(multiSelected);
    multiMergeButton.setVisible(multiSelected);

    deviceCardTitle.setVisible(isRecord);
    recordDeviceBox.setVisible(isRecord);
    refreshRecordButton.setVisible(isRecord);
    recordBadgeLabel.setVisible(isRecord);
    const bool showDeviceTracks =
        isRecord && !deviceTrackList.getBounds().isEmpty() && recordUiState.hasDeviceStatus &&
        recordUiState.deviceStatus.discPresent && !recordUiState.deviceStatus.hiMdFormatted &&
        !recordUiState.deviceStatus.tracks.empty();
    deviceTrackList.setVisible(showDeviceTracks);
    loadDiscButton.setVisible(isRecord);
    readDiscTocButton.setVisible(isRecord);
    eraseDiscButton.setVisible(isRecord);
    planCardTitle.setVisible(isRecord);
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
}

void MdWorkspace::refreshPlanSummaryLabels() {
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
        planHintLabel.setText(juce::String(static_cast<int>(physicalPlan.spans.size())) +
                                  juce::String::fromUTF8(" physical spans \xc2\xb7 ") +
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
            "Drag splits or edges in the MD Tracks lane; repeated parts share physical clusters",
            juce::dontSendNotification);
    }
}

void MdWorkspace::rebuildPartRows(int preferredPartRow) {
    partRows.clear();
    const auto selected = selectedTrackIndices0();
    if (selected.size() == 1) {
        const int row = selected.front();
        const auto& track = discState.tracks[static_cast<std::size_t>(row)];
        const auto spanRefs = computeSpanRefCounts(physicalPlan);

        for (std::size_t p = 0; p < track.parts.size(); ++p) {
            PartRow pr;
            pr.index0 = static_cast<int>(p);
            pr.startSample = track.parts[p].startSample;
            pr.endSample = track.parts[p].endSample;
            for (const auto& place : physicalPlan.placements) {
                if (place.trackIndex == row && place.partIndex == static_cast<int>(p)) {
                    pr.spanIndex = place.spanIndex;
                    pr.startAddr = place.startAddr;
                    pr.endAddr = place.endAddr;
                    pr.sharedSpan = place.spanIndex >= 0 &&
                                    static_cast<std::size_t>(place.spanIndex) < spanRefs.size() &&
                                    spanRefs[static_cast<std::size_t>(place.spanIndex)] > 1;
                    break;
                }
            }
            partRows.push_back(std::move(pr));
        }

        syncingControls = true;
        const auto titleStr = track.halfWidthTitle;
        trackBaselineTitle = juce::String::fromUTF8(titleStr.c_str());
        if (!trackTitleEditor.hasKeyboardFocus(true)) {
            trackTitleEditor.setText(trackBaselineTitle, juce::dontSendNotification);
        }
        trackCodecBox.setSelectedId(
            track.codecOverride ? (carta::md_ui::codecToMenuId(*track.codecOverride) + 1) : 1,
            juce::dontSendNotification);
        const bool onDiscTrack = carta::mdTrackUsesOnDiscAudio(discState, track);
        trackCodecBox.setEnabled(!onDiscTrack);
        trackCodecBox.setTooltip(
            onDiscTrack
                ? "Encoding mode is fixed for existing on-disc audio (re-encoding not supported "
                  "in UTOC-only mode)"
                : "Override encoding mode for this track only, or inherit the disc default");
        const auto& rowMeta = trackRows[static_cast<std::size_t>(row)];
        trackGroupButton.setButtonText(rowMeta.groupName.empty()
                                           ? "Group: None..."
                                           : "Grp: " +
                                                 juce::String::fromUTF8(rowMeta.groupName.c_str()));
        partsHeaderLabel.setText("UTOC Parts (" + juce::String(static_cast<int>(partRows.size())) +
                                     ")",
                                 juce::dontSendNotification);
        syncingControls = false;
    } else if (selected.size() > 1) {
        multiHeaderLabel.setText(
            juce::String(static_cast<int>(selected.size())) + " tracks selected (Tracks " +
                juce::String(selected.front() + 1) + juce::String::fromUTF8("\xe2\x80\x93") +
                juce::String(selected.back() + 1) + ")",
            juce::dontSendNotification);
    }

    syncingControls = true;
    partsList.updateContent();
    if (!partRows.empty()) {
        const int chosen =
            (preferredPartRow >= 0 && preferredPartRow < static_cast<int>(partRows.size()))
                ? preferredPartRow
                : 0;
        partsList.selectRow(chosen, false, true);
    } else {
        partsList.deselectAllRows();
    }
    syncingControls = false;
}

void MdWorkspace::updateButtonStates() {
    const auto selected = selectedTrackIndices0();
    const int row = selected.size() == 1 ? selected.front() : -1;
    const int partRow = partsList.getSelectedRow();
    const bool hasSel = timeSelection.hasRange();
    const bool hasValidSinglePart = partRow >= 0 && partRow < static_cast<int>(partRows.size());
    const bool hasAudio = masterEndSample > 0;

    insertTrackButton.setEnabled(hasSel || hasAudio);
    duplicateTrackButton.setEnabled(!selected.empty());
    bool canSplit = false;
    if (row >= 0) {
        const auto& tr = discState.tracks[static_cast<std::size_t>(row)];
        for (const auto& p : tr.parts) {
            if (playheadSample > p.startSample && playheadSample < p.endSample) {
                canSplit = true;
                break;
            }
        }
    }
    splitTrackButton.setEnabled(canSplit);
    moveUpButton.setEnabled(row > 0);
    moveDownButton.setEnabled(row >= 0 && row + 1 < static_cast<int>(trackRows.size()));
    removeTrackButton.setEnabled(!selected.empty());
    groupMenuButton.setEnabled(!trackRows.empty());

    previewTrackButton.setEnabled(row >= 0);
    previewFromButton.setEnabled(row >= 0);
    reviewWavButton.setEnabled(!trackRows.empty());

    addPartButton.setEnabled(row >= 0 && (hasSel || hasAudio));
    setPartSelButton.setEnabled(row >= 0 && hasValidSinglePart && hasSel);
    repeatPartButton.setEnabled(row >= 0 && hasValidSinglePart);
    delPartButton.setEnabled(row >= 0 && hasValidSinglePart && partRows.size() > 1);
    seekPartButton.setEnabled(row >= 0 && hasValidSinglePart);

    const bool contiguous = selected.size() >= 1 && (selected.back() - selected.front() + 1 ==
                                                     static_cast<int>(selected.size()));
    multiGroupButton.setEnabled(contiguous);
    multiUngroupButton.setEnabled(!selected.empty());
    multiDuplicateButton.setEnabled(!selected.empty());
    multiRemoveButton.setEnabled(!selected.empty());
    multiMergeButton.setEnabled(contiguous && selected.size() >= 2);
}

// ============================================================================
// Selection, Commit, & Command Dispatch Triggers
// ============================================================================

void MdWorkspace::handleTrackSelectionChanged(int lastRowSelected) {
    if (syncingControls) {
        return;
    }
    syncingControls = true;
    if (lastRowSelected >= 0 && lastRowSelected < static_cast<int>(planRows.size())) {
        planList.selectRow(lastRowSelected, false, true);
    } else {
        planList.deselectAllRows();
    }
    syncingControls = false;
    rebuildPartRows();
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
    if (lastRowSelected < 0 || lastRowSelected >= static_cast<int>(trackRows.size())) {
        return;
    }
    syncingControls = true;
    trackList.selectRow(lastRowSelected, false, true);
    trackList.scrollToEnsureRowIsOnscreen(lastRowSelected);
    syncingControls = false;
    rebuildPartRows();
    updateButtonStates();
    repaint();
    if (onTrackSelectionChanged) {
        onTrackSelectionChanged(trackRows[static_cast<std::size_t>(lastRowSelected)].id);
    }
}

void MdWorkspace::triggerOpenPlanRow(int row) {
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

void MdWorkspace::handlePartSelectionChanged(int) {
    if (syncingControls) {
        return;
    }
    updateButtonStates();
    repaint();
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
    const int row = singleSelectedTrackIndex();
    if (row < 0 || !onCommand) {
        return;
    }
    const int dest = row + delta;
    if (dest < 0 || dest >= static_cast<int>(trackRows.size())) {
        return;
    }
    onCommand({"disc", "track", "move", juce::String(row), juce::String(dest)}, revision);
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

void MdWorkspace::triggerAddPart() {
    commitPendingEdits();
    const int row = singleSelectedTrackIndex();
    if (row < 0 || !onCommand) {
        return;
    }
    juce::int64 start = 0;
    juce::int64 end = 0;
    if (!resolveSelectedOrPlayheadRange(timeSelection, playheadSample, masterEndSample, start,
                                        end)) {
        if (onFeedback) {
            onFeedback("Select a timeline range first.");
        }
        return;
    }
    onCommand(
        {"disc", "track", "part", "add", juce::String(row), juce::String(start), juce::String(end)},
        revision);
}

void MdWorkspace::triggerSetPartToSelection() {
    commitPendingEdits();
    const int row = singleSelectedTrackIndex();
    const int partRow = partsList.getSelectedRow();
    if (row < 0 || partRow < 0 || !timeSelection.hasRange() || !onCommand) {
        return;
    }
    onCommand({"disc", "track", "part", "set", juce::String(row), juce::String(partRow),
               juce::String(timeSelection.start), juce::String(timeSelection.end)},
              revision);
}

void MdWorkspace::triggerDeletePart(int partRow) {
    commitPendingEdits();
    const int row = singleSelectedTrackIndex();
    if (row < 0 || partRow < 0 || partRow >= static_cast<int>(partRows.size()) || !onCommand) {
        return;
    }
    onCommand({"disc", "track", "part", "remove", juce::String(row), juce::String(partRow)},
              revision);
}

void MdWorkspace::triggerSeekPart(int partRow) {
    const int row = singleSelectedTrackIndex();
    if (row < 0 || partRow < 0 || partRow >= static_cast<int>(partRows.size())) {
        return;
    }
    if (onSeekPart) {
        onSeekPart(trackRows[static_cast<std::size_t>(row)].id, partRow, revision);
    }
}

// ============================================================================
// Popup Menus & Group Assignment Dialog
// ============================================================================

void MdWorkspace::showRepeatPartMenu() {
    const int row = singleSelectedTrackIndex();
    const int partRow = partsList.getSelectedRow();
    if (row < 0 || partRow < 0) {
        return;
    }

    juce::PopupMenu menu;
    menu.addItem(1, "Repeat 1x (share physical clusters)");
    menu.addItem(2, "Repeat 2x (3 plays total)");
    menu.addItem(3, "Repeat 4x (5 plays total)");
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&repeatPartButton),
                       [safe = juce::Component::SafePointer<MdWorkspace>(this), row, partRow,
                        rev = revision](int choice) {
                           if (safe == nullptr || choice <= 0 || !safe->onCommand) {
                               return;
                           }
                           const int count = (choice == 1) ? 1 : (choice == 2 ? 2 : 4);
                           safe->onCommand({"disc", "track", "part", "repeat", juce::String(row),
                                            juce::String(partRow), juce::String(count)},
                                           rev);
                       });
}

void MdWorkspace::showTrackContextMenu(int row) {
    if (row < 0 || row >= static_cast<int>(trackRows.size())) {
        return;
    }
    juce::PopupMenu menu;
    menu.addItem(1, "Rename Track...");
    menu.addItem(2, "ASCII & Full-Width Titles...");
    menu.addItem(3, "Flags & Timestamp...");
    menu.addSeparator();
    menu.addItem(4, "Duplicate Track");
    menu.addItem(8, "Merge with Next Track", row + 1 < static_cast<int>(trackRows.size()));
    menu.addItem(5, "Remove Track");
    menu.addSeparator();
    menu.addItem(6, "Preview Track");
    menu.addItem(7, "Preview From Here to End");
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&trackList),
                       [safe = juce::Component::SafePointer<MdWorkspace>(this), row](int choice) {
                           if (safe == nullptr || choice <= 0) {
                               return;
                           }
                           safe->trackList.selectRow(row, false, true);
                           if (choice == 1) {
                               safe->focusTrackRename();
                           } else if (choice == 2 && safe->onEditTrackForms) {
                               safe->onEditTrackForms();
                           } else if (choice == 3 && safe->onEditTrackFlags) {
                               safe->onEditTrackFlags();
                           } else if (choice == 4) {
                               safe->triggerDuplicateTrack();
                           } else if (choice == 8 && safe->onCommand) {
                               safe->onCommand({"disc", "track", "merge", juce::String(row)},
                                               safe->revision);
                           } else if (choice == 5) {
                               safe->triggerRemoveSelectedTracks();
                           } else if (choice == 6) {
                               safe->triggerPreviewSelected(false);
                           } else if (choice == 7) {
                               safe->triggerPreviewSelected(true);
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
    if (tr.sharesPhysicalSpan) {
        attrs.add("ALIAS");
    }
    const auto attrText = attrs.joinIntoString(" ");
    const int attrWidth =
        attrText.isNotEmpty() ? std::min(120, static_cast<int>(attrText.length()) * 7 + 12) : 0;

    const int rightReserve = 166;
    const int titleX = 114;
    const int titleAvail = std::max(30, width - titleX - rightReserve - attrWidth);
    if (!(inlineEditingRow == row && inlineTrackTitleEditor.isVisible())) {
        g.setColour(textPrimary);
        g.setFont(juce::FontOptions(12.2f, selected ? juce::Font::bold : juce::Font::plain));
        g.drawText(tr.title, titleX, 0, titleAvail, height, juce::Justification::centredLeft, true);
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

    // Parts count
    g.setColour(tr.partCount > 1 ? (dark ? juce::Colour(0xff5eead4) : juce::Colour(0xff0f766e))
                                 : textMuted);
    g.setFont(juce::FontOptions(11.3f, tr.partCount > 1 ? juce::Font::bold : juce::Font::plain));
    g.drawText(juce::String(tr.partCount) + (tr.partCount > 1 ? "p" : ""), width - 104, 0, 34,
               height, juce::Justification::centred, false);

    // Duration
    g.setColour(textPrimary);
    g.setFont(juce::FontOptions(11.5f));
    g.drawText(carta::md_ui::formatClock(tr.totalSamples), width - 66, 0, 56, height,
               juce::Justification::centredRight, false);
}

void MdWorkspace::paintPartRow(int row, juce::Graphics& g, int width, int height,
                               bool selected) const {
    if (row < 0 || row >= static_cast<int>(partRows.size())) {
        return;
    }
    const auto& pr = partRows[static_cast<std::size_t>(row)];
    const bool dark = juce::Desktop::getInstance().isDarkModeActive();
    const auto textPrimary = dark ? juce::Colour(0xffe0e0e0) : juce::Colour(0xff262626);
    const auto textMuted = dark ? juce::Colour(0xff9e9e9e) : juce::Colour(0xff666666);
    paintAlternatingRowBackground(g, row, width, height, selected, dark);

    g.setColour(textMuted);
    g.setFont(juce::FontOptions(10.8f, juce::Font::bold));
    g.drawText("P" + juce::String(row + 1), 6, 0, 26, height, juce::Justification::centredLeft,
               false);

    const auto rangeStr = carta::md_ui::formatClock(pr.startSample) +
                          juce::String::fromUTF8(" \xe2\x80\x93 ") +
                          carta::md_ui::formatClock(pr.endSample);
    g.setColour(textPrimary);
    g.setFont(juce::FontOptions(11.3f));
    g.drawText(rangeStr, 34, 0, 122, height, juce::Justification::centredLeft, true);

    const auto addrStr =
        pr.spanIndex >= 0
            ? (carta::md_ui::formatAddr(pr.startAddr) + ".." + carta::md_ui::formatAddr(pr.endAddr))
            : juce::String("(unplanned)");
    g.setColour(textMuted);
    g.setFont(juce::FontOptions(10.6f));
    g.drawText(addrStr, 158, 0, std::max(40, width - 220), height, juce::Justification::centredLeft,
               true);

    if (pr.sharedSpan) {
        const auto tag = juce::Rectangle<int>(width - 58, 4, 52, height - 8);
        const auto teal = dark ? juce::Colour(0xff34a39e) : juce::Colour(0xff0f766e);
        g.setColour(teal.withAlpha(dark ? 0.25f : 0.14f));
        g.fillRoundedRectangle(tag.toFloat(), 3.0f);
        g.setColour(teal);
        g.setFont(juce::FontOptions(9.8f, juce::Font::bold));
        g.drawText("SHARED", tag, juce::Justification::centred, false);
    }
}

juce::String MdWorkspace::tooltipForTrackRow(int row) const {
    if (row < 0 || row >= static_cast<int>(trackRows.size())) {
        return {};
    }
    const auto& tr = trackRows[static_cast<std::size_t>(row)];
    juce::String tip = "Track " + juce::String(row + 1) + ": " + tr.title;
    if (tr.fullWidthTitle.isNotEmpty()) {
        tip += " [" + tr.fullWidthTitle + "]";
    }
    tip += juce::String::fromUTF8(" \xc2\xb7 ") + carta::md_ui::codecLongLabel(tr.effectiveCodec) +
           juce::String::fromUTF8(" \xc2\xb7 ") + carta::md_ui::formatClock(tr.totalSamples) +
           " (" + juce::String(tr.partCount) + (tr.partCount == 1 ? " part)" : " parts)");
    if (tr.sharesPhysicalSpan) {
        tip += juce::String::fromUTF8(" \xc2\xb7 Aliases existing physical clusters");
    }
    tip += juce::String::fromUTF8(" \xc2\xb7 Double-click to edit title in place");
    return tip;
}

juce::String MdWorkspace::tooltipForPartRow(int row) const {
    if (row < 0 || row >= static_cast<int>(partRows.size())) {
        return {};
    }
    const auto& pr = partRows[static_cast<std::size_t>(row)];
    juce::String tip = "Part " + juce::String(row + 1) + ": samples [" +
                       juce::String(pr.startSample) + ", " + juce::String(pr.endSample) + ")";
    if (pr.spanIndex >= 0) {
        tip += juce::String::fromUTF8(" \xc2\xb7 Physical span #") +
               juce::String(pr.spanIndex + 1) + " (" + carta::md_ui::formatAddr(pr.startAddr) +
               " to " + carta::md_ui::formatAddr(pr.endAddr) + ")";
        if (pr.sharedSpan) {
            tip +=
                juce::String::fromUTF8(" \xc2\xb7 Shared with other parts (0 extra disc clusters)");
        }
    }
    return tip;
}

juce::String MdWorkspace::formatSecMmSs(int totalSeconds) {
    const int s = std::max(0, totalSeconds);
    return juce::String::formatted("%02d:%02d", s / 60, s % 60);
}

void MdWorkspace::updatePanelHoverTooltip(juce::Point<int> point) {
    if (budgetPillBounds.contains(point)) {
        lastHoverTooltip =
            utocValid
                ? "UTOC Sectors: " + juce::String(budget.usedTracks) + "/" +
                      juce::String(budget.maxTracks) + " tracks, " +
                      juce::String(budget.usedFragments) + "/" + juce::String(budget.maxFragments) +
                      " fragments, " + juce::String(budget.usedClusters) + "/" +
                      juce::String(budget.totalClusters) + " clusters, " +
                      juce::String(budget.usedHalfWidthCells) + "/" +
                      juce::String(budget.maxHalfWidthCells) + " ASCII title cells, " +
                      juce::String(budget.usedFullWidthCells) + "/" +
                      juce::String(budget.maxFullWidthCells) + " full-width cells."
                : juce::String::fromUTF8(utocErrorText.c_str());
    } else if (activeTab == "record" && targetBarBounds.contains(point)) {
        const int totalClu =
            std::max(1, discState.loadedDisc.active ? discState.loadedDisc.physicalTotalClusters()
                                                    : carta::mdTotalClusters(discState.medium));
        if (discState.loadedDisc.active) {
            lastHoverTooltip =
                "Target Disc Capacity: " + juce::String(physicalPlan.usedClusters) + "/" +
                juce::String(totalClu) + " clusters (" +
                juce::String(physicalPlan.onDiscClusters) + " on-disc + " +
                juce::String(physicalPlan.newUploadClusters) + " new upload(s)" +
                (physicalPlan.reclaimedClusters > 0
                     ? (", reclaim " + juce::String(physicalPlan.reclaimedClusters) + " clusters")
                     : juce::String()) +
                ").";
        } else {
            lastHoverTooltip =
                "Target Disc Capacity: " + juce::String(physicalPlan.usedClusters) + "/" +
                juce::String(totalClu) + " clusters across " +
                juce::String(static_cast<int>(physicalPlan.spans.size())) +
                " physical audio upload(s)" +
                (physicalPlan.savedClusters > 0 ? (" (" + juce::String(physicalPlan.savedClusters) +
                                                   " clusters saved by aliasing)")
                                                : juce::String()) +
                ".";
        }
    } else if (activeTab == "record" && deviceDiscMeterBounds.contains(point) &&
               recordUiState.hasDeviceStatus) {
        const auto& st = recordUiState.deviceStatus;
        lastHoverTooltip =
            st.discPresent
                ? ("Physical Disc: " + juce::String::fromUTF8(st.discTitle.c_str()) + " (" +
                   juce::String(st.trackCount) + " tracks, used " + formatSecMmSs(st.usedSeconds) +
                   ", free " + formatSecMmSs(st.leftSeconds) + ")")
                : "No disc inserted in selected USB recorder.";
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
    const bool over = !utocValid || !planValid || !budget.fits();

    g.setColour(wellBg);
    g.fillRoundedRectangle(bounds.toFloat(), 4.0f);

    const int totalClu =
        std::max(1, discState.loadedDisc.active ? discState.loadedDisc.physicalTotalClusters()
                                                : carta::mdTotalClusters(discState.medium));
    const auto inner = bounds.reduced(1);
    int cursorClu = 0;
    if (discState.loadedDisc.active && physicalPlan.onDiscClusters > 0) {
        const int x1 = inner.getX();
        const int x2 = std::min(
            inner.getRight(),
            std::max(x1 + 3,
                     inner.getX() + static_cast<int>(std::llround(
                                        (static_cast<double>(physicalPlan.onDiscClusters) /
                                         static_cast<double>(totalClu)) *
                                        inner.getWidth()))));
        const auto onDiscRect = juce::Rectangle<int>(x1, inner.getY(), x2 - x1, inner.getHeight());
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
        cursorClu = physicalPlan.onDiscClusters;
    }
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
        const int x2Raw =
            inner.getX() +
            static_cast<int>(std::llround(
                (static_cast<double>(startClu + sp.clusterCount) / static_cast<double>(totalClu)) *
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
            g.drawText("S" + juce::String(static_cast<int>(i + 1)) + " " +
                           carta::md_ui::codecShortLabel(sp.codec),
                       segRect.reduced(3, 0), juce::Justification::centred, true);
        }
    }

    g.setColour(over ? juce::Colour(0xffdc2626) : border);
    g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 4.0f, 1.0f);

    const int pct = static_cast<int>(std::llround(
        (static_cast<double>(physicalPlan.usedClusters) * 100.0) / static_cast<double>(totalClu)));
    const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
    const auto capText =
        discState.loadedDisc.active
            ? (juce::String(physicalPlan.usedClusters) + "/" + juce::String(totalClu) +
               " clusters (" + juce::String(pct) + "%)" + dot +
               juce::String(physicalPlan.onDiscClusters) + " on-disc + " +
               juce::String(physicalPlan.newUploadClusters) + " new" +
               (physicalPlan.reclaimedClusters > 0
                    ? (dot + "reclaim " + juce::String(physicalPlan.reclaimedClusters))
                    : juce::String()))
            : (juce::String(physicalPlan.usedClusters) + "/" + juce::String(totalClu) +
               " clusters (" + juce::String(pct) + "%)" +
               (physicalPlan.spans.empty()
                    ? juce::String()
                    : (dot + juce::String(static_cast<int>(physicalPlan.spans.size())) +
                       " physical upload(s)")));
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

    const int physW = std::min(196, std::max(110, width / 3));
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
    const auto enDash = juce::String::fromUTF8("\xe2\x80\x93");
    if (pr.onDisc && pr.primarySpanIndex < 0) {
        const auto range = "C" + juce::String(pr.spanClusterStart) + enDash +
                           juce::String(pr.spanClusterEnd);
        physText = "ON-DISC " + range + (pr.sharesPhysicalSpan ? " (shared)" : "");
    } else if (pr.primarySpanIndex >= 0) {
        const auto range =
            "C" + juce::String(pr.spanClusterStart) + enDash + juce::String(pr.spanClusterEnd);
        const juce::String prefix =
            pr.onDisc ? "ON-DISC + NEW #"
                      : (discState.loadedDisc.active ? "NEW Span #" : "Span #");
        if (pr.sharesPhysicalSpan) {
            physText =
                "Alias -> Span #" + juce::String(pr.primarySpanIndex + 1) + " (" + range + ")";
        } else if (pr.spanRefCount > 1) {
            physText =
                prefix + juce::String(pr.primarySpanIndex + 1) + " (shared " + range + ")";
        } else {
            physText = prefix + juce::String(pr.primarySpanIndex + 1) + " " + range;
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
    if (pr.onDisc && pr.primarySpanIndex < 0) {
        tip += juce::String::fromUTF8(" \xc2\xb7 Preserved On-Disc Audio (Clusters ") +
               juce::String(pr.spanClusterStart) + ".." + juce::String(pr.spanClusterEnd) + ")";
    } else if (pr.primarySpanIndex >= 0) {
        tip += juce::String::fromUTF8(" \xc2\xb7 ") +
               (discState.loadedDisc.active ? "New Upload Span #" : "Physical Span #") +
               juce::String(pr.primarySpanIndex + 1) + " (Clusters " +
               juce::String(pr.spanClusterStart) + ".." + juce::String(pr.spanClusterEnd) + ")";
    }
    return tip;
}

void MdWorkspace::paintDeviceTrackRow(int row, juce::Graphics& g, int width, int height,
                                      bool selected) const {
    const auto& tracks = recordUiState.deviceStatus.tracks;
    if (row < 0 || row >= static_cast<int>(tracks.size())) {
        return;
    }
    const auto& tr = tracks[static_cast<std::size_t>(row)];
    const bool dark = juce::Desktop::getInstance().isDarkModeActive();
    const auto textPrimary = dark ? juce::Colour(0xffe0e0e0) : juce::Colour(0xff262626);
    const auto textMuted = dark ? juce::Colour(0xff9e9e9e) : juce::Colour(0xff666666);
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
    const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
    tip += dot +
           (tr.codec.empty() ? juce::String("SP") : juce::String::fromUTF8(tr.codec.c_str())) +
           dot + formatSecMmSs(tr.durationSeconds);
    return tip;
}
