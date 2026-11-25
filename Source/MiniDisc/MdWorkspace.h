/**
 * @file MdWorkspace.h
 * Docked two-tab MiniDisc Audio Pool & Play Order authoring and NetMD / Hi-MD recording panel.
 *
 * UI / IPC Contract:
 * - Tab 1 ("tracks" / "Play Order") edits canonical Audio Pool items, Play Order
 *   occurrences & aliases, codec overrides, flags/timestamps, and track groups.
 * - Tab 2 ("record" / "Device & Record") inspects USB NetMD / Hi-MD hardware, shows
 *   inserted-disc tracks as seen by the device alongside the target recording
 *   plan (`cluster:sector.group` / Hi-MD FAT16 bytes), and dispatches read-TOC / erase /
 *   ForceTOCEdit / record workflows.
 * - Every user edit dispatches through `onCommand` (`disc ...`) with the
 *   current optimistic-concurrency `revision` so UI actions, shortcuts, and
 *   CLI/IPC agents share one undoable command executor.
 *
 * Units & Threading:
 * - Timeline positions use 44.1 kHz (`kMdSampleRate`) half-open sample ranges;
 *   physical views report `(cluster, sector, group)` UTOC addresses and
 *   cluster counts.
 * - All component methods run exclusively on the JUCE message thread.
 */

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Editing/Editing.h"
#include "MiniDisc/MdAuthoring.h"
#include "Studio/StudioTheme.h"
#include "MiniDisc/NetMdBridge.h"
#include "MiniDisc/HiMdBridge.h"
#include "Core/Session.h"
#include "MiniDisc/Utoc.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

// ============================================================================
// UI Formatting, Colour, & Menu Helpers
// ============================================================================

