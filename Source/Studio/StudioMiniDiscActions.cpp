/**
 * @file StudioMiniDiscActions.cpp
 * @brief MiniDisc workspace actions, UTOC inspection/bundle I/O, NetMD async
 *        worker orchestration, and disc-order audition preview for `Studio`.
 *
 * Coordinate spaces and units:
 * - Internal `session.mdDisc.tracks` indexing is 0-based; user-facing prompts
 *   use 1-based MD track numbers.
 * - UTOC binary bundles pack Sectors 0, 1, 2, and 4 sequentially (4 * 2352 =
 *   9408 bytes; Sector 3 is omitted).
 * - Disc preview playback converts 44.1 kHz project samples (`carta::sessionRate`)
 *   to seconds for `auditionRange`.
 *
 * Threading and ownership invariants:
 * - UI actions and `discWorkspace` updates execute on the JUCE message thread.
 * - `Studio::lock` protects `session`, `position`, `auditionActive`, `playing`,
 *   `cutPreviewSession`, and `discPreviewPlan`/`discPreviewRenderer`; heavy
 *   snapshot preparation happens outside `lock` before atomic swap under `lock`.
 * - USB NetMD operations copy a `carta::Session` snapshot under `lock`, then
 *   run on an 8 MB-stack `NetMdWorkerThread` (`launchNetMdWorker`) while
 *   `netMdMutex` guards pending status/alert payloads until `pollNetMdJobProgress`
 *   consumes them on the message thread.
 */

#include "Studio/Studio.h"

#include <set>

// ============================================================================
// Track Focus & UTOC Title / Flag Editing
// ============================================================================

bool Studio::focusDiscTrackNumber(juce::uint64 revision, juce::int64 trackNumber,
                                  juce::String& error) {
    if (revision != editGeneration || dragMode != kDragNone || activeOpen) {
        error = "Project changed; try again.";
        return false;
    }
    if (session.mdDisc.tracks.empty()) {
        error = "No MiniDisc tracks yet. Import audio onto Layer 1 or sync from Layer 1 first.";
        return false;
    }
    if (trackNumber < 1 || trackNumber > static_cast<juce::int64>(session.mdDisc.tracks.size())) {
        error = "Enter a track number from 1 to " +
                juce::String(static_cast<juce::int64>(session.mdDisc.tracks.size())) + ".";
        return false;
    }

    const auto id = juce::String::fromUTF8(
        session.mdDisc.tracks[static_cast<size_t>(trackNumber - 1)].id.c_str());
    setDiscWorkspaceVisible(true);
    if (!discWorkspace.selectView("tracks", error)) {
        return false;
    }
    discWorkspace.selectTrackById(id, true);
    refreshDiscWorkspace(true, id);
    status.setText("Focused MiniDisc track " + juce::String(trackNumber) +
                       "; playback is unchanged.",
                   juce::dontSendNotification);
    return true;
}

void Studio::promptDiscTrackNumber() {
    if (session.mdDisc.tracks.empty() || dragMode != kDragNone || activeOpen) {
        status.setText("Create or sync MiniDisc tracks first.", juce::dontSendNotification);
        return;
    }

    const auto revision = editGeneration;
    showTextPrompt("Go to MiniDisc track",
                   "Enter an MD track number. Playback and the playhead stay unchanged.", "track",
                   "MD track number", "1", "Go",
                   [safe = juce::Component::SafePointer<Studio>(this),
                    revision](bool accepted, const juce::String& value) {
                       if (safe == nullptr || !accepted) {
                           return;
                       }
                       juce::int64 trackNumber = 0;
                       juce::String error;
                       if (!parseInteger(value, trackNumber)) {
                           error = "Enter a whole track number.";
                       } else if (safe->focusDiscTrackNumber(revision, trackNumber, error)) {
                           return;
                       }
                       safe->status.setText(error, juce::dontSendNotification);
                   });
}

void Studio::editDiscFormsAndTimestamp() {
    if (dragMode != kDragNone || activeOpen) {
        status.setText("Finish the current operation first.", juce::dontSendNotification);
        return;
    }

    const auto revision = editGeneration;
    showTextFieldsPrompt(
        "MiniDisc title forms & timestamp",
        "Half-width ASCII (UTOC Sector 1), full-width Shift-JIS (UTOC Sector 4), and optional "
        "Sector 2 timestamp (YYYY-MM-DD HH:MM:SS or empty).",
        {{"half", "Half-width disc title",
          juce::String::fromUTF8(session.mdDisc.halfWidthTitle.c_str())},
         {"full", "Full-width disc title (Shift-JIS)",
          juce::String::fromUTF8(session.mdDisc.fullWidthTitle.c_str())},
         {"ts", "Disc timestamp (YYYY-MM-DD HH:MM:SS or empty)",
          juce::String::fromUTF8(session.mdDisc.discTimestamp.c_str())}},
        "Apply",
        [safe = juce::Component::SafePointer<Studio>(this),
         revision](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted || values.size() != 3) {
                return;
            }
            if (revision != safe->editGeneration) {
                safe->status.setText("Project changed; review MiniDisc workspace and try again.",
                                     juce::dontSendNotification);
                safe->refreshDiscWorkspace(true);
                return;
            }

            const auto half = values[0];
            const auto full = values[1];
            const auto ts = values[2].trim();
            bool anyOk = false;

            if (half.toStdString() != safe->session.mdDisc.halfWidthTitle ||
                full.toStdString() != safe->session.mdDisc.fullWidthTitle) {
                const auto res = safe->ipcCommand({"disc", "title-forms", half, full});
                if (!static_cast<bool>(res["ok"])) {
                    safe->status.setText(res["error"].toString(), juce::dontSendNotification);
                    return;
                }
                anyOk = true;
            }

            if (ts.toStdString() != safe->session.mdDisc.discTimestamp) {
                const auto res = safe->ipcCommand(
                    {"disc", "timestamp", ts.isEmpty() ? juce::String("clear") : ts});
                if (!static_cast<bool>(res["ok"])) {
                    safe->status.setText(res["error"].toString(), juce::dontSendNotification);
                    return;
                }
                anyOk = true;
            }

            if (anyOk) {
                safe->refreshDiscWorkspace(true);
                safe->status.setText("Updated MiniDisc title forms / timestamp.",
                                     juce::dontSendNotification);
            } else {
                safe->status.setText("Disc title forms and timestamp unchanged.",
                                     juce::dontSendNotification);
            }
        });
}

void Studio::editSelectedTrackTitleForms() {
    const int idx0 = discWorkspaceVisible ? discWorkspace.singleSelectedTrackIndex() : -1;
    if (idx0 < 0 || idx0 >= static_cast<int>(session.mdDisc.tracks.size())) {
        status.setText("Select one MiniDisc track first.", juce::dontSendNotification);
        return;
    }

    const auto revision = editGeneration;
    const auto& track = session.mdDisc.tracks[static_cast<size_t>(idx0)];
    showTextFieldsPrompt(
        "MD track title forms",
        "Half-width ASCII (UTOC Sector 1) and optional full-width Shift-JIS (UTOC Sector 4).",
        {{"half", "Half-width title", juce::String::fromUTF8(track.halfWidthTitle.c_str())},
         {"full", "Full-width title (Shift-JIS)",
          juce::String::fromUTF8(track.fullWidthTitle.c_str())}},
        "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), revision,
         idx0](bool accepted, const juce::StringArray& values) {
            if (safe != nullptr && accepted && values.size() == 2) {
                safe->discWorkspace.onCommand(
                    {"disc", "track", "title-forms", juce::String(idx0), values[0], values[1]},
                    revision);
            }
        });
}

