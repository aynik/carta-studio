/**
 * @file StudioLifecycle.cpp
 * @brief Studio construction/shutdown, real-time audio callback, UI layout, theming, and 20 Hz
 * timer.
 *
 * Implements `Studio::Studio`, `Studio::~Studio`, `getNextAudioBlock` (real-time
 * audio thread), `resized`, `updateAppearance`, `refreshDiscWorkspace`, and
 * `timerCallback` (message thread).
 *
 * Threading and lock safety:
 * - `getNextAudioBlock` acquires `lock` with `ScopedTryLock` only; if the
 *   message thread holds `lock` during an edit, it clears the buffer, increments
 *   `editLockSilentBlocks`, sets `audioLockContention`, and returns immediately.
 * - `~Studio` cancels all background tasks, shuts down JUCE audio, and joins
 *   every worker `ThreadPool` before destroying components or `neutralLook`.
 * - `timerCallback` runs at 20 Hz on the message thread to commit ready imports,
 *   advance drag edge-scrolling, refresh transport/peak readouts, and schedule
 *   minimal playhead repaint regions.
 */

#include "Studio/Studio.h"

namespace {

juce::int64 cursorSampleFromPosition(double displayPosition) noexcept {
    const double frame = displayPosition * carta::sessionRate;
    if (!std::isfinite(frame) || frame < 0 || frame >= 9007199254740992.0) {
        return 0;
    }
    return static_cast<juce::int64>(std::llround(frame));
}

juce::String formatOutputPeakDb(float value) {
    if (!(value > 0)) {
        return "-inf";
    }
    if (!std::isfinite(value)) {
        return "+inf!";
    }
    const auto db = juce::Decibels::gainToDecibels(value, -160.0f);
    return (db > 0 ? juce::String("+") : juce::String()) + juce::String(db, 1) +
           (value >= 1.0f ? "!" : "");
}

} // namespace

