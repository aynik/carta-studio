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
#include "MiniDisc/NativeCodecs.h"

#include <map>
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
    if (carta::isHiMdMedium(session.mdDisc.medium)) {
        showTextFieldsPrompt(
            "Hi-MD track metadata",
            "Edit Hi-MD UTF-16BE title, artist, and album metadata.",
            {{"title", "Title", juce::String::fromUTF8(track.halfWidthTitle.c_str())},
             {"artist", "Artist", juce::String::fromUTF8(track.artist.c_str())},
             {"album", "Album", juce::String::fromUTF8(track.album.c_str())}},
            "Apply",
            [safe = juce::Component::SafePointer<Studio>(this), revision,
             idx0](bool accepted, const juce::StringArray& values) {
                if (safe == nullptr || !accepted || values.size() != 3) {
                    return;
                }
                if (revision != safe->editGeneration) {
                    safe->status.setText("Project changed; review MiniDisc workspace and try again.",
                                         juce::dontSendNotification);
                    safe->refreshDiscWorkspace(true);
                    return;
                }
                if (idx0 < 0 || idx0 >= static_cast<int>(safe->session.mdDisc.tracks.size())) {
                    return;
                }
                const auto& cur = safe->session.mdDisc.tracks[static_cast<size_t>(idx0)];
                const bool titleChanged = values[0].toStdString() != cur.halfWidthTitle;
                const bool metaChanged = values[1].toStdString() != cur.artist ||
                                         values[2].toStdString() != cur.album;
                bool anyOk = false;
                if (titleChanged) {
                    const auto res =
                        safe->ipcCommand({"disc", "track", "rename", juce::String(idx0), values[0]});
                    if (!static_cast<bool>(res["ok"])) {
                        safe->status.setText(res["error"].toString(), juce::dontSendNotification);
                        return;
                    }
                    anyOk = true;
                }
                if (metaChanged) {
                    const auto res = safe->ipcCommand(
                        {"disc", "track", "metadata", juce::String(idx0), values[1], values[2]});
                    if (!static_cast<bool>(res["ok"])) {
                        safe->status.setText(res["error"].toString(), juce::dontSendNotification);
                        return;
                    }
                    anyOk = true;
                }
                if (anyOk) {
                    safe->refreshDiscWorkspace(true);
                    safe->status.setText("Updated Hi-MD track metadata.",
                                         juce::dontSendNotification);
                } else {
                    safe->status.setText("Hi-MD track metadata unchanged.",
                                         juce::dontSendNotification);
                }
            });
        return;
    }

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
    const bool isHiMd = carta::isHiMdMedium(session.mdDisc.medium);
    const auto effCodec = carta::effectiveTrackCodec(session.mdDisc, track);
    const juce::String codecVal = juce::String(carta::mdCodecModeToString(
        track.codecOverride ? *track.codecOverride : effCodec));
    showTextFieldsPrompt(
        isHiMd ? "Hi-MD track options" : "Track MiniDisc UTOC flags",
        onDiscTrack
            ? "Per-track flags (encoding codec is locked for existing on-disc audio): "
              "write-protect (on/off), SCMS (on/off), emphasis (on/off), and timestamp "
              "(YYYY-MM-DD HH:MM:SS or empty)."
            : (isHiMd
                   ? "Per-track options: codec override (hi-sp, hi-lp, lp2, lp4, pcm, or empty "
                     "for disc default), write-protect (on/off), SCMS (on/off), emphasis "
                     "(on/off), and timestamp (YYYY-MM-DD HH:MM:SS or empty)."
                   : "Per-track UTOC flags: codec override (sp-stereo, sp-mono, lp2, lp4, or "
                     "empty for disc default), write-protect (on/off), SCMS (on/off), emphasis "
                     "(on/off), and Sector 2 timestamp (YYYY-MM-DD HH:MM:SS or empty)."),
        {{"codec", onDiscTrack ? "Codec (locked on disc)" : "Codec override (or empty)",
          track.codecOverride || onDiscTrack ? codecVal : juce::String()},
         {"wp", "Write-protect (on/off)", track.writeProtected ? "on" : "off"},
         {"scms", "SCMS restricted (on/off)", track.scmsCopyProtected ? "on" : "off"},
         {"emph", "Emphasis (on/off)", track.emphasis ? "on" : "off"},
         {"ts", "Timestamp (YYYY-MM-DD HH:MM:SS or empty)",
          juce::String::fromUTF8(track.timestamp.c_str())},
         {"gap", "Force gapped encode before track (on/off)", track.forceGapBefore ? "on" : "off"}},
        "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), revision, idx0, onDiscTrack,
         codecVal](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted || values.size() != 6) {
                return;
            }
            const auto c = values[0].trim();
            const auto wp = values[1].trim();
            const auto scms = values[2].trim();
            const auto emph = values[3].trim();
            const auto ts = values[4].trim();
            const auto gap = values[5].trim();
            const juce::String codecArg =
                (onDiscTrack && c == codecVal) ? juce::String("keep")
                : c.isEmpty()                  ? juce::String("clear")
                                               : c;
            safe->discWorkspace.onCommand({"disc", "track", "flags", juce::String(idx0), codecArg,
                                           wp.isEmpty() ? juce::String("keep") : wp,
                                           scms.isEmpty() ? juce::String("keep") : scms,
                                           emph.isEmpty() ? juce::String("keep") : emph,
                                           ts.isEmpty() ? juce::String("clear") : ts,
                                           gap.isEmpty() ? juce::String("keep") : gap},
                                          revision);
        });
}

// ============================================================================
// UTOC Inspection, Bundle I/O & Disc Program WAV Export
// ============================================================================