namespace carta::md_ui {

/** Formats a 44.1 kHz sample offset as `MM:SS.t` (tenths of a second). */
inline juce::String formatClock(juce::int64 samples) {
    const double seconds =
        static_cast<double>(std::max<juce::int64>(0, samples)) / static_cast<double>(kMdSampleRate);
    const auto wholeTenths = static_cast<juce::int64>(std::llround(seconds * 10.0));
    const auto mins = wholeTenths / 600;
    const auto secs = (wholeTenths / 10) % 60;
    const auto tenths = wholeTenths % 10;
    return juce::String::formatted("%02lld:%02lld.%lld", static_cast<long long>(mins),
                                   static_cast<long long>(secs), static_cast<long long>(tenths));
}

/** Formats a 44.1 kHz sample offset as whole `MM:SS`. */
inline juce::String formatClockWhole(juce::int64 samples) {
    const auto totalSec = static_cast<juce::int64>(
        std::llround(static_cast<double>(std::max<juce::int64>(0, samples)) /
                     static_cast<double>(kMdSampleRate)));
    return juce::String::formatted("%02lld:%02lld", static_cast<long long>(totalSec / 60),
                                   static_cast<long long>(totalSec % 60));
}

/** Formats a physical UTOC address as `CCC:SS.G`. */
inline juce::String formatAddr(const MdPhysAddr& addr) {
    return juce::String::formatted("%03u:%02u.%u", static_cast<unsigned>(addr.cluster),
                                   static_cast<unsigned>(addr.sector),
                                   static_cast<unsigned>(addr.group));
}

/** Returns compact badge text ("SP-ST", "SP-MO", "LP2", "LP4", "Hi-SP", "Hi-LP", "PCM") for `mode`. */
inline juce::String codecShortLabel(MdCodecMode mode) {
    switch (mode) {
    case MdCodecMode::spStereo:
        return "SP-ST";
    case MdCodecMode::spMono:
        return "SP-MO";
    case MdCodecMode::lp2:
        return "LP2";
    case MdCodecMode::lp4:
        return "LP4";
    case MdCodecMode::hiSp:
        return "Hi-SP";
    case MdCodecMode::hiLp:
        return "Hi-LP";
    case MdCodecMode::pcm:
        return "PCM";
    }
    return "SP-ST";
}

/** Returns display name ("SP Stereo", "SP Mono", "LP2", "LP4", "Hi-SP", "Hi-LP", "Linear PCM") for `mode`. */
inline juce::String codecLongLabel(MdCodecMode mode) {
    switch (mode) {
    case MdCodecMode::spStereo:
        return "SP Stereo";
    case MdCodecMode::spMono:
        return "SP Mono";
    case MdCodecMode::lp2:
        return "LP2";
    case MdCodecMode::lp4:
        return "LP4";
    case MdCodecMode::hiSp:
        return "Hi-SP";
    case MdCodecMode::hiLp:
        return "Hi-LP";
    case MdCodecMode::pcm:
        return "Linear PCM";
    }
    return "SP Stereo";
}

/** Returns theme-aware accent colour for `mode`. */
inline juce::Colour codecColour(MdCodecMode mode, bool dark) {
    return carta::theme::codecColour(mode, dark);
}

/** Returns rotating 8-colour group accent palette entry for `groupIndex`. */
inline juce::Colour groupColour(int groupIndex, bool dark) {
    return carta::theme::groupColour(groupIndex, dark);
}

/** Maps `MdCodecMode` to 1-based JUCE ComboBox/PopupMenu item ID. */
inline int codecToMenuId(MdCodecMode mode) noexcept {
    switch (mode) {
    case MdCodecMode::spStereo:
        return 1;
    case MdCodecMode::spMono:
        return 2;
    case MdCodecMode::lp2:
        return 3;
    case MdCodecMode::lp4:
        return 4;
    case MdCodecMode::hiSp:
        return 5;
    case MdCodecMode::hiLp:
        return 6;
    case MdCodecMode::pcm:
        return 7;
    }
    return 1;
}

/** Maps 1-based JUCE menu ID back to `MdCodecMode` (defaults to SP Stereo). */
inline MdCodecMode menuIdToCodec(int id) noexcept {
    switch (id) {
    case 2:
        return MdCodecMode::spMono;
    case 3:
        return MdCodecMode::lp2;
    case 4:
        return MdCodecMode::lp4;
    case 5:
        return MdCodecMode::hiSp;
    case 6:
        return MdCodecMode::hiLp;
    case 7:
        return MdCodecMode::pcm;
    case 1:
    default:
        return MdCodecMode::spStereo;
    }
}

/** Maps `MdMedium` to 1-based JUCE ComboBox/PopupMenu item ID. */
inline int mediumToMenuId(MdMedium medium) noexcept {
    switch (medium) {
    case MdMedium::md60:
        return 1;
    case MdMedium::md74:
        return 2;
    case MdMedium::md80:
        return 3;
    case MdMedium::himd300:
        return 4;
    case MdMedium::himd1gb:
        return 5;
    }
    return 3;
}

/** Maps 1-based JUCE menu ID back to `MdMedium` (defaults to MD 80). */
inline MdMedium menuIdToMedium(int id) noexcept {
    switch (id) {
    case 1:
        return MdMedium::md60;
    case 2:
        return MdMedium::md74;
    case 4:
        return MdMedium::himd300;
    case 5:
        return MdMedium::himd1gb;
    case 3:
    default:
        return MdMedium::md80;
    }
}

/** Paints a rounded surface card with theme-appropriate fill and 1 px stroke. */
inline void paintPanelCard(juce::Graphics& g, juce::Rectangle<int> bounds, bool dark,
                           bool emphasized = false) {
    if (bounds.isEmpty()) {
        return;
    }
    const auto bg = dark ? juce::Colour(0xff262626) : juce::Colour(0xfffafafa);
    const auto border = emphasized ? (dark ? juce::Colour(0xff626262) : juce::Colour(0xff969696))
                                   : (dark ? juce::Colour(0xff3d3d3d) : juce::Colour(0xffd0d0d0));
    g.setColour(bg);
    g.fillRoundedRectangle(bounds.toFloat(), 5.0f);
    g.setColour(border);
    g.drawRoundedRectangle(bounds.toFloat().reduced(0.5f), 5.0f, 1.0f);
}

} // namespace carta::md_ui

#if CARTA_STUDIO_COMPONENT_TEST
struct StudioComponentTests;
#endif

// ============================================================================
// MdWorkspace Component
// ============================================================================

