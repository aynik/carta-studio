/**
 * @file StudioProjectIO.cpp
 * @brief Asynchronous audio import, WAV export, source verification, marker exchange, and project
 * I/O.
 *
 * Implements message-thread validation, background worker dispatch, and
 * generation-checked completion callbacks for:
 * - Audio file decoding & timeline import (`importFile`, `commitReadyImport`, `finishReadyImports`)
 * - Source file checksum verification & relinking (`verifySelectedSources`,
 * `startSourceReplacement`)
 * - Marker JSON & Audacity label exchange (`startMarkerExchange`, `importMarkers`, `exportMarkers`)
 * - WAV preflight/writing (`exportWav`, `exportAllTracks`, `beginExport`, `writeExport`,
 * `exportManifest`)
 * - Project open/save/portable-copy/recovery (`saveProject`, `beginProjectOpen`, `maybeAutosave`,
 * etc.)
 *
 * Threading and media-retention safety:
 * - All `active*` task handles and `pendingImports` live on the JUCE message thread.
 * - `validateRetainedMedia` prevents overwriting any source audio file currently
 *   referenced by the live session, undo/redo history, clipboard, or active jobs.
 * - Background jobs verify `editGeneration`, `dragMode == kDragNone`, and modal
 *   state on the message thread before publishing changes to `session`.
 */

#include "Studio/Studio.h"

namespace {

const char* sourceStatusToIpcString(carta::SourceStatus status) noexcept {
    switch (status) {
    case carta::SourceStatus::matches:
        return "matches";
    case carta::SourceStatus::changed:
        return "changed";
    case carta::SourceStatus::missing:
        return "missing";
    case carta::SourceStatus::unverifiable:
        return "unverifiable";
    case carta::SourceStatus::changedDuringCheck:
        return "changed_during_check";
    case carta::SourceStatus::unreadable:
        return "unreadable";
    }
    return "unreadable";
}

juce::String formatWavEncodingDescription(carta::WavEncoding encoding) {
    if (encoding == carta::WavEncoding::float32) {
        return "32-bit float";
    }
    if (encoding == carta::WavEncoding::pcm16) {
        return "16-bit PCM";
    }
    return "24-bit PCM";
}

const char* directorySyncToIpcString(carta::DirectorySyncStatus status) noexcept {
    if (status == carta::DirectorySyncStatus::confirmed) {
        return "confirmed";
    }
    if (status == carta::DirectorySyncStatus::unconfirmed) {
        return "unconfirmed";
    }
    return "unsupported";
}

bool sessionHasPackageMedia(const carta::Session& s) noexcept {
    return std::any_of(s.tracks.begin(), s.tracks.end(), [](const auto& track) {
        return std::any_of(track.clips.begin(), track.clips.end(),
                           [](const auto& clip) { return clip.audio && clip.audio->sourceOwner; });
    });
}

} // namespace

void Studio::copyWorkerSnapshot(carta::Session& snapshot) {
    const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
    if (ioSnapshotHook) {
        ioSnapshotHook();
    }
#endif
    snapshot = session;
}

bool Studio::validateRetainedMedia(const juce::File& target, juce::String& error) {
    // These paths, history and task pointers are message-thread-owned;
    // destination and filesystem checks need not block playback.
    if (carta::pathsReferToSameFile(recoveryFile, target)) {
        error = "Choose an output other than the active recovery snapshot.";
        return false;
    }
    if (activeOpen) {
        error = "Wait for project opening to finish before writing a file.";
        return false;
    }
    if (activeMarkerExchange && carta::pathsReferToSameFile(activeMarkerExchange->file, target)) {
        error = "Choose an output other than the active marker exchange file.";
        return false;
    }
    std::vector<juce::File> reading;
    for (const auto& pending : pendingImports) {
        reading.push_back(pending->file);
    }
    if (activeSourceCheck) {
        for (const auto& source : activeSourceCheck->sources) {
            if (source.audio) {
                reading.push_back(source.audio->file);
            }
        }
        if (activeSourceCheck->relinking) {
            reading.push_back(activeSourceCheck->relinkFile);
        }
    }
    return carta::validateRetainedMediaDestination(
        session, editHistory.undoSessions(), editHistory.redoSessions(), clipboard, target, error,
        {activeSave ? &activeSave->snapshot : nullptr,
         activeExport ? &activeExport->snapshot : nullptr,
         activeExport ? &activeExport->verificationSnapshot : nullptr,
         activeRecovery ? &activeRecovery->snapshot : nullptr,
         activeLevelAnalysis ? &activeLevelAnalysis->snapshot : nullptr,
         activeMarkerExchange ? &activeMarkerExchange->snapshot : nullptr},
        reading);
}

bool Studio::startMarkerExchange(const juce::File& file, bool writing, bool audacity, bool ipc,
                                 juce::String& error) {
    if (activeMarkerExchange) {
        error = "Wait for the current marker exchange to finish.";
        return false;
    }
    if (activeOpen || !pendingImports.empty()) {
        error = "Wait for loading to finish before exchanging markers.";
        return false;
    }
    if (carta::projectEndSample(session) <= 0) {
        error = "Import audio before exchanging markers.";
        return false;
    }
    if ((activeSave && carta::pathsReferToSameFile(activeSave->file, file)) ||
        (activeExport && carta::pathsReferToSameFile(activeExport->target, file)) ||
        (activeRecovery && carta::pathsReferToSameFile(activeRecovery->file, file))) {
        error = "Choose a different file while another output is being written.";
        return false;
    }
    if (writing) {
        if (!file.hasFileExtension(audacity ? "txt" : "json")) {
            error = audacity ? "Choose a filename ending in .txt."
                             : "Choose a filename ending in .json.";
            return false;
        }
        if (ipc && file.exists()) {
            error = "Marker export destination already exists.";
            return false;
        }
        if (!validateRetainedMedia(file, error) ||
            !carta::validateOutputDestination(session, file, projectFile, error)) {
            return false;
        }
    } else if (!file.existsAsFile()) {
        error = "Marker input file does not exist.";
        return false;
    }

    auto task = std::make_shared<MarkerExchangeTask>();
    task->file = file;
    task->snapshot = session;
    task->generation = editGeneration;
    task->writing = writing;
    task->audacity = audacity;
    task->ipc = ipc;
    activeMarkerExchange = task;

    if (ipc) {
        lastIpcMarkerExchangeGeneration = task->generation;
        auto* info = new juce::DynamicObject();
        info->setProperty("state", "running");
        info->setProperty("operation", writing ? "export" : "import");
        info->setProperty("format", audacity ? "labels" : "json");
        info->setProperty("path", file.getFullPathName());
        info->setProperty("stale", false);
        info->setProperty("cancellation_requested", false);
        if (writing) {
            info->setProperty("file_written", false);
        }
        lastIpcMarkerExchange = juce::var(info);
    }

    const auto protectedProject = projectFile;
    markerExchangePool.addJob([safe = juce::Component::SafePointer<Studio>(this), task,
                               protectedProject] {
        auto markers = std::make_shared<std::vector<carta::Marker>>();
        auto reconciliation = std::make_shared<carta::MarkerImportReconciliation>();
        juce::String problem;
        bool ok = false;
        try {
            const auto keepGoing = [task] { return !task->cancelled.load(); };
            if (task->writing) {
                ok = task->audacity
                         ? carta::writeAudacityLabels(task->snapshot, task->file, problem,
                                                      protectedProject, task->ipc, keepGoing)
                         : carta::writeMarkerFile(task->snapshot, task->file, problem,
                                                  protectedProject, task->ipc, keepGoing);
            } else {
                ok = task->audacity ? carta::readAudacityLabels(task->snapshot, task->file,
                                                                *markers, problem, keepGoing)
                                    : carta::readMarkerFile(task->snapshot, task->file, *markers,
                                                            problem, keepGoing);
                if (ok && keepGoing()) {
                    *reconciliation =
                        carta::reconcileImportedMarkers(task->snapshot.markers, *markers);
                }
            }
        } catch (const std::exception& e) {
            problem = "Marker exchange failed: " + juce::String(e.what());
        } catch (...) {
            problem = "Marker exchange failed.";
        }

        juce::MessageManager::callAsync([safe, task, markers, reconciliation, ok, problem] {
            if (safe == nullptr || safe->activeMarkerExchange != task) {
                return;
            }
            bool success = ok;
            bool changed = false;
            juce::String failure = problem;
            const auto count = task->writing ? task->snapshot.markers.size() : markers->size();
            if (success && !task->writing && task->cancelled.load()) {
                success = false;
                failure = "Marker exchange cancelled.";
            }
            if (success && !task->writing) {
                if (safe->editGeneration != task->generation || safe->dragMode != kDragNone ||
                    safe->activeOpen || !safe->pendingImports.empty() || safe->savePromptActive ||
                    juce::Component::getCurrentlyModalComponent() != nullptr) {
                    success = false;
                    failure =
                        "Project or editor state changed during marker import; no markers were "
                        "replaced.";
                } else {
                    const auto& current = safe->session.markers;
                    changed = current.size() != markers->size();
                    if (!changed) {
                        for (size_t i = 0; i < markers->size(); ++i) {
                            if (current[i].name != (*markers)[i].name ||
                                std::abs(current[i].position - (*markers)[i].position) > 1e-12) {
                                changed = true;
                                break;
                            }
                        }
                    }
                    if (changed) {
                        const juce::ScopedLock guard(safe->lock);
                        safe->stopSpecialAudition();
                        const auto generation = safe->editGeneration;
                        const auto selectedId =
                            safe->selectedMarker >= 0 && static_cast<size_t>(safe->selectedMarker) <
                                                             safe->session.markers.size()
                                ? safe->session.markers[static_cast<size_t>(safe->selectedMarker)]
                                      .id
                                : juce::String{};
                        success = safe->prepareRangeSessionEdit(
                            [&](carta::Session& candidate) {
                                candidate.markers = std::move(*markers);
                                return true;
                            },
                            [&] {
                                safe->selectedMarker = -1;
                                if (selectedId.isNotEmpty()) {
                                    for (size_t i = 0; i < safe->session.markers.size(); ++i) {
                                        if (safe->session.markers[i].id == selectedId) {
                                            safe->selectedMarker = static_cast<int>(i);
                                            break;
                                        }
                                    }
                                }
                                safe->timelineClipSelection.retainAfterStableEdit(generation,
                                                                                  generation + 1);
                                if (safe->timelineTrackRevision == generation) {
                                    safe->timelineTrackRevision = generation + 1;
                                }
                            },
                            task->audacity ? "Import Audacity labels" : "Import markers", failure,
                            true);
                        if (success && task->ipc) {
                            safe->lastIpcMarkerExchangeGeneration = safe->editGeneration;
                        }
                    }
                }
            }
            if (auto* result =
                    task->ipc ? safe->lastIpcMarkerExchange.getDynamicObject() : nullptr) {
                result->setProperty("state", success                                   ? "complete"
                                             : failure == "Marker exchange cancelled." ? "cancelled"
                                                                                       : "failed");
                result->setProperty("stale",
                                    safe->editGeneration != safe->lastIpcMarkerExchangeGeneration);
                result->setProperty("cancellation_requested", task->cancelled.load());
                if (success) {
                    result->setProperty("marker_count", static_cast<int>(count));
                    if (!task->writing) {
                        result->setProperty("changed", changed);
                        result->setProperty(
                            "retained_marker_ids",
                            static_cast<int>(changed ? reconciliation->retained : count));
                        result->setProperty("removed_title_form_markers", 0);
                    }
                } else {
                    result->setProperty("error", failure.isNotEmpty()
                                                     ? failure
                                                     : juce::String("Marker exchange failed."));
                }
                if (task->writing) {
                    result->setProperty("file_written", ok);
                }
            }
            safe->activeMarkerExchange.reset();
            safe->status.setText(
                success ? (task->writing ? "Exported markers to " + task->file.getFileName() + "."
                           : changed     ? "Imported markers; Undo restores the previous markers."
                                         : "Markers unchanged.")
                        : failure,
                juce::dontSendNotification);
            safe->menuItemsChanged();
            safe->repaint();
        });
    });

    status.setText(writing ? "Exporting markers..." : "Importing markers...",
                   juce::dontSendNotification);
    menuItemsChanged();
    return true;
}

void Studio::recordImportIssue(const juce::File& file, const juce::String& reason) {
    if (importIssues.size() == 200) {
        importIssues.erase(importIssues.begin());
        ++omittedImportIssues;
    }
    importIssues.emplace_back(file.getFullPathName(), reason);
    status.setText("Import issue: " + file.getFileName() + ". See File > Import issues.",
                   juce::dontSendNotification);
    menuItemsChanged();
    resized();
}

void Studio::clearImportIssues() {
    importIssues.clear();
    omittedImportIssues = 0;
    menuItemsChanged();
    resized();
    status.setText("Import issues cleared.", juce::dontSendNotification);
}