void Studio::inspectCurrentUtoc() {
    discWorkspace.commitPendingEdits();
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
    discWorkspace.commitPendingEdits();
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
            case carta::MdCodecMode::hiSp:
                row.codec = "Hi-SP";
                break;
            case carta::MdCodecMode::hiLp:
                row.codec = "Hi-LP";
                break;
            case carta::MdCodecMode::pcm:
                row.codec = "PCM";
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
                // Pack Sectors 0, 1, 2, and 4 into the 9408-byte UTOC bundle (Sector 3 / P-TOC omitted).
                const std::size_t sec4Idx = carta::utocSector4IndexIn5SectorDump(rawSectors);
                std::vector<std::uint8_t> bundle(4 * carta::kUtocSectorBytes, 0);
                std::memcpy(bundle.data() + 0 * carta::kUtocSectorBytes, rawSectors[0].data(),
                            carta::kUtocSectorBytes);
                std::memcpy(bundle.data() + 1 * carta::kUtocSectorBytes, rawSectors[1].data(),
                            carta::kUtocSectorBytes);
                std::memcpy(bundle.data() + 2 * carta::kUtocSectorBytes, rawSectors[2].data(),
                            carta::kUtocSectorBytes);
                std::memcpy(bundle.data() + 3 * carta::kUtocSectorBytes, rawSectors[sec4Idx].data(),
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
            if (deviceRate > 0) {
                session.prepareResampling(resampling, deviceRate);
            }
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

bool Studio::hasUnloadedOnDiscPlaceholders() const {
    if (!session.mdDisc.loadedDisc.active) {
        return false;
    }
    for (const auto& track : session.tracks) {
        for (const auto& clip : track.clips) {
            if (clip.audio && clip.audio->onDiscPlaceholder && !clip.onDiscRegionId.empty()) {
                return true;
            }
        }
    }
    return false;
}

bool Studio::canDownloadDiscAudio() const {
    if (netMdBusy.load(std::memory_order_acquire) || hiMdBusy.load(std::memory_order_acquire) ||
        !hasUnloadedOnDiscPlaceholders()) {
        return false;
    }
    if (carta::isHiMdMedium(session.mdDisc.medium)) {
        if (cachedHiMdStatus.has_value()) {
            return !cachedHiMdStatus->tracks.empty();
        }
        return !cachedHiMdDevices.empty();
    }
    if (cachedNetMdPreflight.has_value() && cachedNetMdPreflight->deviceConnected) {
        const auto& st = cachedNetMdPreflight->device;
        return st.discPresent && !st.hiMdFormatted && st.supportsAtracRecovery;
    }
    return !cachedNetMdDevices.empty();
}

juce::Rectangle<int> Studio::onDiscDownloadPillBounds(const juce::Rectangle<int>& clipRect,
                                                      const juce::Rectangle<int>& visibleRect,
                                                      bool downloading,
                                                      bool canDownload) const noexcept {
    if (!downloading && !canDownload) {
        return {};
    }
    const int header = std::min(titleHeight, std::max(0, clipRect.getHeight() - 8));
    const auto content = clipRect.withTrimmedTop(header);
    const auto vis = content.getIntersection(visibleRect);
    if (vis.getHeight() < 20 || vis.getWidth() < 92) {
        return {};
    }
    const int pillW = std::min(vis.getWidth() - 12, downloading ? 136 : 124);
    const int pillH = 20;
    return {vis.getX() + 6, vis.getCentreY() - pillH / 2, pillW, pillH};
}

bool Studio::isClipCurrentlyDownloading(const std::string& regionId, std::int64_t clipStartSample,
                                        std::int64_t clipEndSample) {
    if (regionId.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lk(netMdMutex);
    if (activeDownloadRegionId != regionId) {
        return false;
    }
    if (activeDownloadClipStartSample < 0 || activeDownloadClipEndSample <= activeDownloadClipStartSample) {
        return true;
    }
    return std::abs(activeDownloadClipStartSample - clipStartSample) <= 2 &&
           std::abs(activeDownloadClipEndSample - clipEndSample) <= 2;
}

std::string Studio::currentDownloadingRegionId() {
    std::lock_guard<std::mutex> lk(netMdMutex);
    return activeDownloadRegionId;
}

double Studio::currentDownloadingRegionProgress() {
    if (netMdBusy.load(std::memory_order_acquire)) {
        return carta::getNetMdProgress().progress;
    }
    if (hiMdBusy.load(std::memory_order_acquire)) {
        return carta::getHiMdProgress().progress;
    }
    std::lock_guard<std::mutex> lk(netMdMutex);
    return activeDownloadRegionProgress;
}

void Studio::drainPendingDownloadedRegions() {
    std::vector<PendingDownloadedRegion> ready;
    {
        std::lock_guard<std::mutex> lk(netMdMutex);
        ready.swap(pendingDownloadedRegions);
    }
    if (ready.empty()) {
        return;
    }
    bool anyApplied = false;
    {
        const juce::ScopedLock guard(lock);
        for (const auto& item : ready) {
            if (carta::applyDownloadedOnDiscRegionAudio(session, item.regionId, item.audio,
                                                        item.clipStartSample,
                                                        item.clipEndSample)) {
                anyApplied = true;
            }
        }
        if (anyApplied) {
            if (deviceRate > 0) {
                session.prepareResampling(resampling, deviceRate);
            }
            dirty = true;
            ++editGeneration;
        }
    }
    if (anyApplied) {
        refreshDiscWorkspace(false);
        repaint();
    }
}

void Studio::startNetMdDownloadAudioJob(const std::string& devId,
                                        std::vector<std::string> regionIdsIn, bool showAlerts,
                                        std::optional<int> onlyUtocTrackIndex0,
                                        std::vector<DownloadTargetClip> clipTargetsIn) {
    struct ClipJobItem {
        carta::MdOnDiscRegion region;
        std::int64_t clipStartSample = -1;
        std::int64_t clipEndSample = -1;
        std::string label;
        std::shared_ptr<carta::Audio> existingAudio;
        std::vector<std::pair<int, std::int64_t>> tracksWithStartLin; // (trackIndex0, fragStartLin)
    };

    std::vector<ClipJobItem> jobs;
    std::string prepError;
    {
        const juce::ScopedLock guard(lock);
        if (!session.mdDisc.loadedDisc.active ||
            session.mdDisc.importedUtocSectors[0].size() != carta::kUtocSectorBytes) {
            prepError = "Load a MiniDisc UTOC first before downloading disc audio.";
        } else {
            carta::UtocDiscState utoc{};
            std::string parseErr;
            if (!carta::parseUtocSectors(session.mdDisc.importedUtocSectors[0].data(), nullptr,
                                         nullptr, nullptr, utoc, parseErr) ||
                !utoc.hasSector0) {
                prepError = parseErr.empty() ? "Stored UTOC Sector 0 is invalid." : parseErr;
            } else {
                struct UtocTrackSpan {
                    int trackIndex0 = 0;
                    std::int64_t startLin = 0;
                    std::int64_t endLin = 0;
                };
                std::vector<UtocTrackSpan> utocTracks;
                for (int t = 1; t <= static_cast<int>(utoc.trackCount); ++t) {
                    std::uint8_t slot = utoc.trackMap[static_cast<std::size_t>(t)];
                    std::set<std::uint8_t> seen;
                    std::int64_t minLin = std::numeric_limits<std::int64_t>::max();
                    std::int64_t maxLin = -1;
                    while (slot != 0 && seen.insert(slot).second) {
                        const auto& frag = utoc.fragments[slot];
                        if (frag.isValid()) {
                            minLin = std::min(minLin, frag.start.toLinearGroup());
                            maxLin = std::max(maxLin, frag.end.toLinearGroup());
                        }
                        slot = frag.link;
                    }
                    if (maxLin >= minLin) {
                        utocTracks.push_back({t - 1, minLin, maxLin});
                    }
                }

                std::map<std::string, const carta::MdOnDiscRegion*> regById;
                for (const auto& reg : session.mdDisc.loadedDisc.regions) {
                    regById[reg.id] = &reg;
                }

                std::set<std::string> regionFilter(regionIdsIn.begin(), regionIdsIn.end());
                auto matchesClipTarget = [&](const std::string& rId, std::int64_t cStart,
                                             std::int64_t cEnd) {
                    if (clipTargetsIn.empty()) {
                        return regionFilter.empty() || regionFilter.count(rId) > 0;
                    }
                    for (const auto& t : clipTargetsIn) {
                        if (t.regionId != rId) {
                            continue;
                        }
                        if (t.clipStartSample < 0 || t.clipEndSample <= t.clipStartSample) {
                            return true;
                        }
                        if (std::abs(t.clipStartSample - cStart) <= 2 &&
                            std::abs(t.clipEndSample - cEnd) <= 2) {
                            return true;
                        }
                    }
                    return false;
                };

                for (const auto& tr : session.tracks) {
                    for (const auto& clip : tr.clips) {
                        if (clip.onDiscRegionId.empty() || !clip.audio) {
                            continue;
                        }
                        const auto regIt = regById.find(clip.onDiscRegionId);
                        if (regIt == regById.end()) {
                            continue;
                        }
                        const auto& reg = *regIt->second;
                        const auto cStart = static_cast<std::int64_t>(
                            std::llround(clip.start * carta::sessionRate));
                        const auto cEnd = static_cast<std::int64_t>(
                            std::llround((clip.start + clip.duration) * carta::sessionRate));
                        if (cEnd <= cStart) {
                            continue;
                        }
                        if (!matchesClipTarget(reg.id, cStart, cEnd)) {
                            continue;
                        }
                        if (clipTargetsIn.empty() && regionFilter.empty() &&
                            !onlyUtocTrackIndex0.has_value() && !clip.audio->onDiscPlaceholder) {
                            continue;
                        }

                        // Map this clip's timeline span [cStart, cEnd) back to physical UTOC
                        // sound-unit addresses inside `reg`.
                        const std::int64_t samplesPerUnit = carta::mdSamplesPerUnit(reg.codec);
                        const std::int64_t regStartLin = reg.physStart.toLinearUnits();
                        const std::int64_t clipStartUnits =
                            regStartLin +
                            std::max<std::int64_t>(0, (cStart - reg.timelineStartSample) /
                                                          std::max<std::int64_t>(1, samplesPerUnit));
                        const std::int64_t clipEndUnits =
                            regStartLin +
                            std::max<std::int64_t>(0, (cEnd - reg.timelineStartSample - 1) /
                                                          std::max<std::int64_t>(1, samplesPerUnit));

                        std::vector<std::pair<int, std::int64_t>> matched;
                        // Prefer a single UTOC track that completely covers this clip so a slight
                        // overlap with an adjacent track does not trigger downloading two tracks.
                        int singleCoverIdx = -1;
                        std::int64_t singleCoverStart = 0;
                        std::int64_t singleCoverLen = std::numeric_limits<std::int64_t>::max();
                        for (const auto& ut : utocTracks) {
                            if (onlyUtocTrackIndex0.has_value() &&
                                ut.trackIndex0 != *onlyUtocTrackIndex0) {
                                continue;
                            }
                            if (ut.startLin <= clipStartUnits && ut.endLin >= clipEndUnits) {
                                const std::int64_t spanLen = ut.endLin - ut.startLin;
                                if (spanLen < singleCoverLen) {
                                    singleCoverLen = spanLen;
                                    singleCoverIdx = ut.trackIndex0;
                                    singleCoverStart = ut.startLin;
                                }
                            }
                        }
                        if (singleCoverIdx >= 0) {
                            matched.emplace_back(singleCoverIdx, singleCoverStart);
                        } else {
                            for (const auto& ut : utocTracks) {
                                if (onlyUtocTrackIndex0.has_value() &&
                                    ut.trackIndex0 != *onlyUtocTrackIndex0) {
                                    continue;
                                }
                                if (ut.startLin <= clipEndUnits && ut.endLin >= clipStartUnits) {
                                    matched.emplace_back(ut.trackIndex0, ut.startLin);
                                }
                            }
                            std::sort(matched.begin(), matched.end(),
                                      [](const auto& a, const auto& b) {
                                          return a.second < b.second;
                                      });
                        }
                        if (!matched.empty()) {
                            jobs.push_back({reg, cStart, cEnd,
                                            clip.name.isNotEmpty() ? clip.name.toStdString()
                                                                   : reg.id,
                                            clip.audio->hasPcm() ? clip.audio : nullptr,
                                            std::move(matched)});
                        }
                    }
                }
            }
        }
    }

    if (!prepError.empty()) {
        status.setText(juce::String::fromUTF8(prepError.c_str()), juce::dontSendNotification);
        return;
    }
    if (jobs.empty()) {
        status.setText("All selected on-disc clips are already downloaded.",
                       juce::dontSendNotification);
        return;
    }

    netMdStopRequested.store(false, std::memory_order_release);
    carta::clearNetMdDownloadCancel();

    launchNetMdWorker(
        "download-audio", devId, "Downloading audio from MiniDisc...",
        [this, devId, jobList = std::move(jobs), showAlerts](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody,
            std::optional<carta::NetMdPreflightReport>&) mutable {
            int downloadedCount = 0;
            bool cancelled = false;
            const int totalJobs = static_cast<int>(jobList.size());
            const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory);

            for (int ri = 0; ri < totalJobs; ++ri) {
                if (netMdStopRequested.load(std::memory_order_acquire) ||
                    carta::isNetMdDownloadCancelRequested()) {
                    cancelled = true;
                    break;
                }
                const auto& job = jobList[static_cast<std::size_t>(ri)];
                const int channels = carta::mdChannelsForCodec(job.region.codec);
                const std::int64_t targetFrames64 =
                    std::max<std::int64_t>(1, job.clipEndSample - job.clipStartSample);
                if (targetFrames64 > std::numeric_limits<int>::max()) {
                    err = "On-disc clip exceeds maximum supported sample length.";
                    break;
                }
                const int targetFrames = static_cast<int>(targetFrames64);
                {
                    std::lock_guard<std::mutex> lk(netMdMutex);
                    activeDownloadRegionId = job.region.id;
                    activeDownloadClipStartSample = job.clipStartSample;
                    activeDownloadClipEndSample = job.clipEndSample;
                    activeDownloadRegionProgress = 0.0;
                }

                std::vector<float> regL(static_cast<std::size_t>(targetFrames), 0.0f);
                std::vector<float> regR(
                    channels == 2 ? static_cast<std::size_t>(targetFrames) : 0u, 0.0f);
                if (job.existingAudio && job.existingAudio->hasPcm()) {
                    const int prevN = std::min(targetFrames, job.existingAudio->numSamples());
                    if (const float* prevL = job.existingAudio->channelData(0)) {
                        std::memcpy(regL.data(), prevL,
                                    static_cast<std::size_t>(prevN) * sizeof(float));
                    }
                    if (channels == 2) {
                        const float* prevR =
                            job.existingAudio->numChannels() >= 2
                                ? job.existingAudio->channelData(1)
                                : job.existingAudio->channelData(0);
                        if (prevR != nullptr) {
                            std::memcpy(regR.data(), prevR,
                                        static_cast<std::size_t>(prevN) * sizeof(float));
                        }
                    }
                }
                const std::string codecStr = carta::mdCodecModeToString(job.region.codec);
                const std::int64_t samplesPerUnit = carta::mdSamplesPerUnit(job.region.codec);
                const std::int64_t clipStartLin =
                    job.region.physStart.toLinearUnits() +
                    std::max<std::int64_t>(0, (job.clipStartSample - job.region.timelineStartSample) /
                                                  std::max<std::int64_t>(1, samplesPerUnit));

                bool clipOk = true;
                for (std::size_t ti = 0; ti < job.tracksWithStartLin.size(); ++ti) {
                    const int trackIndex0 = job.tracksWithStartLin[ti].first;
                    const std::int64_t fragStartLin = job.tracksWithStartLin[ti].second;
                    const auto stamp =
                        static_cast<juce::int64>(juce::Time::getHighResolutionTicks());
                    const juce::File tmpFile = tempDir.getChildFile(
                        "carta_netmd_dl_" + juce::String(stamp) + "_" + juce::String(trackIndex0) +
                        ".bin");
                    struct TempCleaner {
                        juce::File f;
                        ~TempCleaner() {
                            if (f.existsAsFile()) {
                                f.deleteFile();
                            }
                        }
                    } cleaner{tmpFile};

                    std::string ext;
                    if (!carta::downloadNetMdTrack(devId, trackIndex0,
                                                   tmpFile.getFullPathName().toStdString(), ext,
                                                   err)) {
                        clipOk = false;
                        break;
                    }

                    juce::MemoryBlock rawData;
                    if (!tmpFile.loadFileAsData(rawData) || rawData.getSize() == 0) {
                        err = "Downloaded NetMD track file is empty.";
                        clipOk = false;
                        break;
                    }

                    const auto* rawPtr = static_cast<const std::uint8_t*>(rawData.getData());
                    const std::size_t rawLen = rawData.getSize();
                    const std::uint8_t* payloadPtr = rawPtr;
                    std::size_t payloadLen = rawLen;
                    if (codecStr == "sp-stereo" || codecStr == "sp-mono") {
                        constexpr std::size_t kAeaHdr = 2048u;
                        if (rawLen <= kAeaHdr) {
                            err = "Downloaded .aea track is smaller than the 2048-byte header.";
                            clipOk = false;
                            break;
                        }
                        payloadPtr = rawPtr + kAeaHdr;
                        payloadLen = rawLen - kAeaHdr;
                    } else if (rawLen >= 12u && std::memcmp(rawPtr, "RIFF", 4) == 0 &&
                               std::memcmp(rawPtr + 8, "WAVE", 4) == 0) {
                        std::size_t off = 12u;
                        bool foundData = false;
                        while (off + 8u <= rawLen) {
                            const std::uint32_t sz =
                                static_cast<std::uint32_t>(rawPtr[off + 4]) |
                                (static_cast<std::uint32_t>(rawPtr[off + 5]) << 8) |
                                (static_cast<std::uint32_t>(rawPtr[off + 6]) << 16) |
                                (static_cast<std::uint32_t>(rawPtr[off + 7]) << 24);
                            if (std::memcmp(rawPtr + off, "data", 4) == 0) {
                                payloadPtr = rawPtr + off + 8u;
                                payloadLen = std::min<std::size_t>(sz, rawLen - (off + 8u));
                                foundData = true;
                                break;
                            }
                            const std::size_t step = 8u + static_cast<std::size_t>(sz) + (sz & 1u);
                            if (step == 0 || off + step > rawLen) {
                                break;
                            }
                            off += step;
                        }
                        if (!foundData) {
                            err = "Downloaded ATRAC3 WAV is missing a data chunk.";
                            clipOk = false;
                            break;
                        }
                    }

                    std::vector<float> decL;
                    std::vector<float> decR;
                    int decCh = 0;
                    if (!carta::decodeRawAtracPayload(codecStr, payloadPtr, payloadLen, decL, decR,
                                                      decCh, err)) {
                        clipOk = false;
                        break;
                    }

                    const std::int64_t signedOffset = (fragStartLin - clipStartLin) * samplesPerUnit;
                    const int srcOffset = static_cast<int>(
                        std::clamp<std::int64_t>(-signedOffset, 0, static_cast<std::int64_t>(decL.size())));
                    const int dstOffset = static_cast<int>(
                        std::clamp<std::int64_t>(signedOffset, 0, targetFrames));
                    const int copyN = std::min(static_cast<int>(decL.size()) - srcOffset,
                                               std::max(0, targetFrames - dstOffset));
                    for (int s = 0; s < copyN; ++s) {
                        regL[static_cast<std::size_t>(dstOffset + s)] =
                            decL[static_cast<std::size_t>(srcOffset + s)];
                    }
                    if (channels == 2) {
                        const auto& srcR = (decCh >= 2 && decR.size() >= decL.size()) ? decR : decL;
                        for (int s = 0; s < copyN; ++s) {
                            regR[static_cast<std::size_t>(dstOffset + s)] =
                                srcR[static_cast<std::size_t>(srcOffset + s)];
                        }
                    }

                    std::shared_ptr<carta::Audio> decodedAudio;
                    if (!carta::createDecodedOnDiscAudioSource(
                            job.label.empty() ? job.region.id : job.label, regL.data(),
                            channels == 2 ? regR.data() : nullptr, channels, targetFrames,
                            targetFrames, decodedAudio, err)) {
                        clipOk = false;
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> lk(netMdMutex);
                        pendingDownloadedRegions.push_back({job.region.id, std::move(decodedAudio),
                                                            job.clipStartSample,
                                                            job.clipEndSample});
                    }
                }

                if (!clipOk) {
                    break;
                }
                ++downloadedCount;
            }

            {
                std::lock_guard<std::mutex> lk(netMdMutex);
                activeDownloadRegionId.clear();
                activeDownloadClipStartSample = -1;
                activeDownloadClipEndSample = -1;
                activeDownloadRegionProgress = 0.0;
            }
            const bool wasCancelled = cancelled ||
                                      netMdStopRequested.load(std::memory_order_acquire) ||
                                      carta::isNetMdDownloadCancelRequested();
            netMdStopRequested.store(false, std::memory_order_release);
            carta::clearNetMdDownloadCancel();

            if (wasCancelled) {
                err.clear();
                auto* o = new juce::DynamicObject();
                o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
                o->setProperty("cancelled", true);
                o->setProperty("downloaded_regions", downloadedCount);
                result = juce::var(o);
                uiStatus = downloadedCount > 0
                               ? ("Download cancelled (" + juce::String(downloadedCount) +
                                  " track(s) kept).")
                               : "Disc audio download cancelled.";
                return true;
            }

            if (!err.empty()) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Download Disc Audio Failed";
                    alertBody = uiStatus;
                }
                return false;
            }

            auto* o = new juce::DynamicObject();
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            o->setProperty("downloaded_regions", downloadedCount);
            result = juce::var(o);
            uiStatus = "Downloaded audio for " + juce::String(downloadedCount) +
                       " on-disc track(s).";
            return true;
        });
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
    const auto prevPreflight =
        (cachedNetMdPreflight.has_value() && cachedNetMdPreflight->device.deviceId == devId)
            ? cachedNetMdPreflight
            : std::optional<carta::NetMdPreflightReport>{};

    launchNetMdWorker(
        "write-utoc", devId, "Writing UTOC Sectors 0/1/2/4...",
        [this, snapCopy = snapshot, devId, confirmHardware, showAlerts, prevPreflight](
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

            const std::size_t sec4Idx = carta::utocSector4IndexIn5SectorDump(committedRaw);
            const std::size_t preservedIdx = (sec4Idx == 3u) ? 4u : 3u;
            if (snapCopy.mdDisc.loadedDisc.active) {
                snapCopy.mdDisc.loadedDisc.baselineSector0Hash = committedHash;
                snapCopy.mdDisc.loadedDisc.rawSector3.assign(committedRaw[preservedIdx].begin(),
                                                             committedRaw[preservedIdx].end());
            }
            {
                std::lock_guard<std::mutex> lk(netMdMutex);
                netMdPendingCommittedRawSectors = committedRaw;
                netMdPendingHasCommittedRaw = true;
                netMdPendingCommittedHash = committedHash;
                netMdPendingNeedsReload = false;
            }

            if (prevPreflight.has_value() && prevPreflight->deviceConnected) {
                auto updatedDev = prevPreflight->device;
                applyCommittedDiscIdentity(updatedDev,
                                           static_cast<int>(snapCopy.mdDisc.tracks.size()),
                                           snapCopy.mdDisc.halfWidthTitle,
                                           snapCopy.mdDisc.fullWidthTitle, false,
                                           &snapCopy.mdDisc);
                afterPreflight = carta::preflightNetMdRecordingWithStatus(
                    snapCopy, snapCopy.mdDisc, updatedDev);
            } else {
                afterPreflight = carta::preflightNetMdRecording(snapCopy, snapCopy.mdDisc, devId);
                if (afterPreflight->deviceConnected) {
                    applyCommittedDiscIdentity(afterPreflight->device,
                                               static_cast<int>(snapCopy.mdDisc.tracks.size()),
                                               snapCopy.mdDisc.halfWidthTitle,
                                               snapCopy.mdDisc.fullWidthTitle, false,
                                               &snapCopy.mdDisc);
                }
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
    const auto prevPreflight =
        (cachedNetMdPreflight.has_value() && cachedNetMdPreflight->device.deviceId == devId)
            ? cachedNetMdPreflight
            : std::optional<carta::NetMdPreflightReport>{};

    launchNetMdWorker(
        "record", devId, "Preparing ATRAC spans...",
        [this, snapshot = std::move(snapshotIn), devId, confirmHardware, showAlerts, prevPreflight](
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
                const std::size_t sec4Idx =
                    carta::utocSector4IndexIn5SectorDump(rec.committedRawSectors);
                const std::size_t preservedIdx = (sec4Idx == 3u) ? 4u : 3u;
                if (snapshot.mdDisc.loadedDisc.active) {
                    snapshot.mdDisc.loadedDisc.baselineSector0Hash = rec.committedSector0Hash;
                    snapshot.mdDisc.loadedDisc.rawSector3.assign(
                        rec.committedRawSectors[preservedIdx].begin(),
                        rec.committedRawSectors[preservedIdx].end());
                }
                std::lock_guard<std::mutex> lk(netMdMutex);
                netMdPendingCommittedRawSectors = rec.committedRawSectors;
                netMdPendingHasCommittedRaw = true;
                netMdPendingCommittedHash = rec.committedSector0Hash;
                netMdPendingNeedsReload = rec.needsReloadToFinishUpload;
            }

            if (!rec.needsReloadToFinishUpload && prevPreflight.has_value() &&
                prevPreflight->deviceConnected) {
                auto updatedDev = prevPreflight->device;
                applyCommittedDiscIdentity(updatedDev,
                                           static_cast<int>(snapshot.mdDisc.tracks.size()),
                                           snapshot.mdDisc.halfWidthTitle,
                                           snapshot.mdDisc.fullWidthTitle, false, &snapshot.mdDisc);
                afterPreflight = carta::preflightNetMdRecordingWithStatus(
                    snapshot, snapshot.mdDisc, updatedDev);
            } else {
                afterPreflight = carta::preflightNetMdRecording(snapshot, snapshot.mdDisc, devId);
                if (afterPreflight->deviceConnected && !rec.needsReloadToFinishUpload) {
                    applyCommittedDiscIdentity(afterPreflight->device,
                                               static_cast<int>(snapshot.mdDisc.tracks.size()),
                                               snapshot.mdDisc.halfWidthTitle,
                                               snapshot.mdDisc.fullWidthTitle, false,
                                               &snapshot.mdDisc);
                }
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
    drainPendingDownloadedRegions();
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
                        const std::size_t sec4Idx =
                            carta::utocSector4IndexIn5SectorDump(committedRaw);
                        const std::size_t preservedIdx = (sec4Idx == 3u) ? 4u : 3u;
                        session.mdDisc.loadedDisc.baselineSector0Hash = committedHash;
                        session.mdDisc.loadedDisc.rawSector3.assign(
                            committedRaw[preservedIdx].begin(), committedRaw[preservedIdx].end());
                        carta::UtocDiscState parsedCommitted{};
                        std::string parseErr;
                        if (carta::parseUtocSectors(
                                committedRaw[0].data(), committedRaw[1].data(),
                                committedRaw[2].data(), committedRaw[sec4Idx].data(),
                                parsedCommitted, parseErr) &&
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
        if (!currentDownloadingRegionId().empty()) {
            repaint();
        }
    }
}

// ============================================================================
// Hi-MD Device Discovery, Status JSON, Async Worker Jobs & Polling
// ============================================================================

std::string Studio::resolveActiveHiMdDeviceId(const juce::String& explicitId, std::string& error) {
    if (explicitId.isNotEmpty()) {
        return explicitId.toStdString();
    }

    if (discWorkspace.selectedRecordDeviceIsHiMd()) {
        const auto selected = discWorkspace.selectedRecordDeviceId();
        if (selected.isNotEmpty()) {
            return selected.toStdString();
        }
    }

    std::vector<carta::HiMdDeviceEntry> devs;
    if (!carta::listHiMdDevices(devs, error)) {
        return {};
    }

    cachedHiMdDevices = std::move(devs);
    hiMdScannedOnce = true;
    if (cachedHiMdDevices.empty()) {
        error = "No USB Hi-MD recorder detected. Connect a Sony Hi-MD device and click Refresh.";
        return {};
    }
    return cachedHiMdDevices.front().id;
}

juce::var Studio::buildHiMdStatusJson() {
    auto* root = new juce::DynamicObject();
    const bool busy = hiMdBusy.load(std::memory_order_acquire);
    const auto prog = carta::getHiMdProgress();

    std::lock_guard<std::mutex> lk(hiMdMutex);
    root->setProperty("active", busy);
    root->setProperty("operation",
                      juce::String::fromUTF8((busy ? hiMdActiveOp : hiMdLastOp).c_str()));
    root->setProperty("device_id",
                      juce::String::fromUTF8((busy ? hiMdActiveDevId : hiMdLastDevId).c_str()));
    root->setProperty("phase", juce::String::fromUTF8(prog.phase.c_str()));
    root->setProperty("progress", prog.progress);
    root->setProperty("bytes_done", static_cast<juce::int64>(prog.bytesDone));
    root->setProperty("bytes_total", static_cast<juce::int64>(prog.bytesTotal));
    root->setProperty("ok", hiMdLastOk);
    root->setProperty("error", juce::String::fromUTF8(hiMdLastError.c_str()));
    root->setProperty("elevated", carta::isHiMdElevatedSessionActive());
    root->setProperty("elevation_required", hiMdNeedsElevation);
    root->setProperty("result", hiMdLastResult);
    return juce::var(root);
}

void Studio::startHiMdProbeJob(const std::string& devId, bool allowElevationPrompt) {
    carta::Session snapshot;
    {
        const juce::ScopedLock guard(lock);
        copyWorkerSnapshot(snapshot);
    }

    launchHiMdWorker(
        "probe", devId, "Probing Hi-MD recorder...",
        [this, snapshot, devId, allowElevationPrompt](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String&,
            juce::String&, std::optional<carta::HiMdDeviceStatus>& afterStatus) mutable {
            carta::HiMdDeviceStatus st{};
            if (!carta::probeHiMdDevice(devId, st, err, allowElevationPrompt)) {
                const bool needsElev = carta::isHiMdPermissionError(err);
                if (needsElev) {
                    std::lock_guard<std::mutex> lk(hiMdMutex);
                    hiMdPendingNeedsElevation = true;
                    uiStatus = "Administrator privileges required for Hi-MD USB access. Click "
                               "'Unlock...' to authorize once for this session.";
                } else {
                    uiStatus = juce::String::fromUTF8(err.c_str());
                }
                return false;
            }
            afterStatus = st;
            const auto pre = carta::preflightHiMdRecording(snapshot.mdDisc, &st);

            auto* o = new juce::DynamicObject();
            o->setProperty("device_id", juce::String::fromUTF8(st.deviceId.c_str()));
            o->setProperty("device_name", juce::String::fromUTF8(st.deviceName.c_str()));
            o->setProperty("disc_title", juce::String::fromUTF8(st.discTitle.c_str()));
            o->setProperty("disc_id", juce::String::fromUTF8(st.discId.c_str()));
            o->setProperty("datanum", st.datanum);
            o->setProperty("track_count", st.trackCount);
            o->setProperty("group_count", static_cast<int>(st.groups.size()));
            o->setProperty("fs_total", static_cast<juce::int64>(st.fsTotalBytes));
            o->setProperty("fs_used", static_cast<juce::int64>(st.fsUsedBytes));
            o->setProperty("fs_free", static_cast<juce::int64>(st.fsFreeBytes));
            o->setProperty("preflight_valid", pre.valid);
            o->setProperty("preflight_error", juce::String::fromUTF8(pre.error.c_str()));
            result = juce::var(o);

            uiStatus = "Probed Hi-MD '" +
                       juce::String::fromUTF8(
                           (st.discTitle.empty() ? st.deviceName : st.discTitle).c_str()) +
                       "' (" + juce::String(st.trackCount) + " track(s)).";
            return true;
        });
}

void Studio::startHiMdLoadDiscJob(const std::string& devId, bool showAlerts) {
    launchHiMdWorker(
        "load-disc", devId, "Loading Hi-MD tracks & groups...",
        [this, devId, showAlerts](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody, std::optional<carta::HiMdDeviceStatus>& afterStatus) mutable {
            carta::MdDisc loadedDisc{};
            carta::HiMdDeviceStatus devStatus{};
            if (!carta::loadHiMdDiscIntoSession(devId, loadedDisc, &devStatus, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Load Hi-MD Failed";
                    alertBody = uiStatus;
                }
                return false;
            }
            afterStatus = devStatus;
            {
                std::lock_guard<std::mutex> lk(hiMdMutex);
                hiMdPendingLoadedDisc = loadedDisc;
            }

            auto* o = new juce::DynamicObject();
            o->setProperty("loaded", true);
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            o->setProperty("medium", juce::String(carta::mdMediumToString(loadedDisc.medium)));
            o->setProperty("disc_title",
                           juce::String::fromUTF8(loadedDisc.halfWidthTitle.c_str()));
            o->setProperty("track_count", static_cast<int>(loadedDisc.tracks.size()));
            o->setProperty("group_count", static_cast<int>(loadedDisc.groups.size()));
            o->setProperty("region_count",
                           static_cast<int>(loadedDisc.loadedDisc.regions.size()));
            result = juce::var(o);

            const auto discName = loadedDisc.halfWidthTitle;
            uiStatus =
                "Loaded Hi-MD " +
                (discName.empty() ? juce::String::fromUTF8(devId.c_str())
                                  : ("'" + juce::String::fromUTF8(discName.c_str()) + "'")) +
                " (" + juce::String(static_cast<int>(loadedDisc.tracks.size())) + " track(s), " +
                juce::String(static_cast<int>(loadedDisc.groups.size())) + " group(s)).";
            return true;
        });
}

void Studio::startHiMdDownloadAudioJob(const std::string& devId,
                                       std::vector<std::string> regionIdsIn, bool showAlerts) {
    struct HiMdRegionJobItem {
        carta::MdOnDiscRegion region;
        std::string label;
    };

    std::vector<HiMdRegionJobItem> jobs;
    {
        const juce::ScopedLock guard(lock);
        if (!session.mdDisc.loadedDisc.active) {
            status.setText("Load a Hi-MD disc first before downloading disc audio.",
                           juce::dontSendNotification);
            return;
        }
        std::set<std::string> filter(regionIdsIn.begin(), regionIdsIn.end());
        std::set<std::string> placeholderRegionIds;
        std::map<std::string, std::string> regionLabels;
        for (const auto& tr : session.tracks) {
            for (const auto& clip : tr.clips) {
                if (!clip.onDiscRegionId.empty()) {
                    if (clip.audio && clip.audio->onDiscPlaceholder) {
                        placeholderRegionIds.insert(clip.onDiscRegionId);
                    }
                    if (regionLabels[clip.onDiscRegionId].empty() && clip.name.isNotEmpty()) {
                        regionLabels[clip.onDiscRegionId] = clip.name.toStdString();
                    }
                }
            }
        }
        for (const auto& reg : session.mdDisc.loadedDisc.regions) {
            if (reg.hiMdSlot <= 0) {
                continue;
            }
            if (!filter.empty() && filter.count(reg.id) == 0) {
                continue;
            }
            if (filter.empty() && placeholderRegionIds.count(reg.id) == 0) {
                continue;
            }
            jobs.push_back({reg, regionLabels[reg.id]});
        }
    }

    if (jobs.empty()) {
        status.setText("All selected Hi-MD regions are already downloaded.",
                       juce::dontSendNotification);
        return;
    }

    netMdStopRequested.store(false, std::memory_order_release);
    launchHiMdWorker(
        "download-audio", devId, "Downloading audio from Hi-MD disc...",
        [this, devId, jobList = std::move(jobs), showAlerts](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody, std::optional<carta::HiMdDeviceStatus>& afterStatus) mutable {
            carta::HiMdDeviceStatus devStatus{};
            if (!carta::probeHiMdDevice(devId, devStatus, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Download Hi-MD Audio Failed";
                    alertBody = uiStatus;
                }
                return false;
            }
            afterStatus = devStatus;

            std::map<int, int> slotToTrackIndex;
            for (const auto& tr : devStatus.tracks) {
                if (tr.slot > 0 && slotToTrackIndex.find(tr.slot) == slotToTrackIndex.end()) {
                    slotToTrackIndex[tr.slot] = tr.index;
                }
            }

            int downloadedCount = 0;
            bool cancelled = false;
            const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory);
            for (const auto& job : jobList) {
                if (netMdStopRequested.load(std::memory_order_acquire)) {
                    cancelled = true;
                    break;
                }
                const auto it = slotToTrackIndex.find(job.region.hiMdSlot);
                if (it == slotToTrackIndex.end()) {
                    err = "Hi-MD track slot " + std::to_string(job.region.hiMdSlot) +
                          " is no longer present on the inserted disc.";
                    break;
                }
                const int trackIndex0 = it->second;
                const std::int64_t targetFrames64 =
                    std::max<std::int64_t>(1, job.region.timelineEndSample -
                                                  job.region.timelineStartSample);
                if (targetFrames64 > std::numeric_limits<int>::max()) {
                    err = "Hi-MD region exceeds maximum supported sample length.";
                    break;
                }
                const int targetFrames = static_cast<int>(targetFrames64);

                {
                    std::lock_guard<std::mutex> lk(netMdMutex);
                    activeDownloadRegionId = job.region.id;
                    activeDownloadRegionProgress = 0.0;
                }

                const auto stamp =
                    static_cast<juce::int64>(juce::Time::getHighResolutionTicks());
                const juce::File tmpFile = tempDir.getChildFile(
                    "carta_himd_dl_" + juce::String(stamp) + "_" + juce::String(trackIndex0) +
                    ".raw");
                struct TempCleaner {
                    juce::File f;
                    ~TempCleaner() {
                        if (f.existsAsFile()) {
                            f.deleteFile();
                        }
                    }
                } cleaner{tmpFile};

                if (!carta::dumpHiMdTrack(devId, trackIndex0,
                                          tmpFile.getFullPathName().toStdString(),
                                          /*rawAudio=*/true, err)) {
                    break;
                }

                juce::MemoryBlock rawData;
                if (!tmpFile.loadFileAsData(rawData) || rawData.getSize() == 0) {
                    err = "Dumped Hi-MD audio file is empty.";
                    break;
                }

                std::vector<float> decL;
                std::vector<float> decR;
                int decCh = 0;
                if (!carta::decodeRawAtracPayload(
                        carta::mdCodecModeToString(job.region.codec),
                        static_cast<const std::uint8_t*>(rawData.getData()), rawData.getSize(),
                        decL, decR, decCh, err)) {
                    break;
                }

                const int channels = std::max(1, std::min(2, decCh));
                std::shared_ptr<carta::Audio> decodedAudio;
                if (!carta::createDecodedOnDiscAudioSource(
                        job.label.empty() ? job.region.id : job.label, decL.data(),
                        channels == 2 ? decR.data() : nullptr, channels,
                        static_cast<int>(decL.size()), targetFrames, decodedAudio, err)) {
                    break;
                }
                {
                    std::lock_guard<std::mutex> lk(netMdMutex);
                    pendingDownloadedRegions.push_back({job.region.id, std::move(decodedAudio)});
                }
                ++downloadedCount;
            }

            {
                std::lock_guard<std::mutex> lk(netMdMutex);
                activeDownloadRegionId.clear();
                activeDownloadClipStartSample = -1;
                activeDownloadClipEndSample = -1;
                activeDownloadRegionProgress = 0.0;
            }
            const bool wasCancelled = cancelled || netMdStopRequested.load(std::memory_order_acquire);
            netMdStopRequested.store(false, std::memory_order_release);
            if (wasCancelled) {
                err.clear();
                auto* o = new juce::DynamicObject();
                o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
                o->setProperty("cancelled", true);
                o->setProperty("downloaded_regions", downloadedCount);
                result = juce::var(o);
                uiStatus = downloadedCount > 0
                               ? ("Download cancelled (" + juce::String(downloadedCount) +
                                  " track(s) kept).")
                               : "Hi-MD audio download cancelled.";
                return true;
            }

            if (!err.empty()) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Download Hi-MD Audio Failed";
                    alertBody = uiStatus;
                }
                return false;
            }

            auto* o = new juce::DynamicObject();
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            o->setProperty("downloaded_regions", downloadedCount);
            result = juce::var(o);
            uiStatus = "Downloaded audio for " + juce::String(downloadedCount) +
                       " Hi-MD region(s).";
            return true;
        });
}

void Studio::startHiMdRecordJob(carta::Session&& snapshotIn, const std::string& devId,
                                 bool confirmHardware, bool showAlerts) {
    launchHiMdWorker(
        "record", devId, "Preparing Hi-MD recording...",
        [snapshot = std::move(snapshotIn), devId, confirmHardware, showAlerts](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody, std::optional<carta::HiMdDeviceStatus>& afterStatus) mutable {
            carta::HiMdRecordResult rec{};
            if (!carta::recordHiMdDisc(snapshot, snapshot.mdDisc, devId, confirmHardware, rec,
                                       err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Hi-MD Recording Failed";
                    alertBody = uiStatus;
                }
                return false;
            }

            carta::HiMdDeviceStatus refreshed{};
            std::string probeErr;
            if (carta::probeHiMdDevice(devId, refreshed, probeErr)) {
                afterStatus = refreshed;
            }

            auto* o = new juce::DynamicObject();
            o->setProperty("device_id", juce::String::fromUTF8(rec.deviceId.c_str()));
            o->setProperty("device_name", juce::String::fromUTF8(rec.deviceName.c_str()));
            o->setProperty("disc_title", juce::String::fromUTF8(rec.discTitle.c_str()));
            o->setProperty("uploaded_tracks", rec.uploadedTracks);
            o->setProperty("unique_audio_tracks", rec.uniqueAudioTracksUploaded);
            o->setProperty("preserved_slots", rec.preservedAudioSlots);
            o->setProperty("freed_slots", rec.freedAudioSlots);
            o->setProperty("gapless_runs", rec.gaplessRuns);
            o->setProperty("uploaded_bytes", static_cast<juce::int64>(rec.uploadedBytes));
            o->setProperty("blocks_written", static_cast<juce::int64>(rec.blocksWritten));
            o->setProperty("datanum", rec.datanum);
            o->setProperty("icv_updated", rec.icvUpdated);
            o->setProperty("hardware_confirmed", rec.hardwareConfirmed);
            result = juce::var(o);

            const juce::String mbStr(static_cast<double>(rec.uploadedBytes) / (1024.0 * 1024.0),
                                     2);
            if (rec.preservedAudioSlots > 0) {
                uiStatus = "Committed Hi-MD (" + juce::String(rec.uploadedTracks) +
                           " Play Order track(s): " + juce::String(rec.preservedAudioSlots) +
                           " preserved, " + juce::String(rec.uniqueAudioTracksUploaded) +
                           " new, " + mbStr + " MB).";
            } else if (rec.uniqueAudioTracksUploaded > 0 &&
                       rec.uniqueAudioTracksUploaded < rec.uploadedTracks) {
                uiStatus = "Recorded " + juce::String(rec.uploadedTracks) +
                           " Play Order track(s) (" + juce::String(rec.uniqueAudioTracksUploaded) +
                           " unique audio, " + mbStr + " MB) to Hi-MD.";
            } else {
                uiStatus = "Recorded " + juce::String(rec.uploadedTracks) + " track(s) (" + mbStr +
                           " MB) to Hi-MD.";
            }
            return true;
        });
}

void Studio::startHiMdWriteIndexJob(const carta::Session& snapshot, const std::string& devId,
                                    bool confirmHardware, bool showAlerts) {
    launchHiMdWorker(
        "write-index", devId, "Writing Hi-MD Play Order & metadata index...",
        [snapCopy = snapshot, devId, confirmHardware, showAlerts](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody, std::optional<carta::HiMdDeviceStatus>& afterStatus) mutable {
            carta::HiMdRecordResult rec{};
            if (!carta::writeHiMdIndex(snapCopy.mdDisc, devId, confirmHardware, rec, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Write Hi-MD Index Failed";
                    alertBody = uiStatus;
                }
                return false;
            }

            carta::HiMdDeviceStatus refreshed{};
            std::string probeErr;
            if (carta::probeHiMdDevice(devId, refreshed, probeErr)) {
                afterStatus = refreshed;
            }

            auto* o = new juce::DynamicObject();
            o->setProperty("committed_index", true);
            o->setProperty("device_id", juce::String::fromUTF8(rec.deviceId.c_str()));
            o->setProperty("disc_title", juce::String::fromUTF8(rec.discTitle.c_str()));
            o->setProperty("track_count", rec.uploadedTracks);
            o->setProperty("preserved_slots", rec.preservedAudioSlots);
            o->setProperty("freed_slots", rec.freedAudioSlots);
            o->setProperty("datanum", rec.datanum);
            o->setProperty("icv_updated", rec.icvUpdated);
            result = juce::var(o);

            uiStatus = "Committed Hi-MD index (" + juce::String(rec.uploadedTracks) +
                       " Play Order track(s), " + juce::String(rec.preservedAudioSlots) +
                       " preserved slot(s)" +
                       (rec.freedAudioSlots > 0
                            ? (", freed " + juce::String(rec.freedAudioSlots) + " slot(s)")
                            : juce::String()) +
                       ").";
            return true;
        });
}

void Studio::startHiMdDumpJob(const std::string& devId, int trackIndex, const juce::File& outFile,
                              bool rawAudio, bool showAlerts) {
    const auto prevStatus =
        (cachedHiMdStatus.has_value() && cachedHiMdStatus->deviceId == devId) ? cachedHiMdStatus
                                                                              : std::optional<carta::HiMdDeviceStatus>{};
    const juce::String outPath = outFile.getFullPathName();
    launchHiMdWorker(
        "dump", devId, "Dumping Hi-MD track...",
        [devId, trackIndex, outFile, outPath, rawAudio, showAlerts, prevStatus](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody, std::optional<carta::HiMdDeviceStatus>& afterStatus) mutable {
            if (!carta::dumpHiMdTrack(devId, trackIndex, outPath.toStdString(), rawAudio, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Hi-MD Track Dump Failed";
                    alertBody = uiStatus;
                }
                return false;
            }
            if (prevStatus.has_value()) {
                afterStatus = prevStatus;
            }
            auto* o = new juce::DynamicObject();
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            o->setProperty("track_index", trackIndex);
            o->setProperty("output_file", outPath);
            o->setProperty("raw", rawAudio);
            o->setProperty("bytes", static_cast<juce::int64>(outFile.getSize()));
            result = juce::var(o);

            uiStatus = "Dumped Hi-MD track " + juce::String(trackIndex + 1) + " to " +
                       outFile.getFileName() + ".";
            if (showAlerts) {
                alertTitle = "Hi-MD Track Dump Complete";
                alertBody = uiStatus + "\n" + outPath;
            }
            return true;
        });
}

void Studio::startHiMdRenameDiscJob(const std::string& devId, const std::string& title,
                                    bool confirmHardware, bool showAlerts) {
    launchHiMdWorker(
        "rename-disc", devId, "Renaming Hi-MD disc...",
        [devId, title, confirmHardware, showAlerts](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody, std::optional<carta::HiMdDeviceStatus>& afterStatus) mutable {
            if (!carta::renameHiMdDisc(devId, title, confirmHardware, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Rename Hi-MD Disc Failed";
                    alertBody = uiStatus;
                }
                return false;
            }
            carta::HiMdDeviceStatus refreshed{};
            std::string probeErr;
            if (carta::probeHiMdDevice(devId, refreshed, probeErr)) {
                afterStatus = refreshed;
            }
            auto* o = new juce::DynamicObject();
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            o->setProperty("title", juce::String::fromUTF8(title.c_str()));
            result = juce::var(o);
            uiStatus = "Renamed Hi-MD disc to '" + juce::String::fromUTF8(title.c_str()) + "'.";
            return true;
        });
}

void Studio::startHiMdRenameTrackJob(const std::string& devId, int trackIndex, bool hasTitle,
                                     const std::string& title, bool hasArtist,
                                     const std::string& artist, bool hasAlbum,
                                     const std::string& album, bool confirmHardware,
                                     bool showAlerts) {
    launchHiMdWorker(
        "rename-track", devId, "Updating Hi-MD track metadata...",
        [devId, trackIndex, hasTitle, title, hasArtist, artist, hasAlbum, album, confirmHardware,
         showAlerts](std::string& err, juce::var& result, juce::String& uiStatus,
                     juce::String& alertTitle, juce::String& alertBody,
                     std::optional<carta::HiMdDeviceStatus>& afterStatus) mutable {
            if (!carta::renameHiMdTrack(devId, trackIndex, hasTitle, title, hasArtist, artist,
                                        hasAlbum, album, confirmHardware, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Update Hi-MD Track Metadata Failed";
                    alertBody = uiStatus;
                }
                return false;
            }
            carta::HiMdDeviceStatus refreshed{};
            std::string probeErr;
            if (carta::probeHiMdDevice(devId, refreshed, probeErr)) {
                afterStatus = refreshed;
            }
            auto* o = new juce::DynamicObject();
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            o->setProperty("track_index", trackIndex);
            result = juce::var(o);
            uiStatus = "Updated Hi-MD track " + juce::String(trackIndex + 1) + " metadata.";
            return true;
        });
}

void Studio::startHiMdMoveTrackJob(const std::string& devId, int fromIndex, int toIndex,
                                   bool confirmHardware, bool showAlerts) {
    launchHiMdWorker(
        "move-track", devId, "Reordering Hi-MD track...",
        [devId, fromIndex, toIndex, confirmHardware, showAlerts](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody, std::optional<carta::HiMdDeviceStatus>& afterStatus) mutable {
            if (!carta::moveHiMdTrack(devId, fromIndex, toIndex, confirmHardware, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Move Hi-MD Track Failed";
                    alertBody = uiStatus;
                }
                return false;
            }
            carta::HiMdDeviceStatus refreshed{};
            std::string probeErr;
            if (carta::probeHiMdDevice(devId, refreshed, probeErr)) {
                afterStatus = refreshed;
            }
            auto* o = new juce::DynamicObject();
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            o->setProperty("from", fromIndex);
            o->setProperty("to", toIndex);
            result = juce::var(o);
            uiStatus = "Moved Hi-MD track " + juce::String(fromIndex + 1) + " to position " +
                       juce::String(toIndex + 1) + ".";
            return true;
        });
}

void Studio::startHiMdSetGroupsJob(const std::string& devId, std::vector<carta::MdGroup> groupsIn,
                                   int trackCount, bool confirmHardware, bool showAlerts) {
    launchHiMdWorker(
        "set-groups", devId, "Updating Hi-MD groups...",
        [devId, groups = std::move(groupsIn), trackCount, confirmHardware, showAlerts](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody, std::optional<carta::HiMdDeviceStatus>& afterStatus) mutable {
            if (!carta::setHiMdGroups(devId, groups, trackCount, confirmHardware, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Update Hi-MD Groups Failed";
                    alertBody = uiStatus;
                }
                return false;
            }
            carta::HiMdDeviceStatus refreshed{};
            std::string probeErr;
            if (carta::probeHiMdDevice(devId, refreshed, probeErr)) {
                afterStatus = refreshed;
            }
            auto* o = new juce::DynamicObject();
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            o->setProperty("group_count", static_cast<int>(groups.size()));
            result = juce::var(o);
            uiStatus =
                "Updated Hi-MD groups (" + juce::String(static_cast<int>(groups.size())) + ").";
            return true;
        });
}

void Studio::startHiMdDeleteTracksJob(const std::string& devId, std::vector<int> trackIndicesIn,
                                      bool confirmHardware, bool showAlerts) {
    launchHiMdWorker(
        "delete-tracks", devId, "Deleting Hi-MD tracks...",
        [devId, trackIndices = std::move(trackIndicesIn), confirmHardware, showAlerts](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody, std::optional<carta::HiMdDeviceStatus>& afterStatus) mutable {
            if (!carta::deleteHiMdTracks(devId, trackIndices, confirmHardware, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Delete Hi-MD Tracks Failed";
                    alertBody = uiStatus;
                }
                return false;
            }
            carta::HiMdDeviceStatus refreshed{};
            std::string probeErr;
            if (carta::probeHiMdDevice(devId, refreshed, probeErr)) {
                afterStatus = refreshed;
            }
            auto* o = new juce::DynamicObject();
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            o->setProperty("deleted_count", static_cast<int>(trackIndices.size()));
            result = juce::var(o);
            uiStatus = "Deleted " + juce::String(static_cast<int>(trackIndices.size())) +
                       " track(s) from Hi-MD.";
            return true;
        });
}

void Studio::startHiMdWipeJob(const std::string& devId, bool reformat, bool confirmHardware,
                              bool showAlerts) {
    launchHiMdWorker(
        "wipe", devId, reformat ? "Reformatting Hi-MD filesystem..." : "Wiping Hi-MD tracks...",
        [devId, reformat, confirmHardware, showAlerts](
            std::string& err, juce::var& result, juce::String& uiStatus, juce::String& alertTitle,
            juce::String& alertBody, std::optional<carta::HiMdDeviceStatus>& afterStatus) mutable {
            if (!carta::wipeHiMdDisc(devId, reformat, confirmHardware, err)) {
                uiStatus = juce::String::fromUTF8(err.c_str());
                if (showAlerts) {
                    alertTitle = "Wipe Hi-MD Failed";
                    alertBody = uiStatus;
                }
                return false;
            }
            carta::HiMdDeviceStatus refreshed{};
            std::string probeErr;
            if (carta::probeHiMdDevice(devId, refreshed, probeErr)) {
                afterStatus = refreshed;
            }
            auto* o = new juce::DynamicObject();
            o->setProperty("wiped", true);
            o->setProperty("reformat", reformat);
            o->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            result = juce::var(o);
            uiStatus = reformat ? "Reformatted Hi-MD FAT16 filesystem." : "Wiped all Hi-MD tracks.";
            return true;
        });
}

void Studio::pollHiMdJobProgress() {
    drainPendingDownloadedRegions();
    const bool busy = hiMdBusy.load(std::memory_order_acquire);
    if (hiMdDonePending.exchange(false, std::memory_order_acq_rel)) {
        if (!busy) {
            hiMdWorker.reset();
        }
        juce::String uiStatus;
        juce::String alertTitle;
        juce::String alertBody;
        bool alertIsWarning = false;
        std::optional<carta::HiMdDeviceStatus> afterStatus;
        std::optional<carta::MdDisc> pendingLoadedDisc;
        bool pendingNeedsElev = false;
        {
            std::lock_guard<std::mutex> lk(hiMdMutex);
            uiStatus = hiMdPendingUiStatus;
            alertTitle = hiMdPendingAlertTitle;
            alertBody = hiMdPendingAlertBody;
            alertIsWarning = !hiMdLastOk;
            afterStatus = std::move(hiMdPendingStatus);
            hiMdPendingStatus.reset();
            pendingLoadedDisc = std::move(hiMdPendingLoadedDisc);
            hiMdPendingLoadedDisc.reset();
            pendingNeedsElev = hiMdPendingNeedsElevation;
            hiMdPendingNeedsElevation = false;
        }
        hiMdNeedsElevation = !hiMdLastOk && pendingNeedsElev;
        if (afterStatus.has_value()) {
            cachedHiMdStatus = afterStatus;
        }
        if (pendingLoadedDisc.has_value()) {
            juce::String applyErr;
            if (!applyLoadedMiniDisc(*pendingLoadedDisc, applyErr)) {
                uiStatus = applyErr;
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
        if (!currentDownloadingRegionId().empty()) {
            repaint();
        }
    }
}

void Studio::refreshRecordWorkspace(bool scanDevices, bool allowElevationPrompt) {
    const bool busy =
        netMdBusy.load(std::memory_order_acquire) || hiMdBusy.load(std::memory_order_acquire);
    if (scanDevices && !busy) {
        std::vector<carta::NetMdDeviceEntry> netDevs;
        std::string scanErr;
        if (carta::listNetMdDevices(netDevs, scanErr)) {
            cachedNetMdDevices = std::move(netDevs);
            netMdScannedOnce = true;
        } else {
            cachedNetMdDevices.clear();
            netMdScannedOnce = true;
        }

        std::vector<carta::HiMdDeviceEntry> hiDevs;
        std::string hiErr;
        if (carta::listHiMdDevices(hiDevs, hiErr)) {
            cachedHiMdDevices = std::move(hiDevs);
            hiMdScannedOnce = true;
        } else {
            cachedHiMdDevices.clear();
            hiMdScannedOnce = true;
        }

        if (cachedNetMdPreflight.has_value()) {
            const bool stillPresent = std::any_of(
                cachedNetMdDevices.begin(), cachedNetMdDevices.end(),
                [&](const auto& d) { return d.id == cachedNetMdPreflight->device.deviceId; });
            if (!stillPresent) {
                cachedNetMdPreflight.reset();
            }
        }
        if (cachedHiMdStatus.has_value()) {
            const bool stillPresent = std::any_of(
                cachedHiMdDevices.begin(), cachedHiMdDevices.end(),
                [&](const auto& d) { return d.id == cachedHiMdStatus->deviceId; });
            if (!stillPresent) {
                cachedHiMdStatus.reset();
            }
        }
    }

    std::string preferredId = discWorkspace.selectedRecordDeviceId().toStdString();
    bool preferredIsHiMd = discWorkspace.selectedRecordDeviceIsHiMd();
    const auto hasNetMdId = [&](const std::string& id) {
        return std::any_of(cachedNetMdDevices.begin(), cachedNetMdDevices.end(),
                           [&](const auto& d) { return d.id == id; });
    };
    const auto hasHiMdId = [&](const std::string& id) {
        return std::any_of(cachedHiMdDevices.begin(), cachedHiMdDevices.end(),
                           [&](const auto& d) { return d.id == id; });
    };
    if (preferredId.empty() ||
        (preferredIsHiMd ? !hasHiMdId(preferredId) : !hasNetMdId(preferredId))) {
        preferredId.clear();
        if (carta::isHiMdMedium(session.mdDisc.medium)) {
            if (!cachedHiMdDevices.empty()) {
                preferredId = cachedHiMdDevices.front().id;
                preferredIsHiMd = true;
            } else if (!cachedNetMdDevices.empty()) {
                preferredId = cachedNetMdDevices.front().id;
                preferredIsHiMd = false;
            }
        } else {
            if (!cachedNetMdDevices.empty()) {
                preferredId = cachedNetMdDevices.front().id;
                preferredIsHiMd = false;
            } else if (!cachedHiMdDevices.empty()) {
                preferredId = cachedHiMdDevices.front().id;
                preferredIsHiMd = true;
            }
        }
    }

    if (scanDevices && !preferredId.empty() && !busy) {
        if (preferredIsHiMd) {
            startHiMdProbeJob(preferredId, allowElevationPrompt);
        } else {
            startNetMdProbeJob(preferredId);
        }
        return;
    }
    if (!preferredIsHiMd || preferredId.empty() ||
        (cachedHiMdStatus.has_value() && cachedHiMdStatus->deviceId == preferredId)) {
        hiMdNeedsElevation = false;
    }

    const bool discIsHiMd = carta::isHiMdMedium(session.mdDisc.medium);
    carta::MdPhysicalPlan plan;
    std::string planErr;
    const bool planOk = carta::planMdPhysicalAudio(session.mdDisc, plan, planErr);
    carta::UtocDiscState utoc;
    std::string reason;
    const bool compiled = discIsHiMd ? false : carta::compileMdDiscToUtoc(session.mdDisc, utoc, reason);
    const std::string medium = carta::mdMediumToString(session.mdDisc.medium);
    const int physTotal = session.mdDisc.loadedDisc.active
                              ? session.mdDisc.loadedDisc.physicalTotalClusters()
                              : 0;
    const auto dot = juce::String::fromUTF8(" \xc2\xb7 ");
    const auto budget =
        compiled ? carta::computeUtocBudget(utoc, medium, physTotal) : carta::UtocBudget{};

    juce::String utocSummary;
    if (discIsHiMd) {
        const auto estPre = carta::preflightHiMdRecording(session.mdDisc, nullptr);
        const double estMb = static_cast<double>(estPre.requiredBytes) / (1024.0 * 1024.0);
        const juce::String trkStr =
            (estPre.uniqueAudioTracksToUpload > 0 &&
             estPre.uniqueAudioTracksToUpload < estPre.tracksToUpload)
                ? (juce::String(estPre.tracksToUpload) + " trk (" +
                   juce::String(estPre.uniqueAudioTracksToUpload) + " uniq)")
                : (juce::String(static_cast<int>(session.mdDisc.tracks.size())) + " trk");
        utocSummary = "Hi-MD FAT16" + dot + trkStr + dot +
                      juce::String(estMb, estMb >= 100.0 ? 0 : 1) + " MB";
    } else if (compiled) {
        utocSummary = juce::String(budget.usedTracks) + "/" + juce::String(budget.maxTracks) +
                      " trk" + dot + juce::String(budget.usedFragments) + "/" +
                      juce::String(budget.maxFragments) + " frag" + dot +
                      juce::String(budget.usedHalfWidthCells) + " HW" + dot +
                      juce::String(budget.usedFullWidthCells) + " FW" +
                      (budget.fits() ? "" : " (OVER)");
    } else {
        utocSummary = "UTOC error: " + juce::String::fromUTF8(reason.c_str());
    }

    MdWorkspace::RecordUiSnapshot snap;
    snap.devices = cachedNetMdDevices;
    snap.hiMdDevices = cachedHiMdDevices;
    snap.activeDeviceId = preferredId;
    snap.activeDeviceIsHiMd = preferredIsHiMd;
    snap.utocSummaryText = utocSummary;
    snap.busy = busy;
    {
        std::lock_guard<std::mutex> lk(netMdMutex);
        snap.downloadingAudio =
            (netMdBusy.load(std::memory_order_acquire) && netMdActiveOp == "download-audio");
    }
    if (!snap.downloadingAudio) {
        std::lock_guard<std::mutex> lk(hiMdMutex);
        snap.downloadingAudio =
            (hiMdBusy.load(std::memory_order_acquire) && hiMdActiveOp == "download-audio");
    }
    if (hiMdBusy.load(std::memory_order_acquire)) {
        const auto hp = carta::getHiMdProgress();
        snap.progress.active = true;
        snap.progress.operation = hp.operation;
        snap.progress.phase = hp.phase;
        snap.progress.progress = hp.progress;
        snap.progress.bytesDone = hp.bytesDone;
        snap.progress.bytesTotal = hp.bytesTotal;
    } else {
        snap.progress = carta::getNetMdProgress();
    }
    snap.badgeText = "No USB device";
    snap.statusText =
        (discIsHiMd || compiled)
            ? "Connect a Sony NetMD / Hi-MD recorder via USB and click Refresh."
            : juce::String::fromUTF8(reason.c_str());

    if (preferredIsHiMd) {
        if (cachedHiMdStatus.has_value() && cachedHiMdStatus->deviceId == preferredId) {
            const auto& st = *cachedHiMdStatus;
            snap.hasHiMdStatus = true;
            snap.hiMdStatus = st;
            const auto pre = carta::preflightHiMdRecording(session.mdDisc, &st);
            const juce::String vid =
                st.inquiryVid.empty() ? "SONY" : juce::String::fromUTF8(st.inquiryVid.c_str());
            const juce::String pid =
                st.inquiryPid.empty() ? "Hi-MD" : juce::String::fromUTF8(st.inquiryPid.c_str());
            snap.badgeText = "Hi-MD USB" + dot + vid + " " + pid + dot +
                             juce::String(st.trackCount) + " trk" + dot + "DATANUM " +
                             juce::String(st.datanum);
            const bool loadedActive = session.mdDisc.loadedDisc.active;
            snap.canLoadDisc = true;
            snap.canDownloadAudio = discIsHiMd && loadedActive && !st.tracks.empty() &&
                                    hasUnloadedOnDiscPlaceholders();
            snap.canDumpTrack = !st.tracks.empty();
            snap.canErase = true;
            snap.canManageHiMd = true;
            snap.canRecord = discIsHiMd && pre.valid &&
                             (loadedActive || st.trackCount == 0) &&
                             (!session.mdDisc.tracks.empty() || loadedActive) &&
                             pendingImports.empty();

            if (!discIsHiMd) {
                snap.statusText =
                    "Switch Target Medium to Hi-MD 300M or Hi-MD 1GB to record to this Hi-MD "
                    "device.";
            } else if (!loadedActive && st.trackCount > 0) {
                snap.statusText = "Hi-MD disc has " + juce::String(st.trackCount) +
                                  " track(s) — click Load Disc, Dump Track..., Disc Tools..., or "
                                  "Wipe Disc...";
            } else if (session.mdDisc.tracks.empty() && !loadedActive) {
                snap.statusText = "Hi-MD ready (" + juce::String::fromUTF8(st.deviceName.c_str()) +
                                  "). Add tracks or click Load Disc.";
            } else if (!pre.valid) {
                snap.statusText = juce::String::fromUTF8(pre.error.c_str());
            } else if (loadedActive) {
                const double reqMb =
                    static_cast<double>(pre.requiredBytes) / (1024.0 * 1024.0);
                if (pre.newUniqueAudioTracks == 0) {
                    snap.statusText = "Ready to update Hi-MD Play Order/index (preserving " +
                                      juce::String(pre.preservedOnDiscTracks) +
                                      " on-disc track(s), 0 new audio).";
                } else {
                    snap.statusText = "Ready (preserve " +
                                      juce::String(pre.preservedOnDiscTracks) +
                                      " on-disc, upload " +
                                      juce::String(pre.newUniqueAudioTracks) +
                                      " new unique audio, " +
                                      juce::String(reqMb, reqMb >= 100.0 ? 0 : 1) + " MB).";
                }
            } else {
                const double reqMb =
                    static_cast<double>(pre.requiredBytes) / (1024.0 * 1024.0);
                snap.statusText =
                    (pre.uniqueAudioTracksToUpload < pre.tracksToUpload)
                        ? ("Ready to record " + juce::String(pre.tracksToUpload) +
                           " Play Order track(s) (" + juce::String(pre.uniqueAudioTracksToUpload) +
                           " unique audio, " + juce::String(reqMb, reqMb >= 100.0 ? 0 : 1) +
                           " MB) to Hi-MD.")
                        : ("Ready to record " + juce::String(pre.tracksToUpload) + " track(s) (" +
                           juce::String(reqMb, reqMb >= 100.0 ? 0 : 1) + " MB) to Hi-MD.");
            }
        } else if (!cachedHiMdDevices.empty()) {
            if (hiMdNeedsElevation) {
                snap.hiMdNeedsElevation = true;
                snap.badgeText = "Hi-MD locked (admin required)";
                snap.statusText = "Administrator privileges required for Hi-MD USB access. Click "
                                  "'Unlock...' to authorize once for this session.";
            } else {
                snap.badgeText = "Hi-MD found (not probed)";
                snap.statusText = "Click Refresh to probe USB Hi-MD recorder & FAT16 filesystem.";
            }
        }
    } else if (cachedNetMdPreflight.has_value() && cachedNetMdPreflight->device.deviceId == preferredId) {
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
            snap.canDownloadAudio = !discIsHiMd && loadedActive && st.discPresent &&
                                    !st.hiMdFormatted && st.supportsAtracRecovery &&
                                    hasUnloadedOnDiscPlaceholders();
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

            if (discIsHiMd) {
                snap.statusText =
                    "Switch Target Medium to MD 60/74/80 to record via NetMD, or select a Hi-MD "
                    "device.";
            } else if (!st.discPresent) {
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
    if (selectedId.isEmpty() || netMdBusy.load() || hiMdBusy.load()) {
        refreshRecordWorkspace(false);
        return;
    }
    if (discWorkspace.selectedRecordDeviceIsHiMd()) {
        startHiMdProbeJob(selectedId.toStdString(), false);
    } else {
        startNetMdProbeJob(selectedId.toStdString());
    }
}

void Studio::triggerReadDiscTocFromUi() {
    if (netMdBusy.load() || hiMdBusy.load()) {
        status.setText("Wait for the current MiniDisc operation to finish.",
                       juce::dontSendNotification);
        return;
    }

    if (discWorkspace.selectedRecordDeviceIsHiMd()) {
        triggerHiMdDumpTrackFromUi();
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
    if (netMdBusy.load() || hiMdBusy.load()) {
        status.setText("Wait for the current MiniDisc operation to finish.",
                       juce::dontSendNotification);
        return;
    }

    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty()) {
        status.setText("Select or refresh a MiniDisc device first.", juce::dontSendNotification);
        return;
    }

    const bool isHiMd = discWorkspace.selectedRecordDeviceIsHiMd();
    const std::string devId = selectedId.toStdString();
    const bool hasExistingSession =
        dirty || !session.tracks.empty() || !carta::isDefaultMdDisc(session.mdDisc);
    if (!hasExistingSession) {
        if (isHiMd) {
            startHiMdLoadDiscJob(devId, true);
        } else {
            startNetMdLoadDiscJob(devId, true);
        }
        return;
    }

    showGuardedStudioAlert(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle(isHiMd ? "Load Hi-MD into editor" : "Load MiniDisc into editor")
            .withMessage((isHiMd ? "Load Hi-MD disc from '" : "Load MiniDisc UTOC from '") +
                         selectedId +
                         "' into the editor? This replaces the current timeline (Undo restores "
                         "it).")
            .withButton("Load Disc")
            .withButton("Cancel")
            .withAssociatedComponent(this),
        [safe = juce::Component::SafePointer<Studio>(this), devId, isHiMd](int answer) {
            if (safe == nullptr || answer != 0 || safe->netMdBusy.load() || safe->hiMdBusy.load()) {
                return;
            }
            if (isHiMd) {
                safe->startHiMdLoadDiscJob(devId, true);
            } else {
                safe->startNetMdLoadDiscJob(devId, true);
            }
        });
}

void Studio::triggerDownloadDiscAudioFromUi(std::vector<std::string> regionIds,
                                            std::optional<int> onlyUtocTrackIndex0,
                                            std::vector<DownloadTargetClip> clipTargets) {
    if (netMdBusy.load() || hiMdBusy.load()) {
        status.setText("Wait for the current MiniDisc operation to finish.",
                       juce::dontSendNotification);
        return;
    }
    if (!onlyUtocTrackIndex0.has_value() && clipTargets.empty() &&
        !hasUnloadedOnDiscPlaceholders()) {
        status.setText("All on-disc audio regions are already downloaded.",
                       juce::dontSendNotification);
        return;
    }

    const bool discIsHiMd = carta::isHiMdMedium(session.mdDisc.medium);
    std::string devId = session.mdDisc.loadedDisc.sourceDeviceId;
    if (discIsHiMd) {
        if (discWorkspace.selectedRecordDeviceIsHiMd() &&
            discWorkspace.selectedRecordDeviceId().isNotEmpty()) {
            devId = discWorkspace.selectedRecordDeviceId().toStdString();
        } else if (cachedHiMdStatus.has_value() && !cachedHiMdStatus->deviceId.empty()) {
            devId = cachedHiMdStatus->deviceId;
        } else if (!cachedHiMdDevices.empty()) {
            devId = cachedHiMdDevices.front().id;
        }
        if (devId.empty()) {
            status.setText("Connect a Hi-MD device and click Refresh first.",
                           juce::dontSendNotification);
            return;
        }
        if (regionIds.empty() && !clipTargets.empty()) {
            for (const auto& t : clipTargets) {
                regionIds.push_back(t.regionId);
            }
        }
        startHiMdDownloadAudioJob(devId, std::move(regionIds), true);
    } else {
        if (!discWorkspace.selectedRecordDeviceIsHiMd() &&
            discWorkspace.selectedRecordDeviceId().isNotEmpty()) {
            devId = discWorkspace.selectedRecordDeviceId().toStdString();
        } else if (cachedNetMdPreflight.has_value() && cachedNetMdPreflight->deviceConnected &&
                   !cachedNetMdPreflight->device.deviceId.empty()) {
            devId = cachedNetMdPreflight->device.deviceId;
        } else if (!cachedNetMdDevices.empty()) {
            devId = cachedNetMdDevices.front().id;
        }
        if (devId.empty()) {
            status.setText("Connect a NetMD device and click Refresh first.",
                           juce::dontSendNotification);
            return;
        }
        startNetMdDownloadAudioJob(devId, std::move(regionIds), true, onlyUtocTrackIndex0,
                                   std::move(clipTargets));
    }
}

void Studio::cancelActiveDiscDownload() {
    if (!netMdBusy.load(std::memory_order_acquire) && !hiMdBusy.load(std::memory_order_acquire)) {
        return;
    }
    netMdStopRequested.store(true, std::memory_order_release);
    carta::requestNetMdDownloadCancel();
    status.setText("Cancelling disc audio download...", juce::dontSendNotification);
    repaint();
}

void Studio::triggerEraseDiscFromUi() {
    if (netMdBusy.load() || hiMdBusy.load()) {
        status.setText("Wait for the current MiniDisc operation to finish.",
                       juce::dontSendNotification);
        return;
    }

    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty()) {
        status.setText("Select or refresh a MiniDisc device first.", juce::dontSendNotification);
        return;
    }

    const std::string devId = selectedId.toStdString();
    if (discWorkspace.selectedRecordDeviceIsHiMd()) {
        showGuardedStudioAlert(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle("Wipe Hi-MD disc")
                .withMessage("Erase all tracks on Hi-MD disc '" + selectedId +
                             "' or perform a full FAT16 reformat?")
                .withButton("Wipe Tracks")
                .withButton("Reformat FAT16")
                .withButton("Cancel")
                .withAssociatedComponent(this),
            [safe = juce::Component::SafePointer<Studio>(this), devId](int answer) {
                if (safe == nullptr || (answer != 0 && answer != 1) || safe->netMdBusy.load() ||
                    safe->hiMdBusy.load()) {
                    return;
                }
                safe->startHiMdWipeJob(devId, answer == 1, true, true);
            });
        return;
    }

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
                               if (safe == nullptr || answer != 0 || safe->netMdBusy.load() ||
                                   safe->hiMdBusy.load()) {
                                   return;
                               }
                               safe->startNetMdEraseJob(devId, true);
                           });
}

void Studio::triggerRecordWriteUtocFromUi() {
    discWorkspace.commitPendingEdits();
    if (netMdBusy.load() || hiMdBusy.load()) {
        status.setText("Wait for the current MiniDisc operation to finish.",
                       juce::dontSendNotification);
        return;
    }

    if (discWorkspace.selectedRecordDeviceIsHiMd()) {
        triggerHiMdDiscToolsMenuFromUi();
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
            if (safe == nullptr || answer != 0 || safe->netMdBusy.load() ||
                safe->hiMdBusy.load()) {
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
    discWorkspace.commitPendingEdits();
    if (netMdBusy.load() || hiMdBusy.load()) {
        status.setText("Wait for the current MiniDisc operation to finish.",
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
        status.setText("Select or refresh a MiniDisc device first.", juce::dontSendNotification);
        return;
    }

    const std::string devId = selectedId.toStdString();
    if (discWorkspace.selectedRecordDeviceIsHiMd()) {
        carta::MdDisc discCopy;
        {
            const juce::ScopedLock guard(lock);
            discCopy = session.mdDisc;
        }
        const auto pre = carta::preflightHiMdRecording(
            discCopy, cachedHiMdStatus.has_value() ? &(*cachedHiMdStatus) : nullptr);
        const double reqMb = static_cast<double>(pre.requiredBytes) / (1024.0 * 1024.0);
        juce::String promptMsg;
        if (discCopy.loadedDisc.active) {
            if (pre.newUniqueAudioTracks == 0) {
                promptMsg = "Update Hi-MD index on '" + selectedId + "' (preserving " +
                            juce::String(pre.preservedOnDiscTracks) +
                            " on-disc track(s), 0 new uploads)?";
            } else {
                promptMsg = "Preserve " + juce::String(pre.preservedOnDiscTracks) +
                            " on-disc track(s) and upload " +
                            juce::String(pre.newUniqueAudioTracks) + " new unique audio item(s) (" +
                            juce::String(reqMb, reqMb >= 100.0 ? 0 : 1) +
                            " MB) to Hi-MD device '" + selectedId + "'?";
            }
        } else if (pre.uniqueAudioTracksToUpload < pre.tracksToUpload) {
            promptMsg = "Encode and upload " + juce::String(pre.uniqueAudioTracksToUpload) +
                        " unique audio item(s) for " + juce::String(pre.tracksToUpload) +
                        " Play Order track(s) (" +
                        juce::String(reqMb, reqMb >= 100.0 ? 0 : 1) + " MB) to Hi-MD device '" +
                        selectedId + "'?";
        } else {
            promptMsg = "Encode and upload " + juce::String(pre.tracksToUpload) + " track(s) (" +
                        juce::String(reqMb, reqMb >= 100.0 ? 0 : 1) + " MB) to Hi-MD device '" +
                        selectedId + "'?";
        }
        showGuardedStudioAlert(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle("Record Hi-MD")
                .withMessage(promptMsg)
                .withButton("Record Hi-MD")
                .withButton("Cancel")
                .withAssociatedComponent(this),
            [safe = juce::Component::SafePointer<Studio>(this), devId](int answer) {
                if (safe == nullptr || answer != 0 || safe->netMdBusy.load() ||
                    safe->hiMdBusy.load() || !safe->pendingImports.empty()) {
                    return;
                }
                carta::Session snapshot;
                {
                    const juce::ScopedLock guard(safe->lock);
                    safe->copyWorkerSnapshot(snapshot);
                }
                safe->startHiMdRecordJob(std::move(snapshot), devId, true, true);
            });
        return;
    }

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
                                   safe->hiMdBusy.load() || !safe->pendingImports.empty()) {
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

void Studio::triggerHiMdDumpTrackFromUi(int trackIndex) {
    if (netMdBusy.load() || hiMdBusy.load()) {
        status.setText("Wait for the current MiniDisc operation to finish.",
                       juce::dontSendNotification);
        return;
    }
    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty() || !discWorkspace.selectedRecordDeviceIsHiMd() ||
        !cachedHiMdStatus.has_value() || cachedHiMdStatus->tracks.empty()) {
        status.setText("Select a probed Hi-MD disc with tracks first.", juce::dontSendNotification);
        return;
    }

    int targetIdx = trackIndex >= 0 ? trackIndex : discWorkspace.selectedDeviceTrackIndex0();
    if (targetIdx < 0 || targetIdx >= static_cast<int>(cachedHiMdStatus->tracks.size())) {
        targetIdx = 0;
    }
    const auto& tr = cachedHiMdStatus->tracks[static_cast<std::size_t>(targetIdx)];
    const bool isPcm = (tr.encoding == "PCM" || tr.encoding == "LPCM");
    const juce::String ext = isPcm ? ".wav" : ".oma";
    const juce::String baseTitle =
        tr.title.empty() ? ("track_" + juce::String(targetIdx + 1))
                         : juce::File::createLegalFileName(juce::String::fromUTF8(tr.title.c_str()));
    const juce::File suggested =
        juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile(baseTitle + ext);
    const std::string devId = selectedId.toStdString();

    launchStudioChooser(
        "Dump Hi-MD track " + juce::String(targetIdx + 1), suggested, isPcm ? "*.wav" : "*.oma",
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<Studio>(this), devId,
         targetIdx](const StudioFileSelection& fc) {
            if (safe == nullptr) {
                return;
            }
            const auto outFile = fc.getResult();
            if (outFile == juce::File{} || safe->netMdBusy.load() || safe->hiMdBusy.load()) {
                return;
            }
            safe->startHiMdDumpJob(devId, targetIdx, outFile, false, true);
        });
}

void Studio::triggerHiMdDiscToolsMenuFromUi() {
    if (netMdBusy.load() || hiMdBusy.load() || !discWorkspace.selectedRecordDeviceIsHiMd()) {
        return;
    }
    const int selTrack = discWorkspace.selectedDeviceTrackIndex0();
    const int nTracks =
        cachedHiMdStatus.has_value() ? static_cast<int>(cachedHiMdStatus->tracks.size()) : 0;
    const bool hasTracks = selTrack >= 0 && selTrack < nTracks;
    bool canWriteIndex = session.mdDisc.loadedDisc.active &&
                         carta::isHiMdMedium(session.mdDisc.medium);
    if (canWriteIndex) {
        for (const auto& tr : session.mdDisc.tracks) {
            if (!carta::mdTrackUsesOnDiscAudio(session.mdDisc, tr)) {
                canWriteIndex = false;
                break;
            }
        }
    }

    juce::PopupMenu menu;
    menu.addItem(1, "Rename Hi-MD Disc...");
    menu.addItem(2, "Sync Session Groups to Hi-MD", !session.mdDisc.groups.empty() && nTracks > 0);
    menu.addItem(8, "Write Session Index Only...", canWriteIndex);
    menu.addSeparator();
    menu.addItem(3, "Edit Selected Track Metadata...", hasTracks);
    menu.addItem(4, "Move Selected Track Up", hasTracks && selTrack > 0);
    menu.addItem(5, "Move Selected Track Down", hasTracks && selTrack + 1 < nTracks);
    menu.addItem(6, "Delete Selected Track...", hasTracks);
    menu.addItem(7, "Dump Selected Track...", hasTracks);

    showStudioPopupMenu(menu, juce::PopupMenu::Options().withTargetComponent(this),
                        [safe = juce::Component::SafePointer<Studio>(this), selTrack](int choice) {
                           if (safe == nullptr || choice <= 0) {
                               return;
                           }
                           if (choice == 1) {
                               safe->triggerHiMdRenameDiscFromUi();
                           } else if (choice == 2) {
                               safe->triggerHiMdSyncGroupsFromUi();
                           } else if (choice == 8) {
                               safe->triggerHiMdWriteIndexFromUi();
                           } else if (choice == 3) {
                               safe->triggerHiMdRenameTrackFromUi(selTrack);
                           } else if (choice == 4) {
                               safe->triggerHiMdMoveTrackFromUi(selTrack, -1);
                           } else if (choice == 5) {
                               safe->triggerHiMdMoveTrackFromUi(selTrack, 1);
                           } else if (choice == 6) {
                               safe->triggerHiMdDeleteTrackFromUi(selTrack);
                           } else if (choice == 7) {
                               safe->triggerHiMdDumpTrackFromUi(selTrack);
                           }
                       });
}

void Studio::triggerHiMdRenameDiscFromUi() {
    if (netMdBusy.load() || hiMdBusy.load() || !discWorkspace.selectedRecordDeviceIsHiMd()) {
        return;
    }
    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty()) {
        return;
    }
    const juce::String curTitle =
        cachedHiMdStatus.has_value()
            ? juce::String::fromUTF8(cachedHiMdStatus->discTitle.c_str())
            : juce::String();
    const std::string devId = selectedId.toStdString();
    showTextPrompt("Rename Hi-MD Disc", "Enter new Hi-MD disc title:", "title", "Disc title",
                   curTitle, "Rename",
                   [safe = juce::Component::SafePointer<Studio>(this),
                    devId](bool accepted, const juce::String& value) {
                       if (safe == nullptr || !accepted || safe->netMdBusy.load() ||
                           safe->hiMdBusy.load()) {
                           return;
                       }
                       safe->startHiMdRenameDiscJob(devId, value.toStdString(), true, true);
                   });
}

void Studio::triggerHiMdRenameTrackFromUi(int trackIndex) {
    if (netMdBusy.load() || hiMdBusy.load() || !discWorkspace.selectedRecordDeviceIsHiMd() ||
        !cachedHiMdStatus.has_value() || trackIndex < 0 ||
        trackIndex >= static_cast<int>(cachedHiMdStatus->tracks.size())) {
        return;
    }
    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty()) {
        return;
    }
    const auto& tr = cachedHiMdStatus->tracks[static_cast<std::size_t>(trackIndex)];
    const std::string devId = selectedId.toStdString();
    showTextFieldsPrompt(
        "Edit Hi-MD Track " + juce::String(trackIndex + 1),
        "Update UTF-16BE track metadata directly on the Hi-MD disc.",
        {{"title", "Title", juce::String::fromUTF8(tr.title.c_str())},
         {"artist", "Artist", juce::String::fromUTF8(tr.artist.c_str())},
         {"album", "Album", juce::String::fromUTF8(tr.album.c_str())}},
        "Apply",
        [safe = juce::Component::SafePointer<Studio>(this), devId,
         trackIndex](bool accepted, const juce::StringArray& values) {
            if (safe == nullptr || !accepted || values.size() != 3 || safe->netMdBusy.load() ||
                safe->hiMdBusy.load()) {
                return;
            }
            safe->startHiMdRenameTrackJob(devId, trackIndex, true, values[0].toStdString(), true,
                                          values[1].toStdString(), true, values[2].toStdString(),
                                          true, true);
        });
}

void Studio::triggerHiMdMoveTrackFromUi(int trackIndex, int delta) {
    if (netMdBusy.load() || hiMdBusy.load() || !discWorkspace.selectedRecordDeviceIsHiMd() ||
        !cachedHiMdStatus.has_value()) {
        return;
    }
    const int nTracks = static_cast<int>(cachedHiMdStatus->tracks.size());
    const int dest = trackIndex + delta;
    if (trackIndex < 0 || trackIndex >= nTracks || dest < 0 || dest >= nTracks) {
        return;
    }
    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty()) {
        return;
    }
    startHiMdMoveTrackJob(selectedId.toStdString(), trackIndex, dest, true, true);
}

void Studio::triggerHiMdDeleteTrackFromUi(int trackIndex) {
    if (netMdBusy.load() || hiMdBusy.load() || !discWorkspace.selectedRecordDeviceIsHiMd() ||
        !cachedHiMdStatus.has_value() || trackIndex < 0 ||
        trackIndex >= static_cast<int>(cachedHiMdStatus->tracks.size())) {
        return;
    }
    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty()) {
        return;
    }
    const auto& tr = cachedHiMdStatus->tracks[static_cast<std::size_t>(trackIndex)];
    const std::string devId = selectedId.toStdString();
    showGuardedStudioAlert(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle("Delete Hi-MD Track")
            .withMessage("Delete track " + juce::String(trackIndex + 1) +
                         (tr.title.empty() ? juce::String()
                                           : (" ('" + juce::String::fromUTF8(tr.title.c_str()) +
                                              "')")) +
                         " from Hi-MD device '" + selectedId + "'?")
            .withButton("Delete Track")
            .withButton("Cancel")
            .withAssociatedComponent(this),
        [safe = juce::Component::SafePointer<Studio>(this), devId, trackIndex](int answer) {
            if (safe == nullptr || answer != 0 || safe->netMdBusy.load() || safe->hiMdBusy.load()) {
                return;
            }
            safe->startHiMdDeleteTracksJob(devId, {trackIndex}, true, true);
        });
}

void Studio::triggerHiMdSyncGroupsFromUi() {
    if (netMdBusy.load() || hiMdBusy.load() || !discWorkspace.selectedRecordDeviceIsHiMd() ||
        !cachedHiMdStatus.has_value()) {
        return;
    }
    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty()) {
        return;
    }
    startHiMdSetGroupsJob(selectedId.toStdString(), session.mdDisc.groups,
                          cachedHiMdStatus->trackCount, true, true);
}