void Studio::editSelectedTrackMdFlags() {
    const int idx0 = discWorkspaceVisible ? discWorkspace.singleSelectedTrackIndex() : -1;
    if (idx0 < 0 || idx0 >= static_cast<int>(session.mdDisc.tracks.size())) {
        status.setText("Select one MiniDisc track first.", juce::dontSendNotification);
        return;
    }

    const auto revision = editGeneration;
    const auto& track = session.mdDisc.tracks[static_cast<size_t>(idx0)];
    const bool onDiscTrack = carta::mdTrackUsesOnDiscAudio(session.mdDisc, track);
    const auto effCodec = carta::effectiveTrackCodec(session.mdDisc, track);
    const juce::String codecVal = juce::String(carta::mdCodecModeToString(
        track.codecOverride ? *track.codecOverride : effCodec));
    showTextFieldsPrompt(
        "Track MiniDisc UTOC flags",
        onDiscTrack
            ? "Per-track UTOC flags (encoding codec is locked for existing on-disc audio): "
              "write-protect (on/off), SCMS (on/off), emphasis (on/off), and Sector 2 "
              "timestamp (YYYY-MM-DD HH:MM:SS or empty)."
            : "Per-track UTOC flags: codec override (sp-stereo, sp-mono, lp2, lp4, or empty for "
              "disc default), write-protect (on/off), SCMS (on/off), emphasis (on/off), and "
              "Sector 2 timestamp (YYYY-MM-DD HH:MM:SS or empty).",
        {{"codec", onDiscTrack ? "Codec (locked on disc)" : "Codec override (or empty)",
          track.codecOverride || onDiscTrack ? codecVal : juce::String()},
         {"wp", "Write-protect (on/off)", track.writeProtected ? "on" : "off"},
         {"scms", "SCMS restricted (on/off)", track.scmsCopyProtected ? "on" : "off"},
         {"emph", "Emphasis (on/off)", track.emphasis ? "on" : "off"},
         {"ts", "Timestamp (YYYY-MM-DD HH:MM:SS or empty)",
          juce::String::fromUTF8(track.timestamp.c_str())}},
        "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), revision, idx0, onDiscTrack,
         codecVal](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted || values.size() != 5) {
                return;
            }
            const auto c = values[0].trim();
            const auto wp = values[1].trim();
            const auto scms = values[2].trim();
            const auto emph = values[3].trim();
            const auto ts = values[4].trim();
            const juce::String codecArg =
                (onDiscTrack && c == codecVal) ? juce::String("keep")
                : c.isEmpty()                  ? juce::String("clear")
                                               : c;
            safe->discWorkspace.onCommand({"disc", "track", "flags", juce::String(idx0), codecArg,
                                           wp.isEmpty() ? juce::String("keep") : wp,
                                           scms.isEmpty() ? juce::String("keep") : scms,
                                           emph.isEmpty() ? juce::String("keep") : emph,
                                           ts.isEmpty() ? juce::String("clear") : ts},
                                          revision);
        });
}

// ============================================================================
// UTOC Inspection, Bundle I/O & Disc Program WAV Export
// ============================================================================

void Studio::inspectCurrentUtoc() {
    carta::UtocDiscState utoc;
    std::string reason;
    if (!carta::compileMdDiscToUtoc(session.mdDisc, utoc, reason)) {
        status.setText(juce::String::fromUTF8(reason.c_str()), juce::dontSendNotification);
        return;
    }

    const auto report =
        carta::formatUtocInspectionReport(utoc, carta::mdMediumToString(session.mdDisc.medium));
    showGuardedStudioAlert(juce::MessageBoxOptions()
                               .withIconType(juce::MessageBoxIconType::InfoIcon)
                               .withTitle("MiniDisc UTOC Sector Inspection")
                               .withMessage(juce::String::fromUTF8(report.c_str()))
                               .withButton("OK")
                               .withAssociatedComponent(this));
}

void Studio::exportCurrentUtocDialog() {
    launchStudioChooser(
        "Export UTOC bundle (Sectors 0, 1, 2, 4)",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("disc.utoc"),
        "*.utoc;*.bin",
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<Studio>(this)](const StudioFileSelection& fc) {
            if (safe == nullptr) {
                return;
            }
            const auto file = fc.getResult();
            if (file == juce::File{}) {
                return;
            }
            const auto result = safe->ipcCommand({"utoc", "export", file.getFullPathName()});
            if (static_cast<bool>(result["ok"])) {
                const bool loadedActive = safe->session.mdDisc.loadedDisc.active;
                safe->status.setText(
                    loadedActive
                        ? ("Exported 9408-byte UTOC bundle to " + file.getFileName())
                        : ("Exported 9408-byte virtual UTOC bundle to " + file.getFileName() +
                           " (use Save Project for exact timeline edits)."),
                    juce::dontSendNotification);
            } else {
                safe->status.setText(result["error"].toString(), juce::dontSendNotification);
            }
        });
}

void Studio::importUtocDialog() {
    launchStudioChooser(
        "Import UTOC Sector binary (.utoc / .bin)",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.utoc;*.bin",
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<Studio>(this)](const StudioFileSelection& fc) {
            if (safe == nullptr) {
                return;
            }
            const auto file = fc.getResult();
            if (file == juce::File{}) {
                return;
            }
            const auto result = safe->ipcCommand({"utoc", "import", file.getFullPathName()});
            safe->status.setText(static_cast<bool>(result["ok"])
                                     ? "Imported UTOC sectors from " + file.getFileName()
                                     : result["error"].toString(),
                                 juce::dontSendNotification);
        });
}

void Studio::startDiscProgramWavExport() {
    exportWav(false, false, false, false, false, true);
}

// ============================================================================
// NetMD Device Discovery, Status JSON & Async Hardware Worker Jobs
// ============================================================================

std::string Studio::resolveActiveNetMdDeviceId(const juce::String& explicitId, std::string& error) {
    if (explicitId.isNotEmpty()) {
        return explicitId.toStdString();
    }

    const auto selected = discWorkspace.selectedRecordDeviceId();
    if (selected.isNotEmpty()) {
        return selected.toStdString();
    }

    std::vector<carta::NetMdDeviceEntry> devs;
    if (!carta::listNetMdDevices(devs, error)) {
        return {};
    }

    cachedNetMdDevices = std::move(devs);
    netMdScannedOnce = true;
    if (cachedNetMdDevices.empty()) {
        error = "No USB NetMD recorder detected. Connect a supported device and click Refresh.";
        return {};
    }
    return cachedNetMdDevices.front().id;
}