juce::String Studio::importIssueText() const {
    juce::String report =
        "Import issues from this session. Successfully imported files remain in the project.\n\n";
    if (omittedImportIssues > 0) {
        report += juce::String(omittedImportIssues) + " older issues omitted.\n\n";
    }
    for (const auto& issue : importIssues) {
        report += issue.first + "\n" + issue.second + "\n\n";
    }
    return report;
}

void Studio::showImportIssues() {
    if (importIssues.empty()) {
        return;
    }
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Import issues";
    auto* content = new ImportIssueReport(importIssueText());
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.resizable = true;
    options.useNativeTitleBar = true;
    options.dialogBackgroundColour = juce::Desktop::getInstance().isDarkModeActive()
                                         ? juce::Colour(0xff252525)
                                         : juce::Colour(0xffeeeeee);
    options.launchAsync();
    juce::MessageManager::callAsync(
        [safe = juce::Component::SafePointer<ImportIssueReport>(content)] {
            if (safe != nullptr) {
                safe->focusText();
            }
        });
}

void Studio::showTrackSources() {
    const int row = selectedTrack;
    const auto generation = editGeneration;
    if (row < 0 || row >= static_cast<int>(session.tracks.size())) {
        return;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (sourceEnumerationHook) {
        sourceEnumerationHook();
    }
#endif
    if (generation != editGeneration || row != selectedTrack ||
        row >= static_cast<int>(session.tracks.size())) {
        status.setText("The track changed; open source information again.",
                       juce::dontSendNotification);
        return;
    }
    const auto& track = session.tracks[static_cast<size_t>(row)];
    auto name = track.name;
    auto sources = carta::trackSources(track);
    if (sources.empty()) {
        status.setText("This track has no audio sources.", juce::dontSendNotification);
        return;
    }
    showTrackSourceList(std::move(name), std::move(sources));
}

void Studio::showTrackSourceList(juce::String name, std::vector<carta::TrackSource> sources) {
    std::vector<juce::String> checks;
    checks.reserve(sources.size());
    for (const auto& source : sources) {
        juce::String time;
        const auto* checked = sourceChecks.lookup(source.audio, &time);
        checks.push_back(checked ? "Last check " + time + ": " + checked->detail
                                 : juce::String("Not checked this session"));
    }
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Track sources";
    const auto revision = editGeneration;
    auto relink = [safe = juce::Component::SafePointer<Studio>(this),
                   revision](std::shared_ptr<carta::Audio> source) {
        juce::MessageManager::callAsync([safe, candidateSource = std::move(source), revision] {
            if (safe != nullptr) {
                safe->chooseSourceReplacement(candidateSource, revision);
            }
        });
    };
    auto* content = new TrackSourceList(std::move(name), std::move(sources), std::move(checks),
                                        std::move(relink));
    options.content.setOwned(content);
    options.componentToCentreAround = this;
    options.resizable = true;
    options.useNativeTitleBar = true;
    options.dialogBackgroundColour = juce::Desktop::getInstance().isDarkModeActive()
                                         ? juce::Colour(0xff252525)
                                         : juce::Colour(0xffeeeeee);
    options.launchAsync();
    juce::MessageManager::callAsync(
        [safe = juce::Component::SafePointer<TrackSourceList>(content)] {
            if (safe != nullptr) {
                safe->focusList();
            }
        });
}

bool Studio::verifySelectedSources(bool allTracks, int requestedTrack, bool ipc) {
    if (activeSourceCheck) {
        status.setText("Source verification is already running.", juce::dontSendNotification);
        return false;
    }
    auto task = std::make_shared<SourceCheckTask>();
    task->ipc = ipc;
    task->generation = editGeneration;
    const int targetTrack = requestedTrack >= 0 ? requestedTrack : selectedTrack;
    if (!allTracks && (targetTrack < 0 || targetTrack >= static_cast<int>(session.tracks.size()))) {
        status.setText("Select a track first.", juce::dontSendNotification);
        return false;
    }
#if CARTA_STUDIO_COMPONENT_TEST
    if (sourceEnumerationHook) {
        sourceEnumerationHook();
    }
#endif
    if (task->generation != editGeneration ||
        (!allTracks && requestedTrack < 0 && targetTrack != selectedTrack) ||
        (!allTracks && targetTrack >= static_cast<int>(session.tracks.size()))) {
        status.setText("The project or track changed; start the source check again.",
                       juce::dontSendNotification);
        return false;
    }
    if (allTracks) {
        task->name = "All tracks";
        task->sources = carta::sessionSources(session);
    } else {
        const auto& track = session.tracks[static_cast<size_t>(targetTrack)];
        task->name = track.name;
        task->sources = carta::trackSources(track);
    }
    if (task->sources.empty()) {
        status.setText(allTracks ? "There are no audio sources to check."
                                 : "This track has no audio sources.",
                       juce::dontSendNotification);
        return false;
    }
    if (ipc) {
        auto* info = new juce::DynamicObject();
        info->setProperty("state", "running");
        info->setProperty("scope", allTracks ? "all" : "track");
        info->setProperty("progress", 0.0);
        info->setProperty("cancellation_requested", false);
        info->setProperty("stale", false);
        if (!allTracks) {
            info->setProperty("track", requestedTrack);
        }
        lastIpcSourceCheck = juce::var(info);
        lastIpcSourceGeneration = task->generation;
    }
    activeSourceCheck = task;
    resized();
    menuItemsChanged();
    status.setText(allTracks ? "Checking all source files..." : "Checking track source files...",
                   juce::dontSendNotification);

    sourceCheckPool.addJob([safe = juce::Component::SafePointer<Studio>(this), task] {
        std::vector<carta::SourceCheck> checks;
        juce::String error;
        bool ok = false;
        try {
            ok = carta::verifyTrackSources(task->sources, checks, error, [task](double value) {
                task->progress.store(value);
                return !task->cancelled.load();
            });
        } catch (const std::exception& e) {
            error = "Could not verify sources: " + juce::String(e.what());
        } catch (...) {
            error = "Could not verify sources.";
        }

        juce::MessageManager::callAsync([safe, task, results = std::move(checks), error,
                                         ok]() mutable {
            if (safe == nullptr || safe->activeSourceCheck != task) {
                return;
            }
            safe->activeSourceCheck.reset();
            safe->resized();
            safe->menuItemsChanged();
            auto* ipcResult = task->ipc ? safe->lastIpcSourceCheck.getDynamicObject() : nullptr;
            const bool cancelled = task->cancelled.load();
            const bool stale = safe->editGeneration != task->generation;
            if (ipcResult) {
                ipcResult->setProperty("state", cancelled ? "cancelled"
                                                : ok      ? "complete"
                                                          : "failed");
                ipcResult->setProperty(
                    "progress",
                    ok && !cancelled ? 1.0 : juce::jlimit(0.0, 1.0, task->progress.load()));
                ipcResult->setProperty("cancellation_requested", cancelled);
                ipcResult->setProperty("stale", stale);
                if (!ok && !cancelled) {
                    ipcResult->setProperty("error", error);
                }
            }
            if (cancelled) {
                safe->status.setText("Source verification cancelled.", juce::dontSendNotification);
                return;
            }
            if (!ok) {
                safe->status.setText(error, juce::dontSendNotification);
                return;
            }
            if (ipcResult) {
                juce::Array<juce::var> rows;
                for (size_t i = 0; i < results.size(); ++i) {
                    const auto& check = results[i];
                    auto* row = new juce::DynamicObject();
                    row->setProperty("path", task->sources[i].audio->file.getFullPathName());
                    row->setProperty("clip_count", task->sources[i].clipCount);
                    row->setProperty("status", sourceStatusToIpcString(check.status));
                    row->setProperty("detail", check.detail);
                    rows.add(juce::var(row));
                }
                ipcResult->setProperty("sources", juce::var(rows));
            }
            if (stale) {
                safe->status.setText("Project changed during source verification; run it again.",
                                     juce::dontSendNotification);
                return;
            }
            const auto time = juce::Time::getCurrentTime().toString(true, true);
            if (!safe->sourceChecks.record(task->sources, results, time)) {
                safe->status.setText("Could not record source verification results.",
                                     juce::dontSendNotification);
                return;
            }
            const auto issues =
                std::count_if(results.begin(), results.end(), [](const auto& entry) {
                    return entry.status != carta::SourceStatus::matches;
                });
            safe->status.setText(issues > 0 ? juce::String("Source verification found ") +
                                                  juce::String(static_cast<int>(issues)) +
                                                  (issues == 1 ? " issue." : " issues.")
                                            : "Source verification complete; all files match.",
                                 juce::dontSendNotification);
            safe->repaint();
            if (!task->ipc) {
                safe->showTrackSourceList(task->name, task->sources);
            }
        });
    });
    return true;
}

void Studio::chooseSourceReplacement(const std::shared_ptr<carta::Audio>& source,
                                     juce::uint64 revision) {
    if (!source || source->sourceHash.length() != 64 ||
        !source->sourceHash.containsOnly("0123456789abcdef")) {
        status.setText("This source has no checksum for safe relinking.",
                       juce::dontSendNotification);
        return;
    }
    if (editGeneration != revision || dragMode != kDragNone || activeSourceCheck || activeOpen) {
        status.setText(
            "Project changed or another source operation is running; reopen source information.",
            juce::dontSendNotification);
        return;
    }
    launchStudioChooser("Choose checksum-identical audio", source->file.getParentDirectory(), "*",
                        juce::FileBrowserComponent::openMode |
                            juce::FileBrowserComponent::canSelectFiles,
                        [safe = juce::Component::SafePointer<Studio>(this), source,
                         revision](const StudioFileSelection& fc) {
                            if (safe == nullptr || fc.getResult() == juce::File{}) {
                                return;
                            }
                            safe->startSourceReplacement(source, fc.getResult(), revision);
                        });
}

bool Studio::startSourceReplacement(const std::shared_ptr<carta::Audio>& source,
                                    const juce::File& file, juce::uint64 revision, bool ipc) {
    if (!source || source->sourceHash.length() != 64 ||
        !source->sourceHash.containsOnly("0123456789abcdef")) {
        status.setText("This source has no checksum for safe relinking.",
                       juce::dontSendNotification);
        return false;
    }
    if (editGeneration != revision || dragMode != kDragNone || activeSourceCheck || activeOpen) {
        status.setText(
            "Project changed or another source operation is running; reopen source information.",
            juce::dontSendNotification);
        return false;
    }
    if ((activeExport && carta::pathsReferToSameFile(activeExport->target, file)) ||
        (activeSave && carta::pathsReferToSameFile(activeSave->file, file)) ||
        (activeRecovery && carta::pathsReferToSameFile(activeRecovery->file, file)) ||
        (activeMarkerExchange && activeMarkerExchange->writing &&
         carta::pathsReferToSameFile(activeMarkerExchange->file, file)) ||
        carta::pathsReferToSameFile(recoveryFile, file)) {
        status.setText("Choose a file that is not currently being written.",
                       juce::dontSendNotification);
        return false;
    }

    auto task = std::make_shared<SourceCheckTask>();
    task->generation = revision;
    task->sources = {{source, 1}};
    task->relinkFile = file;
    task->relinking = true;
    task->ipc = ipc;
    if (ipc) {
        auto* info = new juce::DynamicObject();
        info->setProperty("state", "running");
        info->setProperty("scope", "relink");
        info->setProperty("source", source->file.getFullPathName());
        info->setProperty("replacement", file.getFullPathName());
        info->setProperty("progress", 0.0);
        info->setProperty("cancellation_requested", false);
        info->setProperty("stale", false);
        lastIpcSourceCheck = juce::var(info);
        lastIpcSourceGeneration = revision;
    }
    activeSourceCheck = task;
    status.setText("Verifying replacement source...", juce::dontSendNotification);
    menuItemsChanged();
    resized();

    sourceCheckPool.addJob([safe = juce::Component::SafePointer<Studio>(this), task, source] {
        std::shared_ptr<carta::Audio> replacement;
        juce::String error;
        try {
            juce::AudioFormatManager manager;
            manager.registerBasicFormats();
            replacement = carta::loadVerifiedReplacement(manager, source, task->relinkFile, error,
                                                         [task](double p) {
                                                             task->progress.store(p);
                                                             return !task->cancelled.load();
                                                         });
        } catch (const std::exception& e) {
            error = "Source relink failed: " + juce::String(e.what());
        } catch (...) {
            error = "Source relink failed.";
        }

        juce::MessageManager::callAsync([safe, task, source, replacement, error] {
            if (safe == nullptr || safe->activeSourceCheck != task) {
                return;
            }
            safe->activeSourceCheck.reset();
            safe->resized();
            safe->menuItemsChanged();

            auto finish = [&](const juce::String& state, const juce::String& message,
                              bool changed = false) {
                if (auto* info =
                        task->ipc ? safe->lastIpcSourceCheck.getDynamicObject() : nullptr) {
                    info->setProperty("state", state);
                    info->setProperty(
                        "progress",
                        state == "complete" ? 1.0 : juce::jlimit(0.0, 1.0, task->progress.load()));
                    info->setProperty("cancellation_requested", task->cancelled.load());
                    info->setProperty("stale",
                                      safe->editGeneration != safe->lastIpcSourceGeneration);
                    if (state == "failed") {
                        info->setProperty("error", message);
                    }
                    if (state == "complete") {
                        info->setProperty("changed", changed);
                    }
                }
                safe->status.setText(message, juce::dontSendNotification);
            };

            if (task->cancelled.load() || !replacement) {
                finish(task->cancelled.load() ? "cancelled" : "failed",
                       task->cancelled.load() ? "Source relink cancelled." : error);
                return;
            }
            if (safe->editGeneration != task->generation || safe->dragMode != kDragNone) {
                finish("failed", "Project changed; reopen source information and relink again.");
                return;
            }
            if ((safe->activeExport &&
                 carta::pathsReferToSameFile(safe->activeExport->target, task->relinkFile)) ||
                (safe->activeSave &&
                 carta::pathsReferToSameFile(safe->activeSave->file, task->relinkFile))) {
                finish("failed", "Replacement file is being written; try again afterward.");
                return;
            }
            if (source->file == replacement->file) {
                safe->sourceChecks.record({{source, 1}},
                                          {{carta::SourceStatus::matches,
                                            "Matches imported checksum and decoded samples"}},
                                          juce::Time::getCurrentTime().toString(true, true));
                finish("complete", "Original source verified; its path is unchanged.");
                safe->repaint();
                return;
            }
            juce::String message;
            const bool changed = safe->mutateMixState(
                [&](carta::Session& candidate) {
                    if (safe->editGeneration != task->generation || safe->dragMode != kDragNone) {
                        message = "Project changed; relink again.";
                        return false;
                    }
                    return carta::replaceVerifiedSource(candidate, source, replacement, message);
                },
                message, "Relink source");
            if (!changed) {
                finish("failed", message);
                return;
            }
            safe->sourceChecks.record(
                {{replacement, 1}},
                {{carta::SourceStatus::matches, "Matches imported checksum and decoded samples"}},
                juce::Time::getCurrentTime().toString(true, true));
            if (task->ipc) {
                safe->lastIpcSourceGeneration = safe->editGeneration;
            }
            finish("complete",
                   "Relinked all clips sharing that imported source; save to keep the new path.",
                   true);
            safe->repaint();
        });
    });
    return true;
}

juce::int64 Studio::layerEndSample(const carta::Track& track) {
    juce::int64 end = 0;
    for (const auto& clip : track.clips) {
        juce::int64 first = 0;
        juce::int64 last = 0;
        if (carta::clipSampleRange(clip, first, last)) {
            end = std::max(end, last);
        }
    }
    return end;
}

bool Studio::hasMarkerAtSample(const carta::Session& s, juce::int64 sample) {
    for (const auto& m : s.markers) {
        if (std::isfinite(m.position) &&
            std::floor(m.position * carta::sessionRate + 0.5) == static_cast<double>(sample)) {
            return true;
        }
    }
    return false;
}

bool Studio::commitReadyImport(const std::shared_ptr<ImportTask>& task, juce::String& error) {
    const juce::ScopedLock guard(lock);
    if (dragMode != kDragNone || task->cancelled.load()) {
        error = task->cancelled.load() ? "Import cancelled." : "Finish the current drag first.";
        return false;
    }
    if (task->targetTrack >= 0 && task->targetRevision != editGeneration) {
        error = "Project changed while importing; select the destination layer and try again.";
        return false;
    }
    stopSpecialAudition();
    const auto before = captureSelectionState();
    const auto generation = editGeneration;
    carta::ClipSelection nextClips;
    std::set<int> nextTracks;
    juce::int64 placedSample = std::max<juce::int64>(0, task->startSample);
    juce::int64 placedEndSample = 0;
    int placedTrack = -1;

    const bool inserted = prepareRangeSessionEdit(
        [&](carta::Session& candidate) {
            placedSample = std::max<juce::int64>(0, task->startSample);
            if (task->newLayer) {
                placedTrack = static_cast<int>(candidate.tracks.size());
                if (!carta::addImportedAudioTrack(candidate, task->audio, placedSample)) {
                    error = "Could not place imported audio at that timeline position.";
                    return false;
                }
            } else {
                const int destTrack = task->targetTrack >= 0 ? task->targetTrack : 0;
                if (task->batchId != 0) {
                    const auto it = importBatchCursor.find(task->batchId);
                    if (it != importBatchCursor.end()) {
                        placedSample = std::max(placedSample, it->second);
                    }
                }
                if (destTrack == 0 && candidate.tracks.empty()) {
                    placedTrack = 0;
                    if (!carta::addImportedAudioTrack(candidate, task->audio, placedSample)) {
                        error = "Could not place imported audio at that timeline position.";
                        return false;
                    }
                } else {
                    if (destTrack < 0 || destTrack >= static_cast<int>(candidate.tracks.size())) {
                        error = "Destination layer no longer exists.";
                        return false;
                    }
                    if (task->sequential) {
                        placedSample = std::max(
                            placedSample,
                            layerEndSample(candidate.tracks[static_cast<size_t>(destTrack)]));
                    }
                    placedTrack = destTrack;
                    if (!carta::addImportedAudioToTrack(candidate, destTrack, task->audio,
                                                        placedSample)) {
                        error = "Could not place imported audio at that timeline position.";
                        return false;
                    }
                }
            }

            juce::int64 dummyFirst = 0;
            if (!candidate.tracks[static_cast<size_t>(placedTrack)].clips.empty()) {
                carta::clipSampleRange(
                    candidate.tracks[static_cast<size_t>(placedTrack)].clips.back(), dummyFirst,
                    placedEndSample);
            }
            if (task->autoMarker && !hasMarkerAtSample(candidate, placedSample)) {
                auto markerName =
                    task->file.getFileNameWithoutExtension().replaceCharacters("\r\n", "  ").trim();
                if (markerName.isEmpty()) {
                    markerName = "Marker " + juce::String(candidate.markers.size() + 1);
                }
                candidate.markers.push_back({0, markerName});
                juce::String markerError;
                if (!carta::setMarkerSample(candidate,
                                            static_cast<int>(candidate.markers.size()) - 1,
                                            placedSample, markerError)) {
                    error = markerError;
                    return false;
                }
            }

            juce::int64 maxMdEnd = 0;
            for (const auto& tr : candidate.mdDisc.tracks) {
                for (const auto& pt : tr.parts) {
                    maxMdEnd = std::max(maxMdEnd, static_cast<juce::int64>(pt.endSample));
                }
            }
            if (task->autoMdTrack && candidate.mdDisc.autoTrackOnImport &&
                placedEndSample > placedSample &&
                ((placedTrack == 0 && task->sequential) || placedEndSample > maxMdEnd)) {
                auto mdTitle =
                    task->file.getFileNameWithoutExtension().replaceCharacters("\r\n", "  ").trim();
                if (mdTitle.isEmpty()) {
                    mdTitle = "Track " +
                              juce::String(static_cast<int>(candidate.mdDisc.tracks.size()) + 1);
                }
                carta::applyAutoTrackForPlacedClip(candidate.mdDisc, mdTitle.toStdString(),
                                                   placedSample, placedEndSample);
            }

            if (!carta::preservesMarkerBounds(session, candidate, error)) {
                return false;
            }
            if (!nextClips.restore(candidate, before.selectedClips, generation + 1)) {
                nextClips.clear();
            }
            nextTracks.insert(before.selectedTracks.begin(), before.selectedTracks.end());
            return true;
        },
        [&] {
            resampling.registerPrepared(task->audio->sampleRate, deviceRate, task->kernel);
            timelineClipSelection = std::move(nextClips);
            timelineTracks = std::move(nextTracks);
            timelineTrackRevision = generation + 1;
            keyboardRange.active = false;
        },
        "Import audio", error);

    if (inserted) {
        task->startSample = placedSample;
        if (task->batchId != 0 && placedEndSample > 0) {
            importBatchCursor[task->batchId] = placedEndSample;
        }
        if (selectedTrack < 0) {
            if (task->newLayer) {
                selectedTrack = static_cast<int>(session.tracks.size()) - 1;
            } else if (placedTrack >= 0 && placedTrack < static_cast<int>(session.tracks.size())) {
                selectedTrack = placedTrack;
            }
        }
        for (const auto& pending : pendingImports) {
            if (pending->targetRevision == generation) {
                pending->targetRevision = editGeneration;
            }
        }
        repaint();
    }
    return inserted;
}

void Studio::finishReadyImports() {
    while (dragMode == kDragNone && !pendingImports.empty() && pendingImports.front()->ready) {
        const auto task = pendingImports.front();
        pendingImports.erase(pendingImports.begin());
        const auto releaseBatch = [&] {
            if (task->batchId != 0 &&
                std::none_of(pendingImports.begin(), pendingImports.end(),
                             [id = task->batchId](const auto& p) { return p->batchId == id; })) {
                importBatchCursor.erase(task->batchId);
            }
        };
        if (task->cancelled.load()) {
            releaseBatch();
            recordImportIssue(task->file, "Import cancelled.");
            continue;
        }
        if (!task->audio) {
            releaseBatch();
            recordImportIssue(task->file,
                              task->error.isEmpty() ? "Could not decode audio." : task->error);
            continue;
        }
        if (activeExport && carta::pathsReferToSameFile(activeExport->target, task->file)) {
            releaseBatch();
            recordImportIssue(task->file, "Cannot import the WAV currently being exported.");
            continue;
        }
        juce::String error;
        if (!commitReadyImport(task, error)) {
            releaseBatch();
            recordImportIssue(task->file,
                              error.isNotEmpty()
                                  ? error
                                  : "Could not place imported audio at that timeline position.");
            continue;
        }
        releaseBatch();
        const auto where = "Imported " + task->audio->file.getFileName() + " at sample " +
                           juce::String(task->startSample) + ". ";
        status.setText(where + (task->audio->numChannels() > 2
                                    ? "Confirm Layer > Channel routing before export."
                                    : "Drag title to move; drag edges to trim."),
                       juce::dontSendNotification);
    }
}

bool Studio::importFile(const juce::File& file, juce::int64 startSample, int targetTrack,
                        int markerPolicy, bool forceNewLayer, juce::uint64 batchId) {
    if (savePromptActive) {
        status.setText("Finish the current dialog before importing audio.",
                       juce::dontSendNotification);
        return false;
    }
    if (activeOpen) {
        recordImportIssue(file,
                          "Wait for project opening to finish or cancel it before importing.");
        return false;
    }
    if (carta::pathsReferToSameFile(recoveryFile, file)) {
        recordImportIssue(file, "Cannot import the active recovery snapshot.");
        return false;
    }
    if (activeSave && carta::pathsReferToSameFile(activeSave->file, file)) {
        recordImportIssue(file, "Cannot import the project file currently being saved.");
        return false;
    }
    if (activeExport && carta::pathsReferToSameFile(activeExport->target, file)) {
        recordImportIssue(file, "Cannot import the WAV currently being exported.");
        return false;
    }
    if (activeMarkerExchange && activeMarkerExchange->writing &&
        carta::pathsReferToSameFile(activeMarkerExchange->file, file)) {
        recordImportIssue(file, "Cannot import a file while marker export is writing it.");
        return false;
    }

    const bool legacyNewLayer =
        !forceNewLayer && startSample >= 0 && targetTrack == -1 && markerPolicy == -1;
    const bool sequential = !forceNewLayer && !legacyNewLayer &&
                            (targetTrack == -2 || (targetTrack >= 0 && startSample < 0));
    const bool newLayer = forceNewLayer || legacyNewLayer;
    const juce::int64 effectiveStart = startSample < 0 ? 0 : startSample;

    auto task = std::make_shared<ImportTask>(file, effectiveStart);
    task->sequential = sequential;
    task->newLayer = newLayer;
    task->targetTrack = targetTrack < 0 ? -1 : targetTrack;
    task->batchId = batchId;
    task->autoMarker = (markerPolicy > 0);
    task->autoMdTrack = (markerPolicy != 0) && !newLayer;
    task->targetRevision = editGeneration;
    pendingImports.push_back(task);

    double outputRate = 0;
    {
        const juce::ScopedLock guard(lock);
        outputRate = deviceRate;
    }
    status.setText("Import queued: " + file.getFileName() + ".", juce::dontSendNotification);

    importPool.addJob([safe = juce::Component::SafePointer<Studio>(this), task, cache = importCache,
                       outputRate] {
        std::shared_ptr<carta::Audio> audio;
        std::shared_ptr<const carta::SincResampler> kernel;
        juce::String error;
        try {
            if (!task->cancelled.load()) {
                juce::AudioFormatManager manager;
                manager.registerBasicFormats();
                audio = carta::loadAudio(manager, task->file, error, [task](double progress) {
                    task->progress.store(progress * 0.95);
                    return !task->cancelled.load();
                });
                if (audio && !task->cancelled.load() && std::isfinite(outputRate) &&
                    outputRate > 0) {
                    const auto key = std::make_pair(audio->sampleRate, outputRate);
                    const auto it = cache->kernels.find(key);
                    if (it != cache->kernels.end()) {
                        kernel = it->second.lock();
                    }
                    if (!kernel) {
                        kernel =
                            std::make_shared<carta::SincResampler>(audio->sampleRate, outputRate);
                        cache->kernels[key] = kernel;
                    }
                }
            }
        } catch (const std::exception& e) {
            audio.reset();
            error = "Import failed: " + juce::String(e.what());
        } catch (...) {
            audio.reset();
            error = "Import failed.";
        }
        task->progress.store(1);

        juce::MessageManager::callAsync([safe, task, audio, kernel, error] {
            if (safe == nullptr) {
                return;
            }
            const auto it =
                std::find(safe->pendingImports.begin(), safe->pendingImports.end(), task);
            if (it == safe->pendingImports.end()) {
                return;
            }
            task->audio = audio;
            task->kernel = kernel;
            task->error = error;
            task->ready = true;
            if (safe->dragMode == kDragNone) {
                safe->finishReadyImports();
            } else {
                safe->status.setText("Audio import ready; finish or cancel the current drag.",
                                     juce::dontSendNotification);
            }
        });
    });
    return true;
}

void Studio::importAudio() {
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
#if CARTA_STUDIO_MINIMP3
    carta::registerNativeMp3Format(formats);
#endif
    launchStudioChooser(
        "Import audio", juce::File{}, formats.getWildcardForAllFormats(),
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::canSelectMultipleItems,
        [safe = juce::Component::SafePointer<Studio>(this)](const StudioFileSelection& fc) {
            if (safe == nullptr) {
                return;
            }
            for (const auto& file : fc.getResults()) {
                safe->importFile(file);
            }
        });
}

void Studio::exportWav(bool selectionOnly, bool selectedTracksOnly, bool markerTrack, bool clipOnly,
                       bool groupOnly, bool discReview) {
    if (!pendingImports.empty()) {
        status.setText("Wait for imports to finish or cancel them before exporting.",
                       juce::dontSendNotification);
        return;
    }
    if (activeExport) {
        status.setText("A WAV export is already running.", juce::dontSendNotification);
        return;
    }
    carta::WavRange range;
    carta::Session snapshot;
    juce::File protectedProject;
    int channels = 2;
    juce::uint64 snapshotGeneration = 0;
    const carta::WavOptions wavOptions =
        discReview ? carta::WavOptions{carta::WavEncoding::float32, false, false}
                   : carta::WavOptions{wavEncoding,
                                       wavEncoding != carta::WavEncoding::float32 && wavDither,
                                       wavEmbedMarkers};
    int firstSelectedTrack = 0;
    int lastSelectedTrack = -1;
    double cursor = 0;
    int clipTrack = -1;
    int clipIndex = -1;
    juce::String clipName;
    bool clipMuted = false;
    std::vector<carta::ClipReference> group;
    std::vector<int> independentTracks;

    {
        const juce::ScopedLock guard(lock);
        channels = masterChannels;
        cursor = position;
        protectedProject = projectFile;
        snapshotGeneration = editGeneration;
        const juce::ScopedUnlock unlock(lock);
#if CARTA_STUDIO_COMPONENT_TEST
        if (ioSnapshotHook) {
            ioSnapshotHook();
        }
#endif
        if (session.length() <= 0) {
            status.setText("Import audio before exporting.", juce::dontSendNotification);
            return;
        }
        if (selectionOnly) {
            if (!timeSelection.hasRange()) {
                status.setText("Select a time range before exporting.", juce::dontSendNotification);
                return;
            }
            range = {timeSelection.start, timeSelection.end};
            firstSelectedTrack = timeSelection.firstTrack;
            lastSelectedTrack = timeSelection.lastTrack;
            if (selectedTracksOnly) {
                independentTracks = selectedTimelineTracks();
            }
        }
        if (clipOnly) {
            if (!validSelection()) {
                status.setText("Select a clip before exporting.", juce::dontSendNotification);
                return;
            }
            clipTrack = selectedTrack;
            clipIndex = selectedClip;
            const auto& track = session.tracks[static_cast<size_t>(clipTrack)];
            clipName = carta::clipDisplayName(track.clips[static_cast<size_t>(clipIndex)]);
            clipMuted = track.muted;
        }
        if (groupOnly) {
            group = timelineClipSelection.get(editGeneration);
            if (group.empty()) {
                status.setText("Select clip titles before exporting.", juce::dontSendNotification);
                return;
            }
        }
        snapshot = session;
    }

    const auto protectedMedia = snapshot;
    juce::String error;
    if (clipOnly && !carta::isolateClip(snapshot, clipTrack, clipIndex, range.start, range.end)) {
        status.setText("This clip contains no project sample positions.",
                       juce::dontSendNotification);
        return;
    }
    if (groupOnly && !carta::isolateClipGroup(snapshot, group, range.start, range.end, error)) {
        status.setText(error, juce::dontSendNotification);
        return;
    }
    if (selectedTracksOnly &&
        !(independentTracks.empty()
              ? carta::limitToTracks(snapshot, firstSelectedTrack, lastSelectedTrack)
              : carta::limitToTrackIndices(snapshot, independentTracks))) {
        status.setText("Selected tracks are no longer valid.", juce::dontSendNotification);
        return;
    }

    int markerIndex = -1;
    carta::MasterTrack markerRegion;
    if (markerTrack) {
        const double sample = cursor * carta::sessionRate;
        if (!std::isfinite(sample) || sample < 0 || sample >= 9007199254740992.0 ||
            !carta::masterTrackAtSample(snapshot,
                                        static_cast<juce::int64>(std::floor(sample + 1.0e-7)),
                                        markerRegion, markerIndex, error)) {
            status.setText(error.isEmpty() ? "Place the playhead within the master." : error,
                           juce::dontSendNotification);
            return;
        }
        range = markerRegion.range;
    } else if (!selectionOnly && !clipOnly && !groupOnly) {
        range = carta::fullWavRange(snapshot);
    }

    if (discReview && !carta::validateDiscProgramWav(snapshot, channels, range, error)) {
        status.setText(error, juce::dontSendNotification);
        return;
    }
    if (!carta::validateWavRange(snapshot, channels, range, error, wavOptions)) {
        status.setText(error, juce::dontSendNotification);
        return;
    }

    const auto title = discReview  ? "Export disc program review WAV"
                       : groupOnly ? "Export selected clips WAV"
                       : clipOnly  ? "Export selected clip WAV"
                       : markerTrack
                           ? "Export track " + juce::String(markerIndex + 1) + " WAV (full mix)"
                       : selectedTracksOnly ? "Export time selection WAV (selected tracks)"
                       : selectionOnly      ? "Export time selection WAV (full mix)"
                                            : "Export master WAV";
    const auto filename = discReview      ? "Carta disc review.wav"
                          : groupOnly     ? "Carta clips.wav"
                          : clipOnly      ? "Carta clip.wav"
                          : markerTrack   ? "Carta track " + juce::String(markerIndex + 1) + ".wav"
                          : selectionOnly ? "Carta selection.wav"
                                          : "Carta master.wav";
    const auto duration =
        carta::formatTimelineTime(range.end - range.start, carta::TimelineTimeFormat::clock);
    const auto encoding = formatWavEncodingDescription(wavOptions.encoding);

    juce::String detail = groupOnly     ? juce::String(group.size()) + " selected clips. "
                          : clipOnly    ? clipName + ". "
                          : markerTrack ? markerRegion.name + ". "
                                        : juce::String();
    detail += "Samples " + juce::String(range.start) + utf8Str("–") + juce::String(range.end) +
              " (end exclusive), duration " + duration + ", " +
              (channels == 1 ? "mono" : "stereo") + ", " + encoding +
              (wavOptions.dither ? ", dither on." : ".");
    if (clipOnly && clipMuted) {
        detail += " Track is muted; output will be silent.";
    }
    if (groupOnly) {
        detail += " Selected tracks retain mute and solo.";
    }
    status.setText(detail, juce::dontSendNotification);

    const auto chooserTitle = title + utf8Str(" — ") + duration;
#if CARTA_STUDIO_COMPONENT_TEST
    if (exportChooserHook) {
        exportChooserHook(chooserTitle, range, channels, wavOptions, snapshot);
        return;
    }
#endif
    launchStudioChooser(
        chooserTitle,
        juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile(filename),
        "*.wav",
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<Studio>(this), snapshot, protectedMedia, channels,
         range, protectedProject, wavOptions, snapshotGeneration,
         discReview](const StudioFileSelection& fc) {
            if (safe == nullptr || fc.getResult() == juce::File{}) {
                return;
            }
            if (safe->activeExport) {
                safe->status.setText("A WAV export is already running.",
                                     juce::dontSendNotification);
                return;
            }
            const auto target = fc.getResult();
            if (!target.hasFileExtension("wav")) {
                safe->status.setText("Choose a filename ending in .wav.",
                                     juce::dontSendNotification);
                return;
            }
            juce::String destinationError;
            if (!carta::validateOutputDestination(protectedMedia, target, protectedProject,
                                                  destinationError) ||
                !carta::validateOutputDestination(snapshot, target, safe->projectFile,
                                                  destinationError) ||
                !safe->validateRetainedMedia(target, destinationError)) {
                safe->status.setText(destinationError, juce::dontSendNotification);
                return;
            }
            if (safe->activeSave && carta::pathsReferToSameFile(safe->activeSave->file, target)) {
                safe->status.setText("Choose an output other than the project being saved.",
                                     juce::dontSendNotification);
                return;
            }
            auto task = std::make_shared<ExportTask>();
            task->target = target;
            task->wavOptions = wavOptions;
            task->snapshot = snapshot;
            task->snapshotGeneration = snapshotGeneration;
            task->discProgram = discReview;
            safe->activeExport = task;
            if (discReview) {
                safe->writeExport(task, channels, range, protectedProject);
            } else {
                safe->beginExport(task, channels, range, protectedProject);
            }
        });
}

