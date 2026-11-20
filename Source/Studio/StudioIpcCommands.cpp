/**
 * @file StudioIpcCommands.cpp
 * @brief Local IPC JSON command router and domain handlers for `Studio`.
 *
 * Implements `Studio::ipcCommand(const juce::StringArray&)`, which dispatches
 * synchronous automation commands arriving on the JUCE message thread over the
 * local Unix domain socket / Windows named pipe (`carta::IpcServer`).
 *
 * Protocol invariants:
 * - Every response is `{ "ok": true, "result": ... }` or
 *   `{ "ok": false, "error": "..." }`.
 * - Sample coordinates use 44.1 kHz project samples (`carta::sessionRate`) and
 *   half-open `[start, end)` intervals.
 * - Long-running operations (import, open, save, export, analysis, source
 *   verification, marker exchange, PNG capture, NetMD USB jobs) queue onto
 *   background pools and report status/progress via domain `status` commands.
 */

#include "Studio/Studio.h"

namespace {

juce::var ipcReply(bool ok, const juce::var& value) {
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", ok);
    o->setProperty(ok ? "result" : "error", value);
    return juce::var(o);
}

bool parseNonNegativeIpcInt(const juce::String& text, juce::int64& value) {
    const auto bytes = text.toStdString();
    const auto parsed = std::from_chars(bytes.data(), bytes.data() + bytes.size(), value);
    return !bytes.empty() && parsed.ec == std::errc{} &&
           parsed.ptr == bytes.data() + bytes.size() && value >= 0 && value < 9007199254740992LL;
}

juce::DynamicObject* buildUtocBudgetObject(const carta::UtocBudget& budget) {
    auto* b = new juce::DynamicObject();
    b->setProperty("used_clusters", budget.usedClusters);
    b->setProperty("total_clusters", budget.totalClusters);
    b->setProperty("used_tracks", budget.usedTracks);
    b->setProperty("max_tracks", budget.maxTracks);
    b->setProperty("used_fragments", budget.usedFragments);
    b->setProperty("max_fragments", budget.maxFragments);
    b->setProperty("used_half_width_cells", budget.usedHalfWidthCells);
    b->setProperty("max_half_width_cells", budget.maxHalfWidthCells);
    b->setProperty("used_full_width_cells", budget.usedFullWidthCells);
    b->setProperty("max_full_width_cells", budget.maxFullWidthCells);
    b->setProperty("shared_fragment_count", budget.sharedFragmentCount);
    b->setProperty("fits", budget.fits());
    return b;
}

juce::var buildMdDiscStatusJson(const carta::MdDisc& d) {
    carta::MdPhysicalPlan plan;
    std::string planErr;
    const bool planOk = carta::planMdPhysicalAudio(d, plan, planErr);
    carta::UtocDiscState utoc;
    std::string utocErr;
    const bool utocOk = carta::compileMdDiscToUtoc(d, utoc, utocErr);
    const int physTotal = d.loadedDisc.active ? d.loadedDisc.physicalTotalClusters() : 0;
    const auto budget =
        utocOk ? carta::computeUtocBudget(utoc, carta::mdMediumToString(d.medium), physTotal)
               : carta::UtocBudget{};

    auto* b = buildUtocBudgetObject(budget);

    juce::Array<juce::var> spanArr;
    for (size_t spIdx = 0; spIdx < plan.spans.size(); ++spIdx) {
        const auto& sp = plan.spans[spIdx];
        auto* so = new juce::DynamicObject();
        so->setProperty("index", static_cast<int>(spIdx));
        so->setProperty("codec", juce::String(carta::mdCodecModeToString(sp.codec)));
        so->setProperty("start_sample", static_cast<juce::int64>(sp.startSample));
        so->setProperty("end_sample", static_cast<juce::int64>(sp.endSample));
        so->setProperty("unit_count", sp.unitCount);
        so->setProperty("start_cluster", static_cast<int>(sp.startCluster));
        so->setProperty("cluster_count", sp.clusterCount);
        spanArr.add(juce::var(so));
    }

    juce::Array<juce::var> placeArr;
    for (const auto& pl : plan.placements) {
        auto* po = new juce::DynamicObject();
        po->setProperty("track_index", pl.trackIndex);
        po->setProperty("part_index", pl.partIndex);
        po->setProperty("span_index", pl.spanIndex);
        po->setProperty("unit_offset", pl.unitOffsetInSpan);
        po->setProperty("unit_count", pl.unitCount);
        po->setProperty("start_cluster", static_cast<int>(pl.startAddr.cluster));
        po->setProperty("start_sector", static_cast<int>(pl.startAddr.sector));
        po->setProperty("start_group", static_cast<int>(pl.startAddr.group));
        po->setProperty("end_cluster", static_cast<int>(pl.endAddr.cluster));
        po->setProperty("end_sector", static_cast<int>(pl.endAddr.sector));
        po->setProperty("end_group", static_cast<int>(pl.endAddr.group));
        placeArr.add(juce::var(po));
    }

    auto* planObj = new juce::DynamicObject();
    planObj->setProperty("valid", planOk);
    planObj->setProperty("error", juce::String::fromUTF8(planErr.c_str()));
    planObj->setProperty("used_clusters", plan.usedClusters);
    planObj->setProperty("uncoalesced_clusters", plan.uncoalescedClusters);
    planObj->setProperty("saved_clusters", plan.savedClusters);
    planObj->setProperty("total_clusters", plan.totalClusters);
    planObj->setProperty("on_disc_clusters", plan.onDiscClusters);
    planObj->setProperty("new_upload_clusters", plan.newUploadClusters);
    planObj->setProperty("reclaimed_clusters", plan.reclaimedClusters);
    planObj->setProperty("initial_free_clusters", plan.initialFreeClusters);
    planObj->setProperty("free_clusters_after_reclaim", plan.freeClustersAfterReclaim);
    planObj->setProperty("logical_play_samples", static_cast<juce::int64>(plan.logicalPlaySamples));
    planObj->setProperty("unique_physical_samples",
                         static_cast<juce::int64>(plan.uniquePhysicalSamples));
    planObj->setProperty("spans", juce::var(spanArr));
    planObj->setProperty("placements", juce::var(placeArr));

    juce::Array<juce::var> tracksArr;
    for (size_t ti = 0; ti < d.tracks.size(); ++ti) {
        const auto& tr = d.tracks[ti];
        auto* to = new juce::DynamicObject();
        to->setProperty("index", static_cast<int>(ti));
        to->setProperty("id", juce::String::fromUTF8(tr.id.c_str()));
        to->setProperty("half_width_title", juce::String::fromUTF8(tr.halfWidthTitle.c_str()));
        to->setProperty("full_width_title", juce::String::fromUTF8(tr.fullWidthTitle.c_str()));
        to->setProperty("codec_override",
                        tr.codecOverride
                            ? juce::var(juce::String(carta::mdCodecModeToString(*tr.codecOverride)))
                            : juce::var());
        to->setProperty("effective_codec", juce::String(carta::mdCodecModeToString(
                                               carta::effectiveTrackCodec(d, tr))));
        to->setProperty("write_protected", tr.writeProtected);
        to->setProperty("scms", tr.scmsCopyProtected);
        to->setProperty("emphasis", tr.emphasis);
        to->setProperty("timestamp", juce::String::fromUTF8(tr.timestamp.c_str()));
        to->setProperty("total_samples", static_cast<juce::int64>(tr.totalSamples()));

        juce::Array<juce::var> partsArr;
        for (size_t pi = 0; pi < tr.parts.size(); ++pi) {
            auto* pObj = new juce::DynamicObject();
            pObj->setProperty("index", static_cast<int>(pi));
            pObj->setProperty("start_sample", static_cast<juce::int64>(tr.parts[pi].startSample));
            pObj->setProperty("end_sample", static_cast<juce::int64>(tr.parts[pi].endSample));
            partsArr.add(juce::var(pObj));
        }
        to->setProperty("parts", juce::var(partsArr));
        tracksArr.add(juce::var(to));
    }

    juce::Array<juce::var> groupsArr;
    for (size_t gi = 0; gi < d.groups.size(); ++gi) {
        const auto& grp = d.groups[gi];
        auto* go = new juce::DynamicObject();
        go->setProperty("index", static_cast<int>(gi));
        go->setProperty("start_track", grp.startTrack);
        go->setProperty("end_track", grp.endTrack);
        go->setProperty("half_width_title", juce::String::fromUTF8(grp.halfWidthTitle.c_str()));
        go->setProperty("full_width_title", juce::String::fromUTF8(grp.fullWidthTitle.c_str()));
        groupsArr.add(juce::var(go));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty("medium", juce::String(carta::mdMediumToString(d.medium)));
    root->setProperty("default_codec", juce::String(carta::mdCodecModeToString(d.defaultCodec)));
    root->setProperty("auto_track_on_import", d.autoTrackOnImport);
    root->setProperty("half_width_title", juce::String::fromUTF8(d.halfWidthTitle.c_str()));
    root->setProperty("full_width_title", juce::String::fromUTF8(d.fullWidthTitle.c_str()));
    root->setProperty("timestamp", juce::String::fromUTF8(d.discTimestamp.c_str()));
    root->setProperty("track_count", static_cast<int>(d.tracks.size()));
    root->setProperty("group_count", static_cast<int>(d.groups.size()));
    root->setProperty("plan_valid", planOk);
    root->setProperty("plan_error", juce::String::fromUTF8(planErr.c_str()));
    root->setProperty("utoc_valid", utocOk);
    root->setProperty("utoc_error", juce::String::fromUTF8(utocErr.c_str()));
    auto* loadedObj = new juce::DynamicObject();
    loadedObj->setProperty("active", d.loadedDisc.active);
    loadedObj->setProperty("source_device_id",
                           juce::String::fromUTF8(d.loadedDisc.sourceDeviceId.c_str()));
    loadedObj->setProperty("baseline_sector0_hash",
                           juce::String::fromUTF8(d.loadedDisc.baselineSector0Hash.c_str()));
    loadedObj->setProperty("physical_start_cluster",
                           static_cast<int>(d.loadedDisc.physicalStartCluster));
    loadedObj->setProperty("physical_last_cluster",
                           static_cast<int>(d.loadedDisc.physicalLastCluster));
    loadedObj->setProperty("physical_total_clusters", d.loadedDisc.physicalTotalClusters());
    loadedObj->setProperty("region_count", static_cast<int>(d.loadedDisc.regions.size()));
    loadedObj->setProperty("initial_free_range_count",
                           static_cast<int>(d.loadedDisc.initialFreeRanges.size()));
    loadedObj->setProperty("defective_range_count",
                           static_cast<int>(d.loadedDisc.defectiveRanges.size()));

    root->setProperty("budget", juce::var(b));
    root->setProperty("physical_plan", juce::var(planObj));
    root->setProperty("loaded_disc", juce::var(loadedObj));
    root->setProperty("tracks", juce::var(tracksArr));
    root->setProperty("groups", juce::var(groupsArr));
    return juce::var(root);
}

juce::var handleFormatsCommand(const juce::StringArray& args) {
    if (args.size() != 1) {
        return ipcReply(false, "Usage: ipc formats");
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
#if CARTA_STUDIO_MINIMP3
    carta::registerNativeMp3Format(formats);
#endif
    auto* result = new juce::DynamicObject();
    result->setProperty("import_wildcard", formats.getWildcardForAllFormats());
    juce::Array<juce::var> readers;
    for (int i = 0; i < formats.getNumKnownFormats(); ++i) {
        const auto* format = formats.getKnownFormat(i);
        auto* entry = new juce::DynamicObject();
        entry->setProperty("name", format->getFormatName());
        juce::Array<juce::var> extensions;
        for (const auto& extension : format->getFileExtensions()) {
            extensions.add(extension);
        }
        entry->setProperty("extensions", juce::var(extensions));
        readers.add(juce::var(entry));
    }
    result->setProperty("readers", juce::var(readers));
    return ipcReply(true, juce::var(result));
}

juce::var handleSchemaCommand(const juce::StringArray& args) {
    if (args.size() > 2) {
        return ipcReply(false, "Usage: ipc schema [DOMAIN]");
    }
    auto makeDomain = [](const char* summary, std::initializer_list<const char*> cmds) {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("summary", summary);
        juce::Array<juce::var> list;
        for (const char* c : cmds) {
            list.add(juce::String(c));
        }
        obj->setProperty("commands", list);
        return juce::var(obj);
    };

    juce::DynamicObject::Ptr domains(new juce::DynamicObject());
    domains->setProperty(
        "status",
        makeDomain("Inspect global editor state, playhead, selection, and background jobs",
                   {"status"}));
    domains->setProperty("app", makeDomain("Lifecycle and headless daemon management",
                                           {"daemon start|stop|status", "ipc --auto-start ...",
                                            "app quit [force]"}));
    domains->setProperty(
        "project",
        makeDomain("Open, save, export, undo/redo, inspect Carta projects",
                   {"project open FILE", "project save | save-as|package-as|save-copy FILE",
                    "project cancel|status|undo|redo|clear-history|recover"}));
    domains->setProperty(
        "import",
        makeDomain("Queue sequential Layer 1 audio imports (with auto track markers) or place "
                   "onto a specific/new mixing layer",
                   {"import [--layer INDEX | --new-layer] [--at SAMPLE] [--marker | --no-marker] "
                    "FILE [...]",
                    "imports status|cancel|clear-issues"}));
    domains->setProperty(
        "layers",
        makeDomain("Inspect DAW mixing layers, clips, routing, fades, master tracks, and track "
                   "markers",
                   {"layers", "tracks", "master-tracks", "markers"}));
    domains->setProperty(
        "tracks",
        makeDomain("Inspect timeline layers, clips, routing, fades, and markers (alias of layers)",
                   {"layers", "tracks", "master-tracks", "markers"}));
    domains->setProperty(
        "markers",
        makeDomain("Create, move, rename, shift, find, and exchange track markers",
                   {"marker add SAMPLE NAME", "marker set INDEX SAMPLE", "marker rename INDEX NAME",
                    "marker set-positions ...", "marker rename-many ...", "marker shift-many ...",
                    "marker delete ...", "marker delete-selection",
                    "marker import|export json|labels FILE", "marker exchange status|cancel"}));
    domains->setProperty(
        "select",
        makeDomain("Inspect and set timeline time selection, mixing layers, clips, and edit points",
                   {"select tracks clear|INDEX...", "select clips clear|LAYER CLIP...",
                    "select time FIRST LAST START END", "select all", "select track INDEX",
                    "select clip LAYER CLIP", "select clip-range LAYER CLIP",
                    "select point LAYER SAMPLE",
                    "select memory|clear-memory|restore-memory|master-tracks"}));
    domains->setProperty(
        "edit",
        makeDomain(
            "Non-destructive timeline cutting, pasting, silencing, amplification, normalization, "
            "and fades",
            {"edit delete|split|trim|silence|duplicate|copy|cut|paste|undo|redo", "edit amplify DB",
             "edit normalize [TARGET_DB] [linked|independent]", "edit close-gap focused|tracks|all",
             "edit crossfade selected|same|across SAMPLES"}));
    domains->setProperty(
        "clip",
        makeDomain("Per-clip envelope, gain, linear/curved fade lengths and shapes",
                   {"clip set LAYER CLIP gain|fade-in|fade-out VALUE",
                    "clip set LAYER CLIP fade-in-shape|fade-out-shape "
                    "linear|equal-power|s-curve|exponential",
                    "clip fade-shape fade-in|fade-out|both linear|equal-power|s-curve|exponential",
                    "clip envelope-selected GAIN|keep FADE_IN|keep FADE_OUT|keep",
                    "clip split-range|trim-range|rename"}));
    domains->setProperty(
        "layer",
        makeDomain("Mixing layer height, mute/solo/gain/balance, stereo routing, and channel "
                   "split/combine",
                   {"layer select-range|focus-range|add [NAME]|delete",
                    "layer toggle INDEX muted|solo",
                    "layer set-selected gain|gain-db|balance VALUE",
                    "layer scale-gain|scale-gain-db VALUE",
                    "layer set INDEX gain|balance|view|height|waveform-zoom|channel-heights VALUE",
                    "layer routing INDEX ...", "layer stereo INDEX swap|fold",
                    "layer channels INDEX split|combine"}));
    domains->setProperty(
        "track",
        makeDomain("Mixing layer controls (compatibility alias of layer)",
                   {"track select-range|focus-range|add [NAME]|delete",
                    "track toggle INDEX muted|solo",
                    "track set-selected gain|gain-db|balance VALUE",
                    "track scale-gain|scale-gain-db VALUE",
                    "track set INDEX gain|balance|view|height|waveform-zoom|channel-heights VALUE",
                    "track routing INDEX ...", "track stereo INDEX swap|fold",
                    "track channels INDEX split|combine"}));
    domains->setProperty(
        "transport",
        makeDomain("Play, pause, stop, selection audition, and loop playback",
                   {"transport play|pause|stop|stop-here|reset-interruptions",
                    "transport context [BEFORE AFTER]", "transport selection-context full|tracks",
                    "transport audition-selection full|tracks once|loop",
                    "transport audition-point|audition-close-gap|audition-crossfade|audition-clips",
                    "loop set START END | from-selection | play | clear"}));
    domains->setProperty(
        "analysis",
        makeDomain(
            "Zero-crossing, quiet-gap, source-silence, and peak/LUFS measurement",
            {"analysis zero-crossings any|all", "analysis quiet THRESHOLD_DB MIN_SAMPLES",
             "analysis source-silence TRACK CLIP MIN_SAMPLES",
             "analysis levels master|selection|tracks|track INDEX",
             "analysis status|cancel|audition INDEX|select INDEX|detach INDEX|apply-markers"}));
    domains->setProperty(
        "view",
        makeDomain(
            "Timeline framing, spectrogram/waveform modes, window sizing, and PNG screenshots",
            {"view status", "view resize WIDTH HEIGHT", "view capture FILE", "view capture status",
             "view fit|master-track INDEX|range START END", "view default waveform|spectrogram",
             "view waveform-rms on|off|waveform-zoom 1|2|4|8|waveform-scale "
             "linear|decibels|waveform-shape full|half",
             "view follow off|page|pinned|resume",
             "view spectrum-range LOW HIGH|spectrum-preset|spectrum-window|spectrum-floor|spectrum-"
             "fft"}));
    domains->setProperty(
        "disc",
        makeDomain(
            "UTOC-native MiniDisc tracks, sequential parts, coalesced spans, groups, titles, and "
            "Disc workspace tabs",
            {"disc status", "disc view tracks|record|open|close|status",
             "disc medium md60|md74|md80", "disc codec sp-stereo|sp-mono|lp2|lp4",
             "disc auto-track on|off", "disc title HALF [FULL]", "disc title-forms HALF FULL",
             "disc timestamp [ISO|clear]", "disc sync clips|layer1|markers",
             "disc track add|add-selection|remove|move|duplicate|split|merge|rename|title-forms|"
             "flags|part",
             "disc group set|clear", "disc focus TRACK_INDEX [PART_INDEX]",
             "disc preview [TRACK_INDEX | from TRACK_INDEX]"}));
    domains->setProperty(
        "utoc",
        makeDomain("Physical 2352-byte UTOC Sectors 0/1/2/4 budget, inspection, export, and import",
                   {"utoc budget", "utoc inspect", "utoc export FILE", "utoc import FILE"}));
    domains->setProperty(
        "encode",
        makeDomain("Porffor natively compiled ATRAC1/ATRAC3/ATRAC3plus encoders",
                   {"encode sp-stereo|sp-mono|lp2|lp4|hi-sp|hi-lp INPUT_WAV OUTPUT_FILE"}));
    domains->setProperty("netmd",
                         makeDomain("USB NetMD & Type-S / Hi-MD hardware bridge",
                                    {"netmd devices", "netmd status", "netmd preflight [DEVICE_ID]",
                                     "netmd load-disc [DEVICE_ID]",
                                     "netmd read-utoc [DEVICE_ID] [--out FILE]",
                                     "netmd erase [DEVICE_ID] [confirm-hardware]",
                                     "netmd write-utoc [DEVICE_ID] [confirm-hardware]",
                                     "netmd record [DEVICE_ID] [confirm-hardware]"}));

    if (args.size() == 2) {
        const auto& key = args[1];
        if (!domains->hasProperty(key)) {
            return ipcReply(false, "Unknown schema domain: " + key);
        }
        auto* out = new juce::DynamicObject();
        out->setProperty("domain", key);
        out->setProperty("spec", domains->getProperty(key));
        return ipcReply(true, juce::var(out));
    }
    auto* envelope = new juce::DynamicObject();
    envelope->setProperty("ok", "boolean");
    envelope->setProperty("result", "any (when ok is true)");
    envelope->setProperty("error", "string (when ok is false)");
    auto* root = new juce::DynamicObject();
    root->setProperty("protocol_version", 1);
    root->setProperty("sample_rate", carta::sessionRate);
    root->setProperty("max_linear_gain", 16.0);
    root->setProperty("response_envelope", juce::var(envelope));
    root->setProperty("domains", juce::var(domains.get()));
    return ipcReply(true, juce::var(root));
}

juce::var handleEncodeCommand(const juce::StringArray& args) {
    if (args.size() != 4) {
        return ipcReply(
            false, "Usage: encode sp-stereo|sp-mono|lp2|lp4|hi-sp|hi-lp INPUT_WAV OUTPUT_FILE");
    }
    const auto encodeMode = args[1].toStdString();
    const auto resolveFile = [](const juce::String& raw) {
        return juce::File::isAbsolutePath(raw)
                   ? juce::File(raw)
                   : juce::File::getCurrentWorkingDirectory().getChildFile(raw);
    };
    const juce::File inputFile = resolveFile(args[2]);
    const juce::File outputFile = resolveFile(args[3]);
    std::int64_t samplesOut = 0;
    std::int64_t bytesOut = 0;
    std::string encodeError;
    if (!carta::encodeWavFileToAtrac(encodeMode, inputFile, outputFile, samplesOut, bytesOut,
                                     encodeError)) {
        return ipcReply(false, juce::String::fromUTF8(encodeError.c_str()));
    }
    const auto* spec = carta::findAtracModeSpec(encodeMode);
    auto* root = new juce::DynamicObject();
    root->setProperty("mode", spec ? juce::String::fromUTF8(spec->id) : args[1]);
    root->setProperty("codec", spec ? juce::String::fromUTF8(spec->codec) : juce::String());
    root->setProperty("input", inputFile.getFullPathName());
    root->setProperty("output", outputFile.getFullPathName());
    root->setProperty("samples", static_cast<juce::int64>(samplesOut));
    root->setProperty("encoded_bytes", static_cast<juce::int64>(bytesOut));
    return ipcReply(true, juce::var(root));
}

} // namespace

/**
 * @brief Private helper struct grouping IPC domain handlers with full `Studio` access.
 */
struct Studio::IpcRouter {
    static void refreshProjectStatus(Studio& studio) {
        if (auto* result = studio.lastIpcProject.getDynamicObject()) {
            if (studio.lastIpcProjectHasSnapshot) {
                result->setProperty("stale",
                                    studio.lastIpcProjectGeneration != studio.editGeneration ||
                                        !sameLoop(studio.lastIpcProjectLoop, studio.savedLoop) ||
                                        studio.hasPendingGestureEdit());
            }
            if (studio.activeOpen && studio.activeOpen->ipc) {
                result->setProperty("progress",
                                    juce::jlimit(0.0, 1.0, studio.activeOpen->progress.load()));
                result->setProperty("cancellation_requested", studio.activeOpen->cancelled.load());
            } else if (studio.activeSave && studio.activeSave->ipc) {
                result->setProperty("progress",
                                    juce::jlimit(0.0, 1.0, studio.activeSave->progress.load()));
                result->setProperty("cancellation_requested", studio.activeSave->cancelled.load());
            }
        }
    }

    static bool applyMdTrackEdit(carta::MdDisc& d, const juce::StringArray& args,
                                 juce::String& preferredTrackId, juce::String& actionLabel,
                                 std::string& mdErr,
                                 const std::function<bool(const std::string&)>& fail,
                                 const carta::TimeSelection& capturedSelection) {
        if (args[2] == "add" && (args.size() == 5 || args.size() == 6 || args.size() == 7)) {
            juce::int64 start = 0;
            juce::int64 end = 0;
            juce::int64 ins = static_cast<juce::int64>(d.tracks.size());
            if (!carta::parseSampleOrTime(args[3], start) ||
                !carta::parseSampleOrTime(args[4], end) || end <= start) {
                return fail("Track START and END must specify a valid positive range.");
            }
            const std::string title = args.size() >= 6 ? args[5].toStdString() : std::string{};
            if (args.size() == 7 && !parseNonNegativeIpcInt(args[6], ins)) {
                return fail("Insert index must be a nonnegative integer.");
            }
            carta::MdTrack track;
            track.id = carta::makeMdTrackId();
            track.halfWidthTitle = title;
            track.parts.push_back({start, end});
            preferredTrackId = juce::String::fromUTF8(track.id.c_str());
            actionLabel = "Add MiniDisc track";
            return carta::insertMdTrack(d, static_cast<int>(ins) + 1, std::move(track), mdErr) ||
                   fail(mdErr);
        }
        if (args[2] == "add-selection" && (args.size() == 3 || args.size() == 4)) {
            if (!capturedSelection.hasRange()) {
                return fail("Select a timeline time range first.");
            }
            juce::int64 ins = static_cast<juce::int64>(d.tracks.size());
            if (args.size() == 4 && !parseNonNegativeIpcInt(args[3], ins)) {
                return fail("Insert index must be a nonnegative integer.");
            }
            carta::MdTrack track;
            track.id = carta::makeMdTrackId();
            track.halfWidthTitle =
                ("Track " + juce::String(static_cast<int>(ins) + 1)).toStdString();
            track.parts.push_back({capturedSelection.start, capturedSelection.end});
            preferredTrackId = juce::String::fromUTF8(track.id.c_str());
            actionLabel = "Add MiniDisc track from selection";
            return carta::insertMdTrack(d, static_cast<int>(ins) + 1, std::move(track), mdErr) ||
                   fail(mdErr);
        }
        if (args[2] == "remove" && args.size() == 4) {
            juce::int64 idx = 0;
            if (!parseNonNegativeIpcInt(args[3], idx) ||
                idx >= static_cast<juce::int64>(d.tracks.size())) {
                return fail("MD track index out of range.");
            }
            actionLabel = "Remove MiniDisc track";
            return carta::removeMdTrack(d, static_cast<int>(idx), mdErr) || fail(mdErr);
        }
        if (args[2] == "move" && args.size() == 5) {
            juce::int64 from = 0;
            juce::int64 to = 0;
            if (!parseNonNegativeIpcInt(args[3], from) || !parseNonNegativeIpcInt(args[4], to) ||
                from >= static_cast<juce::int64>(d.tracks.size()) ||
                to >= static_cast<juce::int64>(d.tracks.size())) {
                return fail("MD track index out of range.");
            }
            preferredTrackId =
                juce::String::fromUTF8(d.tracks[static_cast<size_t>(from)].id.c_str());
            actionLabel = "Move MiniDisc track";
            return carta::moveMdTrack(d, static_cast<int>(from), static_cast<int>(to), mdErr) ||
                   fail(mdErr);
        }
        if (args[2] == "duplicate" && (args.size() == 4 || args.size() == 5)) {
            juce::int64 idx = 0;
            juce::int64 count = 1;
            if (!parseNonNegativeIpcInt(args[3], idx) ||
                idx >= static_cast<juce::int64>(d.tracks.size())) {
                return fail("MD track index out of range.");
            }
            if (args.size() == 5 &&
                (!parseNonNegativeIpcInt(args[4], count) || count < 1 || count > 64)) {
                return fail("Duplicate count must be 1..64.");
            }
            actionLabel = "Duplicate MiniDisc track";
            for (juce::int64 c = 0; c < count; ++c) {
                if (!carta::duplicateMdTrack(d, static_cast<int>(idx + c), mdErr)) {
                    return fail(mdErr);
                }
            }
            if (idx + count < static_cast<juce::int64>(d.tracks.size())) {
                preferredTrackId =
                    juce::String::fromUTF8(d.tracks[static_cast<size_t>(idx + count)].id.c_str());
            }
            return true;
        }
        if (args[2] == "split" && args.size() == 5) {
            juce::int64 idx = 0;
            juce::int64 sample = 0;
            if (!parseNonNegativeIpcInt(args[3], idx) ||
                idx >= static_cast<juce::int64>(d.tracks.size())) {
                return fail("MD track index out of range.");
            }
            if (!carta::parseSampleOrTime(args[4], sample)) {
                return fail("Split position must be a valid sample or time.");
            }
            actionLabel = "Split MiniDisc track";
            if (!carta::splitMdTrackAtSample(d, static_cast<int>(idx), sample, mdErr)) {
                return fail(mdErr);
            }
            if (idx + 1 < static_cast<juce::int64>(d.tracks.size())) {
                preferredTrackId =
                    juce::String::fromUTF8(d.tracks[static_cast<size_t>(idx + 1)].id.c_str());
            }
            return true;
        }
        if (args[2] == "merge" && args.size() == 4) {
            juce::int64 idx = 0;
            if (!parseNonNegativeIpcInt(args[3], idx) ||
                idx >= static_cast<juce::int64>(d.tracks.size())) {
                return fail("MD track index out of range.");
            }
            actionLabel = "Merge MiniDisc tracks";
            if (!carta::mergeMdTrackWithNext(d, static_cast<int>(idx), mdErr)) {
                return fail(mdErr);
            }
            preferredTrackId =
                juce::String::fromUTF8(d.tracks[static_cast<size_t>(idx)].id.c_str());
            return true;
        }
        if (args[2] == "rename" && args.size() == 5) {
            juce::int64 idx = 0;
            if (!parseNonNegativeIpcInt(args[3], idx) ||
                idx >= static_cast<juce::int64>(d.tracks.size())) {
                return fail("MD track index out of range.");
            }
            auto& tr = d.tracks[static_cast<size_t>(idx)];
            const auto next = args[4].toStdString();
            if (tr.halfWidthTitle == next) {
                return fail("Track title is unchanged.");
            }
            tr.halfWidthTitle = next;
            preferredTrackId = juce::String::fromUTF8(tr.id.c_str());
            actionLabel = "Rename MiniDisc track";
            return carta::validateMdDisc(d, mdErr) || fail(mdErr);
        }
        if (args[2] == "title-forms" && args.size() == 6) {
            juce::int64 idx = 0;
            if (!parseNonNegativeIpcInt(args[3], idx) ||
                idx >= static_cast<juce::int64>(d.tracks.size())) {
                return fail("MD track index out of range.");
            }
            auto& tr = d.tracks[static_cast<size_t>(idx)];
            const auto half = args[4].toStdString();
            const auto full = args[5].toStdString();
            if (tr.halfWidthTitle == half && tr.fullWidthTitle == full) {
                return fail("Track title forms are unchanged.");
            }
            tr.halfWidthTitle = half;
            tr.fullWidthTitle = full;
            preferredTrackId = juce::String::fromUTF8(tr.id.c_str());
            actionLabel = "Set MD track title forms";
            return carta::validateMdDisc(d, mdErr) || fail(mdErr);
        }
        if (args[2] == "flags" && (args.size() == 8 || args.size() == 9)) {
            juce::int64 idx = 0;
            if (!parseNonNegativeIpcInt(args[3], idx) ||
                idx >= static_cast<juce::int64>(d.tracks.size())) {
                return fail("MD track index out of range.");
            }
            auto nextTr = d.tracks[static_cast<size_t>(idx)];
            const bool isOnDisc = carta::mdTrackUsesOnDiscAudio(d, nextTr);
            const auto currentEffCodec = carta::effectiveTrackCodec(d, nextTr);
            if (isOnDisc) {
                if (args[4] == "clear") {
                    if (d.defaultCodec != currentEffCodec) {
                        return fail(
                            "Cannot change encoding codec of an on-disc track without "
                            "re-encoding.");
                    }
                    nextTr.codecOverride = currentEffCodec;
                } else if (args[4] != "keep") {
                    carta::MdCodecMode m = d.defaultCodec;
                    if (!carta::parseMdCodecMode(args[4].toStdString(), m)) {
                        return fail(
                            "Track codec must be sp-stereo, sp-mono, lp2, lp4, clear, or keep.");
                    }
                    if (m != currentEffCodec) {
                        return fail(
                            "Cannot change encoding codec of an on-disc track without "
                            "re-encoding.");
                    }
                    nextTr.codecOverride = currentEffCodec;
                }
            } else {
                if (args[4] == "clear") {
                    nextTr.codecOverride.reset();
                } else if (args[4] != "keep") {
                    carta::MdCodecMode m = d.defaultCodec;
                    if (!carta::parseMdCodecMode(args[4].toStdString(), m)) {
                        return fail(
                            "Track codec must be sp-stereo, sp-mono, lp2, lp4, clear, or keep.");
                    }
                    nextTr.codecOverride = m;
                }
            }
            auto parseBoolOpt = [](const juce::String& tok, bool& target, std::string& errOut,
                                   const char* label) {
                if (tok == "keep") {
                    return true;
                }
                if (tok == "on" || tok == "true" || tok == "1") {
                    target = true;
                    return true;
                }
                if (tok == "off" || tok == "false" || tok == "0") {
                    target = false;
                    return true;
                }
                errOut = std::string(label) + " flag must be on, off, or keep.";
                return false;
            };
            if (!parseBoolOpt(args[5], nextTr.writeProtected, mdErr, "Write-protect") ||
                !parseBoolOpt(args[6], nextTr.scmsCopyProtected, mdErr, "SCMS") ||
                !parseBoolOpt(args[7], nextTr.emphasis, mdErr, "Emphasis")) {
                return fail(mdErr);
            }
            if (args.size() == 9 && args[8] != "keep") {
                if (args[8] == "clear") {
                    nextTr.timestamp.clear();
                } else {
                    carta::UtocTimestamp parsed;
                    if (!carta::parseUtocTimestampString(args[8].toStdString(), parsed, mdErr)) {
                        return fail(mdErr);
                    }
                    nextTr.timestamp = args[8].toStdString();
                }
            }
            if (nextTr == d.tracks[static_cast<size_t>(idx)]) {
                return fail("MD track flags are unchanged.");
            }
            d.tracks[static_cast<size_t>(idx)] = nextTr;
            preferredTrackId = juce::String::fromUTF8(nextTr.id.c_str());
            actionLabel = "Update MD track flags";
            return carta::validateMdDisc(d, mdErr) || fail(mdErr);
        }
        if (args[2] == "part" && args.size() >= 6) {
            juce::int64 idx = 0;
            if (!parseNonNegativeIpcInt(args[4], idx) ||
                idx >= static_cast<juce::int64>(d.tracks.size())) {
                return fail("MD track index out of range.");
            }
            auto& tr = d.tracks[static_cast<size_t>(idx)];
            preferredTrackId = juce::String::fromUTF8(tr.id.c_str());
            if (args[3] == "add" && (args.size() == 7 || args.size() == 8)) {
                juce::int64 s0 = 0;
                juce::int64 e0 = 0;
                juce::int64 pIns = static_cast<juce::int64>(tr.parts.size());
                if (!carta::parseSampleOrTime(args[5], s0) ||
                    !carta::parseSampleOrTime(args[6], e0) || e0 <= s0) {
                    return fail("Part range must satisfy START < END.");
                }
                if (args.size() == 8 && (!parseNonNegativeIpcInt(args[7], pIns) ||
                                         pIns > static_cast<juce::int64>(tr.parts.size()))) {
                    return fail("Part insert index out of range.");
                }
                tr.parts.insert(tr.parts.begin() + static_cast<ptrdiff_t>(pIns), {s0, e0});
                actionLabel = "Add UTOC part";
                return carta::validateMdDisc(d, mdErr) || fail(mdErr);
            }
            if (args[3] == "set" && args.size() == 8) {
                juce::int64 pIdx = 0;
                juce::int64 s0 = 0;
                juce::int64 e0 = 0;
                if (!parseNonNegativeIpcInt(args[5], pIdx) ||
                    pIdx >= static_cast<juce::int64>(tr.parts.size())) {
                    return fail("Part index out of range.");
                }
                if (!carta::parseSampleOrTime(args[6], s0) ||
                    !carta::parseSampleOrTime(args[7], e0) || e0 <= s0) {
                    return fail("Part range must satisfy START < END.");
                }
                tr.parts[static_cast<size_t>(pIdx)] = {s0, e0};
                actionLabel = "Set UTOC part range";
                return carta::validateMdDisc(d, mdErr) || fail(mdErr);
            }
            if (args[3] == "repeat" && args.size() == 7) {
                juce::int64 pIdx = 0;
                juce::int64 count = 1;
                if (!parseNonNegativeIpcInt(args[5], pIdx) ||
                    pIdx >= static_cast<juce::int64>(tr.parts.size())) {
                    return fail("Part index out of range.");
                }
                if (!parseNonNegativeIpcInt(args[6], count) || count < 1 || count > 64) {
                    return fail("Repeat count must be 1..64.");
                }
                const auto copyPart = tr.parts[static_cast<size_t>(pIdx)];
                tr.parts.insert(tr.parts.begin() + static_cast<ptrdiff_t>(pIdx + 1),
                                static_cast<size_t>(count), copyPart);
                actionLabel = "Repeat UTOC part";
                return carta::validateMdDisc(d, mdErr) || fail(mdErr);
            }
            if (args[3] == "remove" && args.size() == 6) {
                juce::int64 pIdx = 0;
                if (!parseNonNegativeIpcInt(args[5], pIdx) ||
                    pIdx >= static_cast<juce::int64>(tr.parts.size())) {
                    return fail("Part index out of range.");
                }
                if (tr.parts.size() <= 1) {
                    return fail("A MiniDisc track must keep at least one part.");
                }
                tr.parts.erase(tr.parts.begin() + static_cast<ptrdiff_t>(pIdx));
                actionLabel = "Remove UTOC part";
                return carta::validateMdDisc(d, mdErr) || fail(mdErr);
            }
        }
        return fail("Unknown or invalid ipc disc track command.");
    }

    static juce::var handleDisc(Studio& studio, const juce::StringArray& args) {
        if (args.size() == 2 && args[1] == "status") {
            return ipcReply(true, buildMdDiscStatusJson(studio.session.mdDisc));
        }
        if (args.size() == 3 && args[1] == "view") {
            if (args[2] == "status") {
                return ipcReply(true, studio.discWorkspaceVisible
                                          ? studio.discWorkspace.currentView()
                                          : juce::String("closed"));
            }
            if (studio.dragMode != kDragNone || studio.activeOpen ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                return ipcReply(false, "Finish the current drag, dialog or opening first.");
            }
            if (args[2] == "close" || args[2] == "hide") {
                studio.setDiscWorkspaceVisible(false);
                return ipcReply(true, "closed");
            }
            if (args[2] == "open" || args[2] == "show") {
                studio.setDiscWorkspaceVisible(true);
                studio.refreshDiscWorkspace(true);
                return ipcReply(true, studio.discWorkspace.currentView());
            }
            juce::String viewError;
            if (!studio.discWorkspace.selectView(args[2], viewError)) {
                return ipcReply(false, viewError);
            }
            studio.setDiscWorkspaceVisible(true);
            studio.saveWorkspacePreferences();
            studio.refreshDiscWorkspace(true);
            return ipcReply(true, studio.discWorkspace.currentView());
        }
        if ((args.size() == 3 || args.size() == 4) && args[1] == "focus") {
            juce::int64 idx = 0;
            juce::int64 partIdx = -1;
            if (!parseNonNegativeIpcInt(args[2], idx) ||
                idx >= static_cast<juce::int64>(studio.session.mdDisc.tracks.size())) {
                return ipcReply(false, "MD track index out of range.");
            }
            if (args.size() == 4 && !parseNonNegativeIpcInt(args[3], partIdx)) {
                return ipcReply(false, "MD part index must be a nonnegative integer.");
            }
            studio.setDiscWorkspaceVisible(true);
            const auto trackId = juce::String::fromUTF8(
                studio.session.mdDisc.tracks[static_cast<size_t>(idx)].id.c_str());
            studio.discWorkspace.selectTrackById(trackId, true);
            if (partIdx >= 0) {
                const auto& parts = studio.session.mdDisc.tracks[static_cast<size_t>(idx)].parts;
                if (partIdx >= static_cast<juce::int64>(parts.size())) {
                    return ipcReply(false, "MD part index out of range.");
                }
                if (studio.discWorkspace.onSeekPart) {
                    studio.discWorkspace.onSeekPart(trackId, static_cast<int>(partIdx),
                                                    studio.editGeneration);
                }
            }
            return ipcReply(true, trackId);
        }
        if (args.size() >= 2 && args[1] == "preview") {
            if (args.size() == 2) {
                return studio.startDiscPreview(studio.editGeneration)
                           ? ipcReply(true, "Previewing MiniDisc playback order")
                           : ipcReply(false, studio.status.getText());
            }
            if (args.size() == 3) {
                juce::int64 idx = 0;
                if (!parseNonNegativeIpcInt(args[2], idx) ||
                    idx >= static_cast<juce::int64>(studio.session.mdDisc.tracks.size())) {
                    return ipcReply(false, "MD track index out of range.");
                }
                const auto trackId = juce::String::fromUTF8(
                    studio.session.mdDisc.tracks[static_cast<size_t>(idx)].id.c_str());
                return studio.startDiscPreview(studio.editGeneration, trackId, false)
                           ? ipcReply(true, "Previewing MD track")
                           : ipcReply(false, studio.status.getText());
            }
            if (args.size() == 4 && args[2] == "from") {
                juce::int64 idx = 0;
                if (!parseNonNegativeIpcInt(args[3], idx) ||
                    idx >= static_cast<juce::int64>(studio.session.mdDisc.tracks.size())) {
                    return ipcReply(false, "MD track index out of range.");
                }
                const auto trackId = juce::String::fromUTF8(
                    studio.session.mdDisc.tracks[static_cast<size_t>(idx)].id.c_str());
                return studio.startDiscPreview(studio.editGeneration, trackId, true)
                           ? ipcReply(true, "Previewing MiniDisc from track")
                           : ipcReply(false, studio.status.getText());
            }
            return ipcReply(false, "Usage: ipc disc preview [TRACK_INDEX | from TRACK_INDEX]");
        }
        if (studio.dragMode != kDragNone || studio.activeOpen ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag, dialog or opening first.");
        }

        juce::String error;
        juce::String preferredTrackId;
        juce::String actionLabel = "Edit MiniDisc UTOC";
        const auto capturedSelection = studio.timeSelection;
        const auto capturedSelectedClips = studio.timelineClipSelection.get(studio.editGeneration);
        const bool changed = studio.mutateMixState(
            [&](carta::Session& candidate) {
                auto& d = candidate.mdDisc;
                std::string mdErr;
                auto fail = [&](const std::string& msg) {
                    error = juce::String::fromUTF8(msg.c_str());
                    return false;
                };
                if (args.size() == 3 && args[1] == "medium") {
                    carta::MdMedium med = d.medium;
                    if (!carta::parseMdMedium(args[2].toStdString(), med)) {
                        return fail("Medium must be md60, md74, or md80.");
                    }
                    if (d.medium == med) {
                        return fail("MiniDisc medium is unchanged.");
                    }
                    d.medium = med;
                    actionLabel = "Change MiniDisc medium";
                    return carta::validateMdDisc(d, mdErr) || fail(mdErr);
                }
                if (args.size() == 3 && args[1] == "codec") {
                    carta::MdCodecMode modeVal = d.defaultCodec;
                    if (!carta::parseMdCodecMode(args[2].toStdString(), modeVal)) {
                        return fail("Codec must be sp-stereo, sp-mono, lp2, or lp4.");
                    }
                    if (d.defaultCodec == modeVal) {
                        return fail("Default MiniDisc codec is unchanged.");
                    }
                    d.defaultCodec = modeVal;
                    actionLabel = "Change MiniDisc default codec";
                    return carta::validateMdDisc(d, mdErr) || fail(mdErr);
                }
                if (args.size() == 3 && args[1] == "auto-track") {
                    if (args[2] != "on" && args[2] != "off") {
                        return fail("Auto-track must be on or off.");
                    }
                    const bool enabled = args[2] == "on";
                    if (d.autoTrackOnImport == enabled) {
                        return fail("Auto-track from clips setting is unchanged.");
                    }
                    d.autoTrackOnImport = enabled;
                    actionLabel = "Toggle auto-track from clips";
                    return true;
                }
                if ((args.size() == 3 || args.size() == 4) && args[1] == "title") {
                    const auto half = args[2].toStdString();
                    const auto full = args.size() == 4 ? args[3].toStdString() : d.fullWidthTitle;
                    if (d.halfWidthTitle == half && d.fullWidthTitle == full) {
                        return fail("Disc title is unchanged.");
                    }
                    d.halfWidthTitle = half;
                    d.fullWidthTitle = full;
                    actionLabel = "Rename MiniDisc";
                    return carta::validateMdDisc(d, mdErr) || fail(mdErr);
                }
                if (args.size() == 4 && args[1] == "title-forms") {
                    const auto half = args[2].toStdString();
                    const auto full = args[3].toStdString();
                    if (d.halfWidthTitle == half && d.fullWidthTitle == full) {
                        return fail("Disc title forms are unchanged.");
                    }
                    d.halfWidthTitle = half;
                    d.fullWidthTitle = full;
                    actionLabel = "Set disc title forms";
                    return carta::validateMdDisc(d, mdErr) || fail(mdErr);
                }
                if ((args.size() == 2 || args.size() == 3) && args[1] == "timestamp") {
                    const std::string ts = (args.size() == 3 && args[2] != "clear")
                                               ? args[2].toStdString()
                                               : std::string{};
                    if (!ts.empty()) {
                        carta::UtocTimestamp parsed;
                        if (!carta::parseUtocTimestampString(ts, parsed, mdErr)) {
                            return fail(mdErr);
                        }
                    }
                    if (d.discTimestamp == ts) {
                        return fail("Disc timestamp is unchanged.");
                    }
                    d.discTimestamp = ts;
                    actionLabel = "Set disc timestamp";
                    return true;
                }
                if (args.size() == 3 && args[1] == "sync") {
                    if (args[2] == "clips") {
                        actionLabel = "Sync MD tracks from clips";
                        std::vector<std::pair<int, int>> explicitClips;
                        explicitClips.reserve(capturedSelectedClips.size());
                        for (const auto& ref : capturedSelectedClips) {
                            explicitClips.emplace_back(ref.track, ref.clip);
                        }
                        return carta::syncTracksFromClips(candidate, d, mdErr, explicitClips) ||
                               fail(mdErr);
                    }
                    if (args[2] == "layer1") {
                        actionLabel = "Sync MD tracks from Layer 1";
                        return carta::syncTracksFromLayerClips(candidate, d, mdErr) || fail(mdErr);
                    }
                    if (args[2] == "markers") {
                        actionLabel = "Sync MD tracks from Markers";
                        return carta::syncTracksFromMarkers(candidate, d, mdErr) || fail(mdErr);
                    }
                    return fail("Sync source must be clips, layer1, or markers.");
                }
                if (args.size() >= 4 && args[1] == "group") {
                    if (args[2] == "set" && (args.size() == 6 || args.size() == 7)) {
                        juce::int64 s1 = 0;
                        juce::int64 e1 = 0;
                        if (!parseNonNegativeIpcInt(args[3], s1) ||
                            !parseNonNegativeIpcInt(args[4], e1)) {
                            return fail("Track range must contain positive 1-based track numbers.");
                        }
                        const std::string half = args[5].toStdString();
                        const std::string full =
                            args.size() == 7 ? args[6].toStdString() : std::string{};
                        actionLabel = "Set MiniDisc group";
                        return carta::setMdGroup(d, static_cast<int>(s1), static_cast<int>(e1),
                                                 half, full, mdErr) ||
                               fail(mdErr);
                    }
                    if (args[2] == "clear" && args.size() == 5) {
                        juce::int64 s1 = 0;
                        juce::int64 e1 = 0;
                        if (!parseNonNegativeIpcInt(args[3], s1) ||
                            !parseNonNegativeIpcInt(args[4], e1)) {
                            return fail("Track range must contain positive 1-based track numbers.");
                        }
                        actionLabel = "Clear MiniDisc group";
                        return carta::ungroupMdRange(d, static_cast<int>(s1), static_cast<int>(e1),
                                                     mdErr) ||
                               fail(mdErr);
                    }
                    return fail(
                        "Usage: ipc disc group set START END TITLE [FULL_WIDTH] | clear START END");
                }
                if (args.size() >= 3 && args[1] == "track") {
                    return applyMdTrackEdit(d, args, preferredTrackId, actionLabel, mdErr, fail,
                                            capturedSelection);
                }
                return fail(
                    "Usage: ipc disc "
                    "status|view|medium|codec|auto-track|title|title-forms|timestamp|sync|track|"
                    "group|focus|preview");
            },
            error, actionLabel);

        if (changed) {
            studio.status.setText(actionLabel + ".", juce::dontSendNotification);
            studio.refreshDiscWorkspace(true, preferredTrackId);
        }
        return changed ? ipcReply(true, buildMdDiscStatusJson(studio.session.mdDisc))
                       : ipcReply(false, error);
    }

    static juce::var handleMeter(Studio& studio, const juce::StringArray& args) {
        if (args.size() != 2 || (args[1] != "status" && args[1] != "reset")) {
            return ipcReply(false, "Usage: ipc meter status|reset");
        }
        if (args[1] == "reset") {
            studio.resetOutputPeaks();
            return ipcReply(true, "Output peaks reset");
        }
        carta::OutputPeakSnapshot peaks;
        {
            const juce::ScopedLock guard(studio.lock);
            peaks = studio.outputPeakHold.snapshot();
        }
        auto* result = new juce::DynamicObject();
        result->setProperty("channels", peaks.channels);
        juce::Array<juce::var> rows;
        for (int channel = 0; channel < peaks.channels; ++channel) {
            const auto value = channel == 0 ? peaks.left : peaks.right;
            auto* row = new juce::DynamicObject();
            row->setProperty("peak", static_cast<double>(value));
            row->setProperty("at_or_above_full_scale", value >= 1.0f);
            rows.add(juce::var(row));
        }
        result->setProperty("held", juce::var(rows));
        return ipcReply(true, juce::var(result));
    }

    static juce::var handleHistory(Studio& studio, const juce::StringArray& args) {
        if (args.size() == 2 && args[1] == "list") {
            auto* result = new juce::DynamicObject();
            result->setProperty("generation", static_cast<juce::int64>(studio.editGeneration));
            result->setProperty("revision",
                                static_cast<juce::int64>(studio.editHistory.revision()));
            for (const bool redo : {false, true}) {
                juce::Array<juce::var> items;
                const auto targets = studio.editHistory.targets(redo);
                for (size_t i = 0; i < targets.size(); ++i) {
                    auto* item = new juce::DynamicObject();
                    item->setProperty("steps", static_cast<int>(i + 1));
                    item->setProperty("revision", static_cast<juce::int64>(targets[i].revision));
                    item->setProperty("action", targets[i].action);
                    items.add(juce::var(item));
                }
                result->setProperty(redo ? "redo" : "undo", juce::var(items));
            }
            return ipcReply(true, juce::var(result));
        }
        if (args.size() != 5 || (args[1] != "undo" && args[1] != "redo")) {
            return ipcReply(false, "Usage: ipc history list | undo|redo STEPS GENERATION REVISION");
        }
        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag or dialog first.");
        }
        juce::int64 steps = 0;
        juce::int64 generation = 0;
        juce::int64 revision = 0;
        if (!parseNonNegativeIpcInt(args[2], steps) || steps == 0 ||
            !parseNonNegativeIpcInt(args[3], generation) ||
            !parseNonNegativeIpcInt(args[4], revision)) {
            return ipcReply(false,
                            "Expected positive steps and nonnegative generation and revision.");
        }
        const bool redo = args[1] == "redo";
        const auto targets = studio.editHistory.targets(redo);
        if (static_cast<juce::uint64>(generation) != studio.editGeneration ||
            static_cast<juce::uint64>(steps) > targets.size() ||
            targets[static_cast<size_t>(steps - 1)].revision !=
                static_cast<juce::uint64>(revision)) {
            return ipcReply(false, "History changed; list it again.");
        }
        const auto before = studio.editGeneration;
        studio.jumpHistory({studio.editGeneration, static_cast<juce::uint64>(revision),
                            static_cast<size_t>(steps), redo});
        const auto moved = studio.editGeneration - before;
        auto* result = new juce::DynamicObject();
        result->setProperty("steps_moved", static_cast<juce::int64>(moved));
        result->setProperty("complete", moved == static_cast<juce::uint64>(steps));
        result->setProperty("revision", static_cast<juce::int64>(studio.editHistory.revision()));
        return ipcReply(true, juce::var(result));
    }

    static juce::var handleSnap(Studio& studio, const juce::StringArray& args) {
        if (args.size() == 2 && args[1] == "status") {
            auto* targets = new juce::DynamicObject();
            targets->setProperty("clip_edges", studio.snapOptions.clipEdges);
            targets->setProperty("markers", studio.snapOptions.markers);
            targets->setProperty("selection", studio.snapOptions.selection);
            targets->setProperty("playhead", studio.snapOptions.playhead);
            targets->setProperty("timeline_start", studio.snapOptions.timelineStart);
            auto* o = new juce::DynamicObject();
            o->setProperty("mode", studio.snapMode == 0   ? "off"
                                   : studio.snapMode == 1 ? "grid"
                                                          : "boundaries");
            o->setProperty("grid_samples", studio.snapGridSamples);
            o->setProperty("targets", juce::var(targets));
            return ipcReply(true, juce::var(o));
        }
        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag or dialog first.");
        }
        if (args.size() == 3 && args[1] == "grid") {
            if (!studio.setSnapGrid(args[2])) {
                return ipcReply(
                    false, "Enter a positive grid interval in project samples, seconds or time.");
            }
            return ipcReply(true, studio.snapGridSamples);
        }
        if (args.size() == 3 && args[1] == "mode" &&
            (args[2] == "off" || args[2] == "grid" || args[2] == "boundaries")) {
            studio.snapMode = args[2] == "off" ? 0 : args[2] == "grid" ? 1 : 2;
            studio.syncSnapButtonAppearance();
            studio.menuItemsChanged();
            return ipcReply(true, args[2]);
        }
        if (args.size() == 4 && args[1] == "target" && (args[3] == "on" || args[3] == "off")) {
            bool* option = args[2] == "clip-edges"       ? &studio.snapOptions.clipEdges
                           : args[2] == "markers"        ? &studio.snapOptions.markers
                           : args[2] == "selection"      ? &studio.snapOptions.selection
                           : args[2] == "playhead"       ? &studio.snapOptions.playhead
                           : args[2] == "timeline-start" ? &studio.snapOptions.timelineStart
                                                         : nullptr;
            if (!option) {
                return ipcReply(false, "Unknown snap target.");
            }
            *option = args[3] == "on";
            studio.menuItemsChanged();
            studio.status.setText(
                "Snap boundary targets updated for the next drag. Boundary snapping must be "
                "enabled.",
                juce::dontSendNotification);
            return ipcReply(true, *option);
        }
        return ipcReply(
            false, "Usage: ipc snap status | grid INTERVAL | mode off|grid|boundaries | target "
                   "clip-edges|markers|selection|playhead|timeline-start on|off");
    }