juce::var Studio::buildNetMdStatusJson() {
    auto* root = new juce::DynamicObject();
    const bool busy = netMdBusy.load(std::memory_order_acquire);
    const auto prog = carta::getNetMdProgress();

    std::lock_guard<std::mutex> lk(netMdMutex);
    root->setProperty("active", busy);
    root->setProperty("operation",
                      juce::String::fromUTF8((busy ? netMdActiveOp : netMdLastOp).c_str()));
    root->setProperty("device_id",
                      juce::String::fromUTF8((busy ? netMdActiveDevId : netMdLastDevId).c_str()));
    root->setProperty("phase", juce::String::fromUTF8(prog.phase.c_str()));
    root->setProperty("progress", prog.progress);
    root->setProperty("bytes_done", static_cast<juce::int64>(prog.bytesDone));
    root->setProperty("bytes_total", static_cast<juce::int64>(prog.bytesTotal));
    root->setProperty("ok", netMdLastOk);
    root->setProperty("error", juce::String::fromUTF8(netMdLastError.c_str()));
    root->setProperty("result", netMdLastResult);
    return juce::var(root);
}

void Studio::startNetMdProbeJob(const std::string& devId) {
    carta::Session snapshot;
    {
        const juce::ScopedLock guard(lock);
        copyWorkerSnapshot(snapshot);
    }

    launchNetMdWorker(
        "probe", devId, "Probing NetMD recorder...",
        [snapshot, devId](std::string& err, juce::var&, juce::String& uiStatus, juce::String&,
                          juce::String&,
                          std::optional<carta::NetMdPreflightReport>& afterPreflight) mutable {
            const auto report = carta::preflightNetMdRecording(snapshot, snapshot.mdDisc, devId);
            afterPreflight = report;
            uiStatus = juce::String::fromUTF8(report.statusText.c_str());
            if (!report.deviceConnected && !report.blocker.empty()) {
                err = report.blocker;
            }
            return report.deviceConnected;
        });
}

void Studio::applyCommittedDiscIdentity(carta::NetMdDeviceStatus& dev, int trackCount,
                                        const std::string& hwTitle, const std::string& fwTitle,
                                        bool hiMd, const carta::MdDisc* committedDisc) {
    dev.trackCount = trackCount;
    dev.discTitle = hwTitle;
    dev.fullWidthTitle = fwTitle;
    dev.hiMdFormatted = hiMd;
    if (committedDisc != nullptr) {
        dev.tracks.clear();
        dev.tracks.reserve(committedDisc->tracks.size());
        for (std::size_t i = 0; i < committedDisc->tracks.size(); ++i) {
            const auto& trk = committedDisc->tracks[i];
            carta::NetMdDeviceTrack row;
            row.index = static_cast<int>(i);
            row.title = trk.halfWidthTitle;
            row.fullWidthTitle = trk.fullWidthTitle;
            switch (carta::effectiveTrackCodec(*committedDisc, trk)) {
            case carta::MdCodecMode::spStereo:
                row.codec = "SP";
                break;
            case carta::MdCodecMode::spMono:
                row.codec = "Mono";
                break;
            case carta::MdCodecMode::lp2:
                row.codec = "LP2";
                break;
            case carta::MdCodecMode::lp4:
                row.codec = "LP4";
                break;
            }
            row.durationSeconds =
                static_cast<int>(std::llround(static_cast<double>(trk.totalSamples()) /
                                              static_cast<double>(carta::kMdSampleRate)));
            dev.tracks.push_back(std::move(row));
        }
    }
}

void Studio::startNetMdReadUtocJob(const std::string& devId, const juce::File& outFile,
                                   const juce::String& outPath, bool showPopup) {
    const auto medium = carta::mdMediumToString(session.mdDisc.medium);
    const auto prevPreflight =
        (cachedNetMdPreflight.has_value() && cachedNetMdPreflight->device.deviceId == devId)
            ? cachedNetMdPreflight
            : std::optional<carta::NetMdPreflightReport>{};

    launchNetMdWorker(
        "read-utoc", devId, "Reading UTOC sectors 0..4...",
        [devId, outFile, outPath, showPopup, medium,
         prevPreflight](std::string& err, juce::var& result, juce::String& uiStatus,
                        juce::String& alertTitle, juce::String& alertBody,
                        std::optional<carta::NetMdPreflightReport>& afterPreflight) mutable {
            std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> rawSectors{};
            carta::UtocDiscState parsed{};
            if (!carta::readNetMdDiscUtoc(devId, rawSectors, parsed, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showPopup) {
                    alertTitle = "Read Disc TOC Failed";
                    alertBody = uiStatus;
                }
                return false;
            }

            if (outFile != juce::File{}) {
                // Pack Sectors 0, 1, 2, and 4 into the 9408-byte UTOC bundle (Sector 3 omitted).
                std::vector<std::uint8_t> bundle(4 * carta::kUtocSectorBytes, 0);
                std::memcpy(bundle.data() + 0 * carta::kUtocSectorBytes, rawSectors[0].data(),
                            carta::kUtocSectorBytes);
                std::memcpy(bundle.data() + 1 * carta::kUtocSectorBytes, rawSectors[1].data(),
                            carta::kUtocSectorBytes);
                std::memcpy(bundle.data() + 2 * carta::kUtocSectorBytes, rawSectors[2].data(),
                            carta::kUtocSectorBytes);
                std::memcpy(bundle.data() + 3 * carta::kUtocSectorBytes, rawSectors[4].data(),
                            carta::kUtocSectorBytes);

                juce::TemporaryFile tmp(outFile);
                if (!tmp.getFile().replaceWithData(bundle.data(), bundle.size()) ||
                    !tmp.overwriteTargetFileWithTemporary()) {
                    err = "Failed writing UTOC bundle to " + outPath.toStdString();
                    uiStatus = juce::String::fromUTF8(err.c_str());
                    return false;
                }
            }

            carta::MdDisc decoded{};
            std::string decodeErr;
            carta::mdDiscFromUtoc(parsed, decoded, decodeErr);
            const auto report = carta::formatUtocInspectionReport(parsed, medium);
            const bool nonAudio = carta::utocHasNonAudioTrack(parsed);
            if (prevPreflight.has_value() && prevPreflight->deviceConnected) {
                afterPreflight = prevPreflight;
                applyCommittedDiscIdentity(afterPreflight->device, parsed.trackCount,
                                           decoded.halfWidthTitle, decoded.fullWidthTitle, nonAudio,
                                           &decoded);
            }

            auto* o = new juce::DynamicObject();
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            o->setProperty("track_count", parsed.trackCount);
            o->setProperty("group_count", static_cast<int>(decoded.groups.size()));
            o->setProperty("disc_title", juce::String::fromUTF8(decoded.halfWidthTitle.c_str()));
            o->setProperty("full_width_title",
                           juce::String::fromUTF8(decoded.fullWidthTitle.c_str()));
            o->setProperty("non_audio_disc", nonAudio);
            o->setProperty("report", juce::String::fromUTF8(report.c_str()));
            if (outPath.isNotEmpty()) {
                o->setProperty("output_file", outPath);
            }
            result = juce::var(o);
            uiStatus = "Read physical UTOC (" + juce::String(parsed.trackCount) + " track(s)" +
                       (nonAudio ? ", Hi-MD data flag present" : "") + ").";
            if (showPopup) {
                alertTitle = "Physical MiniDisc UTOC Inspection";
                alertBody = juce::String::fromUTF8(report.c_str());
            }
            return true;
        });
}