void Studio::exportAllTracks() {
    if (!pendingImports.empty()) {
        status.setText("Wait for imports to finish or cancel them before exporting.",
                       juce::dontSendNotification);
        return;
    }
    if (activeExport) {
        status.setText("A WAV export is already running.", juce::dontSendNotification);
        return;
    }
    carta::Session snapshot;
    int channels = 2;
    juce::File protectedProject;
    juce::uint64 snapshotGeneration = 0;
    {
        const juce::ScopedLock guard(lock);
        channels = masterChannels;
        protectedProject = projectFile;
        snapshotGeneration = editGeneration;
        copyWorkerSnapshot(snapshot);
    }
    const carta::WavOptions options{
        wavEncoding, wavEncoding != carta::WavEncoding::float32 && wavDither, wavEmbedMarkers};
    std::vector<carta::MasterTrack> tracks;
    juce::String error;
    if (!carta::getMasterTracks(snapshot, tracks, error)) {
        status.setText(error, juce::dontSendNotification);
        return;
    }
    for (const auto& track : tracks) {
        if (!carta::validateWavRange(snapshot, channels, track.range, error, options)) {
            status.setText(error, juce::dontSendNotification);
            return;
        }
    }
    const auto range = carta::fullWavRange(snapshot);
    const auto duration =
        carta::formatTimelineTime(range.end - range.start, carta::TimelineTimeFormat::clock);
    const auto encoding = formatWavEncodingDescription(options.encoding);
    status.setText("Export " + juce::String(static_cast<int>(tracks.size())) +
                       " master tracks, samples " + juce::String(range.start) + utf8Str("–") +
                       juce::String(range.end) + " (end exclusive), duration " + duration + ", " +
                       (channels == 1 ? "mono" : "stereo") + ", " + encoding +
                       (options.dither ? ", dither on." : "."),
                   juce::dontSendNotification);
    const auto chooserTitle = "Choose parent folder for " +
                              juce::String(static_cast<int>(tracks.size())) +
                              utf8Str(" tracks — ") + duration;
#if CARTA_STUDIO_COMPONENT_TEST
    if (exportChooserHook) {
        exportChooserHook(chooserTitle, range, channels, options, snapshot);
        return;
    }
#endif
    launchStudioChooser(
        chooserTitle, juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*",
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safe = juce::Component::SafePointer<Studio>(this), snapshot, channels, range,
         protectedProject, options, tracks, snapshotGeneration](const StudioFileSelection& fc) {
            if (safe == nullptr || fc.getResult() == juce::File{}) {
                return;
            }
            if (safe->activeExport) {
                safe->status.setText("A WAV export is already running.",
                                     juce::dontSendNotification);
                return;
            }
            if (!safe->pendingImports.empty()) {
                safe->status.setText("Wait for imports to finish before exporting.",
                                     juce::dontSendNotification);
                return;
            }
            if (!fc.getResult().isDirectory()) {
                safe->status.setText("Choose an existing parent folder.",
                                     juce::dontSendNotification);
                return;
            }
            auto task = std::make_shared<ExportTask>();
            task->target = fc.getResult();
            task->wavOptions = options;
            task->batch = true;
            task->tracks = tracks;
            task->snapshot = snapshot;
            task->snapshotGeneration = snapshotGeneration;
            safe->activeExport = task;
            safe->status.setText("Checking " + juce::String(static_cast<int>(tracks.size())) +
                                     " master tracks for export.",
                                 juce::dontSendNotification);
            safe->beginExport(task, channels, range, protectedProject);
        });
}