    static juce::var handleSources(Studio& studio, const juce::StringArray& args) {
        if (args.size() == 2 && args[1] == "status") {
            if (studio.lastIpcSourceCheck.isVoid()) {
                return ipcReply(false, "No IPC source check has been started.");
            }
            auto* result = studio.lastIpcSourceCheck.getDynamicObject();
            if (studio.activeSourceCheck && studio.activeSourceCheck->ipc) {
                result->setProperty(
                    "progress", juce::jlimit(0.0, 1.0, studio.activeSourceCheck->progress.load()));
                result->setProperty("cancellation_requested",
                                    studio.activeSourceCheck->cancelled.load());
            }
            result->setProperty("stale", studio.editGeneration != studio.lastIpcSourceGeneration);
            return ipcReply(true, studio.lastIpcSourceCheck);
        }
        if (args.size() == 2 && args[1] == "cancel") {
            if (!studio.activeSourceCheck || !studio.activeSourceCheck->ipc) {
                return ipcReply(false, "No IPC source check is running.");
            }
            studio.activeSourceCheck->cancelled.store(true);
            return ipcReply(true, "Cancellation requested");
        }
        if (args.size() == 5 && args[1] == "relink") {
            juce::int64 track = 0;
            juce::int64 clip = 0;
            if (!parseNonNegativeIpcInt(args[2], track) ||
                track >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                !parseNonNegativeIpcInt(args[3], clip) ||
                clip >= static_cast<juce::int64>(
                            studio.session.tracks[static_cast<size_t>(track)].clips.size()) ||
                !juce::File::isAbsolutePath(args[4])) {
                return ipcReply(false,
                                "Expected a valid track, clip and absolute replacement path.");
            }
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr || studio.activeOpen) {
                return ipcReply(false, "Finish the current drag, dialog or project opening first.");
            }
            const auto source = studio.session.tracks[static_cast<size_t>(track)]
                                    .clips[static_cast<size_t>(clip)]
                                    .audio;
            return studio.startSourceReplacement(source, juce::File(args[4]), studio.editGeneration,
                                                 true)
                       ? ipcReply(true, "Source relink queued")
                       : ipcReply(false, studio.status.getText());
        }
        if ((args.size() == 3 && args[1] == "verify" && args[2] == "all") ||
            (args.size() == 4 && args[1] == "verify" && args[2] == "track")) {
            juce::int64 track = 0;
            const bool all = args.size() == 3;
            if (!all && (!parseNonNegativeIpcInt(args[3], track) ||
                         track >= static_cast<juce::int64>(studio.session.tracks.size()))) {
                return ipcReply(false, "Invalid track index.");
            }
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr || studio.activeOpen) {
                return ipcReply(false, "Finish the current drag, dialog or project opening first.");
            }
            return studio.verifySelectedSources(all, all ? -1 : static_cast<int>(track), true)
                       ? ipcReply(true, "Source verification queued")
                       : ipcReply(false, studio.status.getText());
        }
        return ipcReply(
            false,
            "Usage: ipc sources verify all | verify track INDEX | relink TRACK CLIP FILE | status "
            "| cancel");
    }

    static juce::var handleAnalysis(Studio& studio, const juce::StringArray& args) {
        if (args.size() == 2 && args[1] == "status") {
            if (studio.lastIpcAnalysis.isVoid()) {
                return ipcReply(false, "No IPC analysis has been started.");
            }
            if (auto* result = studio.lastIpcAnalysis.getDynamicObject()) {
                if (studio.activeLevelAnalysis && studio.activeLevelAnalysis->ipc) {
                    result->setProperty(
                        "progress",
                        juce::jlimit(0.0, 1.0, studio.activeLevelAnalysis->progress.load()));
                    result->setProperty("cancellation_requested",
                                        studio.activeLevelAnalysis->cancelled.load());
                }
                result->setProperty("stale", studio.ipcAnalysisStale());
            }
            return ipcReply(true, studio.lastIpcAnalysis);
        }
        if (args.size() == 2 && args[1] == "cancel") {
            if (!studio.activeLevelAnalysis || !studio.activeLevelAnalysis->ipc) {
                return ipcReply(false, "No IPC analysis is running.");
            }
            studio.activeLevelAnalysis->cancelled.store(true);
            return ipcReply(true, "Cancellation requested");
        }
        if (args.size() == 3 && (args[1] == "locate-peak" || args[1] == "locate-over")) {
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                return ipcReply(false, "Finish the current drag or dialog first.");
            }
            const auto* info = studio.lastIpcAnalysis.getDynamicObject();
            if (!info || info->getProperty("kind").toString() != "levels" ||
                info->getProperty("state").toString() != "complete") {
                return ipcReply(false, "Complete a level analysis first.");
            }
            if (studio.ipcAnalysisStale()) {
                return ipcReply(false, "The level analysis is stale; scan again.");
            }
            const auto outputs = info->getProperty("outputs");
            const auto* rows = outputs.getArray();
            const int channel = args[2] == "left"    ? 0
                                : args[2] == "right" ? 1
                                : args[2] == "mono"  ? 0
                                                     : -1;
            if (channel < 0 || rows == nullptr || channel >= rows->size() ||
                (args[2] == "mono" && rows->size() != 1) ||
                (args[2] != "mono" && rows->size() != 2)) {
                return ipcReply(false, "Choose mono for a mono analysis or left/right for stereo.");
            }
            const bool overload = args[1] == "locate-over";
            const auto target =
                (*rows)[channel][overload ? "first_above_full_scale" : "peak_sample"];
            if (target.isVoid()) {
                return ipcReply(false, overload ? "That output has no sample above 0 dBFS."
                                                : "That output is silent; it has no nonzero peak.");
            }
            const auto sample = static_cast<juce::int64>(target);
            juce::String error;
            if (!studio.locateLevelSample(
                    sample, studio.lastIpcAnalysisGeneration, studio.lastIpcAnalysisChannels,
                    overload ? "First sample above 0 dBFS" : "Sample peak", error)) {
                return ipcReply(false, error);
            }
            return ipcReply(true, sample);
        }
        if (args.size() == 3 && args[1] == "zero-crossings" &&
            (args[2] == "any" || args[2] == "all")) {
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr || studio.activeOpen) {
                return ipcReply(false, "Finish the current drag, dialog or project opening first.");
            }
            return studio.findSelectionZeroCrossings(args[2] == "all", true)
                       ? ipcReply(true, "Zero-crossing search queued")
                       : ipcReply(false, studio.status.getText());
        }
        if ((args.size() == 3 || args.size() == 4) && args[1] == "audition") {
            if (args.size() == 4 && args[3] != "start" && args[3] != "middle" && args[3] != "end") {
                return ipcReply(false, "Usage: ipc analysis audition INDEX [start|middle|end]");
            }
            juce::int64 index = 0;
            if (!parseNonNegativeIpcInt(args[2], index)) {
                return ipcReply(false, "Expected a nonnegative gap index.");
            }
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                return ipcReply(false, "Finish the current drag or dialog first.");
            }
            const juce::ScopedLock guard(studio.lock);
            auto* result = studio.lastIpcAnalysis.getDynamicObject();
            if (result == nullptr ||
                (result->getProperty("kind").toString() != "quiet" &&
                 result->getProperty("kind").toString() != "source-silence") ||
                result->getProperty("state").toString() != "complete") {
                return ipcReply(false, "Complete a gap analysis first.");
            }
            if (studio.editGeneration != studio.lastIpcAnalysisGeneration ||
                studio.masterChannels != studio.lastIpcAnalysisChannels) {
                return ipcReply(false, "The project changed since the analysis; scan again.");
            }
            const auto gaps = result->getProperty("gaps");
            const auto* rows = gaps.getArray();
            if (rows == nullptr || index >= rows->size()) {
                return ipcReply(false, "Invalid gap index.");
            }
            const auto& gap = (*rows)[static_cast<int>(index)];
            const carta::QuietRegion region{static_cast<juce::int64>(gap["start_sample"]),
                                            static_cast<juce::int64>(gap["end_sample"])};
            const auto action = args.size() == 4 && args[3] == "start" ? QuietGapAction::start
                                : args.size() == 4 && args[3] == "end" ? QuietGapAction::end
                                                                       : QuietGapAction::middle;
            const auto sample = quietGapAuditionPoint(region, action);
            if (!studio.requireAudioOutput() || !studio.startBoundaryAudition(sample)) {
                return ipcReply(false, studio.status.getText());
            }
            return ipcReply(true, sample);
        }
        if (args.size() == 3 && args[1] == "select") {
            juce::int64 index = 0;
            if (!parseNonNegativeIpcInt(args[2], index)) {
                return ipcReply(false, "Expected a nonnegative gap index.");
            }
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                return ipcReply(false, "Finish the current drag or dialog first.");
            }
            auto* result = studio.lastIpcAnalysis.getDynamicObject();
            if (result == nullptr ||
                (result->getProperty("kind").toString() != "quiet" &&
                 result->getProperty("kind").toString() != "source-silence") ||
                result->getProperty("state").toString() != "complete") {
                return ipcReply(false, "Complete a gap analysis first.");
            }
            const auto gaps = result->getProperty("gaps");
            const auto* rows = gaps.getArray();
            if (rows == nullptr || index >= rows->size()) {
                return ipcReply(false, "Invalid gap index.");
            }
            const auto& gap = (*rows)[static_cast<int>(index)];
            juce::String error;
            const auto start = static_cast<juce::int64>(gap["start_sample"]);
            const auto end = static_cast<juce::int64>(gap["end_sample"]);
            const bool selected =
                result->getProperty("kind").toString() == "source-silence"
                    ? studio.selectClipSilenceRegion(
                          start, end, static_cast<int>(result->getProperty("track")),
                          studio.lastIpcAnalysisGeneration, studio.lastIpcAnalysisChannels, error)
                    : studio.selectQuietRegion(start, end, studio.lastIpcAnalysisGeneration,
                                               studio.lastIpcAnalysisChannels, error);
            if (!selected) {
                return ipcReply(false, error);
            }
            return ipcReply(true, "Gap selected");
        }
        if (args.size() == 3 && args[1] == "detach") {
            juce::int64 index = 0;
            if (!parseNonNegativeIpcInt(args[2], index)) {
                return ipcReply(false, "Expected a nonnegative gap index.");
            }
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                return ipcReply(false, "Finish the current drag or dialog first.");
            }
            auto* result = studio.lastIpcAnalysis.getDynamicObject();
            if (!result || result->getProperty("kind").toString() != "source-silence" ||
                result->getProperty("state").toString() != "complete") {
                return ipcReply(false, "Complete a source-silence analysis first.");
            }
            const auto gaps = result->getProperty("gaps");
            const auto* rows = gaps.getArray();
            if (!rows || index >= rows->size()) {
                return ipcReply(false, "Invalid gap index.");
            }
            const auto& gap = (*rows)[static_cast<int>(index)];
            if (gap["detach_start_sample"].isVoid() || gap["detach_end_sample"].isVoid()) {
                return ipcReply(false, "This gap has no conservative detach interior.");
            }
            juce::String error;
            const auto start = static_cast<juce::int64>(gap["detach_start_sample"]);
            const auto end = static_cast<juce::int64>(gap["detach_end_sample"]);
            if (!studio.detachScannedSilence(static_cast<int>(result->getProperty("track")),
                                             static_cast<int>(result->getProperty("clip")), start,
                                             end, studio.lastIpcAnalysisGeneration,
                                             studio.lastIpcAnalysisChannels, error)) {
                return ipcReply(false, error);
            }
            auto* detached = new juce::DynamicObject();
            detached->setProperty("start_sample", start);
            detached->setProperty("end_sample", end);
            return ipcReply(true, juce::var(detached));
        }
        if (args.size() == 2 && args[1] == "detach-all") {
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                return ipcReply(false, "Finish the current drag or dialog first.");
            }
            auto* result = studio.lastIpcAnalysis.getDynamicObject();
            if (!result || result->getProperty("kind").toString() != "source-silence" ||
                result->getProperty("state").toString() != "complete") {
                return ipcReply(false, "Complete a source-silence analysis first.");
            }
            const auto gaps = result->getProperty("gaps");
            const auto* rows = gaps.getArray();
            if (!rows) {
                return ipcReply(false, "Source-silence results are unavailable; scan again.");
            }
            std::vector<carta::QuietRegion> ranges;
            for (const auto& gap : *rows) {
                if (!gap["detach_start_sample"].isVoid() && !gap["detach_end_sample"].isVoid()) {
                    ranges.push_back({static_cast<juce::int64>(gap["detach_start_sample"]),
                                      static_cast<juce::int64>(gap["detach_end_sample"])});
                }
            }
            const auto skipped = static_cast<int>(rows->size()) - static_cast<int>(ranges.size());
            juce::String error;
            if (!studio.detachScannedSilences(static_cast<int>(result->getProperty("track")),
                                              static_cast<int>(result->getProperty("clip")), ranges,
                                              studio.lastIpcAnalysisGeneration,
                                              studio.lastIpcAnalysisChannels, error)) {
                return ipcReply(false, error);
            }
            auto* detached = new juce::DynamicObject();
            detached->setProperty("detached", static_cast<int>(ranges.size()));
            detached->setProperty("skipped", skipped);
            return ipcReply(true, juce::var(detached));
        }
        if (args.size() >= 2 && args[1] == "apply-markers") {
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                return ipcReply(false, "Finish the current drag or dialog first.");
            }
            auto* result = studio.lastIpcAnalysis.getDynamicObject();
            if (result == nullptr || result->getProperty("kind").toString() != "quiet" ||
                result->getProperty("state").toString() != "complete") {
                return ipcReply(false, "Complete a quiet-gap analysis first.");
            }
            if (studio.editGeneration != studio.lastIpcAnalysisGeneration ||
                studio.masterChannels != studio.lastIpcAnalysisChannels) {
                return ipcReply(false, "The project changed since the analysis; scan again.");
            }
            juce::String suggestions = studio.lastIpcSuggestions;
            int count = studio.lastIpcSuggestionCount;
            if (args.size() > 2) {
                const auto gaps = result->getProperty("gaps");
                const auto* rows = gaps.getArray();
                if (rows == nullptr) {
                    return ipcReply(false, "Quiet-gap results are unavailable; scan again.");
                }
                std::set<juce::int64> indices;
                juce::StringArray selected;
                for (int i = 2; i < args.size(); ++i) {
                    juce::int64 index = 0;
                    if (!parseNonNegativeIpcInt(args[i], index) || index >= rows->size() ||
                        !indices.insert(index).second) {
                        return ipcReply(false, "Invalid or duplicate gap index.");
                    }
                    const auto& gap = (*rows)[static_cast<int>(index)];
                    if (static_cast<bool>(gap["existing_marker"])) {
                        return ipcReply(false, "A selected gap already contains a marker.");
                    }
                    selected.add(gap["suggested_sample"].toString() + " " +
                                 gap["suggested_name"].toString());
                }
                suggestions = selected.joinIntoString("\n");
                count = selected.size();
            }
            if (suggestions.isEmpty()) {
                return ipcReply(false, "No unmarked quiet gaps to add.");
            }
            juce::String error;
            const bool added = studio.mutateMarkers(
                [&](carta::Session& candidate) {
                    if (studio.editGeneration != studio.lastIpcAnalysisGeneration ||
                        studio.masterChannels != studio.lastIpcAnalysisChannels) {
                        error = "The project changed since the analysis; scan again.";
                        return false;
                    }
                    return carta::addSuggestedMarkers(candidate, suggestions, error);
                },
                [] {}, "Add suggested markers", error);
            if (!added) {
                return ipcReply(false, error.isNotEmpty() ? error : "No markers were added.");
            }
            studio.status.setText("Added " + juce::String(count) +
                                      " suggested track markers; undo removes them together.",
                                  juce::dontSendNotification);
            auto* applied = new juce::DynamicObject();
            applied->setProperty("added", count);
            return ipcReply(true, juce::var(applied));
        }
        if (args.size() == 4 && args[1] == "quiet") {
            double threshold = 0;
            juce::int64 minimum = 0;
            if (!Studio::parseNumber(args[2], threshold) || threshold < -120 || threshold > 0 ||
                !parseNonNegativeIpcInt(args[3], minimum) || minimum < 1 ||
                minimum > 60 * 44100LL) {
                return ipcReply(
                    false,
                    "Threshold must be -120..0 dBFS and minimum duration 1..2646000 samples.");
            }
            if (studio.activeLevelAnalysis || !studio.pendingImports.empty() ||
                studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                return ipcReply(false,
                                "Finish the active analysis, imports, drag or dialog first.");
            }
            juce::String error;
            if (!studio.startIpcQuietAnalysis(threshold, minimum, error)) {
                return ipcReply(false, error);
            }
            return ipcReply(true, "Quiet-gap analysis queued");
        }
        if (args.size() == 5 && args[1] == "source-silence") {
            juce::int64 track = 0;
            juce::int64 clip = 0;
            juce::int64 minimum = 0;
            if (!parseNonNegativeIpcInt(args[2], track) || !parseNonNegativeIpcInt(args[3], clip) ||
                !parseNonNegativeIpcInt(args[4], minimum) || minimum < 1 ||
                minimum > 60 * 44100LL ||
                track >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                clip >= static_cast<juce::int64>(
                            studio.session.tracks[static_cast<size_t>(track)].clips.size())) {
                return ipcReply(
                    false, "Expected valid track and clip indices, and minimum duration 1..2646000 "
                           "samples.");
            }
            if (studio.activeLevelAnalysis || !studio.pendingImports.empty() ||
                studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr || studio.activeOpen) {
                return ipcReply(
                    false, "Finish the active analysis, imports, opening, drag or dialog first.");
            }
            juce::String error;
            if (!studio.startClipSilenceAnalysis(static_cast<int>(track), static_cast<int>(clip),
                                                 minimum, true, error)) {
                return ipcReply(false, error);
            }
            return ipcReply(true, "Source-silence analysis queued");
        }
        if ((args.size() == 3 || args.size() == 4) && args[1] == "levels") {
            const auto scope = args[2];
            juce::int64 index = 0;
            if (!((args.size() == 3 &&
                   (scope == "master" || scope == "selection" || scope == "tracks")) ||
                  (args.size() == 4 && scope == "track" &&
                   parseNonNegativeIpcInt(args[3], index)))) {
                return ipcReply(false,
                                "Usage: ipc analysis levels master|selection|tracks | track INDEX");
            }
            if (studio.activeLevelAnalysis || !studio.pendingImports.empty() ||
                studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                return ipcReply(false,
                                "Finish the active analysis, imports, drag or dialog first.");
            }
            juce::String error;
            if (!studio.startIpcLevelAnalysis(scope, index, error)) {
                return ipcReply(false, error);
            }
            return ipcReply(true, "Level analysis queued");
        }
        return ipcReply(
            false,
            "Usage: ipc analysis zero-crossings any|all | quiet THRESHOLD_DB MIN_SAMPLES | "
            "source-silence TRACK CLIP MIN_SAMPLES | levels master|selection|tracks | levels track "
            "INDEX | status | cancel | audition INDEX | select INDEX | detach INDEX | "
            "apply-markers "
            "[GAP_INDEX ...]");
    }

    static juce::var handleView(Studio& studio, const juce::StringArray& args) {
        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag or dialog first.");
        }
        if (args.size() == 3 && args[1] == "capture" && args[2] == "status") {
            return studio.lastIpcCapture.isVoid()
                       ? ipcReply(false, "No capture has been requested.")
                       : ipcReply(true, studio.lastIpcCapture);
        }
        if (args.size() == 3 && args[1] == "capture") {
            const juce::File file(args[2]);
            juce::String error;
            if (studio.captureRunning) {
                return ipcReply(false, "Wait for the current capture to finish.");
            }
            if (!juce::File::isAbsolutePath(args[2]) || !file.hasFileExtension("png") ||
                !file.getParentDirectory().isDirectory() || file.exists()) {
                return ipcReply(false, "Choose a new absolute .png path in an existing directory.");
            }
            if ((studio.activeSave && carta::pathsReferToSameFile(studio.activeSave->file, file)) ||
                (studio.activeExport &&
                 carta::pathsReferToSameFile(studio.activeExport->target, file)) ||
                !studio.validateRetainedMedia(file, error) ||
                !carta::validateOutputDestination(studio.session, file, studio.projectFile,
                                                  error)) {
                return ipcReply(false, error.isNotEmpty()
                                           ? error
                                           : "Choose a different path from the current output.");
            }
            if (studio.getWidth() <= 0 || studio.getHeight() <= 0 || studio.getWidth() > 4000 ||
                studio.getHeight() > 2500) {
                return ipcReply(false, "Editor dimensions are unavailable or too large.");
            }
            juce::Image image(juce::Image::ARGB, studio.getWidth(), studio.getHeight(), true,
                              juce::SoftwareImageType{});
            {
                juce::Graphics graphics(image);
                studio.paintEntireComponent(graphics, true);
            }
            int pending = 0;
            for (const auto& entry : studio.waveforms) {
                if (entry.second.pending) {
                    ++pending;
                }
            }
            for (const auto& entry : studio.spectra) {
                if (entry.second.pending) {
                    ++pending;
                }
            }
            auto* result = new juce::DynamicObject();
            result->setProperty("state", "running");
            result->setProperty("path", file.getFullPathName());
            result->setProperty("width", image.getWidth());
            result->setProperty("height", image.getHeight());
            result->setProperty("pending_tiles", pending);
            studio.lastIpcCapture = juce::var(result);
            studio.captureRunning = true;
            studio.capturePool.addJob(
                [safe = juce::Component::SafePointer<Studio>(&studio), file, image] {
                    juce::String captureError;
                    bool ok = false;
                    try {
                        ok = carta::writeNewPng(image, file, captureError);
                    } catch (...) {
                        captureError = "Capture failed unexpectedly.";
                    }
                    juce::MessageManager::callAsync([safe, ok, captureError] {
                        if (safe == nullptr) {
                            return;
                        }
                        safe->captureRunning = false;
                        if (auto* output = safe->lastIpcCapture.getDynamicObject()) {
                            output->setProperty("state", ok ? "complete" : "failed");
                            if (!ok) {
                                output->setProperty("error", captureError);
                            }
                        }
                    });
                });
            return ipcReply(true, "Capture queued");
        }
        if (args.size() == 2 && args[1] == "status") {
            auto* view = new juce::DynamicObject();
            view->setProperty("start_seconds", studio.viewStart);
            view->setProperty("end_seconds", studio.viewStart + studio.viewLength());
            view->setProperty("width", studio.getWidth());
            view->setProperty("height", studio.getHeight());
            view->setProperty("timeline_end_seconds", studio.fullLength());
            view->setProperty("first_track", studio.firstTrack);
            view->setProperty("default", studio.spectrumView ? "spectrogram" : "waveform");
            view->setProperty("waveform_rms", studio.showWaveformRms);
            view->setProperty("waveform_zoom", studio.waveformVerticalZoom);
            view->setProperty("waveform_scale", studio.waveformScale == carta::WaveformScale::linear
                                                    ? "linear"
                                                    : "decibels");
            view->setProperty("waveform_shape", studio.waveformHalf ? "half" : "full");
            view->setProperty("follow", studio.followMode == 0   ? "off"
                                        : studio.followMode == 1 ? "page"
                                                                 : "pinned");
            view->setProperty("follow_suspended", studio.followSuspended);
            view->setProperty("spectrum_band",
                              studio.spectrumBand == carta::SpectrumBand::full     ? "full"
                              : studio.spectrumBand == carta::SpectrumBand::bass   ? "bass"
                              : studio.spectrumBand == carta::SpectrumBand::voice  ? "voice"
                              : studio.spectrumBand == carta::SpectrumBand::treble ? "treble"
                                                                                   : "custom");
            view->setProperty("spectrum_window",
                              studio.spectrumWindow == carta::SpectrumWindow::hann ? "hann"
                              : studio.spectrumWindow == carta::SpectrumWindow::hamming
                                  ? "hamming"
                                  : "blackman");
            view->setProperty("spectrum_fft_size", 1 << studio.spectrumFftOrder);
            view->setProperty("spectrum_floor_db", studio.spectrumFloorDb);
            view->setProperty("spectrum_custom_low_hz", studio.spectrumCustomLow);
            view->setProperty("spectrum_custom_high_hz", studio.spectrumCustomHigh);
            return ipcReply(true, juce::var(view));
        }
        if (args.size() == 2 && args[1] == "fit") {
            studio.zoomToArrangement();
            return ipcReply(true, "View fitted");
        }
        if (args.size() == 3 && args[1] == "master-track") {
            juce::int64 index = 0;
            juce::String error;
            if (!parseNonNegativeIpcInt(args[2], index) ||
                index > std::numeric_limits<int>::max() ||
                !studio.zoomToMasterTrack(static_cast<int>(index), error)) {
                return ipcReply(false, error.isNotEmpty() ? error : "Invalid master track index.");
            }
            return ipcReply(true, "View fitted to master track");
        }
        if (args.size() == 4 && args[1] == "range") {
            juce::int64 start = 0;
            juce::int64 end = 0;
            if (!parseNonNegativeIpcInt(args[2], start) || !parseNonNegativeIpcInt(args[3], end) ||
                end <= start || end - start < 16 ||
                static_cast<double>(end) / carta::sessionRate > studio.fullLength()) {
                return ipcReply(
                    false,
                    "Expected a visible half-open sample range of at least 16 samples within the "
                    "timeline.");
            }
            studio.setZoomView(static_cast<double>(end - start) / carta::sessionRate,
                               static_cast<double>(start) / carta::sessionRate);
            return ipcReply(true, "View updated");
        }
        if (args.size() == 3 && args[1] == "default" &&
            (args[2] == "waveform" || args[2] == "spectrogram")) {
            studio.spectrumView = args[2] == "spectrogram";
            studio.menuItemsChanged();
            studio.repaint();
            return ipcReply(true, "Default view updated");
        }
        if (args.size() == 3 && args[1] == "waveform-rms" &&
            (args[2] == "on" || args[2] == "off")) {
            studio.setWaveformRms(args[2] == "on");
            return ipcReply(true, "Waveform RMS updated");
        }
        if (args.size() == 3 && args[1] == "waveform-zoom") {
            juce::int64 value = 0;
            if (!Studio::parseInteger(args[2], value) ||
                (value != 1 && value != 2 && value != 4 && value != 8)) {
                return ipcReply(false, "Waveform zoom must be 1, 2, 4 or 8.");
            }
            studio.setWaveformZoom(static_cast<int>(value));
            return ipcReply(true, "Waveform zoom updated");
        }
        if (args.size() == 3 && args[1] == "waveform-scale") {
            if (args[2] != "linear" && args[2] != "decibels") {
                return ipcReply(false, "Waveform scale must be linear or decibels.");
            }
            studio.setWaveformScale(args[2] == "linear" ? carta::WaveformScale::linear
                                                        : carta::WaveformScale::decibels);
            return ipcReply(true, "Waveform scale updated");
        }
        if (args.size() == 3 && args[1] == "waveform-shape") {
            if (args[2] != "full" && args[2] != "half") {
                return ipcReply(false, "Waveform shape must be full or half.");
            }
            studio.setWaveformHalf(args[2] == "half");
            return ipcReply(true, "Waveform shape updated");
        }
        if (args.size() == 3 && args[1] == "follow" && args[2] == "resume") {
            if (studio.followMode == 0) {
                return ipcReply(false, "Enable a follow mode first.");
            }
            studio.resumeFollowing();
            return ipcReply(true, "Playhead following resumed");
        }
        if (args.size() == 3 && args[1] == "follow" &&
            (args[2] == "off" || args[2] == "page" || args[2] == "pinned")) {
            studio.setFollowMode(args[2] == "off" ? 0 : args[2] == "page" ? 1 : 2);
            return ipcReply(true, "Follow mode updated");
        }
        if (args.size() == 4 && args[1] == "spectrum-range") {
            juce::int64 low = 0;
            juce::int64 high = 0;
            if (!parseNonNegativeIpcInt(args[2], low) || !parseNonNegativeIpcInt(args[3], high) ||
                low < 1 || high <= low || high > 192000) {
                return ipcReply(
                    false, "Spectrum range must be integer Hz with 1 <= low < high <= 192000.");
            }
            studio.setCustomSpectrumRange(static_cast<int>(low), static_cast<int>(high));
            return ipcReply(true, "Spectrum range updated");
        }
        if (args.size() == 3 && args[1] == "spectrum-preset") {
            const int preset = args[2] == "full"     ? 0
                               : args[2] == "bass"   ? 1
                               : args[2] == "voice"  ? 2
                               : args[2] == "treble" ? 3
                                                     : -1;
            if (preset < 0) {
                return ipcReply(false, "Spectrum preset must be full, bass, voice or treble.");
            }
            studio.setSpectrumPreset(preset);
            return ipcReply(true, "Spectrum preset updated");
        }
        if (args.size() == 3 && args[1] == "spectrum-window") {
            if (args[2] != "hann" && args[2] != "hamming" && args[2] != "blackman") {
                return ipcReply(false, "Spectrum window must be hann, hamming or blackman.");
            }
            studio.setSpectrumWindow(args[2] == "hann"      ? carta::SpectrumWindow::hann
                                     : args[2] == "hamming" ? carta::SpectrumWindow::hamming
                                                            : carta::SpectrumWindow::blackman);
            return ipcReply(true, "Spectrum window updated");
        }
        if (args.size() == 3 && args[1] == "spectrum-floor") {
            juce::int64 floor = 0;
            if (!Studio::parseInteger(args[2], floor) || floor < -120 || floor > -40 ||
                floor % 20 != 0) {
                return ipcReply(false, "Spectrum floor must be -40, -60, -80, -100 or -120 dB.");
            }
            studio.setSpectrumFloor(static_cast<int>(floor));
            return ipcReply(true, "Spectrum floor updated");
        }
        if (args.size() == 4 && args[1] == "resize") {
            juce::int64 width = 0;
            juce::int64 height = 0;
            if (!Studio::parseInteger(args[2], width) || !Studio::parseInteger(args[3], height) ||
                width < 600 || width > 4000 || height < 350 || height > 2500) {
                return ipcReply(false,
                                "Window dimensions must be WIDTH 600..4000 and HEIGHT 350..2500.");
            }
            if (auto* top = studio.findParentComponentOfClass<juce::DocumentWindow>()) {
                top->setContentComponentSize(static_cast<int>(width), static_cast<int>(height));
            } else {
                studio.setSize(static_cast<int>(width), static_cast<int>(height));
            }
            studio.resized();
            studio.repaint();
            auto* out = new juce::DynamicObject();
            out->setProperty("width", studio.getWidth());
            out->setProperty("height", studio.getHeight());
            return ipcReply(true, juce::var(out));
        }
        if (args.size() == 3 && args[1] == "spectrum-fft") {
            juce::int64 size = 0;
            if (!Studio::parseInteger(args[2], size) ||
                (size != 1024 && size != 2048 && size != 4096 && size != 8192)) {
                return ipcReply(false,
                                "Spectrum FFT size must be 1024, 2048, 4096 or 8192 samples.");
            }
            studio.setSpectrumFftOrder(size == 1024   ? 10
                                       : size == 2048 ? 11
                                       : size == 4096 ? 12
                                                      : 13);
            return ipcReply(true, "Spectrum FFT size updated");
        }
        return ipcReply(
            false,
            "Usage: ipc view status | resize WIDTH HEIGHT | capture FILE | fit | master-track "
            "INDEX | range START_SAMPLE END_SAMPLE | default waveform|spectrogram | waveform-rms "
            "on|off | waveform-zoom 1|2|4|8 | waveform-scale linear|decibels | waveform-shape "
            "full|half | follow off|page|pinned | spectrum-range LOW HIGH | spectrum-preset "
            "full|bass|voice|treble | spectrum-window hann|hamming|blackman | spectrum-floor "
            "-40|-60|-80|-100|-120 | spectrum-fft 1024|2048|4096|8192");
    }

    static juce::var handleLoop(Studio& studio, const juce::StringArray& args) {
        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag or dialog first.");
        }
        juce::String error;
        if (args.size() == 4 && args[1] == "set") {
            juce::int64 first = 0;
            juce::int64 last = 0;
            if (!parseNonNegativeIpcInt(args[2], first) || !parseNonNegativeIpcInt(args[3], last) ||
                !studio.setSavedLoop(first, last, error)) {
                return ipcReply(false,
                                error.isNotEmpty() ? error : "Invalid half-open sample range.");
            }
            return ipcReply(true, "Loop region set");
        }
        if (args.size() == 2 && args[1] == "from-selection") {
            return studio.setSavedLoop(studio.timeSelection.start, studio.timeSelection.end, error)
                       ? ipcReply(true, "Loop region set")
                       : ipcReply(false, error);
        }
        if (args.size() == 2 && args[1] == "clear") {
            return studio.clearSavedLoop() ? ipcReply(true, "Loop region cleared")
                                           : ipcReply(false, "No loop region is set.");
        }
        if (args.size() == 2 && args[1] == "play") {
            const auto outputError = studio.audioOutputError();
            if (outputError.isNotEmpty()) {
                return ipcReply(false, outputError);
            }
            return studio.startSavedLoop(error) ? ipcReply(true, "Loop started")
                                                : ipcReply(false, error);
        }
        return ipcReply(false, "Usage: ipc loop set START END | from-selection | play | clear");
    }

    static juce::var handleSeek(Studio& studio, const juce::StringArray& args) {
        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag or dialog first.");
        }
        juce::int64 sample = 0;
        if (args.size() != 2 || !parseNonNegativeIpcInt(args[1], sample)) {
            return ipcReply(false, "Usage: ipc seek SAMPLE");
        }