void Studio::startNetMdLoadDiscJob(const std::string& devId, bool showAlerts) {
    launchNetMdWorker(
        "load-disc", devId, "Reading MiniDisc UTOC...",
        [this, devId, showAlerts](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody,
            std::optional<carta::NetMdPreflightReport>& afterPreflight) mutable {
            carta::MdDisc loadedDisc{};
            carta::NetMdDeviceStatus devStatus{};
            if (!carta::loadNetMdDisc(devId, loadedDisc, &devStatus, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Load MiniDisc Failed";
                    alertBody = uiStatus;
                }
                return false;
            }

            carta::Session previewSession;
            carta::populateLoadedDiscSession(loadedDisc, previewSession);
            afterPreflight = carta::preflightNetMdRecordingWithStatus(
                previewSession, previewSession.mdDisc, devStatus);

            {
                std::lock_guard<std::mutex> lk(netMdMutex);
                netMdPendingLoadedDisc = loadedDisc;
                netMdPendingShowLoadAlerts = showAlerts;
            }

            auto* o = new juce::DynamicObject();
            o->setProperty("loaded", true);
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            o->setProperty("medium",
                           juce::String(carta::mdMediumToString(loadedDisc.medium)));
            o->setProperty("half_width_title",
                           juce::String::fromUTF8(loadedDisc.halfWidthTitle.c_str()));
            o->setProperty("full_width_title",
                           juce::String::fromUTF8(loadedDisc.fullWidthTitle.c_str()));
            o->setProperty("track_count", static_cast<int>(loadedDisc.tracks.size()));
            o->setProperty("group_count", static_cast<int>(loadedDisc.groups.size()));
            o->setProperty("region_count",
                           static_cast<int>(loadedDisc.loadedDisc.regions.size()));
            o->setProperty(
                "baseline_sector0_hash",
                juce::String::fromUTF8(loadedDisc.loadedDisc.baselineSector0Hash.c_str()));
            o->setProperty("physical_start_cluster",
                           static_cast<int>(loadedDisc.loadedDisc.physicalStartCluster));
            o->setProperty("physical_last_cluster",
                           static_cast<int>(loadedDisc.loadedDisc.physicalLastCluster));
            result = juce::var(o);

            const auto discName = !loadedDisc.halfWidthTitle.empty() ? loadedDisc.halfWidthTitle
                                                                     : loadedDisc.fullWidthTitle;
            uiStatus =
                "Loaded MiniDisc " +
                (discName.empty() ? juce::String::fromUTF8(devId.c_str())
                                  : ("'" + juce::String::fromUTF8(discName.c_str()) + "'")) +
                " (" + juce::String(static_cast<int>(loadedDisc.tracks.size())) + " track(s), " +
                juce::String(static_cast<int>(loadedDisc.loadedDisc.regions.size())) +
                " on-disc region(s)).";
            return true;
        });
}

bool Studio::applyLoadedMiniDisc(const carta::MdDisc& loaded, juce::String& error) {
    const bool applied = mutate(
        [&]() {
            carta::populateLoadedDiscSession(loaded, session);
            selectedTrack = session.tracks.empty() ? -1 : 0;
            selectedClip = -1;
            selectedMarker = -1;
            timeSelection = {};
            timelineClipSelection.clear();
            position = 0.0;
            firstTrack = 0;
            return true;
        },
        &error, "Load MiniDisc UTOC", false, false, true);
    if (!applied) {
        return false;
    }

    zoom = 1.0;
    viewStart = 0.0;
    setDiscWorkspaceVisible(true);
    refreshDiscWorkspace(true);
    menuItemsChanged();
    repaint();
    return true;
}

void Studio::startNetMdEraseJob(const std::string& devId, bool confirmHardware, bool showAlerts) {
    carta::Session snapshot;
    {
        const juce::ScopedLock guard(lock);
        copyWorkerSnapshot(snapshot);
    }

    launchNetMdWorker(
        "erase", devId, "Erasing MiniDisc...",
        [snapshot, devId, confirmHardware, showAlerts](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody,
            std::optional<carta::NetMdPreflightReport>& afterPreflight) mutable {
            carta::NetMdDeviceStatus afterStatus{};
            if (!carta::eraseNetMdDisc(devId, confirmHardware, afterStatus, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Erase MiniDisc Failed";
                    alertBody = uiStatus;
                }
                return false;
            }

            afterPreflight = !afterStatus.deviceId.empty()
                                 ? carta::preflightNetMdRecordingWithStatus(
                                       snapshot, snapshot.mdDisc, afterStatus)
                                 : carta::preflightNetMdRecording(snapshot, snapshot.mdDisc, devId);
            const auto& st = afterPreflight->deviceConnected ? afterPreflight->device : afterStatus;
            auto* o = new juce::DynamicObject();
            o->setProperty("erased", true);
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            o->setProperty("disc_present", st.discPresent);
            o->setProperty("writable", st.writable);
            o->setProperty("hi_md_formatted", st.hiMdFormatted);
            o->setProperty("track_count", st.trackCount);
            o->setProperty("left_seconds", st.leftSeconds);
            result = juce::var(o);
            uiStatus = "Erased MiniDisc in " + juce::String::fromUTF8(devId.c_str()) +
                       ". Disc is ready to record.";
            return true;
        });
}

void Studio::startNetMdWriteUtocJob(const carta::Session& snapshot, const std::string& devId,
                                    bool confirmHardware, bool showAlerts) {
    launchNetMdWorker(
        "write-utoc", devId, "Writing UTOC Sectors 0/1/2/4...",
        [this, snapCopy = snapshot, devId, confirmHardware, showAlerts](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody,
            std::optional<carta::NetMdPreflightReport>& afterPreflight) mutable {
            carta::UtocBudget budget{};
            std::string committedHash;
            std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> committedRaw{};
            if (!carta::writeNetMdUtoc(snapCopy.mdDisc, devId, confirmHardware, budget, err,
                                       &committedHash, &committedRaw)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Write MiniDisc UTOC Failed";
                    alertBody = uiStatus +
                                "\n\nUse Help -> Export Diagnostic Log to save details.";
                }
                return false;
            }

            if (snapCopy.mdDisc.loadedDisc.active) {
                snapCopy.mdDisc.loadedDisc.baselineSector0Hash = committedHash;
                snapCopy.mdDisc.loadedDisc.rawSector3.assign(committedRaw[3].begin(),
                                                             committedRaw[3].end());
            }
            {
                std::lock_guard<std::mutex> lk(netMdMutex);
                netMdPendingCommittedRawSectors = committedRaw;
                netMdPendingHasCommittedRaw = true;
                netMdPendingCommittedHash = committedHash;
                netMdPendingNeedsReload = false;
            }

            afterPreflight = carta::preflightNetMdRecording(snapCopy, snapCopy.mdDisc, devId);
            if (afterPreflight->deviceConnected) {
                applyCommittedDiscIdentity(afterPreflight->device,
                                           static_cast<int>(snapCopy.mdDisc.tracks.size()),
                                           snapCopy.mdDisc.halfWidthTitle,
                                           snapCopy.mdDisc.fullWidthTitle, false, &snapCopy.mdDisc);
            }

            auto* o = new juce::DynamicObject();
            o->setProperty("committed_utoc", true);
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            o->setProperty("used_clusters", budget.usedClusters);
            o->setProperty("total_clusters", budget.totalClusters);
            o->setProperty("used_tracks", budget.usedTracks);
            o->setProperty("used_fragments", budget.usedFragments);
            o->setProperty("shared_fragments", budget.sharedFragmentCount);
            o->setProperty("committed_sector0_hash",
                           juce::String::fromUTF8(committedHash.c_str()));
            result = juce::var(o);
            uiStatus = "Committed UTOC (" + juce::String(budget.usedTracks) + " tracks, " +
                       juce::String(budget.usedFragments) +
                       " fragments). Eject & re-insert disc for player display to refresh.";
            return true;
        });
}