void Studio::beginExport(std::shared_ptr<ExportTask> task, int channels, carta::WavRange range,
                         juce::File protectedProject) {
    task->phase.store(0);
    task->progress.store(0);
    exportPool.addJob([safe = juce::Component::SafePointer<Studio>(this), task, channels, range,
                       protectedProject] {
        carta::ExportPreflight preflight;
        preflight.channels = channels;
        juce::String error;
        bool ok = false;
        try {
            if (!task->batch) {
                ok = carta::scanExportPreflight(
                    task->snapshot, channels, range, preflight, error,
                    [task](double progress) {
                        task->progress.store(progress);
                        return !task->cancelled.load();
                    },
                    task->wavOptions);
            } else {
                ok = true;
                const double total = static_cast<double>(range.end - range.start);
                double done = 0;
                for (const auto& track : task->tracks) {
                    const double length = static_cast<double>(track.range.end - track.range.start);
                    carta::ExportPreflight trackCheck;
                    if (!carta::scanExportPreflight(
                            task->snapshot, channels, track.range, trackCheck, error,
                            [task, done, length, total](double progress) {
                                task->progress.store((done + length * progress) / total);
                                return !task->cancelled.load();
                            },
                            task->wavOptions)) {
                        ok = false;
                        break;
                    }
                    for (int ch = 0; ch < channels; ++ch) {
                        preflight.peak[ch] = std::max(preflight.peak[ch], trackCheck.peak[ch]);
                        preflight.clippedSamples[ch] += trackCheck.clippedSamples[ch];
                        if (trackCheck.firstClipped[ch] >= 0 &&
                            (preflight.firstClipped[ch] < 0 ||
                             trackCheck.firstClipped[ch] < preflight.firstClipped[ch])) {
                            preflight.firstClipped[ch] = trackCheck.firstClipped[ch];
                        }
                    }
                    done += length;
                }
            }
        } catch (const std::exception& e) {
            error = "Could not check export: " + juce::String(e.what());
        } catch (...) {
            error = "Could not check export.";
        }

        juce::MessageManager::callAsync([safe, task, channels, range, protectedProject, preflight,
                                         ok, error] {
            if (safe == nullptr || safe->activeExport != task) {
                return;
            }
            if (!ok || task->cancelled.load()) {
                if (task->ipc) {
                    if (auto* result = safe->lastIpcExport.getDynamicObject()) {
                        result->setProperty("state",
                                            task->cancelled.load() ? "cancelled" : "failed");
                        result->setProperty("phase", "finished");
                        result->setProperty("progress",
                                            juce::jlimit(0.0, 1.0, task->progress.load()));
                        result->setProperty("cancellation_requested", task->cancelled.load());
                        result->setProperty("error", task->cancelled.load()
                                                         ? juce::String("Export cancelled.")
                                                         : error);
                    }
                }
                safe->activeExport.reset();
                safe->status.setText(task->cancelled.load() ? "WAV export cancelled." : error,
                                     juce::dontSendNotification);
                return;
            }

            float peak = 0;
            bool willClip = false;
            for (int ch = 0; ch < channels; ++ch) {
                peak = std::max(peak, preflight.peak[ch]);
                willClip |= preflight.clippedSamples[ch] > 0;
            }
            task->peak = peak;
            const auto opts = task->wavOptions;
            if (task->ipc) {
                if (auto* result = safe->lastIpcExport.getDynamicObject()) {
                    juce::Array<juce::var> rows;
                    for (int ch = 0; ch < channels; ++ch) {
                        auto* row = new juce::DynamicObject();
                        row->setProperty("peak", static_cast<double>(preflight.peak[ch]));
                        row->setProperty("clipped_samples", preflight.clippedSamples[ch]);
                        row->setProperty("first_clipped_sample", preflight.firstClipped[ch]);
                        rows.add(juce::var(row));
                    }
                    result->setProperty("preflight", juce::var(rows));
                }
            }
            if (!willClip) {
                safe->writeExport(task, channels, range, protectedProject);
                return;
            }
            if (task->ipc) {
                if (task->allowClip) {
                    safe->writeExport(task, channels, range, protectedProject);
                    return;
                }
                if (auto* result = safe->lastIpcExport.getDynamicObject()) {
                    result->setProperty("state", "failed");
                    result->setProperty("phase", "finished");
                    result->setProperty("progress", 1.0);
                    result->setProperty("file_written", false);
                    result->setProperty(
                        "error",
                        "PCM export would clip; reduce gain or explicitly use allow-clip.");
                }
                safe->activeExport.reset();
                safe->status.setText("PCM export rejected because it would clip.",
                                     juce::dontSendNotification);
                safe->menuItemsChanged();
                safe->resized();
                return;
            }

            task->phase.store(2);
            const auto db = juce::Decibels::gainToDecibels(peak);
            juce::String details = "The rendered sample peak is " + juce::String(db, 2) + " dBFS. ";
            for (int ch = 0; ch < channels; ++ch) {
                if (preflight.clippedSamples[ch] > 0) {
                    details += "\n" +
                               juce::String(channels == 1 ? "Mono"
                                            : ch == 0     ? "Left"
                                                          : "Right") +
                               ": " + juce::String(preflight.clippedSamples[ch]) +
                               " clipped samples; first at project sample " +
                               juce::String(preflight.firstClipped[ch]) + ".";
                }
            }
            const double margin =
                (opts.dither ? 2.0 : 1.0) /
                (opts.encoding == carta::WavEncoding::pcm16 ? 32768.0 : 8388608.0);
            const double reduction =
                std::ceil(
                    std::max(0.0, 20.0 * std::log10(static_cast<double>(peak) / (1.0 - margin))) *
                    100.0) /
                100.0;
            details += "\n\nReduce gain by at least " + juce::String(reduction, 2) +
                       " dB to provide sample-peak headroom" + (opts.dither ? " for dither" : "") +
                       ", then check again. This is not a true-peak measurement.";

            safe->showGuardedStudioAlert(
                juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                    .withTitle("PCM export will clip")
                    .withMessage(details + "\n\nExport anyway?")
                    .withButton("Export anyway")
                    .withButton("Cancel")
                    .withAssociatedComponent(safe.getComponent()),
                [safe, task, channels, range, protectedProject](int answer) {
                    if (safe == nullptr || safe->activeExport != task) {
                        return;
                    }
                    if (answer == 0 && !task->cancelled.load()) {
                        safe->writeExport(task, channels, range, protectedProject);
                    } else {
                        safe->activeExport.reset();
                        safe->status.setText("WAV export cancelled.", juce::dontSendNotification);
                    }
                });
        });
    });
}