#if CARTA_STUDIO_COMPONENT_TEST
        if (studio.navigationPreviewHook) {
            studio.navigationPreviewHook();
        }
#endif
        if (sample > carta::projectEndSample(studio.session)) {
            return ipcReply(false, "Sample is beyond the project end.");
        }
        {
            const juce::ScopedLock guard(studio.lock);
            studio.stopSpecialAudition();
            studio.position = static_cast<double>(sample) / carta::sessionRate;
            studio.auditionActive = false;
            studio.keyboardRange.active = false;
        }
        studio.repaint();
        return ipcReply(true, sample);
    }

    static juce::var handleSelect(Studio& studio, const juce::StringArray& args) {
        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag or dialog first.");
        }
        if (args.size() >= 2 && args[1] == "source-clips") {
            if (args.size() > 3 || (args.size() == 3 && args[2] != "all" && args[2] != "tracks" &&
                                    args[2] != "range")) {
                return ipcReply(false, "Usage: ipc select source-clips [all|tracks|range]");
            }
            const int scope = args.size() == 2 || args[2] == "all" ? 0
                              : args[2] == "tracks"                ? 1
                                                                   : 2;
            return studio.selectSourceClips(scope) ? ipcReply(true, "Source clips selected")
                                                   : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 2 && args[1] == "clip-span") {
            return studio.selectSelectedClipSpan() ? ipcReply(true, "Clip span selected")
                                                   : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 3 && args[1] == "focus-track") {
            juce::int64 track = 0;
            if (!parseNonNegativeIpcInt(args[2], track) ||
                track >= static_cast<juce::int64>(studio.session.tracks.size())) {
                return ipcReply(false, "Invalid track index.");
            }
            studio.focusTrackWithoutSeeking(static_cast<int>(track));
            return ipcReply(true, "Track focused");
        }
        if (args.size() == 4 && args[1] == "focus-clip") {
            juce::int64 track = 0;
            juce::int64 clip = 0;
            if (!parseNonNegativeIpcInt(args[2], track) || !parseNonNegativeIpcInt(args[3], clip) ||
                track >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                clip >= static_cast<juce::int64>(
                            studio.session.tracks[static_cast<size_t>(track)].clips.size())) {
                return ipcReply(false, "Invalid track or clip index.");
            }
            studio.focusClipWithoutSeeking(static_cast<int>(track), static_cast<int>(clip));
            return ipcReply(true, "Clip focused");
        }
        if (args.size() >= 2 && args[1] == "clip-scope") {
            if (args.size() != 3 ||
                (args[2] != "all" && args[2] != "tracks" && args[2] != "range")) {
                return ipcReply(false, "Usage: ipc select clip-scope all|tracks|range");
            }
            return studio.selectClipsInScope(args[2] == "all"      ? 206
                                             : args[2] == "tracks" ? 207
                                                                   : 208)
                       ? ipcReply(true, "Clips selected")
                       : ipcReply(false, studio.status.getText());
        }

        const juce::ScopedLock guard(studio.lock);
        if (args.size() >= 2 && args[1] == "memory") {
            if (args.size() != 3) {
                return ipcReply(false, "Usage: ipc select memory status|remember|restore|clear");
            }
            if (args[2] == "status") {
                const bool available = studio.selectionMemory.canRestore(
                    static_cast<int>(studio.session.tracks.size()), studio.editGeneration);
                auto* result = new juce::DynamicObject();
                result->setProperty("present", studio.selectionMemory.present);
                result->setProperty("available", available);
                if (available) {
                    const auto& value = studio.selectionMemory.selection;
                    auto* selection = new juce::DynamicObject();
                    selection->setProperty("start_sample", value.start);
                    selection->setProperty("end_sample", value.end);
                    selection->setProperty("first_track", value.firstTrack);
                    selection->setProperty("last_track", value.lastTrack);
                    juce::Array<juce::var> tracks;
                    for (int track : studio.rememberedTimelineTracks) {
                        tracks.add(track);
                    }
                    selection->setProperty("independent_tracks", juce::var(tracks));
                    result->setProperty("selection", juce::var(selection));
                } else {
                    result->setProperty("selection", juce::var());
                }
                return ipcReply(true, juce::var(result));
            }
            if (args[2] == "clear") {
                return studio.clearRememberedSelection()
                           ? ipcReply(true, "Selection bookmark cleared")
                           : ipcReply(false, studio.status.getText());
            }
            if (args[2] == "remember" || args[2] == "restore") {
                return studio.rememberSelection(args[2] == "restore")
                           ? ipcReply(true, "Selection bookmark updated")
                           : ipcReply(false, studio.status.getText());
            }
            return ipcReply(false, "Usage: ipc select memory status|remember|restore|clear");
        }
        if (args.size() == 3 && args[1] == "clip-time") {
            const int portion = args[2] == "all"       ? 0
                                : args[2] == "visible" ? 249
                                : args[2] == "before"  ? 250
                                : args[2] == "after"   ? 251
                                                       : -1;
            if (portion < 0) {
                return ipcReply(false, "Usage: ipc select clip-time all|visible|before|after");
            }
            return studio.selectClipTime(portion) ? ipcReply(true, "Clip time selected")
                                                  : ipcReply(false, studio.status.getText());
        }
        if (args.size() >= 2 && args[1] == "point-gap") {
            int scope = 0;
            std::vector<int> tracks;
            if (args.size() == 3 && args[2] == "selected") {
                scope = 1;
            } else if (args.size() == 3 && args[2] == "all") {
                scope = 2;
            } else if (args.size() >= 4 && args[2] == "tracks") {
                scope = 3;
                std::set<int> seen;
                for (int i = 3; i < args.size(); ++i) {
                    juce::int64 track = 0;
                    if (!parseNonNegativeIpcInt(args[i], track) ||
                        track >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                        !seen.insert(static_cast<int>(track)).second) {
                        return ipcReply(false, "Invalid or duplicate track index.");
                    }
                    tracks.push_back(static_cast<int>(track));
                }
            } else if (args.size() != 2) {
                return ipcReply(false,
                                "Usage: ipc select point-gap [selected|all|tracks INDEX ...]");
            }
            return studio.selectGapAtEditPoint(scope, tracks)
                       ? ipcReply(true, "Gap selected")
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 3 && (args[1] == "point-boundary" || args[1] == "point-range")) {
            const auto name = args[2];
            const int action = name == "start"               ? 0
                               : name == "end"               ? 1
                               : name == "previous-marker"   ? 2
                               : name == "next-marker"       ? 3
                               : name == "previous-clip"     ? 4
                               : name == "next-clip"         ? 5
                               : name == "previous-clip-all" ? 6
                               : name == "next-clip-all"     ? 7
                                                             : -1;
            if (action < 0) {
                return ipcReply(false, "Invalid edit-point boundary.");
            }
            if (args[1] == "point-range") {
                return studio.selectFromEditPoint(action)
                           ? ipcReply(true, "Range selected")
                           : ipcReply(false, studio.status.getText());
            }
            return studio.navigateEditPoint(action) ? ipcReply(true, studio.timeSelection.start)
                                                    : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 4 && args[1] == "point") {
            juce::int64 track = 0;
            juce::int64 sample = 0;
            if (!parseNonNegativeIpcInt(args[2], track) ||
                !parseNonNegativeIpcInt(args[3], sample) ||
                track >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                sample > carta::projectEndSample(studio.session)) {
                return ipcReply(false, "Invalid track or edit-point sample.");
            }
            studio.prepareSelectionFocus();
            studio.selectedTrack = static_cast<int>(track);
            studio.selectedClip = studio.selectedMarker = -1;
            studio.timelineTracks.clear();
            studio.timelineClipSelection.clear();
            studio.keyboardRange.active = false;
            studio.keyboardTracks.active = false;
            studio.timeSelection = {sample, sample, studio.selectedTrack, studio.selectedTrack};
            if (!studio.selectionPlaybackContinues()) {
                studio.position = static_cast<double>(sample) / carta::sessionRate;
            }
            studio.revealTrack(studio.selectedTrack);
            studio.status.setText("Edit point: sample " + juce::String(sample) + ", track " +
                                      juce::String(studio.selectedTrack + 1) + ".",
                                  juce::dontSendNotification);
            studio.menuItemsChanged();
            studio.repaint();
            return ipcReply(true, "Edit point selected");
        }
        if (args.size() == 4 && args[1] == "clip") {
            juce::int64 track = 0;
            juce::int64 clip = 0;
            if (!parseNonNegativeIpcInt(args[2], track) || !parseNonNegativeIpcInt(args[3], clip) ||
                track >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                clip >= static_cast<juce::int64>(
                            studio.session.tracks[static_cast<size_t>(track)].clips.size())) {
                return ipcReply(false, "Invalid track or clip index.");
            }
            studio.prepareSelectionFocus();
            studio.selectedTrack = static_cast<int>(track);
            studio.selectedClip = static_cast<int>(clip);
            studio.selectedMarker = -1;
            studio.timeSelection = {};
            studio.timelineTracks.clear();
            studio.timelineClipSelection.clear();
            studio.keyboardRange.active = false;
            studio.keyboardTracks.active = false;
            studio.revealTrack(studio.selectedTrack);
            studio.menuItemsChanged();
            studio.repaint();
            return ipcReply(true, "Clip focused");
        }
        if (args.size() >= 4 && args[1] == "clips" && (args.size() - 2) % 2 == 0) {
            carta::ClipSelection chosen;
            std::set<std::pair<int, int>> seen;
            std::vector<carta::ClipReference> requested;
            int focusTrack = -1;
            int focusClip = -1;
            for (int i = 2; i < args.size(); i += 2) {
                juce::int64 track = 0;
                juce::int64 clip = 0;
                if (!parseNonNegativeIpcInt(args[i], track) ||
                    !parseNonNegativeIpcInt(args[i + 1], clip) ||
                    track >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                    clip >= static_cast<juce::int64>(
                                studio.session.tracks[static_cast<size_t>(track)].clips.size()) ||
                    !seen.emplace(static_cast<int>(track), static_cast<int>(clip)).second) {
                    return ipcReply(false, "Invalid or duplicate clip reference.");
                }
                if (focusTrack < 0) {
                    focusTrack = static_cast<int>(track);
                    focusClip = static_cast<int>(clip);
                }
                requested.push_back({static_cast<int>(track), static_cast<int>(clip)});
            }
            const auto revision = studio.editGeneration;
            const int previousTrack = studio.selectedTrack;
            const int previousClip = studio.selectedClip;
            const int previousMarker = studio.selectedMarker;
            const auto previousTime = studio.timeSelection;
            const auto previousRows = studio.selectedTimelineTracks();
            const auto previousClips = studio.timelineClipSelection.get(revision);
            const auto unchanged = [&] {
                return studio.editGeneration == revision && studio.selectedTrack == previousTrack &&
                       studio.selectedClip == previousClip &&
                       studio.selectedMarker == previousMarker &&
                       studio.timeSelection.start == previousTime.start &&
                       studio.timeSelection.end == previousTime.end &&
                       studio.timeSelection.firstTrack == previousTime.firstTrack &&
                       studio.timeSelection.lastTrack == previousTime.lastTrack &&
                       studio.selectedTimelineTracks() == previousRows &&
                       sameClipReferences(studio.timelineClipSelection.get(revision),
                                          previousClips);
            };
            bool valid = false;
            bool stale = false;
            {
                const juce::ScopedUnlock unlock(studio.lock);
#if CARTA_STUDIO_COMPONENT_TEST
                if (studio.scopeSelectionHook) {
                    studio.scopeSelectionHook();
                }
#endif
                stale = !unchanged();
                if (!stale) {
                    valid = chosen.selectExpanded(studio.session, requested, revision);
                }
            }
            if (stale || !unchanged()) {
                return ipcReply(
                    false, "The project or selection changed; clip selection was not applied.");
            }
            if (!valid) {
                return ipcReply(false, "Invalid clip selection or group identity.");
            }
            studio.prepareSelectionFocus();
            studio.selectedTrack = focusTrack;
            studio.selectedClip = focusClip;
            studio.selectedMarker = -1;
            studio.timeSelection = {};
            studio.timelineTracks.clear();
            studio.timelineClipSelection = std::move(chosen);
            studio.keyboardRange.active = false;
            studio.keyboardTracks.active = false;
            studio.revealTrack(studio.selectedTrack);
            studio.menuItemsChanged();
            studio.repaint();
            return ipcReply(true, "Clips selected");
        }
        if (args.size() == 6 && args[1] == "range") {
            juce::int64 first = 0;
            juce::int64 last = 0;
            juce::int64 start = 0;
            juce::int64 end = 0;
            if (!parseNonNegativeIpcInt(args[2], first) || !parseNonNegativeIpcInt(args[3], last) ||
                !parseNonNegativeIpcInt(args[4], start) || !parseNonNegativeIpcInt(args[5], end) ||
                first > last || last >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                start >= end || end > carta::projectEndSample(studio.session)) {
                return ipcReply(false, "Invalid track or half-open sample range.");
            }
            carta::TimeSelection selection{start, end, static_cast<int>(first),
                                           static_cast<int>(last)};
            if (!carta::activateTimeSelection(studio.timeSelection, studio.timelineClipSelection,
                                              selection,
                                              static_cast<int>(studio.session.tracks.size()))) {
                return ipcReply(false, "Invalid selection.");
            }
            studio.prepareSelectionFocus();
            studio.selectedTrack = static_cast<int>(first);
            studio.selectedClip = studio.selectedMarker = -1;
            studio.timelineTracks.clear();
            studio.keyboardRange.active = false;
            studio.keyboardTracks.active = false;
            studio.revealTrack(studio.selectedTrack);
            studio.menuItemsChanged();
            studio.repaint();
            return ipcReply(true, "Range selected");
        }
        if (args.size() >= 5 && args[1] == "tracks") {
            juce::int64 start = 0;
            juce::int64 end = 0;
            juce::int64 index = 0;
            std::set<int> tracks;
            if (!parseNonNegativeIpcInt(args[2], start) || !parseNonNegativeIpcInt(args[3], end) ||
                start >= end || end > carta::projectEndSample(studio.session)) {
                return ipcReply(false, "Invalid half-open sample range.");
            }
            for (int i = 4; i < args.size(); ++i) {
                if (!parseNonNegativeIpcInt(args[i], index) ||
                    index >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                    !tracks.insert(static_cast<int>(index)).second) {
                    return ipcReply(false, "Invalid or duplicate track index.");
                }
            }
            carta::TimeSelection selection{start, end, *tracks.begin(), *tracks.rbegin()};
            if (!carta::activateTimeSelection(studio.timeSelection, studio.timelineClipSelection,
                                              selection,
                                              static_cast<int>(studio.session.tracks.size()))) {
                return ipcReply(false, "Invalid selection.");
            }
            studio.prepareSelectionFocus();
            studio.selectedTrack = *tracks.begin();
            studio.selectedClip = studio.selectedMarker = -1;
            studio.timelineTracks = std::move(tracks);
            studio.timelineTrackRevision = studio.editGeneration;
            studio.keyboardRange.active = false;
            studio.keyboardTracks.active = false;
            studio.revealTrack(studio.selectedTrack);
            studio.menuItemsChanged();
            studio.repaint();
            return ipcReply(true, "Independent tracks selected");
        }
        return ipcReply(
            false,
            "Usage: ipc select clip-time all|visible|before|after | point TRACK SAMPLE | clip "
            "TRACK "
            "CLIP | focus-track TRACK | focus-clip TRACK CLIP | clips TRACK CLIP [TRACK CLIP ...] "
            "| range FIRST_TRACK LAST_TRACK START_SAMPLE END_SAMPLE | ipc select tracks "
            "START_SAMPLE END_SAMPLE TRACK [TRACK ...]");
    }

    static juce::var handleEdit(Studio& studio, const juce::StringArray& args) {
        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag or dialog first.");
        }
        if (args.size() == 2 && (args[1] == "duplicate-time" || args[1] == "trim-time")) {
            if (!studio.timeSelection.hasRange()) {
                return ipcReply(false, "Select a time range first.");
            }
            const bool changed = args[1] == "duplicate-time" ? studio.duplicateTimeSelection()
                                                             : studio.trimSelectionTracks();
            return changed ? ipcReply(true, args[1]) : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 2 && args[1] == "join-next") {
            juce::String error;
            return studio.joinFocusedClip(error) ? ipcReply(true, "Joined contiguous source clips")
                                                 : ipcReply(false, error);
        }
        if (args.size() == 2 && args[1] == "split-to-new-tracks") {
            juce::String error;
            return studio.splitSelectionToNewTracks(error)
                       ? ipcReply(true, "Selected time moved to new tracks")
                       : ipcReply(false, error);
        }
        if (args.size() == 3 && args[1] == "split-tracks") {
            if (args[2] != "selected" && args[2] != "all") {
                return ipcReply(false, "Usage: ipc edit split-tracks selected|all");
            }
            juce::String error;
            return studio.splitTracksAtPlayhead(args[2] == "all", error)
                       ? ipcReply(true, "Tracks split at playhead")
                       : ipcReply(false, error);
        }
        if (args.size() == 2 && args[1] == "crossfade-selected") {
            return studio.crossfadeSelectedPair() ? ipcReply(true, "Crossfade applied")
                                                  : ipcReply(false, studio.status.getText());
        }
        if (args[1] == "amplify") {
            double deltaDb = 0.0;
            juce::String error;
            if (args.size() != 3 || !Studio::parseNumber(args[2], deltaDb)) {
                return ipcReply(false, "Usage: ipc edit amplify DB");
            }
            return studio.applyClipMixOrRangeGainEdit(false, deltaDb, false, "Amplify", error)
                       ? ipcReply(true, "Amplified")
                       : ipcReply(false, error);
        }
        if (args[1] == "normalize") {
            double targetDb = -1.0;
            bool independent = false;
            juce::String error;
            if (args.size() == 3) {
                if (args[2] == "linked" || args[2] == "independent") {
                    independent = (args[2] == "independent");
                } else if (!Studio::parseNumber(args[2], targetDb)) {
                    return ipcReply(false,
                                    "Usage: ipc edit normalize [TARGET_DB] [linked|independent]");
                }
            } else if (args.size() == 4) {
                if (!Studio::parseNumber(args[2], targetDb) ||
                    (args[3] != "linked" && args[3] != "independent")) {
                    return ipcReply(false,
                                    "Usage: ipc edit normalize [TARGET_DB] [linked|independent]");
                }
                independent = (args[3] == "independent");
            } else if (args.size() != 2) {
                return ipcReply(false,
                                "Usage: ipc edit normalize [TARGET_DB] [linked|independent]");
            }
            return studio.applyClipMixOrRangeGainEdit(true, targetDb, independent, "Normalize",
                                                      error)
                       ? ipcReply(true, "Normalized")
                       : ipcReply(false, error);
        }
        if ((args.size() == 5 && args[1] == "move-clip") ||
            (args.size() == 6 && args[1] == "trim-clip")) {
            juce::int64 track = 0;
            juce::int64 clip = 0;
            juce::int64 start = 0;
            juce::int64 end = 0;
            juce::String error;
            const bool trim = args[1] == "trim-clip";
            const bool setStart = args[4] != "keep";
            const bool setEnd = trim && args[5] != "keep";
            if (!parseNonNegativeIpcInt(args[2], track) || !parseNonNegativeIpcInt(args[3], clip) ||
                track >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                clip >= static_cast<juce::int64>(
                            studio.session.tracks[static_cast<size_t>(track)].clips.size()) ||
                (!trim && !setStart) || (trim && !setStart && !setEnd) ||
                (setStart && !parseNonNegativeIpcInt(args[4], start)) ||
                (setEnd && !parseNonNegativeIpcInt(args[5], end))) {
                return ipcReply(
                    false,
                    "Usage: ipc edit move-clip TRACK CLIP START | ipc edit trim-clip TRACK CLIP "
                    "START|keep END|keep");
            }
            return studio.editSingleClipSamples(static_cast<int>(track), static_cast<int>(clip),
                                                studio.editGeneration, trim, setStart, start,
                                                setEnd, end, error)
                       ? ipcReply(true, trim ? "Clip trimmed" : "Clip moved")
                       : ipcReply(false, error);
        }
        if (args.size() == 5 && args[1] == "rename-clip") {
            juce::int64 track = 0;
            juce::int64 clip = 0;
            juce::String error;
            if (!parseNonNegativeIpcInt(args[2], track) || !parseNonNegativeIpcInt(args[3], clip) ||
                track >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                clip >= static_cast<juce::int64>(
                            studio.session.tracks[static_cast<size_t>(track)].clips.size())) {
                return ipcReply(false, "Usage: ipc edit rename-clip TRACK CLIP NAME");
            }
            return studio.renameClipTo(static_cast<int>(track), static_cast<int>(clip),
                                       studio.editGeneration, args[4], error)
                       ? ipcReply(true, "Clip renamed")
                       : ipcReply(false, error);
        }
        if (args.size() == 2 && args[1] == "move-range-impact") {
            if (!studio.timeSelection.hasRange()) {
                return ipcReply(false, "Select a time range first.");
            }
            auto* impact = new juce::DynamicObject();
            impact->setProperty("linked_groups",
                                carta::rangeMoveLinkedGroups(studio.session, studio.timeSelection,
                                                             studio.selectedTimelineTracks()));
            return ipcReply(true, juce::var(impact));
        }
        if (args.size() == 4 && args[1] == "move-range") {
            juce::int64 target = 0;
            juce::int64 start = 0;
            juce::String error;
            if (!parseNonNegativeIpcInt(args[2], target) ||
                target > std::numeric_limits<int>::max() ||
                !parseNonNegativeIpcInt(args[3], start)) {
                return ipcReply(false, "Usage: ipc edit move-range TARGET_TRACK START_SAMPLE");
            }
            const auto selection = studio.timeSelection;
            const auto tracks = studio.selectedTimelineTracks();
            return studio.moveTimeSelectionTo(studio.editGeneration, selection, tracks,
                                              static_cast<int>(target), start, error)
                       ? ipcReply(true, "Selected time moved")
                       : ipcReply(false, error);
        }
        if (args.size() == 3 &&
            (args[1] == "close-gap" || args[1] == "cut-close-gap" || args[1] == "insert-gap")) {
            if (args[2] != "focused" && args[2] != "tracks" && args[2] != "all") {
                return ipcReply(
                    false, "Usage: ipc edit close-gap|cut-close-gap|insert-gap focused|tracks|all");
            }
            if (!studio.timeSelection.hasRange()) {
                return ipcReply(false, "Select a time range first.");
            }
            const auto before = studio.editGeneration;
            const bool all = args[2] == "all";
            const bool focused = args[2] == "focused";
            if (args[1] == "insert-gap") {
                studio.insertSelectionGap(all, focused);
            } else if (args[1] == "cut-close-gap") {
                studio.rippleCutSelection(all, focused);
            } else {
                studio.rippleSelection(all, focused);
            }
            return studio.editGeneration != before
                       ? ipcReply(true,
                                  args[1] == "close-gap" ? juce::String("Gap closed") : args[1])
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 2 && (args[1] == "group" || args[1] == "ungroup")) {
            return studio.setPersistentClipGrouping(args[1] == "group")
                       ? ipcReply(true, args[1])
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 2 && (args[1] == "sequence" || args[1] == "distribute")) {
            return studio.sequenceSelectedClips(args[1] == "distribute")
                       ? ipcReply(true, args[1])
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 6 && args[1] == "align-clips") {
            juce::int64 track = 0;
            juce::int64 clip = 0;
            juce::int64 sample = 0;
            if (!parseNonNegativeIpcInt(args[2], track) || !parseNonNegativeIpcInt(args[3], clip) ||
                track >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                clip >= static_cast<juce::int64>(
                            studio.session.tracks[static_cast<size_t>(track)].clips.size()) ||
                (args[4] != "start" && args[4] != "end") ||
                !parseNonNegativeIpcInt(args[5], sample)) {
                return ipcReply(false, "Usage: ipc edit align-clips TRACK CLIP start|end SAMPLE");
            }
            const auto generation = studio.editGeneration;
            const auto selected = studio.timelineClipSelection.get(generation);
            return studio.alignSelectedGroupSample(
                       selected, generation, {static_cast<int>(track), static_cast<int>(clip)},
                       args[4] == "end", sample)
                       ? ipcReply(true, "align-clips")
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 3 && args[1] == "align-group") {
            if (args[2] != "start" && args[2] != "end") {
                return ipcReply(false, "Usage: ipc edit align-group start|end");
            }
            return studio.trimSelectedEdgesToPlayhead(args[2] == "end")
                       ? ipcReply(true, "align-group")
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 3 && args[1] == "clips") {
            const int action = args[2] == "copy"        ? 188
                               : args[2] == "cut"       ? 189
                               : args[2] == "delete"    ? 190
                               : args[2] == "duplicate" ? 191
                               : args[2] == "split"     ? 192
                               : args[2] == "join"      ? 291
                                                        : -1;
            if (action < 0) {
                return ipcReply(false,
                                "Usage: ipc edit clips copy|cut|delete|duplicate|split|join");
            }
            return studio.performTimelineClipAction(action)
                       ? ipcReply(true, args[2])
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() >= 2 && args[1] == "slip-clip") {
            juce::int64 track = 0;
            juce::int64 clip = 0;
            juce::int64 samples = 0;
            juce::String error;
            if (args.size() != 5 || !parseNonNegativeIpcInt(args[2], track) ||
                !parseNonNegativeIpcInt(args[3], clip) ||
                track >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                clip >= static_cast<juce::int64>(
                            studio.session.tracks[static_cast<size_t>(track)].clips.size()) ||
                !Studio::parseInteger(args[4], samples)) {
                return ipcReply(false, "Usage: ipc edit slip-clip TRACK CLIP SIGNED_SAMPLES");
            }
            return studio.slipClipAt(static_cast<int>(track), static_cast<int>(clip),
                                     studio.editGeneration, samples, false, error)
                       ? ipcReply(true, "slip-clip")
                       : ipcReply(false, error);
        }
        if (args.size() >= 2 && args[1] == "slip-clips") {
            juce::int64 samples = 0;
            if (args.size() != 3 || !Studio::parseInteger(args[2], samples)) {
                return ipcReply(false, "Usage: ipc edit slip-clips SIGNED_SAMPLES");
            }
            const auto generation = studio.editGeneration;
            const auto clips = studio.timelineClipSelection.get(generation);
            return studio.slipSelectedClips(clips, generation, samples)
                       ? ipcReply(true, "slip-clips")
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 4 && args[1] == "trim-group") {
            juce::int64 samples = 0;
            if ((args[2] != "start" && args[2] != "end") ||
                !Studio::parseInteger(args[3], samples)) {
                return ipcReply(false, "Usage: ipc edit trim-group start|end SIGNED_SAMPLES");
            }
            const auto generation = studio.editGeneration;
            const auto clips = studio.timelineClipSelection.get(generation);
            return studio.adjustSelectedClipEdges(clips, generation, args[2] == "end", samples)
                       ? ipcReply(true, "trim-group")
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 6 && args[1] == "trim-group-range") {
            juce::int64 track = 0;
            juce::int64 clip = 0;
            juce::int64 start = 0;
            juce::int64 end = 0;
            const bool setStart = args[4] != "keep";
            const bool setEnd = args[5] != "keep";
            if (!parseNonNegativeIpcInt(args[2], track) || !parseNonNegativeIpcInt(args[3], clip) ||
                track >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                clip >= static_cast<juce::int64>(
                            studio.session.tracks[static_cast<size_t>(track)].clips.size()) ||
                (!setStart && !setEnd) || (setStart && !parseNonNegativeIpcInt(args[4], start)) ||
                (setEnd && !parseNonNegativeIpcInt(args[5], end))) {
                return ipcReply(false,
                                "Usage: ipc edit trim-group-range TRACK CLIP START|keep END|keep");
            }
            const auto generation = studio.editGeneration;
            const auto clips = studio.timelineClipSelection.get(generation);
            return studio.trimSelectedGroupRange(clips, generation,
                                                 {static_cast<int>(track), static_cast<int>(clip)},
                                                 setStart, start, setEnd, end)
                       ? ipcReply(true, "trim-group-range")
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 3 && (args[1] == "paste" || args[1] == "paste-point")) {
            const auto policy = args[2] == "mix"          ? 0
                                : args[2] == "replace"    ? 1
                                : args[2] == "insert"     ? 2
                                : args[2] == "insert-all" ? 3
                                                          : -1;
            if (policy < 0) {
                return ipcReply(false,
                                "Usage: ipc edit paste|paste-point mix|replace|insert|insert-all");
            }
            const auto before = studio.editGeneration;
            studio.pasteSelection(policy, args[1] == "paste-point");
            return before != studio.editGeneration
                       ? ipcReply(true, "paste")
                       : ipcReply(false, "Paste had no effect: " + studio.status.getText());
        }
        if (args.size() != 2) {
            return ipcReply(false,
                            "Usage: ipc edit split|delete|undo|redo|copy|cut | ipc edit paste "
                            "mix|replace|insert|insert-all");
        }
        if (args[1] == "copy" || args[1] == "cut") {
            if (!studio.timeSelection.hasRange()) {
                return ipcReply(false, "Select a time range to copy or cut.");
            }
            return studio.copySelection(args[1] == "cut")
                       ? ipcReply(true, args[1])
                       : ipcReply(false, studio.status.getText());
        }
        const auto before = studio.editGeneration;
        if (args[1] == "split") {
            studio.split.onClick();
        } else if (args[1] == "delete") {
            studio.remove.onClick();
        } else if (args[1] == "undo") {
            studio.restoreHistory(false);
        } else if (args[1] == "redo") {
            studio.restoreHistory(true);
        } else {
            return ipcReply(false,
                            "Usage: ipc edit split|delete|undo|redo|copy|cut | ipc edit paste "
                            "mix|replace|insert|insert-all");
        }
        if (before == studio.editGeneration) {
            return ipcReply(false, "Edit had no effect: " + studio.status.getText());
        }
        return ipcReply(true, args[1]);
    }

    static juce::var handleMarker(Studio& studio, const juce::StringArray& args) {
        if (args.size() == 3 && args[1] == "exchange" && args[2] == "status") {
            if (studio.lastIpcMarkerExchange.isVoid()) {
                return ipcReply(false, "No IPC marker exchange has been started.");
            }
            if (auto* result = studio.lastIpcMarkerExchange.getDynamicObject()) {
                result->setProperty("stale", studio.editGeneration !=
                                                 studio.lastIpcMarkerExchangeGeneration);
                if (studio.activeMarkerExchange && studio.activeMarkerExchange->ipc) {
                    result->setProperty("cancellation_requested",
                                        studio.activeMarkerExchange->cancelled.load());
                }
            }
            return ipcReply(true, studio.lastIpcMarkerExchange);
        }
        if (args.size() == 3 && args[1] == "exchange" && args[2] == "cancel") {
            if (!studio.activeMarkerExchange || !studio.activeMarkerExchange->ipc) {
                return ipcReply(false, "No IPC marker exchange is running.");
            }
            studio.activeMarkerExchange->cancelled.store(true);
            return ipcReply(true, "Cancellation requested");
        }
        if (args.size() >= 2 && (args[1] == "import" || args[1] == "export")) {
            if (args.size() != 4 || (args[2] != "json" && args[2] != "labels") ||
                !juce::File::isAbsolutePath(args[3])) {
                return ipcReply(false, "Usage: ipc marker import|export json|labels FILE");
            }
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                return ipcReply(false, "Finish the current drag or dialog first.");
            }
            juce::String error;
            return studio.startMarkerExchange(juce::File(args[3]), args[1] == "export",
                                              args[2] == "labels", true, error)
                       ? ipcReply(true, "Marker exchange queued")
                       : ipcReply(false, error);
        }
        if (args.size() >= 2 && args[1] == "find") {
            if (args.size() != 3) {
                return ipcReply(false, "Usage: ipc marker find ID");
            }
            for (size_t i = 0; i < studio.session.markers.size(); ++i) {
                if (studio.session.markers[i].id == args[2]) {
                    return ipcReply(true, static_cast<int>(i));
                }
            }
            return ipcReply(false, "Marker identity not found.");
        }

        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag or dialog first.");
        }
        if (args.size() < 2) {
            return ipcReply(false,
                            "Usage: ipc marker add SAMPLE NAME | ipc marker delete INDEX|id:ID");
        }
        const auto markerReference = [&](const juce::String& text, int& index) {
            if (text.startsWith("id:")) {
                const auto id = text.substring(3);
                for (size_t i = 0; i < studio.session.markers.size(); ++i) {
                    if (studio.session.markers[i].id == id) {
                        index = static_cast<int>(i);
                        return true;
                    }
                }
                return false;
            }
            juce::int64 parsed = 0;
            if (!parseNonNegativeIpcInt(text, parsed) ||
                parsed >= static_cast<juce::int64>(studio.session.markers.size())) {
                return false;
            }
            index = static_cast<int>(parsed);
            return true;
        };

        if (args[1] == "focus") {
            int index = 0;
            if (args.size() != 3 || !markerReference(args[2], index)) {
                return ipcReply(false, "Usage: ipc marker focus INDEX|id:ID");
            }
            studio.focusMarker(index);
            return ipcReply(true, "Marker focused");
        }
        if (args[1] == "focus-adjacent") {
            if (args.size() != 3 || (args[2] != "previous" && args[2] != "next")) {
                return ipcReply(false, "Usage: ipc marker focus-adjacent previous|next");
            }
            return studio.focusAdjacentMarker(args[2] == "next")
                       ? ipcReply(true, studio.selectedMarker)
                       : ipcReply(false, studio.status.getText());
        }
        if (args[1] == "audition-focused" && args.size() == 2) {
            return studio.auditionFocusedMarker()
                       ? ipcReply(true, "Marker boundary audition playing")
                       : ipcReply(false, studio.status.getText());
        }
        if (args[1] == "delete-selection" && args.size() == 2) {
            int removed = 0;
            juce::String error;
            if (!studio.deleteMarkersInSelection(removed, error)) {
                return ipcReply(false, error);
            }
            auto* result = new juce::DynamicObject();
            result->setProperty("removed", removed);
            return ipcReply(true, juce::var(result));
        }
        if (args[1] == "set-positions") {
            if (args.size() < 4 || args.size() % 2 != 0) {
                return ipcReply(false,
                                "Usage: ipc marker set-positions INDEX|id:ID SAMPLE [INDEX|id:ID "
                                "SAMPLE ...]");
            }
            std::vector<int> indices;
            std::vector<juce::int64> samples;
            for (int i = 2; i < args.size(); i += 2) {
                int index = 0;
                juce::int64 sample = 0;
                if (!markerReference(args[i], index) ||
                    !parseNonNegativeIpcInt(args[i + 1], sample)) {
                    return ipcReply(false, "Invalid marker reference or sample.");
                }
                indices.push_back(index);
                samples.push_back(sample);
            }
            juce::String error;
            const bool changed = studio.mutateMarkers(
                [&](carta::Session& candidate) {
                    return carta::setMarkerSamples(candidate, indices, samples, error);
                },
                [] {}, "Move selected markers", error);
            return changed ? ipcReply(true, "Marker positions updated") : ipcReply(false, error);
        }
        if (args[1] == "rename-many") {
            if (args.size() < 4 || args.size() % 2 != 0) {
                return ipcReply(
                    false, "Usage: ipc marker rename-many INDEX|id:ID NAME [INDEX|id:ID NAME ...]");
            }
            std::vector<int> indices;
            std::vector<juce::String> names;
            for (int i = 2; i < args.size(); i += 2) {
                int index = 0;
                if (!markerReference(args[i], index)) {
                    return ipcReply(false, "Invalid marker reference.");
                }
                indices.push_back(index);
                names.push_back(args[i + 1]);
            }
            juce::String error;
            const bool changed = studio.mutateMarkers(
                [&](carta::Session& candidate) {
                    return carta::renameMarkerIndices(candidate, indices, names, error);
                },
                [] {}, "Rename selected markers", error);
            return changed ? ipcReply(true, "Marker names updated") : ipcReply(false, error);
        }
        if (args[1] == "shift-many") {
            juce::int64 delta = 0;
            if (args.size() < 4 || !Studio::parseInteger(args[2], delta)) {
                return ipcReply(
                    false,
                    "Usage: ipc marker shift-many SIGNED_SAMPLES INDEX|id:ID [INDEX|id:ID ...]");
            }
            std::vector<int> indices;
            for (int i = 3; i < args.size(); ++i) {
                int index = 0;
                if (!markerReference(args[i], index)) {
                    return ipcReply(false, "Invalid marker reference.");
                }
                indices.push_back(index);
            }
            juce::String error;
            const bool changed = studio.mutateMarkers(
                [&](carta::Session& candidate) {
                    return carta::shiftMarkerIndices(candidate, indices, delta, error);
                },
                [] {}, "Shift selected markers", error);
            return changed ? ipcReply(true, "Marker positions shifted") : ipcReply(false, error);
        }
        if (args[1] == "from-clips") {
            if (args.size() != 3 || (args[2] != "start" && args[2] != "end")) {
                return ipcReply(false, "Usage: ipc marker from-clips start|end");
            }
            const auto before = studio.session.markers.size();
            if (!studio.addClipBoundaryMarkers(args[2] == "end")) {
                return ipcReply(false, studio.status.getText());
            }
            auto* result = new juce::DynamicObject();
            result->setProperty("added", static_cast<int>(studio.session.markers.size() - before));
            return ipcReply(true, juce::var(result));
        }
        if (args[1] == "nudge" && args.size() == 4) {
            int index = 0;
            juce::String error;
            if (!markerReference(args[2], index) || (args[3] != "-1" && args[3] != "1")) {
                return ipcReply(false, "Usage: ipc marker nudge INDEX|id:ID -1|1");
            }
            return studio.nudgeMarker(index, args[3] == "-1" ? -1 : 1, error)
                       ? ipcReply(true, "Marker nudged")
                       : ipcReply(false, error);
        }

        juce::String error;
        bool changed = false;
        if (args[1] == "set" && args.size() == 5) {
            int markerIndex = 0;
            if (!markerReference(args[2], markerIndex)) {
                return ipcReply(false, "Invalid marker reference.");
            }
            if (args[3] == "sample") {
                juce::int64 sample = 0;
                if (!parseNonNegativeIpcInt(args[4], sample) ||
                    sample >= carta::projectEndSample(studio.session)) {
                    return ipcReply(false, "Marker sample must be within the master duration.");
                }
                if (std::equal_to<double>{}(
                        studio.session.markers[static_cast<size_t>(markerIndex)].position,
                        static_cast<double>(sample) / carta::sessionRate)) {
                    return ipcReply(true, "Unchanged");
                }
                changed = studio.mutateMarkers(
                    [&](carta::Session& candidate) {
                        return carta::setMarkerSample(candidate, markerIndex, sample, error);
                    },
                    [] {}, "Move marker", error);
            } else if (args[3] == "name") {
                const auto name = args[4];
                if (name.trim().isEmpty() || name.containsAnyOf("\r\n")) {
                    return ipcReply(false, "Marker name must be nonempty and single-line.");
                }
                if (studio.session.markers[static_cast<size_t>(markerIndex)].name == name) {
                    return ipcReply(true, "Unchanged");
                }
                changed = studio.mutateMarkers(
                    [&](carta::Session& candidate) {
                        return carta::renameMarkerIndices(candidate, {markerIndex}, {name}, error);
                    },
                    [] {}, "Rename marker", error);
            } else {
                return ipcReply(false, "Usage: ipc marker set INDEX sample SAMPLE | name NAME");
            }
        } else if (args[1] == "add-point" && args.size() == 3) {
            return studio.addMarkerAtEditPoint(args[2]) ? ipcReply(true, "add-point")
                                                        : ipcReply(false, studio.status.getText());
        } else if (args[1] == "add" && args.size() == 4) {
            juce::int64 sample = 0;
            const auto name = args[3];
            if (!parseNonNegativeIpcInt(args[2], sample) || name.trim().isEmpty()) {
                return ipcReply(false, "Expected a nonnegative sample and nonempty name.");
            }
            int index = -1;
            changed = studio.mutateMarkers(
                [&](carta::Session& candidate) {
                    candidate.markers.push_back({0, name});
                    index = static_cast<int>(candidate.markers.size()) - 1;
                    return carta::setMarkerSample(candidate, index, sample, error);
                },
                [&] { studio.selectedMarker = index; }, "Add marker", error);
        } else if ((args[1] == "delete" && args.size() == 3) ||
                   (args[1] == "delete-many" && args.size() >= 3)) {
            std::vector<int> indices;
            indices.reserve(static_cast<size_t>(args.size() - 2));
            for (int i = 2; i < args.size(); ++i) {
                int index = 0;
                if (!markerReference(args[i], index)) {
                    return ipcReply(false, "Invalid marker reference.");
                }
                indices.push_back(index);
            }
            changed = studio.mutateMarkers(
                [&](carta::Session& candidate) {
                    return carta::deleteMarkers(candidate, indices, error);
                },
                [&] {
                    if (studio.selectedMarker < 0) {
                        return;
                    }
                    if (std::find(indices.begin(), indices.end(), studio.selectedMarker) !=
                        indices.end()) {
                        studio.selectedMarker = -1;
                    } else {
                        studio.selectedMarker -= static_cast<int>(
                            std::count_if(indices.begin(), indices.end(), [&](int index) {
                                return index < studio.selectedMarker;
                            }));
                    }
                },
                indices.size() == 1 ? "Delete marker" : "Delete markers", error);
        } else {
            return ipcReply(
                false,
                "Usage: ipc marker add SAMPLE NAME | ipc marker set INDEX|id:ID sample SAMPLE | "
                "name NAME | ipc marker delete INDEX|id:ID");
        }
        return changed ? ipcReply(true, args[1])
                       : ipcReply(false, error.isNotEmpty() ? error : "Marker edit had no effect.");
    }

    static juce::var handleClip(Studio& studio, const juce::StringArray& args) {
        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag or dialog first.");
        }
        if (args.size() >= 2 && args[1] == "envelope-selected") {
            if (args.size() != 5 || (args[2] == "keep" && args[3] == "keep" && args[4] == "keep")) {
                return ipcReply(false,
                                "Usage: ipc clip envelope-selected GAIN|keep FADE_IN_SAMPLES|keep "
                                "FADE_OUT_SAMPLES|keep");
            }
            double gain = -1;
            double fadeIn = -1;
            double fadeOut = -1;
            if (args[2] != "keep" &&
                (!Studio::parseNumber(args[2], gain) || gain < 0 || gain > carta::maxLinearGain)) {
                return ipcReply(false, "Gain must be a finite number from 0 to 16, or keep.");
            }
            const auto parseFade = [&](const juce::String& text, double& fade) {
                juce::int64 samples = 0;
                if (text == "keep") {
                    return true;
                }
                if (!parseNonNegativeIpcInt(text, samples)) {
                    return false;
                }
                fade = static_cast<double>(samples) / carta::sessionRate;
                return true;
            };
            if (!parseFade(args[3], fadeIn) || !parseFade(args[4], fadeOut)) {
                return ipcReply(
                    false, "Fade durations must be nonnegative project-sample counts, or keep.");
            }
            const auto clips = studio.timelineClipSelection.get(studio.editGeneration);
            const bool changed = studio.setSelectedClipEnvelopes(clips, studio.editGeneration, gain,
                                                                 fadeIn, fadeOut);
            const auto message = studio.status.getText();
            return changed                                           ? ipcReply(true, "Updated")
                   : message == "Selected clip envelopes unchanged." ? ipcReply(true, "Unchanged")
                                                                     : ipcReply(false, message);
        }
        if (args.size() == 4 && args[1] == "fade-shape") {
            if (args[2] != "fade-in" && args[2] != "fade-out" && args[2] != "both") {
                return ipcReply(false, "Usage: ipc clip fade-shape fade-in|fade-out|both "
                                       "linear|equal-power|s-curve|exponential");
            }
            carta::FadeShape shape = carta::FadeShape::linear;
            if (!carta::parseFadeShape(args[3].toStdString(), shape)) {
                return ipcReply(false,
                                "Fade shape must be linear, equal-power, s-curve, or exponential.");
            }
            juce::String error;
            return studio.setTargetClipFadeShapes(args[2] != "fade-out", args[2] != "fade-in",
                                                  shape, error)
                       ? ipcReply(true, "Updated")
                       : ipcReply(false, error);
        }
        juce::int64 trackIndex = 0;
        juce::int64 clipIndex = 0;
        if (args.size() != 6 || args[1] != "set" || !parseNonNegativeIpcInt(args[2], trackIndex) ||
            !parseNonNegativeIpcInt(args[3], clipIndex) ||
            trackIndex >= static_cast<juce::int64>(studio.session.tracks.size()) ||
            clipIndex >= static_cast<juce::int64>(
                             studio.session.tracks[static_cast<size_t>(trackIndex)].clips.size())) {
            return ipcReply(
                false,
                "Usage: ipc clip set TRACK CLIP gain VALUE | fade-in|fade-out SAMPLES | "
                "fade-in-shape|fade-out-shape SHAPE | ipc clip fade-shape fade-in|fade-out|both "
                "SHAPE");
        }
        auto& clip = studio.session.tracks[static_cast<size_t>(trackIndex)]
                         .clips[static_cast<size_t>(clipIndex)];
        double value = 0;
        juce::int64 samples = 0;
        const bool gain = args[4] == "gain";
        const bool fadeIn = args[4] == "fade-in";
        const bool fadeOut = args[4] == "fade-out";
        const bool fadeInShape = args[4] == "fade-in-shape";
        const bool fadeOutShape = args[4] == "fade-out-shape";
        carta::FadeShape shape = carta::FadeShape::linear;
        if (gain) {
            if (!Studio::parseNumber(args[5], value) || value < 0 || value > carta::maxLinearGain) {
                return ipcReply(false, "Gain must be a finite number from 0 to 16.");
            }
        } else if (fadeIn || fadeOut) {
            if (!parseNonNegativeIpcInt(args[5], samples)) {
                return ipcReply(false, "Fade duration must be a nonnegative project-sample count.");
            }
            value = static_cast<double>(samples) / carta::sessionRate;
            if (value > clip.duration) {
                return ipcReply(false, "Fade must fit within the clip.");
            }
        } else if (fadeInShape || fadeOutShape) {
            if (!carta::parseFadeShape(args[5].toStdString(), shape)) {
                return ipcReply(false,
                                "Fade shape must be linear, equal-power, s-curve, or exponential.");
            }
        } else {
            return ipcReply(
                false, "Usage: ipc clip set TRACK CLIP gain VALUE | fade-in|fade-out SAMPLES | "
                       "fade-in-shape|fade-out-shape SHAPE");
        }

        // Exact equality avoids discarding small but representable edits.
        if (gain && std::equal_to<float>{}(clip.gain, static_cast<float>(value))) {
            return ipcReply(true, "Unchanged");
        }
        if (fadeIn && std::equal_to<double>{}(clip.fadeIn, value) &&
            std::equal_to<double>{}(clip.fadeInStart, clip.offset)) {
            return ipcReply(true, "Unchanged");
        }
        if (fadeOut && std::equal_to<double>{}(clip.fadeOut, value) &&
            std::equal_to<double>{}(clip.fadeOutEnd, clip.offset + clip.duration)) {
            return ipcReply(true, "Unchanged");
        }
        if (fadeInShape && clip.fadeInShape == shape) {
            return ipcReply(true, "Unchanged");
        }
        if (fadeOutShape && clip.fadeOutShape == shape) {
            return ipcReply(true, "Unchanged");
        }
        juce::String error;
        const bool changed = studio.mutateMixState(
            [&](carta::Session& candidate) {
                auto& target = candidate.tracks[static_cast<size_t>(trackIndex)]
                                   .clips[static_cast<size_t>(clipIndex)];
                if (gain) {
                    target.gain = static_cast<float>(value);
                    return true;
                }
                if (fadeInShape) {
                    target.fadeInShape = shape;
                    return true;
                }
                if (fadeOutShape) {
                    target.fadeOutShape = shape;
                    return true;
                }
                return carta::setClipFade(target, fadeIn, value);
            },
            error,
            gain                            ? "Change clip gain"
            : (fadeInShape || fadeOutShape) ? "Change fade shape"
                                            : "Change clip fade");
        return changed ? ipcReply(true, "Updated")
                       : ipcReply(false,
                                  error.isNotEmpty() ? error : "Could not change clip envelope.");
    }

    static juce::var handleMaster(Studio& studio, const juce::StringArray& args) {
        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag or dialog first.");
        }
        if (args.size() >= 2 && args[1] == "duration") {
            if (args.size() == 2) {
                auto* result = new juce::DynamicObject();
                result->setProperty("minimum_samples", studio.session.minimumEndSample);
                result->setProperty("end_sample", carta::projectEndSample(studio.session));
                return ipcReply(true, juce::var(result));
            }
            juce::int64 end = 0;
            juce::String error;
            if (args.size() != 3 || !parseNonNegativeIpcInt(args[2], end) ||
                end >= 9007199254740992LL) {
                return ipcReply(false, "Usage: ipc master duration [MINIMUM_SAMPLES]");
            }
            return studio.setMinimumMasterDuration(studio.editGeneration, end, error)
                       ? ipcReply(true, "Master duration updated")
                       : ipcReply(false, error);
        }
        if (args.size() != 2 || (args[1] != "mono" && args[1] != "stereo")) {
            return ipcReply(false, "Usage: ipc master mono|stereo | duration [MINIMUM_SAMPLES]");
        }
        const int channels = args[1] == "mono" ? 1 : 2;
        if (studio.masterChannels == channels) {
            return ipcReply(true, "Unchanged");
        }
        juce::String error;
        const bool changed = studio.changeMasterChannels(channels, error);
        if (!changed) {
            return ipcReply(false,
                            error.isNotEmpty() ? error : "Could not change master channels.");
        }
        studio.mode.setSelectedId(studio.masterChannels, juce::dontSendNotification);
        return ipcReply(true, args[1]);
    }

    static juce::var handleTrackOrLayer(Studio& studio, const juce::StringArray& args) {
        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag or dialog first.");
        }
        if (args.size() == 3 && (args[1] == "scale-gain" || args[1] == "scale-gain-db")) {
            double factor = 0;
            if (args[1] == "scale-gain-db"
                    ? !Studio::parseTrackGainDb(args[2], factor, false)
                    : (!Studio::parseNumber(args[2], factor) || factor < 0)) {
                return ipcReply(false,
                                "Gain factor must be nonnegative and finite, or a valid dB value.");
            }
            juce::String error;
            const auto tracks = studio.effectiveMixTracks();
            return studio.scaleTrackGains(studio.editGeneration, tracks, factor, error)
                       ? ipcReply(true, "Selected track gains updated")
                       : ipcReply(false, error);
        }
        if (args.size() >= 2 && args[1] == "set-selected") {
            if (args.size() == 4 && args[2] == "gain-db") {
                double gain = 0;
                bool changed = false;
                juce::String error;
                if (!Studio::parseTrackGainDb(args[3], gain)) {
                    return ipcReply(
                        false,
                        "Gain must be -inf or a finite dB value producing a linear gain from 0 to "
                        "16.");
                }
                return studio.setTrackGains(studio.editGeneration, studio.effectiveMixTracks(),
                                            gain, changed, error)
                           ? ipcReply(true, changed ? "Updated" : "Unchanged")
                           : ipcReply(false, error);
            }
            if (args.size() == 4 && args[2] == "balance") {
                double balance = 0;
                bool changed = false;
                juce::String error;
                if (!Studio::parseNumber(args[3], balance) || balance < -1 || balance > 1) {
                    return ipcReply(false, "Balance must be a finite number from -1 to 1.");
                }
                return studio.setTrackBalances(studio.editGeneration, studio.effectiveMixTracks(),
                                               balance, changed, error)
                           ? ipcReply(true, changed ? "Updated" : "Unchanged")
                           : ipcReply(false, error);
            }
            if (args.size() == 4 && args[2] == "gain") {
                double gain = 0;
                bool changed = false;
                juce::String error;
                if (!Studio::parseNumber(args[3], gain) || gain < 0 ||
                    gain > carta::maxLinearGain) {
                    return ipcReply(false, "Gain must be a finite number from 0 to 16.");
                }
                return studio.setTrackGains(studio.editGeneration, studio.effectiveMixTracks(),
                                            gain, changed, error)
                           ? ipcReply(true, changed ? "Updated" : "Unchanged")
                           : ipcReply(false, error);
            }
            if (args.size() != 4 || (args[2] != "mute" && args[2] != "solo") ||
                (args[3] != "on" && args[3] != "off")) {
                return ipcReply(
                    false,
                    "Usage: ipc track set-selected mute|solo on|off | gain|gain-db|balance VALUE");
            }
            const auto tracks = studio.effectiveMixTracks();
            if (tracks.empty()) {
                return ipcReply(false, "Select a track or time range first.");
            }
            const bool mute = args[2] == "mute";
            const bool value = args[3] == "on";
            const bool unchanged = std::all_of(tracks.begin(), tracks.end(), [&](int i) {
                const auto& track = studio.session.tracks[static_cast<size_t>(i)];
                return (mute ? track.muted : track.solo) == value;
            });
            if (unchanged) {
                return ipcReply(true, "Unchanged");
            }
            const int action = (mute ? 0 : 2) + (value ? 0 : 1);
            return studio.applyTrackMixSelection(studio.editGeneration, tracks, action)
                       ? ipcReply(true, "Updated")
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() >= 2 && args[1] == "stereo") {
            juce::int64 index = 0;
            if (args.size() != 4 || !parseNonNegativeIpcInt(args[2], index) ||
                index >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                (args[3] != "swap" && args[3] != "fold")) {
                return ipcReply(false, "Usage: ipc track stereo INDEX swap|fold");
            }
            juce::String error;
            return studio.editStereoTrackRouting(static_cast<int>(index), args[3] == "fold", error)
                       ? ipcReply(true, "Updated")
                       : ipcReply(false, error);
        }
        if (args.size() >= 2 && args[1] == "channels") {
            juce::int64 index = 0;
            if (args.size() != 4 || !parseNonNegativeIpcInt(args[2], index) ||
                index >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                (args[3] != "split" && args[3] != "combine")) {
                return ipcReply(false, "Usage: ipc track channels INDEX split|combine");
            }
            juce::String error;
            return studio.editStereoChannelTracks(static_cast<int>(index), args[3] == "combine",
                                                  error)
                       ? ipcReply(true, "Updated")
                       : ipcReply(false, error);
        }

        juce::int64 index = 0;
        if (args.size() == 5 && args[1] == "set" && args[3] == "channel-heights" &&
            parseNonNegativeIpcInt(args[2], index) &&
            index < static_cast<juce::int64>(studio.session.tracks.size())) {
            juce::String error;
            return studio.setChannelHeights(static_cast<int>(index), studio.editGeneration, args[4],
                                            error)
                       ? ipcReply(true, "Channel heights updated")
                       : ipcReply(false, error);
        }
        if (args.size() == 5 && args[1] == "set" && parseNonNegativeIpcInt(args[2], index) &&
            index < static_cast<juce::int64>(studio.session.tracks.size()) &&
            (args[3] == "view" || args[3] == "height" || args[3] == "waveform-zoom")) {
            auto& track = studio.session.tracks[static_cast<size_t>(index)];
            juce::String error;
            if (args[3] == "waveform-zoom") {
                juce::int64 value = 0;
                if (args[4] != "default" &&
                    (!parseNonNegativeIpcInt(args[4], value) || value == 0 || value > 8 ||
                     !carta::validWaveformZoom(static_cast<int>(value)))) {
                    return ipcReply(false, "Waveform zoom must be default, 1, 2, 4 or 8.");
                }
                if (track.waveformZoom == value) {
                    return ipcReply(true, "Unchanged");
                }
                const bool changed = studio.mutateMixState(
                    [&](carta::Session& candidate) {
                        candidate.tracks[static_cast<size_t>(index)].waveformZoom =
                            static_cast<int>(value);
                        return true;
                    },
                    error, "Change track waveform zoom");
                return changed ? ipcReply(true, "Updated")
                               : ipcReply(false, error.isNotEmpty()
                                                     ? error
                                                     : "Could not change track waveform zoom.");
            }
            if (args[3] == "view") {
                const auto view = args[4] == "default"       ? carta::TrackView::useDefault
                                  : args[4] == "waveform"    ? carta::TrackView::waveform
                                  : args[4] == "spectrogram" ? carta::TrackView::spectrogram
                                  : args[4] == "combined"    ? carta::TrackView::combined
                                                             : static_cast<carta::TrackView>(-1);
                if (static_cast<int>(view) < 0) {
                    return ipcReply(
                        false, "Track view must be default, waveform, spectrogram or combined.");
                }
                if (track.view == view) {
                    return ipcReply(true, "Unchanged");
                }
                const bool changed = studio.mutateMixState(
                    [&](carta::Session& candidate) {
                        candidate.tracks[static_cast<size_t>(index)].view = view;
                        return true;
                    },
                    error, "Change track view");
                return changed
                           ? ipcReply(true, "Updated")
                           : ipcReply(false,
                                      error.isNotEmpty() ? error : "Could not change track view.");
            }
            juce::int64 height = 0;
            if (!parseNonNegativeIpcInt(args[4], height) || height < 72 || height > 800) {
                return ipcReply(false, "Track height must be from 72 to 800 pixels.");
            }
            if (track.height == height && !track.collapsed) {
                return ipcReply(true, "Unchanged");
            }
            const bool changed = studio.mutateMixState(
                [&](carta::Session& candidate) {
                    auto& target = candidate.tracks[static_cast<size_t>(index)];
                    target.height = static_cast<int>(height);
                    target.collapsed = false;
                    return true;
                },
                error, "Change track height");
            return changed ? ipcReply(true, "Updated")
                           : ipcReply(false, error.isNotEmpty() ? error
                                                                : "Could not change track height.");
        }
        if ((args.size() == 2 || args.size() == 3) && args[1] == "add") {
            const auto name =
                args.size() == 3
                    ? args[2].trim()
                    : ("Layer " + juce::String(static_cast<int>(studio.session.tracks.size() + 1)));
            if (name.isEmpty() || name.containsAnyOf("\r\n")) {
                return ipcReply(false, "Layer name must be nonempty and single-line.");
            }
            juce::String error;
            const bool changed = studio.changeTracks(0, -1, name, error);
            if (!changed) {
                return ipcReply(false, error.isNotEmpty() ? error : "Could not add layer.");
            }
            return ipcReply(true, studio.selectedTrack);
        }
        if (args.size() >= 3 && (args[1] == "rename" || args[1] == "duplicate" ||
                                 args[1] == "remove" || args[1] == "move")) {
            const auto action = args[1];
            if (!parseNonNegativeIpcInt(args[2], index) ||
                index >= static_cast<juce::int64>(studio.session.tracks.size())) {
                return ipcReply(false, "Invalid track index.");
            }
            const int source = static_cast<int>(index);
            juce::int64 destination = 0;
            juce::String name;
            if (action == "rename") {
                if (args.size() != 4 || (name = args[3].trim()).isEmpty() ||
                    name.containsAnyOf("\r\n")) {
                    return ipcReply(false, "Track name must be nonempty and single-line.");
                }
                if (studio.session.tracks[static_cast<size_t>(source)].name == name) {
                    return ipcReply(true, "Unchanged");
                }
            } else if (action == "move") {
                if (args.size() != 4 || !parseNonNegativeIpcInt(args[3], destination) ||
                    destination >= static_cast<juce::int64>(studio.session.tracks.size())) {
                    return ipcReply(false, "Invalid destination track index.");
                }
                if (destination == index) {
                    return ipcReply(true, "Unchanged");
                }
                return studio.reorderCurrentTrack(source, static_cast<int>(destination))
                           ? ipcReply(true, "move")
                           : ipcReply(false, "Could not move track.");
            } else if (args.size() != 3) {
                return ipcReply(false, "Usage: ipc track duplicate|remove INDEX");
            }
            juce::String error;
            if (action != "rename") {
                return studio.changeTracks(action == "duplicate" ? 1 : 2, source, {}, error)
                           ? ipcReply(true, action)
                           : ipcReply(false,
                                      error.isNotEmpty() ? error : "Could not change track.");
            }
            const bool changed = studio.renameTrackTo(source, name, error);
            if (!changed) {
                return ipcReply(false, error.isNotEmpty() ? error : "Could not change track.");
            }
            if (studio.selectedTrack >= 0) {
                studio.revealTrack(studio.selectedTrack);
            }
            return ipcReply(true, action);
        }
        if (args.size() >= 4 && args[1] == "routing") {
            if (!parseNonNegativeIpcInt(args[2], index) ||
                index >= static_cast<juce::int64>(studio.session.tracks.size())) {
                return ipcReply(false, "Invalid track index.");
            }
            auto& track = studio.session.tracks[static_cast<size_t>(index)];
            const int count = carta::maxTrackChannels(track);
            if (args.size() == 4 && args[3] == "clear") {
                if (track.routing.empty()) {
                    return ipcReply(true, "Unchanged");
                }
                juce::String error;
                const bool changed = studio.mutateMixState(
                    [&](carta::Session& candidate) {
                        candidate.tracks[static_cast<size_t>(index)].routing.clear();
                        return true;
                    },
                    error, "Clear track routing");
                return changed ? ipcReply(true, "Cleared")
                               : ipcReply(false,
                                          error.isNotEmpty() ? error : "Could not clear routing.");
            }
            if (count <= 0 || count > 4096 || args.size() != 3 + 2 * count) {
                return ipcReply(
                    false,
                    "Provide one left and right coefficient per source channel (maximum 4096 "
                    "channels).");
            }
            std::vector<carta::StereoRoute> routes;
            routes.reserve(static_cast<size_t>(count));
            for (int channel = 0; channel < count; ++channel) {
                double left = 0;
                double right = 0;
                if (!Studio::parseNumber(args[3 + 2 * channel], left) ||
                    !Studio::parseNumber(args[4 + 2 * channel], right) || left < -2 || left > 2 ||
                    right < -2 || right > 2) {
                    return ipcReply(false,
                                    "Routing coefficients must be finite numbers from -2 to 2.");
                }
                routes.push_back({static_cast<float>(left), static_cast<float>(right)});
            }
            const bool same = track.routing.size() == routes.size() &&
                              std::equal(track.routing.begin(), track.routing.end(), routes.begin(),
                                         [](const auto& a, const auto& b) {
                                             return std::equal_to<float>{}(a.left, b.left) &&
                                                    std::equal_to<float>{}(a.right, b.right);
                                         });
            if (same) {
                return ipcReply(true, "Unchanged");
            }
            juce::String error;
            const bool changed = studio.mutateMixState(
                [&](carta::Session& candidate) {
                    candidate.tracks[static_cast<size_t>(index)].routing = std::move(routes);
                    return true;
                },
                error, "Change track routing");
            return changed
                       ? ipcReply(true, "Updated")
                       : ipcReply(false, error.isNotEmpty() ? error : "Could not change routing.");
        }
        if (args.size() == 5 && args[1] == "set" && parseNonNegativeIpcInt(args[2], index) &&
            index < static_cast<juce::int64>(studio.session.tracks.size()) &&
            (args[3] == "gain" || args[3] == "balance")) {
            double value = 0;
            const bool gain = args[3] == "gain";
            if (!Studio::parseNumber(args[4], value) || value < (gain ? 0.0 : -1.0) ||
                value > (gain ? static_cast<double>(carta::maxLinearGain) : 1.0)) {
                return ipcReply(false, gain ? "Gain must be a finite number from 0 to 16."
                                            : "Balance must be a finite number from -1 to 1.");
            }
            auto& track = studio.session.tracks[static_cast<size_t>(index)];
            float& parameter = gain ? track.gain : track.pan;
            if (std::equal_to<float>{}(parameter, static_cast<float>(value))) {
                return ipcReply(true, "Unchanged");
            }
            juce::String error;
            const bool changed = studio.mutateMixState(
                [&](carta::Session& candidate) {
                    auto& target = candidate.tracks[static_cast<size_t>(index)];
                    (gain ? target.gain : target.pan) = static_cast<float>(value);
                    return true;
                },
                error, gain ? "Change track gain" : "Change track balance");
            return changed ? ipcReply(true, "Updated")
                           : ipcReply(false,
                                      error.isNotEmpty() ? error : "Could not change track mix.");
        }
        if (args.size() != 5 || args[1] != "set" || !parseNonNegativeIpcInt(args[2], index) ||
            index >= static_cast<juce::int64>(studio.session.tracks.size()) ||
            (args[3] != "mute" && args[3] != "solo") || (args[4] != "on" && args[4] != "off")) {
            return ipcReply(false, "Usage: ipc track set INDEX mute|solo on|off");
        }
        const auto flag = args[3] == "mute" ? carta::TrackFlag::mute : carta::TrackFlag::solo;
        const bool value = args[4] == "on";
        const auto& track = studio.session.tracks[static_cast<size_t>(index)];
        if ((flag == carta::TrackFlag::mute ? track.muted : track.solo) == value) {
            return ipcReply(true, "Unchanged");
        }
        juce::String error;
        const bool changed = studio.mutateMixState(
            [&](carta::Session& candidate) {
                return carta::setTrackFlags(candidate, {static_cast<int>(index)}, flag, value);
            },
            error, args[3] == "mute" ? "Change track mute" : "Change track solo");
        return changed
                   ? ipcReply(true, "Updated")
                   : ipcReply(false, error.isNotEmpty() ? error : "Could not change track state.");
    }

    static juce::var handleProject(Studio& studio, const juce::StringArray& args) {
        if (args.size() == 2 && args[1] == "status") {
            if (studio.lastIpcProject.isVoid()) {
                return ipcReply(false, "No IPC project operation has been started.");
            }
            refreshProjectStatus(studio);
            return ipcReply(true, studio.lastIpcProject);
        }
        if (args.size() == 2 && args[1] == "cancel") {
            if (studio.activeOpen && studio.activeOpen->ipc) {
                studio.activeOpen->cancelled.store(true);
            } else if (studio.activeSave && studio.activeSave->ipc) {
                studio.activeSave->cancelled.store(true);
            } else {
                return ipcReply(false, "No IPC project operation is running.");
            }
            refreshProjectStatus(studio);
            return ipcReply(true, "Cancellation requested");
        }
        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag or dialog first.");
        }
        if (studio.activeOpen || studio.activeSave || !studio.pendingImports.empty()) {
            return ipcReply(false, "Wait for loading or saving to finish.");
        }
        if ((args.size() == 2 && args[1] == "save") ||
            (args.size() == 3 && (args[1] == "save-as" || args[1] == "package-as"))) {
            const bool saveAs = args[1] != "save";
            const bool forcePackage = args[1] == "package-as";
            if (!saveAs && studio.projectFile == juce::File{}) {
                return ipcReply(false,
                                "This project has no current file; use ipc project save-as FILE.");
            }
            if (saveAs && !juce::File::isAbsolutePath(args[2])) {
                return ipcReply(false, "Project path must be absolute.");
            }
            const auto file = saveAs ? juce::File(args[2]) : studio.projectFile;
            if (saveAs && (!file.hasFileExtension("carta") || file.exists() ||
                           !file.getParentDirectory().isDirectory())) {
                return ipcReply(false, "Choose a new .carta file in an existing directory.");
            }
            studio.lastIpcProjectHasSnapshot = false;
            auto* info = new juce::DynamicObject();
            info->setProperty("state", "running");
            info->setProperty("operation", forcePackage ? "package-as"
                                           : saveAs     ? "save-as"
                                                        : "save");
            info->setProperty("path", file.getFullPathName());
            info->setProperty("progress", 0.0);
            info->setProperty("cancellation_requested", false);
            info->setProperty("file_written", false);
            studio.lastIpcProject = juce::var(info);
            studio.saveProject({}, saveAs, true, saveAs ? file : juce::File{}, saveAs,
                               forcePackage);
            if (!studio.activeSave) {
                info->setProperty("state", "failed");
                info->setProperty("error", studio.status.getText());
                return ipcReply(false, studio.status.getText());
            }
            studio.lastIpcProjectGeneration = studio.editGeneration;
            studio.lastIpcProjectLoop = studio.activeSave->savedLoop;
            studio.lastIpcProjectHasSnapshot = true;
            info->setProperty("stale", false);
            return ipcReply(true, "Project save queued");
        }
        if (args.size() < 3 || (args[1] != "save-copy" && args[1] != "open") ||
            !juce::File::isAbsolutePath(args[2]) || (args[1] == "save-copy" && args.size() != 3) ||
            (args[1] == "open" && (args.size() - 3) % 2 != 0)) {
            return ipcReply(
                false, "Usage: ipc project save | save-as|package-as|save-copy FILE | open FILE "
                       "[ORIGINAL REPLACEMENT ...]");
        }
        const juce::File file(args[2]);
        juce::String error;
        if (!file.hasFileExtension("carta")) {
            return ipcReply(false, "Project path must end in .carta.");
        }
        if (args[1] == "open") {
            if (studio.dirty) {
                return ipcReply(
                    false, "Save or discard the current edits in the editor before opening another "
                           "project.");
            }
            if (!file.existsAsFile()) {
                return ipcReply(false, "Project file does not exist.");
            }
            if (studio.activeRecovery &&
                carta::pathsReferToSameFile(studio.activeRecovery->file, file)) {
                return ipcReply(false, "Wait for the recovery snapshot to finish.");
            }
            if ((studio.activeExport &&
                 carta::pathsReferToSameFile(studio.activeExport->target, file)) ||
                (studio.activeMarkerExchange && studio.activeMarkerExchange->writing &&
                 carta::pathsReferToSameFile(studio.activeMarkerExchange->file, file))) {
                return ipcReply(false, "Wait for the current output to finish before opening it.");
            }
            carta::SourceRelinks relinks;
            for (int i = 3; i < args.size(); i += 2) {
                if (!juce::File::isAbsolutePath(args[i]) ||
                    !juce::File::isAbsolutePath(args[i + 1])) {
                    return ipcReply(false, "Relink paths must be absolute.");
                }
                const juce::File original(args[i]);
                const juce::File replacement(args[i + 1]);
                if (!replacement.existsAsFile()) {
                    return ipcReply(false, "Replacement audio file does not exist: " + args[i + 1]);
                }
                if ((studio.activeExport &&
                     carta::pathsReferToSameFile(studio.activeExport->target, replacement)) ||
                    (studio.activeRecovery &&
                     carta::pathsReferToSameFile(studio.activeRecovery->file, replacement)) ||
                    (studio.activeMarkerExchange && studio.activeMarkerExchange->writing &&
                     carta::pathsReferToSameFile(studio.activeMarkerExchange->file, replacement))) {
                    return ipcReply(false,
                                    "Wait for the current output to finish before relinking it.");
                }
                if (!relinks.emplace(original.getFullPathName(), replacement).second) {
                    return ipcReply(false, "Duplicate original source path in relinks.");
                }
            }
            studio.lastIpcProjectHasSnapshot = false;
            auto* info = new juce::DynamicObject();
            info->setProperty("state", "running");
            info->setProperty("operation", "open");
            info->setProperty("path", file.getFullPathName());
            info->setProperty("progress", 0.0);
            info->setProperty("cancellation_requested", false);
            studio.lastIpcProject = juce::var(info);
            studio.beginProjectOpen(file, false, true, std::move(relinks));
            return ipcReply(true, "Project open queued");
        }
        if (file.exists() || !file.getParentDirectory().isDirectory()) {
            return ipcReply(false, "Choose a new project path in an existing directory.");
        }
        if (studio.activeExport && carta::pathsReferToSameFile(studio.activeExport->target, file)) {
            return ipcReply(false, "Choose a different file while WAV export is running.");
        }
        if (!studio.validateRetainedMedia(file, error)) {
            return ipcReply(false, error);
        }

        auto task = std::make_shared<SaveTask>();
        task->file = file;
        task->ipc = true;
        task->nativePackage = true;
        int channels = 0;
        {
            const juce::ScopedLock guard(studio.lock);
            task->revision = studio.editHistory.revision();
            task->savedLoop = studio.savedLoop;
            channels = studio.masterChannels;
            studio.copyWorkerSnapshot(task->snapshot);
        }
        task->nativePackage =
            task->nativePackage ||
            std::any_of(task->snapshot.tracks.begin(), task->snapshot.tracks.end(),
                        [](const auto& track) {
                            return std::any_of(track.clips.begin(), track.clips.end(),
                                               [](const auto& clip) {
                                                   return clip.audio && clip.audio->sourceOwner;
                                               });
                        });
        studio.lastIpcProjectGeneration = studio.editGeneration;
        studio.lastIpcProjectLoop = task->savedLoop;
        studio.lastIpcProjectHasSnapshot = true;

        auto* info = new juce::DynamicObject();
        info->setProperty("state", "running");
        info->setProperty("operation", "save-copy");
        info->setProperty("path", file.getFullPathName());
        info->setProperty("progress", 0.0);
        info->setProperty("cancellation_requested", false);
        info->setProperty("file_written", false);
        info->setProperty("stale", false);
        studio.lastIpcProject = juce::var(info);
        studio.activeSave = task;

        studio.savePool.addJob([safe = juce::Component::SafePointer<Studio>(&studio), task,
                                channels] {
            juce::String failure;
            bool ok = false;
            auto directorySync = carta::DirectorySyncStatus::unsupported;
            try {
                const auto report = [task](double p) {
                    task->progress.store(p);
                    return !task->cancelled.load();
                };
                ok = task->nativePackage
                         ? carta::saveNativePackage(task->snapshot, channels, task->file, failure,
                                                    report, &directorySync, task->savedLoop, true,
                                                    false)
                         : carta::saveSessionCopy(task->snapshot, channels, task->file, failure,
                                                  report, &directorySync, task->savedLoop);
            } catch (const std::exception& e) {
                failure = "Project save failed: " + juce::String(e.what());
            } catch (...) {
                failure = "Project save failed.";
            }
            juce::MessageManager::callAsync([safe, task, ok, failure, directorySync] {
                if (safe == nullptr || safe->activeSave != task) {
                    return;
                }
                const bool uncertain =
                    ok && directorySync == carta::DirectorySyncStatus::unconfirmed;
                safe->activeSave.reset();
                if (auto* result = safe->lastIpcProject.getDynamicObject()) {
                    result->setProperty("state", ok                       ? "complete"
                                                 : task->cancelled.load() ? "cancelled"
                                                                          : "failed");
                    result->setProperty("progress",
                                        ok ? 1.0 : juce::jlimit(0.0, 1.0, task->progress.load()));
                    result->setProperty("cancellation_requested", task->cancelled.load());
                    result->setProperty("file_written", ok);
                    if (!ok) {
                        result->setProperty("error", failure);
                    }
                    if (ok) {
                        result->setProperty("directory_sync",
                                            directorySync == carta::DirectorySyncStatus::confirmed
                                                ? "confirmed"
                                            : uncertain ? "unconfirmed"
                                                        : "unsupported");
                        result->setProperty("native_package", task->nativePackage);
                    }
                }
                safe->status.setText(
                    ok ? (uncertain ? "Project copy written, but directory synchronization failed."
                                    : "Saved project copy; current project remains unchanged.")
                       : failure,
                    juce::dontSendNotification);
            });
        });
        return ipcReply(true, "Project copy queued");
    }

    static juce::var handleRender(Studio& studio, const juce::StringArray& args) {
        if (args.size() == 5 && args[1] == "stereo-pair") {
            juce::int64 left = 0;
            juce::int64 right = 0;
            if (!parseNonNegativeIpcInt(args[2], left) || !parseNonNegativeIpcInt(args[3], right) ||
                left >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                right >= static_cast<juce::int64>(studio.session.tracks.size()) ||
                !juce::File::isAbsolutePath(args[4])) {
                return ipcReply(false, "Usage: ipc render stereo-pair LEFT_TRACK RIGHT_TRACK FILE");
            }
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                return ipcReply(false, "Finish the current drag or dialog first.");
            }
            juce::String error;
            if (!studio.startSelectedTrackRender(
                    juce::File(args[4]), true, error, RenderScope::stereoPair,
                    {static_cast<int>(left), static_cast<int>(right)})) {
                return ipcReply(false, error);
            }
            return ipcReply(true, "Stereo pair render queued");
        }
        const bool allowRoutingChange =
            args.size() == 4 && args[1] == "consolidate" && args[3] == "allow-routing-change";
        if ((args.size() != 3 && !allowRoutingChange) ||
            (args[1] != "selected" && args[1] != "tracks" && args[1] != "clips" &&
             args[1] != "consolidate") ||
            !juce::File::isAbsolutePath(args[2])) {
            return ipcReply(
                false, "Usage: ipc render selected|tracks|clips FILE | ipc render consolidate FILE "
                       "[allow-routing-change]");
        }
        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag or dialog first.");
        }
        juce::String error;
        const auto scope = args[1] == "tracks"        ? RenderScope::tracks
                           : args[1] == "clips"       ? RenderScope::clips
                           : args[1] == "consolidate" ? RenderScope::consolidate
                                                      : RenderScope::selectedTime;
        if (!studio.startSelectedTrackRender(juce::File(args[2]), true, error, scope, {},
                                             allowRoutingChange)) {
            return ipcReply(false, error);
        }
        return ipcReply(true, "Track render queued");
    }

    static juce::var handleExport(Studio& studio, const juce::StringArray& args) {
        if (args.size() == 2 && args[1] == "cancel") {
            if (!studio.activeExport || !studio.activeExport->ipc) {
                return ipcReply(false, "No IPC export is running.");
            }
            studio.activeExport->cancelled.store(true);
            return ipcReply(true, "Cancellation requested; check export status for the outcome.");
        }
        const bool trackExport = args.size() >= 4 && args[1] == "track";
        const bool discExport = args.size() == 3 && args[1] == "disc-review";
        const bool batchExport = args.size() >= 3 && args[1] == "tracks";
        const bool rangeExport = args.size() >= 4 && args[1] == "selection" &&
                                 (args[2] == "full" || args[2] == "tracks");
        const bool clipExport = args.size() >= 3 && (args[1] == "clip" || args[1] == "clips");
        const bool crossfadeExport =
            args.size() >= 4 && args[1] == "crossfade" &&
            (args[2] == "selected" || args[2] == "same" || args[2] == "across");
        const bool rippleExport = args.size() >= 4 && args[1] == "close-gap" &&
                                  (args[2] == "focused" || args[2] == "tracks" || args[2] == "all");
        juce::int64 trackIndex = 0;
        if (!((args.size() >= 3 && args[1] == "wav") || discExport ||
              (trackExport && parseNonNegativeIpcInt(args[2], trackIndex)) || rangeExport ||
              clipExport || batchExport || crossfadeExport || rippleExport)) {
            return ipcReply(
                false,
                "Usage: ipc export disc-review FILE | wav|clip|clips FILE | track INDEX FILE | "
                "tracks PARENT | selection full|tracks FILE | crossfade selected|same|across FILE "
                "| close-gap focused|tracks|all FILE [pcm16|pcm24|float32] [dither] [cues] "
                "[allow-clip]");
        }
        const int pathIndex =
            (trackExport || rangeExport || crossfadeExport || rippleExport) ? 3 : 2;
        if (!juce::File::isAbsolutePath(args[pathIndex])) {
            return ipcReply(false, "Export path must be absolute.");
        }

        carta::WavOptions options{carta::WavEncoding::float32, false, false};
        bool formatSet = false;
        bool ditherSet = false;
        bool cuesSet = false;
        bool allowClip = false;
        for (int i = pathIndex + 1; i < args.size(); ++i) {
            if (args[i] == "pcm16" || args[i] == "pcm24" || args[i] == "float32") {
                if (formatSet) {
                    return ipcReply(false, "Specify only one WAV encoding.");
                }
                formatSet = true;
                options.encoding = args[i] == "pcm16"   ? carta::WavEncoding::pcm16
                                   : args[i] == "pcm24" ? carta::WavEncoding::pcm24
                                                        : carta::WavEncoding::float32;
            } else if (args[i] == "dither" && !ditherSet) {
                ditherSet = true;
                options.dither = true;
            } else if (args[i] == "cues" && !cuesSet) {
                cuesSet = true;
                options.embedMarkers = true;
            } else if (args[i] == "allow-clip" && !allowClip) {
                allowClip = true;
            } else {
                return ipcReply(false, "Unknown or duplicate export option: " + args[i]);
            }
        }
        if (options.encoding == carta::WavEncoding::float32 && (options.dither || allowClip)) {
            return ipcReply(false, "Dither and allow-clip require PCM encoding.");
        }
        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current drag or dialog first.");
        }
        if (studio.activeExport || !studio.pendingImports.empty()) {
            return ipcReply(false, "Wait for the active export and imports to finish.");
        }
        const juce::File target(args[pathIndex]);
        juce::String error;
        if (batchExport) {
            if (!target.isDirectory()) {
                return ipcReply(false, "Choose an existing parent folder for the track export.");
            }
        } else {
            if (!target.hasFileExtension("wav")) {
                return ipcReply(false, "Export path must end in .wav.");
            }
            if (target.exists()) {
                return ipcReply(false, "Export destination already exists.");
            }
            if (!target.getParentDirectory().isDirectory()) {
                return ipcReply(false, "Export parent directory does not exist.");
            }
            if (!studio.validateRetainedMedia(target, error)) {
                return ipcReply(false, error);
            }
            if (studio.activeSave && carta::pathsReferToSameFile(studio.activeSave->file, target)) {
                return ipcReply(false, "Choose an output other than the project being saved.");
            }
        }

        auto task = std::make_shared<ExportTask>();
        task->target = target;
        task->ipc = true;
        task->noReplace = true;
        task->wavOptions = options;
        task->allowClip = allowClip;
        task->batch = batchExport;
        task->discProgram = discExport;