Studio::Studio(bool headless) : headlessMode(headless) {
    const auto overridePath =
        juce::SystemStats::getEnvironmentVariable("CARTA_STUDIO_RECOVERY_DIR", {});
    recoveryDirectory =
        juce::File::isAbsolutePath(overridePath)
            ? juce::File(overridePath)
            : juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                  .getChildFile("Carta Studio")
                  .getChildFile("Recovery");
    recoveryFile = newRecoveryPath();
    lastEditTime = lastRecoveryAttempt = juce::Time::getMillisecondCounterHiRes();

    juce::LookAndFeel::setDefaultLookAndFeel(&neutralLook);
    setLookAndFeel(&neutralLook);

    commandManager.registerAllCommandsForTarget(this);
    commandManager.setFirstCommandTarget(this);

    setWantsKeyboardFocus(true);
    setTitle("Carta Studio timeline");
    setHelpText(
        "Use Alt and arrow keys to select layers and clips. Press F3 to browse layers and clips. "
        "Press F4 for timeline status. MiniDisc tracks are laid out sequentially along Layer 1 or "
        "custom timeline ranges in the bottom MiniDisc workspace.");

#if !CARTA_STUDIO_COMPONENT_TEST
    if (!headlessMode) {
        carta::setHiMdElevationPromptEnabled(true);
    }
#endif

    // --- MiniDisc workspace callbacks ---
    addAndMakeVisible(discWorkspace);
    discWorkspace.onCommand = [this](const juce::StringArray& command, juce::uint64 revision) {
        if (revision != editGeneration) {
            status.setText("Project changed; review the MiniDisc workspace and try again.",
                           juce::dontSendNotification);
            refreshDiscWorkspace(true);
            return false;
        }
        const auto result = ipcCommand(command);
        const bool ok = static_cast<bool>(result["ok"]);
        const auto payload = result["result"];
        if (!ok) {
            status.setText(result["error"].toString(), juce::dontSendNotification);
        } else if (payload.isString()) {
            status.setText(payload.toString(), juce::dontSendNotification);
        }
        refreshDiscWorkspace(true);
        return ok;
    };
    discWorkspace.onSyncFromClips = [this] {
        discWorkspace.onCommand({"disc", "sync", "clips"}, editGeneration);
    };
    discWorkspace.onEditDiscFormsAndTimestamp = [this] { editDiscFormsAndTimestamp(); };
    discWorkspace.onEditTrackForms = [this] { editSelectedTrackTitleForms(); };
    discWorkspace.onEditTrackFlags = [this] { editSelectedTrackMdFlags(); };
    discWorkspace.onInspectUtoc = [this] { inspectCurrentUtoc(); };
    discWorkspace.onImportUtoc = [this] { importUtocDialog(); };
    discWorkspace.onExportUtoc = [this] { exportCurrentUtocDialog(); };
    discWorkspace.onReviewWav = [this] { startDiscProgramWavExport(); };
    discWorkspace.onRefreshRecord = [this] { refreshRecordWorkspace(true, true); };
    discWorkspace.onRecordPreflight = [this] { triggerRecordProbeFromUi(); };
    discWorkspace.onLoadDisc = [this] { triggerLoadDiscFromUi(); };
    discWorkspace.onDownloadDiscAudio = [this] { triggerDownloadDiscAudioFromUi({}); };
    discWorkspace.onCancelDownloadAudio = [this] { cancelActiveDiscDownload(); };
    discWorkspace.onReadDiscToc = [this] { triggerReadDiscTocFromUi(); };
    discWorkspace.onEraseDisc = [this] { triggerEraseDiscFromUi(); };
    discWorkspace.onRecordWriteUtoc = [this] { triggerRecordWriteUtocFromUi(); };
    discWorkspace.onRecordStart = [this] { triggerRecordStartFromUi(); };
    discWorkspace.onDumpHiMdTrack = [this](int trackIndex) {
        triggerHiMdDumpTrackFromUi(trackIndex);
    };
    discWorkspace.onHiMdDiscTools = [this] { triggerHiMdDiscToolsMenuFromUi(); };
    discWorkspace.onRenameHiMdTrack = [this](int trackIndex) {
        triggerHiMdRenameTrackFromUi(trackIndex);
    };
    discWorkspace.onMoveHiMdTrack = [this](int trackIndex, int delta) {
        triggerHiMdMoveTrackFromUi(trackIndex, delta);
    };
    discWorkspace.onDeleteHiMdTrack = [this](int trackIndex) {
        triggerHiMdDeleteTrackFromUi(trackIndex);
    };
    discWorkspace.onImportAudio = [this] { importAudio(); };
    auto seekMdTimelineRange = [this](juce::int64 startSample, juce::int64 endSample,
                                      const juce::String& focusTrackId) {
        const auto spanEnd = carta::projectEndSample(session);
        if (endSample <= startSample || endSample > spanEnd) {
            status.setText("This audio range extends beyond current timeline audio.",
                           juce::dontSendNotification);
            return;
        }
        const bool keepPlayback = selectionPlaybackContinues();
        stopSpecialAudition();
        carta::activateTimeSelection(timeSelection, timelineClipSelection,
                                     {startSample, endSample, 0, 0},
                                     static_cast<int>(session.tracks.size()));
        if (!keepPlayback) {
            position = static_cast<double>(startSample) / carta::sessionRate;
            playing = false;
            auditionActive = false;
        }
        const auto at = static_cast<double>(startSample) / carta::sessionRate;
        if (at < viewStart || at >= viewStart + viewLength()) {
            viewStart = juce::jlimit(0.0, std::max(0.0, fullLength() - viewLength()),
                                     at - viewLength() * 0.25);
            suspendFollowingForNavigation();
        }
        refreshDiscWorkspace(false, focusTrackId);
        menuItemsChanged();
        repaint();
    };
    discWorkspace.onSeekPart = [this, seekMdTimelineRange](const juce::String& trackId, int partIndex,
                                                           juce::uint64 revision) {
        if (revision != editGeneration || dragMode != kDragNone) {
            return;
        }
        const int idx = carta::findMdTrackIndex(session.mdDisc, trackId.toStdString());
        if (idx < 0 || idx >= static_cast<int>(session.mdDisc.tracks.size())) {
            return;
        }
        const auto& parts = session.mdDisc.tracks[static_cast<size_t>(idx)].parts;
        if (partIndex < 0 || partIndex >= static_cast<int>(parts.size())) {
            return;
        }
        const auto& part = parts[static_cast<size_t>(partIndex)];
        seekMdTimelineRange(part.startSample, part.endSample, trackId);
    };
    discWorkspace.onSeekRange = [this, seekMdTimelineRange](juce::int64 startSample,
                                                            juce::int64 endSample,
                                                            juce::uint64 revision) {
        if (revision != editGeneration || dragMode != kDragNone) {
            return;
        }
        seekMdTimelineRange(startSample, endSample, {});
    };
    discWorkspace.onFeedback = [this](const juce::String& message) {
        status.setText(message, juce::dontSendNotification);
    };
    discWorkspace.onTabChanged = [this](const juce::String& view) {
        if (view == "record" && (!netMdScannedOnce || !hiMdScannedOnce)) {
            refreshRecordWorkspace(true, false);
        } else {
            refreshRecordWorkspace(false);
        }
        saveWorkspacePreferences();
        menuItemsChanged();
    };
    discWorkspace.onTrackSelectionChanged = [this](const juce::String&) {
        repaint(mdRibbonArea());
    };
    discWorkspace.onClose = [this] { setDiscWorkspaceVisible(false); };
    discWorkspace.onPreviewTrack = [this](const juce::String& id, bool throughEnd) {
        startDiscPreview(editGeneration, id, throughEnd);
    };
    discWorkspace.onResize = [this](int delta) {
        if (!discResizeActive) {
            discResizeStart = visibleDiscWorkspaceHeight();
            discResizeActive = true;
        }
        discWorkspaceHeight = juce::jlimit(180, std::max(180, getHeight() - menuHeight() - 175),
                                           discResizeStart - delta);
        resized();
        repaint();
    };
    discWorkspace.onResizeEnd = [this] {
        discResizeActive = false;
        saveWorkspacePreferences();
    };

    // --- Toolbar controls, labels, overview scrollbar, and menu bar ---
    addAndMakeVisible(play);
    addAndMakeVisible(stop);
    addAndMakeVisible(zoomOutButton);
    addAndMakeVisible(zoomInButton);
    addAndMakeVisible(zoomFitButton);
    addAndMakeVisible(add);
    addAndMakeVisible(addLayerButton);
    addAndMakeVisible(splitMdButton);
    addAndMakeVisible(snapButton);
    addAndMakeVisible(status);
    addAndMakeVisible(clock);
    addAndMakeVisible(timelineReadout);
    addChildComponent(exportStatus);
    addChildComponent(outputPeakLabel);

    addAndMakeVisible(timelineScroll);
    timelineScroll.addListener(this);
    timelineScroll.setAutoHide(false);
    timelineScroll.setWantsKeyboardFocus(true);
    timelineScroll.setTitle("Timeline overview");
    timelineScroll.setDescription(
        "Scroll the visible timeline horizontally. Arrow keys move by one tenth of the visible "
        "range; Page Up and Page Down move by a page. Space plays or pauses; K stops at the "
        "playhead.");
    timelineReadout.setTooltip(
        "Playhead and selection positions in the chosen time units; selection end is exclusive. F4 "
        "announces exact project samples.");
    outputPeakLabel.setTitle("Playback output sample peaks");
    outputPeakLabel.setFont(juce::FontOptions(11.0f));
    outputPeakLabel.setTooltip(
        "Current / held playback output sample peaks in dBFS. These are not true peaks or an "
        "export preflight.");

#if JUCE_MAC && !CARTA_STUDIO_COMPONENT_TEST
    if (!headlessMode) {
        juce::MenuBarModel::setMacMainMenu(this);
    }
#elif !JUCE_MAC
    menuBar = std::make_unique<juce::MenuBarComponent>(static_cast<juce::MenuBarModel*>(this));
    addAndMakeVisible(*menuBar);
#endif

    updateAppearance();

    mode.addItem("Stereo", 2);
    mode.addItem("Mono", 1);
    mode.setSelectedId(2, juce::dontSendNotification);
    mode.onChange = [this] {
        juce::String error;
        if (!changeMasterChannels(mode.getSelectedId(), error)) {
            mode.setSelectedId(masterChannels, juce::dontSendNotification);
            if (error.isNotEmpty()) {
                status.setText(error, juce::dontSendNotification);
            }
        }
    };

    const auto oldRecoveries =
        recoveryDirectory.findChildFiles(juce::File::findFiles, false, "*.recovery.carta");
    status.setText(
        oldRecoveries.isEmpty()
            ? "Import audio to start arranging. Click the ruler to move the playhead."
            : "Recovery snapshots are available in File > Recovery > Recover autosaved "
              "session.",
        juce::dontSendNotification);

    zoomOutButton.setTooltip("Zoom out around the playhead (-)");
    zoomOutButton.setTitle("Zoom out");
    zoomInButton.setTooltip("Zoom in around the playhead (+)");
    zoomInButton.setTitle("Zoom in");
    zoomFitButton.setTooltip("Fit the arrangement in the timeline");
    zoomFitButton.setTitle("Fit arrangement");
    add.setTooltip("Import audio files sequentially onto Layer 1 and create MD tracks");
    add.setTitle("Import audio");
    addLayerButton.setTooltip(
        "Add an empty mixing layer (e.g. for overlays, voiceover, or backing stems)");
    addLayerButton.setTitle("Add empty layer");
    splitMdButton.setTooltip(
        "Split the MD track at the playhead (or create MD tracks from clips first)");
    splitMdButton.setTitle("Add MD track split");
    splitMdButton.onClick = [this] { splitMdTrackAtPlayhead(); };

    snapButton.setTitle("Toggle magnetic snapping");
    snapButton.onClick = [this] {
        snapMode = snapMode == 0 ? 2 : 0;
        syncSnapButtonAppearance();
        menuItemsChanged();
        status.setText(snapMode == 0
                           ? "Snapping off."
                           : "Snap to grid and boundaries; hold Shift while dragging to override.",
                       juce::dontSendNotification);
        repaint();
    };
    syncSnapButtonAppearance();

    marker.setTooltip("Add a track marker at the playhead");
    marker.setTitle("Add track marker");
    for (auto* button : {&play, &stop, &zoomOutButton, &zoomInButton, &zoomFitButton, &add,
                         &addLayerButton, &splitMdButton, &snapButton, &marker}) {
        carta::theme::setButtonRole(*button, carta::theme::kRoleToolbar);
    }
    for (auto* button : {&zoomOutButton, &zoomInButton, &zoomFitButton, &add, &addLayerButton,
                         &splitMdButton, &snapButton, &marker}) {
        button->setWantsKeyboardFocus(false);
    }

    zoomOutButton.onClick = [this] { zoomAt(0.5); };
    zoomInButton.onClick = [this] { zoomAt(2.0); };
    zoomFitButton.onClick = [this] { zoomToArrangement(); };
    add.onClick = [this] { importAudio(); };
    addLayerButton.onClick = [this] {
        juce::String error;
        if (!changeTracks(0, -1,
                          "Layer " + juce::String(static_cast<int>(session.tracks.size() + 1)),
                          error) &&
            error.isNotEmpty()) {
            status.setText(error, juce::dontSendNotification);
        }
    };

    // --- Transport and quick-edit button handlers ---
    play.onClick = [this] {
        const auto error = audioOutputError();
        bool nowPlaying = false;
        bool rejected = false;
        {
            const juce::ScopedLock guard(lock);
            if (!playing && error.isNotEmpty()) {
                rejected = true;
            } else {
                if (!playing && (!auditionActive || position >= auditionRange.end)) {
                    auditionActive = false;
                    if (position >= playbackMasterRange().end) {
                        position = 0;
                    }
                }
                playing = !playing;
                nowPlaying = playing;
            }
        }
        const juce::String message = rejected ? error : nowPlaying ? "Playing." : "Paused.";
        status.setText(message, juce::dontSendNotification);
        announceNavigation(message);
        repaint();
    };
    stop.onClick = [this] {
        {
            const juce::ScopedLock guard(lock);
            playing = false;
            position = 0;
            auditionActive = false;
            cutPreviewActive = false;
            groupAuditionActive = false;
        }
        const juce::String message = "Stopped; returned to the start.";
        status.setText(message, juce::dontSendNotification);
        announceNavigation(message);
        repaint();
    };
    split.onClick = [this] {
        if (timeSelection.hasRange() && !selectedTimelineTracks().empty()) {
            applyIndependentTrackRange(editGeneration, timeSelection, selectedTimelineTracks(), 11);
            return;
        }
        if (!timeSelection.hasRange() && ensureFocusedPersistentGroup()) {
            performTimelineClipAction(192);
            return;
        }
        splitFocusedAudio();
    };
    remove.onClick = [this] {
        if (timeSelection.hasRange()) {
            deleteSelection();
            return;
        }
        if (ensureFocusedPersistentGroup()) {
            performTimelineClipAction(190);
            return;
        }
        juce::String error;
        if (!editFocusedClip(1, 0, error) && error.isNotEmpty()) {
            status.setText(error, juce::dontSendNotification);
        }
    };
    marker.onClick = [this] {
        double samplePosition = 0;
        {
            const juce::ScopedLock guard(lock);
            samplePosition = position * carta::sessionRate;
        }
        if (!std::isfinite(samplePosition) || samplePosition < 0 ||
            samplePosition >= static_cast<double>(std::numeric_limits<juce::int64>::max())) {
            status.setText("Invalid marker position.", juce::dontSendNotification);
            return;
        }
        int index = -1;
        juce::String error;
        const bool changed = mutateMarkers(
            [&](carta::Session& candidate) {
                candidate.markers.push_back(
                    {0, "Marker " + juce::String(candidate.markers.size() + 1)});
                index = static_cast<int>(candidate.markers.size()) - 1;
                return carta::setMarkerSample(
                    candidate, index, static_cast<juce::int64>(std::llround(samplePosition)),
                    error);
            },
            [&] { selectedMarker = index; }, "Add marker", error);
        status.setText(changed ? "Added track marker." : error, juce::dontSendNotification);
    };
    undo.onClick = [this] { restoreHistory(false); };
    save.onClick = [this] { saveProject(); };
    load.onClick = [this] { loadProject(); };
    exportButton.onClick = [this] { exportWav(); };

#if !CARTA_STUDIO_COMPONENT_TEST
    audioSettingsFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                            .getChildFile("Carta Studio")
                            .getChildFile("audio-device.xml");
    restoreWorkspacePreferences(audioSettingsFile.getSiblingFile("disc-workspace.xml"));
    std::unique_ptr<juce::XmlElement> savedAudio;
    juce::String settingsError;
    const bool settingsValid =
        carta::loadDeviceSettings(audioSettingsFile, savedAudio, settingsError);
    setSize(1100, 650);
    if (!headlessMode) {
        setAudioChannels(0, 2, savedAudio.get());
        if (!settingsValid) {
            status.setText(settingsError + " Using default audio settings.",
                           juce::dontSendNotification);
        } else if (savedAudio) {
            carta::AudioOutputSnapshot actual;
            if (auto* device = deviceManager.getCurrentAudioDevice()) {
                actual.open = device->isOpen();
                actual.name = device->getName();
                actual.type = device->getTypeName();
                actual.sampleRate = device->getCurrentSampleRate();
                actual.bufferSize = device->getCurrentBufferSizeSamples();
                actual.channels = device->getActiveOutputChannels();
            }
            const auto difference = carta::describeDeviceRestoreDifference(*savedAudio, actual);
            if (difference.isNotEmpty()) {
                status.setText(difference, juce::dontSendNotification);
            }
        }
    }
    startTimerHz(20);
#else
    setSize(1100, 650);
#endif
}