void Studio::writeExport(std::shared_ptr<ExportTask> task, int channels, carta::WavRange range,
                         juce::File protectedProject) {
    task->phase.store(1);
    task->progress.store(0);
    exportPool.addJob([safe = juce::Component::SafePointer<Studio>(this), task, channels, range,
                       protectedProject] {
        bool clipped = false;
        bool ok = false;
        auto directorySync = carta::DirectorySyncStatus::unsupported;
        juce::String error;
        carta::TrackBatchResult batch;
        try {
            const auto progress = [task](double value) {
                task->progress.store(value);
#if CARTA_STUDIO_COMPONENT_TEST
                if (task->writeProgressHook && !task->writeProgressHook(value)) {
                    task->cancelled.store(true);
                }
#endif
                return !task->cancelled.load();
            };
            if (task->discProgram) {
                ok = carta::exportDiscProgramWav(task->snapshot, channels, task->target, error,
                                                 protectedProject, progress, task->noReplace,
                                                 &directorySync);
            } else if (task->batch) {
                ok = carta::exportTrackWavs(task->snapshot, channels, task->target, batch, error,
                                            protectedProject, progress, task->wavOptions);
                clipped = batch.clipped;
            } else {
                ok = carta::exportWavRange(task->snapshot, channels, range, task->target, clipped,
                                           error, protectedProject, progress, task->wavOptions,
                                           task->noReplace, &directorySync);
            }
        } catch (const std::exception& e) {
            error = "Export failed: " + juce::String(e.what());
        } catch (...) {
            error = "Export failed.";
        }

        juce::MessageManager::callAsync([safe, task, clipped, ok, error, batch, directorySync] {
            if (safe == nullptr || safe->activeExport != task) {
                return;
            }
            safe->activeExport.reset();
            const auto sync = task->batch ? batch.directorySync : directorySync;
            const auto stale =
                task->snapshotGeneration != safe->editGeneration || safe->hasPendingGestureEdit();
            const auto staleMessage =
                stale && (ok || (task->batch && batch.completed > 0))
                    ? " Export used an earlier project snapshot; current edits are not included."
                    : "";
            const auto syncText = directorySyncToIpcString(sync);
            if (task->ipc && (ok || (task->batch && batch.directory != juce::File{}))) {
                if (auto* result = safe->lastIpcExport.getDynamicObject()) {
                    result->setProperty("directory_sync", syncText);
                }
            }
            if (task->ipc) {
                if (auto* result = safe->lastIpcExport.getDynamicObject()) {
                    result->setProperty("state", ok                       ? "complete"
                                                 : task->cancelled.load() ? "cancelled"
                                                                          : "failed");
                    result->setProperty("phase", "finished");
                    result->setProperty("progress",
                                        ok ? 1.0 : juce::jlimit(0.0, 1.0, task->progress.load()));
                    result->setProperty("cancellation_requested", task->cancelled.load());
                    result->setProperty("file_written", task->batch ? batch.completed > 0 : ok);
                    result->setProperty("clipped", task->batch ? batch.clipped : clipped);
                    if (task->batch) {
                        result->setProperty("completed_tracks", batch.completed);
                        result->setProperty("manifest_written", ok);
                        if (batch.directory != juce::File{}) {
                            result->setProperty("output_folder", batch.directory.getFullPathName());
                        }
                    }
                    if (!ok) {
                        result->setProperty("error", error);
                    }
                }
            }
            if (task->batch) {
                const auto location = batch.directory == juce::File{}
                                          ? juce::String()
                                          : " Folder: " + batch.directory.getFileName() + " (" +
                                                batch.directory.getFullPathName() + ").";
                const auto result =
                    ok ? "Exported " + juce::String(batch.completed) + " tracks and manifest." +
                             (clipped ? juce::String(" PCM samples were clipped.")
                              : task->wavOptions.encoding == carta::WavEncoding::float32 &&
                                      task->peak > 1.0f
                                  ? juce::String(" Float levels above 0 dBFS are preserved.")
                                  : juce::String())
                       : error + " Completed tracks: " + juce::String(batch.completed) + ".";
                safe->status.setText(
                    result + location + staleMessage +
                        (sync == carta::DirectorySyncStatus::unconfirmed
                             ? " Directory synchronization failed; output was written but "
                               "durability is unconfirmed."
                             : ""),
                    juce::dontSendNotification);
            } else {
                const auto message =
                    ok ? (task->discProgram
                              ? "Exported " + task->target.getFileName() + " with disc track cues."
                          : clipped ? juce::String("Exported; PCM samples were clipped.")
                          : task->wavOptions.encoding == carta::WavEncoding::float32 &&
                                  task->peak > 1.0f
                              ? juce::String(
                                    "Exported float WAV; levels above 0 dBFS are preserved.")
                              : "Exported " + task->target.getFileName() +
                                    (task->wavOptions.embedMarkers
                                         ? ". Included markers are embedded as WAV cues."
                                         : ". Markers are not embedded."))
                       : error;
                safe->status.setText(
                    message + staleMessage +
                        (ok && sync == carta::DirectorySyncStatus::unconfirmed
                             ? " Directory synchronization failed; output was written but "
                               "durability is unconfirmed."
                             : ""),
                    juce::dontSendNotification);
            }
        });
    });
}

