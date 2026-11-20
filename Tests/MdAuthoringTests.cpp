/**
 * @file MdAuthoringTests.cpp
 * @brief Unit tests for UTOC-native MiniDisc authoring (`MdDisc`, `MdTrack`, `MdGroup`).
 *
 * Covers default disc detection, auto-tracking on Layer 1 import/placement,
 * Layer 1 and marker sync, group span maintenance across structural track
 * edits, Sound Group coalescing + UTOC roundtrip, and interactive MD ribbon
 * boundary/part manipulation at the 44.1 kHz project sample rate.
 */
#include "MiniDisc/MdAuthoring.h"
#include "Core/Session.h"
#include "Editing/Editing.h"
#include "IO/SessionIO.h"
#include "MiniDisc/Utoc.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        ++gFailures;
    }
}

std::shared_ptr<carta::Audio> makeSilentAudio(int samples) {
    auto a = std::make_shared<carta::Audio>();
    a->sampleRate = carta::sessionRate;
    a->samples.setSize(2, samples);
    a->samples.clear();
    return a;
}

/** Verifies fresh `MdDisc` defaults and `appendAutoTrackForClip` behavior. */
void testDefaultAndAutoTrack() {
    carta::MdDisc disc;
    check(carta::isDefaultMdDisc(disc), "fresh MdDisc is default");

    check(carta::appendAutoTrackForClip(disc, "Intro.wav", 0, 44100), "append auto track 1");
    check(!carta::isDefaultMdDisc(disc), "non-empty MdDisc is no longer default");
    check(disc.tracks.size() == 1 && disc.tracks[0].halfWidthTitle == "Intro",
          "extension stripped from auto track title");
    check(disc.tracks[0].parts.size() == 1 && disc.tracks[0].parts[0].startSample == 0 &&
              disc.tracks[0].parts[0].endSample == 44100,
          "auto track covers clip range");

    check(carta::appendAutoTrackForClip(disc, "Main Theme.flac", 44100, 132300),
          "append auto track 2");
    check(disc.tracks.size() == 2 && disc.tracks[1].halfWidthTitle == "Main Theme",
          "second auto track appended");

    disc.autoTrackOnImport = false;
    check(!carta::appendAutoTrackForClip(disc, "Ignored.wav", 132300, 176400),
          "auto-track disabled suppresses append");
    check(disc.tracks.size() == 2, "disabled auto-track leaves track list unchanged");
}

/** Verifies `syncTracksFromLayerClips` and `syncTracksFromMarkers` rebuilding. */
void testSyncFromLayerClipsAndMarkers() {
    carta::Session session;
    auto a = makeSilentAudio(44100);
    auto b = makeSilentAudio(44100 * 2);
    check(carta::addImportedAudioTrack(session, a, 0), "add Layer 1 clip A");
    session.tracks[0].clips[0].name = "Opening.wav";
    check(carta::addImportedAudioToTrack(session, 0, b, 44100), "add Layer 1 clip B");
    session.tracks[0].clips[1].name = "Finale.aiff";

    carta::MdDisc disc;
    disc.autoTrackOnImport = false;
    std::string err;
    check(carta::syncTracksFromLayerClips(session, disc, err),
          "syncTracksFromLayerClips builds tracks: " + err);
    check(disc.autoTrackOnImport, "syncTracksFromLayerClips re-enables autoTrackOnImport");
    check(disc.tracks.size() == 2, "two Layer 1 clips produce two MD tracks");
    check(disc.tracks[0].halfWidthTitle == "Opening" && disc.tracks[1].halfWidthTitle == "Finale",
          "Layer 1 sync strips extensions and sorts chronologically");

    // Delete all tracks manually and recover cleanly via syncTracksFromLayerClips.
    check(carta::removeMdTrack(disc, 1, err) && carta::removeMdTrack(disc, 0, err),
          "remove all tracks");
    check(disc.tracks.empty(), "all MD tracks removed");
    check(carta::syncTracksFromLayerClips(session, disc, err) && disc.tracks.size() == 2,
          "removing all tracks can be recovered cleanly with syncTracksFromLayerClips");

    // Markers sync with intro gap before first marker.
    session.markers.push_back({1.0, "Verse"});
    session.markers.push_back({2.0, "Chorus"});
    check(carta::syncTracksFromMarkers(session, disc, err),
          "syncTracksFromMarkers succeeds: " + err);
    check(disc.tracks.size() == 3, "markers at 1s and 2s on 3s timeline produce 3 tracks");
    check(disc.tracks[0].halfWidthTitle == "Track 1" && disc.tracks[0].parts[0].startSample == 0 &&
              disc.tracks[0].parts[0].endSample == 44100,
          "intro region before first marker preserved");
    check(disc.tracks[1].halfWidthTitle == "Verse" && disc.tracks[2].halfWidthTitle == "Chorus",
          "marker names become track titles");
}