void Studio::startNetMdRecordJob(carta::Session&& snapshotIn, const std::string& devId,
                                 bool confirmHardware, bool showAlerts) {
    launchNetMdWorker(
        "record", devId, "Preparing ATRAC spans...",
        [this, snapshot = std::move(snapshotIn), devId, confirmHardware, showAlerts](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody,
            std::optional<carta::NetMdPreflightReport>& afterPreflight) mutable {
            carta::NetMdRecordResult rec{};
            if (!carta::recordNetMdDisc(snapshot, snapshot.mdDisc, devId, confirmHardware, rec,
                                        err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "MiniDisc Recording Failed";
                    alertBody = uiStatus +
                                "\n\nUse Help -> Export Diagnostic Log to save details.";
                }
                return false;
            }

            if (rec.hasCommittedRawSectors) {
                if (snapshot.mdDisc.loadedDisc.active) {
                    snapshot.mdDisc.loadedDisc.baselineSector0Hash = rec.committedSector0Hash;
                    snapshot.mdDisc.loadedDisc.rawSector3.assign(
                        rec.committedRawSectors[3].begin(), rec.committedRawSectors[3].end());
                }
                std::lock_guard<std::mutex> lk(netMdMutex);
                netMdPendingCommittedRawSectors = rec.committedRawSectors;
                netMdPendingHasCommittedRaw = true;
                netMdPendingCommittedHash = rec.committedSector0Hash;
                netMdPendingNeedsReload = rec.needsReloadToFinishUpload;
            }

            afterPreflight = carta::preflightNetMdRecording(snapshot, snapshot.mdDisc, devId);
            if (afterPreflight->deviceConnected && !rec.needsReloadToFinishUpload) {
                applyCommittedDiscIdentity(afterPreflight->device,
                                           static_cast<int>(snapshot.mdDisc.tracks.size()),
                                           snapshot.mdDisc.halfWidthTitle,
                                           snapshot.mdDisc.fullWidthTitle, false, &snapshot.mdDisc);
            }

            auto* o = new juce::DynamicObject();
            o->setProperty("device_id", juce::String::fromUTF8(rec.deviceId.c_str()));
            o->setProperty("uploaded_physical_segments", rec.uploadedPhysicalSegments);
            o->setProperty("logical_tracks", rec.logicalTracks);
            o->setProperty("groups", rec.groups);
            o->setProperty("encoded_bytes", static_cast<juce::int64>(rec.encodedBytes));
            o->setProperty("committed_utoc", rec.committedUtoc);
            o->setProperty("preserved_on_disc_tracks", rec.preservedOnDiscTracks);
            o->setProperty("reclaimed_clusters", rec.reclaimedClusters);
            o->setProperty("needs_reload_to_finish_upload", rec.needsReloadToFinishUpload);
            o->setProperty("next_action_hint", juce::String::fromUTF8(rec.nextActionHint.c_str()));
            o->setProperty("committed_sector0_hash",
                           juce::String::fromUTF8(rec.committedSector0Hash.c_str()));
            o->setProperty("used_clusters", rec.budget.usedClusters);
            o->setProperty("total_clusters", rec.budget.totalClusters);
            o->setProperty("used_tracks", rec.budget.usedTracks);
            o->setProperty("used_fragments", rec.budget.usedFragments);
            o->setProperty("shared_fragments", rec.budget.sharedFragmentCount);
            result = juce::var(o);

            if (rec.needsReloadToFinishUpload) {
                alertTitle = "Reload MiniDisc to finish recording";
                alertBody = juce::String::fromUTF8(rec.nextActionHint.c_str());
                uiStatus = alertBody;
            } else if (snapshot.mdDisc.loadedDisc.active) {
                uiStatus = "Synced MiniDisc: preserved " + juce::String(rec.preservedOnDiscTracks) +
                           " on-disc track(s), uploaded " +
                           juce::String(rec.uploadedPhysicalSegments) + " new span(s), reclaimed " +
                           juce::String(rec.reclaimedClusters) + " cluster(s) -> " +
                           juce::String(rec.logicalTracks) +
                           " track(s). Eject & re-insert disc for player display to refresh.";
            } else {
                uiStatus = "Recorded " + juce::String(rec.uploadedPhysicalSegments) +
                           " physical segment(s) -> " + juce::String(rec.logicalTracks) +
                           " UTOC track(s). Eject & re-insert disc for player display to refresh.";
            }
            return true;
        });
}

// ============================================================================
// NetMD Polling, Record Workspace Refresh & Hardware UI Triggers
// ============================================================================

