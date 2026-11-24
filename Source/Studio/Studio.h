/**
 * @file Studio.h
 * @brief Main Carta Studio DAW timeline, transport, MiniDisc authoring, and NetMD editor component.
 *
 * `Studio` coordinates the interactive audio timeline, non-destructive session
 * editing, real-time playback, MiniDisc UTOC authoring workspace, background
 * worker jobs, and local IPC automation.
 *
 * Coordinate systems:
 * - Project edits, selections, markers, and UTOC parts use 44.1 kHz project
 *   samples (`carta::sessionRate`) over half-open `[start, end)` intervals.
 * - Playback position (`position`), viewport bounds (`viewStart`, `viewLength()`),
 *   and clip layout times (`clip.start`, `clip.duration`, `clip.offset`) are
 *   expressed in seconds.
 * - Physical MiniDisc layout uses Rainbow Book UTOC sectors (0/1/2/4) and
 *   clusters/sectors/sound-groups compiled from `MdDisc`.
 *
 * Threading and locking invariants:
 * - The JUCE message thread owns all child components, `editGeneration`,
 *   `editHistory`, selection/drag state, waveform/spectrum caches, and
 *   background task shared pointers (`activeExport`, `activeOpen`, etc.).
 * - `lock` (`juce::CriticalSection`) guards shared audio-callback state
 *   (`session`, `position`, `auditionRange`, `savedLoop`, `masterChannels`,
 *   `deviceRate`, `resampling`, `playbackClock`, `playbackIndex`, etc.).
 *   `getNextAudioBlock` uses `ScopedTryLock` and never blocks the audio thread.
 * - Lock-free atomics (`playing`, `audio*` flags, `editLockSilentBlocks`,
 *   `netMdBusy`, `netMdDonePending`, `netMdStopRequested`) bridge worker/audio
 *   threads and the 20 Hz UI timer.
 * - `netMdMutex` protects `NetMdWorkerThread` status and pending UI alerts.
 */

#pragma once

#if CARTA_STUDIO_MINIMP3
#include "IO/NativeMp3Format.h"
#endif
#include "Core/Session.h"
#include "MiniDisc/MdAuthoring.h"
#include "IO/SessionIO.h"
#include "MiniDisc/Utoc.h"
#include "MiniDisc/NativeCodecs.h"
#include "MiniDisc/NetMdBridge.h"
#include "MiniDisc/HiMdBridge.h"
#include "Editing/Editing.h"
#include "Audio/Transport.h"
#include "Audio/Spectrogram.h"
#include "Audio/DeviceSettings.h"
#include "MiniDisc/WorkspacePreferences.h"
#include "Platform/Ipc.h"
#include "Platform/NativeSavePrompt.h"
#include "MiniDisc/MdWorkspace.h"
#if JUCE_LINUX || CARTA_STUDIO_COMPONENT_TEST
#include "Audio/StableSpectrumRaster.h"
#endif
#include <map>
#include <set>
#include <juce_audio_utils/juce_audio_utils.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <charconv>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <atomic>
#include <tuple>

#include "Studio/StudioLookAndFeel.h"
#include "Studio/StudioDialogs.h"

/**
 * @brief Top-level DAW editor, audio source, drag-and-drop target, and IPC host component.
 */