/** Verifies `MdGroup` spans remain valid across insert, split, duplicate, move, remove, and
 * ungroup. */
void testGroupConsistencyAcrossTrackEdits() {
    carta::MdDisc disc;
    check(carta::appendAutoTrackForClip(disc, "One", 0, 44100), "add t1");
    check(carta::appendAutoTrackForClip(disc, "Two", 44100, 88200), "add t2");
    check(carta::appendAutoTrackForClip(disc, "Three", 88200, 132300), "add t3");
    check(carta::appendAutoTrackForClip(disc, "Four", 132300, 176400), "add t4");

    std::string err;
    check(carta::setMdGroup(disc, 1, 2, "Side A", "", err), "set group 1..2 Side A: " + err);
    check(carta::setMdGroup(disc, 3, 4, "Side B", "", err), "set group 3..4 Side B: " + err);
    check(disc.groups.size() == 2, "two disjoint groups created");

    // Insert track BETWEEN track 1 and track 2 (1-based pos 2) inside Side A.
    carta::MdTrack mid;
    mid.halfWidthTitle = "Inserted Mid";
    mid.parts.push_back({1024, 8192});
    check(carta::insertMdTrack(disc, 2, mid, err), "insert mid-group succeeds: " + err);
    check(disc.tracks.size() == 5 && disc.tracks[1].halfWidthTitle == "Inserted Mid",
          "inserted at pos 2");
    check(disc.groups[0].startTrack == 1 && disc.groups[0].endTrack == 3 &&
              disc.groups[0].halfWidthTitle == "Side A",
          "Side A expands to 1..3 after mid-group insert");
    check(disc.groups[1].startTrack == 4 && disc.groups[1].endTrack == 5 &&
              disc.groups[1].halfWidthTitle == "Side B",
          "Side B shifts to 4..5 after earlier insert");

    // Split track 1 (0-based index 0) mid-part; expands Side A to 1..4.
    check(carta::splitMdTrackAtSample(disc, 0, 22050, err), "split track 1 at 22050: " + err);
    check(disc.tracks.size() == 6 && disc.tracks[0].parts[0].endSample == 22050 &&
              disc.tracks[1].parts[0].startSample == 22050,
          "split cuts track cleanly across new track");
    check(disc.groups[0].startTrack == 1 && disc.groups[0].endTrack == 4,
          "Side A expands to 1..4 after split");
    check(disc.groups[1].startTrack == 5 && disc.groups[1].endTrack == 6,
          "Side B shifts to 5..6 after split");

    // Duplicate track inside Side A.
    check(carta::duplicateMdTrack(disc, 1, err), "duplicate track 2: " + err);
    check(disc.tracks.size() == 7 && disc.groups[0].endTrack == 5 &&
              disc.groups[1].startTrack == 6 && disc.groups[1].endTrack == 7,
          "duplicate expands Side A cleanly");

    // Move track 2 to position 1 inside Side A.
    const std::string movedId = disc.tracks[1].id;
    check(carta::moveMdTrack(disc, 1, 0, err), "move track within Side A: " + err);
    check(disc.tracks[0].id == movedId && disc.groups[0].startTrack == 1 &&
              disc.groups[0].endTrack == 5,
          "reorder inside group keeps group span intact");

    // Remove track from Side A.
    check(carta::removeMdTrack(disc, 0, err), "remove track 1 from Side A: " + err);
    check(disc.tracks.size() == 6 && disc.groups[0].startTrack == 1 &&
              disc.groups[0].endTrack == 4 && disc.groups[1].startTrack == 5 &&
              disc.groups[1].endTrack == 6,
          "remove shrinks Side A cleanly and shifts Side B");

    // Ungroup partial range & verify removing sole track of 1-track group drops the group.
    check(carta::ungroupMdRange(disc, 1, 3, err), "ungroup 1..3 leaves 4..4 Side A");
    check(disc.groups.size() == 2 && disc.groups[0].startTrack == 4 && disc.groups[0].endTrack == 4,
          "partial ungroup shrinks group to single remaining track 4");
    check(carta::removeMdTrack(disc, 3, err), "remove sole track of 1-track group: " + err);
    check(carta::validateMdDisc(disc, err),
          "disc remains valid after deleting 1-track group: " + err);
    check(disc.groups.size() == 1 && disc.groups[0].halfWidthTitle == "Side B" &&
              disc.groups[0].startTrack == 4 && disc.groups[0].endTrack == 5,
          "deleting sole track of 1-track group removes that group and shifts Side B");
}