Studio::~Studio() {
    stopTimer();
    tooltipWindow.reset();
#if !CARTA_STUDIO_COMPONENT_TEST
    if (!headlessMode) {
        saveAudioDevicePreferences(false);
    }
#endif
    audioSettingsWindow.reset();

    netMdStopRequested.store(true);
    netMdWorker.reset();
    hiMdWorker.reset();
    carta::stopHiMdElevatedSession();

    if (activeExport) {
        activeExport->cancelled.store(true);
    }
    if (activeOpen) {
        activeOpen->cancelled.store(true);
    }
    if (activeSave) {
        activeSave->cancelled.store(true);
    }
    if (activeSourceCheck) {
        activeSourceCheck->cancelled.store(true);
    }
    if (activeLevelAnalysis) {
        activeLevelAnalysis->cancelled.store(true);
    }
    if (activeMarkerExchange) {
        activeMarkerExchange->cancelled.store(true);
    }
    if (cleanExitApproved || !hasUnsavedChanges()) {
        retireRecovery();
    }
    for (const auto& task : pendingImports) {
        task->cancelled.store(true);
    }

    shutdownAudio();
    playbackIndexPool.removeAllJobs(true, -1);
    importPool.removeAllJobs(true, -1);
    exportPool.removeAllJobs(true, -1);
    levelPool.removeAllJobs(true, -1);
    openPool.removeAllJobs(true, -1);
    savePool.removeAllJobs(true, -1);
    recoveryPool.removeAllJobs(true, -1);
    sourceCheckPool.removeAllJobs(true, -1);
    markerExchangePool.removeAllJobs(true, -1);
    capturePool.removeAllJobs(true, -1);

    for (auto& item : waveforms) {
        if (item.second.cancelled) {
            item.second.cancelled->store(true);
        }
    }
    waveformPool.removeAllJobs(true, -1);
    spectrumPool.removeAllJobs(true, -1);

#if JUCE_MAC && !CARTA_STUDIO_COMPONENT_TEST
    if (!headlessMode) {
        juce::MenuBarModel::setMacMainMenu(nullptr);
    }
#elif !JUCE_MAC
    menuBar.reset();
#endif

    setLookAndFeel(nullptr);
    if (&juce::LookAndFeel::getDefaultLookAndFeel() == &neutralLook) {
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }
}

bool Studio::hasPendingGestureEdit() const {
    return ((dragMode >= kDragClipMove && dragMode <= kDragTrackHeight) ||
            dragMode == kDragChannelHeight || dragMode == kDragHeaderControl ||
            dragMode == kDragMdRibbon) &&
           dragged;
}

bool Studio::hasUnsavedChanges() {
    const juce::ScopedLock guard(lock);
    return dirty || hasPendingGestureEdit();
}

void Studio::approveCleanExit() noexcept {
    cleanExitApproved = true;
}

void Studio::requestClose(std::function<void()> close) {
    confirmSaveBefore([this, onClose = std::move(close)] {
        cleanExitApproved = true;
        onClose();
    });
}

void Studio::prepareToPlay(int, double rate) {
    const juce::ScopedLock guard(lock);
    deviceRate = rate;
    resampling.prepareOutput(rate);
}

void Studio::releaseResources() {
    const juce::ScopedLock guard(lock);
    if (playing || auditionActive) {
        if (cutPreviewActive) {
            position = cutPreviewOriginalPosition;
        }
        playing = false;
        auditionActive = false;
        cutPreviewActive = false;
        groupAuditionActive = false;
        audioInterrupted.store(true);
    }
    deviceRate = 0;
}

void Studio::getNextAudioBlock(const juce::AudioSourceChannelInfo& info) {
    info.clearActiveBufferRegion();
    const juce::ScopedTryLock guard(lock);
    if (!guard.isLocked()) {
        if (info.numSamples > 0 && playing.load(std::memory_order_relaxed)) {
            editLockSilentBlocks.fetch_add(1, std::memory_order_relaxed);
            audioLockContention.store(true, std::memory_order_relaxed);
        }
        return;
    }
    if (!playing || deviceRate <= 0) {
        return;
    }
    if (!carta::outputSupportsMaster(info.buffer->getNumChannels(), masterChannels)) {
        if (cutPreviewActive) {
            position = cutPreviewOriginalPosition;
        }
        playing = false;
        auditionActive = false;
        cutPreviewActive = false;
        groupAuditionActive = false;
        audioInterrupted.store(true);
        return;
    }
    if (savedLoopPlaying() &&
        static_cast<double>(savedLoop.end) / carta::sessionRate > playbackMasterRange().end) {
        playing = false;
        auditionActive = false;
        audioLoopInvalid.store(true);
        return;
    }
    const auto range = auditionActive ? auditionRange : playbackMasterRange();
    const auto& renderedSession =
        auditionActive && (cutPreviewActive || groupAuditionActive) ? cutPreviewSession : session;
    const double blockPosition = position;
    const auto* index = playbackIndexForCurrentSession();
    bool unsupportedRate = false;
    if (cutPreviewActive && discPreviewActive) {
        playing = carta::renderDiscPreview(
            renderedSession, discPreviewPlan, discPreviewRenderer, *info.buffer, info.startSample,
            info.numSamples, discPreviewFrames, deviceRate, masterChannels, &unsupportedRate);
        position = static_cast<double>(discPreviewPlan.programStart) / carta::sessionRate +
                   static_cast<double>(discPreviewFrames) / deviceRate;
    } else {
        playing = carta::renderPlayback(
            renderedSession, *info.buffer, info.startSample, info.numSamples, position, deviceRate,
            masterChannels, range, &resampling, &playbackClock, index, &unsupportedRate);
    }
    if (unsupportedRate) {
        info.clearActiveBufferRegion();
        position = cutPreviewActive ? cutPreviewOriginalPosition : blockPosition;
        playing = false;
        auditionActive = false;
        cutPreviewActive = false;
        groupAuditionActive = false;
        audioInvalidRate.store(true);
        return;
    }
    if (carta::containsNonFiniteAudio(*info.buffer, info.startSample, info.numSamples)) {
        info.clearActiveBufferRegion();
        position = cutPreviewActive ? cutPreviewOriginalPosition : blockPosition;
        playing = false;
        auditionActive = false;
        cutPreviewActive = false;
        groupAuditionActive = false;
        audioInvalidMix.store(true);
        return;
    }
    outputPeakHold.observe(*info.buffer, info.startSample, info.numSamples);
    if (!playing) {
        if (cutPreviewActive) {
            position = cutPreviewOriginalPosition;
        }
        auditionActive = false;
        cutPreviewActive = false;
        groupAuditionActive = false;
    }
}

void Studio::resized() {
    const int top = menuHeight();
#if !JUCE_MAC
    menuBar->setBounds(0, 0, getWidth(), top);
#endif
    int barX = 6;
    play.setBounds(barX, top + 3, 48, 26);
    barX += 49;
    stop.setBounds(barX, top + 3, 48, 26);
    barX += 55;
    zoomOutButton.setBounds(barX, top + 3, 24, 26);
    barX += 25;
    zoomInButton.setBounds(barX, top + 3, 24, 26);
    barX += 25;
    zoomFitButton.setBounds(barX, top + 3, 34, 26);
    barX += 40;
    add.setBounds(barX, top + 3, 62, 26);
    barX += 64;
    addLayerButton.setBounds(barX, top + 3, 60, 26);
    barX += 62;
    splitMdButton.setBounds(barX, top + 3, 94, 26);
    barX += 96;
    snapButton.setBounds(barX, top + 3, 52, 26);
    barX += 58;

    const int rightMargin = 8;
    const int availClock = std::max(0, getWidth() - barX - rightMargin);
    const int clockWidth = availClock >= 205 ? 205 : availClock;
    clock.setBounds(barX, top + 3, clockWidth, 26);

    const int afterClock = barX + clockWidth + 8;
    const int peakWidth =
        showOutputPeaks ? std::min(220, std::max(0, getWidth() - afterClock - rightMargin)) : 0;
    const int readoutWidth =
        std::max(0, getWidth() - afterClock - rightMargin - (peakWidth > 0 ? peakWidth + 4 : 0));
    timelineReadout.setBounds(afterClock, top + 3, readoutWidth, 26);
    outputPeakLabel.setBounds(getWidth() - rightMargin - peakWidth, top + 3, peakWidth, 26);

    const int progressWidth =
        (activeExport || activeOpen || activeSave || activeSourceCheck || activeLevelAnalysis ||
         !pendingImports.empty() || !importIssues.empty())
            ? std::min(330, std::max(0, getWidth() / 2))
            : 0;
    const int workspaceHeight = visibleDiscWorkspaceHeight();
    timelineScroll.setBounds(std::min(labelWidth, getWidth()),
                             std::max(menuHeight() + 32, getHeight() - 37 - workspaceHeight),
                             std::max(0, getWidth() - labelWidth), 14);
    discWorkspace.setBounds(0, getHeight() - 23 - workspaceHeight, getWidth(), workspaceHeight);
    discWorkspace.setVisible(discWorkspaceVisible && workspaceHeight > 0);
    status.setBounds(8, getHeight() - 23, std::max(0, getWidth() - 16 - progressWidth), 20);
    exportStatus.setBounds(getWidth() - progressWidth - 8, getHeight() - 23, progressWidth, 20);
}