void Studio::exportManifest() {
    if (!pendingImports.empty()) {
        status.setText("Wait for imports to finish or cancel them before exporting.",
                       juce::dontSendNotification);
        return;
    }
    {
        const juce::ScopedLock guard(lock);
        if (session.length() <= 0) {
            status.setText("Import audio before exporting a track manifest.",
                           juce::dontSendNotification);
            return;
        }
    }
    launchStudioChooser(
        "Export track manifest",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory)
            .getChildFile("Carta tracks.json"),
        "*.json",
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<Studio>(this)](const StudioFileSelection& fc) {
            if (safe == nullptr || fc.getResult() == juce::File{}) {
                return;
            }
            const auto target = fc.getResult();
            if (safe->activeExport &&
                carta::pathsReferToSameFile(safe->activeExport->target, target)) {
                safe->status.setText("Choose a different file while WAV export is running.",
                                     juce::dontSendNotification);
                return;
            }
            if (safe->activeSave && carta::pathsReferToSameFile(safe->activeSave->file, target)) {
                safe->status.setText("Choose an output other than the project being saved.",
                                     juce::dontSendNotification);
                return;
            }
            if (!target.hasFileExtension("json")) {
                safe->status.setText("Choose a filename ending in .json.",
                                     juce::dontSendNotification);
                return;
            }
            carta::Session snapshot;
            int channels;
            {
                const juce::ScopedLock guard(safe->lock);
                snapshot = safe->session;
                channels = safe->masterChannels;
            }
            juce::String error;
            if (!safe->validateRetainedMedia(target, error)) {
                safe->status.setText(error, juce::dontSendNotification);
                return;
            }
            const auto ok =
                carta::exportTrackManifest(snapshot, channels, target, error, safe->projectFile);
            safe->status.setText(ok ? "Exported " + target.getFileName() +
                                          ". Export the matching WAV separately."
                                    : error,
                                 juce::dontSendNotification);
        });
}

void Studio::importMarkers(bool audacity) {
    if (activeMarkerExchange || activeOpen || !pendingImports.empty()) {
        status.setText("Wait for the current marker exchange or loading to finish.",
                       juce::dontSendNotification);
        return;
    }
    if (carta::projectEndSample(session) <= 0) {
        status.setText("Import audio before importing markers.", juce::dontSendNotification);
        return;
    }
    launchStudioChooser(
        audacity ? "Import Audacity point labels (replace markers)"
                 : "Import markers (replace current markers)",
        juce::File{}, audacity ? "*.txt" : "*.json",
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<Studio>(this),
         audacity](const StudioFileSelection& fc) {
            if (safe == nullptr || fc.getResult() == juce::File{}) {
                return;
            }
            if (safe->dragMode != kDragNone) {
                safe->status.setText("Finish or cancel the current drag before importing markers.",
                                     juce::dontSendNotification);
                return;
            }
            juce::String error;
            if (!safe->startMarkerExchange(fc.getResult(), false, audacity, false, error)) {
                safe->status.setText(error, juce::dontSendNotification);
            }
        });
}

void Studio::exportMarkers(bool audacity) {
    if (activeMarkerExchange || activeOpen || !pendingImports.empty()) {
        status.setText("Wait for the current marker exchange or loading to finish.",
                       juce::dontSendNotification);
        return;
    }
    if (carta::projectEndSample(session) <= 0) {
        status.setText("Import audio before exporting markers.", juce::dontSendNotification);
        return;
    }
    launchStudioChooser(
        audacity ? "Export Audacity point labels" : "Export markers",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory)
            .getChildFile(audacity ? "Carta labels.txt" : "Carta markers.json"),
        audacity ? "*.txt" : "*.json",
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
            juce::FileBrowserComponent::warnAboutOverwriting,
        [safe = juce::Component::SafePointer<Studio>(this),
         audacity](const StudioFileSelection& fc) {
            if (safe == nullptr || fc.getResult() == juce::File{}) {
                return;
            }
            if (safe->dragMode != kDragNone) {
                safe->status.setText("Finish or cancel the current drag before exporting markers.",
                                     juce::dontSendNotification);
                return;
            }
            juce::String error;
            if (!safe->startMarkerExchange(fc.getResult(), true, audacity, false, error)) {
                safe->status.setText(error, juce::dontSendNotification);
            }
        });
}

void Studio::confirmSaveBefore(std::function<void()> action) {
    if (savePromptActive || juce::Component::getCurrentlyModalComponent() != nullptr) {
        status.setText("Finish the current dialog first.", juce::dontSendNotification);
        return;
    }
    if (dragMode != kDragNone) {
        status.setText("Finish or cancel the current drag first.", juce::dontSendNotification);
        return;
    }
    if (activeSave) {
        status.setText("Wait for the project save to finish or use File > Cancel saving project.",
                       juce::dontSendNotification);
        return;
    }
    if (activeOpen) {
        status.setText("Wait for project opening to finish or use File > Cancel opening project.",
                       juce::dontSendNotification);
        return;
    }
    if (!pendingImports.empty()) {
        status.setText("Wait for imports to finish or use File > Cancel pending imports first.",
                       juce::dontSendNotification);
        return;
    }
    if (!hasUnsavedChanges()) {
        action();
        return;
    }
    const auto revision = editGeneration;
    const auto loop = savedLoop;
    const auto file = projectFile;
    const bool wasDirty = dirty;
    savePromptActive = true;
    auto completed = [safe = juce::Component::SafePointer<Studio>(this),
                      nextAction = std::move(action), revision, loop, file, wasDirty](int answer) {
        if (safe == nullptr) {
            return;
        }
        safe->savePromptActive = false;
        if (answer != 0 && answer != 1) {
            return;
        }
        if (answer == 1) {
            if (safe->editGeneration != revision || !sameLoop(safe->savedLoop, loop) ||
                safe->projectFile != file || safe->dirty != wasDirty ||
                safe->dragMode != kDragNone || safe->activeOpen || safe->activeSave ||
                !safe->pendingImports.empty()) {
                safe->status.setText(
                    "Project or background work changed; review it before discarding.",
                    juce::dontSendNotification);
                return;
            }
            nextAction();
            return;
        }
        if (answer == 0) {
            safe->saveProject([safe, nextAction](bool saved) {
                if (saved) {
                    juce::MessageManager::callAsync([safe, nextAction] {
                        if (safe != nullptr) {
                            safe->confirmSaveBefore(nextAction);
                        }
                    });
                }
            });
        }
    };

#if JUCE_MAC && !CARTA_STUDIO_COMPONENT_TEST
    carta::showNativeSavePrompt(this, std::move(completed));
#elif CARTA_STUDIO_COMPONENT_TEST
    auto* dialog = new juce::AlertWindow("Unsaved project", "Save your changes before continuing?",
                                         juce::MessageBoxIconType::QuestionIcon);
    dialog->addButton("Save", 1);
    dialog->addButton("Discard", 2);
    dialog->addButton("Cancel", 0);
    dialog->enterModalState(
        true, juce::ModalCallbackFunction::create([done = std::move(completed)](int answer) {
            done(answer == 1 ? 0 : answer == 2 ? 1 : -1);
        }),
        true);
#else
    showStudioAlert(juce::MessageBoxOptions()
                        .withIconType(juce::MessageBoxIconType::QuestionIcon)
                        .withTitle("Unsaved project")
                        .withMessage("Save your changes before continuing?")
                        .withButton("Save")
                        .withButton("Discard")
                        .withButton("Cancel")
                        .withAssociatedComponent(this),
                    std::move(completed));
#endif
}

void Studio::newProject() {
    confirmSaveBefore([safe = juce::Component::SafePointer<Studio>(this)] {
        if (safe == nullptr) {
            return;
        }
        safe->retireRecovery();
        {
            const juce::ScopedLock guard(safe->lock);
            safe->playing = false;
            safe->position = 0;
            safe->auditionActive = false;
            safe->savedLoop = {};
            safe->persistedLoop = {};
            safe->timeSelection = {};
            safe->selectionMemory = {};
            safe->zoomMemory.clear();
            safe->session = {};
            safe->editHistory.reset();
            ++safe->editGeneration;
            safe->refreshCommittedMasterRange();
            safe->selectedTrack = safe->selectedClip = safe->selectedMarker = -1;
            safe->firstTrack = 0;
            safe->zoom = 1;
            safe->viewStart = 0;
            safe->masterChannels = 2;
            safe->dirty = false;
            safe->pendingDurability = false;
            safe->projectFile = juce::File{};
            safe->projectIsPackage = false;
        }
        safe->refreshDiscWorkspace(true);
        safe->mode.setSelectedId(2, juce::dontSendNotification);
        safe->status.setText("New project", juce::dontSendNotification);
        safe->menuItemsChanged();
        safe->repaint();
    });
}

void Studio::exportPortableProject() {
    if (activeSave || activeOpen || !pendingImports.empty()) {
        status.setText("Wait for current loading or saving to finish.", juce::dontSendNotification);
        return;
    }
    launchStudioChooser(
        "Choose a parent folder for the portable project",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*",
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safe = juce::Component::SafePointer<Studio>(this)](const StudioFileSelection& fc) {
            if (safe == nullptr || fc.getResult() == juce::File{}) {
                return;
            }
            const auto parent = fc.getResult();
            if (!parent.isDirectory()) {
                safe->status.setText("Choose an existing parent folder.",
                                     juce::dontSendNotification);
                return;
            }
            if (safe->activeSave || safe->activeOpen || !safe->pendingImports.empty()) {
                safe->status.setText("Wait for current loading or saving to finish.",
                                     juce::dontSendNotification);
                return;
            }
            carta::Session snapshot;
            int channels = 2;
            juce::uint64 revision = 0;
            carta::WavRange loop;
            {
                const juce::ScopedLock guard(safe->lock);
                if (safe->dragMode != kDragNone) {
                    safe->status.setText(
                        "Finish the current drag before exporting a portable copy.",
                        juce::dontSendNotification);
                    return;
                }
                channels = safe->masterChannels;
                revision = safe->editHistory.revision();
                loop = safe->savedLoop;
                safe->copyWorkerSnapshot(snapshot);
            }
            auto task = std::make_shared<SaveTask>();
            task->file = parent;
            task->revision = revision;
            task->savedLoop = loop;
            task->portable = true;
            task->snapshot = std::move(snapshot);
            safe->activeSave = task;
            safe->status.setText("Copying and verifying project media...",
                                 juce::dontSendNotification);

            safe->savePool.addJob([safe, task, channels] {
                juce::String error;
                juce::File directory;
                bool ok = false;
                try {
                    const auto report = [task](double value) {
                        task->progress.store(value);
                        return !task->cancelled.load();
                    };
                    ok = carta::exportPortableSession(task->snapshot, channels, task->file,
                                                      directory, error, report, task->savedLoop);
                } catch (const std::exception& e) {
                    error = "Portable project export failed: " + juce::String(e.what());
                } catch (...) {
                    error = "Portable project export failed.";
                }

                juce::MessageManager::callAsync([safe, task, directory, ok, error] {
                    if (safe == nullptr || safe->activeSave != task) {
                        return;
                    }
                    safe->activeSave.reset();
                    if (ok) {
                        bool newer = false;
                        {
                            const juce::ScopedLock guard(safe->lock);
                            newer = safe->editHistory.revision() != task->revision ||
                                    !sameLoop(safe->savedLoop, task->savedLoop);
                        }
                        safe->status.setText(
                            "Portable project copy: " + directory.getFullPathName() + "." +
                                (newer ? " Current edits are newer." : ""),
                            juce::dontSendNotification);
                    } else {
                        const auto message =
                            error + (directory != juce::File{} ? "\n\nPartial files remain in: " +
                                                                     directory.getFullPathName()
                                                               : juce::String());
                        safe->status.setText(error, juce::dontSendNotification);
                        if (directory != juce::File{}) {
                            safe->showGuardedStudioAlert(
                                juce::MessageBoxOptions()
                                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                                    .withTitle("Portable project incomplete")
                                    .withMessage(message)
                                    .withButton("OK")
                                    .withAssociatedComponent(safe.getComponent()));
                        }
                    }
                });
            });
        });
}