/** Verifies Sound Group coalescing across shared/mixed-codec parts and UTOC roundtrip. */
void testPhysicalPlanCoalescingAndUtocRoundtrip() {
    carta::MdDisc disc;
    disc.medium = carta::MdMedium::md80;
    disc.defaultCodec = carta::MdCodecMode::spStereo;
    disc.halfWidthTitle = "Mixtape 99";
    disc.fullWidthTitle = "\xE3\x83\x9F\xE3\x83\x83\xE3\x82\xAF\xE3\x82\xB9"; // ミックス
    disc.discTimestamp = "2026-11-15 10:00:00";

    carta::MdTrack t1;
    t1.id = carta::makeMdTrackId();
    t1.halfWidthTitle = "Intro+Body";
    t1.writeProtected = true;
    t1.scmsCopyProtected = true;
    t1.parts = {{0, 44100 * 60}, {44100 * 60, 44100 * 120}};

    carta::MdTrack t2;
    t2.id = carta::makeMdTrackId();
    t2.halfWidthTitle = "Reprise";
    t2.parts = {{0, 44100 * 60}}; // Reuses first part physically

    carta::MdTrack t3;
    t3.id = carta::makeMdTrackId();
    t3.halfWidthTitle = "LP2 Bonus";
    t3.codecOverride = carta::MdCodecMode::lp2;
    t3.parts = {{0, 44100 * 60}}; // Different codec -> must NOT coalesce into SP span!

    disc.tracks = {t1, t2, t3};
    std::string err;
    check(carta::setMdGroup(disc, 1, 2, "Main Set", "", err), "set Main Set group: " + err);

    carta::MdPhysicalPlan plan;
    check(carta::planMdPhysicalAudio(disc, plan, err), "planMdPhysicalAudio succeeds: " + err);
    check(plan.spans.size() == 2, "SP parts coalesce into 1 span while LP2 gets its own span");
    check(plan.spans[0].codec == carta::MdCodecMode::spStereo &&
              plan.spans[1].codec == carta::MdCodecMode::lp2,
          "span codecs preserved accurately");
    check(plan.savedClusters > 0 && plan.usedClusters < plan.uncoalescedClusters,
          "Sound Group coalescing + reuse saves physical clusters");

    carta::UtocDiscState utoc;
    check(carta::compileMdDiscToUtoc(disc, utoc, err), "compileMdDiscToUtoc succeeds: " + err);
    carta::MdDisc restored;
    check(carta::mdDiscFromUtoc(utoc, restored, err), "mdDiscFromUtoc roundtrips: " + err);
    check(restored.tracks.size() == 3, "restored 3 tracks");
    check(restored.tracks[0].writeProtected && restored.tracks[0].scmsCopyProtected,
          "WP+SCMS flags preserved");
    check(restored.tracks[0].parts.size() == 2 && restored.tracks[1].parts.size() == 1,
          "multi-part counts preserved");
    check(restored.tracks[0].parts[0] == restored.tracks[1].parts[0],
          "shared physical part identity preserved across tracks");
    check(restored.tracks[2].codecOverride == carta::MdCodecMode::lp2,
          "per-track LP2 override preserved");
    check(restored.groups.size() == 1 && restored.groups[0].startTrack == 1 &&
              restored.groups[0].endTrack == 2 && restored.groups[0].halfWidthTitle == "Main Set",
          "group range preserved through UTOC");
    check(restored.discTimestamp == "2026-11-15 10:00:00", "disc timestamp preserved through UTOC");
}