void Studio::pollNetMdJobProgress() {
    const bool busy = netMdBusy.load(std::memory_order_acquire);
    if (netMdDonePending.exchange(false, std::memory_order_acq_rel)) {
        if (!busy) {
            netMdWorker.reset();
        }
        juce::String uiStatus;
        juce::String alertTitle;
        juce::String alertBody;
        bool alertIsWarning = false;
        std::optional<carta::NetMdPreflightReport> afterPreflight;
        std::optional<carta::MdDisc> pendingLoadedDisc;
        std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> committedRaw{};
        bool hasCommittedRaw = false;
        std::string committedHash;
        {
            std::lock_guard<std::mutex> lk(netMdMutex);
            uiStatus = netMdPendingUiStatus;
            alertTitle = netMdPendingAlertTitle;
            alertBody = netMdPendingAlertBody;
            alertIsWarning = !netMdLastOk;
            afterPreflight = netMdPendingPreflight;
            pendingLoadedDisc = std::move(netMdPendingLoadedDisc);
            netMdPendingLoadedDisc.reset();
            hasCommittedRaw = netMdPendingHasCommittedRaw;
            committedRaw = netMdPendingCommittedRawSectors;
            committedHash = std::move(netMdPendingCommittedHash);
            netMdPendingHasCommittedRaw = false;
            netMdPendingCommittedHash.clear();
            netMdPendingNeedsReload = false;
        }
        if (pendingLoadedDisc.has_value()) {
            juce::String applyErr;
            if (applyLoadedMiniDisc(*pendingLoadedDisc, applyErr)) {
                if (afterPreflight.has_value()) {
                    cachedNetMdPreflight = afterPreflight;
                }
            } else {
                uiStatus = applyErr;
            }
        } else {
            if (hasCommittedRaw) {
                bool refreshedLoaded = false;
                {
                    const juce::ScopedLock guard(lock);
                    if (session.mdDisc.loadedDisc.active) {
                        session.mdDisc.loadedDisc.baselineSector0Hash = committedHash;
                        session.mdDisc.loadedDisc.rawSector3.assign(committedRaw[3].begin(),
                                                                    committedRaw[3].end());
                        carta::UtocDiscState parsedCommitted{};
                        std::string parseErr;
                        if (carta::parseUtocSectors(
                                committedRaw[0].data(), committedRaw[1].data(),
                                committedRaw[2].data(), committedRaw[4].data(), parsedCommitted,
                                parseErr) &&
                            parsedCommitted.hasSector0) {
                            session.mdDisc.loadedDisc.initialFreeRanges.clear();
                            std::uint8_t slot = parsedCommitted.trackMap[0];
                            std::set<std::uint8_t> seen;
                            while (slot != 0 && seen.insert(slot).second && seen.size() <= 255) {
                                const auto& f = parsedCommitted.fragments[slot];
                                if (f.isValid() &&
                                    f.end.toLinearGroup() >= f.start.toLinearGroup()) {
                                    session.mdDisc.loadedDisc.initialFreeRanges.push_back(
                                        {{f.start.cluster, f.start.sector, f.start.group},
                                         {f.end.cluster, f.end.sector, f.end.group}});
                                }
                                slot = f.link;
                            }
                        }
                        refreshedLoaded = true;
                    }
                }
                if (refreshedLoaded) {
                    refreshDiscWorkspace(false);
                }
            }
            if (afterPreflight.has_value()) {
                cachedNetMdPreflight = afterPreflight;
            }
        }
        if (uiStatus.isNotEmpty()) {
            status.setText(uiStatus, juce::dontSendNotification);
        }
        refreshRecordWorkspace(false);
        if (alertBody.isNotEmpty() && !headlessMode) {
            showGuardedStudioAlert(
                juce::MessageBoxOptions()
                    .withIconType(alertIsWarning ? juce::MessageBoxIconType::WarningIcon
                                                 : juce::MessageBoxIconType::InfoIcon)
                    .withTitle(alertTitle)
                    .withMessage(alertBody)
                    .withButton("OK")
                    .withAssociatedComponent(this));
        }
        return;
    }

    if (busy) {
        refreshRecordWorkspace(false);
    }
}

void Studio::refreshRecordWorkspace(bool scanDevices) {
    const bool busy = netMdBusy.load(std::memory_order_acquire);
    if (scanDevices && !busy) {
        std::vector<carta::NetMdDeviceEntry> devs;
        std::string scanErr;
        if (carta::listNetMdDevices(devs, scanErr)) {
            cachedNetMdDevices = std::move(devs);
            netMdScannedOnce = true;
        } else {
            cachedNetMdDevices.clear();
            netMdScannedOnce = true;
        }

        if (cachedNetMdPreflight.has_value()) {
            const bool stillPresent = std::any_of(
                cachedNetMdDevices.begin(), cachedNetMdDevices.end(),
                [&](const auto& d) { return d.id == cachedNetMdPreflight->device.deviceId; });
            if (!stillPresent) {
                cachedNetMdPreflight.reset();
            }
        }
    }

    std::string preferredId = discWorkspace.selectedRecordDeviceId().toStdString();
    if (preferredId.empty() && !cachedNetMdDevices.empty()) {
        preferredId = cachedNetMdDevices.front().id;
    }
    if (scanDevices && !preferredId.empty() && !busy) {
        startNetMdProbeJob(preferredId);
        return;
    }

    carta::MdPhysicalPlan plan;
    std::string planErr;
    const bool planOk = carta::planMdPhysicalAudio(session.mdDisc, plan, planErr);
    carta::UtocDiscState utoc;
    std::string reason;
    const bool compiled = carta::compileMdDiscToUtoc(session.mdDisc, utoc, reason);
    const std::string medium = carta::mdMediumToString(session.mdDisc.medium);
    const int physTotal = session.mdDisc.loadedDisc.active
                              ? session.mdDisc.loadedDisc.physicalTotalClusters()
                              : 0;
    const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
    const auto budget =
        compiled ? carta::computeUtocBudget(utoc, medium, physTotal) : carta::UtocBudget{};
    const juce::String utocSummary =
        compiled
            ? (juce::String(budget.usedTracks) + "/" + juce::String(budget.maxTracks) + " trk" +
               dot + juce::String(budget.usedFragments) + "/" + juce::String(budget.maxFragments) +
               " frag" + dot + juce::String(budget.usedHalfWidthCells) + " HW" + dot +
               juce::String(budget.usedFullWidthCells) + " FW" + (budget.fits() ? "" : " (OVER)"))
            : ("UTOC error: " + juce::String::fromUTF8(reason.c_str()));

    MdWorkspace::RecordUiSnapshot snap;
    snap.devices = cachedNetMdDevices;
    snap.activeDeviceId = preferredId;
    snap.utocSummaryText = utocSummary;
    snap.busy = busy;
    snap.progress = carta::getNetMdProgress();
    snap.badgeText = "No USB device";
    snap.statusText = compiled ? "Connect a Sony NetMD / Hi-MD recorder via USB and click Refresh."
                               : juce::String::fromUTF8(reason.c_str());

    if (cachedNetMdPreflight.has_value() && cachedNetMdPreflight->device.deviceId == preferredId) {
        const auto& pre = *cachedNetMdPreflight;
        snap.hasDeviceStatus = pre.deviceConnected;
        snap.deviceStatus = pre.device;
        if (pre.deviceConnected) {
            const auto& st = pre.device;
            const juce::String fw =
                st.versionCode.empty() ? "?" : juce::String::fromUTF8(st.versionCode.c_str());
            const juce::String spLabel =
                !st.supportsSpUpload
                    ? "No"
                    : (fw.startsWithChar('H') ? "Hi-MD"
                                              : (fw.startsWithChar('R') ? "Type-R" : "Type-S"));
            snap.badgeText = "FW " + fw + dot +
                             "ForceTOC: " + (st.supportsForceTocEdit ? "OK" : "NO") + dot +
                             "SPUpload: " + spLabel;
            const bool spNeeded =
                std::any_of(plan.spans.begin(), plan.spans.end(), [](const auto& s) {
                    return s.codec == carta::MdCodecMode::spStereo ||
                           s.codec == carta::MdCodecMode::spMono;
                });
            const bool loadedActive = session.mdDisc.loadedDisc.active;
            int preservedTracks = 0;
            if (loadedActive) {
                for (const auto& tr : session.mdDisc.tracks) {
                    if (carta::mdTrackUsesOnDiscAudio(session.mdDisc, tr)) {
                        ++preservedTracks;
                    }
                }
            }
            const bool insufficientAfterReclaim =
                loadedActive && !plan.spans.empty() &&
                plan.freeClustersAfterReclaim < plan.newUploadClusters;

            snap.canLoadDisc = st.discPresent && !st.hiMdFormatted && st.supportsForceTocEdit;
            snap.canReadToc = st.discPresent && st.supportsForceTocEdit;
            snap.canErase = st.discPresent && !st.writeProtected;
            const bool baseWritable = compiled && budget.fits() && st.discPresent && st.writable &&
                                      !st.writeProtected && !st.hiMdFormatted &&
                                      st.supportsForceTocEdit;
            snap.canWriteUtoc = baseWritable && (!loadedActive || plan.spans.empty());
            snap.canRecord = baseWritable && planOk &&
                             (!session.mdDisc.tracks.empty() || loadedActive) &&
                             (loadedActive || st.trackCount == 0) &&
                             !insufficientAfterReclaim && (!spNeeded || st.supportsSpUpload) &&
                             pendingImports.empty();

            if (!st.discPresent) {
                snap.statusText = "Insert a MiniDisc into '" +
                                  juce::String::fromUTF8(st.deviceName.c_str()) +
                                  "' and click Refresh.";
            } else if (st.hiMdFormatted) {
                snap.statusText =
                    "Disc is Hi-MD formatted — click Erase Disc... to reformat for standard NetMD "
                    "recording.";
            } else if (st.writeProtected) {
                snap.statusText = "Disc is write-protected. Unlock its tab and click Refresh.";
            } else if (!compiled) {
                snap.statusText = juce::String::fromUTF8(reason.c_str());
            } else if (!budget.fits()) {
                snap.statusText = "Target project exceeds UTOC capacity (" +
                                  juce::String::fromUTF8(medium.c_str()) + ").";
            } else if (!loadedActive && st.trackCount > 0) {
                snap.statusText = "Disc is not empty (" + juce::String(st.trackCount) +
                                  " track(s)) — click Load Disc to edit non-destructively, or "
                                  "Erase Disc... first.";
            } else if (insufficientAfterReclaim) {
                snap.statusText = "Not enough free clusters after reclaiming deleted tracks (" +
                                  juce::String(plan.freeClustersAfterReclaim) + " free, " +
                                  juce::String(plan.newUploadClusters) + " needed).";
            } else if (session.mdDisc.tracks.empty() && !loadedActive) {
                snap.statusText = "Device ready (" + juce::String::fromUTF8(st.deviceName.c_str()) +
                                  "). Add tracks or click Load Disc.";
            } else if (spNeeded && !st.supportsSpUpload) {
                snap.statusText =
                    "SP upload requires a Type-S, Type-R, or supported Hi-MD recorder (switch "
                    "tracks to LP2/LP4 or connect compatible hardware).";
            } else if (loadedActive) {
                const bool twoStep =
                    !plan.spans.empty() && plan.initialFreeClusters < plan.newUploadClusters;
                snap.statusText = "Ready (preserve " + juce::String(preservedTracks) +
                                  " on-disc, upload " +
                                  juce::String(static_cast<int>(plan.spans.size())) +
                                  " new span(s), reclaim " + juce::String(plan.reclaimedClusters) +
                                  " cl" + (twoStep ? "; 2-step reclaim + reload" : "") + ").";
            } else {
                snap.statusText =
                    "Ready to record " + juce::String(static_cast<int>(plan.spans.size())) +
                    " physical span(s) -> " +
                    juce::String(static_cast<int>(session.mdDisc.tracks.size())) + " track(s).";
            }
        } else {
            snap.statusText = juce::String::fromUTF8(pre.blocker.empty() ? pre.statusText.c_str()
                                                                         : pre.blocker.c_str());
        }
    } else if (!cachedNetMdDevices.empty()) {
        snap.badgeText = "Device found (not probed)";
        snap.statusText = "Click Refresh to probe USB recorder & inserted MiniDisc.";
    }

    discWorkspace.updateRecordSummary(snap);
}