class Studio final : public juce::AudioAppComponent,
                     public juce::FileDragAndDropTarget,
                     public juce::TooltipClient,
                     private juce::Timer,
                     private juce::MenuBarModel,
                     private juce::ScrollBar::Listener,
                     private juce::ApplicationCommandTarget {
#if CARTA_STUDIO_COMPONENT_TEST
    friend struct StudioComponentTests;
#endif
    struct IpcRouter;

    using NeutralLook = StudioNeutralLook;
    using AudioSettingsWindow = StudioAudioSettingsWindow;

    // Active drag gesture identifiers stored in `dragMode`.
    static constexpr int kDragNone = 0;
    static constexpr int kDragClipMove = 1;
    static constexpr int kDragClipTrimLeft = 2;
    static constexpr int kDragClipTrimRight = 3;
    static constexpr int kDragMarkerOrMasterEnd = 4;
    static constexpr int kDragTrackHeight = 5;
    static constexpr int kDragTimeSelection = 6;
    static constexpr int kDragTimeSelectionResize = 7;
    static constexpr int kDragPlayhead = 8;
    static constexpr int kDragGroupMove = 9;
    static constexpr int kDragGroupEdge = 10;
    static constexpr int kDragSavedLoop = 11;
    static constexpr int kDragTrackReorder = 12;
    static constexpr int kDragChannelHeight = 13;
    static constexpr int kDragMarquee = 14;
    // Value 15 is intentionally unused.
    static constexpr int kDragHeaderControl = 16;
    static constexpr int kDragMdRibbon = 17;

    struct HistoryMenuCommand {
        juce::uint64 generation = 0;
        juce::uint64 revision = 0;
        size_t steps = 0;
        bool redo = false;
    };

    struct ImportTask {
        ImportTask(juce::File f, juce::int64 start) : file(std::move(f)), startSample(start) {}

        juce::File file;
        juce::int64 startSample = 0;
        int targetTrack = -1;
        juce::uint64 targetRevision = 0;
        juce::uint64 batchId = 0;
        bool sequential = false;
        bool newLayer = true;
        bool autoMarker = false;
        bool autoMdTrack = false;
        std::atomic<bool> cancelled{false};
        std::atomic<double> progress{0};
        // Written and consumed on the message thread after decoding finishes.
        bool ready = false;
        std::shared_ptr<carta::Audio> audio;
        std::shared_ptr<const carta::SincResampler> kernel;
        juce::String error;
    };

    struct ImportCache {
        std::map<std::pair<double, double>, std::weak_ptr<const carta::SincResampler>> kernels;
    };

    struct OpenTask {
        juce::File file;
        std::atomic<bool> cancelled{false};
        std::atomic<double> progress{0};
        juce::uint64 generation = 0;
        double outputRate = 0;
        bool requireRecovery = false;
        bool ipc = false;
        carta::WavRange originalLoop;
        carta::SourceRelinks relinks;
    };

    struct OpenResult {
        carta::Session session;
        int channels = 2;
        bool recovered = false;
        bool relinked = false;
        bool assignedMarkerIds = false;
        bool nativePackage = false;
        carta::SourceRelinkRequest relinkRequest;
        carta::WavRange savedLoop;
        std::map<double, std::shared_ptr<const carta::SincResampler>> kernels;
    };

    struct RecoveryTask {
        juce::File file;
        juce::uint64 revision = 0;
        bool announce = false;
        bool nativePackage = false;
        carta::WavRange savedLoop;
        std::atomic<bool> cancelled{false};
        std::atomic<bool> retired{false};
        carta::Session snapshot;
    };

    struct SaveTask {
        juce::File file;
        std::atomic<bool> cancelled{false};
        std::atomic<double> progress{0};
        juce::uint64 revision = 0;
        bool portable = false;
        bool ipc = false;
        bool noReplace = false;
        bool nativePackage = false;
        carta::Session snapshot;
        carta::WavRange savedLoop;
    };

    struct SourceCheckTask {
        std::atomic<bool> cancelled{false};
        std::atomic<double> progress{0};
        juce::uint64 generation = 0;
        juce::String name;
        std::vector<carta::TrackSource> sources;
        juce::File relinkFile;
        bool relinking = false;
        bool ipc = false;
    };

    struct MarkerExchangeTask {
        juce::File file;
        carta::Session snapshot;
        juce::uint64 generation = 0;
        bool writing = false;
        bool audacity = false;
        bool ipc = false;
        std::atomic<bool> cancelled{false};
    };

    struct LevelAnalysisTask {
        std::atomic<bool> cancelled{false};
        std::atomic<double> progress{0};
        juce::uint64 generation = 0;
        carta::Session snapshot;
        juce::String progressLabel = "Levels";
        bool ipc = false;
    };

    struct ExportTask {
        std::atomic<bool> cancelled{false};
        std::atomic<double> progress{0};
        std::atomic<int> phase{0}; // 0: preflight, 1: writing, 2: awaiting clipping decision.
        juce::File target;
        carta::WavOptions wavOptions;
        float peak = 0;
        bool batch = false;
        bool ipc = false;
        bool noReplace = false;
        bool renderTracks = false;
        bool allowClip = false;
        bool discProgram = false;
        juce::uint64 snapshotGeneration = 0;
        std::vector<carta::MasterTrack> tracks;
        carta::Session snapshot;
        carta::Session verificationSnapshot;
#if CARTA_STUDIO_COMPONENT_TEST
        std::function<bool(double)> writeProgressHook;
        std::function<void()> verificationHook;
#endif
    };

    enum class RenderScope { selectedTime, tracks, clips, consolidate, stereoPair };

    enum class MdRibbonGesture {
        none,
        emptySpace,
        sharedSplit,
        leftEdge,
        rightEdge,
        slideBody,
        duplicateAlias
    };

    struct MdRibbonHit {
        int trackIndex = -1;
        int partIndex = -1;
        juce::Rectangle<int> bounds;
        juce::String trackId;
        juce::String title;
        int groupColorIndex = -1;
        std::int64_t startSample = 0;
        std::int64_t endSample = 0;
        bool sharesPhysical = false;
        int sharedCount = 1;
    };

    struct MdRibbonPointerInfo {
        MdRibbonGesture gesture = MdRibbonGesture::none;
        int trackIndex = -1;
        int partIndex = -1;
        int rightTrackIndex = -1;
        int rightPartIndex = -1;
        juce::String trackId;
        juce::String title;
        std::int64_t boundarySample = 0;
    };

    struct ChannelBoundary {
        int track = -1;
        int lane = -1;
        int pairTop = 0;
        int pairBottom = 0;
    };

    struct NetMdWorkerThread final : public juce::Thread {
        explicit NetMdWorkerThread(std::function<void()> fn)
            : juce::Thread("CartaNetMD", 8u * 1024u * 1024u), bodyFn(std::move(fn)) {
            startThread();
        }
        ~NetMdWorkerThread() override { waitForThreadToExit(-1); }
        void run() override {
            if (bodyFn) {
                bodyFn();
            }
        }
        std::function<void()> bodyFn;
    };

    struct TrackHeaderControls {
        int track = -1;
        juce::Rectangle<int> collapse;
        juce::Rectangle<int> mute;
        juce::Rectangle<int> solo;
        juce::Rectangle<int> gain;
        juce::Rectangle<int> balance;
    };

  public:
    // ------------------------------------------------------------------------
    // Lifecycle, IPC, Drag-and-Drop, Audio Callback, and Timeline Events
    // ------------------------------------------------------------------------
    explicit Studio(bool headless = false);
    ~Studio() override;

    /** @brief Executes a synchronous IPC command on the message thread and returns `{ok,
     * result|error}`. */
    juce::var ipcCommand(const juce::StringArray& args);

    /** @brief Queues decoding and timeline placement for an audio file. */
    bool importFile(const juce::File& file, juce::int64 startSample = -1, int targetTrack = -2,
                    int markerPolicy = -1, bool forceNewLayer = false, juce::uint64 batchId = 0);

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragEnter(const juce::StringArray&, int x, int y) override;
    void fileDragMove(const juce::StringArray&, int x, int y) override;
    void fileDragExit(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    /** @brief Returns true while an in-progress timeline drag has uncommitted edits. */
    bool hasPendingGestureEdit() const;

    /** @brief Returns true if unsaved changes or an uncommitted gesture edit exist. */
    bool hasUnsavedChanges();

    /** @brief Marks exit as approved so shutdown retires recovery snapshots cleanly. */
    void approveCleanExit() noexcept;

    /** @brief Prompts to save unsaved changes before running `close` and exiting. */
    void requestClose(std::function<void()> close);

    void prepareToPlay(int, double rate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override;
    void resized() override;

    // ------------------------------------------------------------------------
    // MiniDisc Ribbon Layout, Hit-Testing, and Timeline Painting / Pointer Input
    // ------------------------------------------------------------------------
    std::vector<int> timelineMarkerPixels(juce::Rectangle<int> area, double span) const;
    int visibleMdRibbonHeight() const;
    int timelineHeaderHeight() const;
    juce::Rectangle<int> mdRibbonArea() const;
    static juce::Colour mdRibbonGroupColour(int colorIdx, bool dark);
    std::vector<MdRibbonHit> layoutMdRibbonHits(juce::Rectangle<int> area, double span,
                                                juce::Rectangle<int> ribbon) const;
    std::optional<MdRibbonHit> mdRibbonHitAt(juce::Point<int> point) const;
    MdRibbonPointerInfo classifyMdRibbonPointer(juce::Point<int> point,
                                                const juce::ModifierKeys& mods) const;
    void drawMdRibbon(juce::Graphics& g, juce::Rectangle<int> area, double span, bool dark) const;
    void paint(juce::Graphics& g) override;
    void showMdRibbonContextMenu(const juce::MouseEvent& event, const MdRibbonPointerInfo& info);
    void updateDraggedMdRibbon(const juce::ModifierKeys& mods);

    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent&) override;
    juce::String getTooltip() override;
    void mouseDoubleClick(const juce::MouseEvent& event) override;
    void mouseMagnify(const juce::MouseEvent& event, float scaleFactor) override;
    void mouseWheelMove(const juce::MouseEvent& event,
                        const juce::MouseWheelDetails& wheel) override;

  private:
    // ------------------------------------------------------------------------
    // Timeline Scope, Playback Master Range, and Accessibility
    // ------------------------------------------------------------------------
    std::vector<int> selectedTimelineTracks() const;
    bool trackInTimeScope(int track) const;
    juce::String describeTimelineTracks() const;
    void toggleTimelineTrack(int track);
    carta::PlaybackRange playbackMasterRange();
    void refreshCommittedMasterRange();
    const carta::RenderClipIndex* playbackIndexForCurrentSession() const noexcept;
    void queuePlaybackIndex();
    void prepareClipDragEnd();
    void refreshClipDragRange();
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    void focusTimeline();
    juce::String describeWaveformDisplay() const;
    void announceTimeline();

    // ------------------------------------------------------------------------
    // Interactive Drag Preview Updates & Context Menus
    // ------------------------------------------------------------------------
    int clipDropTrackAt(juce::Point<int> point) const;
    void updateClipDropTarget();
    void updateGroupEdgeDrag();
    void finishGroupEdgeDrag(juce::Point<int> point);
    void updateGroupDrag();
    void finishGroupDrag(juce::Point<int> point);
    void updateDraggedMasterEnd();
    void updateDraggedClip();
    void updateDraggedPlayhead();
    void updateDraggedLoop();
    void updateDraggedSelection();
    bool showMarkerContextMenu(const juce::MouseEvent& event);
    void addTrackWaveformZoomMenu(juce::PopupMenu& parent, int trackIndex) const;
    bool showTrackContextMenu(const juce::MouseEvent& event);
    bool showSelectionContextMenu(const juce::MouseEvent& event);
    juce::PopupMenu selectedClipContextMenu() const;
    bool performContextClipEdit(int track, int clip, juce::uint64 revision, bool splitting,
                                juce::int64 sample = 0);
    void showClipContextMenu(const juce::MouseEvent& event);
    bool hasAnyAudioSelection() const;
    void clearAudioSelection(bool announce = false, bool lockHeld = true);
    void syncSnapButtonAppearance();
    void splitMdTrackAtPlayhead();
    void updateAppearance();

    // ------------------------------------------------------------------------
    // Menu Bar, Commands, Shortcuts, and Timeline Editing Actions
    // ------------------------------------------------------------------------
    juce::StringArray getMenuBarNames() override;
    juce::ApplicationCommandTarget* getNextCommandTarget() override;
    bool canUseSelectionForDisc(bool updateRange);
    void useSelectionForDisc(bool updateRange);
    void getAllCommands(juce::Array<juce::CommandID>& ids) override;
    void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override;
    bool perform(const InvocationInfo& info) override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String&) override;
    void menuItemSelected(int id, int) override;
    bool keyPressed(const juce::KeyPress& key) override;

    bool duplicateTimeSelection();
    bool copySelection(bool cut);
    bool trimSelectionTracks();
    void deleteSelection();
    bool resolveRippleScope(bool focusedTrack, carta::TimeSelection& scope);
    void rippleSelection(bool allTracks, bool focusedTrack = false);
    void rippleCutSelection(bool allTracks, bool focusedTrack = false);
    void insertSelectionGap(bool allTracks, bool focusedTrack = false);
    bool addMarkerAtEditPoint(const juce::String& name);
    void setEditPointAtPlayhead();
    bool selectFromEditPoint(int action);
    bool selectGapAtEditPoint(int scope = 0, const std::vector<int>& explicitTracks = {});
    bool navigateEditPoint(int action);
    void nudgeEditPoint(int direction);
    void setFollowMode(int newMode);
    void suspendFollowingForNavigation();
    void resumeFollowing();
    void pasteSelection(int policy = 0, bool atPoint = false);
    carta::EditorState captureState() const;
    carta::EditorState captureSelectionState();
    bool reorderCurrentTrack(int source, int destination);

    /**
     * @brief Remaps clip, time, and independent-layer selections after track reordering or
     * deletion.
     */
    template <class Map>
    void restoreMappedTrackSelections(const carta::EditorState& before, Map remap,
                                      int oldTrackCount = -1) {
        const int oldCount =
            oldTrackCount >= 0 ? oldTrackCount : static_cast<int>(before.session.tracks.size());
        auto selection = before.selection;
        std::vector<carta::ClipReference> clips;
        for (auto clip : before.selectedClips) {
            clip.track = remap(clip.track);
            if (clip.track >= 0) {
                clips.push_back(clip);
            }
        }
        std::set<int> rows;
        const bool explicitRows = !before.selectedTracks.empty();
        for (int row : before.selectedTracks) {
            const int mapped = remap(row);
            if (mapped >= 0) {
                rows.insert(mapped);
            }
        }
        if (!explicitRows && selection.hasRange() && selection.lastTrack < oldCount) {
            for (int row = selection.firstTrack; row <= selection.lastTrack; ++row) {
                const int mapped = remap(row);
                if (mapped >= 0) {
                    rows.insert(mapped);
                }
            }
        }
        if (selection.hasRange()) {
            if (rows.empty()) {
                selection = {};
            } else {
                selection.firstTrack = *rows.begin();
                selection.lastTrack = *rows.rbegin();
                if (!explicitRows && static_cast<size_t>(selection.lastTrack -
                                                         selection.firstTrack + 1) == rows.size()) {
                    rows.clear();
                }
            }
        } else if (selection.firstTrack >= 0 && selection.lastTrack >= selection.firstTrack &&
                   selection.lastTrack < oldCount) {
            const int first = remap(selection.firstTrack);
            const int last = remap(selection.lastTrack);
            if (first < 0 || last < 0) {
                selection = {};
            } else {
                selection.firstTrack = std::min(first, last);
                selection.lastTrack = std::max(first, last);
            }
        }
        timeSelection = selection;
        if (!timelineClipSelection.restore(session, clips, editGeneration)) {
            timelineClipSelection.clear();
        }
        timelineTracks = std::move(rows);
        timelineTrackRevision = editGeneration;
        keyboardRange.active = false;
        menuItemsChanged();
        repaint();
    }

    // ------------------------------------------------------------------------
    // Undo History, Loops, Snap, and Transactional Session Mutation Helpers
    // ------------------------------------------------------------------------
    bool changeTracks(int operation, int source, const juce::String& name, juce::String& error);
    bool reorderTrack(int source, int destination, carta::EditorState before);
    void applyStateFields(const carta::EditorState& state, bool keepTransport = false);
    void applyState(carta::EditorState state);
    size_t traverseHistory(bool redo, size_t steps, bool& pruned);
    void restoreHistory(bool redo);
    void jumpHistory(const HistoryMenuCommand& command);
    void setAuditionRange(carta::PlaybackRange range, bool saved = false);
    bool savedLoopAudition() const;
    bool savedLoopPlaying() const;
    bool selectionPlaybackContinues() const;
    bool loopDirty() const noexcept;
    juce::String describeSavedLoop(bool loopPlaying) const;
    juce::String describeSavedLoop() const;
    void prepareSelectionFocus();
    bool setSavedLoop(juce::int64 start, juce::int64 end, juce::String& error);
    bool clearSavedLoop();
    bool startSavedLoop(juce::String& error);
    bool setSnapGrid(const juce::String& text);
    void editSnapGrid();
    juce::String describeEditLockInterruptions() const;
    void resetOutputPeaks();
    void resetEditLockInterruptions();
    void stopAtPlayhead();
    void stopSpecialAudition();
    double editPlayhead() const noexcept;
    bool setMinimumMasterDuration(juce::uint64 revision, juce::int64 end, juce::String& error);
    void editMinimumMasterDuration();
    bool dialogTargetCurrent(juce::uint64 revision);
    void retainIndexStableSelections(juce::uint64 previousGeneration);

    /**
     * @brief Runs an in-place locked edit `f` and records undo history if marker bounds remain
     * valid.
     */
    template <class F>
    bool mutate(F f, juce::String* rejection, juce::String action, bool retainSelection = false,
                bool renderGeometryStable = false, bool allowOnDiscMutation = false) {
        const juce::ScopedLock guard(lock);
        if (dragMode != kDragNone) {
            const juce::String message = "Finish or cancel the current drag first.";
            status.setText(message, juce::dontSendNotification);
            if (rejection) {
                *rejection = message;
            }
            return false;
        }
        stopSpecialAudition();
        auto old = captureState();
        const int oldFirstTrack = firstTrack;
        const auto previousGeneration = editGeneration;
        bool changed = f();
        if (changed) {
            juce::String error;
            if ((allowOnDiscMutation || carta::preservesOnDiscAudio(old.session, session, error)) &&
                (renderGeometryStable || carta::preservesMarkerBounds(old.session, session, error))) {
                pushHistory(std::move(old), std::move(action), renderGeometryStable);
                if (retainSelection) {
                    retainIndexStableSelections(previousGeneration);
                }
            } else {
                firstTrack = oldFirstTrack;
                applyState(std::move(old));
                status.setText(error, juce::dontSendNotification);
                if (rejection) {
                    *rejection = error;
                }
                changed = false;
            }
        }
        repaint();
        return changed;
    }

    void pushHistory(carta::EditorState old, juce::String action,
                     bool renderGeometryStable = false);

    /**
     * @brief Prepares a candidate session edit while unlocked so audio playback continues
     * uninterrupted.
     *
     * Must be called with `lock` held on the message thread. Rejects publication if
     * `editGeneration` changed while unlocked.
     */
    template <class Prepare, class Publish>
    bool prepareRangeSessionEdit(Prepare prepare, Publish publish, juce::String action,
                                 juce::String& error, bool renderGeometryStable = false,
                                 bool allowOnDiscMutation = false) {
        const auto generation = editGeneration;
        carta::Session candidate;
        carta::PlaybackRange candidateRange;
        std::vector<carta::ClipReference> historyClips;
        std::vector<int> historyTracks;
        bool changed = false;
        {
            const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
            if (rangePreparationHook) {
                rangePreparationHook();
            }
#endif
            candidate = session;
            changed = prepare(candidate) &&
                      (allowOnDiscMutation ||
                       carta::preservesOnDiscAudio(session, candidate, error));
            if (changed) {
                candidateRange =
                    renderGeometryStable && masterRangeValid && masterRangeRevision == generation
                        ? masterRange
                        : carta::fullMasterPlaybackRange(candidate);
#if CARTA_STUDIO_COMPONENT_TEST
                if (historySelectionSnapshotHook) {
                    historySelectionSnapshotHook();
                }
#endif
                historyClips = timelineClipSelection.get(generation);
                historyTracks = selectedTimelineTracks();
            } else {
                candidate = {};
            }
        }
        if (changed && generation != editGeneration) {
            changed = false;
            error = "Project changed during the edit.";
        }
        if (!changed) {
            return false;
        }
        // A rejected preparation must not interrupt a running preview. Once
        // the candidate is known to be valid, restore its original playhead
        // before capturing undo state and publishing the edit.
        stopSpecialAudition();
        carta::EditorState before{{},           timeSelection,           selectedTrack,
                                  selectedClip, selectedMarker,          masterChannels,
                                  position,     std::move(historyClips), std::move(historyTracks)};
        std::swap(session, candidate);
        before.session = std::move(candidate);
        publish();
        pushPreparedHistory(std::move(before), std::move(action), candidateRange,
                            renderGeometryStable);
        return true;
    }

    template <class Prepare, class Publish>
    bool mutateMarkers(Prepare prepare, Publish publish, juce::String action, juce::String& error) {
        const juce::ScopedLock guard(lock);
        if (dragMode != kDragNone) {
            error = "Finish or cancel the current drag first.";
            return false;
        }
        const auto generation = editGeneration;
        const bool changed = prepareRangeSessionEdit(
            std::move(prepare),
            [&] {
                publish();
                timelineClipSelection.retainAfterStableEdit(generation, generation + 1);
                if (timelineTrackRevision == generation) {
                    timelineTrackRevision = generation + 1;
                }
            },
            std::move(action), error, true);
        repaint();
        return changed;
    }

    void pushPreparedHistory(carta::EditorState old, juce::String action,
                             carta::PlaybackRange range, bool renderGeometryStable = false);

    template <class Change>
    bool mutateMixState(Change change, juce::String& error, juce::String action) {
        const juce::ScopedLock guard(lock);
        if (dragMode != kDragNone) {
            error = "Finish or cancel the current drag first.";
            return false;
        }
        const auto generation = editGeneration;
        const bool changed = prepareRangeSessionEdit(
            [&](carta::Session& candidate) { return change(candidate); },
            [&] {
                timelineClipSelection.retainAfterStableEdit(generation, generation + 1);
                if (timelineTrackRevision == generation) {
                    timelineTrackRevision = generation + 1;
                }
            },
            std::move(action), error, true);
        repaint();
        return changed;
    }

    bool editStereoTrackRouting(int track, bool fold, juce::String& error);
    bool changeMasterChannels(int channels, juce::String& error);
    bool editStereoChannelTracks(int track, bool combine, juce::String& error);
    bool editFocusedClip(int action, juce::int64 sample, juce::String& error);
    bool joinFocusedClip(juce::String& error);
    bool splitSelectionToNewTracks(juce::String& error);
    bool splitTracksAtPlayhead(bool all, juce::String& error);
    void splitFocusedAudio();

    template <class Change>
    bool mutateGeometryState(Change change, juce::String& error, juce::String action) {
        const juce::ScopedLock guard(lock);
        if (dragMode != kDragNone) {
            error = "Finish or cancel the current drag first.";
            return false;
        }
        const auto generation = editGeneration;
        const auto editPosition = editPlayhead();
        const bool changed = prepareRangeSessionEdit(
            [&](carta::Session& candidate) {
                return change(candidate, editPosition) &&
                       carta::preservesMarkerBounds(session, candidate, error);
            },
            [&] {
                timelineClipSelection.retainAfterStableEdit(generation, generation + 1);
                if (timelineTrackRevision == generation) {
                    timelineTrackRevision = generation + 1;
                }
            },
            std::move(action), error);
        repaint();
        return changed;
    }

    // ------------------------------------------------------------------------
    // Snapshot Safety, Selection Navigation, Viewport, and Clip/Track Editing
    // ------------------------------------------------------------------------
    void copyWorkerSnapshot(carta::Session& snapshot);
    bool validateRetainedMedia(const juce::File& target, juce::String& error);
    bool startMarkerExchange(const juce::File& file, bool writing, bool audacity, bool ipc,
                             juce::String& error);
    bool validSelection() const;
    void revealTrack(int track);
    void announceNavigation(const juce::String& message);
    bool ensureFocusedPersistentGroup();
    bool setPersistentClipGrouping(bool group);
    void toggleFocusedClipSelection();
    bool selectSourceClips(int scope = 0);
    bool selectClipsInScope(int command);
    void focusTrackWithoutSeeking(int track);
    void focusAdjacentTrack(bool next);
    void selectAdjacentTrack(bool next);
    void focusClipWithoutSeeking(int track, int clipIndex);
    void focusAdjacentClip(bool next);
    void selectAdjacentClip(bool next);
    int menuHeight() const;
    void restoreWorkspacePreferences(const juce::File& file);
    void saveWorkspacePreferences();
    int visibleDiscWorkspaceHeight() const;
    void refreshDiscWorkspace(bool force = false, const juce::String& preferredTrackId = {});
    void setDiscWorkspaceVisible(bool visible);
    juce::Rectangle<int> timeline() const;
    juce::int64 marqueeDocumentY(int y) const;
    juce::Rectangle<int> marqueeBounds() const;
    void updateMarqueePreview();
    const carta::ClipSelection& displayedClipSelection() const noexcept;
    bool clipAppearsSelected(int track, int clip) const;
    void syncTimelineScroll();
    void scrollBarMoved(juce::ScrollBar* bar, double start) override;
    juce::Rectangle<int> trackArea(int track) const;
    int trackAt(int y) const;
    void restoreGestureClipSelection();
    void preparePersistentGroupGesture(juce::Point<int> point);
    void restoreTrackDragSelection();
    void updateDraggedTrack();
    int trackResizeAt(juce::Point<int> point) const;
    ChannelBoundary channelBoundaryAt(juce::Point<int> point) const;
    void updateDraggedChannelHeight(int y);
    double fullLength() const;
    double viewLength() const;
    void setZoomView(double span, double start);
    void zoomAround(double factor, double anchor, double ratio);
    void zoomAt(double factor);
    void zoomToArrangement();
    bool zoomToMasterTrack(int index, juce::String& error);
    void zoomToMasterTrackAtPlayhead();
    void togglePreviousZoom();
    void zoomToSelected();
    void zoomToSelectedClips();
    void zoomToTimeSelection();
    static bool parseInteger(const juce::String& text, juce::int64& value);
    void setSpectrumFftOrder(int order);
    void setSpectrumFloor(int floor);
    void setSpectrumWindow(carta::SpectrumWindow window);
    void setSpectrumPreset(int preset);
    bool setCustomSpectrumRange(int low, int high);
    void editCustomSpectrumRange();
    bool setChannelHeights(int index, juce::uint64 revision, const juce::String& text,
                           juce::String& error);
    void editChannelHeights();
    void nudgeTrackHeight(int direction);
    void editTrackHeight();
    void nudgeClipGroup(int direction);
    bool sequenceSelectedClips(bool distribute);
    bool addClipBoundaryMarkers(bool ends);
    void addClipSourceCues();
    void nudgeClip(int direction);
    std::vector<int> effectiveMixTracks() const;
    static bool parseTrackGainDb(const juce::String& input, double& gain, bool limitGain = true);
    bool setTrackGains(juce::uint64 revision, const std::vector<int>& tracks, double value,
                       bool& changed, juce::String& error);
    bool setTrackBalances(juce::uint64 revision, const std::vector<int>& tracks, double value,
                          bool& changed, juce::String& error);
    void setSelectedTrackBalanceDialog();
    void setSelectedTrackGainDialog();
    void setSelectedTrackGainDbDialog();
    bool scaleTrackGains(juce::uint64 revision, const std::vector<int>& tracks, double factor,
                         juce::String& error);
    void editSelectedTrackGain();
    void adjustSelectedTrackGainDbDialog();
    void setSelectedTrackFlag(int command);
    void alignSelectedClip(int command);
    void nudgeSelectedClipEdge(int command);
    bool trimSelectedEdgesToPlayhead(bool endEdge);
    bool slipClipAt(int track, int index, juce::uint64 generation, juce::int64 samples,
                    bool requireFocus, juce::String& error);
    void editSlipFocusedClip();
    bool slipSelectedClips(const std::vector<carta::ClipReference>& clips, juce::uint64 generation,
                           juce::int64 samples);
    void editSlipSelectedClips();
    bool adjustSelectedClipEdges(const std::vector<carta::ClipReference>& clips,
                                 juce::uint64 generation, bool endEdge, juce::int64 samples);
    bool trimSelectedGroupRange(const std::vector<carta::ClipReference>& clips,
                                juce::uint64 generation, carta::ClipReference anchor, bool setStart,
                                juce::int64 start, bool setEnd, juce::int64 end);
    bool alignSelectedGroupSample(const std::vector<carta::ClipReference>& clips,
                                  juce::uint64 generation, carta::ClipReference anchor,
                                  bool endEdge, juce::int64 sample);
    void editSelectedClipEdges(bool endEdge);
    static bool sameClipGeometry(const carta::Clip& a, const carta::Clip& b);
    bool renameClipTo(int track, int index, juce::uint64 revision, const juce::String& text,
                      juce::String& error);
    void renameClip();
    bool editSingleClipSamples(int track, int index, juce::uint64 revision, bool trim,
                               bool setStart, juce::int64 start, bool setEnd, juce::int64 end,
                               juce::String& error);
    void editClipSamples(bool trim);
    bool moveTimeSelectionTo(juce::uint64 revision, const carta::TimeSelection& original,
                             const std::vector<int>& independent, int target, juce::int64 start,
                             juce::String& error);
    void moveTimeSelection();
    void editTimeSelection(bool byLength = false);
    static bool parseNumber(const juce::String& text, double& value);
    bool setSelectedClipEnvelopes(const std::vector<carta::ClipReference>& clips,
                                  juce::uint64 generation, double gain, double fadeIn,
                                  double fadeOut);
    void editSelectedClipEnvelopes();
    std::vector<carta::ClipReference> activeFadeShapeTargets() const;
    bool setTargetClipFadeShapes(bool setIn, bool setOut, carta::FadeShape shape,
                                 juce::String& error);
    void addFadeShapeMenus(juce::PopupMenu& target, bool enabled) const;
    bool applyClipMixOrRangeGainEdit(bool normalize, double value, bool independent,
                                     const juce::String& actionName, juce::String& error);
    void showAmplifyDialog();
    void showNormalizeDialog();
    void editClipEnvelope();
    bool applyTrackRoutingMatrix(int index, int count, juce::uint64 revision,
                                 std::vector<carta::StereoRoute> matrix, bool fieldsEdited,
                                 juce::String& error);
    void editTrackRouting();
    void editTrackMix();
    bool renameTrackTo(int index, const juce::String& name, juce::String& error);
    void renameTrack();

    // ------------------------------------------------------------------------
    // Dialog Launchers, Source Verification, and MiniDisc / NetMD Authoring
    // ------------------------------------------------------------------------
    std::vector<CommandSearchEntry> commandSearchEntries();
    void showCommandSearch();
    void showKeyboardHelp();
    void exportDiagnosticLog();
    void revealDiagnosticLogFolder();
    void recordImportIssue(const juce::File& file, const juce::String& reason);
    void clearImportIssues();
    juce::String importIssueText() const;
    void showImportIssues();
    void showTrackSources();
    void showTrackSourceList(juce::String name, std::vector<carta::TrackSource> sources);
    bool verifySelectedSources(bool allTracks, int requestedTrack = -1, bool ipc = false);
    void chooseSourceReplacement(const std::shared_ptr<carta::Audio>& source,
                                 juce::uint64 revision);
    bool startSourceReplacement(const std::shared_ptr<carta::Audio>& source, const juce::File& file,
                                juce::uint64 revision, bool ipc = false);
    bool focusDiscTrackNumber(juce::uint64 revision, juce::int64 trackNumber, juce::String& error);
    void promptDiscTrackNumber();
    void editDiscFormsAndTimestamp();
    void editSelectedTrackTitleForms();
    void editSelectedTrackMdFlags();
    void inspectCurrentUtoc();
    void exportCurrentUtocDialog();
    void importUtocDialog();
    void startDiscProgramWavExport();
    std::string resolveActiveNetMdDeviceId(const juce::String& explicitId, std::string& error);
    juce::var buildNetMdStatusJson();

    /**
     * @brief Runs an exclusive background NetMD USB/bridge job while reporting progress to the UI.
     */
    template <typename Fn>
    void launchNetMdWorker(const std::string& op, const std::string& devId,
                           const std::string& initialPhase, Fn&& body) {
        if (netMdBusy.load() || hiMdBusy.load()) {
            return;
        }
        netMdWorker.reset();
        {
            std::lock_guard<std::mutex> lk(netMdMutex);
            netMdActiveOp = op;
            netMdActiveDevId = devId;
            netMdLastOp = op;
            netMdLastDevId = devId;
            netMdLastOk = false;
            netMdLastError.clear();
            netMdLastResult = juce::var();
            netMdPendingUiStatus.clear();
            netMdPendingAlertTitle.clear();
            netMdPendingAlertBody.clear();
            netMdPendingPreflight.reset();
            netMdPendingLoadedDisc.reset();
            netMdPendingHasCommittedRaw = false;
            netMdPendingCommittedHash.clear();
            netMdPendingNeedsReload = false;
            netMdPendingShowLoadAlerts = true;
        }
        netMdDonePending.store(false);
        carta::setNetMdProgress(op, initialPhase, 0.0);
        netMdBusy.store(true, std::memory_order_release);
        refreshRecordWorkspace(false);
        netMdWorker = std::make_unique<NetMdWorkerThread>(
            [this, op, devId, fn = std::forward<Fn>(body)]() mutable {
                std::string err;
                juce::var result;
                juce::String uiStatus;
                juce::String alertTitle;
                juce::String alertBody;
                std::optional<carta::NetMdPreflightReport> afterPreflight;
                const bool ok = fn(err, result, uiStatus, alertTitle, alertBody, afterPreflight);
                carta::clearNetMdProgress();
                {
                    std::lock_guard<std::mutex> lk(netMdMutex);
                    netMdLastOk = ok;
                    netMdLastError = err;
                    netMdLastResult = result;
                    netMdPendingUiStatus = uiStatus;
                    netMdPendingAlertTitle = alertTitle;
                    netMdPendingAlertBody = alertBody;
                    netMdPendingPreflight = afterPreflight;
                }
                netMdDonePending.store(true, std::memory_order_release);
                netMdBusy.store(false, std::memory_order_release);
            });
    }

    void startNetMdProbeJob(const std::string& devId);
    static void applyCommittedDiscIdentity(carta::NetMdDeviceStatus& dev, int trackCount,
                                           const std::string& hwTitle, const std::string& fwTitle,
                                           bool hiMd, const carta::MdDisc* committedDisc = nullptr);
    void startNetMdReadUtocJob(const std::string& devId, const juce::File& outFile,
                               const juce::String& outPath, bool showPopup);
    void startNetMdLoadDiscJob(const std::string& devId, bool showAlerts = true);
    struct DownloadTargetClip {
        std::string regionId;
        std::int64_t clipStartSample = -1;
        std::int64_t clipEndSample = -1;
    };
    void startNetMdDownloadAudioJob(const std::string& devId, std::vector<std::string> regionIds,
                                    bool showAlerts = true,
                                    std::optional<int> onlyUtocTrackIndex0 = std::nullopt,
                                    std::vector<DownloadTargetClip> clipTargets = {});
    void startNetMdEraseJob(const std::string& devId, bool confirmHardware, bool showAlerts = true);
    void startNetMdWriteUtocJob(const carta::Session& snapshot, const std::string& devId,
                                bool confirmHardware, bool showAlerts = true);
    void startNetMdRecordJob(carta::Session&& snapshotIn, const std::string& devId,
                             bool confirmHardware, bool showAlerts = true);
    bool applyLoadedMiniDisc(const carta::MdDisc& loaded, juce::String& error);
    void pollNetMdJobProgress();
    void refreshRecordWorkspace(bool scanDevices, bool allowElevationPrompt = false);
    void triggerRecordProbeFromUi();
    void triggerReadDiscTocFromUi();
    void triggerLoadDiscFromUi();
    void triggerDownloadDiscAudioFromUi(std::vector<std::string> regionIds = {},
                                        std::optional<int> onlyUtocTrackIndex0 = std::nullopt,
                                        std::vector<DownloadTargetClip> clipTargets = {});
    void cancelActiveDiscDownload();
    bool canDownloadDiscAudio() const;
    bool hasUnloadedOnDiscPlaceholders() const;
    void drainPendingDownloadedRegions();
    juce::Rectangle<int> onDiscDownloadPillBounds(const juce::Rectangle<int>& clipRect,
                                                  const juce::Rectangle<int>& visibleRect,
                                                  bool downloading,
                                                  bool canDownload) const noexcept;
    bool isClipCurrentlyDownloading(const std::string& regionId, std::int64_t clipStartSample,
                                    std::int64_t clipEndSample);
    std::string currentDownloadingRegionId();
    double currentDownloadingRegionProgress();
    void triggerEraseDiscFromUi();
    void triggerRecordWriteUtocFromUi();
    void triggerRecordStartFromUi();

    // Hi-MD USB BOT hardware bridge & async job orchestration
    std::string resolveActiveHiMdDeviceId(const juce::String& explicitId, std::string& error);
    juce::var buildHiMdStatusJson();

    /**
     * @brief Runs an exclusive background Hi-MD USB BOT job while reporting progress to the UI.
     */
    template <typename Fn>
    void launchHiMdWorker(const std::string& op, const std::string& devId,
                          const std::string& initialPhase, Fn&& body) {
        if (hiMdBusy.load() || netMdBusy.load()) {
            return;
        }
        hiMdWorker.reset();
        {
            std::lock_guard<std::mutex> lk(hiMdMutex);
            hiMdActiveOp = op;
            hiMdActiveDevId = devId;
            hiMdLastOp = op;
            hiMdLastDevId = devId;
            hiMdLastOk = false;
            hiMdLastError.clear();
            hiMdLastResult = juce::var();
            hiMdPendingUiStatus.clear();
            hiMdPendingAlertTitle.clear();
            hiMdPendingAlertBody.clear();
            hiMdPendingStatus.reset();
            hiMdPendingLoadedDisc.reset();
            hiMdPendingNeedsElevation = false;
        }
        hiMdDonePending.store(false);
        carta::setHiMdProgress(op, initialPhase, 0.0);
        hiMdBusy.store(true, std::memory_order_release);
        refreshRecordWorkspace(false);
        hiMdWorker = std::make_unique<NetMdWorkerThread>(
            [this, fn = std::forward<Fn>(body)]() mutable {
                std::string err;
                juce::var result;
                juce::String uiStatus;
                juce::String alertTitle;
                juce::String alertBody;
                std::optional<carta::HiMdDeviceStatus> afterStatus;
                const bool ok = fn(err, result, uiStatus, alertTitle, alertBody, afterStatus);
                carta::clearHiMdProgress();
                {
                    std::lock_guard<std::mutex> lk(hiMdMutex);
                    hiMdLastOk = ok;
                    hiMdLastError = err;
                    hiMdLastResult = result;
                    hiMdPendingUiStatus = uiStatus;
                    hiMdPendingAlertTitle = alertTitle;
                    hiMdPendingAlertBody = alertBody;
                    hiMdPendingStatus = afterStatus;
                }
                hiMdDonePending.store(true, std::memory_order_release);
                hiMdBusy.store(false, std::memory_order_release);
            });
    }

    void startHiMdProbeJob(const std::string& devId, bool allowElevationPrompt = true);
    void startHiMdLoadDiscJob(const std::string& devId, bool showAlerts = true);
    void startHiMdDownloadAudioJob(const std::string& devId, std::vector<std::string> regionIds,
                                   bool showAlerts = true);
    void startHiMdRecordJob(carta::Session&& snapshotIn, const std::string& devId,
                            bool confirmHardware, bool showAlerts = true);
    void startHiMdWriteIndexJob(const carta::Session& snapshot, const std::string& devId,
                                bool confirmHardware, bool showAlerts = true);
    void startHiMdDumpJob(const std::string& devId, int trackIndex, const juce::File& outFile,
                          bool rawAudio, bool showAlerts = true);
    void startHiMdRenameDiscJob(const std::string& devId, const std::string& title,
                                bool confirmHardware, bool showAlerts = true);
    void startHiMdRenameTrackJob(const std::string& devId, int trackIndex, bool hasTitle,
                                 const std::string& title, bool hasArtist,
                                 const std::string& artist, bool hasAlbum,
                                 const std::string& album, bool confirmHardware,
                                 bool showAlerts = true);
    void startHiMdMoveTrackJob(const std::string& devId, int fromIndex, int toIndex,
                               bool confirmHardware, bool showAlerts = true);
    void startHiMdSetGroupsJob(const std::string& devId, std::vector<carta::MdGroup> groups,
                               int trackCount, bool confirmHardware, bool showAlerts = true);
    void startHiMdDeleteTracksJob(const std::string& devId, std::vector<int> trackIndices,
                                  bool confirmHardware, bool showAlerts = true);
    void startHiMdWipeJob(const std::string& devId, bool reformat, bool confirmHardware,
                          bool showAlerts = true);
    void pollHiMdJobProgress();
    void triggerHiMdDumpTrackFromUi(int trackIndex = -1);
    void triggerHiMdDiscToolsMenuFromUi();
    void triggerHiMdRenameDiscFromUi();
    void triggerHiMdRenameTrackFromUi(int trackIndex);
    void triggerHiMdMoveTrackFromUi(int trackIndex, int delta);
    void triggerHiMdDeleteTrackFromUi(int trackIndex);
    void triggerHiMdSyncGroupsFromUi();
    void triggerHiMdWriteIndexFromUi();

    // ------------------------------------------------------------------------
    // Analysis, Track Rendering, List Browsers, Audition, and Modal Prompts
    // ------------------------------------------------------------------------
    void configureQuietGapSearch();
    static bool sameClipReferences(const std::vector<carta::ClipReference>& left,
                                   const std::vector<carta::ClipReference>& right);
    bool startSelectedTrackRender(const juce::File& target, bool ipc, juce::String& error,
                                  RenderScope scope = RenderScope::selectedTime,
                                  std::vector<int> explicitTracks = {},
                                  bool allowRoutingChange = false);
    void confirmConsolidationRoutingChange();
    void chooseSelectedTrackRender(RenderScope scope, bool allowRoutingChange = false);
    bool ipcAnalysisStale() const;
    bool startIpcLevelAnalysis(const juce::String& scope, juce::int64 index, juce::String& error);
    bool selectQuietRegion(juce::int64 start, juce::int64 end, juce::uint64 revision, int channels,
                           juce::String& error);
    bool selectClipSilenceRegion(juce::int64 start, juce::int64 end, int track,
                                 juce::uint64 revision, int channels, juce::String& error);
    bool detachScannedSilences(int track, int clip, const std::vector<carta::QuietRegion>& ranges,
                               juce::uint64 revision, int channels, juce::String& error);
    bool detachScannedSilence(int track, int clip, juce::int64 start, juce::int64 end,
                              juce::uint64 revision, int channels, juce::String& error);
    void configureClipSilenceSearch();
    bool startClipSilenceAnalysis(int track, int clip, juce::int64 minimum, bool ipc,
                                  juce::String& error);
    void showClipSilenceList(const std::vector<carta::QuietRegion>& regions,
                             const std::vector<carta::QuietRegion>& safeRegions, int track,
                             int clip, juce::uint64 revision, int channels);
    juce::String selectQuietSuggestion(const juce::String& line,
                                       const std::vector<carta::QuietRegion>& gaps,
                                       juce::uint64 revision, int channels);
    void showQuietGapList(const std::vector<QuietGapList::Row>& rows, juce::uint64 revision,
                          int channels, std::function<void()> selected = {});
    bool startIpcQuietAnalysis(double threshold, juce::int64 minimum, juce::String& error);
    void startQuietGapSearch(double threshold, juce::int64 duration, juce::uint64 revision,
                             int channels);
    bool findSelectionZeroCrossings(bool requireAllChannels = false, bool ipc = false);
    bool locateLevelSample(juce::int64 sample, juce::uint64 revision, int channels,
                           const juce::String& label, juce::String& error);
    bool levelScopeMatches(int scope, const carta::TimeSelection& captured,
                           const std::vector<int>& tracks) const;
    void analyzeLevels(int scope);
    bool performTimelineClipAction(int action, juce::int64 explicitSplitSample = -1);
    bool transformBrowserClipGroup(const std::vector<carta::ClipReference>& clips,
                                   juce::uint64 generation, bool isSplit);
    bool moveSelectedClipGroup(const std::vector<carta::ClipReference>& clips,
                               juce::uint64 generation, juce::int64 samples, juce::int64 tracks,
                               bool requireTimelineSelection);
    void moveClipGroupDialog(std::vector<carta::ClipReference> clips, juce::uint64 generation,
                             bool requireTimelineSelection = false);
    bool expandBrowserClipTargets(std::vector<carta::ClipReference>& clips,
                                  juce::uint64 generation);
    bool removeBrowserClipGroup(const std::vector<carta::ClipReference>& clips,
                                juce::uint64 generation, bool cut);
    void showTrackClipNavigator();
    bool applyTrackMixSelection(juce::uint64 revision, const std::vector<int>& indices, int action);
    bool applyIndependentTrackRange(juce::uint64 revision, carta::TimeSelection captured,
                                    const std::vector<int>& indices, int action,
                                    bool finishStandard = false);
    bool independentRangeCurrent(juce::uint64 revision, const carta::TimeSelection& captured) const;
    bool copyIndependentTrackRange(juce::uint64 revision, const carta::TimeSelection& captured,
                                   const std::vector<int>& indices,
                                   carta::Clipboard* prepared = nullptr);
    bool previewIndependentTrackRange(juce::uint64 revision, const carta::TimeSelection& captured,
                                      const std::vector<int>& indices);
    void performTimelineTrackAction(int id);
    void showTrackMixList(std::vector<int> selected = {});
    bool renameMasterTrack(int index, const juce::String& name, juce::uint64 generation,
                           bool& changed, juce::String& error);
    void renameMasterTrackDialog(int index, juce::uint64 generation,
                                 const juce::String& currentName);
    void showMasterTracks();
    void showOverlapList();
    void performMarkerListAction(juce::uint64 generation, int markerIndex, int action, int row,
                                 bool reopen = true);
    void deleteMarkerListRows(juce::uint64 generation, const std::vector<int>& indices,
                              int anchorRow, bool reopen = true);
    bool shiftMarkerListRows(juce::uint64 generation, const std::vector<int>& indices,
                             juce::int64 delta, juce::String& error);
    bool renameMarkerListRows(juce::uint64 generation, const std::vector<int>& indices,
                              const std::vector<juce::String>& names, juce::String& error);
    bool positionMarkerListRows(juce::uint64 generation, const std::vector<int>& indices,
                                const std::vector<juce::int64>& samples, juce::String& error);
    void positionMarkerListDialog(juce::uint64 generation, std::vector<int> indices);
    void renameMarkerListDialog(juce::uint64 generation, std::vector<int> indices);
    void shiftMarkerListDialog(juce::uint64 generation, std::vector<int> indices);
    bool resolveMarkerListSelection(const std::vector<MarkerList::Row>& rows,
                                    const std::vector<int>& selectedRows, int anchorRow,
                                    std::vector<int>& indices, int& currentAnchor,
                                    juce::String& error) const;
    void showMarkerList(std::vector<int> selectedIndices = {});
    bool startSelectionAudition(bool scoped, bool loop, bool context);
    bool startSelectionContextAudition(bool scoped);
    bool startBoundaryAudition(juce::int64 point, int requiredMarker = -1,
                               int requiredPointTrack = -1);
    bool auditionFocusedMarker();
    bool auditionEditPoint();
    void auditionBoundary(int which);
    void auditionClip(bool loop);
    bool auditionClipGroup(bool loop);
    bool startDiscPreview(juce::uint64 revision, const juce::String& trackId = {},
                          bool throughEnd = false);
    bool prepareRippleAudition(bool allTracks, bool focusedTrack, carta::Session& candidate,
                               carta::PlaybackRange& range, juce::String& error);
    bool startRippleAudition(int scope, juce::uint64 revision, const carta::TimeSelection& captured,
                             const std::vector<int>& indices);
    void auditionRipple(bool allTracks, bool focusedTrack = false);
    bool crossfadeSelectedPair();
    bool startCrossfadeAudition(int scope);
    void auditionSelectedCrossfade();
    void auditionCrossfade(bool acrossTracks);
    bool setAuditionContext(juce::int64 before, juce::int64 after, juce::String& error);
    void editAuditionContext();
    bool deleteMarkersInSelection(int& removed, juce::String& error);
    void shiftMarkersDialog(int command);
    void reopenMarkerList(std::vector<int> selectedIndices = {}, juce::uint64 revision = 0);
    void editMarkerPosition(bool reopenList = false);
    bool nudgeMarker(int index, int direction, juce::String& error);
    void navigateMarker(bool next);
    void seekTimeline(int command);
    void navigateClipBoundary(bool next, bool allTracks);
    bool selectSelectedClipSpan();
    bool selectClipTime(int portion);
    bool selectMasterTrackIndex(int index, bool scoped, juce::uint64 generation,
                                juce::String& error);
    void selectMasterTrack(bool scoped);
    void selectToBoundary(int id);
    void adjustTimeSelection(carta::SelectionPart part, int direction);
    void markLiveSelection(bool markIn);
    void extendSelection(int direction);
    void extendSelectionTracks(int direction);
    bool clearRememberedSelection();
    bool rememberSelection(bool restore);
    void showGuardedStudioAlert(const juce::MessageBoxOptions& options,
                                std::function<void(int)> callback = {});
    void launchStudioChooser(const juce::String& title, const juce::File& initial,
                             const juce::String& filters, int flags,
                             std::function<void(const StudioFileSelection&)> callback);
    void showTextFieldsChoicePrompt(const juce::String& title, const juce::String& message,
                                    std::vector<carta::TextPromptField> fields,
                                    const juce::String& accept, const juce::String& alternative,
                                    std::function<void(int, const juce::StringArray&)> callback);
    void showTextFieldsPrompt(const juce::String& title, const juce::String& message,
                              std::vector<carta::TextPromptField> fields,
                              const juce::String& accept,
                              std::function<void(bool, const juce::StringArray&)> callback);
    void showTextPrompt(const juce::String& title, const juce::String& message,
                        const juce::String& key, const juce::String& label,
                        const juce::String& initial, const juce::String& accept,
                        std::function<void(bool, const juce::String&)> callback);
    void renameMarker(bool reopenList = false);

    // ------------------------------------------------------------------------
    // Coordinate Mapping, Hit-Testing, Waveform/Spectrum Caches, and Painting
    // ------------------------------------------------------------------------
    int timeToX(double t, juce::Rectangle<int> area, double span) const;
    int timeToX(double t) const;
    int markerHitAt(int x, bool cycle, int preferred, int* nearby = nullptr) const;
    bool focusAdjacentMarker(bool next);
    void focusMarker(int index);
    juce::int64 sampleAtX(int x) const;
    juce::int64 snappedPointerSample(int x, bool allowSnap);
    int selectionEdgeAt(juce::Point<int> point) const;
    double xToTime(int x, juce::Rectangle<int> area, double span) const;
    double xToTime(int x) const;
    void updateFileDropGuide(int x, int y);
    juce::Rectangle<int> clipRect(const carta::Clip& c, juce::Rectangle<int> row,
                                  juce::Rectangle<int> area, double span) const;
    juce::Rectangle<int> clipRect(const carta::Clip& c, int track) const;
    std::vector<int> clipHitsAt(const std::vector<carta::Clip>& clips, juce::Point<int> point,
                                juce::Rectangle<int> row, juce::Rectangle<int> area,
                                double span) const;
    std::vector<carta::ClipTitleRegion> titleRegions(int track, juce::Rectangle<int> row,
                                                     juce::Rectangle<int> area, double span) const;
    std::vector<carta::ClipTitleRegion> titleRegions(int track) const;
    bool masterEndHandleAt(juce::Point<int> point) const;
    int savedLoopHandleAt(juce::Point<int> point) const;
    std::pair<juce::Point<int>, juce::Point<int>> fadeHandlePoints(const carta::Clip& clip,
                                                                   int track) const;
    int fadeHandleAt(juce::Point<int> point) const;
    void drawFadeHandles(juce::Graphics& g, const carta::Clip& clip, int track, bool dark) const;
    TrackHeaderControls trackHeaderControls(int track) const;
    int trackHeaderHit(juce::Point<int> point, int* outTrack = nullptr) const;
    static juce::String formatHeaderGainDb(float gain);
    static juce::String formatHeaderBalance(float pan);
    static float headerDraggedGain(float startGain, int dx, bool fine);
    static float headerDraggedBalance(float startPan, int dx, bool fine);
    void toggleHeaderTrackControl(int track, int hit);
    bool selectedGroupEdgeAt(juce::Point<int> point) const;
    void updateCursor(juce::Point<int> point);
    juce::String hoverHelpAt(juce::Point<int> point) const;
    juce::String scaleHoverHelp(int track, int y) const;
    void drawScale(juce::Graphics& g, int track, bool dark, int visibleChannel, int trackChannels,
                   double spectrumRate);
    void cancelPendingSpectrum();
    void cancelPendingWaveforms();
    void setWaveformZoom(int value);
    void setWaveformScale(carta::WaveformScale value);
    void setWaveformHalf(bool enabled);
    void setWaveformRms(bool enabled);
    void cancelUnseenWaveforms();
    void pruneWaveformCache();
    std::shared_ptr<const std::vector<carta::WaveformColumn>>
    waveformColumnsFor(const carta::Clip& clip, int channel, int firstX, int columns,
                       bool includeRms, juce::Rectangle<int> area, double span);
    void updateSpectrumViewport(double span);
    void pruneSpectrumCache();
    juce::Image spectrumFor(const carta::Clip& clip, juce::int64 start, juce::int64 step,
                            int columns, double displayRate, bool& failed);
    void drawSpectrumTiles(juce::Graphics& g, const carta::Clip& clip, juce::Rectangle<int> content,
                           juce::Rectangle<int> visible, int visibleChannel, int trackChannels,
                           double displayRate, const std::vector<int>& weights,
                           juce::Rectangle<int> area, double span);
    void drawSpectrumTiles(juce::Graphics& g, const carta::Clip& clip, juce::Rectangle<int> content,
                           juce::Rectangle<int> visible, int visibleChannel, int trackChannels,
                           double displayRate, const std::vector<int>& weights = {});
    static std::pair<juce::Rectangle<int>, juce::Rectangle<int>>
    clipTrimGripBounds(juce::Rectangle<int> rect);
    void drawClip(juce::Graphics& g, const carta::Clip& c, int t, int index, int visibleChannel,
                  int trackChannels, double spectrumRate, juce::Rectangle<int> row,
                  juce::Rectangle<int> area, double span, bool dark,
                  bool* incompleteWaveformPaint = nullptr);
    void drawClip(juce::Graphics& g, const carta::Clip& c, int t, int index, int visibleChannel,
                  int trackChannels, double spectrumRate);
    void drawTitle(juce::Graphics& g, const carta::Clip& c, int track,
                   const carta::ClipTitleRegion& region, juce::Rectangle<int> row, bool dark);

    // ------------------------------------------------------------------------
    // Import Commit, Timer Polling, Audio Device Settings, Export & Project IO
    // ------------------------------------------------------------------------
    static juce::int64 layerEndSample(const carta::Track& track);
    static bool hasMarkerAtSample(const carta::Session& s, juce::int64 sample);
    bool commitReadyImport(const std::shared_ptr<ImportTask>& task, juce::String& error);
    void finishReadyImports();
    void scrollClipDrag();
    juce::Rectangle<int> planPlayheadRepaint(double displayPosition);
    void timerCallback() override;
    void importAudio();
    juce::String audioOutputError();
    bool requireAudioOutput();
    void showAudioSettings();
    void saveAudioDevicePreferences(bool report);
    void rescanAudioDevices();
    void exportWav(bool selectionOnly = false, bool selectedTracksOnly = false,
                   bool markerTrack = false, bool clipOnly = false, bool groupOnly = false,
                   bool discReview = false);
    void exportAllTracks();
    void beginExport(std::shared_ptr<ExportTask>, int, carta::WavRange, juce::File);
    void writeExport(std::shared_ptr<ExportTask>, int, carta::WavRange, juce::File);
    void exportManifest();
    void exportPortableProject();
    void importMarkers(bool audacity = false);
    void exportMarkers(bool audacity = false);
    void newProject();
    void saveProject(std::function<void(bool)> completion = {}, bool saveAs = false,
                     bool ipc = false, juce::File explicitFile = {}, bool noReplace = false,
                     bool forcePackage = false);
    void confirmSaveBefore(std::function<void()> action);
    void loadProject();
    void recoverProject();
    void beginProjectOpen(const juce::File&, bool requireRecovery = false, bool ipc = false,
                          carta::SourceRelinks relinks = {});
    void continueProjectOpen(const std::shared_ptr<OpenTask>&);
    void maybeAutosave();
    void startRecoverySnapshot(bool announce);
    void retireRecovery(bool preserveCurrent = false);
    juce::File newRecoveryPath() const;

    // ========================================================================
    // Member State (Declaration order preserved for initialization invariants)
    // ========================================================================

    // --- Look-and-feel, top-level menus, audio settings, and atomic playback flags ---
    NeutralLook neutralLook;
    std::unique_ptr<juce::MenuBarComponent> menuBar;
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    bool darkAppearance = false;
    std::unique_ptr<AudioSettingsWindow> audioSettingsWindow;
    juce::File audioSettingsFile;
    // Lock-free flags written by the audio callback and consumed on the message thread.
    std::atomic<bool> audioInterrupted{false};
    std::atomic<bool> audioInvalidMix{false};
    std::atomic<bool> audioInvalidRate{false};
    std::atomic<bool> audioLoopInvalid{false};
    std::atomic<bool> audioLockContention{false};
    std::atomic<std::uint64_t> editLockSilentBlocks{0};

    // --- Playback lock & core session/selection state (guarded by `lock` for audio reads) ---
    juce::CriticalSection lock;
    carta::Session session;
    carta::Session dragBefore;
    carta::TimeSelection timeSelection;
    carta::SelectionMemory selectionMemory;
    carta::ClipSelection timelineClipSelection;
    carta::ClipSelection selectionBeforeClips;
    carta::KeyboardRangeExtension keyboardRange;
    carta::KeyboardTrackExtension keyboardTracks;
    carta::Clipboard clipboard;
    std::set<int> timelineTracks;
    std::set<int> selectionBeforeTracks;
    std::set<int> rememberedTimelineTracks;
    std::set<int> liveInTracks;
    juce::uint64 timelineTrackRevision = 0;
    juce::int64 selectionAnchorSample = 0;
    int selectionAnchorTrack = 0;
    carta::TimeSelection dragSnapSelection;
    carta::TimeSelection selectionBefore;
    carta::TimeSelection selectionResizeBase;
    juce::int64 liveInSample = 0;
    juce::uint64 liveInGeneration = 0;
    int liveInFirstTrack = -1;
    int liveInLastTrack = -1;
    bool liveInPresent = false;

    // --- Message-thread pointer drag, marquee, reorder, and group preview state ---
    juce::Point<int> selectionPointer;
    juce::Point<int> marqueeStart;
    juce::Point<int> marqueeEnd;
    double selectionScrollClock = 0;
    double selectionVerticalElapsed = 0;
    int selectionVerticalDirection = 0;
    bool selectionPointerDragging = false;
    bool selectionSnapAllowed = true;
    double marqueeAnchorTime = 0;
    double marqueeScrollClock = 0;
    double marqueeVerticalElapsed = 0;
    juce::int64 marqueeAnchorY = 0;
    int marqueeVerticalDirection = 0;
    juce::uint64 marqueeRevision = 0;
    carta::ClipSelection marqueePreview;
    bool marqueePreviewValid = false;
    juce::Point<int> trackDragPointer;
    int trackDragSource = -1;
    int trackDragSlot = -1;
    int trackDragScrollDirection = 0;
    double trackDragClock = 0;
    double trackDragScrollElapsed = 0;
    bool trackDragMoved = false;
    juce::Point<int> clipPointer;
    double clipScrollClock = 0;
    double dragViewStart = 0;
    double dragFullLength = 0;
    double clipVerticalElapsed = 0;
    int clipVerticalDirection = 0;
    int clipDropTrack = -1;
    bool clipPointerDragging = false;
    bool clipSnapAllowed = true;
    std::vector<carta::ClipReference> groupDragClips;
    std::vector<carta::Clip> groupEdgePreview;
    bool groupEdgeEnd = false;
    carta::Clip groupDragOriginal;
    carta::ClipGroupMovePreview groupDragPreview;
    carta::ClipGroupEdgePreview groupEdgePlan;
    carta::ClipGroupEdgeMarkerPreview groupEdgeMarkerPreview;
    juce::uint64 groupDragGeneration = 0;
    juce::int64 groupDragSamples = 0;
    int groupDragTracks = 0;
    int groupDragAnchorTrack = -1;
    int groupDragAnchorClip = -1;
    int copyGestureAnchor = -1;
    bool groupDragValid = false;
    bool groupDragCopy = false;
    bool copyGestureCandidate = false;
    juce::String groupDragError;
    carta::ClipOverlapPreview clipOverlapPreview;
    bool clipOverlapReady = false;
    bool groupDragOverlaps = false;
    juce::Point<int> playheadPointer;
    double playheadScrollClock = 0;
    double playheadBefore = 0;
    bool playheadPointerDragging = false;
    bool playheadSnapAllowed = true;
    carta::WavRange loopDragOriginal{};
    carta::WavRange loopDragPreview{};
    juce::Point<int> loopPointer;
    double loopScrollClock = 0;
    bool loopDragEnd = false;
    bool loopPointerDragging = false;
    bool loopSnapAllowed = true;
    double selectionBeforePosition = 0;
    int selectionBeforeTrack = -1;
    int selectionBeforeClip = -1;
    int selectionBeforeMarker = -1;
    bool selectionResizeLeft = false;
    bool selectionClipSnapshotValid = false;
    bool selectionGesturePlayback = false;
    std::vector<double> dragTargets;
    std::vector<double> emptySnapTargets;
    int snapMode = 2;
    juce::int64 snapGridSamples = 1;
    carta::SnapTargetOptions snapOptions;
    bool snapGuideVisible = false;
    double snapGuideTime = 0;
    carta::EditHistory editHistory;
    carta::EditorState dragBeforeState;
    juce::uint64 announcementSerial = 0;
    juce::int64 markerDragMasterEnd = 0;
    juce::ApplicationCommandManager commandManager;

    // --- MiniDisc workspace, ribbon drag state, and NetMD worker bridge ---
    MdWorkspace discWorkspace;
    bool discWorkspaceVisible = true;
    bool discResizeActive = false;
    bool discWorkspaceGesture = false;
    bool discWorkspaceSelHasRange = false;
    int discWorkspaceHeight = carta::WorkspacePreferences{}.height;
    int discResizeStart = carta::WorkspacePreferences{}.height;
    int lastMdRibbonHeight = 0;
    juce::int64 discWorkspaceSelStart = 0;
    juce::int64 discWorkspaceSelEnd = 0;
    juce::uint64 discWorkspaceRevision = std::numeric_limits<juce::uint64>::max();
    juce::String discWorkspaceLastTab = "tracks";
    MdRibbonGesture mdDragGesture = MdRibbonGesture::none;
    int mdDragTrack0 = -1;
    int mdDragPart0 = -1;
    int mdDragRightTrack0 = -1;
    int mdDragRightPart0 = -1;
    juce::int64 mdDragOrigStart = 0;
    juce::int64 mdDragOrigEnd = 0;
    juce::int64 mdDragAnchorSample = 0;
    juce::int64 mdDragMaxSample = 0;
    bool mdDragChanged = false;
    juce::Point<int> mdDragPointer;
    carta::MdDisc mdDragBeforeDisc;
    juce::String mdDragActionLabel;
    juce::String mdDragFocusTrackId;
    std::vector<carta::NetMdDeviceEntry> cachedNetMdDevices;
    std::optional<carta::NetMdPreflightReport> cachedNetMdPreflight;
    bool netMdScannedOnce = false;
    std::unique_ptr<NetMdWorkerThread> netMdWorker;
    std::atomic<bool> netMdBusy{false};
    std::atomic<bool> netMdDonePending{false};
    std::atomic<bool> netMdStopRequested{false};
    std::mutex netMdMutex; // Guards the NetMD status/result fields below across worker/UI threads.
    std::string netMdActiveOp;
    std::string netMdActiveDevId;
    std::string netMdLastOp;
    std::string netMdLastDevId;
    std::string netMdLastError;
    bool netMdLastOk = false;
    juce::var netMdLastResult;
    juce::String netMdPendingUiStatus;
    juce::String netMdPendingAlertTitle;
    juce::String netMdPendingAlertBody;
    std::optional<carta::NetMdPreflightReport> netMdPendingPreflight;
    std::optional<carta::MdDisc> netMdPendingLoadedDisc;
    std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5>
        netMdPendingCommittedRawSectors{};
    bool netMdPendingHasCommittedRaw = false;
    std::string netMdPendingCommittedHash;
    bool netMdPendingNeedsReload = false;
    bool netMdPendingShowLoadAlerts = true;
    struct PendingDownloadedRegion {
        std::string regionId;
        std::shared_ptr<carta::Audio> audio;
        std::int64_t clipStartSample = -1;
        std::int64_t clipEndSample = -1;
    };
    std::vector<PendingDownloadedRegion> pendingDownloadedRegions;
    std::string activeDownloadRegionId;
    std::int64_t activeDownloadClipStartSample = -1;
    std::int64_t activeDownloadClipEndSample = -1;
    double activeDownloadRegionProgress = 0.0;
    std::string hoveredDownloadRegionId;
    std::int64_t hoveredDownloadClipStartSample = -1;
    std::vector<carta::HiMdDeviceEntry> cachedHiMdDevices;
    std::optional<carta::HiMdDeviceStatus> cachedHiMdStatus;
    bool hiMdScannedOnce = false;
    std::unique_ptr<NetMdWorkerThread> hiMdWorker;
    std::atomic<bool> hiMdBusy{false};
    std::atomic<bool> hiMdDonePending{false};
    std::mutex hiMdMutex;
    std::string hiMdActiveOp;
    std::string hiMdActiveDevId;
    std::string hiMdLastOp;
    std::string hiMdLastDevId;
    std::string hiMdLastError;
    bool hiMdLastOk = false;
    juce::var hiMdLastResult;
    juce::String hiMdPendingUiStatus;
    juce::String hiMdPendingAlertTitle;
    juce::String hiMdPendingAlertBody;
    std::optional<carta::HiMdDeviceStatus> hiMdPendingStatus;
    std::optional<carta::MdDisc> hiMdPendingLoadedDisc;
    bool hiMdNeedsElevation = false;
    bool hiMdPendingNeedsElevation = false;
    juce::File workspaceSettingsFile;
    carta::WorkspacePreferences lastWorkspacePreferences;
    juce::String discWorkspaceAudioError;
    bool workspaceSettingsReady = false;
    bool workspaceSettingsWritable = false;

    // --- File chooser, project file state, toolbar widgets, and meters ---
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File projectFile;
    bool projectIsPackage = false;
    juce::TextButton zoomOutButton{"-"};
    juce::TextButton zoomInButton{"+"};
    juce::TextButton zoomFitButton{"Fit"};
    juce::TextButton add{"+ Audio"};
    juce::TextButton addLayerButton{"+ Layer"};
    juce::TextButton play{"Play"};
    juce::TextButton stop{"Stop"};
    juce::TextButton split{"Split"};
    juce::TextButton remove{"Delete"};
    juce::TextButton marker{"+ Marker"};
    juce::TextButton splitMdButton{"+ Track Split"};
    juce::TextButton snapButton{"Snap"};
    juce::TextButton undo{"Undo"};
    juce::TextButton save{"Save"};
    juce::TextButton load{"Open"};
    juce::TextButton exportButton{"Export WAV"};
    juce::ComboBox mode;
    juce::Label status;
    juce::Label clock;
    juce::Label timelineReadout;
    juce::Label exportStatus;
    juce::Label outputPeakLabel;
    TimelineOverview timelineScroll;
    carta::OutputPeakHold outputPeakHold;
    bool showOutputPeaks = false;
    float displayPeakLeft = 0;
    float displayPeakRight = 0;
    double lastPeakUiTime = 0;

    // --- Transport, WAV export preferences, audition/loop state, and playback index ---
    double position = 0;
    double deviceRate = 0;
    double dragSpan = 10;
    std::atomic<bool> playing{false};
    bool dragged = false;
    bool dirty = false;
    bool pendingDurability = false;
    bool savePromptActive = false;
    int masterChannels = 2;
    carta::WavEncoding wavEncoding = carta::WavEncoding::pcm24;
    bool wavDither = false;
    bool wavEmbedMarkers = false;
    carta::PlaybackRange auditionRange;
    carta::WavRange savedLoop{};
    carta::WavRange persistedLoop{};
    carta::WavRange recoveryLoop{};
    bool auditionActive = false;
    bool savedLoopAuditionKind = false;
    carta::ResamplingContext resampling;
    carta::PlaybackClock playbackClock;
    carta::PlaybackRange masterRange;
    carta::PlaybackRange dragMasterRange;
    double dragOtherAudioEnd = 0;
    juce::int64 dragMinimumEnd = 0;
    juce::uint64 masterRangeRevision = 0;
    bool masterRangeValid = true;
    std::shared_ptr<carta::RenderClipIndex> playbackIndex;
    juce::uint64 playbackIndexRevision = 0;
    bool playbackIndexBusy = false;

#if CARTA_STUDIO_COMPONENT_TEST
    bool playbackIndexEnabled = false;
    std::function<bool(double)> exportWriteProgressHook;
    std::function<void(const juce::String&, carta::WavRange, int, carta::WavOptions,
                       const carta::Session&)>
        exportChooserHook;
    std::function<void(std::function<void(int)>)> groupMenuHook;
    std::function<void()> groupPreparationHook;
    std::function<void()> groupSelectionHook;
    std::function<void()> scopeSelectionHook;
    std::function<void()> marqueePreviewHook;
    std::function<void()> focusedGroupExpansionHook;
    std::function<void()> clipContextPreparationHook;
    std::function<void()> clipHitTestHook;
    std::function<void()> groupHitTestHook;
    std::function<void()> timelineAnnouncementHook;
    std::function<void()> groupCommitPreparationHook;
    std::function<void()> groupTrimCommitPreparationHook;
    std::function<void()> groupKeyboardPreparationHook;
    std::function<void()> preparedHistoryHook;
    std::function<void()> historySelectionSnapshotHook;
    std::function<void()> historyTraversalSnapshotHook;
    std::function<void()> historyTraversalPreparedHook;
    std::function<void()> historyValidationHook;
    std::function<void()> historyResultHook;
    std::function<void()> boundaryPreparationHook;
    std::function<void()> boundaryPublicationHook;
    std::function<void()> rangePreparationHook;
    std::function<void()> navigationPreviewHook;
    std::function<void()> focusNavigationHook;
    std::function<void()> focusDescriptionHook;
    std::function<void()> loopPreparationHook;
    std::function<void()> loopPublicationHook;
    std::function<void()> scrollGeometryHook;
    std::function<void()> editPointNavigationHook;
    std::function<void()> singleClipPreparationHook;
    std::function<void()> singleClipCommitHook;
    std::function<void()> singleClipCandidateHook;
    std::function<void()> singleClipPreviewHook;
    std::function<void()> masterEndPreviewHook;
    std::function<void()> crossfadePreviewPreparationHook;
    std::function<void()> ripplePreviewPreparationHook;
    std::function<void()> clipAuditionPreparationHook;
    std::function<void()> clipAuditionDisposalHook;
    std::function<void()> selectionAuditionPreparationHook;
    std::function<void()> selectionAuditionDisposalHook;
    bool testAudioOutputAvailable = false;
    std::function<void()> preparedSelectionHook;
    std::function<void()> analysisSnapshotHook;
    std::function<void(carta::WavRange)> analysisPreparedRangeHook;
    std::function<void()> ioSnapshotHook;
    std::function<void()> sourceEnumerationHook;
    std::function<void()> consolidationVerificationHook;
#else
    bool playbackIndexEnabled = true;
#endif

    double playbackIndexRetryAfter = 0;
    juce::ThreadPool playbackIndexPool{1};
    carta::Session cutPreviewSession;
    bool cutPreviewActive = false;
    bool groupAuditionActive = false;
    bool independentTrackAudition = false;
    bool discPreviewActive = false;
    double cutPreviewOriginalPosition = 0;
    carta::DiscPreviewPlan discPreviewPlan;
    carta::DiscPreviewRenderer discPreviewRenderer;
    juce::int64 discPreviewFrames = 0;

    // --- History menu commands, viewport zoom/scroll, and timeline UI selection ---
    std::map<int, HistoryMenuCommand> historyCommands;
    int nextHistoryCommand = 1000000;
    juce::int64 auditionBefore = 2 * 44100;
    juce::int64 auditionAfter = 2 * 44100;
    double zoom = 1;
    double viewStart = 0;
    int followMode = 0;
    bool followSuspended = false;
    carta::ZoomMemory zoomMemory;
    carta::TimelineTimeFormat timelineTimeFormat = carta::TimelineTimeFormat::automatic;
    bool playheadRepaintValid = false;
    juce::Rectangle<int> playheadRepaintTimeline;
    double playheadRepaintViewStart = 0;
    double playheadRepaintSpan = 0;
    juce::uint64 playheadRepaintGeneration = 0;
    int playheadRepaintFirstTrack = 0;
    int playheadRepaintX = 0;
    int selectedTrack = -1;
    int selectedClip = -1;
    int selectedMarker = -1;
    int firstTrack = 0;
    int dragMode = kDragNone;
    int dragX = 0;
    int resizedTrack = -1;
    int resizeStartY = 0;
    int resizeStartHeight = 0;
    int menuState = -1;
    int menuTrack = -2;
    int doubleClickTrack = -1;
    int doubleClickClip = -1;
    juce::uint64 doubleClickRevision = 0;
    bool popupPending = false;
    bool menuTrackCollapsed = false;
    bool menuCanRestoreSelection = false;
    bool draggingFade = false;
    bool draggingMasterEnd = false;
    carta::TrackView menuTrackView = carta::TrackView::useDefault;
    carta::Clip dragOriginal;
    double smoothVerticalRemainder = 0;
    double lastSmoothVerticalWheelTime = 0;
    int channelDragTrack = -1;
    int channelDragLane = -1;
    int channelDragStartY = 0;
    int channelDragPairPixels = 0;
    int headerDragTrack = -1;
    bool headerDragGain = true;
    bool headerDragMoved = false;
    float headerDragStartValue = 1.0f;
    float headerDragValue = 1.0f;
    juce::Point<int> headerDragAnchor;
    std::vector<int> channelDragOriginal;
    std::vector<int> channelDragScaled;
    juce::int64 dragMasterFloor = 0;
    juce::int64 dragMasterOriginalEnd = 0;

    // --- Waveform and spectrogram tile caches (message thread + worker pools) ---
    struct SpectrumKey {
        const carta::Audio* source = nullptr;
        juce::int64 start = 0;
        juce::int64 step = 0;
        int columns = 0;
        int fftOrder = 11;
        carta::SpectrumBand band = carta::SpectrumBand::full;
        double displayRate = 0;
        double offset = 0;
        double duration = 0;
        float gain = 1;
        double fadeIn = 0;
        double fadeOut = 0;
        double fadeInStart = 0;
        double fadeOutEnd = 0;
        int customLow = 20;
        int customHigh = 22050;
        carta::SpectrumWindow window = carta::SpectrumWindow::hann;
        int floorDb = -80;
        carta::FadeShape fadeInShape = carta::FadeShape::linear;
        carta::FadeShape fadeOutShape = carta::FadeShape::linear;

        bool operator<(const SpectrumKey& other) const noexcept {
            const auto less = std::less<const carta::Audio*>{};
            if (less(source, other.source)) {
                return true;
            }
            if (less(other.source, source)) {
                return false;
            }
            return std::tie(start, step, columns, fftOrder, band, displayRate, offset, duration,
                            gain, fadeIn, fadeOut, fadeInStart, fadeOutEnd, customLow, customHigh,
                            window, floorDb, fadeInShape, fadeOutShape) <
                   std::tie(other.start, other.step, other.columns, other.fftOrder, other.band,
                            other.displayRate, other.offset, other.duration, other.gain,
                            other.fadeIn, other.fadeOut, other.fadeInStart, other.fadeOutEnd,
                            other.customLow, other.customHigh, other.window, other.floorDb,
                            other.fadeInShape, other.fadeOutShape);
        }
    };

    struct WaveformKey {
        const carta::Audio* source = nullptr;
        int channel = 0;
        int left = 0;
        int width = 0;
        int firstX = 0;
        int columns = 0;
        double viewStart = 0;
        double span = 0;
        double start = 0;
        double offset = 0;
        double duration = 0;
        double fadeIn = 0;
        double fadeOut = 0;
        double fadeInStart = 0;
        double fadeOutEnd = 0;
        float gain = 1;
        bool rms = false;
        carta::FadeShape fadeInShape = carta::FadeShape::linear;
        carta::FadeShape fadeOutShape = carta::FadeShape::linear;

        bool operator<(const WaveformKey& other) const noexcept {
            const auto less = std::less<const carta::Audio*>{};
            if (less(source, other.source)) {
                return true;
            }
            if (less(other.source, source)) {
                return false;
            }
            return std::tie(channel, left, width, firstX, columns, viewStart, span, start, offset,
                            duration, fadeIn, fadeOut, fadeInStart, fadeOutEnd, gain, rms,
                            fadeInShape, fadeOutShape) <
                   std::tie(other.channel, other.left, other.width, other.firstX, other.columns,
                            other.viewStart, other.span, other.start, other.offset, other.duration,
                            other.fadeIn, other.fadeOut, other.fadeInStart, other.fadeOutEnd,
                            other.gain, other.rms, other.fadeInShape, other.fadeOutShape);
        }
    };

    struct WaveformEntry {
        std::weak_ptr<carta::Audio> source;
        std::shared_ptr<const std::vector<carta::WaveformColumn>> columns;
        std::shared_ptr<std::atomic<bool>> cancelled;
        bool pending = false;
        bool failed = false;
        size_t bytes = 0;
        juce::uint64 lastUse = 0;
        juce::uint64 seen = 0;
    };

    std::map<WaveformKey, WaveformEntry> waveforms;
    juce::ThreadPool waveformPool{1};
    juce::uint64 waveformUse = 0;
    juce::uint64 waveformPaintEpoch = 0;

    struct SpectrumEntry {
        std::weak_ptr<carta::Audio> source;
        juce::Image image;
        std::shared_ptr<std::atomic<bool>> cancelled;
        bool pending = false;
        bool failed = false;
        size_t bytes = 0;
        juce::uint64 lastUsed = 0;
    };

    std::map<SpectrumKey, SpectrumEntry> spectra;
    juce::ThreadPool spectrumPool{1};
    bool spectrumView = false;
    bool showClipPeakWarnings = true;
    bool showWaveformRms = false;
    int spectrumFftOrder = 11;
    int waveformVerticalZoom = 1;
    carta::SpectrumBand spectrumBand = carta::SpectrumBand::full;
#if JUCE_LINUX || CARTA_STUDIO_COMPONENT_TEST
    carta::StableSpectrumRaster stableSpectrumRaster{16u * 1024u * 1024u};
    bool useStableSpectrumRaster() const noexcept {
#if CARTA_STUDIO_COMPONENT_TEST
        return testStableSpectrumRaster;
#else
        return true;
#endif
    }
#endif
#if CARTA_STUDIO_COMPONENT_TEST
#if JUCE_LINUX
    static constexpr bool defaultStableSpectrumRaster = true;
#else
    static constexpr bool defaultStableSpectrumRaster = false;
#endif
    bool testStableSpectrumRaster = defaultStableSpectrumRaster;
#endif
    int spectrumCustomLow = 20;
    int spectrumCustomHigh = 22050;
    carta::SpectrumWindow spectrumWindow = carta::SpectrumWindow::hann;
    int spectrumFloorDb = -80;
    carta::WaveformScale waveformScale = carta::WaveformScale::linear;
    bool waveformHalf = false;
    bool spectrumViewportValid = false;
    bool spectrumViewportEnabled = false;
    int spectrumViewportWidth = 0;
    double spectrumViewportStart = 0;
    double spectrumViewportSpan = 0;
    juce::uint64 spectrumUse = 0;
    juce::uint64 spectrumViewportGeneration = 0;

    // --- Background job pools, IPC snapshots, recovery state, and import queue ---
    juce::var lastIpcExport;
    juce::var lastIpcProject;
    juce::uint64 lastIpcExportGeneration = 0;
    juce::uint64 lastIpcProjectGeneration = 0;
    bool lastIpcExportHasGeneration = false;
    bool lastIpcProjectHasSnapshot = false;
    carta::WavRange lastIpcProjectLoop{};
    std::shared_ptr<ExportTask> activeExport;
    juce::ThreadPool exportPool{1};
    std::shared_ptr<LevelAnalysisTask> activeLevelAnalysis;
    juce::ThreadPool levelPool{1};
    juce::var lastIpcAnalysis;
    juce::uint64 lastIpcAnalysisGeneration = 0;
    int lastIpcAnalysisChannels = 0;
    int lastIpcSuggestionCount = 0;
    juce::String lastIpcSuggestions;
    juce::String lastIpcAnalysisScope;
    carta::TimeSelection lastIpcAnalysisSelection;
    std::vector<int> lastIpcAnalysisTracks;
    std::vector<carta::ClipReference> lastIpcAnalysisClips;
    std::shared_ptr<OpenTask> activeOpen;
    juce::ThreadPool openPool{1};
    std::shared_ptr<RecoveryTask> activeRecovery;
    juce::ThreadPool recoveryPool{1};
    juce::File recoveryDirectory;
    juce::File recoveryFile;
    juce::uint64 observedGeneration = 0;
    juce::uint64 recoveryRevision = 0;
    double lastEditTime = 0;
    double lastRecoveryAttempt = 0;
    bool hasRecoveryRevision = false;
    bool cleanExitApproved = false;
    bool headlessMode = false;
    std::shared_ptr<SaveTask> activeSave;
    juce::ThreadPool savePool{1};
    juce::uint64 editGeneration = 0;
    carta::SourceVerificationCache sourceChecks;
    juce::var lastIpcSourceCheck;
    juce::uint64 lastIpcSourceGeneration = 0;
    std::shared_ptr<SourceCheckTask> activeSourceCheck;
    juce::ThreadPool sourceCheckPool{1};
    int menuSourceCheckState = 0;
    std::shared_ptr<MarkerExchangeTask> activeMarkerExchange;
    juce::ThreadPool markerExchangePool{1};
    juce::var lastIpcMarkerExchange;
    juce::uint64 lastIpcMarkerExchangeGeneration = 0;
    juce::ThreadPool capturePool{1};
    juce::var lastIpcCapture;
    bool captureRunning = false;
    std::vector<std::shared_ptr<ImportTask>> pendingImports;
    juce::ThreadPool importPool{1};
    std::map<juce::uint64, juce::int64> importBatchCursor;
    juce::uint64 nextImportBatchId = 1;
    std::vector<std::pair<juce::String, juce::String>> importIssues;
    juce::int64 omittedImportIssues = 0;
    bool fileDropGuideVisible = false;
    juce::int64 fileDropSample = 0;
    int fileDropTrack = -1;
    std::shared_ptr<ImportCache> importCache = std::make_shared<ImportCache>();

    // --- Layout geometry constants (pixels) ---
    static constexpr int trackLabelWidth = 110;
    static constexpr int scaleWidth = 52;
    static constexpr int labelWidth = trackLabelWidth + scaleWidth;
    static constexpr int titleHeight = 19;
};