void Studio::saveProject(std::function<void(bool)> completion, bool saveAs, bool ipc,
                         juce::File explicitFile, bool noReplace, bool forcePackage) {
    if (activeSave) {
        status.setText("A project save is already running.", juce::dontSendNotification);
        if (completion) {
            completion(false);
        }
        return;
    }
    if (activeOpen) {
        status.setText("Wait for project opening to finish or cancel it before saving.",
                       juce::dontSendNotification);
        if (completion) {
            completion(false);
        }
        return;
    }
    if (!pendingImports.empty()) {
        status.setText("Wait for imports to finish or cancel them before saving.",
                       juce::dontSendNotification);
        if (completion) {
            completion(false);
        }
        return;
    }
    if (!ipc) {
        discWorkspace.commitPendingEdits();
    }

    const auto write = [safe = juce::Component::SafePointer<Studio>(this), completion, ipc,
                        noReplace, forcePackage, saveAs](const juce::File& file) {
        if (safe == nullptr) {
            return;
        }
        if (file == juce::File{}) {
            if (completion) {
                completion(false);
            }
            return;
        }
        if (safe->activeExport && carta::pathsReferToSameFile(safe->activeExport->target, file)) {
            safe->status.setText("Choose a different project file while WAV export is running.",
                                 juce::dontSendNotification);
            if (completion) {
                completion(false);
            }
            return;
        }
        if (!file.hasFileExtension("carta")) {
            safe->status.setText("Choose a filename ending in .carta.", juce::dontSendNotification);
            if (completion) {
                completion(false);
            }
            return;
        }
        if ((forcePackage || saveAs) && !safe->projectIsPackage &&
            safe->projectFile != juce::File{} &&
            carta::pathsReferToSameFile(file, safe->projectFile)) {
            safe->status.setText("Choose a different file to keep the original project.",
                                 juce::dontSendNotification);
            if (completion) {
                completion(false);
            }
            return;
        }
        if (safe->activeSave || safe->activeOpen || !safe->pendingImports.empty()) {
            safe->status.setText("Wait for current loading or saving to finish.",
                                 juce::dontSendNotification);
            if (completion) {
                completion(false);
            }
            return;
        }
        carta::Session snapshot;
        int channels;
        juce::uint64 revision;
        carta::WavRange loop;
        {
            const juce::ScopedLock guard(safe->lock);
            if (safe->dragMode != kDragNone) {
                safe->status.setText("Finish the current drag before saving.",
                                     juce::dontSendNotification);
                if (completion) {
                    completion(false);
                }
                return;
            }
            channels = safe->masterChannels;
            revision = safe->editHistory.revision();
            loop = safe->savedLoop;
            safe->copyWorkerSnapshot(snapshot);
        }
        const bool containsPackageMedia = sessionHasPackageMedia(snapshot);
        if (containsPackageMedia && !safe->projectIsPackage && safe->projectFile != juce::File{} &&
            carta::pathsReferToSameFile(file, safe->projectFile)) {
            safe->status.setText(
                "This project contains package media. Save to a different file to keep the "
                "original project.",
                juce::dontSendNotification);
            if (completion) {
                completion(false);
            }
            return;
        }
        juce::String error;
        if (!safe->validateRetainedMedia(file, error)) {
            safe->status.setText(error, juce::dontSendNotification);
            if (completion) {
                completion(false);
            }
            return;
        }

        auto task = std::make_shared<SaveTask>();
        task->file = file;
        task->revision = revision;
        task->savedLoop = loop;
        task->ipc = ipc;
        task->noReplace = noReplace;
        task->nativePackage = forcePackage || saveAs || safe->projectFile == juce::File{} ||
                              safe->projectIsPackage || containsPackageMedia;
        task->snapshot = std::move(snapshot);
        safe->activeSave = task;
        safe->status.setText("Saving " + file.getFileName() + "...", juce::dontSendNotification);

        safe->savePool.addJob([safe, task, channels, completion] {
            juce::String saveError;
            bool ok = false;
            auto directorySync = carta::DirectorySyncStatus::unsupported;
            const auto progress = [task](double value) {
                task->progress.store(value);
                return !task->cancelled.load();
            };
            try {
                ok = task->nativePackage
                         ? carta::saveNativePackage(task->snapshot, channels, task->file, saveError,
                                                    progress, &directorySync, task->savedLoop,
                                                    task->noReplace, false)
                     : task->noReplace
                         ? carta::saveSessionCopy(task->snapshot, channels, task->file, saveError,
                                                  progress, &directorySync, task->savedLoop)
                         : carta::saveSession(task->snapshot, channels, task->file, saveError,
                                              progress, &directorySync, task->savedLoop);
            } catch (const std::exception& e) {
                saveError = "Project save failed: " + juce::String(e.what());
            } catch (...) {
                saveError = "Project save failed.";
            }

            juce::MessageManager::callAsync([safe, task, completion, ok, saveError, directorySync] {
                if (safe == nullptr || safe->activeSave != task) {
                    return;
                }
                safe->activeSave.reset();
                bool clean = false;
                if (ok) {
                    const juce::ScopedLock guard(safe->lock);
                    safe->pendingDurability =
                        directorySync == carta::DirectorySyncStatus::unconfirmed;
                    if (!safe->pendingDurability) {
                        safe->editHistory.markSaved(task->revision);
                        safe->persistedLoop = task->savedLoop;
                    }
                    const bool editing = safe->hasPendingGestureEdit();
                    safe->dirty = safe->editHistory.isDirty() || editing ||
                                  safe->pendingDurability || safe->loopDirty();
                    safe->projectFile = task->file;
                    safe->projectIsPackage = task->nativePackage;
                    clean = !safe->dirty && safe->pendingImports.empty();
                }
                if (ok && clean) {
                    safe->retireRecovery();
                }
                safe->status.setText(
                    ok ? (safe->pendingDurability ? "Project written, but directory "
                                                    "synchronization failed. Save again; "
                                                    "recovery is retained."
                          : clean ? "Saved " + task->file.getFileName() +
                                        (task->nativePackage
                                             ? "."
                                             : ". Keep source audio files with the project.")
                                  : "Saved an earlier version; current edits are not saved.")
                       : saveError,
                    juce::dontSendNotification);
                if (ok) {
                    safe->refreshDiscWorkspace(true);
                }
                if (task->ipc) {
                    if (auto* result = safe->lastIpcProject.getDynamicObject()) {
                        result->setProperty("state", ok                       ? "complete"
                                                     : task->cancelled.load() ? "cancelled"
                                                                              : "failed");
                        result->setProperty(
                            "progress", ok ? 1.0 : juce::jlimit(0.0, 1.0, task->progress.load()));
                        result->setProperty("cancellation_requested", task->cancelled.load());
                        result->setProperty("file_written", ok);
                        result->setProperty("clean", clean);
                        if (ok) {
                            result->setProperty("native_package", task->nativePackage);
                            result->setProperty("directory_sync",
                                                directorySyncToIpcString(directorySync));
                        } else {
                            result->setProperty("error", saveError);
                        }
                    }
                }
                if (completion) {
                    completion(ok && clean);
                }
            });
        });
    };

    if (explicitFile != juce::File{}) {
        write(explicitFile);
        return;
    }
    if (!saveAs && projectFile != juce::File{}) {
        write(projectFile);
        return;
    }
    const auto suggested =
        projectFile != juce::File{}
            ? ((forcePackage || saveAs) && !projectIsPackage
                   ? projectFile.getSiblingFile(projectFile.getFileNameWithoutExtension() +
                                                " portable.carta")
                   : projectFile)
            : juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                  .getChildFile("Untitled.carta");
    launchStudioChooser("Save project", suggested, "*.carta",
                        juce::FileBrowserComponent::saveMode |
                            juce::FileBrowserComponent::canSelectFiles |
                            juce::FileBrowserComponent::warnAboutOverwriting,
                        [write](const StudioFileSelection& fc) {
                            const auto file = fc.getResult();
                            write(file);
                        });
}

void Studio::retireRecovery(bool preserveCurrent) {
    const auto retiredFile = recoveryFile;
    if (activeRecovery && activeRecovery->file == retiredFile) {
        activeRecovery->retired.store(true);
        activeRecovery->cancelled.store(true);
    }
    if (!preserveCurrent && retiredFile != juce::File{}) {
        retiredFile.deleteFile();
    }
    recoveryFile = newRecoveryPath();
    hasRecoveryRevision = false;
    recoveryRevision = 0;
    recoveryLoop = {};
    lastEditTime = lastRecoveryAttempt = juce::Time::getMillisecondCounterHiRes();
}

void Studio::maybeAutosave() {
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (observedGeneration != editGeneration) {
        observedGeneration = editGeneration;
        lastEditTime = now;
    }
    if (!dirty &&
        (hasRecoveryRevision || (activeRecovery && activeRecovery->file == recoveryFile))) {
        retireRecovery();
        return;
    }
    if (!dirty || activeRecovery || activeOpen || dragMode != kDragNone ||
        (hasRecoveryRevision && recoveryRevision == editHistory.revision() &&
         sameLoop(recoveryLoop, savedLoop))) {
        return;
    }
    if (now - lastRecoveryAttempt >= 20000.0 &&
        (now - lastEditTime >= 5000.0 || now - lastRecoveryAttempt >= 30000.0)) {
        startRecoverySnapshot(false);
    }
}

void Studio::startRecoverySnapshot(bool announce) {
    if (activeRecovery || activeOpen) {
        if (announce) {
            status.setText("Wait for the current recovery or project opening to finish.",
                           juce::dontSendNotification);
        }
        return;
    }
    if (activeMarkerExchange &&
        carta::pathsReferToSameFile(activeMarkerExchange->file, recoveryFile)) {
        if (announce) {
            status.setText("Wait for the marker exchange using the recovery file to finish.",
                           juce::dontSendNotification);
        }
        return;
    }
    carta::Session snapshot;
    int channels = 2;
    juce::uint64 revision = 0;
    carta::WavRange loop;
    {
        const juce::ScopedLock guard(lock);
        if (!dirty || dragMode != kDragNone) {
            if (announce) {
                status.setText("No completed unsaved edit to snapshot.",
                               juce::dontSendNotification);
            }
            return;
        }
        channels = masterChannels;
        revision = editHistory.revision();
        loop = savedLoop;
        copyWorkerSnapshot(snapshot);
    }
    juce::String error;
    std::vector<juce::File> reading;
    for (const auto& pending : pendingImports) {
        reading.push_back(pending->file);
    }
    if (activeSourceCheck) {
        for (const auto& source : activeSourceCheck->sources) {
            if (source.audio) {
                reading.push_back(source.audio->file);
            }
        }
        if (activeSourceCheck->relinking) {
            reading.push_back(activeSourceCheck->relinkFile);
        }
    }
    if (!carta::validateRetainedMediaDestination(
            snapshot, editHistory.undoSessions(), editHistory.redoSessions(), clipboard,
            recoveryFile, error,
            {activeSave ? &activeSave->snapshot : nullptr,
             activeExport ? &activeExport->snapshot : nullptr,
             activeExport ? &activeExport->verificationSnapshot : nullptr,
             activeLevelAnalysis ? &activeLevelAnalysis->snapshot : nullptr,
             activeMarkerExchange ? &activeMarkerExchange->snapshot : nullptr},
            reading)) {
        status.setText(error, juce::dontSendNotification);
        return;
    }

    auto task = std::make_shared<RecoveryTask>();
    task->file = recoveryFile;
    task->revision = revision;
    task->savedLoop = loop;
    task->announce = announce;
    task->nativePackage =
        projectFile == juce::File{} || projectIsPackage || sessionHasPackageMedia(snapshot);
    task->snapshot = std::move(snapshot);
    activeRecovery = task;
    lastRecoveryAttempt = juce::Time::getMillisecondCounterHiRes();
    menuItemsChanged();
    if (announce) {
        status.setText("Saving recovery snapshot...", juce::dontSendNotification);
    }

    recoveryPool.addJob([safe = juce::Component::SafePointer<Studio>(this), task, channels] {
        juce::String recoveryError;
        bool ok = false;
        auto directorySync = carta::DirectorySyncStatus::unsupported;
        try {
            const auto created = task->file.getParentDirectory().createDirectory();
            if (created.failed()) {
                recoveryError =
                    "Could not create the recovery folder: " + created.getErrorMessage();
            } else if (task->nativePackage) {
                ok = carta::saveNativePackage(
                    task->snapshot, channels, task->file, recoveryError,
                    [task](double) { return !task->cancelled.load(); }, &directorySync,
                    task->savedLoop, false, true);
            } else {
                ok = carta::saveRecoverySession(
                    task->snapshot, channels, task->file, recoveryError,
                    [task](double) { return !task->cancelled.load(); }, &directorySync,
                    task->savedLoop);
            }
        } catch (const std::exception& e) {
            recoveryError = juce::String(e.what());
        } catch (...) {
            recoveryError = "Unknown error.";
        }
        if (task->retired.load()) {
            task->file.deleteFile();
        }
        juce::MessageManager::callAsync([safe, task, ok, recoveryError, directorySync] {
            if (safe == nullptr || safe->activeRecovery != task) {
                return;
            }
            safe->activeRecovery.reset();
            safe->menuItemsChanged();
            if (task->retired.load() || task->file != safe->recoveryFile) {
                return;
            }
            if (ok && directorySync != carta::DirectorySyncStatus::unconfirmed) {
                safe->hasRecoveryRevision = true;
                safe->recoveryRevision = task->revision;
                safe->recoveryLoop = task->savedLoop;
                if (task->announce) {
                    safe->status.setText("Recovery snapshot saved.", juce::dontSendNotification);
                }
            } else if (ok) {
                safe->status.setText(
                    "Recovery snapshot written, but directory synchronization failed; will retry.",
                    juce::dontSendNotification);
            } else if (!task->cancelled.load()) {
                safe->status.setText("Recovery snapshot failed: " + recoveryError,
                                     juce::dontSendNotification);
            }
        });
    });
}