#if CARTA_STUDIO_COMPONENT_TEST
        task->writeProgressHook = studio.exportWriteProgressHook;
#endif
        carta::WavRange range;
        int channels = 0;
        juce::File protectedProject;
        {
            const juce::ScopedLock guard(studio.lock);
            channels = studio.masterChannels;
            protectedProject = studio.projectFile;
            task->snapshotGeneration = studio.editGeneration;
            studio.copyWorkerSnapshot(task->snapshot);
        }
        range = carta::fullWavRange(task->snapshot);
        if (discExport && !carta::validateDiscProgramWav(task->snapshot, channels, range, error)) {
            return ipcReply(false, error);
        }
        const auto protectedMedia = task->snapshot;
        if (clipExport) {
            if (args[1] == "clip") {
                if (!carta::isolateClip(task->snapshot, studio.selectedTrack, studio.selectedClip,
                                        range.start, range.end)) {
                    return ipcReply(false,
                                    "Select a clip containing project samples before exporting.");
                }
            } else {
                const auto clips = studio.timelineClipSelection.get(studio.editGeneration);
                if (!carta::isolateClipGroup(task->snapshot, clips, range.start, range.end,
                                             error)) {
                    return ipcReply(false, error);
                }
            }
        }
        if (rangeExport) {
            if (!studio.timeSelection.hasRange()) {
                return ipcReply(false, "Select a time range before exporting.");
            }
            range = {studio.timeSelection.start, studio.timeSelection.end};
            if (args[2] == "tracks") {
                const auto independent = studio.selectedTimelineTracks();
                if (!(independent.empty()
                          ? carta::limitToTracks(task->snapshot, studio.timeSelection.firstTrack,
                                                 studio.timeSelection.lastTrack)
                          : carta::limitToTrackIndices(task->snapshot, independent))) {
                    return ipcReply(false, "Selected tracks are no longer valid.");
                }
            }
        }
        if (crossfadeExport) {
            carta::PlaybackRange previewRange;
            const auto clips = studio.timelineClipSelection.get(studio.editGeneration);
            const bool ok =
                args[2] == "selected"
                    ? (clips.size() == 2
                           ? carta::prepareSelectedCrossfadePreview(
                                 task->snapshot, clips[0], clips[1], studio.auditionBefore,
                                 studio.auditionAfter, task->snapshot, previewRange, error)
                           : false)
                    : carta::prepareCrossfadePreview(task->snapshot, studio.selectedTrack,
                                                     studio.selectedClip, args[2] == "across",
                                                     studio.auditionBefore, studio.auditionAfter,
                                                     task->snapshot, previewRange, error);
            if (!ok) {
                return ipcReply(false,
                                error.isNotEmpty()
                                    ? error
                                    : "Select exactly two clips to preview their crossfade.");
            }
            range = {
                static_cast<juce::int64>(std::llround(previewRange.start * carta::sessionRate)),
                static_cast<juce::int64>(std::llround(previewRange.end * carta::sessionRate))};
        }
        if (rippleExport) {
            if (!studio.timeSelection.hasRange()) {
                return ipcReply(false, "Select a time range before previewing gap closure.");
            }
            auto selection = studio.timeSelection;
            const bool focused = args[2] == "focused";
            const bool all = args[2] == "all";
            const auto independent = studio.selectedTimelineTracks();
            if (focused) {
                if (studio.selectedTrack < 0 ||
                    studio.selectedTrack >= static_cast<int>(task->snapshot.tracks.size())) {
                    return ipcReply(false, "Focus a track first.");
                }
                selection.firstTrack = selection.lastTrack = studio.selectedTrack;
            }
            carta::PlaybackRange previewRange;
            const bool ok = !all && !focused && !independent.empty()
                                ? carta::prepareIndependentRipplePreview(
                                      task->snapshot, selection, independent, studio.auditionBefore,
                                      studio.auditionAfter, task->snapshot, previewRange, error)
                                : carta::prepareRipplePreview(
                                      task->snapshot, selection, all, studio.auditionBefore,
                                      studio.auditionAfter, task->snapshot, previewRange, error);
            if (!ok) {
                return ipcReply(false,
                                error.isNotEmpty() ? error : "Could not preview gap closure.");
            }
            range = {
                static_cast<juce::int64>(std::llround(previewRange.start * carta::sessionRate)),
                static_cast<juce::int64>(std::llround(previewRange.end * carta::sessionRate))};
        }
        if (trackExport) {
            std::vector<carta::MasterTrack> tracks;
            if (!carta::getMasterTracks(task->snapshot, tracks, error)) {
                return ipcReply(false, error);
            }
            if (trackIndex >= static_cast<juce::int64>(tracks.size())) {
                return ipcReply(false, "Invalid master track index.");
            }
            range = tracks[static_cast<size_t>(trackIndex)].range;
        }
        if (batchExport) {
            if (!carta::getMasterTracks(task->snapshot, task->tracks, error)) {
                return ipcReply(false, error);
            }
            for (const auto& track : task->tracks) {
                if (!carta::validateWavRange(task->snapshot, channels, track.range, error,
                                             task->wavOptions)) {
                    return ipcReply(false, error);
                }
            }
        } else if (!carta::validateWavRange(task->snapshot, channels, range, error,
                                            task->wavOptions) ||
                   !carta::validateOutputDestination(protectedMedia, target, protectedProject,
                                                     error)) {
            return ipcReply(false, error);
        }

        auto* info = new juce::DynamicObject();
        info->setProperty("state", "running");
        info->setProperty("phase", "preflight");
        info->setProperty("progress", 0.0);
        info->setProperty("cancellation_requested", false);
        info->setProperty("file_written", false);
        info->setProperty("encoding", options.encoding == carta::WavEncoding::pcm16   ? "pcm16"
                                      : options.encoding == carta::WavEncoding::pcm24 ? "pcm24"
                                                                                      : "float32");
        info->setProperty("dither", options.dither);
        info->setProperty("cues", options.embedMarkers);
        info->setProperty("path", target.getFullPathName());
        info->setProperty("start_sample", range.start);
        info->setProperty("end_sample", range.end);
        studio.lastIpcExport = juce::var(info);
        studio.lastIpcExportGeneration = task->snapshotGeneration;
        studio.lastIpcExportHasGeneration = true;
        info->setProperty("stale", false);
        if (discExport) {
            info->setProperty("kind", "disc-program-review");
            info->setProperty("cues", true);
        }
        if (crossfadeExport || rippleExport) {
            info->setProperty("kind", crossfadeExport ? "crossfade-preview" : "close-gap-preview");
            info->setProperty("scope", args[2]);
        }
        if (batchExport) {
            info->setProperty("expected_tracks", static_cast<int>(task->tracks.size()));
            info->setProperty("completed_tracks", 0);
            info->setProperty("manifest_written", false);
        }
        studio.activeExport = task;
        if (discExport) {
            studio.writeExport(task, channels, range, protectedProject);
        } else {
            studio.beginExport(task, channels, range, protectedProject);
        }
        return ipcReply(true, "Export queued");
    }

    static juce::var handleImports(Studio& studio, const juce::StringArray& args) {
        if (args.size() != 2 ||
            (args[1] != "status" && args[1] != "cancel" && args[1] != "clear-issues")) {
            return ipcReply(false, "Usage: ipc imports status|cancel|clear-issues");
        }
        if (args[1] == "cancel") {
            int requested = 0;
            for (const auto& task : studio.pendingImports) {
                if (!task->cancelled.exchange(true)) {
                    ++requested;
                }
            }
            if (requested > 0) {
                studio.status.setText(utf8Str("Cancelling pending imports…"),
                                      juce::dontSendNotification);
            }
            auto* result = new juce::DynamicObject();
            result->setProperty("requested", requested);
            return ipcReply(true, juce::var(result));
        }
        if (args[1] == "clear-issues") {
            studio.clearImportIssues();
            return ipcReply(true, "Import issues cleared");
        }
        auto* result = new juce::DynamicObject();
        juce::Array<juce::var> pending;
        juce::Array<juce::var> issues;
        for (const auto& task : studio.pendingImports) {
            auto* entry = new juce::DynamicObject();
            entry->setProperty("path", task->file.getFullPathName());
            entry->setProperty("start_sample", task->startSample);
            entry->setProperty("progress", juce::jlimit(0.0, 1.0, task->progress.load()));
            entry->setProperty("ready", task->ready);
            entry->setProperty("cancellation_requested", task->cancelled.load());
            pending.add(juce::var(entry));
        }
        for (const auto& issue : studio.importIssues) {
            auto* entry = new juce::DynamicObject();
            entry->setProperty("path", issue.first);
            entry->setProperty("reason", issue.second);
            issues.add(juce::var(entry));
        }
        result->setProperty("pending", juce::var(pending));
        result->setProperty("issues", juce::var(issues));
        result->setProperty("omitted", studio.omittedImportIssues);
        return ipcReply(true, juce::var(result));
    }

    static juce::var handleImport(Studio& studio, const juce::StringArray& args) {
        if (juce::Component::getCurrentlyModalComponent() != nullptr) {
            return ipcReply(false, "Finish the current dialog before importing audio.");
        }
        bool forceNewLayer = false;
        bool hasLayer = false;
        bool hasAt = false;
        bool sawNoMarker = false;
        bool sawMarker = false;
        bool endOfOptions = false;
        int explicitLayer = -1;
        juce::int64 explicitStart = -1;
        juce::StringArray files;
        for (int i = 1; i < args.size(); ++i) {
            const auto& arg = args[i];
            if (!endOfOptions && arg == "--") {
                endOfOptions = true;
                continue;
            }
            if (!endOfOptions && arg == "--new-layer") {
                if (hasLayer) {
                    return ipcReply(false, "Specify either --layer or --new-layer, not both.");
                }
                forceNewLayer = true;
                continue;
            }
            if (!endOfOptions && arg == "--layer") {
                if (i + 1 >= args.size()) {
                    return ipcReply(false,
                                    "Usage: ipc import [--layer INDEX | --new-layer] [--at SAMPLE] "
                                    "[--marker | --no-marker] FILE [...]");
                }
                if (forceNewLayer) {
                    return ipcReply(false, "Specify either --layer or --new-layer, not both.");
                }
                juce::int64 parsed = -1;
                if (!Studio::parseInteger(args[++i], parsed) || parsed < 0 ||
                    parsed >= static_cast<juce::int64>(studio.session.tracks.size())) {
                    return ipcReply(false, "Layer index out of range.");
                }
                hasLayer = true;
                explicitLayer = static_cast<int>(parsed);
                continue;
            }
            if (!endOfOptions && arg == "--at") {
                if (i + 1 >= args.size()) {
                    return ipcReply(false,
                                    "Usage: ipc import [--layer INDEX | --new-layer] [--at SAMPLE] "
                                    "[--marker | --no-marker] FILE [...]");
                }
                if (!Studio::parseInteger(args[++i], explicitStart) || explicitStart < 0 ||
                    explicitStart >= 9007199254740992LL) {
                    return ipcReply(false, "Start sample must be a nonnegative integer.");
                }
                hasAt = true;
                continue;
            }
            if (!endOfOptions && arg == "--no-marker") {
                if (sawMarker) {
                    return ipcReply(false, "Specify either --marker or --no-marker, not both.");
                }
                sawNoMarker = true;
                continue;
            }
            if (!endOfOptions && arg == "--marker") {
                if (sawNoMarker) {
                    return ipcReply(false, "Specify either --marker or --no-marker, not both.");
                }
                sawMarker = true;
                continue;
            }
            if (!endOfOptions && arg.startsWith("--")) {
                return ipcReply(false, "Unknown import option: " + arg);
            }
            files.add(arg);
        }
        if (files.isEmpty()) {
            return ipcReply(false, "Usage: ipc import [--layer INDEX | --new-layer] [--at SAMPLE] "
                                   "[--marker | --no-marker] FILE [...]");
        }
        for (const auto& path : files) {
            if (!juce::File::isAbsolutePath(path) || !juce::File(path).existsAsFile()) {
                return ipcReply(false, "Audio file does not exist: " + path);
            }
        }
        const int targetTrack = hasLayer ? explicitLayer : -2;
        const int markerPolicy = sawMarker ? 1 : sawNoMarker ? 0 : -1;
        const juce::uint64 batch =
            (!forceNewLayer && hasAt && files.size() > 1) ? studio.nextImportBatchId++ : 0;
        int queued = 0;
        for (const auto& path : files) {
            if (studio.importFile(juce::File(path), hasAt ? explicitStart : -1, targetTrack,
                                  markerPolicy, forceNewLayer, batch)) {
                ++queued;
            }
        }
        if (queued != files.size()) {
            return ipcReply(false, "Only " + juce::String(queued) +
                                       " imports were queued; check the editor's import issues.");
        }
        auto* o = new juce::DynamicObject();
        o->setProperty("queued", queued);
        return ipcReply(true, juce::var(o));
    }

    static std::optional<juce::var> handleTransport(Studio& studio, const juce::StringArray& args) {
        if (args.size() >= 2 && args[1] == "context") {
            if (args.size() == 2) {
                auto* result = new juce::DynamicObject();
                result->setProperty("before_samples", studio.auditionBefore);
                result->setProperty("after_samples", studio.auditionAfter);
                return ipcReply(true, juce::var(result));
            }
            juce::int64 before = 0;
            juce::int64 after = 0;
            juce::String error;
            if (args.size() != 4 || !parseNonNegativeIpcInt(args[2], before) ||
                !parseNonNegativeIpcInt(args[3], after)) {
                return ipcReply(false,
                                "Usage: ipc transport context [BEFORE_SAMPLES AFTER_SAMPLES]");
            }
            if (!studio.setAuditionContext(before, after, error)) {
                return ipcReply(false, error);
            }
            return ipcReply(true, "Audition context updated");
        }
        if (args.size() == 3 && args[1] == "audition-close-gap") {
            if (args[2] != "focused" && args[2] != "tracks" && args[2] != "all") {
                return ipcReply(false,
                                "Usage: ipc transport audition-close-gap focused|tracks|all");
            }
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr || studio.activeOpen) {
                return ipcReply(false, "Finish the current drag, dialog or project opening first.");
            }
            return studio.startRippleAudition(args[2] == "focused"  ? 0
                                              : args[2] == "tracks" ? 1
                                                                    : 2,
                                              studio.editGeneration, studio.timeSelection,
                                              studio.selectedTimelineTracks())
                       ? ipcReply(true, "Gap-closure preview playing")
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 3 && args[1] == "audition-crossfade") {
            if (args[2] != "selected" && args[2] != "same" && args[2] != "across") {
                return ipcReply(false,
                                "Usage: ipc transport audition-crossfade selected|same|across");
            }
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr || studio.activeOpen) {
                return ipcReply(false, "Finish the current drag, dialog or project opening first.");
            }
            return studio.startCrossfadeAudition(args[2] == "selected" ? 2
                                                 : args[2] == "across" ? 1
                                                                       : 0)
                       ? ipcReply(true, "Crossfade preview playing")
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 3 && args[1] == "audition-clips") {
            if (args[2] != "once" && args[2] != "loop") {
                return ipcReply(false, "Usage: ipc transport audition-clips once|loop");
            }
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr || studio.activeOpen) {
                return ipcReply(false, "Finish the current drag, dialog or project opening first.");
            }
            return studio.auditionClipGroup(args[2] == "loop")
                       ? ipcReply(true, "Selected clips audition playing")
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 3 && args[1] == "selection-context") {
            if (args[2] != "full" && args[2] != "tracks") {
                return ipcReply(false, "Usage: ipc transport selection-context full|tracks");
            }
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                return ipcReply(false, "Finish the current drag or dialog first.");
            }
            if (!studio.requireAudioOutput()) {
                return ipcReply(false, studio.status.getText());
            }
            return studio.startSelectionContextAudition(args[2] == "tracks")
                       ? ipcReply(true, "Playing selection with context")
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 4 && args[1] == "audition-selection") {
            if ((args[2] != "full" && args[2] != "tracks") ||
                (args[3] != "once" && args[3] != "loop")) {
                return ipcReply(false,
                                "Usage: ipc transport audition-selection full|tracks once|loop");
            }
            if (studio.dragMode != kDragNone ||
                juce::Component::getCurrentlyModalComponent() != nullptr || studio.activeOpen) {
                return ipcReply(false, "Finish the current drag, dialog or project opening first.");
            }
            if (!studio.requireAudioOutput()) {
                return ipcReply(false, studio.status.getText());
            }
            return studio.startSelectionAudition(args[2] == "tracks", args[3] == "loop", false)
                       ? ipcReply(true, "Selection audition playing")
                       : ipcReply(false, studio.status.getText());
        }
        if (args.size() == 2) {
            if (args[1] == "play") {
                if (!studio.playing) {
                    const auto error = studio.audioOutputError();
                    if (error.isNotEmpty()) {
                        return ipcReply(false, error);
                    }
                    studio.play.onClick();
                }
            } else if (args[1] == "pause") {
                {
                    const juce::ScopedLock guard(studio.lock);
                    studio.playing = false;
                }
                const juce::String message = "Paused.";
                studio.status.setText(message, juce::dontSendNotification);
                studio.announceNavigation(message);
                studio.repaint();
            } else if (args[1] == "stop") {
                studio.stop.onClick();
            } else if (args[1] == "stop-here") {
                studio.stopAtPlayhead();
            } else if (args[1] == "reset-interruptions") {
                studio.resetEditLockInterruptions();
            } else if (args[1] == "audition-point") {
                if (!studio.auditionEditPoint()) {
                    return ipcReply(false, studio.status.getText());
                }
            } else {
                return ipcReply(false,
                                "Usage: ipc transport "
                                "play|pause|stop|stop-here|reset-interruptions|audition-point");
            }
            return ipcReply(true, args[1]);
        }
        return std::nullopt;
    }

    static juce::var handleMasterTrack(Studio& studio, const juce::StringArray& args) {
        if (studio.dragMode != kDragNone ||
            juce::Component::getCurrentlyModalComponent() != nullptr || studio.activeOpen) {
            return ipcReply(false, "Finish the current drag, dialog or project opening first.");
        }
        juce::int64 index = 0;
        juce::String error;
        bool changed = false;
        if (args.size() != 4 || !parseNonNegativeIpcInt(args[2], index) ||
            index > std::numeric_limits<int>::max()) {
            return ipcReply(false,
                            "Usage: ipc master-track rename INDEX NAME | select INDEX all|tracks");
        }
        if (args[1] == "rename") {
            if (!studio.renameMasterTrack(static_cast<int>(index), args[3], studio.editGeneration,
                                          changed, error)) {
                return ipcReply(false, error);
            }
            return ipcReply(true, changed ? "Master track renamed" : "Unchanged");
        }
        if (args[1] == "select" && (args[3] == "all" || args[3] == "tracks")) {
            return studio.selectMasterTrackIndex(static_cast<int>(index), args[3] == "tracks",
                                                 studio.editGeneration, error)
                       ? ipcReply(true, "Master track time selected")
                       : ipcReply(false, error);
        }
        return ipcReply(false,
                        "Usage: ipc master-track rename INDEX NAME | select INDEX all|tracks");
    }

    static juce::var handleApp(Studio& studio, const juce::StringArray& args) {
        if ((args.size() == 2 && args[1] == "quit") ||
            (args.size() == 3 && args[1] == "quit" && args[2] == "force")) {
            const bool force = args.size() == 3;
            if (!force && studio.hasUnsavedChanges()) {
                return ipcReply(false,
                                "Unsaved changes present; save first or run 'ipc app quit force'.");
            }
            studio.cleanExitApproved = true;
#if !CARTA_STUDIO_COMPONENT_TEST
            if (auto* app = juce::JUCEApplicationBase::getInstance()) {
                juce::MessageManager::callAsync([app] { app->quit(); });
            }
#endif
            return ipcReply(true, "Quitting Carta Studio");
        }
        return ipcReply(false, "Usage: ipc app quit [force]");
    }

    static juce::var handleUtoc(Studio& studio, const juce::StringArray& args) {
        if (args.size() == 2 && (args[1] == "budget" || args[1] == "inspect")) {
            const auto& disc = studio.session.mdDisc;
            carta::UtocDiscState utoc;
            std::string reason;
            if (!carta::compileMdDiscToUtoc(disc, utoc, reason)) {
                return ipcReply(false, juce::String::fromUTF8(reason.c_str()));
            }
            const auto medium = carta::mdMediumToString(disc.medium);
            const auto budget = carta::computeUtocBudget(utoc, medium);
            auto* b = buildUtocBudgetObject(budget);
            if (args[1] == "budget") {
                return ipcReply(true, juce::var(b));
            }
            std::array<std::uint8_t, carta::kUtocSectorBytes> s0{};
            std::array<std::uint8_t, carta::kUtocSectorBytes> s1{};
            std::array<std::uint8_t, carta::kUtocSectorBytes> s2{};
            std::array<std::uint8_t, carta::kUtocSectorBytes> s4{};
            if (!carta::compileUtocSectors(utoc, s0, s1, s2, s4, reason)) {
                return ipcReply(false, juce::String::fromUTF8(reason.c_str()));
            }
            auto* res = new juce::DynamicObject();
            res->setProperty("sector_bytes", static_cast<int>(carta::kUtocSectorBytes));
            res->setProperty("tracks", static_cast<int>(utoc.trackCount));
            res->setProperty("has_sector0", utoc.hasSector0);
            res->setProperty("has_sector1", utoc.hasSector1);
            res->setProperty("has_sector2", utoc.hasSector2);
            res->setProperty("has_sector4", utoc.hasSector4);
            res->setProperty("disc_timestamp", juce::String::fromUTF8(disc.discTimestamp.c_str()));
            res->setProperty("budget", juce::var(b));
            res->setProperty(
                "report",
                juce::String::fromUTF8(carta::formatUtocInspectionReport(utoc, medium).c_str()));
            return ipcReply(true, juce::var(res));
        }
        if (args.size() == 3 && args[1] == "export") {
            if (studio.dragMode != kDragNone || studio.activeOpen ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                return ipcReply(false, "Finish the current drag, dialog or opening first.");
            }
            const juce::File file(args[2]);
            juce::String error;
            if (!juce::File::isAbsolutePath(args[2]) || !file.getParentDirectory().isDirectory() ||
                file.exists()) {
                return ipcReply(false, "Choose a new absolute file path in an existing directory.");
            }
            if ((studio.activeSave && carta::pathsReferToSameFile(studio.activeSave->file, file)) ||
                (studio.activeExport &&
                 carta::pathsReferToSameFile(studio.activeExport->target, file)) ||
                !studio.validateRetainedMedia(file, error) ||
                !carta::validateOutputDestination(studio.session, file, studio.projectFile,
                                                  error)) {
                return ipcReply(false, error.isNotEmpty()
                                           ? error
                                           : "Choose a different path from the current output.");
            }
            carta::UtocDiscState utoc;
            std::string reason;
            if (!carta::compileMdDiscToUtoc(studio.session.mdDisc, utoc, reason)) {
                return ipcReply(false, juce::String::fromUTF8(reason.c_str()));
            }
            std::array<std::uint8_t, carta::kUtocSectorBytes> s0{};
            std::array<std::uint8_t, carta::kUtocSectorBytes> s1{};
            std::array<std::uint8_t, carta::kUtocSectorBytes> s2{};
            std::array<std::uint8_t, carta::kUtocSectorBytes> s4{};
            if (!carta::compileUtocSectors(utoc, s0, s1, s2, s4, reason)) {
                return ipcReply(false, juce::String::fromUTF8(reason.c_str()));
            }
            juce::MemoryBlock bundle;
            bundle.append(s0.data(), s0.size());
            bundle.append(s1.data(), s1.size());
            bundle.append(s2.data(), s2.size());
            bundle.append(s4.data(), s4.size());
            if (!file.replaceWithData(bundle.getData(), bundle.getSize())) {
                return ipcReply(false, "Could not write UTOC bundle file.");
            }
            auto* res = new juce::DynamicObject();
            res->setProperty("path", file.getFullPathName());
            res->setProperty("bytes", static_cast<juce::int64>(bundle.getSize()));
            res->setProperty("sectors", 4);
            return ipcReply(true, juce::var(res));
        }
        if (args.size() == 3 && args[1] == "import") {
            if (studio.dragMode != kDragNone || studio.activeOpen ||
                juce::Component::getCurrentlyModalComponent() != nullptr) {
                return ipcReply(false, "Finish the current drag, dialog or opening first.");
            }
            const juce::File file(args[2]);
            if (!juce::File::isAbsolutePath(args[2]) || !file.existsAsFile()) {
                return ipcReply(false, "Choose an existing absolute UTOC binary file.");
            }
            juce::MemoryBlock data;
            if (!file.loadFileAsData(data)) {
                return ipcReply(false, "Could not read UTOC binary file.");
            }
            const size_t sectorBytes = carta::kUtocSectorBytes;
            const auto* bytes = static_cast<const std::uint8_t*>(data.getData());
            const std::uint8_t* s0 = nullptr;
            const std::uint8_t* s1 = nullptr;
            const std::uint8_t* s2 = nullptr;
            const std::uint8_t* s4 = nullptr;
            if (data.getSize() == sectorBytes) {
                s0 = bytes;
            } else if (data.getSize() == sectorBytes * 4) {
                s0 = bytes;
                s1 = bytes + sectorBytes;
                s2 = bytes + sectorBytes * 2;
                s4 = bytes + sectorBytes * 3;
            } else if (data.getSize() == sectorBytes * 5) {
                s0 = bytes;
                s1 = bytes + sectorBytes;
                s2 = bytes + sectorBytes * 2;
                s4 = bytes + sectorBytes * 4;
            } else {
                return ipcReply(
                    false,
                    "UTOC file size must be 2352 (Sector 0), 9408 (Sectors 0,1,2,4), or 11760 "
                    "(Sectors 0..4) bytes.");
            }
            carta::UtocDiscState utoc;
            std::string reason;
            if (!carta::parseUtocSectors(s0, s1, s2, s4, utoc, reason)) {
                return ipcReply(false, juce::String::fromUTF8(reason.c_str()));
            }
            carta::MdDisc imported;
            if (!carta::mdDiscFromUtoc(utoc, imported, reason)) {
                return ipcReply(false, juce::String::fromUTF8(reason.c_str()));
            }
            if (s0) {
                imported.importedUtocSectors[0].assign(s0, s0 + sectorBytes);
            }
            if (s1) {
                imported.importedUtocSectors[1].assign(s1, s1 + sectorBytes);
            }
            if (s2) {
                imported.importedUtocSectors[2].assign(s2, s2 + sectorBytes);
            }
            if (s4) {
                imported.importedUtocSectors[3].assign(s4, s4 + sectorBytes);
            }
            juce::String error;
            const bool changed = studio.mutateMixState(
                [&](carta::Session& candidate) {
                    candidate.mdDisc = imported;
                    return true;
                },
                error, "Import UTOC sectors");
            if (!changed) {
                return ipcReply(false, error);
            }
            studio.refreshDiscWorkspace(true);
            auto* res = new juce::DynamicObject();
            res->setProperty("title", juce::String::fromUTF8(imported.halfWidthTitle.c_str()));
            res->setProperty("tracks", static_cast<int>(imported.tracks.size()));
            res->setProperty("groups", static_cast<int>(imported.groups.size()));
            return ipcReply(true, juce::var(res));
        }
        return ipcReply(false, "Usage: ipc utoc budget|inspect | export FILE | import FILE");
    }

    static juce::var handleNetMd(Studio& studio, const juce::StringArray& args) {
        if (args.size() < 2) {
            return ipcReply(false,
                            "Usage: netmd devices|status|preflight|load-disc|read-utoc|erase|"
                            "write-utoc|record ...");
        }
        const auto sub = args[1];
        if (sub == "devices") {
            if (args.size() != 2) {
                return ipcReply(false, "Usage: netmd devices");
            }
            std::vector<carta::NetMdDeviceEntry> devs;
            std::string devErr;
            if (!carta::listNetMdDevices(devs, devErr)) {
                return ipcReply(false, juce::String::fromUTF8(devErr.c_str()));
            }
            studio.cachedNetMdDevices = devs;
            studio.netMdScannedOnce = true;
            juce::Array<juce::var> list;
            for (const auto& d : devs) {
                auto* o = new juce::DynamicObject();
                o->setProperty("id", juce::String::fromUTF8(d.id.c_str()));
                o->setProperty("name", juce::String::fromUTF8(d.name.c_str()));
                o->setProperty("vendor_id", d.vendorId);
                o->setProperty("product_id", d.productId);
                o->setProperty("bus", d.bus);
                o->setProperty("address", d.address);
                list.add(juce::var(o));
            }
            auto* root = new juce::DynamicObject();
            root->setProperty("devices", list);
            return ipcReply(true, juce::var(root));
        }
        if (sub == "status") {
            if (args.size() != 2) {
                return ipcReply(false, "Usage: netmd status");
            }
            return ipcReply(true, studio.buildNetMdStatusJson());
        }
        if (sub == "preflight") {
            if (args.size() > 3) {
                return ipcReply(false, "Usage: netmd preflight [DEVICE_ID]");
            }
            if (studio.netMdBusy.load()) {
                return ipcReply(false, "Wait for the active NetMD hardware operation to finish.");
            }
            std::string devErr;
            const std::string devId = studio.resolveActiveNetMdDeviceId(
                args.size() == 3 ? args[2] : juce::String(), devErr);
            carta::Session snapshot;
            {
                const juce::ScopedLock guard(studio.lock);
                studio.copyWorkerSnapshot(snapshot);
            }
            const auto report = carta::preflightNetMdRecording(snapshot, snapshot.mdDisc, devId);
            studio.cachedNetMdPreflight = report;
            studio.refreshRecordWorkspace(false);

            auto* budgetObj = new juce::DynamicObject();
            budgetObj->setProperty("used_clusters", report.budget.usedClusters);
            budgetObj->setProperty("total_clusters", report.budget.totalClusters);
            budgetObj->setProperty("used_tracks", report.budget.usedTracks);
            budgetObj->setProperty("max_tracks", report.budget.maxTracks);
            budgetObj->setProperty("used_fragments", report.budget.usedFragments);
            budgetObj->setProperty("max_fragments", report.budget.maxFragments);
            budgetObj->setProperty("shared_fragments", report.budget.sharedFragmentCount);
            budgetObj->setProperty("used_hw_cells", report.budget.usedHalfWidthCells);
            budgetObj->setProperty("used_fw_cells", report.budget.usedFullWidthCells);
            budgetObj->setProperty("fits", report.budget.fits());

            auto* devObj = new juce::DynamicObject();
            devObj->setProperty("id", juce::String::fromUTF8(report.device.deviceId.c_str()));
            devObj->setProperty("name", juce::String::fromUTF8(report.device.deviceName.c_str()));
            devObj->setProperty("version_code",
                                juce::String::fromUTF8(report.device.versionCode.c_str()));
            devObj->setProperty("supports_sp_upload", report.device.supportsSpUpload);
            devObj->setProperty("supports_force_toc_edit", report.device.supportsForceTocEdit);
            devObj->setProperty("disc_present", report.device.discPresent);
            devObj->setProperty("writable", report.device.writable);
            devObj->setProperty("write_protected", report.device.writeProtected);
            devObj->setProperty("hi_md_formatted", report.device.hiMdFormatted);
            devObj->setProperty("track_count", report.device.trackCount);
            devObj->setProperty("used_seconds", report.device.usedSeconds);
            devObj->setProperty("total_seconds", report.device.totalSeconds);
            devObj->setProperty("left_seconds", report.device.leftSeconds);
            devObj->setProperty("disc_title",
                                juce::String::fromUTF8(report.device.discTitle.c_str()));
            devObj->setProperty("full_width_title",
                                juce::String::fromUTF8(report.device.fullWidthTitle.c_str()));

            auto* root = new juce::DynamicObject();
            root->setProperty("ok", report.ok);
            root->setProperty("utoc_compiled", report.utocCompiled);
            root->setProperty("device_connected", report.deviceConnected);
            root->setProperty("requires_sp_upload", report.requiresSpUpload);
            root->setProperty("requires_force_toc_edit", report.requiresForceTocEdit);
            root->setProperty("can_write_utoc", report.canWriteUtoc);
            root->setProperty("can_record", report.canRecord);
            root->setProperty("unique_upload_segments", report.uniqueUploadSegments);
            root->setProperty("logical_tracks", report.logicalTracks);
            root->setProperty("has_loaded_disc", report.hasLoadedDisc);
            root->setProperty("preserved_on_disc_tracks", report.preservedOnDiscTracks);
            root->setProperty("on_disc_clusters", report.onDiscClusters);
            root->setProperty("new_upload_clusters", report.newUploadClusters);
            root->setProperty("reclaimed_clusters", report.reclaimedClusters);
            root->setProperty("needs_pre_upload_reclaim", report.needsPreUploadReclaim);
            root->setProperty("exploit_badge", juce::String::fromUTF8(report.exploitBadge.c_str()));
            root->setProperty("status", juce::String::fromUTF8(report.statusText.c_str()));
            root->setProperty("blocker", juce::String::fromUTF8(report.blocker.c_str()));
            root->setProperty("summary", juce::String::fromUTF8(report.summary.c_str()));
            root->setProperty("device", juce::var(devObj));
            root->setProperty("budget", juce::var(budgetObj));
            return ipcReply(true, juce::var(root));
        }
        if (sub == "load-disc") {
            if (args.size() > 3) {
                return ipcReply(false, "Usage: netmd load-disc [DEVICE_ID]");
            }
            if (studio.netMdBusy.load()) {
                return ipcReply(false, "Wait for the active NetMD hardware operation to finish.");
            }
            std::string resolveErr;
            const std::string devId = studio.resolveActiveNetMdDeviceId(
                args.size() == 3 ? args[2] : juce::String(), resolveErr);
            if (devId.empty()) {
                return ipcReply(false, juce::String::fromUTF8(resolveErr.c_str()));
            }
            studio.startNetMdLoadDiscJob(devId, false);
            auto* root = new juce::DynamicObject();
            root->setProperty("started", true);
            root->setProperty("operation", "load-disc");
            root->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            return ipcReply(true, juce::var(root));
        }
        if (sub == "read-utoc") {
            if (studio.netMdBusy.load()) {
                return ipcReply(false, "Wait for the active NetMD hardware operation to finish.");
            }
            std::string explicitDevId;
            juce::String outPath;
            for (int i = 2; i < args.size(); ++i) {
                if (args[i] == "--out") {
                    if (i + 1 >= args.size() || outPath.isNotEmpty()) {
                        return ipcReply(false, "Usage: netmd read-utoc [DEVICE_ID] [--out FILE]");
                    }
                    outPath = args[++i];
                } else if (args[i].startsWith("--out=")) {
                    if (outPath.isNotEmpty()) {
                        return ipcReply(false, "Usage: netmd read-utoc [DEVICE_ID] [--out FILE]");
                    }
                    outPath = args[i].fromFirstOccurrenceOf("--out=", false, false);
                } else if (explicitDevId.empty() && !args[i].startsWith("--")) {
                    explicitDevId = args[i].toStdString();
                } else {
                    return ipcReply(false, "Usage: netmd read-utoc [DEVICE_ID] [--out FILE]");
                }
            }
            juce::File outFile;
            if (outPath.isNotEmpty()) {
                outFile = juce::File::isAbsolutePath(outPath)
                              ? juce::File(outPath)
                              : juce::File::getCurrentWorkingDirectory().getChildFile(outPath);
                if (!outFile.getParentDirectory().isDirectory()) {
                    return ipcReply(false, "Output parent directory does not exist for --out.");
                }
                outPath = outFile.getFullPathName();
            }
            std::string resolveErr;
            const std::string devId = studio.resolveActiveNetMdDeviceId(
                juce::String::fromUTF8(explicitDevId.c_str()), resolveErr);
            if (devId.empty()) {
                return ipcReply(false, juce::String::fromUTF8(resolveErr.c_str()));
            }
            studio.startNetMdReadUtocJob(devId, outFile, outPath, false);
            auto* root = new juce::DynamicObject();
            root->setProperty("started", true);
            root->setProperty("operation", "read-utoc");
            root->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            return ipcReply(true, juce::var(root));
        }
        if (sub == "erase" || sub == "write-utoc" || sub == "record") {
            if (args.size() > 4) {
                return ipcReply(false, "Usage: netmd " + sub + " [DEVICE_ID] [confirm-hardware]");
            }
            if (studio.netMdBusy.load()) {
                return ipcReply(false, "Wait for the active NetMD hardware operation to finish.");
            }
            bool confirmHw = false;
            juce::String explicitDevId;
            for (int i = 2; i < args.size(); ++i) {
                if (args[i] == "confirm-hardware") {
                    if (confirmHw) {
                        return ipcReply(false, "Duplicate confirm-hardware argument.");
                    }
                    confirmHw = true;
                } else if (explicitDevId.isEmpty()) {
                    explicitDevId = args[i];
                } else {
                    return ipcReply(false,
                                    "Usage: netmd " + sub + " [DEVICE_ID] [confirm-hardware]");
                }
            }
            std::string resolveErr;
            const std::string devId = studio.resolveActiveNetMdDeviceId(explicitDevId, resolveErr);
            if (devId.empty()) {
                return ipcReply(false, juce::String::fromUTF8(resolveErr.c_str()));
            }
            if (!confirmHw &&
                (!carta::isNetMdMockDeviceId(devId) || carta::isNetMdTestMockGuardEnabled())) {
                return ipcReply(false, "Hardware confirmation required: pass confirm-hardware to "
                                       "authorize touching '" +
                                           juce::String::fromUTF8(devId.c_str()) + "'.");
            }
            if (sub == "erase") {
                studio.startNetMdEraseJob(devId, confirmHw);
            } else {
                if (sub == "record" && !studio.pendingImports.empty()) {
                    return ipcReply(false,
                                    "Wait for imports to finish before recording to MiniDisc.");
                }
                carta::Session snapshot;
                {
                    const juce::ScopedLock guard(studio.lock);
                    studio.copyWorkerSnapshot(snapshot);
                }
                carta::UtocDiscState utocCheck;
                std::string checkErr;
                if (!carta::compileMdDiscToUtoc(snapshot.mdDisc, utocCheck, checkErr)) {
                    return ipcReply(false, juce::String::fromUTF8(checkErr.c_str()));
                }
                if (sub == "write-utoc") {
                    studio.startNetMdWriteUtocJob(snapshot, devId, confirmHw);
                } else {
                    carta::MdPhysicalPlan plan;
                    if (!carta::planMdPhysicalAudio(snapshot.mdDisc, plan, checkErr)) {
                        return ipcReply(false, juce::String::fromUTF8(checkErr.c_str()));
                    }
                    const auto masterSamples = carta::projectEndSample(snapshot);
                    for (size_t ti = 0; ti < snapshot.mdDisc.tracks.size(); ++ti) {
                        for (const auto& part : snapshot.mdDisc.tracks[ti].parts) {
                            if (snapshot.mdDisc.loadedDisc.active &&
                                carta::findMdOnDiscRegionForPart(snapshot.mdDisc.loadedDisc, part) >=
                                    0) {
                                continue;
                            }
                            if (part.startSample < 0 || part.endSample <= part.startSample ||
                                part.endSample > masterSamples) {
                                return ipcReply(false,
                                                "Track " + juce::String(static_cast<int>(ti + 1)) +
                                                    " extends beyond current timeline audio.");
                            }
                        }
                    }
                    studio.startNetMdRecordJob(std::move(snapshot), devId, confirmHw);
                }
            }
            auto* root = new juce::DynamicObject();
            root->setProperty("started", true);
            root->setProperty("operation", sub);
            root->setProperty("device_id", juce::String::fromUTF8(devId.c_str()));
            return ipcReply(true, juce::var(root));
        }
        return ipcReply(false,
                        "Usage: netmd devices|status|preflight|load-disc|read-utoc|erase|"
                        "write-utoc|record ...");
    }

    static juce::var handleStatus(Studio& studio) {
        refreshProjectStatus(studio);
        if (studio.lastIpcExportHasGeneration) {
            if (auto* exportInfo = studio.lastIpcExport.getDynamicObject()) {
                exportInfo->setProperty("stale",
                                        studio.lastIpcExportGeneration != studio.editGeneration ||
                                            studio.hasPendingGestureEdit());
            }
        }
        double statusPosition = 0;
        double statusPreviewOrigin = 0;
        bool statusPlaying = false;
        bool statusLoopPlaying = false;
        bool statusAudition = false;
        bool statusEditPreview = false;
        bool statusDiscPreview = false;
        bool statusGroupAudition = false;
        bool statusIndependentAudition = false;
        bool statusSavedLoop = false;
        carta::PlaybackRange statusAuditionRange;
        {
            const juce::ScopedLock guard(studio.lock);
            statusPosition = studio.position;
            statusPlaying = studio.playing.load(std::memory_order_relaxed);
            statusLoopPlaying = studio.savedLoopPlaying();
            statusAudition = studio.auditionActive;
            statusEditPreview = studio.cutPreviewActive;
            statusDiscPreview = studio.discPreviewActive && studio.cutPreviewActive;
            statusGroupAudition = studio.groupAuditionActive;
            statusIndependentAudition = studio.independentTrackAudition;
            statusSavedLoop = studio.savedLoopAudition();
            statusAuditionRange = studio.auditionRange;
            statusPreviewOrigin = studio.cutPreviewOriginalPosition;
        }
        if (studio.activeExport && studio.activeExport->ipc) {
            if (auto* exportInfo = studio.lastIpcExport.getDynamicObject()) {
                const int phase = studio.activeExport->phase.load();
                exportInfo->setProperty("phase", phase == 0   ? "preflight"
                                                 : phase == 1 ? "writing"
                                                 : phase == 3 ? "reading"
                                                 : phase == 4 ? "verifying"
                                                              : "decision");
                exportInfo->setProperty(
                    "progress", juce::jlimit(0.0, 1.0, studio.activeExport->progress.load()));
                exportInfo->setProperty("cancellation_requested",
                                        studio.activeExport->cancelled.load());
            }
        }
        juce::var auditionStatus;
        if (statusAudition) {
            auto* detail = new juce::DynamicObject();
            const char* kind =
                statusDiscPreview   ? "disc-preview"
                : statusEditPreview ? "edit-preview"
                : statusGroupAudition
                    ? (statusIndependentAudition ? "selected-tracks" : "selected-clips")
                : statusSavedLoop                       ? "saved-loop"
                : statusAuditionRange.onlyClip >= 0     ? "clip"
                : statusAuditionRange.tracks.first >= 0 ? "track-range"
                                                        : "full-mix";
            detail->setProperty("kind", kind);
            detail->setProperty("playing", statusPlaying);
            detail->setProperty("loop", statusAuditionRange.loop);
            detail->setProperty("start_sample",
                                static_cast<juce::int64>(
                                    std::llround(statusAuditionRange.start * carta::sessionRate)));
            detail->setProperty("end_sample", static_cast<juce::int64>(std::llround(
                                                  statusAuditionRange.end * carta::sessionRate)));
            detail->setProperty("restore_sample",
                                statusEditPreview ? juce::var(static_cast<juce::int64>(std::llround(
                                                        statusPreviewOrigin * carta::sessionRate)))
                                                  : juce::var());
            detail->setProperty("first_track", statusAuditionRange.tracks.first >= 0
                                                   ? juce::var(statusAuditionRange.tracks.first)
                                                   : juce::var());
            detail->setProperty("last_track", statusAuditionRange.tracks.last >= 0
                                                  ? juce::var(statusAuditionRange.tracks.last)
                                                  : juce::var());
            detail->setProperty("clip", statusAuditionRange.onlyClip >= 0
                                            ? juce::var(statusAuditionRange.onlyClip)
                                            : juce::var());
            auditionStatus = juce::var(detail);
        }

        auto* o = new juce::DynamicObject();
        o->setProperty("project", studio.projectFile == juce::File{}
                                      ? juce::String()
                                      : studio.projectFile.getFullPathName());
        o->setProperty("dirty", studio.dirty);
        o->setProperty("pending_disc_edits", false);
        o->setProperty("md_disc", buildMdDiscStatusJson(studio.session.mdDisc));
        o->setProperty("playing", statusPlaying);
        o->setProperty("audition", auditionStatus);
        o->setProperty(
            "edit_lock_silent_blocks",
            static_cast<juce::int64>(studio.editLockSilentBlocks.load(std::memory_order_relaxed)));
        o->setProperty("position_seconds", statusPosition);
        o->setProperty("duration_seconds", studio.session.length());
        o->setProperty("sample_rate", carta::sessionRate);
        o->setProperty("master_channels", studio.masterChannels);
        o->setProperty("layer_count", static_cast<int>(studio.session.tracks.size()));
        o->setProperty("track_count", static_cast<int>(studio.session.tracks.size()));
        o->setProperty("marker_count", static_cast<int>(studio.session.markers.size()));
        o->setProperty("pending_imports", static_cast<int>(studio.pendingImports.size()));
        o->setProperty("import_issues",
                       static_cast<int>(studio.importIssues.size()) + studio.omittedImportIssues);
        o->setProperty("message", studio.status.getText());
        o->setProperty("playhead_sample",
                       static_cast<juce::int64>(std::llround(statusPosition * carta::sessionRate)));
        o->setProperty("selected_layer", studio.selectedTrack);
        o->setProperty("selected_track", studio.selectedTrack);
        o->setProperty("selected_clip", studio.selectedClip);
        o->setProperty("selected_marker", studio.selectedMarker);
        o->setProperty("export", studio.lastIpcExport);
        o->setProperty("project_operation", studio.lastIpcProject);
        o->setProperty("netmd", studio.buildNetMdStatusJson());
        o->setProperty("can_undo", studio.editHistory.canUndo());
        o->setProperty("can_redo", studio.editHistory.canRedo());

        if (studio.savedLoop.end > studio.savedLoop.start) {
            auto* loop = new juce::DynamicObject();
            loop->setProperty("start_sample", studio.savedLoop.start);
            loop->setProperty("end_sample", studio.savedLoop.end);
            loop->setProperty("valid",
                              studio.savedLoop.end <= carta::projectEndSample(studio.session));
            loop->setProperty("playing", statusLoopPlaying);
            o->setProperty("loop_region", juce::var(loop));
        } else {
            o->setProperty("loop_region", juce::var());
        }

        juce::Array<juce::var> independent;
        for (int track : studio.selectedTimelineTracks()) {
            independent.add(track);
        }
        o->setProperty("selected_layers", juce::var(independent));
        o->setProperty("selected_tracks", juce::var(independent));

        juce::Array<juce::var> selectedClips;
        for (const auto& ref : studio.timelineClipSelection.get(studio.editGeneration)) {
            auto* item = new juce::DynamicObject();
            item->setProperty("layer", ref.track);
            item->setProperty("track", ref.track);
            item->setProperty("clip", ref.clip);
            selectedClips.add(juce::var(item));
        }
        o->setProperty("selected_clips", juce::var(selectedClips));

        if (studio.timeSelection.hasRange()) {
            auto* range = new juce::DynamicObject();
            range->setProperty("first_track", studio.timeSelection.firstTrack);
            range->setProperty("last_track", studio.timeSelection.lastTrack);
            range->setProperty("start_sample", studio.timeSelection.start);
            range->setProperty("end_sample", studio.timeSelection.end);
            o->setProperty("selection", juce::var(range));
        } else {
            o->setProperty("selection", juce::var());
        }

        if (studio.timeSelection.hasPoint() &&
            studio.timeSelection.firstTrack < static_cast<int>(studio.session.tracks.size())) {
            auto* point = new juce::DynamicObject();
            point->setProperty("layer", studio.timeSelection.firstTrack);
            point->setProperty("track", studio.timeSelection.firstTrack);
            point->setProperty("sample", studio.timeSelection.start);
            o->setProperty("edit_point", juce::var(point));
        } else {
            o->setProperty("edit_point", juce::var());
        }
        return ipcReply(true, juce::var(o));
    }

    static juce::var handleTracks(Studio& studio) {
        juce::Array<juce::var> tracks;
        for (const auto& track : studio.session.tracks) {
            auto* t = new juce::DynamicObject();
            t->setProperty("name", track.name);
            t->setProperty("muted", track.muted);
            t->setProperty("solo", track.solo);
            t->setProperty("gain", track.gain);
            t->setProperty("pan", track.pan);
            const int channels = carta::maxTrackChannels(track);
            t->setProperty("routing_confirmed",
                           channels == 0 || carta::clipRoutingConfirmed(track, channels));

            juce::Array<juce::var> routing;
            for (const auto& route : track.routing) {
                auto* row = new juce::DynamicObject();
                row->setProperty("left", route.left);
                row->setProperty("right", route.right);
                routing.add(juce::var(row));
            }
            t->setProperty("routing", juce::var(routing));

            t->setProperty("height", track.height);
            t->setProperty("collapsed", track.collapsed);
            t->setProperty("waveform_zoom", track.waveformZoom == 0
                                                ? juce::var("default")
                                                : juce::var(track.waveformZoom));
            juce::Array<juce::var> heightWeights;
            for (int weight : track.channelHeightWeights) {
                heightWeights.add(weight);
            }
            t->setProperty("channel_height_weights", heightWeights);
            t->setProperty("view", track.view == carta::TrackView::useDefault    ? "default"
                                   : track.view == carta::TrackView::waveform    ? "waveform"
                                   : track.view == carta::TrackView::spectrogram ? "spectrogram"
                                                                                 : "combined");

            juce::Array<juce::var> clips;
            for (const auto& clip : track.clips) {
                auto* c = new juce::DynamicObject();
                c->setProperty("name", carta::clipDisplayName(clip));
                c->setProperty("custom_name", clip.name);
                juce::int64 first = 0;
                juce::int64 last = 0;
                const bool hasSamples = carta::clipSampleRange(clip, first, last);
                c->setProperty("start_sample", hasSamples ? juce::var(first) : juce::var());
                c->setProperty("end_sample", hasSamples ? juce::var(last) : juce::var());
                c->setProperty("group_id", clip.groupId);
                c->setProperty("source",
                               clip.audio ? clip.audio->file.getFullPathName() : juce::String());
                c->setProperty("source_sample_rate", clip.audio ? clip.audio->sampleRate : 0);
                c->setProperty("source_channels", clip.audio ? clip.audio->numChannels() : 0);
                c->setProperty("start_seconds", clip.start);
                c->setProperty("duration_seconds", clip.duration);
                c->setProperty("offset_seconds", clip.offset);
                c->setProperty("gain", clip.gain);
                c->setProperty("fade_in_seconds", clip.fadeIn);
                c->setProperty("fade_out_seconds", clip.fadeOut);
                c->setProperty("fade_in_source_start", clip.fadeInStart);
                c->setProperty("fade_out_source_end", clip.fadeOutEnd);
                c->setProperty("fade_in_shape", carta::fadeShapeToString(clip.fadeInShape));
                c->setProperty("fade_out_shape", carta::fadeShapeToString(clip.fadeOutShape));
                clips.add(juce::var(c));
            }
            t->setProperty("clips", juce::var(clips));
            tracks.add(juce::var(t));
        }
        return ipcReply(true, juce::var(tracks));
    }

    static juce::var handleMasterTracks(Studio& studio) {
        std::vector<carta::MasterTrack> masterTracks;
        juce::String error;
        if (!carta::getMasterTracks(studio.session, masterTracks, error)) {
            return ipcReply(false, error);
        }
        juce::Array<juce::var> rows;
        for (const auto& track : masterTracks) {
            auto* row = new juce::DynamicObject();
            row->setProperty("name", track.name);
            row->setProperty("start_sample", track.range.start);
            row->setProperty("end_sample", track.range.end);
            rows.add(juce::var(row));
        }
        return ipcReply(true, juce::var(rows));
    }

    static juce::var handleMarkers(Studio& studio) {
        juce::Array<juce::var> markers;
        const auto inspected = carta::inspectMarkers(studio.session);
        std::vector<carta::MarkerInspection> byIndex(studio.session.markers.size());
        for (const auto& row : inspected) {
            byIndex[static_cast<size_t>(row.index)] = row;
        }
        for (size_t index = 0; index < studio.session.markers.size(); ++index) {
            const auto& item = studio.session.markers[index];
            const auto& info = byIndex[index];
            auto* m = new juce::DynamicObject();
            m->setProperty("id", item.id);
            m->setProperty("name", item.name);
            m->setProperty("position_seconds", item.position);
            m->setProperty("sample", info.validPosition && !info.offGrid ? juce::var(info.sample)
                                                                         : juce::var());
            m->setProperty("valid_position", info.validPosition);
            m->setProperty("off_grid", info.offGrid);
            m->setProperty("outside_master", info.outside);
            m->setProperty("duplicate_sample", info.duplicate);
            m->setProperty("empty_name", info.emptyName);
            markers.add(juce::var(m));
        }
        return ipcReply(true, juce::var(markers));
    }
};