/**
 * Ergonomic two-tab MiniDisc UTOC authoring workspace docked below the timeline.
 * All mutations round-trip through the parent Studio IPC command executor so UI
 * actions, keyboard shortcuts, and CLI/IPC agents share one undoable state machine.
 */
class MdWorkspace final : public juce::Component, public juce::TooltipClient {
    class TrackListModel final : public juce::ListBoxModel {
      public:
        explicit TrackListModel(MdWorkspace& ownerRef);
        int getNumRows() override;
        void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                              bool selected) override;
        void selectedRowsChanged(int lastRowSelected) override;
        void listBoxItemClicked(int row, const juce::MouseEvent& event) override;
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent& event) override;
        void returnKeyPressed(int row) override;
        void deleteKeyPressed(int row) override;
        juce::String getTooltipForRow(int row) override;

      private:
        MdWorkspace& owner;
    };

    /** ListBox supporting vertical mouse-drag row reordering with insertion-bar feedback. */
    class ReorderableListBox : public juce::ListBox {
      public:
        ReorderableListBox(const juce::String& name, juce::ListBoxModel* modelRef);
        ~ReorderableListBox() override;

        bool dragReorderEnabled = false;
        std::function<bool(int row)> canStartDrag;
        std::function<void(int fromRow, int insertBeforeRow)> onRowReorder;

        void mouseDown(const juce::MouseEvent& event) override;
        void mouseDrag(const juce::MouseEvent& event) override;
        void mouseUp(const juce::MouseEvent& event) override;
        void paintOverChildren(juce::Graphics& g) override;

      private:
        bool isIgnoredChildComponent(const juce::Component* comp) const noexcept;
        int computeInsertBeforeRow(juce::Point<int> pt);

        bool dragCandidate = false;
        bool dragActive = false;
        int dragFromRow = -1;
        int dragInsertRow = -1;
        int dragStartY = 0;
    };

    class TrackListBox final : public ReorderableListBox {
      public:
        TrackListBox(MdWorkspace& ownerRef, TrackListModel& modelRef);
        bool keyPressed(const juce::KeyPress& key) override;

      private:
        MdWorkspace& owner;
    };

    /** Linear Target Recording Plan / Audio Bank list shown on Tab 2 ("Device & Record"). */
    class PlanListModel final : public juce::ListBoxModel {
      public:
        explicit PlanListModel(MdWorkspace& ownerRef);
        int getNumRows() override;
        void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                              bool selected) override;
        void selectedRowsChanged(int lastRowSelected) override;
        void listBoxItemClicked(int row, const juce::MouseEvent& event) override;
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent& event) override;
        void returnKeyPressed(int row) override;
        juce::String getTooltipForRow(int row) override;

      private:
        MdWorkspace& owner;
    };

    /** Compact on-disc track list model shown inside the Recorder card on Tab 2. */
    class DeviceTrackListModel final : public juce::ListBoxModel {
      public:
        explicit DeviceTrackListModel(MdWorkspace& ownerRef);
        int getNumRows() override;
        void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                              bool selected) override;
        void listBoxItemClicked(int row, const juce::MouseEvent& event) override;
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent& event) override;
        juce::String getTooltipForRow(int row) override;

      private:
        MdWorkspace& owner;
    };

    /** Inline track-title TextEditor supporting Up/Down row navigation on Tracks & UTOC. */
    class InlineTrackTitleEditor final : public juce::TextEditor {
      public:
        explicit InlineTrackTitleEditor(MdWorkspace& ownerRef);
        bool keyPressed(const juce::KeyPress& key) override;

      private:
        MdWorkspace& owner;
    };

#if CARTA_STUDIO_COMPONENT_TEST
    friend struct StudioComponentTests;