/** Verifies auto-track placement, multi-layer clip sync, interactive ribbon edits, and merges. */
void testInteractiveRibbonAndMultiLayerClipSync() {
    std::string err;

    // 1. applyAutoTrackForPlacedClip: sequential, crossfade overlap extension, and contained
    // overlay
    carta::MdDisc autoDisc;
    check(carta::applyAutoTrackForPlacedClip(autoDisc, "Intro.wav", 0, 44032),
          "auto-track accepts initial sequential clip");
    check(autoDisc.tracks.size() == 1 && autoDisc.tracks[0].parts.size() == 1 &&
              autoDisc.tracks[0].parts[0].startSample == 0 &&
              autoDisc.tracks[0].parts[0].endSample == 44032,
          "initial auto-track covers [0, 44032)");

    check(carta::applyAutoTrackForPlacedClip(autoDisc, "Crossfaded.wav", 40000, 88064),
          "auto-track accepts overlapping clip extending past previous end");
    check(autoDisc.tracks.size() == 2 && autoDisc.tracks[0].parts[0].endSample == 40000 &&
              autoDisc.tracks[1].parts[0].startSample == 40000 &&
              autoDisc.tracks[1].parts[0].endSample == 88064,
          "overlapping placed clip trims prior auto-track at crossfade start 40000 and creates "
          "track 2 [40000, 88064)");

    autoDisc.autoTrackOnImport = false;
    check(!carta::applyAutoTrackForPlacedClip(autoDisc, "Disabled.wav", 88064, 120000),
          "autoTrackOnImport == false suppresses applyAutoTrackForPlacedClip");
    autoDisc.autoTrackOnImport = true;

    // 2. Multi-layer syncTracksFromClips (unmuted crossfade handoff + contained stem + muted layer
    // + explicit subset)
    carta::Session session;
    auto a = makeSilentAudio(132096);
    auto makeClip = [&](std::int64_t startSample, std::int64_t durSamples, const char* name) {
        carta::Clip c;
        c.audio = a;
        c.start = static_cast<double>(startSample) / carta::sessionRate;
        c.duration = static_cast<double>(durSamples) / carta::sessionRate;
        c.name = name;
        return c;
    };
    carta::Track layer1;
    layer1.name = "Layer 1";
    layer1.clips.push_back(makeClip(0, 50000, "SongA.wav"));

    carta::Track layer2;
    layer2.name = "Layer 2";
    layer2.clips.push_back(makeClip(10000, 20000, "Stem.wav"));
    layer2.clips.push_back(makeClip(40000, 50000, "SongB.wav"));

    carta::Track layer3;
    layer3.name = "Layer 3";
    layer3.muted = true;
    layer3.clips.push_back(makeClip(0, 120000, "MutedRef.wav"));

    session.tracks = {layer1, layer2, layer3};

    carta::MdDisc syncDisc;
    check(carta::syncTracksFromClips(session, syncDisc, err),
          "syncTracksFromClips multi-layer succeeds: " + err);
    check(
        syncDisc.tracks.size() == 2 && syncDisc.tracks[0].halfWidthTitle == "SongA" &&
            syncDisc.tracks[0].parts.size() == 1 && syncDisc.tracks[0].parts[0].startSample == 0 &&
            syncDisc.tracks[0].parts[0].endSample == 40000 &&
            syncDisc.tracks[1].halfWidthTitle == "SongB" && syncDisc.tracks[1].parts.size() == 1 &&
            syncDisc.tracks[1].parts[0].startSample == 40000 &&
            syncDisc.tracks[1].parts[0].endSample == 90000,
        "syncTracksFromClips trims crossfading SongA at SongB start, ignores contained stem and "
        "muted Layer 3");

    check(carta::syncTracksFromClips(session, syncDisc, err, {{1, 0}}),
          "syncTracksFromClips explicit subset succeeds: " + err);
    check(syncDisc.tracks.size() == 1 && syncDisc.tracks[0].halfWidthTitle == "Stem" &&
              syncDisc.tracks[0].parts[0].startSample == 10000 &&
              syncDisc.tracks[0].parts[0].endSample == 30000,
          "explicit clip selection creates track strictly for selected stem");

    // 3. Interactive boundary edits (moveMdSharedBoundary, trimMdPartEdge, slideMdPartRange) and
    // mergeMdTrackWithNext
    carta::MdDisc editDisc;
    editDisc.tracks = {
        {carta::makeMdTrackId(), "Part1", {}, {}, false, false, false, {}, {{0, 40000}}},
        {carta::makeMdTrackId(), "Part2", {}, {}, false, false, false, {}, {{40000, 90000}}},
        {carta::makeMdTrackId(), "Outro", {}, {}, false, false, false, {}, {{90000, 120000}}}};
    editDisc.groups = {{1, 2, "Pair", {}}};

    check(carta::moveMdSharedBoundary(editDisc, 0, 0, 1, 0, 45000, 120000, err),
          "moveMdSharedBoundary moves rolling split cleanly: " + err);
    check(editDisc.tracks[0].parts[0].endSample == 45000 &&
              editDisc.tracks[1].parts[0].startSample == 45000,
          "moveMdSharedBoundary updates both left end and right start");

    check(carta::trimMdPartEdge(editDisc, 0, 0, true, 42000, 120000, err),
          "trimMdPartEdge trims single edge independently: " + err);
    check(editDisc.tracks[0].parts[0].endSample == 42000 &&
              editDisc.tracks[1].parts[0].startSample == 45000,
          "trimMdPartEdge leaves adjacent track start unchanged");

    check(carta::slideMdPartRange(editDisc, 0, 0, 3000, 120000, err),
          "slideMdPartRange slides part while preserving duration: " + err);
    check(editDisc.tracks[0].parts[0].startSample == 3000 &&
              editDisc.tracks[0].parts[0].endSample == 45000,
          "slideMdPartRange lands at [3000, 45000) abutting Part2");

    check(carta::mergeMdTrackWithNext(editDisc, 0, err),
          "mergeMdTrackWithNext coalesces abutting parts: " + err);
    check(editDisc.tracks.size() == 2 && editDisc.tracks[0].halfWidthTitle == "Part1" &&
              editDisc.tracks[0].parts.size() == 1 &&
              editDisc.tracks[0].parts[0].startSample == 3000 &&
              editDisc.tracks[0].parts[0].endSample == 90000 && editDisc.groups.size() == 1 &&
              editDisc.groups[0].startTrack == 1 && editDisc.groups[0].endTrack == 1,
          "mergeMdTrackWithNext coalesces [3000, 45000)+[45000, 90000) and shrinks group 1..2 to "
          "1..1");
}