void Studio::triggerHiMdWriteIndexFromUi() {
    if (netMdBusy.load() || hiMdBusy.load() || !discWorkspace.selectedRecordDeviceIsHiMd()) {
        status.setText("Wait for the current MiniDisc operation to finish.",
                       juce::dontSendNotification);
        return;
    }
    const auto selectedId = discWorkspace.selectedRecordDeviceId();
    if (selectedId.isEmpty()) {
        status.setText("Select or refresh a Hi-MD device first.", juce::dontSendNotification);
        return;
    }
    const std::string devId = selectedId.toStdString();
    showGuardedStudioAlert(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle("Write Hi-MD Session Index")
            .withMessage("Write edited Play Order, titles, metadata, and groups from the "
                         "current session to Hi-MD device '" +
                         selectedId +
                         "' without re-encoding preserved audio? Unreferenced deleted tracks "
                         "will be freed.")
            .withButton("Write Index")
            .withButton("Cancel")
            .withAssociatedComponent(this),
        [safe = juce::Component::SafePointer<Studio>(this), devId](int answer) {
            if (safe == nullptr || answer != 0 || safe->netMdBusy.load() ||
                safe->hiMdBusy.load()) {
                return;
            }
            carta::Session snapshot;
            {
                const juce::ScopedLock guard(safe->lock);
                safe->copyWorkerSnapshot(snapshot);
            }
            safe->startHiMdWriteIndexJob(snapshot, devId, true, true);
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