#endif

  public:
    /** Cached display row for one Play Order track in Tab 1. */
    struct TrackRow {
        juce::String id;
        juce::String audioId;
        int index0 = 0;
        juce::String title;
        juce::String fullWidthTitle;
        juce::String artist;
        juce::String album;
        std::string groupName;
        int groupColorIndex = -1;
        carta::MdCodecMode effectiveCodec = carta::MdCodecMode::spStereo;
        bool hasCodecOverride = false;
        bool writeProtected = false;
        bool scms = false;
        bool emphasis = false;
        bool hasTimestamp = false;
        int partCount = 0;
        int aliasRefCount = 1;
        bool sharesPhysicalSpan = false;
        int maxSharedRefs = 1;
        bool onDisc = false;
        bool forceGapBefore = false;
        juce::int64 totalSamples = 0;
        juce::int64 firstStartSample = 0;
        juce::int64 firstEndSample = 0;
    };

    /** Cached display row for one Play Order track mapped onto Audio Pool / physical spans on Tab 2. */
    struct PlanRow {
        int trackIndex = 0;
        juce::String id;
        juce::String audioId;
        juce::String title;
        std::string groupName;
        int groupColorIndex = -1;
        carta::MdCodecMode codec = carta::MdCodecMode::spStereo;
        int partCount = 0;
        int aliasRefCount = 1;
        int uniqueAudioSlot = 0;
        juce::int64 totalSamples = 0;
        int primarySpanIndex = -1;
        int spanClusterStart = 0;
        int spanClusterEnd = 0;
        int spanRefCount = 0;
        bool sharesPhysicalSpan = false;
        bool onDisc = false;
        bool forceGapBefore = false;
        bool gappedBefore = false;
        juce::String partHopsText;
    };

    /** Inverse Audio Pool / physical-to-logical map row for the Tab 2 Audio Bank view. */
    struct AudioBankRow {
        bool isOnDisc = false;
        bool isReclaimed = false;
        int onDiscRegionIndex = -1;
        int spanIndex = -1;
        juce::String audioId;
        int uniqueAudioSlot = 0;
        std::int64_t hiMdBytes = 0;
        juce::String label;
        carta::MdCodecMode codec = carta::MdCodecMode::spStereo;
        int startCluster = 0;
        int endCluster = 0;
        int clusterCount = 0;
        int unitCount = 0;
        bool forceGapBefore = false;
        bool gappedBefore = false;
        juce::int64 timelineStartSample = 0;
        juce::int64 timelineEndSample = 0;
        std::vector<int> referencingTrackIndices0;
        int totalPartRefs = 0;
    };

    /** Pickable timeline audio-bank region or local clip entry for "+ From Bank...". */
    struct BankClipEntry {
        juce::String label;
        juce::String layerName;
        juce::int64 startSample = 0;
        juce::int64 endSample = 0;
        bool isOnDisc = false;
        int onDiscRegionIndex = -1;
        int startCluster = 0;
        int endCluster = 0;
        carta::MdCodecMode codec = carta::MdCodecMode::spStereo;
    };

    /** Device, preflight, and live operation status pushed by the parent Studio window. */
    struct RecordUiSnapshot {
        std::vector<carta::NetMdDeviceEntry> devices;
        std::vector<carta::HiMdDeviceEntry> hiMdDevices;
        std::string activeDeviceId;
        bool activeDeviceIsHiMd = false;
        bool hiMdNeedsElevation = false;
        bool hasDeviceStatus = false;
        carta::NetMdDeviceStatus deviceStatus;
        bool hasHiMdStatus = false;
        carta::HiMdDeviceStatus hiMdStatus;
        juce::String badgeText;
        juce::String statusText;
        bool canLoadDisc = false;
        bool canDownloadAudio = false;
        bool downloadingAudio = false;
        bool canReadToc = false;
        bool canDumpTrack = false;
        bool canErase = false;
        bool canWriteUtoc = false;
        bool canManageHiMd = false;
        bool canRecord = false;
        bool busy = false;
        carta::NetMdProgressSnapshot progress;
        juce::String utocSummaryText;
    };

    MdWorkspace();

    // Callbacks connected to Studio
    std::function<bool(const juce::StringArray& command, juce::uint64 revision)> onCommand;
    std::function<void()> onImportAudio;
    std::function<void()> onClose;
    std::function<void()> onSyncFromClips;
    std::function<void()> onEditDiscFormsAndTimestamp;
    std::function<void()> onEditTrackForms;
    std::function<void()> onEditTrackFlags;
    std::function<void()> onReviewWav;
    std::function<void(const juce::String& trackId, int partIndex, juce::uint64 revision)>
        onSeekPart;
    std::function<void(juce::int64 startSample, juce::int64 endSample, juce::uint64 revision)>
        onSeekRange;
    std::function<void(const juce::String& trackId, bool throughEnd)> onPreviewTrack;
    std::function<void()> onInspectUtoc;
    std::function<void()> onImportUtoc;
    std::function<void()> onExportUtoc;
    std::function<void()> onRefreshRecord;
    std::function<void()> onRecordPreflight;
    std::function<void()> onLoadDisc;
    std::function<void()> onDownloadDiscAudio;
    std::function<void()> onCancelDownloadAudio;
    std::function<void()> onReadDiscToc;
    std::function<void()> onEraseDisc;
    std::function<void()> onRecordWriteUtoc;
    std::function<void()> onRecordStart;
    std::function<void(int trackIndex)> onDumpHiMdTrack;
    std::function<void()> onHiMdDiscTools;
    std::function<void(int trackIndex)> onRenameHiMdTrack;
    std::function<void(int trackIndex, int delta)> onMoveHiMdTrack;
    std::function<void(int trackIndex)> onDeleteHiMdTrack;
    std::function<void(const juce::String& tab)> onTabChanged;
    std::function<void(const juce::String& message)> onFeedback;
    std::function<void(const juce::String& trackId)> onTrackSelectionChanged;
    std::function<void(int deltaY)> onResize;
    std::function<void()> onResizeEnd;

    juce::String getTooltip() override;
    void syncAppearance();

    juce::String currentView() const noexcept;
    bool selectView(const juce::String& view, juce::String& error);
    void setActiveTab(const juce::String& tab, bool notify = true);
    juce::String currentPlanViewMode() const noexcept;
    void setPlanViewMode(const juce::String& mode);

    bool hasPendingProgramEdits() const noexcept;
    void commitPendingEdits();

    /** Rebuilds workspace models from `session.mdDisc`, selection, playhead, and generation. */
    void update(const carta::Session& session, const carta::TimeSelection& selection,
                double playheadSec, juce::uint64 generation,
                const juce::String& preferredTrackId = {});

    /** Updates Tab 2 NetMD / Hi-MD device selector, preflight status badges, and action
     * enablement. */
    void updateRecordSummary(const RecordUiSnapshot& snapshot);

    juce::String selectedRecordDeviceId() const;
    bool selectedRecordDeviceIsHiMd() const noexcept;
    int selectedDeviceTrackIndex0() const noexcept;
    std::vector<int> selectedTrackIndices0() const;
    int singleSelectedTrackIndex() const;
    juce::String primaryTrackId() const;
    void selectTrackById(const juce::String& trackId, bool scrollToRow = true);
    void focusTrackRename();

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& event) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;

  private:
    void layoutRightInspector(juce::Rectangle<int> area);
    void layoutDeviceCard(juce::Rectangle<int> area);
    void layoutPlanCard(juce::Rectangle<int> area);
    void updateTabVisibility();
    void refreshPlanSummaryLabels();
    void syncTrackInspectorFields();
    void updateButtonStates();
    void handleTrackSelectionChanged(int lastRowSelected);
    void handlePlanSelectionChanged(int lastRowSelected);
    void handlePlanClick(int row, const juce::MouseEvent& event);
    void triggerOpenPlanRow(int row);
    void handleTrackClick(int row, const juce::MouseEvent& event);
    void handleTrackDoubleClick(int row);
    void beginInlineTrackEdit(int row);
    void commitInlineTrackEdit(bool closeEditor = true);
    void cancelInlineTrackEdit();
    void navigateInlineTrackEdit(int rowDelta);
    void updateInlineTrackEditorBounds();
    void commitDiscTitle();
    void commitTrackTitle();
    void triggerInsertTrack();
    void showInsertAliasMenu();
    void triggerDuplicateTrack();
    void triggerSplitTrack();
    void triggerMoveTrack(int delta);
    void triggerRemoveSelectedTracks();
    void triggerMergeSelectedTracks();
    void triggerCopySelectedTracks();
    void triggerPasteTracksAfter(int afterRow);
    void promptMoveTrackToPosition(int row);
    void handleTrackReorderDrag(int fromRow, int insertBeforeRow);
    void handlePlanReorderDrag(int fromRow, int insertBeforeRow);
    void triggerPreviewSelected(bool throughEnd = false);
    void triggerSeekTrackRange();
    void triggerSetTrackToSelection();
    void triggerMakeUnique();
    void populateAudioBankSubmenu(juce::PopupMenu& menu, int baseId, int chunkSize = 30) const;
    void populateAudioPoolSubmenu(juce::PopupMenu& menu, int baseId, int chunkSize = 30) const;
    void showTrackContextMenu(int row);
    void showPlanContextMenu(int row);
    void showAudioBankContextMenu(int row);
    void showGroupMenu();
    void promptCreateGroupForSelection();
    void clearGroupsForSelection();
    void paintTrackRow(int row, juce::Graphics& g, int width, int height, bool selected) const;
    void paintAudioSourceCard(juce::Graphics& g, bool dark) const;
    void paintDeviceTrackRow(int row, juce::Graphics& g, int width, int height,
                             bool selected) const;
    void handleDeviceTrackClick(int row, const juce::MouseEvent& event);
    void syncCodecDropdownsForMedium(bool isHiMd);
    juce::String tooltipForTrackRow(int row) const;
    juce::String tooltipForDeviceTrackRow(int row) const;
    static juce::String formatSecMmSs(int totalSeconds);
    void updatePanelHoverTooltip(juce::Point<int> point);
    void paintPhysicalDiscMeter(juce::Graphics& g, juce::Rectangle<int> bounds, bool dark) const;
    void paintTargetCapacityBar(juce::Graphics& g, juce::Rectangle<int> bounds, bool dark) const;
    void paintPlanProgressBar(juce::Graphics& g, juce::Rectangle<int> bounds, bool dark) const;
    void paintPlanRow(int row, juce::Graphics& g, int width, int height, bool selected) const;
    void paintAudioBankRow(int row, juce::Graphics& g, int width, int height, bool selected) const;
    juce::String tooltipForPlanRow(int row) const;
    juce::String tooltipForAudioBankRow(int row) const;

    // State
    carta::MdDisc discState;
    carta::MdPhysicalPlan physicalPlan;
    carta::UtocBudget budget{};
    carta::TimeSelection timeSelection{};
    juce::int64 playheadSample = 0;
    juce::int64 masterEndSample = 0;
    juce::uint64 revision = 0;
    bool planValid = true;
    bool utocValid = true;
    std::string planErrorText;
    std::string utocErrorText;
    std::int64_t hiMdUsedBytes = 0;
    std::int64_t hiMdTotalBytes = 0;
    int lastCodecMenuKind = -1; // -1 = uninitialized, 0 = Standard MD, 1 = Hi-MD

    juce::String activeTab = "tracks";
    juce::String planViewMode = "playback";
    juce::String discBaselineTitle;
    juce::String trackBaselineTitle;
    juce::String budgetPillText;
    juce::String lastHoverTooltip;
    bool syncingControls = false;
    bool recordBusy = false;
    bool resizingPanel = false;
    int resizeStartScreenY = 0;
    int inlineEditingRow = -1;
    juce::String inlineBaselineTitle;
    bool inlineTransitioning = false;

    std::vector<TrackRow> trackRows;
    std::vector<PlanRow> planRows;
    std::vector<AudioBankRow> audioBankRows;
    std::vector<BankClipEntry> audioBankClipEntries;
    std::vector<carta::MdTrack> copiedTrackClipboard;
    RecordUiSnapshot recordUiState;

    // Layout rects painted by MdWorkspace
    juce::Rectangle<int> budgetPillBounds;
    juce::Rectangle<int> leftCardBounds;
    juce::Rectangle<int> rightCardBounds;
    juce::Rectangle<int> trackHeaderBounds;
    juce::Rectangle<int> audioSourceCardBounds;
    juce::Rectangle<int> deviceCardBounds;
    juce::Rectangle<int> planCardBounds;
    juce::Rectangle<int> deviceDiscMeterBounds;
    juce::Rectangle<int> targetBarBounds;
    juce::Rectangle<int> planProgressBounds;

    // Header widgets
    juce::Label mdLabel;
    juce::TextEditor discTitleEditor;
    juce::TextButton discFormsButton{"Forms & TS..."};
    juce::ComboBox mediumBox;
    juce::ComboBox codecBox;
    juce::ToggleButton autoTrackToggle{"Auto from Clips"};
    juce::TextButton closeButton{"x"};

    // Tab & Sync widgets
    juce::TextButton tracksTabButton{"Play Order"};
    juce::TextButton recordTabButton{"Device & Record"};
    juce::TextButton syncClipsButton{"Tracks from Clips"};
    juce::TextButton previewTrackButton{"Preview"};
    juce::TextButton previewFromButton{"From Here"};
    juce::TextButton reviewWavButton{"Review WAV..."};
    juce::TextButton inspectUtocButton{"Inspect UTOC..."};
    juce::TextButton importUtocButton{"Import UTOC..."};
    juce::TextButton exportUtocButton{"Export UTOC..."};
    juce::Label planHintLabel;

    // Left pane widgets
    juce::TextButton insertTrackButton{"+ Insert"};
    juce::TextButton insertAliasButton{"+ Alias..."};
    juce::TextButton duplicateTrackButton{"Alias"};
    juce::TextButton splitTrackButton{"Split"};
    juce::TextButton moveUpButton{"Up"};
    juce::TextButton moveDownButton{"Down"};
    juce::TextButton removeTrackButton{"Remove"};
    juce::TextButton groupMenuButton{"Group..."};
    TrackListModel trackModel;
    TrackListBox trackList;
    InlineTrackTitleEditor inlineTrackTitleEditor;

    // Right pane: Empty state
    juce::Label emptyTitleLabel;
    juce::Label emptyBodyLabel;
    juce::TextButton emptySyncClipsButton{"Tracks from Clips"};
    juce::TextButton emptyImportButton{"Import Audio..."};

    // Right pane: Single-track inspector
    juce::Label trackTitleLabel;
    juce::TextEditor trackTitleEditor;
    juce::TextButton trackFormsButton{"Forms..."};
    juce::TextButton trackFlagsButton{"Flags..."};
    juce::Label trackCodecLabel;
    juce::ComboBox trackCodecBox;
    juce::TextButton trackGroupButton{"Group: None..."};
    juce::Label audioSourceHeaderLabel;
    juce::TextButton seekAudioButton{"Seek"};
    juce::TextButton setAudioSelButton{"Set to Sel"};
    juce::TextButton detachAliasButton{"Make Unique"};

    // Right pane: Multi-selection inspector
    juce::Label multiHeaderLabel;
    juce::TextButton multiGroupButton{"Group Selected..."};
    juce::TextButton multiUngroupButton{"Ungroup"};
    juce::TextButton multiDuplicateButton{"Alias Selected"};
    juce::TextButton multiRemoveButton{"Remove All"};
    juce::TextButton multiMergeButton{"Merge Selected"};
    juce::TextButton multiCopyButton{"Copy Selected"};

    // Tab 2 widgets
    juce::Label deviceCardTitle;
    juce::ComboBox recordDeviceBox;
    std::vector<std::string> recordDeviceIds;
    std::vector<bool> recordDeviceIsHiMd;
    juce::TextButton refreshRecordButton{"Refresh"};
    juce::Label recordBadgeLabel;
    DeviceTrackListModel deviceTrackModel;
    juce::ListBox deviceTrackList;
    juce::TextButton loadDiscButton{"Load Disc"};
    juce::TextButton downloadAudioButton{"Download Audio"};
    juce::TextButton readDiscTocButton{"Read Disc TOC..."};
    juce::TextButton eraseDiscButton{"Erase Disc..."};
    juce::Label planCardTitle;
    juce::TextButton planModePlaybackButton{"Playback"};
    juce::TextButton planModeBankButton{"Audio Bank"};
    juce::Label utocSummaryLabel;
    PlanListModel planModel;
    ReorderableListBox planList;
    juce::Label recordStatusLabel;
    juce::TextButton writeUtocButton{"Write UTOC Only..."};
    juce::TextButton recordDiscButton{"Record MiniDisc..."};
};