std::vector<int> Studio::selectedTimelineTracks() const {
    if (timelineTrackRevision != editGeneration) {
        return {};
    }
    std::vector<int> result;
    result.reserve(timelineTracks.size());
    for (int track : timelineTracks) {
        if (track >= 0 && track < static_cast<int>(session.tracks.size())) {
            result.push_back(track);
        }
    }
    return result;
}

bool Studio::trackInTimeScope(int track) const {
    if (!timeSelection.hasRange() || track < 0 ||
        track >= static_cast<int>(session.tracks.size())) {
        return false;
    }
    if (timelineTrackRevision == editGeneration && !timelineTracks.empty()) {
        return timelineTracks.count(track) > 0;
    }
    return track >= timeSelection.firstTrack && track <= timeSelection.lastTrack;
}

juce::String Studio::describeTimelineTracks() const {
    const auto tracks = selectedTimelineTracks();
    if (tracks.empty()) {
        return "No independent tracks selected.";
    }
    juce::String result =
        juce::String(tracks.size()) +
        (tracks.size() == 1 ? " independent track selected: " : " independent tracks selected: ");
    for (size_t i = 0; i < std::min<size_t>(tracks.size(), 8); ++i) {
        if (i > 0) {
            result += ", ";
        }
        const int track = tracks[i];
        result += juce::String(track + 1) + " " + session.tracks[static_cast<size_t>(track)].name;
    }
    if (tracks.size() > 8) {
        result += ", and " + juce::String(tracks.size() - 8) + " more";
    }
    return result +
           ". Range editing commands use these tracks; all-track commands still affect all tracks.";
}

void Studio::toggleTimelineTrack(int track) {
    const juce::ScopedLock guard(lock);
    if (track < 0 || track >= static_cast<int>(session.tracks.size())) {
        status.setText("Focus a track first.", juce::dontSendNotification);
        return;
    }
    stopSpecialAudition();
    if (timelineTrackRevision != editGeneration) {
        timelineTracks.clear();
        timelineTrackRevision = editGeneration;
    }
    if (timelineTracks.count(track) > 0) {
        timelineTracks.erase(track);
    } else {
        timelineTracks.insert(track);
    }
    selectedTrack = track;
    selectedClip = -1;
    selectedMarker = -1;
    timelineClipSelection.clear();
    const auto message = describeTimelineTracks();
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    menuItemsChanged();
    repaint();
}

carta::PlaybackRange Studio::playbackMasterRange() {
    // Group previews, range selection, ruler seeking, marker moves and
    // track resizing do not change the master end. Live single-clip and
    // master-end drags can change it before they are committed.
    if (dragged &&
        ((dragMode >= kDragClipMove && dragMode <= kDragClipTrimRight && !draggingFade) ||
         (dragMode == kDragMarkerOrMasterEnd && draggingMasterEnd))) {
        return dragMasterRange;
    }
    if (!masterRangeValid || masterRangeRevision != editGeneration) {
        masterRange = carta::fullMasterPlaybackRange(session);
        masterRangeRevision = editGeneration;
        masterRangeValid = true;
    }
    return masterRange;
}

void Studio::refreshCommittedMasterRange() {
    masterRange = carta::fullMasterPlaybackRange(session);
    masterRangeRevision = editGeneration;
    masterRangeValid = true;
}

const carta::RenderClipIndex* Studio::playbackIndexForCurrentSession() const noexcept {
    if (!playbackIndex || playbackIndexRevision != editGeneration ||
        (auditionActive && (cutPreviewActive || groupAuditionActive)) ||
        (dragMode >= kDragClipMove && dragMode <= kDragClipTrimRight && !draggingFade)) {
        return nullptr;
    }
    return playbackIndex.get();
}

void Studio::queuePlaybackIndex() {
    if (!playbackIndexEnabled || playbackIndexBusy || dragMode != kDragNone ||
        (playbackIndex && playbackIndexRevision == editGeneration) ||
        juce::Time::getMillisecondCounterHiRes() < playbackIndexRetryAfter) {
        return;
    }
    const auto revision = editGeneration;
    carta::RenderClipIndex::Geometry geometry;
    try {
        geometry = carta::RenderClipIndex::captureGeometry(session);
    } catch (...) {
        playbackIndexRetryAfter = juce::Time::getMillisecondCounterHiRes() + 5000;
        return;
    }
    playbackIndexBusy = true;
    playbackIndexPool.addJob([safe = juce::Component::SafePointer<Studio>(this), revision,
                              snapshot = std::move(geometry)] {
        std::shared_ptr<carta::RenderClipIndex> built;
        try {
            built = std::make_shared<carta::RenderClipIndex>(snapshot);
        } catch (...) {}
        juce::MessageManager::callAsync([safe, revision, built] {
            if (safe == nullptr) {
                return;
            }
            safe->playbackIndexBusy = false;
            if (!built) {
                safe->playbackIndexRetryAfter = juce::Time::getMillisecondCounterHiRes() + 5000;
                return;
            }
            std::shared_ptr<carta::RenderClipIndex> old;
            {
                const juce::ScopedLock guard(safe->lock);
                if (safe->editGeneration == revision) {
                    old = std::move(safe->playbackIndex);
                    safe->playbackIndex = built;
                    safe->playbackIndexRevision = revision;
                }
            }
        });
    });
}

void Studio::prepareClipDragEnd() {
    dragOtherAudioEnd = 0;
    dragMinimumEnd = session.minimumEndSample;
    for (size_t track = 0; track < session.tracks.size(); ++track) {
        const auto& clips = session.tracks[track].clips;
        for (size_t clip = 0; clip < clips.size(); ++clip) {
            if (static_cast<int>(track) == selectedTrack &&
                static_cast<int>(clip) == selectedClip) {
                continue;
            }
            dragOtherAudioEnd =
                std::max(dragOtherAudioEnd, clips[clip].start + clips[clip].duration);
        }
    }
}

void Studio::refreshClipDragRange() {
    const auto& clip =
        session.tracks[static_cast<size_t>(selectedTrack)].clips[static_cast<size_t>(selectedClip)];
    const auto end = carta::projectEndSampleForAudioEnd(
        std::max(dragOtherAudioEnd, clip.start + clip.duration), dragMinimumEnd);
    dragMasterRange = {0, static_cast<double>(end) / carta::sessionRate, false};
}

std::unique_ptr<juce::AccessibilityHandler> Studio::createAccessibilityHandler() {
    juce::AccessibilityActions actions;
    actions.addAction(
        juce::AccessibilityActionType::press, [safe = juce::Component::SafePointer<Studio>(this)] {
            if (safe == nullptr) {
                return;
            }
            if (safe->dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                const juce::String message = "Finish the current drag or dialog first.";
                safe->status.setText(message, juce::dontSendNotification);
                safe->announceNavigation(message);
                return;
            }
            if (safe->session.tracks.empty()) {
                const juce::String message = "There are no tracks to browse.";
                safe->status.setText(message, juce::dontSendNotification);
                safe->announceNavigation(message);
                return;
            }
            safe->showTrackClipNavigator();
        });
    actions.addAction(juce::AccessibilityActionType::showMenu,
                      [safe = juce::Component::SafePointer<Studio>(this)] {
                          if (safe == nullptr) {
                              return;
                          }
                          if (safe->dragMode != kDragNone ||
                              juce::Component::getCurrentlyModalComponent() != nullptr) {
                              const juce::String message =
                                  "Finish the current drag or dialog first.";
                              safe->status.setText(message, juce::dontSendNotification);
                              safe->announceNavigation(message);
                              return;
                          }
                          safe->showCommandSearch();
                      });
    return std::make_unique<juce::AccessibilityHandler>(*this, juce::AccessibilityRole::group,
                                                        std::move(actions));
}

void Studio::focusTimeline() {
    if (juce::Component::getCurrentlyModalComponent() != nullptr) {
        return;
    }
    grabKeyboardFocus();
    const auto message =
        juce::String("Timeline focused. Press F4 for the current timeline status.");
    status.setText(message, juce::dontSendNotification);
    announceNavigation(message);
    repaint();
}

juce::String Studio::describeWaveformDisplay() const {
    auto text = juce::String(waveformHalf ? "Half-wave amplitude; polarity hidden. "
                                          : "Full-wave amplitude; polarity shown. ");
    text += waveformScale == carta::WaveformScale::linear
                ? "Linear waveform scale. "
                : "Logarithmic waveform scale, 60 decibels. ";
    const bool validTrack =
        selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size());
    const int waveformZoom =
        validTrack ? carta::trackWaveformZoom(session.tracks[static_cast<size_t>(selectedTrack)],
                                              waveformVerticalZoom)
                   : waveformVerticalZoom;
    text += (validTrack ? "Focused track waveform zoom " : "Default waveform zoom ") +
            juce::String(waveformZoom) + " times. ";
    text +=
        showWaveformRms ? "Source-channel RMS overlay on. " : "Source-channel RMS overlay off. ";
    text += "Waveforms are before track mixing.";
    return text;
}