juce::var Studio::ipcCommand(const juce::StringArray& args) {
    // IPC callbacks run on the message thread. Session geometry, selection,
    // and background-task handles are message-thread-owned; the audio
    // callback only changes transport state under the playback lock.
    if (args.isEmpty()) {
        return ipcReply(false, "Missing command");
    }
    // Native sheets are not JUCE modal components. Keep the project stable
    // while the user decides, even for IPC commands delivered asynchronously.
    if (savePromptActive && args[0] != "status" && args[0] != "formats" &&
        !(args.size() == 2 && (args[0] == "project" || args[0] == "imports" || args[0] == "disc") &&
          args[1] == "status")) {
        return ipcReply(false, "Finish the current dialog first.");
    }

    const auto& cmd = args[0];
    if (cmd == "formats") {
        return handleFormatsCommand(args);
    }
    if (cmd == "disc") {
        return IpcRouter::handleDisc(*this, args);
    }
    if (cmd == "meter") {
        return IpcRouter::handleMeter(*this, args);
    }
    if (cmd == "history") {
        return IpcRouter::handleHistory(*this, args);
    }
    if (cmd == "snap") {
        return IpcRouter::handleSnap(*this, args);
    }
    if (cmd == "sources") {
        return IpcRouter::handleSources(*this, args);
    }
    if (cmd == "analysis") {
        return IpcRouter::handleAnalysis(*this, args);
    }
    if (cmd == "view") {
        return IpcRouter::handleView(*this, args);
    }
    if (cmd == "loop") {
        return IpcRouter::handleLoop(*this, args);
    }
    if (cmd == "marker") {
        return IpcRouter::handleMarker(*this, args);
    }
    if (cmd == "seek") {
        return IpcRouter::handleSeek(*this, args);
    }
    if (cmd == "select") {
        return IpcRouter::handleSelect(*this, args);
    }
    if (cmd == "edit") {
        return IpcRouter::handleEdit(*this, args);
    }
    if (cmd == "clip") {
        return IpcRouter::handleClip(*this, args);
    }
    if (cmd == "master") {
        return IpcRouter::handleMaster(*this, args);
    }
    if (cmd == "track" || cmd == "layer") {
        return IpcRouter::handleTrackOrLayer(*this, args);
    }
    if (cmd == "project") {
        return IpcRouter::handleProject(*this, args);
    }
    if (cmd == "render") {
        return IpcRouter::handleRender(*this, args);
    }
    if (cmd == "export") {
        return IpcRouter::handleExport(*this, args);
    }
    if (cmd == "imports") {
        return IpcRouter::handleImports(*this, args);
    }
    if (cmd == "import") {
        return IpcRouter::handleImport(*this, args);
    }
    if (cmd == "transport") {
        if (auto res = IpcRouter::handleTransport(*this, args)) {
            return *res;
        }
    }
    if (cmd == "master-track") {
        return IpcRouter::handleMasterTrack(*this, args);
    }
    if (cmd == "app") {
        return IpcRouter::handleApp(*this, args);
    }
    if (cmd == "utoc") {
        return IpcRouter::handleUtoc(*this, args);
    }
    if (cmd == "schema") {
        return handleSchemaCommand(args);
    }
    if (cmd == "encode") {
        return handleEncodeCommand(args);
    }
    if (cmd == "netmd") {
        return IpcRouter::handleNetMd(*this, args);
    }

    if (args.size() != 1) {
        return ipcReply(false, "Unexpected arguments");
    }
    if (cmd == "status") {
        return IpcRouter::handleStatus(*this);
    }
    if (cmd == "tracks" || cmd == "layers") {
        return IpcRouter::handleTracks(*this);
    }
    if (cmd == "master-tracks") {
        return IpcRouter::handleMasterTracks(*this);
    }
    if (cmd == "markers") {
        return IpcRouter::handleMarkers(*this);
    }
    return ipcReply(false, "Unknown IPC command: " + cmd);
}