void Studio::recoverProject() {
    confirmSaveBefore([safe = juce::Component::SafePointer<Studio>(this)] {
        if (safe == nullptr) {
            return;
        }
        const auto files = safe->recoveryDirectory.findChildFiles(juce::File::findFiles, false,
                                                                  "*.recovery.carta");
        if (files.isEmpty()) {
            safe->status.setText("No recovery snapshots found.", juce::dontSendNotification);
            return;
        }
        safe->launchStudioChooser("Recover autosaved session", safe->recoveryDirectory, "*.carta",
                                  juce::FileBrowserComponent::openMode |
                                      juce::FileBrowserComponent::canSelectFiles,
                                  [safe](const StudioFileSelection& fc) {
                                      if (safe == nullptr || fc.getResult() == juce::File{}) {
                                          return;
                                      }
                                      safe->beginProjectOpen(fc.getResult(), true);
                                  });
    });
}

void Studio::loadProject() {
    confirmSaveBefore([safe = juce::Component::SafePointer<Studio>(this)] {
        if (safe == nullptr) {
            return;
        }
        safe->launchStudioChooser(
            "Open project", juce::File{}, "*.carta",
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safe](const StudioFileSelection& fc) {
                if (safe == nullptr || fc.getResult() == juce::File{}) {
                    return;
                }
                const auto file = fc.getResult();
                if (safe->activeExport &&
                    carta::pathsReferToSameFile(safe->activeExport->target, file)) {
                    safe->status.setText("Choose a different project while WAV export is running.",
                                         juce::dontSendNotification);
                    return;
                }
                safe->beginProjectOpen(file);
            });
    });
}

void Studio::beginProjectOpen(const juce::File& file, bool requireRecovery, bool ipc,
                              carta::SourceRelinks relinks) {
    if ((activeExport && carta::pathsReferToSameFile(activeExport->target, file)) ||
        (activeMarkerExchange && activeMarkerExchange->writing &&
         carta::pathsReferToSameFile(activeMarkerExchange->file, file))) {
        status.setText("Wait for the current output to finish before opening it.",
                       juce::dontSendNotification);
        return;
    }
    if (activeRecovery && carta::pathsReferToSameFile(activeRecovery->file, file)) {
        status.setText("Wait for the recovery snapshot to finish.", juce::dontSendNotification);
        return;
    }
    if (activeOpen || activeSave || !pendingImports.empty()) {
        status.setText("Wait for current loading or saving to finish.", juce::dontSendNotification);
        return;
    }
    auto task = std::make_shared<OpenTask>();
    task->file = file;
    task->requireRecovery = requireRecovery;
    task->ipc = ipc;
    task->relinks = std::move(relinks);
    {
        const juce::ScopedLock guard(lock);
        task->generation = editGeneration;
        task->outputRate = deviceRate;
        task->originalLoop = savedLoop;
    }
    activeOpen = task;
    continueProjectOpen(task);
}

void Studio::continueProjectOpen(const std::shared_ptr<OpenTask>& task) {
    if (activeOpen != task) {
        return;
    }
    task->progress.store(0);
    status.setText("Opening " + task->file.getFileName() + "...", juce::dontSendNotification);

    openPool.addJob([safe = juce::Component::SafePointer<Studio>(this), task] {
        auto result = std::make_shared<OpenResult>();
        juce::String error;
        bool ok = false;
        try {
            juce::AudioFormatManager manager;
            manager.registerBasicFormats();
            result->nativePackage = carta::isNativePackage(task->file);
            const auto report = [task](double progress) {
                task->progress.store(progress * 0.95);
                return !task->cancelled.load();
            };
            if (result->nativePackage && !task->relinks.empty()) {
                error = "Package media cannot be relinked externally.";
                ok = false;
            } else if (result->nativePackage) {
                ok =
                    carta::loadNativePackage(result->session, result->channels, manager, task->file,
                                             error, report, &result->savedLoop, &result->recovered);
            } else {
                ok = carta::loadSession(result->session, result->channels, manager, task->file,
                                        error, report, &result->recovered, &task->relinks,
                                        &result->relinkRequest, &result->relinked,
                                        &result->assignedMarkerIds, &result->savedLoop);
            }
            if (ok && task->requireRecovery && !result->recovered) {
                ok = false;
                error = "This file is not a Carta recovery snapshot.";
            }
            if (ok && std::isfinite(task->outputRate) && task->outputRate > 0) {
                for (const auto& track : result->session.tracks) {
                    for (const auto& clip : track.clips) {
                        if (task->cancelled.load()) {
                            ok = false;
                            error = "Project opening cancelled.";
                            break;
                        }
                        const auto rate = clip.audio->sampleRate;
                        if (result->kernels.find(rate) == result->kernels.end()) {
                            result->kernels.emplace(rate, std::make_shared<carta::SincResampler>(
                                                              rate, task->outputRate));
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            ok = false;
            error = "Project opening failed: " + juce::String(e.what());
        } catch (...) {
            ok = false;
            error = "Project opening failed.";
        }
        if (ok) {
            task->progress.store(1);
        }

        juce::MessageManager::callAsync([safe, task, result, ok, error]() mutable {
            if (safe == nullptr || safe->activeOpen != task) {
                return;
            }
            auto finishIpc = [safe, task](bool success, const juce::String& message) {
                if (task->ipc) {
                    if (auto* resultState = safe->lastIpcProject.getDynamicObject()) {
                        resultState->setProperty("state", success                  ? "complete"
                                                          : task->cancelled.load() ? "cancelled"
                                                                                   : "failed");
                        resultState->setProperty(
                            "progress",
                            success ? 1.0 : juce::jlimit(0.0, 1.0, task->progress.load()));
                        resultState->setProperty("cancellation_requested", task->cancelled.load());
                        if (!success) {
                            resultState->setProperty("error", message);
                        }
                    }
                }
            };

            if (task->cancelled.load()) {
                finishIpc(false, "Project opening cancelled.");
                safe->activeOpen.reset();
                safe->status.setText("Project opening cancelled.", juce::dontSendNotification);
                return;
            }
            if (task->ipc && !ok && result->relinkRequest.expectedHash.isNotEmpty()) {
                if (auto* resultState = safe->lastIpcProject.getDynamicObject()) {
                    auto* missing = new juce::DynamicObject();
                    missing->setProperty("original",
                                         result->relinkRequest.original.getFullPathName());
                    missing->setProperty("sha256", result->relinkRequest.expectedHash);
                    resultState->setProperty("relink_request", juce::var(missing));
                }
                finishIpc(false,
                          error + " Supply a checksum-matching relink when opening this project.");
                safe->activeOpen.reset();
                safe->status.setText(error, juce::dontSendNotification);
                return;
            }
            if (!ok && result->relinkRequest.expectedHash.isNotEmpty()) {
                {
                    const juce::ScopedLock guard(safe->lock);
                    if (safe->editGeneration != task->generation ||
                        !sameLoop(safe->savedLoop, task->originalLoop) ||
                        safe->dragMode != kDragNone) {
                        safe->activeOpen.reset();
                        safe->status.setText(
                            "Project changed while opening; current edits were kept.",
                            juce::dontSendNotification);
                        return;
                    }
                }
                const auto original = result->relinkRequest.original;
                safe->status.setText(
                    error + " Choose the original audio to relink; its checksum must match.",
                    juce::dontSendNotification);
                safe->launchStudioChooser(
                    "Relink " + original.getFileName() + " (must match saved checksum)",
                    original.getParentDirectory(), "*",
                    juce::FileBrowserComponent::openMode |
                        juce::FileBrowserComponent::canSelectFiles,
                    [safe, task, original](const StudioFileSelection& fc) {
                        if (safe == nullptr || safe->activeOpen != task) {
                            return;
                        }
                        if (task->cancelled.load() || fc.getResult() == juce::File{}) {
                            safe->activeOpen.reset();
                            safe->status.setText(
                                "Project opening cancelled; current session was kept.",
                                juce::dontSendNotification);
                            return;
                        }
                        {
                            const juce::ScopedLock guard(safe->lock);
                            if (safe->editGeneration != task->generation ||
                                !sameLoop(safe->savedLoop, task->originalLoop) ||
                                safe->dragMode != kDragNone) {
                                safe->activeOpen.reset();
                                safe->status.setText(
                                    "Project changed while opening; current edits were kept.",
                                    juce::dontSendNotification);
                                return;
                            }
                        }
                        task->relinks[original.getFullPathName()] = fc.getResult();
                        safe->continueProjectOpen(task);
                    });
                return;
            }

            safe->activeOpen.reset();
            if (!ok) {
                finishIpc(false, error);
                safe->status.setText(error, juce::dontSendNotification);
                return;
            }
            if (safe->activeExport &&
                !carta::validateOutputDestination(result->session, safe->activeExport->target,
                                                  task->file, error)) {
                finishIpc(false, "Cannot open a project referencing the WAV being exported.");
                safe->status.setText("Cannot open a project referencing the WAV being exported.",
                                     juce::dontSendNotification);
                return;
            }
            {
                const juce::ScopedLock guard(safe->lock);
                if (safe->editGeneration != task->generation ||
                    !sameLoop(safe->savedLoop, task->originalLoop) || safe->dragMode != kDragNone) {
                    finishIpc(false, "Project changed while opening; current edits were kept.");
                    safe->status.setText(
                        "Project changed while opening; current edits were kept. Open it again "
                        "when ready.",
                        juce::dontSendNotification);
                    return;
                }
                if (std::abs(safe->deviceRate - task->outputRate) > 0.01) {
                    finishIpc(false, "Audio device changed while opening.");
                    safe->status.setText(
                        "Audio device changed while opening; please open the project again.",
                        juce::dontSendNotification);
                    return;
                }
                if (juce::Component::getCurrentlyModalComponent() != nullptr) {
                    finishIpc(false, "Close the editing dialog and open the project again.");
                    safe->status.setText("Close the editing dialog and open the project again.",
                                         juce::dontSendNotification);
                    return;
                }
                for (const auto& entry : result->kernels) {
                    safe->resampling.registerPrepared(entry.first, safe->deviceRate, entry.second);
                }
                // Register rates without constructing kernels if there is no active device.
                if (!(safe->deviceRate > 0)) {
                    for (const auto& track : result->session.tracks) {
                        for (const auto& clip : track.clips) {
                            safe->resampling.registerPrepared(clip.audio->sampleRate, 0, {});
                        }
                    }
                }
                safe->retireRecovery(result->recovered &&
                                     carta::pathsReferToSameFile(task->file, safe->recoveryFile));
                safe->playing = false;
                safe->position = 0;
                safe->auditionActive = false;
                safe->savedLoop = result->savedLoop;
                safe->persistedLoop = result->savedLoop;
                safe->timeSelection = {};
                safe->dragMode = kDragNone;
                safe->dragged = false;
                safe->snapGuideVisible = false;
                safe->session = std::move(result->session);
                safe->editHistory.reset();
                ++safe->editGeneration;
                safe->refreshCommittedMasterRange();
                safe->selectedTrack = safe->selectedClip = safe->selectedMarker = -1;
                safe->firstTrack = 0;
                safe->zoom = 1;
                safe->viewStart = 0;
                safe->zoomMemory.clear();
                safe->masterChannels = result->channels;
                safe->pendingDurability = false;
                if (result->recovered) {
                    safe->editHistory.markRecovered();
                    safe->dirty = true;
                    safe->projectFile = juce::File{};
                } else if (result->relinked || result->assignedMarkerIds) {
                    safe->editHistory.markRecovered();
                    safe->dirty = true;
                    safe->projectFile = task->file;
                } else {
                    safe->dirty = false;
                    safe->projectFile = task->file;
                }
                safe->projectIsPackage = result->nativePackage;
            }
            finishIpc(true, {});
            if (task->ipc) {
                if (auto* resultState = safe->lastIpcProject.getDynamicObject()) {
                    resultState->setProperty("marker_ids_need_save", result->assignedMarkerIds);
                    resultState->setProperty("relinked", result->relinked);
                    resultState->setProperty("native_package", result->nativePackage);
                }
            }
            safe->refreshDiscWorkspace(true);
            safe->mode.setSelectedId(result->channels, juce::dontSendNotification);
            const auto openMessage =
                result->recovered
                    ? juce::String("Recovered unsaved session. Use Save As to keep it.")
                : result->relinked && result->assignedMarkerIds
                    ? juce::String(
                          "Opened with replacement audio and new marker identities. Save to keep "
                          "both.")
                : result->relinked
                    ? juce::String(
                          "Opened with verified replacement audio. Save to keep the new source "
                          "paths.")
                : result->assignedMarkerIds
                    ? juce::String(
                          "Assigned marker identities to this older project. Save to keep them.")
                    : "Opened " + task->file.getFileName();
            safe->status.setText(openMessage, juce::dontSendNotification);
            safe->menuItemsChanged();
            safe->repaint();
        });
    });
}
