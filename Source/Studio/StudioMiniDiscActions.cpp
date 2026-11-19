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
    const juce::String codecVal =
        track.codecOverride ? juce::String(carta::mdCodecModeToString(*track.codecOverride))
                            : juce::String();
    showTextFieldsPrompt(
        "Track MiniDisc UTOC flags",
        "Per-track UTOC flags: codec override (sp-stereo, sp-mono, lp2, lp4, or empty for disc "
        "default), write-protect (on/off), SCMS (on/off), emphasis (on/off), and Sector 2 "
        "timestamp (YYYY-MM-DD HH:MM:SS or empty).",
        {{"codec", "Codec override (or empty)", codecVal},
         {"wp", "Write-protect (on/off)", track.writeProtected ? "on" : "off"},
         {"scms", "SCMS restricted (on/off)", track.scmsCopyProtected ? "on" : "off"},
         {"emph", "Emphasis (on/off)", track.emphasis ? "on" : "off"},
         {"ts", "Timestamp (YYYY-MM-DD HH:MM:SS or empty)",
          juce::String::fromUTF8(track.timestamp.c_str())}},
        "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), revision,
         idx0](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted || values.size() != 5) {
                return;
            }
            const auto c = values[0].trim();
            const auto wp = values[1].trim();
            const auto scms = values[2].trim();
            const auto emph = values[3].trim();
            const auto ts = values[4].trim();
            safe->discWorkspace.onCommand({"disc", "track", "flags", juce::String(idx0),
                                           c.isEmpty() ? juce::String("clear") : c,
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
            safe->status.setText(static_cast<bool>(result["ok"])
                                     ? "Exported 9408-byte UTOC bundle to " + file.getFileName()
                                     : result["error"].toString(),
                                 juce::dontSendNotification);
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

void Studio::startNetMdEraseJob(const std::string& devId, bool confirmHardware) {
    carta::Session snapshot;
    {
        const juce::ScopedLock guard(lock);
        copyWorkerSnapshot(snapshot);
    }

    launchNetMdWorker(
        "erase", devId, "Erasing MiniDisc...",
        [snapshot, devId, confirmHardware](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String&,
            juce::String&, std::optional<carta::NetMdPreflightReport>& afterPreflight) mutable {
            carta::NetMdDeviceStatus afterStatus{};
            if (!carta::eraseNetMdDisc(devId, confirmHardware, afterStatus, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                return false;
            }

            afterPreflight = carta::preflightNetMdRecording(snapshot, snapshot.mdDisc, devId);
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
                                    bool confirmHardware) {
    launchNetMdWorker(
        "write-utoc", devId, "Writing UTOC Sectors 0/1/2/4...",
        [snapshot, devId, confirmHardware](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String&,
            juce::String&, std::optional<carta::NetMdPreflightReport>& afterPreflight) mutable {
            carta::UtocBudget budget{};
            if (!carta::writeNetMdUtoc(snapshot.mdDisc, devId, confirmHardware, budget, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                return false;
            }

            afterPreflight = carta::preflightNetMdRecording(snapshot, snapshot.mdDisc, devId);
            if (afterPreflight->deviceConnected) {
                applyCommittedDiscIdentity(afterPreflight->device,
                                           static_cast<int>(snapshot.mdDisc.tracks.size()),
                                           snapshot.mdDisc.halfWidthTitle,
                                           snapshot.mdDisc.fullWidthTitle, false, &snapshot.mdDisc);
            }

            auto* o = new juce::DynamicObject();
            o->setProperty("committed_utoc", true);
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            o->setProperty("used_clusters", budget.usedClusters);
            o->setProperty("total_clusters", budget.totalClusters);
            o->setProperty("used_tracks", budget.usedTracks);
            o->setProperty("used_fragments", budget.usedFragments);
            o->setProperty("shared_fragments", budget.sharedFragmentCount);
            result = juce::var(o);
            uiStatus = "Committed UTOC Sectors 0/1/2/4 (" + juce::String(budget.usedTracks) +
                       " tracks, " + juce::String(budget.usedFragments) + " fragments).";
            return true;
        });
}

void Studio::startNetMdRecordJob(carta::Session&& snapshotIn, const std::string& devId,
                                 bool confirmHardware) {
    launchNetMdWorker(
        "record", devId, "Preparing ATRAC spans...",
        [snapshot = std::move(snapshotIn), devId, confirmHardware](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String&,
            juce::String&, std::optional<carta::NetMdPreflightReport>& afterPreflight) mutable {
            carta::NetMdRecordResult rec{};
            if (!carta::recordNetMdDisc(snapshot, snapshot.mdDisc, devId, confirmHardware, rec,
                                        err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                return false;
            }

            afterPreflight = carta::preflightNetMdRecording(snapshot, snapshot.mdDisc, devId);
            if (afterPreflight->deviceConnected) {
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
            o->setProperty("used_clusters", rec.budget.usedClusters);
            o->setProperty("total_clusters", rec.budget.totalClusters);
            o->setProperty("used_tracks", rec.budget.usedTracks);
            o->setProperty("used_fragments", rec.budget.usedFragments);
            o->setProperty("shared_fragments", rec.budget.sharedFragmentCount);
            result = juce::var(o);
            uiStatus = "Recorded " + juce::String(rec.uploadedPhysicalSegments) +
                       " physical segment(s) -> " + juce::String(rec.logicalTracks) +
                       " UTOC track(s).";
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
        std::optional<carta::NetMdPreflightReport> afterPreflight;
        {
            std::lock_guard<std::mutex> lk(netMdMutex);
            uiStatus = netMdPendingUiStatus;
            alertTitle = netMdPendingAlertTitle;
            alertBody = netMdPendingAlertBody;
            afterPreflight = netMdPendingPreflight;
        }
        if (afterPreflight.has_value()) {
            cachedNetMdPreflight = afterPreflight;
        }
        if (uiStatus.isNotEmpty()) {
            status.setText(uiStatus, juce::dontSendNotification);
        }
        refreshRecordWorkspace(false);
        if (alertBody.isNotEmpty() && !headlessMode) {
            showGuardedStudioAlert(juce::MessageBoxOptions()
                                       .withIconType(juce::MessageBoxIconType::InfoIcon)
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
    const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
    const auto budget = compiled ? carta::computeUtocBudget(utoc, medium) : carta::UtocBudget{};
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
                !st.supportsSpUpload ? "No" : (fw.startsWithChar('R') ? "Type-R" : "Type-S");
            snap.badgeText = "FW " + fw + dot +
                             "ForceTOC: " + (st.supportsForceTocEdit ? "OK" : "NO") + dot +
                             "SPUpload: " + spLabel;
            const bool spNeeded =
                std::any_of(plan.spans.begin(), plan.spans.end(), [](const auto& s) {
                    return s.codec == carta::MdCodecMode::spStereo ||
                           s.codec == carta::MdCodecMode::spMono;
                });
            snap.canReadToc = st.discPresent && st.supportsForceTocEdit;
            snap.canErase = st.discPresent && !st.writeProtected;
            const bool baseWritable = compiled && budget.fits() && st.discPresent && st.writable &&
                                      !st.writeProtected && !st.hiMdFormatted &&
                                      st.supportsForceTocEdit;
            snap.canWriteUtoc = baseWritable;
            snap.canRecord = baseWritable && planOk && !session.mdDisc.tracks.empty() &&
                             (!spNeeded || st.supportsSpUpload) && pendingImports.empty();

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
            } else if (session.mdDisc.tracks.empty()) {
                snap.statusText = "Device ready (" + juce::String::fromUTF8(st.deviceName.c_str()) +
                                  "). Add tracks on Tracks & UTOC to record.";
            } else if (spNeeded && !st.supportsSpUpload) {
                snap.statusText =
                    "SP upload requires a Type-S or Type-R NetMD recorder (switch tracks to "
                    "LP2/LP4 or connect compatible hardware).";
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
    showGuardedStudioAlert(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle("Erase & record MiniDisc")
            .withMessage(
                "Erase the disc in '" + selectedId +
                "', upload ATRAC tracks via NetMD / SPUpload, and commit full UTOC Sectors "
                "0, 1, 2, and 4?")
            .withButton("Erase & Record")
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