void Studio::triggerRecordProbeFromUi() {
    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty() || netMdBusy.load()) {
        refreshRecordWorkspace(false);
        return;
    }
    startNetMdProbeJob(selectedId.toStdString());
}

void Studio::triggerReadDiscTocFromUi() {
    if (netMdBusy.load()) {
        status.setText("Wait for the current NetMD operation to finish.",
                       juce::dontSendNotification);
        return;
    }

    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty()) {
        status.setText("Select or refresh a NetMD device first.", juce::dontSendNotification);
        return;
    }
    startNetMdReadUtocJob(selectedId.toStdString(), juce::File(), {}, true);
}

void Studio::triggerLoadDiscFromUi() {
    if (netMdBusy.load()) {
        status.setText("Wait for the current NetMD operation to finish.",
                       juce::dontSendNotification);
        return;
    }

    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty()) {
        status.setText("Select or refresh a NetMD device first.", juce::dontSendNotification);
        return;
    }

    const std::string devId = selectedId.toStdString();
    const bool hasExistingSession =
        dirty || !session.tracks.empty() || !carta::isDefaultMdDisc(session.mdDisc);
    if (!hasExistingSession) {
        startNetMdLoadDiscJob(devId, true);
        return;
    }

    showGuardedStudioAlert(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Load MiniDisc into editor")
            .withMessage("Load MiniDisc UTOC from '" + selectedId +
                         "' into the editor? This replaces the current timeline (Undo restores "
                         "it).")
            .withButton("Load Disc")
            .withButton("Cancel")
            .withAssociatedComponent(this),
        [safe = juce::Component::SafePointer<Studio>(this), devId](int answer) {
            if (safe == nullptr || answer != 0 || safe->netMdBusy.load()) {
                return;
            }
            safe->startNetMdLoadDiscJob(devId, true);
        });
}

void Studio::triggerEraseDiscFromUi() {
    if (netMdBusy.load()) {
        status.setText("Wait for the current NetMD operation to finish.",
                       juce::dontSendNotification);
        return;
    }

    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty()) {
        status.setText("Select or refresh a NetMD device first.", juce::dontSendNotification);
        return;
    }

    const std::string devId = selectedId.toStdString();
    showGuardedStudioAlert(juce::MessageBoxOptions()
                               .withIconType(juce::MessageBoxIconType::WarningIcon)
                               .withTitle("Erase MiniDisc on hardware")
                               .withMessage("Erase all recorded tracks on the disc in '" +
                                            selectedId +
                                            "' (and reformat Hi-MD discs back to standard MD)?")
                               .withButton("Erase Disc")
                               .withButton("Cancel")
                               .withAssociatedComponent(this),
                           [safe = juce::Component::SafePointer<Studio>(this), devId](int answer) {
                               if (safe == nullptr || answer != 0 || safe->netMdBusy.load()) {
                                   return;
                               }
                               safe->startNetMdEraseJob(devId, true);
                           });
}