void Studio::announceTimeline() {
    double displayPosition = 0;
    bool displayPlaying = false;
    bool displayLoopPlaying = false;
    {
        const juce::ScopedLock guard(lock);
        displayPosition = position;
        displayPlaying = playing;
        displayLoopPlaying = savedLoopPlaying();
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (timelineAnnouncementHook) {
        timelineAnnouncementHook();
    }
#endif
    const auto cursor = cursorSampleFromPosition(displayPosition);
    auto description = carta::describeTimeline(session, timeSelection, selectedTrack, selectedClip,
                                               selectedMarker, cursor, displayPlaying,
                                               &sourceChecks, !selectedTimelineTracks().empty()) +
                       " " +
                       carta::describeClipSelection(timelineClipSelection, editGeneration,
                                                    selectedTrack, selectedClip);
    description += " " + describeTimelineTracks() + " " + describeSavedLoop(displayLoopPlaying) +
                   " " + describeEditLockInterruptions() + " " + describeWaveformDisplay();
    if (liveInPresent && liveInGeneration == editGeneration) {
        description += " Selection in-point at sample " + juce::String(liveInSample) +
                       ". Press O to mark out.";
    }
    juce::AccessibilityHandler::postAnnouncement(
        description, juce::AccessibilityHandler::AnnouncementPriority::medium);
}

bool Studio::hasAnyAudioSelection() const {
    return selectedClip >= 0 || selectedMarker >= 0 || timeSelection.hasRange() ||
           timeSelection.hasPoint() || !timelineClipSelection.get(editGeneration).empty() ||
           !selectedTimelineTracks().empty() || liveInPresent;
}

void Studio::clearAudioSelection(bool announce, bool lockHeld) {
    const bool hadSelection = hasAnyAudioSelection();
    selectedClip = -1;
    selectedMarker = -1;
    timeSelection = {};
    timelineClipSelection.clear();
    timelineTracks.clear();
    liveInPresent = false;
    keyboardRange.active = false;
    keyboardTracks.active = false;
    if (hadSelection || announce) {
        if (announce) {
            status.setText("Selection cleared.", juce::dontSendNotification);
        }
        if (lockHeld) {
            const juce::ScopedUnlock unlock(lock);
            refreshDiscWorkspace(false);
            menuItemsChanged();
        } else {
            refreshDiscWorkspace(false);
            menuItemsChanged();
        }
        repaint();
    }
}

void Studio::syncSnapButtonAppearance() {
    const bool on = snapMode != 0;
    snapButton.setToggleState(on, juce::dontSendNotification);
    snapButton.setColour(juce::TextButton::buttonColourId,
                         darkAppearance ? juce::Colour(0xff2e2e2e) : juce::Colour(0xffececec));
    snapButton.setColour(juce::TextButton::buttonOnColourId,
                         darkAppearance ? juce::Colour(0xff565656) : juce::Colour(0xffcccccc));
    snapButton.setColour(juce::TextButton::textColourOnId,
                         darkAppearance ? juce::Colour(0xfff5f5f5) : juce::Colour(0xff181818));
    snapButton.setColour(juce::TextButton::textColourOffId,
                         darkAppearance ? juce::Colour(0xff9e9e9e) : juce::Colour(0xff666666));
    snapButton.setTooltip(
        snapMode == 0 ? "Magnetic snapping is off (click to snap MD track edges & clips; hold "
                        "Shift to bypass)"
        : snapMode == 1
            ? "Snap to grid enabled (click to turn snapping off; hold Shift to bypass)"
            : "Magnetic snap to clip boundaries & markers enabled (click to toggle; hold Shift to "
              "bypass)");
}

void Studio::splitMdTrackAtPlayhead() {
    if (session.length() <= 0) {
        status.setText("Import or arrange audio before splitting an MD track.",
                       juce::dontSendNotification);
        return;
    }
    if (!discWorkspaceVisible) {
        setDiscWorkspaceVisible(true);
    }
    double curPos = 0;
    {
        const juce::ScopedLock guard(lock);
        curPos = position;
    }
    const auto playheadSample = static_cast<juce::int64>(std::llround(curPos * carta::sessionRate));
    if (session.mdDisc.tracks.empty()) {
        discWorkspace.onCommand({"disc", "sync", "clips"}, editGeneration);
        if (session.mdDisc.tracks.empty()) {
            return;
        }
    }
    int matchIdx = -1;
    const int focused = discWorkspace.singleSelectedTrackIndex();
    const auto covers = [&](int idx0) {
        if (idx0 < 0 || idx0 >= static_cast<int>(session.mdDisc.tracks.size())) {
            return false;
        }
        for (const auto& part : session.mdDisc.tracks[static_cast<size_t>(idx0)].parts) {
            if (part.startSample < playheadSample && playheadSample < part.endSample) {
                return true;
            }
        }
        return false;
    };
    if (covers(focused)) {
        matchIdx = focused;
    } else {
        for (int i = 0; i < static_cast<int>(session.mdDisc.tracks.size()); ++i) {
            if (covers(i)) {
                matchIdx = i;
                break;
            }
        }
    }
    if (matchIdx < 0) {
        status.setText("Move the playhead inside an MD track to split it there.",
                       juce::dontSendNotification);
        return;
    }
    discWorkspace.onCommand(
        {"disc", "track", "split", juce::String(matchIdx), juce::String(playheadSample)},
        editGeneration);
}

void Studio::updateAppearance() {
    darkAppearance = juce::Desktop::getInstance().isDarkModeActive();
    const auto pal = carta::theme::palette(darkAppearance);
    neutralLook.setColourScheme(
        juce::LookAndFeel_V4::ColourScheme{
            pal.windowBg, pal.controlBg, pal.menuBg, pal.controlBorder, pal.textPrimary,
            pal.selectionBg, pal.selectionText, pal.controlHoverBg, pal.textPrimary});

    const auto text = pal.textPrimary;
    const auto control = pal.controlBg;
    const auto menuBg = pal.menuBg;
    const auto outline = pal.controlBorder;
    const auto hiBg = pal.selectionBg;
    const auto focusOutline = pal.focusOutline;

    neutralLook.setColour(juce::TextButton::buttonColourId, control);
    neutralLook.setColour(juce::TextButton::buttonOnColourId, hiBg);
    neutralLook.setColour(juce::TextButton::textColourOffId, text);
    neutralLook.setColour(juce::TextButton::textColourOnId, text);
    neutralLook.setColour(juce::TextEditor::backgroundColourId, pal.inputBg);
    neutralLook.setColour(juce::TextEditor::textColourId, text);
    neutralLook.setColour(juce::TextEditor::highlightColourId, hiBg);
    neutralLook.setColour(juce::TextEditor::highlightedTextColourId, pal.selectionText);
    neutralLook.setColour(juce::TextEditor::outlineColourId, outline);
    neutralLook.setColour(juce::TextEditor::focusedOutlineColourId, focusOutline);
    neutralLook.setColour(juce::Label::textColourId, text);
    neutralLook.setColour(juce::ComboBox::backgroundColourId, control);
    neutralLook.setColour(juce::ComboBox::buttonColourId, control);
    neutralLook.setColour(juce::ComboBox::textColourId, text);
    neutralLook.setColour(juce::ComboBox::arrowColourId, text);
    neutralLook.setColour(juce::ComboBox::outlineColourId, outline);
    neutralLook.setColour(juce::ComboBox::focusedOutlineColourId, focusOutline);
    neutralLook.setColour(juce::ListBox::backgroundColourId, control);
    neutralLook.setColour(juce::ListBox::outlineColourId, outline);
    neutralLook.setColour(juce::ListBox::textColourId, text);
    neutralLook.setColour(juce::ToggleButton::textColourId, text);
    neutralLook.setColour(juce::ToggleButton::tickColourId, text);
    neutralLook.setColour(juce::ToggleButton::tickDisabledColourId, outline);
    neutralLook.setColour(juce::PopupMenu::backgroundColourId, menuBg);
    neutralLook.setColour(juce::PopupMenu::textColourId, text);
    neutralLook.setColour(juce::PopupMenu::headerTextColourId, pal.textSecondary);
    neutralLook.setColour(juce::PopupMenu::highlightedBackgroundColourId, hiBg);
    neutralLook.setColour(juce::PopupMenu::highlightedTextColourId, pal.selectionText);
    neutralLook.setColour(juce::ScrollBar::thumbColourId, pal.scrollThumb);
    neutralLook.setColour(juce::ScrollBar::trackColourId, juce::Colours::transparentBlack);
    neutralLook.setColour(juce::AlertWindow::backgroundColourId, pal.dialogBg);
    neutralLook.setColour(juce::AlertWindow::textColourId, text);
    neutralLook.setColour(juce::AlertWindow::outlineColourId, outline);
    neutralLook.setColour(juce::ResizableWindow::backgroundColourId, pal.windowBg);
    neutralLook.setColour(juce::TooltipWindow::backgroundColourId, menuBg);
    neutralLook.setColour(juce::TooltipWindow::textColourId, text);
    neutralLook.setColour(juce::TooltipWindow::outlineColourId, outline);

    syncSnapButtonAppearance();
    if (audioSettingsWindow) {
        audioSettingsWindow->setBackgroundColour(pal.dialogBg);
        audioSettingsWindow->repaint();
    }
    status.setColour(juce::Label::textColourId, text.withAlpha(0.75f));
    exportStatus.setColour(juce::Label::textColourId, text.withAlpha(0.75f));
    clock.setColour(juce::Label::textColourId, text);
    timelineReadout.setColour(juce::Label::textColourId, text.withAlpha(0.8f));
    outputPeakLabel.setColour(juce::Label::textColourId, text.withAlpha(0.85f));
    sendLookAndFeelChange();
    discWorkspace.sendLookAndFeelChange();
    if (menuBar) {
        menuBar->sendLookAndFeelChange();
    }
    discWorkspace.syncAppearance();
    repaint();
}

int Studio::menuHeight() const {
#if JUCE_MAC
    return 0;
#else
    return 24;
#endif
}

void Studio::restoreWorkspacePreferences(const juce::File& file) {
    workspaceSettingsReady = false;
    workspaceSettingsFile = file;
    workspaceSettingsWritable = carta::loadWorkspacePreferences(file, lastWorkspacePreferences);
    if (workspaceSettingsWritable) {
        discWorkspaceHeight = std::max(180, lastWorkspacePreferences.height);
        discWorkspaceVisible = lastWorkspacePreferences.visible;
        juce::String ignored;
        discWorkspace.selectView(lastWorkspacePreferences.view, ignored);
    }
    workspaceSettingsReady = true;
}

void Studio::saveWorkspacePreferences() {
    if (!workspaceSettingsReady || !workspaceSettingsWritable) {
        return;
    }
    carta::WorkspacePreferences current{discWorkspaceHeight, discWorkspaceVisible,
                                        discWorkspace.currentView()};
    if (!(current == lastWorkspacePreferences) &&
        carta::saveWorkspacePreferences(workspaceSettingsFile, current)) {
        lastWorkspacePreferences = current;
    }
}

int Studio::visibleDiscWorkspaceHeight() const {
    return discWorkspaceVisible ? std::min(std::max(180, discWorkspaceHeight),
                                           std::max(0, getHeight() - menuHeight() - 175))
                                : 0;
}

void Studio::refreshDiscWorkspace(bool force, const juce::String& preferredTrackId) {
    const int ribbonNow = visibleMdRibbonHeight();
    if (ribbonNow != lastMdRibbonHeight) {
        lastMdRibbonHeight = ribbonNow;
        resized();
        repaint(timeline());
    }
    if (!discWorkspaceVisible) {
        repaint(mdRibbonArea());
        return;
    }
    const bool gesture = dragMode != kDragNone;
    const auto viewNow = discWorkspace.currentView();
    const auto outputError = audioOutputError();
    const bool selHasRange = timeSelection.hasRange();
    const auto selStart = timeSelection.start;
    const auto selEnd = timeSelection.end;
    if (!force && discWorkspaceRevision == editGeneration && preferredTrackId.isEmpty() &&
        discWorkspaceGesture == gesture && discWorkspaceLastTab == viewNow &&
        discWorkspaceAudioError == outputError && discWorkspaceSelHasRange == selHasRange &&
        discWorkspaceSelStart == selStart && discWorkspaceSelEnd == selEnd) {
        return;
    }
    discWorkspace.update(session, timeSelection, position, editGeneration, preferredTrackId);
    if (viewNow == "record" || force || discWorkspaceRevision != editGeneration) {
        refreshRecordWorkspace(false);
    }
    discWorkspaceRevision = editGeneration;
    discWorkspaceGesture = gesture;
    discWorkspaceLastTab = viewNow;
    discWorkspaceAudioError = outputError;
    discWorkspaceSelHasRange = selHasRange;
    discWorkspaceSelStart = selStart;
    discWorkspaceSelEnd = selEnd;
    repaint(mdRibbonArea());
}

void Studio::setDiscWorkspaceVisible(bool visible) {
    discResizeActive = false;
    discWorkspaceVisible = visible;
    discWorkspaceRevision = std::numeric_limits<juce::uint64>::max();
    resized();
    refreshDiscWorkspace(true);
    saveWorkspacePreferences();
    menuItemsChanged();
    repaint();
}

void Studio::showGuardedStudioAlert(const juce::MessageBoxOptions& options,
                                    std::function<void(int)> callback) {
    if (savePromptActive || juce::Component::getCurrentlyModalComponent() != nullptr) {
        status.setText("Finish the current dialog first.", juce::dontSendNotification);
        if (callback) {
            callback(-1);
        }
        return;
    }
    savePromptActive = true;
    showStudioAlert(options, [safe = juce::Component::SafePointer<Studio>(this),
                              done = std::move(callback)](int answer) {
        if (safe == nullptr) {
            return;
        }
        safe->savePromptActive = false;
        if (done) {
            done(answer);
        }
    });
}

juce::Rectangle<int> Studio::planPlayheadRepaint(double displayPosition) {
    const auto area = timeline();
    const double span = viewLength();
    const int left = std::min(area.getRight(), area.getX() + labelWidth);
    const int x = std::clamp(timeToX(displayPosition, area, span), left - 1, area.getRight());
    const bool full = !playheadRepaintValid || area != playheadRepaintTimeline ||
                      !std::equal_to<double>{}(viewStart, playheadRepaintViewStart) ||
                      !std::equal_to<double>{}(span, playheadRepaintSpan) ||
                      editGeneration != playheadRepaintGeneration ||
                      firstTrack != playheadRepaintFirstTrack || dragMode != kDragNone;
    const int previous = playheadRepaintX;
    playheadRepaintValid = true;
    playheadRepaintTimeline = area;
    playheadRepaintViewStart = viewStart;
    playheadRepaintSpan = span;
    playheadRepaintGeneration = editGeneration;
    playheadRepaintFirstTrack = firstTrack;
    playheadRepaintX = x;
    if (full) {
        return area;
    }
    if (previous == x) {
        return {};
    }
#if JUCE_LINUX
    // JUCE's software image scaler can produce different pixels at the
    // left edge of a clipped draw. Repaint the complete timeline while a
    // visible spectrogram is present so a moving playhead cannot leave a
    // vertical seam. Waveform-only views keep the narrow damage region.
    const auto visible = carta::visibleTrackRows(
        session, firstTrack, std::max(0, area.getHeight() - timelineHeaderHeight()));
    for (int track = visible.getStart(); track < visible.getEnd(); ++track) {
        const auto& state = session.tracks[static_cast<size_t>(track)];
        if (!state.collapsed && !state.clips.empty() &&
            (state.view == carta::TrackView::combined ||
             carta::trackShowsSpectrum(state, spectrumView))) {
            return area;
        }
    }
#endif
    const int x1 = std::max(area.getX(), std::min(previous, x) - 2);
    const int x2 = std::min(area.getRight(), std::max(previous, x) + 3);
    return {x1, area.getY(), std::max(0, x2 - x1), area.getHeight()};
}

void Studio::timerCallback() {
    const double beforeDragView = viewStart;
    if (darkAppearance != juce::Desktop::getInstance().isDarkModeActive()) {
        updateAppearance();
    }
    if (dragMode == kDragNone) {
        finishReadyImports();
    }
    pollNetMdJobProgress();
    pollHiMdJobProgress();
    refreshDiscWorkspace();
    queuePlaybackIndex();
    maybeAutosave();

    // --- Consume lock-free audio interruption/diagnostic flags ---
    if (audioLockContention.exchange(false, std::memory_order_relaxed)) {
        const juce::String message =
            "Playback briefly silenced while the editor was busy; the playhead did not advance.";
        status.setText(message, juce::dontSendNotification);
        announceNavigation(message);
    }
    if (audioInterrupted.exchange(false)) {
        const juce::String message =
            "Audio output stopped or changed; playback was stopped. Check Transport > Audio output "
            "settings.";
        status.setText(message, juce::dontSendNotification);
        announceNavigation(message);
    }
    if (audioLoopInvalid.exchange(false)) {
        const juce::String message =
            "Playback stopped: the saved loop extends beyond the master. Set a new loop region.";
        status.setText(message, juce::dontSendNotification);
        announceNavigation(message);
    }
    if (audioInvalidMix.exchange(false)) {
        const juce::String message =
            "Playback stopped: the mix produced non-finite audio. Check source levels and gain.";
        status.setText(message, juce::dontSendNotification);
        announceNavigation(message);
    }
    if (audioInvalidRate.exchange(false)) {
        const juce::String message =
            "Playback stopped: a source sample rate is too high for the output device. Convert the "
            "source to a lower rate or choose a higher output rate.";
        status.setText(message, juce::dontSendNotification);
        announceNavigation(message);
    }

    // --- Advance active drag edge-scrolling previews (message thread only) ---
    if (((dragMode >= kDragClipMove && dragMode <= kDragClipTrimRight) ||
         (dragMode == kDragMarkerOrMasterEnd && draggingMasterEnd) || dragMode == kDragGroupMove ||
         dragMode == kDragGroupEdge) &&
        clipPointerDragging) {
        scrollClipDrag();
    }
    // These drag previews and their scroll state belong to the message thread.
    if (dragMode == kDragMarquee && dragged) {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const double elapsed = std::clamp((now - marqueeScrollClock) / 1000.0, 0.0, 0.1);
        marqueeScrollClock = now;
        const auto area = timeline();
        const double before = viewStart;
        const int count = static_cast<int>(session.tracks.size());
        viewStart = carta::selectionEdgeScroll(viewStart, viewLength(), fullLength(),
                                               area.getX() + labelWidth, area.getRight(),
                                               marqueeEnd.x, elapsed);
        const int direction =
            count > 0 && firstTrack >= 0 && firstTrack < count
                ? carta::selectionTrackScrollDirection(area.getY() + timelineHeaderHeight(),
                                                       area.getBottom(), marqueeEnd.y, firstTrack,
                                                       count, trackArea(count - 1).getBottom())
                : 0;
        if (direction != marqueeVerticalDirection) {
            marqueeVerticalDirection = direction;
            marqueeVerticalElapsed = 0;
        }
        bool verticalMoved = false;
        if (direction != 0) {
            marqueeVerticalElapsed += elapsed;
            if (marqueeVerticalElapsed >= 0.2) {
                firstTrack += direction;
                marqueeVerticalElapsed -= 0.2;
                verticalMoved = true;
            }
        }
        if (verticalMoved || viewStart < before || viewStart > before) {
            updateMarqueePreview();
            repaint();
        }
    }
    if (dragMode == kDragTrackReorder && trackDragMoved) {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const double elapsed = std::clamp((now - trackDragClock) / 1000.0, 0.0, 0.1);
        trackDragClock = now;
        const auto area = timeline();
        const int count = static_cast<int>(session.tracks.size());
        const int direction =
            count > 0 && trackDragPointer.x >= area.getX() && trackDragPointer.x < area.getRight()
                ? carta::selectionTrackScrollDirection(
                      area.getY() + timelineHeaderHeight(), area.getBottom(), trackDragPointer.y,
                      firstTrack, count, trackArea(count - 1).getBottom())
                : 0;
        if (direction != trackDragScrollDirection) {
            trackDragScrollDirection = direction;
            trackDragScrollElapsed = 0;
        }
        if (direction != 0) {
            trackDragScrollElapsed += elapsed;
            if (trackDragScrollElapsed >= 0.2) {
                firstTrack += direction;
                trackDragScrollElapsed -= 0.2;
                updateDraggedTrack();
            }
        }
    }
    if ((dragMode == kDragTimeSelection || dragMode == kDragTimeSelectionResize) &&
        selectionPointerDragging) {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const auto area = timeline();
        const double before = viewStart;
        const double elapsed = std::clamp((now - selectionScrollClock) / 1000.0, 0.0, 0.1);
        viewStart = carta::selectionEdgeScroll(viewStart, viewLength(), fullLength(),
                                               area.getX() + labelWidth, area.getRight(),
                                               selectionPointer.x, elapsed);
        selectionScrollClock = now;
        selectionSnapAllowed = !juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
        bool verticalMoved = false;
        const int count = static_cast<int>(session.tracks.size());
        if (count > 0 && firstTrack >= 0 && firstTrack < count) {
            const int direction = carta::selectionTrackScrollDirection(
                area.getY() + timelineHeaderHeight(), area.getBottom(), selectionPointer.y,
                firstTrack, count, trackArea(count - 1).getBottom());
            if (direction != selectionVerticalDirection) {
                selectionVerticalDirection = direction;
                selectionVerticalElapsed = 0;
            }
            if (direction != 0) {
                selectionVerticalElapsed += elapsed;
                if (selectionVerticalElapsed >= 0.2) {
                    firstTrack += direction;
                    selectionVerticalElapsed -= 0.2;
                    verticalMoved = true;
                }
            }
        }
        if (verticalMoved || viewStart < before || viewStart > before) {
            updateDraggedSelection();
        }
    }
    if (dragMode == kDragSavedLoop && loopPointerDragging) {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const auto area = timeline();
        const double before = viewStart;
        viewStart = carta::selectionEdgeScroll(viewStart, viewLength(), fullLength(),
                                               area.getX() + labelWidth, area.getRight(),
                                               loopPointer.x, (now - loopScrollClock) / 1000.0);
        loopScrollClock = now;
        loopSnapAllowed = !juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
        if (viewStart < before || viewStart > before) {
            updateDraggedLoop();
        }
    }
    if (dragMode == kDragPlayhead && playheadPointerDragging) {
#if CARTA_STUDIO_COMPONENT_TEST
        if (scrollGeometryHook) {
            scrollGeometryHook();
        }
#endif
        const double now = juce::Time::getMillisecondCounterHiRes();
        const auto area = timeline();
        const double before = viewStart;
        viewStart = carta::selectionEdgeScroll(
            viewStart, viewLength(), fullLength(), area.getX() + labelWidth, area.getRight(),
            playheadPointer.x, (now - playheadScrollClock) / 1000.0);
        playheadScrollClock = now;
        playheadSnapAllowed = !juce::ModifierKeys::getCurrentModifiersRealtime().isShiftDown();
        if (viewStart < before || viewStart > before) {
            const juce::ScopedLock guard(lock);
            updateDraggedPlayhead();
        }
    }

    // --- Snapshot playback position and update follow mode / accessibility / readouts ---
    double displayPosition = 0;
    bool displayPlaying = false;
    bool displayLoopPlaying = false;
    {
        const juce::ScopedLock guard(lock);
        displayPosition = position;
        displayPlaying = playing;
        displayLoopPlaying = savedLoopPlaying();
    }
    if (displayPlaying && dragMode != kDragNone && std::abs(viewStart - beforeDragView) > 1e-12) {
        suspendFollowingForNavigation();
    }
    if (!displayPlaying && followSuspended) {
        followSuspended = false;
        menuItemsChanged();
    }
    if (followMode != 0 && !followSuspended && displayPlaying && dragMode == kDragNone) {
        viewStart = carta::followPlayheadViewport(viewStart, viewLength(), fullLength(),
                                                  displayPosition, followMode == 2);
    }
    syncTimelineScroll();

    const auto cursor = cursorSampleFromPosition(displayPosition);
    const auto accessibilityDescription =
        carta::describeTimeline(session, timeSelection, selectedTrack, selectedClip, selectedMarker,
                                cursor, displayPlaying, &sourceChecks,
                                !selectedTimelineTracks().empty()) +
        " " +
        carta::describeClipSelection(timelineClipSelection, editGeneration, selectedTrack,
                                     selectedClip) +
        " " + describeTimelineTracks() + " " + describeSavedLoop(displayLoopPlaying) + " " +
        describeEditLockInterruptions() + " " + describeWaveformDisplay() +
        (followSuspended ? " Playhead following paused for navigation." : "");
    if (getDescription() != accessibilityDescription) {
        setDescription(accessibilityDescription);
    }

    play.setButtonText(displayPlaying ? "Pause" : "Play");
    clock.setText(
        carta::formatTimelineTime(cursor, timelineTimeFormat) + " / " +
            carta::formatTimelineTime(carta::projectEndSample(session), timelineTimeFormat),
        juce::dontSendNotification);

    const auto formatReadout = [&](juce::int64 sample) {
        return timelineTimeFormat == carta::TimelineTimeFormat::automatic
                   ? juce::String(sample) + " smp"
                   : carta::formatTimelineTime(sample, timelineTimeFormat);
    };
    auto readout = juce::String(followSuspended ? "FOLLOW PAUSED   |   " : "") + "Playhead " +
                   formatReadout(cursor);
    const auto interruptions = editLockSilentBlocks.load(std::memory_order_relaxed);
    if (interruptions > 0) {
        readout += "   |   Edit-lock silent blocks " +
                   juce::String(static_cast<juce::int64>(interruptions));
    }
    if (timeSelection.hasRange()) {
        readout += "   |   Selection [" + formatReadout(timeSelection.start) + ", " +
                   formatReadout(timeSelection.end) + ")   |   Length " +
                   formatReadout(timeSelection.end - timeSelection.start);
    } else if (timeSelection.hasPoint() &&
               timeSelection.firstTrack < static_cast<int>(session.tracks.size())) {
        readout += "   |   Edit point " + formatReadout(timeSelection.start) + " on track " +
                   juce::String(timeSelection.firstTrack + 1);
    }
    timelineReadout.setText(readout, juce::dontSendNotification);

    // --- Peak meters ---
    const auto recentPeaks = outputPeakHold.takeRecent();
    const double peakNow = juce::Time::getMillisecondCounterHiRes();
    const double peakElapsed =
        lastPeakUiTime > 0 ? std::max(0.0, (peakNow - lastPeakUiTime) / 1000.0) : 0.0;
    lastPeakUiTime = peakNow;
    displayPeakLeft = carta::decayOutputPeak(displayPeakLeft, recentPeaks.left, peakElapsed);
    displayPeakRight = carta::decayOutputPeak(displayPeakRight, recentPeaks.right, peakElapsed);
    if (showOutputPeaks) {
        const auto peaks = outputPeakHold.snapshot();
        outputPeakLabel.setText(peaks.channels == 0
                                    ? utf8Str("Peak: —")
                                    : "L " + formatOutputPeakDb(displayPeakLeft) + "/" +
                                          formatOutputPeakDb(peaks.left) +
                                          (peaks.channels > 1
                                               ? "  R " + formatOutputPeakDb(displayPeakRight) +
                                                     "/" + formatOutputPeakDb(peaks.right)
                                               : juce::String()) +
                                          " dBFS",
                                juce::dontSendNotification);
    }

    // --- Refresh menu enablement bitmask only when relevant state changes ---
    const int state =
        (validSelection() ? 1 : 0) | (selectedMarker >= 0 ? 2 : 0) |
        (editHistory.canUndo() ? 4 : 0) | (editHistory.canRedo() ? 8 : 0) |
        (displayPlaying ? 16 : 0) | (masterChannels == 1 ? 32 : 0) | (spectrumView ? 64 : 0) |
        (timeSelection.hasRange() ? 128 : 0) | (!clipboard.empty() ? 256 : 0) |
        ((selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size())) ? 512
                                                                                         : 0) |
        ((selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size()) &&
          session.tracks[static_cast<size_t>(selectedTrack)].muted)
             ? 1024
             : 0) |
        ((selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size()) &&
          session.tracks[static_cast<size_t>(selectedTrack)].solo)
             ? 2048
             : 0) |
        ((selectedTrack > 0) ? 4096 : 0) |
        ((selectedTrack >= 0 && selectedTrack + 1 < static_cast<int>(session.tracks.size())) ? 8192
                                                                                             : 0) |
        (!session.tracks.empty() ? 16384 : 0) | (session.length() > 0 ? 32768 : 0) |
        (!session.markers.empty() ? 65536 : 0) | (activeExport ? 131072 : 0) |
        ((activeExport && activeExport->cancelled.load()) ? 262144 : 0) |
        ((selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size()) &&
          carta::maxTrackChannels(session.tracks[static_cast<size_t>(selectedTrack)]) > 0)
             ? 524288
             : 0) |
        ((selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size()) &&
          !session.tracks[static_cast<size_t>(selectedTrack)].routing.empty())
             ? 1048576
             : 0) |
        (!pendingImports.empty() ? 2097152 : 0) |
        (std::any_of(pendingImports.begin(), pendingImports.end(),
                     [](const auto& task) { return !task->cancelled.load(); })
             ? 4194304
             : 0) |
        (activeOpen ? 8388608 : 0) | ((activeOpen && activeOpen->cancelled.load()) ? 16777216 : 0) |
        (activeSave ? 33554432 : 0) |
        ((activeSave && activeSave->cancelled.load()) ? 67108864 : 0) |
        ((wavEncoding == carta::WavEncoding::pcm16) ? 134217728 : 0) |
        ((wavEncoding == carta::WavEncoding::float32) ? 268435456 : 0) |
        (wavDither ? 536870912 : 0) | (dirty ? 1073741824 : 0);

    const auto selectedView =
        selectedTrack >= 0 && selectedTrack < static_cast<int>(session.tracks.size())
            ? session.tracks[static_cast<size_t>(selectedTrack)].view
            : carta::TrackView::useDefault;
    const bool selectedCollapsed = selectedTrack >= 0 &&
                                   selectedTrack < static_cast<int>(session.tracks.size()) &&
                                   session.tracks[static_cast<size_t>(selectedTrack)].collapsed;
    const bool canRestoreSelection =
        selectionMemory.canRestore(static_cast<int>(session.tracks.size()), editGeneration);
    const int sourceCheckState =
        activeSourceCheck ? (activeSourceCheck->cancelled.load() ? 2 : 1) : 0;

    if (state != menuState || selectedTrack != menuTrack || selectedView != menuTrackView ||
        selectedCollapsed != menuTrackCollapsed || canRestoreSelection != menuCanRestoreSelection ||
        sourceCheckState != menuSourceCheckState) {
        menuState = state;
        menuTrack = selectedTrack;
        menuTrackView = selectedView;
        menuTrackCollapsed = selectedCollapsed;
        menuCanRestoreSelection = canRestoreSelection;
        menuSourceCheckState = sourceCheckState;
        menuItemsChanged();
    }

    // --- Background task progress banner ---
    const bool hasActivity =
        static_cast<bool>(activeExport) || static_cast<bool>(activeOpen) ||
        static_cast<bool>(activeSave) || static_cast<bool>(activeSourceCheck) ||
        static_cast<bool>(activeLevelAnalysis) || !pendingImports.empty() || !importIssues.empty();
    if (exportStatus.isVisible() != hasActivity) {
        exportStatus.setVisible(hasActivity);
        resized();
    }
    if (hasActivity) {
        juce::String message;
        if (!pendingImports.empty()) {
            const auto& task = pendingImports.front();
            message = task->cancelled.load()
                          ? utf8Str("Cancelling imports…")
                          : "Import " +
                                juce::String(juce::jlimit(
                                    0, 99, juce::roundToInt(task->progress.load() * 100))) +
                                "%";
            if (pendingImports.size() > 1) {
                message +=
                    " (" + juce::String(static_cast<int>(pendingImports.size())) + " queued)";
            }
        }
        if (activeOpen) {
            if (message.isNotEmpty()) {
                message += "  |  ";
            }
            message += activeOpen->cancelled.load()
                           ? "Cancelling project..."
                           : "Open " +
                                 juce::String(juce::jlimit(
                                     0, 99, juce::roundToInt(activeOpen->progress.load() * 100))) +
                                 "%";
        }
        if (activeSave) {
            if (message.isNotEmpty()) {
                message += "  |  ";
            }
            message += activeSave->cancelled.load()
                           ? juce::String("Cancelling save...")
                           : (activeSave->portable ? juce::String("Portable copy ")
                                                   : juce::String("Save ")) +
                                 juce::String(juce::jlimit(
                                     0, 99, juce::roundToInt(activeSave->progress.load() * 100))) +
                                 "%";
        }
        if (activeSourceCheck) {
            if (message.isNotEmpty()) {
                message += "  |  ";
            }
            message +=
                activeSourceCheck->cancelled.load()
                    ? "Cancelling source operation..."
                    : (activeSourceCheck->relinking ? "Relink " : "Sources ") +
                          juce::String(juce::jlimit(
                              0, 99, juce::roundToInt(activeSourceCheck->progress.load() * 100))) +
                          "%";
        }
        if (activeLevelAnalysis) {
            if (message.isNotEmpty()) {
                message += "  |  ";
            }
            message +=
                activeLevelAnalysis->cancelled.load()
                    ? "Cancelling audio analysis..."
                    : activeLevelAnalysis->progressLabel + " " +
                          juce::String(juce::jlimit(
                              0, 99,
                              juce::roundToInt(activeLevelAnalysis->progress.load() * 100))) +
                          "%";
        }
        if (activeExport) {
            if (message.isNotEmpty()) {
                message += utf8Str("  ·  ");
            }
            const auto phase = activeExport->phase.load();
            const auto percent =
                juce::jlimit(0, 99, juce::roundToInt(activeExport->progress.load() * 100));
            message += activeExport->cancelled.load() ? juce::String(utf8Str("Cancelling WAV…"))
                       : phase == 2
                           ? juce::String("Clipping decision needed")
                           : (activeExport->renderTracks ? (phase == 4   ? "Verifying render "
                                                            : phase == 3 ? "Reading render "
                                                                         : "Rendering ")
                              : phase == 0               ? "Check "
                              : activeExport->batch      ? "Tracks "
                                                         : "WAV ") +
                                 juce::String(percent) + "%";
        }
        if (!importIssues.empty()) {
            const auto count = static_cast<juce::int64>(importIssues.size()) + omittedImportIssues;
            const auto issueText =
                juce::String(count) + (count == 1 ? utf8Str(" import issue · File menu")
                                                  : utf8Str(" import issues · File menu"));
            message = message.isEmpty() ? issueText : issueText + "  |  " + message;
        }
        exportStatus.setText(message, juce::dontSendNotification);
    }

    const auto title = (projectFile == juce::File{} ? juce::String("Untitled")
                                                    : projectFile.getFileNameWithoutExtension()) +
                       (dirty ? utf8Str(" • ") : utf8Str(" — ")) + "Carta Studio";
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>();
        window && window->getName() != title) {
        window->setName(title);
    }

    const auto damage = planPlayheadRepaint(displayPosition);
    if (!damage.isEmpty()) {
        repaint(damage);
    }
}