/** Verifies Stage 1 loaded-disc lock, placeholder silence render, auto-track preservation, and v26 SessionIO. */
void testLoadedDiscLocksPlaceholderAndSessionIO() {
    carta::MdDisc loaded;
    loaded.medium = carta::MdMedium::md80;
    loaded.defaultCodec = carta::MdCodecMode::spStereo;
    loaded.halfWidthTitle = "Loaded Master";
    loaded.loadedDisc.active = true;
    loaded.loadedDisc.sourceDeviceId = "mock:mz-n920";
    loaded.loadedDisc.baselineSector0Hash =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    loaded.loadedDisc.deviceSignature = 0x534F;
    loaded.loadedDisc.physicalStartCluster = 50;
    loaded.loadedDisc.physicalLastCluster = 2300;
    loaded.loadedDisc.regions = {
        {"od-0", carta::MdCodecMode::spStereo, 0xCE, {50, 0, 0}, {50, 31, 10}, 352, 0, 90112},
        {"od-1", carta::MdCodecMode::lp2, 0xC2, {52, 0, 0}, {53, 31, 10}, 704, 90112, 450560}};
    loaded.loadedDisc.initialFreeRanges = {{{100, 0, 0}, {2300, 31, 10}}};
    loaded.loadedDisc.defectiveRanges = {{{200, 0, 0}, {200, 31, 10}}};
    loaded.loadedDisc.rawSector3.assign(carta::kUtocSectorBytes, 0xA5);

    carta::MdTrack t1;
    t1.id = "od-t1";
    t1.halfWidthTitle = "Disc Track 1";
    t1.codecOverride = carta::MdCodecMode::spStereo;
    t1.parts = {{0, 90112}};
    carta::MdTrack t2;
    t2.id = "od-t2";
    t2.halfWidthTitle = "Disc Track 2";
    t2.codecOverride = carta::MdCodecMode::lp2;
    t2.parts = {{90112, 450560}};
    loaded.tracks = {t1, t2};

    carta::Session session;
    carta::populateLoadedDiscSession(loaded, session);
    check(session.tracks.size() == 1 && session.tracks[0].clips.size() == 2,
          "populateLoadedDiscSession creates locked placeholder clips on Track 0");
    check(session.tracks[0].clips[0].lockedOnDisc && session.tracks[0].clips[1].lockedOnDisc &&
              session.tracks[0].clips[0].audio->onDiscPlaceholder &&
              !session.tracks[0].clips[0].audio->hasPcm(),
          "placeholder clips carry onDiscPlaceholder=true and lockedOnDisc=true with 0 PCM RAM");

    // Placeholder rendering adds zero contribution and zero peak ranges safely
    juce::AudioBuffer<float> mix(2, 1024);
    mix.clear();
    mix.setSample(0, 0, 0.25f);
    session.render(mix, 0, 1024, 0.0, carta::sessionRate, 2);
    bool silenceExact = (mix.getSample(0, 0) == 0.25f);
    for (int ch = 0; ch < mix.getNumChannels(); ++ch) {
        for (int i = 0; i < mix.getNumSamples(); ++i) {
            if (ch == 0 && i == 0) {
                continue;
            }
            if (mix.getSample(ch, i) != 0.0f) {
                silenceExact = false;
            }
        }
    }
    check(silenceExact, "Session::render outputs exact silence for onDiscPlaceholder clips");
    const auto wf = carta::waveformRange(*session.tracks[0].clips[0].audio, 0, 0, 1000);
    check(wf.getStart() == 0.0f && wf.getEnd() == 0.0f,
          "waveformRange returns 0..0 safely on onDiscPlaceholder audio");

    // preservesOnDiscAudio guards
    juce::String lockErr;
    {
        carta::Session s = session;
        s.tracks[0].clips[0].start += 0.5;
        check(!carta::preservesOnDiscAudio(session, s, lockErr),
              "preservesOnDiscAudio blocks moving locked on-disc clip");
    }
    {
        carta::Session s = session;
        s.tracks[0].clips[0].duration -= 0.1;
        check(!carta::preservesOnDiscAudio(session, s, lockErr),
              "preservesOnDiscAudio blocks trimming locked on-disc clip");
    }
    {
        carta::Session s = session;
        s.tracks[0].clips[0].fadeIn = 0.25;
        check(!carta::preservesOnDiscAudio(session, s, lockErr),
              "preservesOnDiscAudio blocks adding fade to locked on-disc clip");
    }
    {
        carta::Session s = session;
        s.tracks[0].clips[0].gain = 0.5f;
        check(!carta::preservesOnDiscAudio(session, s, lockErr),
              "preservesOnDiscAudio blocks changing gain on locked on-disc clip");
    }
    {
        carta::Session s = session;
        s.tracks[0].clips.erase(s.tracks[0].clips.begin());
        check(!carta::preservesOnDiscAudio(session, s, lockErr),
              "preservesOnDiscAudio blocks deleting locked on-disc clip");
    }
    {
        carta::Session s = session;
        s.tracks[0].clips.push_back(s.tracks[0].clips[0]);
        check(!carta::preservesOnDiscAudio(session, s, lockErr),
              "preservesOnDiscAudio blocks duplicating locked on-disc clip");
    }
    {
        carta::Session s = session;
        auto local = makeSilentAudio(44100);
        // Overlapping a locked clip on Track 0 fails
        carta::Clip overlapClip;
        overlapClip.audio = local;
        overlapClip.start = 0.0;
        overlapClip.offset = 0.0;
        overlapClip.duration = 1.0;
        overlapClip.name = "Overlap";
        s.tracks[0].clips.push_back(overlapClip);
        check(!carta::preservesOnDiscAudio(session, s, lockErr),
              "preservesOnDiscAudio blocks placing new clip overlapping locked clip on same track");
    }
    {
        carta::Session s = session;
        auto local = makeSilentAudio(44100);
        // Appending unlocked local audio on Track 1 (or after locked clips) succeeds!
        carta::Clip newClip;
        newClip.audio = local;
        newClip.start = 450560.0 / carta::sessionRate;
        newClip.offset = 0.0;
        newClip.duration = 1.0;
        newClip.name = "New";
        carta::Track newTr;
        newTr.name = "Overdub";
        newTr.clips.push_back(newClip);
        s.tracks.push_back(std::move(newTr));
        check(carta::preservesOnDiscAudio(session, s, lockErr),
              "preservesOnDiscAudio allows adding new unlocked audio tracks/clips: " +
                  lockErr.toStdString());

        // Auto-track & sync preservation
        carta::MdDisc d = s.mdDisc;
        std::string mdErr;
        check(carta::applyAutoTrackForPlacedClip(d, "NewClip.wav", 450560, 494660),
              "applyAutoTrackForPlacedClip appends track on loaded disc");
        check(d.tracks.size() == 3 && d.tracks[0].id == "od-t1" && d.tracks[1].id == "od-t2" &&
                  d.tracks[2].halfWidthTitle == "NewClip",
              "applyAutoTrackForPlacedClip preserves loaded on-disc tracks");

        carta::MdDisc synced = s.mdDisc;
        check(carta::syncTracksFromClips(s, synced, mdErr),
              "syncTracksFromClips on loaded disc succeeds: " + mdErr);
        check(synced.tracks.size() >= 3 && synced.tracks[0].id == "od-t1" &&
                  synced.tracks[1].id == "od-t2",
              "syncTracksFromClips preserves loaded on-disc tracks while adding unlocked clips");
    }

    // SessionIO v26 save/load roundtrip (plus v25 stays default for non-loaded projects)
    {
        const auto tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile("carta_loaded_disc_io_" +
                                              juce::String(juce::Time::getHighResolutionTicks()));
        check(tmpDir.createDirectory(), "create temp dir for v26 SessionIO test");
        struct Cleanup {
            juce::File d;
            ~Cleanup() { d.deleteRecursively(); }
        } cleanup{tmpDir};

        const auto v26File = tmpDir.getChildFile("loaded_disc.carta");
        juce::String ioErr;
        check(carta::saveSession(session, 2, v26File, ioErr),
              "saveSession v26 loaded disc failed: " + ioErr.toStdString());
        const auto parsedJson = juce::JSON::parse(v26File.loadFileAsString());
        check(static_cast<int>(parsedJson["version"]) == 26,
              "loaded disc session must serialize at project version 26");

        carta::Session reopened;
        int reopenedCh = 0;
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        check(carta::loadSession(reopened, reopenedCh, fm, v26File, ioErr),
              "loadSession v26 loaded disc failed: " + ioErr.toStdString());
        check(reopened.mdDisc == session.mdDisc && reopened.tracks.size() == 1 &&
                  reopened.tracks[0].clips.size() == 2 &&
                  reopened.tracks[0].clips[0].lockedOnDisc &&
                  reopened.tracks[0].clips[0].onDiscRegionId == "od-0" &&
                  reopened.tracks[0].clips[0].audio->onDiscPlaceholder &&
                  reopened.tracks[0].clips[0].audio->placeholderFrames == 90112,
              "loadSession v26 restores loadedDisc metadata, locked clips, and placeholder Audio "
              "identically");

        // Verify non-loaded project still writes v25
        carta::Session v25Session;
        v25Session.mdDisc.halfWidthTitle = "Plain";
        const auto wavFile = tmpDir.getChildFile("plain.wav");
        {
            juce::AudioFormatManager wfm;
            wfm.registerBasicFormats();
            if (auto* fmt = wfm.findFormatForFileExtension("wav")) {
                std::unique_ptr<juce::OutputStream> outStream(wavFile.createOutputStream());
                auto writer = fmt->createWriterFor(
                    outStream,
                    juce::AudioFormatWriterOptions{}
                        .withSampleRate(44100.0)
                        .withNumChannels(2)
                        .withBitsPerSample(16));
                if (writer) {
                    juce::AudioBuffer<float> buf(2, 1024);
                    buf.clear();
                    writer->writeFromAudioSampleBuffer(buf, 0, 1024);
                }
            }
        }
        auto aPlain = carta::loadAudio(fm, wavFile, ioErr);
        check(aPlain != nullptr, "loadAudio plain.wav failed: " + ioErr.toStdString());
        if (!aPlain) {
            return;
        }
        carta::Clip plainClip;
        plainClip.audio = aPlain;
        plainClip.start = 0.0;
        plainClip.offset = 0.0;
        plainClip.duration = 1024.0 / 44100.0;
        plainClip.name = "Plain";
        carta::Track plainTrack;
        plainTrack.name = "Layer 1";
        plainTrack.clips.push_back(plainClip);
        v25Session.tracks.push_back(std::move(plainTrack));
        const auto v25File = tmpDir.getChildFile("plain_v25.carta");
        check(carta::saveSession(v25Session, 2, v25File, ioErr),
              "saveSession non-loaded project failed: " + ioErr.toStdString());
        const auto parsedV25 = juce::JSON::parse(v25File.loadFileAsString());
        check(static_cast<int>(parsedV25["version"]) == 25,
              "non-loaded project must remain at project version 25");
    }
}