void Studio::triggerRecordWriteUtocFromUi() {
    if (netMdBusy.load()) {
        status.setText("Wait for the current NetMD operation to finish.",
                       juce::dontSendNotification);
        return;
    }

    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty()) {
        status.setText("Select or refresh a NetMD device first.", juce::dontSendNotification);
        return;
    }

    const std::string devId = selectedId.toStdString();
    showGuardedStudioAlert(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle("Write MiniDisc UTOC on hardware")
            .withMessage(
                "Write compiled UTOC Sectors 0, 1, 2, and 4 directly to MiniDisc device '" +
                selectedId +
                "' via ForceTOCEdit? This modifies the disc table of contents immediately.")
            .withButton("Write UTOC")
            .withButton("Cancel")
            .withAssociatedComponent(this),
        [safe = juce::Component::SafePointer<Studio>(this), devId](int answer) {
            if (safe == nullptr || answer != 0 || safe->netMdBusy.load()) {
                return;
            }
            carta::Session snapshot;
            {
                const juce::ScopedLock guard(safe->lock);
                safe->copyWorkerSnapshot(snapshot);
            }
            safe->startNetMdWriteUtocJob(snapshot, devId, true);
        });
}

void Studio::triggerRecordStartFromUi() {
    if (netMdBusy.load()) {
        status.setText("Wait for the current NetMD operation to finish.",
                       juce::dontSendNotification);
        return;
    }
    if (!pendingImports.empty()) {
        status.setText("Wait for imports to finish before recording to MiniDisc.",
                       juce::dontSendNotification);
        return;
    }

    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty()) {
        status.setText("Select or refresh a NetMD device first.", juce::dontSendNotification);
        return;
    }

    const std::string devId = selectedId.toStdString();
    bool loadedActive = false;
    carta::MdPhysicalPlan plan{};
    int preservedTracks = 0;
    int totalTracks = 0;
    {
        const juce::ScopedLock guard(lock);
        loadedActive = session.mdDisc.loadedDisc.active;
        totalTracks = static_cast<int>(session.mdDisc.tracks.size());
        std::string planErr;
        carta::planMdPhysicalAudio(session.mdDisc, plan, planErr);
        if (loadedActive) {
            for (const auto& tr : session.mdDisc.tracks) {
                if (carta::mdTrackUsesOnDiscAudio(session.mdDisc, tr)) {
                    ++preservedTracks;
                }
            }
        }
    }

    juce::String promptMsg;
    if (loadedActive) {
        if (plan.spans.empty()) {
            promptMsg = "Update UTOC on '" + selectedId + "' non-destructively (preserving " +
                        juce::String(preservedTracks) + " on-disc track(s) and reclaiming " +
                        juce::String(plan.reclaimedClusters) + " cluster(s)) without erasing?";
        } else if (plan.initialFreeClusters < plan.newUploadClusters) {
            promptMsg = "Step 1 of 2 on '" + selectedId + "': Reclaim " +
                        juce::String(plan.reclaimedClusters) +
                        " cluster(s) into P-FRA first without erasing existing audio. After "
                        "confirming, eject and reinsert the disc, then click Record MiniDisc... "
                        "again to upload " +
                        juce::String(static_cast<int>(plan.spans.size())) + " new span(s).";
        } else {
            promptMsg = "Preserve " + juce::String(preservedTracks) +
                        " existing on-disc track(s) on '" + selectedId + "', upload " +
                        juce::String(static_cast<int>(plan.spans.size())) +
                        " new ATRAC span(s), and commit combined UTOC for " +
                        juce::String(totalTracks) + " total track(s) (no erase)?";
        }
    } else {
        promptMsg = "Upload " + juce::String(static_cast<int>(plan.spans.size())) +
                    " ATRAC span(s) to the blank disc in '" + selectedId +
                    "' and commit full UTOC Sectors 0, 1, 2, and 4?";
    }

    showGuardedStudioAlert(juce::MessageBoxOptions()
                               .withIconType(juce::MessageBoxIconType::WarningIcon)
                               .withTitle("Record MiniDisc")
                               .withMessage(promptMsg)
                               .withButton("Record")
                               .withButton("Cancel")
                               .withAssociatedComponent(this),
                           [safe = juce::Component::SafePointer<Studio>(this), devId](int answer) {
                               if (safe == nullptr || answer != 0 || safe->netMdBusy.load() ||
                                   !safe->pendingImports.empty()) {
                                   return;
                               }
                               carta::Session snapshot;
                               {
                                   const juce::ScopedLock guard(safe->lock);
                                   safe->copyWorkerSnapshot(snapshot);
                               }
                               safe->startNetMdRecordJob(std::move(snapshot), devId, true);
                           });
}

// ============================================================================
// Real-Time Disc-Order Preview
// ============================================================================

bool Studio::startDiscPreview(juce::uint64 revision, const juce::String& trackId, bool throughEnd) {
    if (!requireAudioOutput()) {
        return false;
    }
    if (savePromptActive || dragMode != kDragNone || activeOpen ||
        juce::Component::getCurrentlyModalComponent() != nullptr) {
        status.setText("Finish the current drag, dialog or project opening first.",
                       juce::dontSendNotification);
        return false;
    }

    carta::Session candidate;
    carta::DiscPreviewPlan plan;
    carta::DiscPreviewRenderer renderer;
    juce::String error;
    double rate = 0;
    int channels = 0;
    {
        const juce::ScopedLock guard(lock);
        if (revision != editGeneration) {
            status.setText("Project changed; preview the disc again.", juce::dontSendNotification);
            return false;
        }
        rate = deviceRate;
        channels = masterChannels;
        copyWorkerSnapshot(candidate);
    }

    if (!carta::prepareDiscPreview(candidate, plan, error, trackId, throughEnd) ||
        !carta::prepareDiscPreviewRenderer(candidate, rate, channels, renderer, error)) {
        status.setText(error, juce::dontSendNotification);
        return false;
    }

    {
        const juce::ScopedLock guard(lock);
        if (revision != editGeneration || dragMode != kDragNone ||
            !std::equal_to<double>{}(rate, deviceRate) || channels != masterChannels) {
            status.setText("Project or audio output changed; preview the disc again.",
                           juce::dontSendNotification);
            return false;
        }
        stopSpecialAudition();
        std::swap(cutPreviewSession, candidate);
        std::swap(discPreviewPlan, plan);
        std::swap(discPreviewRenderer, renderer);
        setAuditionRange(
            {static_cast<double>(discPreviewPlan.programStart) / carta::sessionRate,
             static_cast<double>(discPreviewPlan.programStart + discPreviewPlan.samples) /
                 carta::sessionRate,
             false});
        cutPreviewOriginalPosition = position;
        position = auditionRange.start;
        discPreviewFrames = 0;
        discPreviewActive = true;
        cutPreviewActive = true;
        groupAuditionActive = false;
        auditionActive = true;
        playing = true;
    }

    status.setText(trackId.isEmpty() ? "Previewing disc order; playhead shows program time."
                   : throughEnd
                       ? "Previewing from the selected disc track; playhead shows program time."
                       : "Previewing selected disc track; playhead shows track time.",
                   juce::dontSendNotification);
    repaint();
    return true;
}