juce::String Studio::audioOutputError() {
#if CARTA_STUDIO_COMPONENT_TEST
    if (testAudioOutputAvailable) {
        return {};
    }
#endif
    auto* output = deviceManager.getCurrentAudioDevice();
    if (!output || !output->isOpen() || !output->isPlaying()) {
        return "No active audio output. Open Transport > Audio output settings.";
    }
    const int channels = output->getActiveOutputChannels().countNumberOfSetBits();
    {
        const juce::ScopedLock guard(lock);
        if (!(deviceRate > 0)) {
            return "Audio output is not ready. Check Transport > Audio output settings.";
        }
        if (channels < 1) {
            return "No active output channels. Check Transport > Audio output settings.";
        }
        if (!carta::outputSupportsMaster(channels, masterChannels)) {
            return "Stereo requires two output channels. Choose a stereo pair in Transport > Audio "
                   "output settings.";
        }
    }
    return {};
}

bool Studio::requireAudioOutput() {
    const auto error = audioOutputError();
    if (error.isEmpty()) {
        return true;
    }
    status.setText(error, juce::dontSendNotification);
    return false;
}

void Studio::showAudioSettings() {
    if (!audioSettingsWindow) {
        audioSettingsWindow =
            std::make_unique<AudioSettingsWindow>(deviceManager, neutralLook, this);
        audioSettingsWindow->onClose = [safe = juce::Component::SafePointer<Studio>(this)] {
            if (safe != nullptr) {
                safe->saveAudioDevicePreferences(true);
            }
        };
    }
    audioSettingsWindow->setVisible(true);
    audioSettingsWindow->toFront(true);
}

void Studio::saveAudioDevicePreferences(bool report) {
    const auto state = deviceManager.createStateXml();
    if (!state) {
        return;
    }
    juce::String error;
    if (!carta::saveDeviceSettings(audioSettingsFile, *state, error) && report) {
        status.setText(error, juce::dontSendNotification);
    }
}

void Studio::rescanAudioDevices() {
    saveAudioDevicePreferences(true);
    audioSettingsWindow.reset();
    for (auto* type : deviceManager.getAvailableDeviceTypes()) {
        type->scanForDevices();
    }
    status.setText(
        "Audio devices rescanned. Open Transport > Audio output settings to choose an output.",
        juce::dontSendNotification);
}

juce::File Studio::newRecoveryPath() const {
    return recoveryDirectory.getChildFile("session-" + juce::Uuid().toString() + ".recovery.carta");
}