/** Verifies non-linear multi-part reordering, track cloning to position, and block moving. */
void testNonLinearPartAndBlockEdits() {
    std::string err;

    // 1. Part reordering inside a multi-part track (moveMdTrackPart)
    {
        carta::MdDisc disc;
        disc.medium = carta::MdMedium::md80;
        disc.defaultCodec = carta::MdCodecMode::spStereo;
        disc.halfWidthTitle = "PART REORDER";
        carta::MdTrack t;
        t.id = carta::makeMdTrackId();
        t.halfWidthTitle = "Medley";
        t.parts = {{0, 40000}, {40000, 80000}, {80000, 120000}};
        disc.tracks = {t};

        check(carta::moveMdTrackPart(disc, 0, 0, 2, err),
              "moveMdTrackPart moves P1 to P3 cleanly: " + err);
        check(disc.tracks[0].parts.size() == 3 && disc.tracks[0].parts[0].startSample == 40000 &&
                  disc.tracks[0].parts[1].startSample == 80000 &&
                  disc.tracks[0].parts[2].startSample == 0,
              "moveMdTrackPart preserves all part sample bounds in new playback order");
        check(!carta::moveMdTrackPart(disc, 0, -1, 0, err) && !err.empty(),
              "moveMdTrackPart rejects negative source index");
        check(!carta::moveMdTrackPart(disc, 0, 0, 3, err) && !err.empty(),
              "moveMdTrackPart rejects out-of-range destination index");
    }

    // 2. Clone track to explicit position shares physical audio with zero extra clusters
    {
        carta::MdDisc disc;
        disc.medium = carta::MdMedium::md80;
        disc.defaultCodec = carta::MdCodecMode::spStereo;
        disc.halfWidthTitle = "CLONE DISC";
        carta::MdTrack t0{carta::makeMdTrackId(), "Alpha", {}, {}, false, false, false, {},
                          {{0, 44100}}};
        carta::MdTrack t1{carta::makeMdTrackId(), "Beta", {}, {}, false, false, false, {},
                          {{44100, 88200}}};
        carta::MdTrack t2{carta::makeMdTrackId(), "Gamma", {}, {}, false, false, false, {},
                          {{88200, 132300}}};
        disc.tracks = {t0, t1, t2};
        disc.groups = {{1, 2, "Pair", {}}};

        carta::MdPhysicalPlan planBefore;
        check(carta::planMdPhysicalAudio(disc, planBefore, err),
              "Initial physical plan succeeds before cloneMdTrackToPosition: " + err);

        check(carta::cloneMdTrackToPosition(disc, 0, 2, err),
              "cloneMdTrackToPosition inserts shared clone at index 2: " + err);
        check(disc.tracks.size() == 4 && disc.tracks[2].id != disc.tracks[0].id &&
                  disc.tracks[2].halfWidthTitle == "Alpha" &&
                  disc.tracks[2].parts == disc.tracks[0].parts,
              "Cloned track receives a fresh ID while aliasing source parts verbatim");
        check(disc.groups.size() == 1 && disc.groups[0].startTrack == 1 &&
                  disc.groups[0].endTrack == 2,
              "Cloning outside group preserves group 1..2 boundaries");

        carta::MdPhysicalPlan planAfter;
        check(carta::planMdPhysicalAudio(disc, planAfter, err),
              "Physical plan succeeds after cloneMdTrackToPosition: " + err);
        check(planAfter.newUploadClusters == planBefore.newUploadClusters &&
                  planAfter.usedClusters == planBefore.usedClusters &&
                  planAfter.savedClusters > planBefore.savedClusters,
              "Cloned track costs 0 additional physical clusters and increases saved clusters");
    }

    // 3. Contiguous multi-track block move (moveMdTrackBlock) with group preservation
    {
        carta::MdDisc disc;
        disc.medium = carta::MdMedium::md80;
        disc.defaultCodec = carta::MdCodecMode::spStereo;
        disc.halfWidthTitle = "BLOCK MOVE";
        for (int i = 0; i < 5; ++i) {
            carta::MdTrack tr;
            tr.id = carta::makeMdTrackId();
            tr.halfWidthTitle = "T" + std::to_string(i + 1);
            tr.parts = {{static_cast<std::int64_t>(i) * 44100,
                         static_cast<std::int64_t>(i + 1) * 44100}};
            disc.tracks.push_back(std::move(tr));
        }
        disc.groups = {{1, 2, "Group 1", {}}, {3, 4, "Group 2", {}}};

        check(carta::moveMdTrackBlock(disc, 0, 1, 3, err),
              "moveMdTrackBlock moves tracks 1..2 after T5: " + err);
        check(disc.tracks[0].halfWidthTitle == "T3" && disc.tracks[1].halfWidthTitle == "T4" &&
                  disc.tracks[2].halfWidthTitle == "T5" && disc.tracks[3].halfWidthTitle == "T1" &&
                  disc.tracks[4].halfWidthTitle == "T2",
              "moveMdTrackBlock yields T3, T4, T5, T1, T2 order");
        check(disc.groups.size() == 2 && disc.groups[0].halfWidthTitle == "Group 2" &&
                  disc.groups[0].startTrack == 1 && disc.groups[0].endTrack == 2 &&
                  disc.groups[1].halfWidthTitle == "Group 1" && disc.groups[1].startTrack == 4 &&
                  disc.groups[1].endTrack == 5,
              "moveMdTrackBlock keeps intact groups contiguous and remaps track numbers cleanly");

        // Move back up as a contiguous block
        check(carta::moveMdTrackBlock(disc, 3, 4, 0, err),
              "moveMdTrackBlock moves block back to index 0: " + err);
        check(disc.tracks[0].halfWidthTitle == "T1" && disc.tracks[1].halfWidthTitle == "T2" &&
                  disc.groups.size() == 2 && disc.groups[0].halfWidthTitle == "Group 1" &&
                  disc.groups[0].startTrack == 1 && disc.groups[0].endTrack == 2,
              "Moving contiguous grouped block restores original track order and Group 1..2");
    }
}

} // namespace

int main() {
    testDefaultAndAutoTrack();
    testSyncFromLayerClipsAndMarkers();
    testGroupConsistencyAcrossTrackEdits();
    testPhysicalPlanCoalescingAndUtocRoundtrip();
    testInteractiveRibbonAndMultiLayerClipSync();
    testLoadedDiscLocksPlaceholderAndSessionIO();
    testNonLinearPartAndBlockEdits();
    if (gFailures == 0) {
        std::cout << "MdAuthoringTests passed.\n";
        return 0;
    }
    return 1;
}
