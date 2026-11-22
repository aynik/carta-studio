/**
 * @file NetMdTests.cpp
 * @brief End-to-end NetMD bridge tests against the out-of-process `carta-netmd` Mock MZ-N920.
 *
 * Verifies hardware confirmation guards, device discovery/probe/preflight,
 * Sound Group coalesced SP + LP2 upload parity, KillEepromWrite + ForceTOCEdit
 * UTOC roundtrip, disc erase safety, and 544 KB thread stack safety on ~20 MB
 * SP uploads across the real subprocess IPC boundary without touching physical
 * USB hardware.
 */
#include "MiniDisc/MdAuthoring.h"
#include "Editing/Editing.h"
#include "MiniDisc/NativeCodecs.h"
#include "MiniDisc/NetMdBridge.h"
#include "MiniDisc/Utoc.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

#define REQUIRE(cond, msg)                                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] " << (msg) << "\n";         \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

struct TestMockUploadedTrack {
    int trackIndex = -1;
    std::string mode;
    std::vector<std::uint8_t> decryptedAudioBytes;
    carta::UtocAddress startAddr{};
    carta::UtocAddress endAddr{};
};

struct TestMockSnapshot {
    int spUploadUsedCount = 0;
    bool safetyPatchApplied = false;
    int tocCommitCount = 0;
    int tocPollCount = 0;
    std::int64_t sleptMs = 0;
    std::vector<TestMockUploadedTrack> uploadedTracks;
    std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> committedUtoc{};
};

bool fetchMockSnapshot(TestMockSnapshot& out, std::string& error) {
    out = {};
    const auto tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("carta_netmd_test_snap_" +
                                          juce::String(juce::Time::getHighResolutionTicks()));
    if (!tmpDir.createDirectory()) {
        error = "Failed creating temporary snapshot directory";
        return false;
    }
    struct DirCleanup {
        juce::File dir;
        ~DirCleanup() { dir.deleteRecursively(); }
    } cleanup{tmpDir};

    std::string rawJson;
    if (!carta::fetchNetMdTestMockSnapshot(tmpDir.getFullPathName().toStdString(), rawJson,
                                           error)) {
        return false;
    }

    juce::var parsed;
    if (juce::JSON::parse(juce::String::fromUTF8(rawJson.c_str()), parsed).failed() ||
        !parsed.isObject()) {
        error = "Malformed mock-snapshot JSON from carta-netmd";
        return false;
    }
    const auto* obj = parsed.getDynamicObject();
    out.spUploadUsedCount = static_cast<int>(obj->getProperty("sp_upload_used_count"));
    out.safetyPatchApplied = static_cast<bool>(obj->getProperty("safety_patch_applied"));
    out.tocCommitCount = static_cast<int>(obj->getProperty("toc_commit_count"));
    out.tocPollCount = static_cast<int>(obj->getProperty("toc_poll_count"));
    out.sleptMs = static_cast<std::int64_t>(static_cast<juce::int64>(obj->getProperty("slept_ms")));

    juce::MemoryBlock utocData;
    if (!tmpDir.getChildFile("committed_utoc.bin").loadFileAsData(utocData) ||
        utocData.getSize() != 5 * carta::kUtocSectorBytes) {
        error = "Missing or invalid committed_utoc.bin in mock snapshot";
        return false;
    }
    const auto* uBytes = static_cast<const std::uint8_t*>(utocData.getData());
    for (std::size_t s = 0; s < 5; ++s) {
        std::memcpy(out.committedUtoc[s].data(), uBytes + s * carta::kUtocSectorBytes,
                    carta::kUtocSectorBytes);
    }

    if (const auto* arr = obj->getProperty("uploaded_tracks").getArray()) {
        out.uploadedTracks.reserve(static_cast<std::size_t>(arr->size()));
        for (int i = 0; i < arr->size(); ++i) {
            const auto* trObj = (*arr)[i].getDynamicObject();
            if (!trObj) {
                continue;
            }
            TestMockUploadedTrack tr;
            tr.trackIndex = static_cast<int>(trObj->getProperty("index"));
            tr.mode = trObj->getProperty("mode").toString().toStdString();
            tr.startAddr = carta::UtocAddress::fromLinearGroup(static_cast<std::int64_t>(
                static_cast<juce::int64>(trObj->getProperty("start_linear"))));
            tr.endAddr = carta::UtocAddress::fromLinearGroup(static_cast<std::int64_t>(
                static_cast<juce::int64>(trObj->getProperty("end_linear"))));
            juce::MemoryBlock trackData;
            if (!tmpDir.getChildFile("track_" + juce::String(i) + ".bin")
                     .loadFileAsData(trackData)) {
                error = "Missing track_" + std::to_string(i) + ".bin in mock snapshot";
                return false;
            }
            tr.decryptedAudioBytes.resize(trackData.getSize());
            if (trackData.getSize() > 0) {
                std::memcpy(tr.decryptedAudioBytes.data(), trackData.getData(),
                            trackData.getSize());
            }
            out.uploadedTracks.push_back(std::move(tr));
        }
    }
    return true;
}

/** Builds a 3-zone deterministic stereo `Session` spanning `3 * segLen` 44.1 kHz samples. */
carta::Session makeThreeRangeSession(std::int64_t segLen) {
    carta::Session session;
    auto audio = std::make_shared<carta::Audio>();
    audio->sampleRate = carta::sessionRate;
    const int totalSamples = static_cast<int>(segLen * 3);
    audio->samples.setSize(2, totalSamples);
    auto* L = audio->samples.getWritePointer(0);
    auto* R = audio->samples.getWritePointer(1);
    const double twoPi = 6.28318530717958647692;
    for (int i = 0; i < totalSamples; ++i) {
        const int zone = static_cast<int>(i / segLen);
        const double fL = zone == 0 ? 220.0 : zone == 1 ? 440.0 : 550.0;
        const double fR = zone == 0 ? 330.0 : zone == 1 ? 660.0 : 880.0;
        L[i] = 0.45f * static_cast<float>(
                           std::sin(twoPi * fL * static_cast<double>(i) / carta::sessionRate));
        R[i] = 0.40f * static_cast<float>(
                           std::sin(twoPi * fR * static_cast<double>(i) / carta::sessionRate));
    }
    carta::buildWaveformPeaks(*audio);
    carta::addImportedAudioTrack(session, audio, 0);
    return session;
}

/** Builds an `MdDisc` combining non-linear parts, shared ranges, groups, WP/SCMS flags, and LP2. */
carta::MdDisc makeComplexMdDisc(std::int64_t segLen) {
    carta::MdDisc d;
    d.medium = carta::MdMedium::md80;
    d.defaultCodec = carta::MdCodecMode::spStereo;
    d.halfWidthTitle = "CARTA MD";
    d.fullWidthTitle = "\xE3\x82\xAB\xE3\x83\xAB\xE3\x82\xBF\xE7\x9B\xA4"; // カルタ盤
    d.discTimestamp = "2026-11-15 04:00:00";

    const carta::MdPart p1{0, segLen};
    const carta::MdPart p2{segLen, segLen * 2};
    const carta::MdPart p3{segLen * 2, segLen * 3};

    carta::MdTrack t1;
    t1.id = "trk-1";
    t1.halfWidthTitle = "TRK ONE";
    t1.fullWidthTitle = "\xE7\xAC\xAC\xE4\xB8\x80\xE6\x9B\xB2"; // 第一曲
    t1.writeProtected = true;
    t1.scmsCopyProtected = true;
    t1.timestamp = "2026-11-15 04:01:02";
    t1.parts = {p2, p1}; // non-linear playback order across coalesced SP span

    carta::MdTrack t2;
    t2.id = "trk-2";
    t2.halfWidthTitle = "SHARED S1";
    t2.fullWidthTitle = "\xE5\x85\xB1\xE6\x9C\x89\xE6\x9B\xB2"; // 共有曲
    t2.parts = {p1}; // shared range with Track 1's second part

    carta::MdTrack t3;
    t3.id = "trk-3";
    t3.halfWidthTitle = "LP2 FINALE";
    t3.fullWidthTitle = "\xE6\x9C\x80\xE7\xB5\x82\xE6\x9B\xB2"; // 最終曲
    t3.codecOverride = carta::MdCodecMode::lp2;
    t3.scmsCopyProtected = true;
    t3.parts = {p3};

    d.tracks = {std::move(t1), std::move(t2), std::move(t3)};
    d.groups.push_back({1, 2, "Side A", ""});
    return d;
}

/** Renders and encodes the reference NetMD wire payload for `[startSample, endSample)`. */
bool expectedWireForRange(const carta::Session& session, std::int64_t startSample,
                          std::int64_t endSample, const std::string& mode,
                          std::vector<std::uint8_t>& outWire, std::string& error) {
    const auto* spec = carta::findAtracModeSpec(mode);
    if (!spec) {
        error = "Missing spec for " + mode;
        return false;
    }
    std::vector<float> left;
    std::vector<float> right;
    if (!carta::renderSessionMixRange(session, startSample, endSample, spec->channels, left, right,
                                      error)) {
        return false;
    }
    const std::size_t unpaddedSamples = left.size();
    std::size_t targetSize = unpaddedSamples;
    if (mode == "sp-stereo" || mode == "sp-mono") {
        const std::size_t pcmFrames = (unpaddedSamples + 511) / 512;
        const std::size_t remFrames = (pcmFrames + 1) % 11;
        targetSize = (pcmFrames + (remFrames == 0 ? 0 : (11 - remFrames))) * 512;
    } else {
        const std::size_t alignQuantum = (mode == "lp2") ? 2048 : 4096;
        if (unpaddedSamples % alignQuantum != 0) {
            targetSize = unpaddedSamples + (alignQuantum - (unpaddedSamples % alignQuantum));
        }
    }
    if (targetSize > unpaddedSamples) {
        left.resize(targetSize, 0.0f);
        if (!right.empty()) {
            right.resize(targetSize, 0.0f);
        }
    }
    std::vector<std::uint8_t> container;
    if (!carta::encodeAtracContainer(
            mode, left.data(), spec->channels == 2 ? right.data() : left.data(), spec->channels,
            static_cast<int>(left.size()), container, error)) {
        return false;
    }
    return carta::extractNetMdWirePayload(mode, container, outWire, error);
}

} // namespace

int main() {
    constexpr std::int64_t kSegLen = 44032; // exact multiple of 512 and 1024 (~0.998s)
    const auto session = makeThreeRangeSession(kSegLen);
    REQUIRE(carta::projectEndSample(session) == kSegLen * 3,
            "Expected 3-segment synthesized session");
    const auto disc = makeComplexMdDisc(kSegLen);

    carta::MdPhysicalPlan physPlan{};
    std::string planErr;
    REQUIRE(carta::planMdPhysicalAudio(disc, physPlan, planErr),
            "planMdPhysicalAudio failed: " + planErr);
    REQUIRE(physPlan.spans.size() == 2, "Expected 2 coalesced physical spans (1 SP + 1 LP2)");

    // 1. Hardware safety guard test via out-of-process `carta-netmd session --mock`
    std::string mockErr;
    REQUIRE(carta::setNetMdTestMockSessionEnabled(true, mockErr),
            "Failed starting carta-netmd mock session: " + mockErr);
    REQUIRE(carta::resetNetMdTestMockSession(mockErr), "Failed resetting mock session: " + mockErr);
    REQUIRE(carta::setNetMdTestMockGuard(true, mockErr), "Failed enabling mock guard: " + mockErr);
    {
        carta::UtocBudget budget{};
        std::string guardErr;
        const bool utocBlocked =
            !carta::writeNetMdUtoc(disc, "mock:mz-n920", false, budget, guardErr);
        REQUIRE(utocBlocked, "writeNetMdUtoc must block unconfirmed hardware write");
        REQUIRE(guardErr.find("confirmation") != std::string::npos,
                "Expected confirmation error on blocked UTOC write, got: " + guardErr);

        carta::NetMdRecordResult recRes{};
        guardErr.clear();
        const bool recBlocked =
            !carta::recordNetMdDisc(session, disc, "mock:mz-n920", false, recRes, guardErr);
        REQUIRE(recBlocked, "recordNetMdDisc must block unconfirmed hardware write");
        REQUIRE(guardErr.find("confirmation") != std::string::npos,
                "Expected confirmation error on blocked recording, got: " + guardErr);

        TestMockSnapshot snap{};
        REQUIRE(fetchMockSnapshot(snap, mockErr), "fetchMockSnapshot failed: " + mockErr);
        REQUIRE(snap.tocCommitCount == 0 && snap.uploadedTracks.empty(),
                "Blocked calls must not touch mock hardware");
    }
    REQUIRE(carta::setNetMdTestMockGuard(false, mockErr),
            "Failed disabling mock guard: " + mockErr);

    // 2. Discovery & device capability probe
    {
        std::vector<carta::NetMdDeviceEntry> devs;
        std::string err;
        REQUIRE(carta::listNetMdDevices(devs, err), "listNetMdDevices failed: " + err);
        bool foundMock = false;
        for (const auto& d : devs) {
            if (d.id == "mock:mz-n920" && d.isMock) {
                foundMock = true;
            }
        }
        REQUIRE(foundMock, "Expected mock:mz-n920 in NetMD device list");

        carta::NetMdDeviceStatus st{};
        REQUIRE(carta::probeNetMdDevice("mock:mz-n920", st, err),
                "probeNetMdDevice failed: " + err);
        REQUIRE(st.versionCode == "S1.600",
                "Expected mock MZ-N920 version S1.600, got: " + st.versionCode);
        REQUIRE(st.supportsSpUpload, "Expected supportsSpUpload=true");
        REQUIRE(st.supportsForceTocEdit, "Expected supportsForceTocEdit=true");
        REQUIRE(st.discPresent && st.writable && !st.writeProtected,
                "Expected writable inserted mock disc");
    }

    // 3. Preflight check & full coalesced / non-linear / shared-part / mixed-mode NetMD recording
    {
        const auto pre = carta::preflightNetMdRecording(session, disc, "mock:mz-n920");
        REQUIRE(pre.canRecord, "Preflight blocked unexpectedly: " + pre.blocker);
        REQUIRE(pre.uniqueUploadSegments == 2, "Expected 2 coalesced physical upload spans");
        REQUIRE(pre.logicalTracks == 3, "Expected 3 logical tracks");
        REQUIRE(pre.requiresSpUpload && pre.requiresForceTocEdit,
                "Expected SPUpload & ForceTOCEdit flags");

        carta::NetMdRecordResult rec{};
        std::string err;
        std::atomic<bool> pollerStarted{false};
        std::atomic<bool> pollerDone{false};
        std::atomic<bool> sawBytesExceedTotal{false};
        std::atomic<bool> sawEncodingMidProgress{false};
        std::atomic<bool> sawUploadByteProgress{false};
        std::thread poller([&] {
            pollerStarted.store(true, std::memory_order_release);
            while (!pollerDone.load(std::memory_order_acquire)) {
                const auto p = carta::getNetMdProgress();
                if (p.active) {
                    if (p.bytesTotal > 0 && p.bytesDone > p.bytesTotal) {
                        sawBytesExceedTotal.store(true, std::memory_order_relaxed);
                    }
                    if (p.phase.find("Encoding ") == 0 && p.progress > 0.025 && p.progress < 0.31) {
                        sawEncodingMidProgress.store(true, std::memory_order_relaxed);
                    }
                    if (p.bytesTotal > 0 && p.bytesDone > 0) {
                        sawUploadByteProgress.store(true, std::memory_order_relaxed);
                    }
                }
                std::this_thread::yield();
            }
        });
        while (!pollerStarted.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const bool recOk = carta::recordNetMdDisc(session, disc, "mock:mz-n920", false, rec, err);
        pollerDone.store(true, std::memory_order_release);
        poller.join();
        REQUIRE(recOk, "recordNetMdDisc failed: " + err);
        REQUIRE(!sawBytesExceedTotal.load(),
                "Upload progress bytesDone must never exceed bytesTotal");
        REQUIRE(sawEncodingMidProgress.load(),
                "Encoding progress must advance smoothly past 2% while encoding spans");
        REQUIRE(sawUploadByteProgress.load(),
                "USB bulk upload progress must report non-zero bytesDone <= bytesTotal");
        REQUIRE(rec.uploadedPhysicalSegments == 2,
                "Expected 2 physical uploads (coalesced SP + LP2)");
        REQUIRE(rec.logicalTracks == 3, "Expected 3 UTOC tracks");
        REQUIRE(rec.groups == 1, "Expected 1 group");
        REQUIRE(rec.committedUtoc, "Expected committedUtoc=true");

        TestMockSnapshot snap{};
        REQUIRE(fetchMockSnapshot(snap, err), "fetchMockSnapshot failed: " + err);
        REQUIRE(snap.uploadedTracks.size() == 2,
                "Expected 2 coalesced spans uploaded to mock hardware");
        REQUIRE(snap.spUploadUsedCount == 1, "Expected SPUpload used once for coalesced SP span");
        REQUIRE(snap.safetyPatchApplied,
                "Expected KillEepromWrite safety patch applied on SP upload");
        REQUIRE(snap.tocCommitCount == 1, "Expected 1 ForceTOCEdit UTOC commit");
        REQUIRE(snap.tocPollCount == 4,
                "Expected 4 ForceTOCEdit completion polls (2 busy + 2 idle), got: " +
                    std::to_string(snap.tocPollCount));
        REQUIRE(snap.sleptMs > 0 && snap.sleptMs < 15000,
                "Expected ForceTOCEdit polling to finish well under 50s fallback, sleptMs=" +
                    std::to_string(snap.sleptMs));

        for (std::size_t u = 0; u < physPlan.spans.size(); ++u) {
            const auto& span = physPlan.spans[u];
            const std::string mode = carta::mdCodecModeToString(span.codec);
            const bool isSp = (span.codec == carta::MdCodecMode::spStereo ||
                               span.codec == carta::MdCodecMode::spMono);
            std::vector<std::uint8_t> refWire;
            REQUIRE(
                expectedWireForRange(session, span.startSample, span.endSample, mode, refWire, err),
                "Failed generating reference wire payload: " + err);
            const auto& uploaded = snap.uploadedTracks[u].decryptedAudioBytes;
            REQUIRE(uploaded.size() == refWire.size(),
                    "Uploaded byte count mismatch at physical span " + std::to_string(u));
            if (!isSp) {
                REQUIRE(uploaded == refWire, "LP2 wire payload byte mismatch");
            } else {
                REQUIRE(uploaded.size() % 212 == 0,
                        "Expected SP uploaded payload in 212-byte sound groups");
                REQUIRE(uploaded.size() % 2332 == 0,
                        "Expected SP uploaded payload aligned to 2332-byte physical sectors");
                for (std::size_t off = 0; off < uploaded.size(); off += 212) {
                    for (std::size_t k = 0; k < 210; ++k) {
                        REQUIRE(uploaded[off + k] == refWire[off + k],
                                "SP ATRAC1 body byte mismatch");
                    }
                    REQUIRE(uploaded[off + 210] == refWire[off + 1] &&
                                uploaded[off + 211] == refWire[off + 0],
                            "SPUpload sound group footer [210, 211] mismatch");
                }
            }
        }

        // Parse committed UTOC Sectors 0/1/2/4 and validate full MD semantics.
        carta::UtocDiscState parsed{};
        REQUIRE(carta::parseUtocSectors(snap.committedUtoc[0].data(), snap.committedUtoc[1].data(),
                                        snap.committedUtoc[2].data(), snap.committedUtoc[4].data(),
                                        parsed, err),
                "parseUtocSectors failed: " + err);
        REQUIRE(parsed.trackCount == 3, "Expected 3 UTOC tracks");

        const auto budget = carta::computeUtocBudget(parsed, "md80");
        REQUIRE(budget.sharedFragmentCount >= 1, "Expected shared UTOC fragment for p1");

        const auto f1 = parsed.trackMap[1];
        REQUIRE(f1 != 0 && parsed.fragments[f1].link != 0,
                "Track 1 must chain 2 non-linear fragments");
        const auto f1Second = parsed.fragments[f1].link;
        const auto f2 = parsed.trackMap[2];
        REQUIRE(f2 != 0 && parsed.fragments[f2].link == 0, "Track 2 must have 1 fragment");

        const std::int64_t midSpanLinear = snap.uploadedTracks[0].startAddr.toLinearGroup() +
                                           physPlan.placements[0].unitOffsetInSpan;
        REQUIRE(parsed.fragments[f1].start.toLinearGroup() == midSpanLinear,
                "Track 1 fragment 1 must map to p2 upper subrange inside coalesced SP span 0");
        REQUIRE(parsed.fragments[f1Second].start == snap.uploadedTracks[0].startAddr &&
                    parsed.fragments[f1Second].end.toLinearGroup() == midSpanLinear - 1,
                "Track 1 fragment 2 must map to p1 lower subrange inside coalesced SP span 0");
        REQUIRE(parsed.fragments[f1].start.toLinearGroup() >
                    parsed.fragments[f1Second].start.toLinearGroup(),
                "Track 1 must reference p2 after p1 physically (non-linear order)");
        REQUIRE(parsed.fragments[f1Second].start == parsed.fragments[f2].start &&
                    parsed.fragments[f1Second].end == parsed.fragments[f2].end,
                "Track 1's second fragment and Track 2 must share p1 physical sound group range");

        const auto m1 = parsed.fragments[f1].mode;
        REQUIRE((m1 & carta::kUtocSpMode) != 0 && (m1 & carta::kUtocStereo) != 0,
                "Track 1 mode must be SP stereo");
        REQUIRE((m1 & carta::kUtocWritable) == 0, "Track 1 WP flag must clear writable bit");
        REQUIRE((m1 & carta::kUtocScmsUnrestricted) == 0,
                "Track 1 SCMS flag must restrict digital copy");

        const auto f3 = parsed.trackMap[3];
        REQUIRE(f3 != 0, "Track 3 fragment slot must be non-zero");
        REQUIRE(carta::audioModeFromUtocMode(parsed.fragments[f3].mode) == "lp2",
                "Track 3 mode must round-trip as lp2");

        carta::MdDisc rt{};
        REQUIRE(carta::mdDiscFromUtoc(parsed, rt, err), "mdDiscFromUtoc failed: " + err);
        REQUIRE(rt.tracks.size() == 3, "Expected 3 round-tripped tracks");
        REQUIRE(rt.fullWidthTitle == "\xE3\x82\xAB\xE3\x83\xAB\xE3\x82\xBF\xE7\x9B\xA4",
                "Disc Shift-JIS title mismatch: " + rt.fullWidthTitle);
        REQUIRE(rt.tracks[0].fullWidthTitle == "\xE7\xAC\xAC\xE4\xB8\x80\xE6\x9B\xB2",
                "Track 1 FW title mismatch");
        REQUIRE(rt.tracks[1].fullWidthTitle == "\xE5\x85\xB1\xE6\x9C\x89\xE6\x9B\xB2",
                "Track 2 FW title mismatch");
        REQUIRE(rt.tracks[2].fullWidthTitle == "\xE6\x9C\x80\xE7\xB5\x82\xE6\x9B\xB2",
                "Track 3 FW title mismatch");
        REQUIRE(rt.discTimestamp == "2026-11-15 04:00:00", "Disc Sector 2 timestamp mismatch");
        REQUIRE(rt.tracks[0].timestamp == "2026-11-15 04:01:02",
                "Track 1 Sector 2 timestamp mismatch");
        REQUIRE(rt.tracks[0].parts.size() == 2 && rt.tracks[1].parts.size() == 1 &&
                    rt.tracks[0].parts[1] == rt.tracks[1].parts[0],
                "Reconstructed parts preserve shared p1 range");
        REQUIRE(rt.groups.size() == 1 && rt.groups[0].halfWidthTitle == "Side A" &&
                    rt.groups[0].startTrack == 1 && rt.groups[0].endTrack == 2,
                "Expected Side A group covering tracks 1-2");

        // Read back UTOC from mock without modifying disc
        std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> readRaw{};
        carta::UtocDiscState readUtoc{};
        REQUIRE(carta::readNetMdDiscUtoc("mock:mz-n920", readRaw, readUtoc, err),
                "readNetMdDiscUtoc failed: " + err);
        carta::MdDisc readRt{};
        REQUIRE(carta::mdDiscFromUtoc(readUtoc, readRt, err),
                "mdDiscFromUtoc on readNetMdDiscUtoc failed: " + err);
        REQUIRE(readUtoc.trackCount == 3 && readRt.halfWidthTitle == "CARTA MD" &&
                    readRt.groups.size() == 1 && readRt.groups[0].halfWidthTitle == "Side A",
                "readNetMdDiscUtoc must match recorded disc state");

        // Verify probeNetMdDevice reports the on-disc uploaded spans cleanly via NetMD AVC.
        carta::NetMdDeviceStatus probedAfterRec{};
        REQUIRE(carta::probeNetMdDevice("mock:mz-n920", probedAfterRec, err),
                "probeNetMdDevice after record failed: " + err);
        REQUIRE(probedAfterRec.tracks.size() == 2 && probedAfterRec.tracks[0].codec == "SP" &&
                    probedAfterRec.tracks[1].codec == "LP2" &&
                    !probedAfterRec.tracks[0].title.empty() &&
                    probedAfterRec.tracks[0].durationSeconds > 0,
                "probeNetMdDevice must return uploaded tracks with titles, codecs, and durations");

        // Compare streaming encodeAtracNetMdWire against container extraction for all NetMD modes.
        for (const std::string& mode : {"sp-stereo", "sp-mono", "lp2", "lp4"}) {
            std::vector<std::uint8_t> containerWire;
            REQUIRE(expectedWireForRange(session, 0, kSegLen, mode, containerWire, err),
                    "Container wire reference encode failed for " + mode + ": " + err);

            const auto* spec = carta::findAtracModeSpec(mode);
            REQUIRE(spec != nullptr, "Missing spec for " + mode);
            std::vector<float> left;
            std::vector<float> right;
            REQUIRE(
                carta::renderSessionMixRange(session, 0, kSegLen, spec->channels, left, right, err),
                "Render failed for " + mode + ": " + err);
            const std::size_t unpadded = left.size();
            std::size_t targetSize = unpadded;
            if (mode == "sp-stereo" || mode == "sp-mono") {
                const std::size_t pcmFrames = (unpadded + 511u) / 512u;
                const std::size_t remFrames = (pcmFrames + 1u) % 11u;
                targetSize = (pcmFrames + (remFrames == 0 ? 0 : (11u - remFrames))) * 512u;
            } else {
                const std::size_t alignQuantum = (mode == "lp2") ? 2048u : 4096u;
                if (unpadded % alignQuantum != 0) {
                    targetSize = unpadded + (alignQuantum - (unpadded % alignQuantum));
                }
            }
            if (targetSize > unpadded) {
                left.resize(targetSize, 0.0f);
                if (!right.empty()) {
                    right.resize(targetSize, 0.0f);
                }
            }

            std::vector<std::uint8_t> streamedWire;
            int callbackCount = 0;
            int lastDoneFrames = -1;
            int lastTotalFrames = 0;
            REQUIRE(carta::encodeAtracNetMdWire(
                        mode, left.data(), spec->channels == 2 ? right.data() : nullptr,
                        spec->channels, static_cast<int>(left.size()), streamedWire,
                        [&](int done, int total) {
                            ++callbackCount;
                            if (done < lastDoneFrames || done > total) {
                                lastDoneFrames = -999;
                            } else {
                                lastDoneFrames = done;
                            }
                            lastTotalFrames = total;
                        },
                        err),
                    "encodeAtracNetMdWire failed for " + mode + ": " + err);
            REQUIRE(callbackCount >= 2 && lastDoneFrames == lastTotalFrames && lastTotalFrames > 0,
                    "Expected monotonic frame progress callbacks for " + mode);
            REQUIRE(streamedWire == containerWire,
                    "Streaming wire bytes must match container wire bytes bit-for-bit in " + mode);
        }

        // Hardware confirmation guard + eraseNetMdDisc
        REQUIRE(carta::setNetMdTestMockGuard(true, mockErr),
                "Failed enabling mock guard for erase: " + mockErr);
        std::string eraseGuardErr;
        carta::NetMdDeviceStatus afterSt{};
        REQUIRE(!carta::eraseNetMdDisc("mock:mz-n920", false, afterSt, eraseGuardErr),
                "eraseNetMdDisc must reject confirmHardware=false");
        REQUIRE(carta::eraseNetMdDisc("mock:mz-n920", true, afterSt, err),
                "eraseNetMdDisc confirmHardware=true failed: " + err);
        REQUIRE(afterSt.trackCount == 0 && afterSt.writable && !afterSt.hiMdFormatted,
                "Erased mock disc must be blank and writable");
        REQUIRE(carta::setNetMdTestMockGuard(false, mockErr),
                "Failed disabling mock guard: " + mockErr);
    }

    // 3B. Non-destructive Load Disc -> Edit -> Write UTOC / Record / Pre-upload Reclaim + 2-step
    {
        REQUIRE(carta::resetNetMdTestMockSession(mockErr),
                "Failed resetting mock session before Stage 1 loaded-disc tests: " + mockErr);

        auto buildSeedUtoc = [&](std::uint16_t freeStartCluster, std::uint16_t freeEndCluster,
                                 std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5>& outRaw) {
            for (auto& s : outRaw) {
                s.fill(0);
            }
            for (std::size_t i = 0; i < carta::kUtocSectorBytes; ++i) {
                outRaw[3][i] = static_cast<std::uint8_t>((i * 19u + 7u) & 0xFFu);
            }
            carta::UtocDiscState st{};
            st.hasSector0 = true;
            st.hasSector1 = true;
            st.hasSector2 = true;
            st.hasSector4 = true;
            st.deviceSignature = 0x534F;
            st.discNonEmpty = 1;
            st.trackCount = 2;
            st.timestamps[1] = {26, 11, 20, 14, 0, 0, 0x534F};
            st.timestampMap[0] = 1;
            st.nextFreeTimestampSlot = 2;
            for (std::uint8_t i = 1; i < 255; ++i) {
                st.fragments[i].link = static_cast<std::uint8_t>(i + 1);
            }
            st.fragments[255].link = 0;
            st.nextFreeTrackSlot = 1;

            auto allocF = [&](carta::UtocAddress a, carta::UtocAddress b, std::uint8_t mode) {
                const std::uint8_t slot = st.nextFreeTrackSlot;
                st.nextFreeTrackSlot = st.fragments[slot].link;
                st.fragments[slot].start = a;
                st.fragments[slot].end = b;
                st.fragments[slot].mode = mode;
                st.fragments[slot].link = 0;
                return slot;
            };
            const std::uint8_t spMode = static_cast<std::uint8_t>(
                carta::kUtocSpMode | carta::kUtocStereo | carta::kUtocWritable |
                carta::kUtocScmsUnrestricted | carta::kUtocReservedCodec);
            const std::uint8_t lp2Mode = carta::utocModeFromTrack("lp2", false, false, false);

            // Track 1: clusters 50..500 (large, so deleting frees 451 clusters!), Track 2: 501..600 (odd SP)
            st.trackMap[1] = allocF({50, 0, 0}, {500, 31, 10}, lp2Mode);
            st.trackMap[2] = allocF({501, 0, 0}, {600, 2, 0}, spMode);
            if (freeEndCluster >= freeStartCluster) {
                st.trackMap[0] = allocF({freeStartCluster, 0, 0}, {freeEndCluster, 31, 10}, 0);
            }
            st.sec0aPadding[14] = allocF({610, 0, 0}, {610, 31, 10}, 0); // P-DFA

            auto setHw = [&](std::size_t idx, const std::string& txt) {
                const std::uint8_t slot = static_cast<std::uint8_t>(idx + 1);
                st.halfTitleCells[slot].bytes.fill(0);
                for (std::size_t j = 0; j < 7 && j < txt.size(); ++j) {
                    st.halfTitleCells[slot].bytes[j] = static_cast<std::uint8_t>(txt[j]);
                }
                st.halfTitleCells[slot].link = 0;
                st.halfTitleMap[idx] = slot;
            };
            setHw(0, "ORIG MD");
            setHw(1, "OrigT1");
            setHw(2, "OrigT2");
            st.nextFreeHalfTitleSlot = 4;

            std::string compErr;
            return carta::compileUtocSectors(st, outRaw[0], outRaw[1], outRaw[2], outRaw[4], compErr);
        };

        // 1) Unloaded non-empty disc refusal (never auto-erase)
        std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> seedRaw{};
        REQUIRE(buildSeedUtoc(650, 2251, seedRaw), "failed building seed UTOC");
        REQUIRE(carta::seedNetMdTestMockUtoc(seedRaw, mockErr),
                "seedNetMdTestMockUtoc failed: " + mockErr);

        constexpr std::int64_t kNewLen = 44100LL * 4;
        carta::Session freshLocalSession = makeThreeRangeSession(kNewLen);
        carta::MdDisc unloadedDisc;
        unloadedDisc.medium = carta::MdMedium::md80;
        unloadedDisc.tracks = {
            {carta::makeMdTrackId(), "Local Only", {}, carta::MdCodecMode::lp2, false, false, false,
             {}, {{0, kNewLen}}}};
        carta::NetMdRecordResult refusedRes{};
        std::string refuseErr;
        REQUIRE(!carta::recordNetMdDisc(freshLocalSession, unloadedDisc, "mock:mz-n920", true,
                                        refusedRes, refuseErr),
                "recordNetMdDisc MUST refuse overwriting non-empty unloaded disc without erase");
        REQUIRE(refuseErr.find("not empty") != std::string::npos,
                "Expected non-empty refusal message, got: " + refuseErr);

        // 2) Load + UTOC-only edit + Write UTOC + Record (Subpath 2A) preserving Sector 3
        carta::MdDisc loadedDisc{};
        carta::NetMdDeviceStatus loadStatus{};
        REQUIRE(carta::loadNetMdDisc("mock:mz-n920", loadedDisc, &loadStatus, mockErr),
                "loadNetMdDisc failed: " + mockErr);
        REQUIRE(loadedDisc.loadedDisc.active && loadedDisc.tracks.size() == 2,
                "Expected 2 loaded tracks");

        loadedDisc.tracks[1].halfWidthTitle = "KeptT2";
        loadedDisc.tracks[1].writeProtected = true;
        loadedDisc.tracks.erase(loadedDisc.tracks.begin()); // delete OrigT1 -> reclaim 50..500

        carta::UtocBudget writeBudget{};
        std::string committedHash;
        std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> committedRaw{};
        REQUIRE(carta::writeNetMdUtoc(loadedDisc, "mock:mz-n920", true, writeBudget, mockErr,
                                      &committedHash, &committedRaw),
                "writeNetMdUtoc on loaded disc failed: " + mockErr);
        REQUIRE(!committedHash.empty() && committedRaw[3] == seedRaw[3],
                "writeNetMdUtoc must return hash and preserve Sector 3 verbatim");

        // Update baseline hash + P-FRA after write, then also verify Subpath 2A in recordNetMdDisc
        loadedDisc.loadedDisc.baselineSector0Hash = committedHash;
        carta::Session loadedOnlySession;
        carta::populateLoadedDiscSession(loadedDisc, loadedOnlySession);
        carta::NetMdRecordResult utocOnlyRes{};
        REQUIRE(carta::recordNetMdDisc(loadedOnlySession, loadedDisc, "mock:mz-n920", true,
                                       utocOnlyRes, mockErr),
                "recordNetMdDisc Subpath 2A failed: " + mockErr);
        REQUIRE(utocOnlyRes.uploadedPhysicalSegments == 0 && utocOnlyRes.committedUtoc &&
                    utocOnlyRes.preservedOnDiscTracks == 1 && utocOnlyRes.reclaimedClusters == 451 &&
                    !utocOnlyRes.needsReloadToFinishUpload &&
                    utocOnlyRes.committedRawSectors[3] == seedRaw[3],
                "Subpath 2A must preserve Sector 3, report 0 uploads and 451 reclaimed clusters");

        // 3) Disc-swap SHA-256 guard
        std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> swappedRaw{};
        REQUIRE(buildSeedUtoc(700, 2251, swappedRaw), "failed building swapped UTOC");
        REQUIRE(carta::seedNetMdTestMockUtoc(swappedRaw, mockErr),
                "seedNetMdTestMockUtoc (swap) failed: " + mockErr);
        std::string swapErr;
        REQUIRE(!carta::writeNetMdUtoc(loadedDisc, "mock:mz-n920", true, writeBudget, swapErr),
                "writeNetMdUtoc must block after disc-swap SHA-256 mismatch");
        REQUIRE(swapErr.find("changed") != std::string::npos,
                "Expected disc-swap SHA-256 mismatch diagnostic, got: " + swapErr);

        // 4) Non-destructive single-step append (Subpath 2B when free P-FRA suffices)
        REQUIRE(carta::seedNetMdTestMockUtoc(seedRaw, mockErr),
                "re-seed for single-step append failed: " + mockErr);
        carta::MdDisc appendDisc{};
        REQUIRE(carta::loadNetMdDisc("mock:mz-n920", appendDisc, nullptr, mockErr),
                "loadNetMdDisc for append failed: " + mockErr);
        carta::Session appendSession;
        carta::populateLoadedDiscSession(appendDisc, appendSession);
        const std::int64_t appendStart = appendDisc.loadedDisc.regions.back().timelineEndSample;
        auto appendAudio = std::make_shared<carta::Audio>();
        appendAudio->sampleRate = carta::sessionRate;
        appendAudio->samples.setSize(2, static_cast<int>(kNewLen));
        appendAudio->samples.clear();
        for (int i = 0; i < static_cast<int>(kNewLen); ++i) {
            appendAudio->samples.setSample(0, i, static_cast<float>(0.25 * std::sin(0.05 * i)));
            appendAudio->samples.setSample(1, i, static_cast<float>(0.25 * std::sin(0.07 * i)));
        }
        {
            carta::Clip c;
            c.audio = appendAudio;
            c.start = static_cast<double>(appendStart) / carta::sessionRate;
            c.offset = 0.0;
            c.duration = static_cast<double>(kNewLen) / carta::sessionRate;
            c.name = "BonusLP2";
            carta::Track tr;
            tr.name = "New Layer";
            tr.clips.push_back(std::move(c));
            appendSession.tracks.push_back(std::move(tr));
        }
        appendDisc.tracks.push_back({carta::makeMdTrackId(),
                                     "Appended LP2",
                                     {},
                                     carta::MdCodecMode::lp2,
                                     false,
                                     false,
                                     false,
                                     {},
                                     {{appendStart, appendStart + kNewLen}}});

        carta::NetMdRecordResult appendRes{};
        REQUIRE(carta::recordNetMdDisc(appendSession, appendDisc, "mock:mz-n920", true, appendRes,
                                       mockErr),
                "Subpath 2B single-step non-destructive append failed: " + mockErr);
        REQUIRE(appendRes.uploadedPhysicalSegments == 1 && appendRes.preservedOnDiscTracks == 2 &&
                    !appendRes.needsReloadToFinishUpload && appendRes.logicalTracks == 3 &&
                    appendRes.committedRawSectors[3] == seedRaw[3],
                "Single-step append must upload 1 new span, keep 2 on-disc tracks + Sector 3");

        TestMockSnapshot appendSnap{};
        REQUIRE(fetchMockSnapshot(appendSnap, mockErr), "fetchMockSnapshot failed: " + mockErr);
        carta::UtocDiscState appendUtoc{};
        REQUIRE(carta::parseUtocSectors(appendSnap.committedUtoc[0].data(),
                                        appendSnap.committedUtoc[1].data(),
                                        appendSnap.committedUtoc[2].data(),
                                        appendSnap.committedUtoc[4].data(), appendUtoc, mockErr),
                "parse committed appended UTOC failed: " + mockErr);
        const bool combinedPreserved =
            appendUtoc.trackCount == 3 &&
            appendUtoc.fragments[appendUtoc.trackMap[1]].start == carta::UtocAddress{50, 0, 0} &&
            appendUtoc.fragments[appendUtoc.trackMap[2]].end == carta::UtocAddress{600, 2, 0};
        REQUIRE(combinedPreserved,
                "Combined UTOC must preserve existing on-disc tracks verbatim alongside appended "
                "track");

        // 5) Pre-upload reclaim + 2-step reload (seed full disc: only 1 whole free cluster in P-FRA,
        //    delete Track 1 freeing 451 clusters, append 2-cluster LP2 track)
        std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> tightRaw{};
        REQUIRE(buildSeedUtoc(2250, 2250, tightRaw), "failed building tight free-space UTOC");
        REQUIRE(carta::seedNetMdTestMockUtoc(tightRaw, mockErr),
                "seed tight UTOC failed: " + mockErr);
        carta::MdDisc twoStepDisc{};
        REQUIRE(carta::loadNetMdDisc("mock:mz-n920", twoStepDisc, nullptr, mockErr),
                "load tight UTOC failed: " + mockErr);
        // Delete Track 1 (50..500 = 451 clusters) while keeping Track 2 (501..600), and append 1 LP2 track (~2 cl)
        twoStepDisc.tracks.erase(twoStepDisc.tracks.begin());
        carta::Session twoStepSession;
        carta::populateLoadedDiscSession(twoStepDisc, twoStepSession);
        const std::int64_t tsEnd = twoStepDisc.loadedDisc.regions.back().timelineEndSample;
        constexpr std::int64_t kLongNewLen = 44100LL * 45; // > 1 cluster in LP2
        auto longAudio = std::make_shared<carta::Audio>();
        longAudio->sampleRate = carta::sessionRate;
        longAudio->samples.setSize(2, static_cast<int>(kLongNewLen));
        longAudio->samples.clear();
        for (int i = 0; i < static_cast<int>(kLongNewLen); ++i) {
            longAudio->samples.setSample(0, i, static_cast<float>(0.2 * std::sin(0.03 * i)));
            longAudio->samples.setSample(1, i, static_cast<float>(0.2 * std::cos(0.03 * i)));
        }
        {
            carta::Clip c;
            c.audio = longAudio;
            c.start = static_cast<double>(tsEnd) / carta::sessionRate;
            c.offset = 0.0;
            c.duration = static_cast<double>(kLongNewLen) / carta::sessionRate;
            c.name = "TwoStepLP2";
            carta::Track tr;
            tr.name = "New Layer";
            tr.clips.push_back(std::move(c));
            twoStepSession.tracks.push_back(std::move(tr));
        }
        twoStepDisc.tracks.push_back({carta::makeMdTrackId(),
                                      "TwoStep LP2",
                                      {},
                                      carta::MdCodecMode::lp2,
                                      false,
                                      false,
                                      false,
                                      {},
                                      {{tsEnd, tsEnd + kLongNewLen}}});

        // Pass 1: commits reclaimUtoc and returns needsReloadToFinishUpload=true
        carta::NetMdRecordResult step1{};
        REQUIRE(carta::recordNetMdDisc(twoStepSession, twoStepDisc, "mock:mz-n920", true, step1,
                                       mockErr),
                "2-step record Pass 1 failed: " + mockErr);
        REQUIRE(step1.needsReloadToFinishUpload && step1.uploadedPhysicalSegments == 0 &&
                    step1.committedUtoc && step1.reclaimedClusters == 451,
                "Pass 1 must reclaim clusters into P-FRA and request disc reload");

        // Simulate eject & reinsert with the reclaimed UTOC committed in Pass 1
        REQUIRE(carta::seedNetMdTestMockUtoc(step1.committedRawSectors, mockErr),
                "Mock disc reload after Pass 1 failed: " + mockErr);
        twoStepDisc.loadedDisc.baselineSector0Hash = step1.committedSector0Hash;
        carta::UtocDiscState reloadedUtoc{};
        REQUIRE(carta::parseUtocSectors(step1.committedRawSectors[0].data(),
                                        step1.committedRawSectors[1].data(),
                                        step1.committedRawSectors[2].data(),
                                        step1.committedRawSectors[4].data(), reloadedUtoc, mockErr),
                "parse committed reclaim UTOC failed: " + mockErr);
        twoStepDisc.loadedDisc.initialFreeRanges.clear();
        for (std::uint8_t cur = reloadedUtoc.trackMap[0]; cur != 0;
             cur = reloadedUtoc.fragments[cur].link) {
            const auto& f = reloadedUtoc.fragments[cur];
            twoStepDisc.loadedDisc.initialFreeRanges.push_back(
                {{f.start.cluster, f.start.sector, f.start.group},
                 {f.end.cluster, f.end.sector, f.end.group}});
        }

        // Pass 2: now succeeds in uploading into reclaimed space and commits final combined UTOC!
        carta::NetMdRecordResult step2{};
        REQUIRE(carta::recordNetMdDisc(twoStepSession, twoStepDisc, "mock:mz-n920", true, step2,
                                       mockErr),
                "2-step record Pass 2 failed: " + mockErr);
        REQUIRE(!step2.needsReloadToFinishUpload && step2.uploadedPhysicalSegments == 1 &&
                    step2.committedUtoc && step2.logicalTracks == 2 &&
                    step2.committedRawSectors[3] == tightRaw[3],
                "Pass 2 must upload new track into reclaimed space and preserve Track 2 + Sector 3");

        // 6) Large-disc (>40 track) fast probe skips AVC track loop, while loadNetMdDisc
        //    populates all 50 track metadata entries from UTOC and preflightWithStatus succeeds
        //    with zero extra USB queries.
        {
            carta::UtocDiscState st50{};
            st50.hasSector0 = true;
            st50.hasSector1 = true;
            st50.hasSector2 = true;
            st50.hasSector4 = true;
            st50.deviceSignature = 0x534F;
            st50.discNonEmpty = 1;
            st50.trackCount = 50;
            st50.timestamps[1] = {26, 11, 20, 14, 0, 0, 0x534F};
            st50.timestampMap[0] = 1;
            st50.nextFreeTimestampSlot = 2;
            for (std::uint8_t i = 1; i < 255; ++i) {
                st50.fragments[i].link = static_cast<std::uint8_t>(i + 1);
            }
            st50.fragments[255].link = 0;
            st50.nextFreeTrackSlot = 1;

            auto allocFrag50 = [&](carta::UtocAddress a, carta::UtocAddress b, std::uint8_t mode) {
                const std::uint8_t slot = st50.nextFreeTrackSlot;
                st50.nextFreeTrackSlot = st50.fragments[slot].link;
                st50.fragments[slot].start = a;
                st50.fragments[slot].end = b;
                st50.fragments[slot].mode = mode;
                st50.fragments[slot].link = 0;
                return slot;
            };
            const std::uint8_t spMode = static_cast<std::uint8_t>(
                carta::kUtocSpMode | carta::kUtocStereo | carta::kUtocWritable |
                carta::kUtocScmsUnrestricted | carta::kUtocReservedCodec);
            for (int t = 0; t < 50; ++t) {
                const auto cl = static_cast<std::uint16_t>(50 + t);
                st50.trackMap[static_cast<std::size_t>(t + 1)] =
                    allocFrag50({cl, 0, 0}, {cl, 31, 10}, spMode);
            }
            st50.trackMap[0] = allocFrag50({100, 0, 0}, {2251, 31, 10}, 0);

            auto setHw50 = [&](std::size_t idx, const std::string& txt) {
                const std::uint8_t slot = static_cast<std::uint8_t>(idx + 1);
                st50.halfTitleCells[slot].bytes.fill(0);
                for (std::size_t j = 0; j < 7 && j < txt.size(); ++j) {
                    st50.halfTitleCells[slot].bytes[j] = static_cast<std::uint8_t>(txt[j]);
                }
                st50.halfTitleCells[slot].link = 0;
                st50.halfTitleMap[idx] = slot;
            };
            setHw50(0, "50TRKMD");
            for (int t = 0; t < 50; ++t) {
                setHw50(static_cast<std::size_t>(t + 1), "T" + std::to_string(t + 1));
            }
            st50.nextFreeHalfTitleSlot = 52;

            std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> raw50{};
            std::string compErr50;
            REQUIRE(carta::compileUtocSectors(st50, raw50[0], raw50[1], raw50[2], raw50[4],
                                              compErr50),
                    "compile 50-track UTOC failed: " + compErr50);
            REQUIRE(carta::seedNetMdTestMockUtoc(raw50, mockErr),
                    "seed 50-track mock UTOC failed: " + mockErr);

            carta::NetMdDeviceStatus largeProbe{};
            REQUIRE(carta::probeNetMdDevice("mock:mz-n920", largeProbe, mockErr),
                    "probeNetMdDevice 50-track failed: " + mockErr);
            REQUIRE(largeProbe.trackCount == 50 && largeProbe.tracks.empty(),
                    "probeNetMdDevice must skip per-track AVC polling when trackCount > 40");

            carta::MdDisc largeLoaded{};
            carta::NetMdDeviceStatus largeLoadedStatus{};
            REQUIRE(carta::loadNetMdDisc("mock:mz-n920", largeLoaded, &largeLoadedStatus, mockErr),
                    "loadNetMdDisc 50-track failed: " + mockErr);
            REQUIRE(largeLoaded.tracks.size() == 50 && largeLoadedStatus.trackCount == 50 &&
                        largeLoadedStatus.tracks.size() == 50,
                    "loadNetMdDisc must populate all 50 track entries from UTOC");
            REQUIRE(largeLoadedStatus.tracks[0].title == "T1" &&
                        largeLoadedStatus.tracks[49].title == "T50" &&
                        largeLoadedStatus.tracks[0].codec == "SP" &&
                        largeLoadedStatus.tracks[0].durationSeconds > 0,
                    "loadNetMdDisc must populate valid titles, codecs, and durations");

            carta::Session largeSess;
            carta::populateLoadedDiscSession(largeLoaded, largeSess);
            const auto pre50 = carta::preflightNetMdRecordingWithStatus(largeSess, largeLoaded,
                                                                        largeLoadedStatus);
            REQUIRE(pre50.deviceConnected && pre50.canWriteUtoc && pre50.canRecord &&
                        pre50.preservedOnDiscTracks == 50,
                    "preflightNetMdRecordingWithStatus must succeed with 50 preserved tracks");
        }
    }

    // 3C. Raw-span recovery for Write UTOC Only, P-DFA preservation, physical budget, and imported UTOC
    {
        auto buildSingleSpanSeedUtoc =
            [&](carta::UtocAddress startAddr, std::int64_t unitCount, std::uint8_t modeByte,
                carta::UtocAddress freeStart, carta::UtocAddress freeEnd, bool withPdfa,
                std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5>& outRaw) -> bool {
            for (auto& s : outRaw) {
                s.fill(0);
            }
            carta::UtocDiscState st{};
            st.hasSector0 = true;
            st.hasSector1 = true;
            st.hasSector2 = true;
            st.hasSector4 = false;
            st.deviceSignature = 0x534F;
            st.discNonEmpty = 1;
            st.trackCount = 1;
            for (std::uint8_t i = 1; i < 255; ++i) {
                st.fragments[i].link = static_cast<std::uint8_t>(i + 1);
            }
            st.fragments[255].link = 0;
            st.nextFreeTrackSlot = 1;

            auto allocF = [&](carta::UtocAddress a, carta::UtocAddress b, std::uint8_t mode) {
                const std::uint8_t slot = st.nextFreeTrackSlot;
                st.nextFreeTrackSlot = st.fragments[slot].link;
                st.fragments[slot].start = a;
                st.fragments[slot].end = b;
                st.fragments[slot].mode = mode;
                st.fragments[slot].link = 0;
                return slot;
            };
            st.trackMap[1] = allocF(
                startAddr,
                carta::UtocAddress::fromLinearGroup(startAddr.toLinearGroup() + unitCount - 1),
                modeByte);
            st.trackMap[0] = allocF(freeStart, freeEnd, 0);
            if (withPdfa) {
                st.sec0aPadding[14] = allocF({610, 0, 0}, {610, 31, 10}, 0);
            }
            st.halfTitleCells[1].bytes = {'R', 'A', 'W', 0, 0, 0, 0};
            st.halfTitleCells[1].link = 0;
            st.halfTitleMap[0] = 1;
            st.nextFreeHalfTitleSlot = 2;

            std::string compErr;
            return carta::compileUtocSectors(st, outRaw[0], outRaw[1], outRaw[2], outRaw[4],
                                             compErr);
        };

        const std::uint8_t spStereoMode =
            carta::utocModeFromTrack("sp-stereo", false, false, false);
        const std::uint8_t spMonoMode = carta::utocModeFromTrack("sp-mono", false, false, false);

        // 3C.1: Multi-track SP-Stereo coalesced 1-track upload recovery with empty cache + P-DFA
        carta::MdDisc sp3Disc;
        sp3Disc.medium = carta::MdMedium::md80;
        sp3Disc.defaultCodec = carta::MdCodecMode::spStereo;
        sp3Disc.halfWidthTitle = "SP3 TEST";
        for (int i = 0; i < 3; ++i) {
            carta::MdTrack t;
            t.id = "sp3-" + std::to_string(i + 1);
            t.halfWidthTitle = "Track " + std::to_string(i + 1);
            t.parts = {{i * kSegLen, (i + 1) * kSegLen}};
            sp3Disc.tracks.push_back(std::move(t));
        }
        carta::MdPhysicalPlan sp3Plan{};
        REQUIRE(carta::planMdPhysicalAudio(sp3Disc, sp3Plan, planErr) && sp3Plan.spans.size() == 1,
                "3-track SP-Stereo disc must coalesce into 1 span");

        // 3 * 44032 = 132096 samples -> 258 frames -> (258+1) padded mod 11 = 264 frames = 528 SG
        std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> raw3c1{};
        REQUIRE(buildSingleSpanSeedUtoc({50, 0, 0}, 528, spStereoMode, {611, 0, 0}, {2251, 31, 10},
                                        true, raw3c1),
                "Failed building 3C.1 seed UTOC");
        REQUIRE(carta::resetNetMdTestMockSession(mockErr) &&
                    carta::seedNetMdTestMockUtoc(raw3c1, mockErr) &&
                    carta::setNetMdTestMockGuard(true, mockErr),
                "Failed seeding 3C.1 mock state: " + mockErr);
        {
            carta::UtocBudget b{};
            std::string err3c1, hash3c1;
            std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> committed{};
            REQUIRE(carta::writeNetMdUtoc(sp3Disc, "mock:mz-n920", true, b, err3c1, &hash3c1,
                                          &committed),
                    "3C.1 writeNetMdUtoc recovery failed: " + err3c1);
            carta::UtocDiscState parsed{};
            REQUIRE(carta::parseUtocSectors(committed[0].data(), committed[1].data(),
                                            committed[2].data(), committed[4].data(), parsed,
                                            err3c1),
                    "3C.1 parse committed UTOC failed: " + err3c1);
            REQUIRE(parsed.trackCount == 3, "3C.1 must commit 3 tracks");
            const auto startLin = carta::UtocAddress{50, 0, 0}.toLinearGroup();
            const std::int64_t perTrackUnits = 172; // 44032 / 256
            for (int t = 0; t < 3; ++t) {
                const std::uint8_t slot = parsed.trackMap[static_cast<std::size_t>(t + 1)];
                REQUIRE(slot != 0 && parsed.fragments[slot].link == 0,
                        "3C.1 each remapped track must have 1 contiguous fragment");
                REQUIRE(parsed.fragments[slot].start.toLinearGroup() ==
                                startLin + t * perTrackUnits &&
                            parsed.fragments[slot].end.toLinearGroup() ==
                                startLin + (t + 1) * perTrackUnits - 1,
                        "3C.1 remapped physical addresses must match exact sequential subranges");
            }
            const bool hasPfra =
                parsed.trackMap[0] != 0 &&
                parsed.fragments[parsed.trackMap[0]].start == carta::UtocAddress{611, 0, 0};
            REQUIRE(hasPfra, "3C.1 must preserve P-FRA");
            const bool hasPdfa =
                parsed.sec0aPadding[14] != 0 &&
                parsed.fragments[parsed.sec0aPadding[14]].start ==
                    carta::UtocAddress{610, 0, 0} &&
                parsed.fragments[parsed.sec0aPadding[14]].end ==
                    carta::UtocAddress{610, 31, 10};
            REQUIRE(hasPdfa, "3C.1 must preserve P-DFA chain across Sector 0 remap");
        }

        // 3C.2: Single-track SP-Stereo padded raw upload recovery (dUnits > lUnits)
        carta::MdDisc sp1Disc;
        sp1Disc.medium = carta::MdMedium::md80;
        sp1Disc.defaultCodec = carta::MdCodecMode::spStereo;
        sp1Disc.halfWidthTitle = "SP1 TEST";
        sp1Disc.tracks.push_back({"sp1-1", "Solo", {}, {}, false, false, false, "", {{0, kSegLen}}});
        std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> raw3c2{};
        REQUIRE(buildSingleSpanSeedUtoc({50, 0, 0}, 172 + 14, spStereoMode, {52, 0, 0},
                                        {2251, 31, 10}, false, raw3c2),
                "Failed building 3C.2 seed UTOC");
        REQUIRE(carta::seedNetMdTestMockUtoc(raw3c2, mockErr),
                "Failed seeding 3C.2 mock UTOC: " + mockErr);
        {
            carta::UtocBudget b{};
            std::string err3c2, hash3c2;
            std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> committed{};
            REQUIRE(carta::writeNetMdUtoc(sp1Disc, "mock:mz-n920", true, b, err3c2, &hash3c2,
                                          &committed),
                    "3C.2 single-track padded writeNetMdUtoc failed: " + err3c2);
            carta::UtocDiscState parsed{};
            REQUIRE(carta::parseUtocSectors(committed[0].data(), committed[1].data(),
                                            committed[2].data(), committed[4].data(), parsed,
                                            err3c2),
                    "3C.2 parse failed: " + err3c2);
            const std::uint8_t s = parsed.trackMap[1];
            REQUIRE(parsed.trackCount == 1 && s != 0 &&
                        (parsed.fragments[s].end.toLinearGroup() -
                         parsed.fragments[s].start.toLinearGroup() + 1) == 172,
                    "3C.2 must trim padded 186 SG raw upload to exact 172 SG track");
        }

        // 3C.3: Mid-cluster start crossing nominal MD60 cluster cap when real disc is MD80 (2352 cl)
        {
            constexpr std::int64_t kHalfUnits = 881LL * 352LL; // total 1762 * 352 = 620224 units
            carta::MdDisc bigMd60;
            bigMd60.medium = carta::MdMedium::md60;
            bigMd60.defaultCodec = carta::MdCodecMode::spStereo;
            bigMd60.halfWidthTitle = "MID CLUSTER";
            bigMd60.tracks.push_back({"b1",
                                      "Part 1",
                                      {},
                                      {},
                                      false,
                                      false,
                                      false,
                                      "",
                                      {{0, kHalfUnits * 256}}});
            bigMd60.tracks.push_back({"b2",
                                      "Part 2",
                                      {},
                                      {},
                                      false,
                                      false,
                                      false,
                                      "",
                                      {{kHalfUnits * 256, 2 * kHalfUnits * 256}}});

            std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> raw3c3{};
            REQUIRE(buildSingleSpanSeedUtoc({50, 16, 0}, 2 * kHalfUnits + 8, spStereoMode,
                                            {1813, 0, 0}, {2401, 31, 10}, false, raw3c3),
                    "Failed building 3C.3 seed UTOC");
            REQUIRE(carta::seedNetMdTestMockUtoc(raw3c3, mockErr),
                    "Failed seeding 3C.3 mock UTOC: " + mockErr);
            carta::UtocBudget b{};
            std::string err3c3;
            REQUIRE(carta::writeNetMdUtoc(bigMd60, "mock:mz-n920", true, b, err3c3),
                    "3C.3 writeNetMdUtoc must use physical disc capacity: " + err3c3);
            REQUIRE(b.totalClusters == 2352 && b.usedClusters == 1763 && b.fits(),
                    "3C.3 expected real physical disc capacity 2352 with 1763 occupied clusters");
        }

        // 3C.4: Imported physical UTOC backup recovery
        {
            constexpr std::int64_t kElevenClustersUnits = 11LL * 352LL; // 3872 SG per track
            carta::MdDisc impDisc;
            impDisc.medium = carta::MdMedium::md80;
            impDisc.defaultCodec = carta::MdCodecMode::spStereo;
            impDisc.halfWidthTitle = "IMPORTED PHYS";
            impDisc.tracks.push_back({"i1",
                                      "Imp One",
                                      {},
                                      {},
                                      false,
                                      false,
                                      false,
                                      "",
                                      {{0, kElevenClustersUnits * 256}}});
            impDisc.tracks.push_back({"i2",
                                      "Imp Two",
                                      {},
                                      {},
                                      false,
                                      false,
                                      false,
                                      "",
                                      {{kElevenClustersUnits * 256, 2 * kElevenClustersUnits * 256}}});

            // Build physical backup UTOC pointing at clusters 50..60 and 70..80.
            carta::UtocDiscState impState{};
            impState.hasSector0 = true;
            impState.hasSector1 = true;
            impState.hasSector2 = true;
            impState.deviceSignature = 0x534F;
            impState.discNonEmpty = 1;
            impState.trackCount = 2;
            for (std::uint8_t i = 1; i < 255; ++i) {
                impState.fragments[i].link = static_cast<std::uint8_t>(i + 1);
            }
            impState.fragments[255].link = 0;
            impState.nextFreeTrackSlot = 1;
            auto allocImp = [&](carta::UtocAddress a, carta::UtocAddress b, std::uint8_t mode) {
                const std::uint8_t slot = impState.nextFreeTrackSlot;
                impState.nextFreeTrackSlot = impState.fragments[slot].link;
                impState.fragments[slot].start = a;
                impState.fragments[slot].end = b;
                impState.fragments[slot].mode = mode;
                impState.fragments[slot].link = 0;
                return slot;
            };
            impState.trackMap[1] = allocImp({50, 0, 0}, {60, 31, 10}, spStereoMode);
            impState.trackMap[2] = allocImp({70, 0, 0}, {80, 31, 10}, spStereoMode);
            impState.trackMap[0] = allocImp({121, 0, 0}, {2401, 31, 10}, 0);
            impState.halfTitleCells[1].bytes = {'I', 'M', 'P', 0, 0, 0, 0};
            impState.halfTitleMap[0] = 1;
            impState.nextFreeHalfTitleSlot = 2;
            std::array<std::uint8_t, carta::kUtocSectorBytes> is0{}, is1{}, is2{}, is4{};
            std::string impCompErr;
            REQUIRE(carta::compileUtocSectors(impState, is0, is1, is2, is4, impCompErr),
                    "Failed compiling 3C.4 imported UTOC: " + impCompErr);
            impDisc.importedUtocSectors[0].assign(is0.begin(), is0.end());
            impDisc.importedUtocSectors[1].assign(is1.begin(), is1.end());
            impDisc.importedUtocSectors[2].assign(is2.begin(), is2.end());
            impDisc.importedUtocSectors[3].assign(is4.begin(), is4.end());

            // Seed hardware with 1 big SP-Stereo span (50..120 = 71 clusters) covering both tracks.
            std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> raw3c4{};
            REQUIRE(buildSingleSpanSeedUtoc({50, 0, 0}, 71LL * 352LL, spStereoMode, {121, 0, 0},
                                            {2401, 31, 10}, false, raw3c4),
                    "Failed building 3C.4 seed UTOC");
            REQUIRE(carta::seedNetMdTestMockUtoc(raw3c4, mockErr),
                    "Failed seeding 3C.4 mock UTOC: " + mockErr);

            carta::UtocBudget b{};
            std::string err3c4, hash3c4;
            std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> committed{};
            REQUIRE(carta::writeNetMdUtoc(impDisc, "mock:mz-n920", true, b, err3c4, &hash3c4,
                                          &committed),
                    "3C.4 writeNetMdUtoc with imported physical UTOC failed: " + err3c4);
            carta::UtocDiscState parsed{};
            REQUIRE(carta::parseUtocSectors(committed[0].data(), committed[1].data(),
                                            committed[2].data(), committed[4].data(), parsed,
                                            err3c4),
                    "3C.4 parse failed: " + err3c4);
            const bool preservedImportedPhys =
                parsed.trackCount == 2 &&
                parsed.fragments[parsed.trackMap[1]].start == carta::UtocAddress{50, 0, 0} &&
                parsed.fragments[parsed.trackMap[1]].end == carta::UtocAddress{60, 31, 10} &&
                parsed.fragments[parsed.trackMap[2]].start == carta::UtocAddress{70, 0, 0} &&
                parsed.fragments[parsed.trackMap[2]].end == carta::UtocAddress{80, 31, 10};
            REQUIRE(preservedImportedPhys,
                    "3C.4 must preserve physical cluster addresses from imported UTOC");
        }

        // 3C.5: Mismatch rejection under hardware guard
        {
            carta::UtocBudget b{};
            std::string misErr;
            std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> badRaw{};

            // Wrong codec (sp-mono on disc vs sp-stereo in project)
            REQUIRE(buildSingleSpanSeedUtoc({50, 0, 0}, 516, spMonoMode, {55, 0, 0}, {2251, 31, 10},
                                            false, badRaw) &&
                        carta::seedNetMdTestMockUtoc(badRaw, mockErr),
                    "Failed seeding 3C.5 codec mismatch UTOC: " + mockErr);
            REQUIRE(!carta::writeNetMdUtoc(sp3Disc, "mock:mz-n920", true, b, misErr) &&
                        misErr.find("does not match") != std::string::npos,
                    "3C.5 must reject sp-mono disc for sp-stereo project");

            // Too short (506 < 516)
            misErr.clear();
            REQUIRE(buildSingleSpanSeedUtoc({50, 0, 0}, 506, spStereoMode, {55, 0, 0},
                                            {2251, 31, 10}, false, badRaw) &&
                        carta::seedNetMdTestMockUtoc(badRaw, mockErr),
                    "Failed seeding 3C.5 too-short UTOC: " + mockErr);
            REQUIRE(!carta::writeNetMdUtoc(sp3Disc, "mock:mz-n920", true, b, misErr) &&
                        misErr.find("does not match") != std::string::npos,
                    "3C.5 must reject too-short span on disc");

            // Far too long (5528 > 528 + 352)
            misErr.clear();
            REQUIRE(buildSingleSpanSeedUtoc({50, 0, 0}, 528 + 5000, spStereoMode, {70, 0, 0},
                                            {2251, 31, 10}, false, badRaw) &&
                        carta::seedNetMdTestMockUtoc(badRaw, mockErr),
                    "Failed seeding 3C.5 too-long UTOC: " + mockErr);
            REQUIRE(!carta::writeNetMdUtoc(sp3Disc, "mock:mz-n920", true, b, misErr) &&
                        misErr.find("does not match") != std::string::npos,
                    "3C.5 must reject unrelated long span on disc");
        }

        REQUIRE(carta::setNetMdTestMockGuard(false, mockErr),
                "Failed restoring mock guard after 3C: " + mockErr);
    }

    // 3D. Multi-slice gapless 1-span upload and forced gapped break 2-span upload
    {
        std::string err;
        REQUIRE(carta::resetNetMdTestMockSession(err),
                "Failed resetting mock session for 3D multi-slice test: " + err);

        carta::MdDisc multiDisc;
        multiDisc.medium = carta::MdMedium::md80;
        multiDisc.defaultCodec = carta::MdCodecMode::spStereo;
        multiDisc.halfWidthTitle = "MultiSlice";
        carta::MdTrack m1;
        m1.id = carta::makeMdTrackId();
        m1.halfWidthTitle = "Slice One";
        m1.parts = {{0, kSegLen}};
        carta::MdTrack m2;
        m2.id = carta::makeMdTrackId();
        m2.halfWidthTitle = "Slice Two";
        m2.parts = {{kSegLen * 2, kSegLen * 3}};
        multiDisc.tracks = {m1, m2};

        carta::MdPhysicalPlan multiPlan{};
        REQUIRE(carta::planMdPhysicalAudio(multiDisc, multiPlan, err) &&
                    multiPlan.spans.size() == 1 && multiPlan.spans[0].slices.size() == 2,
                "3D multi-slice disc must plan 1 SP span with 2 slices: " + err);

        carta::NetMdRecordResult multiRec{};
        REQUIRE(carta::recordNetMdDisc(session, multiDisc, "mock:mz-n920", false, multiRec, err),
                "3D multi-slice recordNetMdDisc failed: " + err);
        REQUIRE(multiRec.uploadedPhysicalSegments == 1 && multiRec.logicalTracks == 2 &&
                    multiRec.committedUtoc,
                "3D multi-slice must upload 1 physical segment with 2 logical UTOC tracks");

        TestMockSnapshot multiSnap;
        REQUIRE(fetchMockSnapshot(multiSnap, err), "fetchMockSnapshot 3D multi failed: " + err);
        REQUIRE(multiSnap.uploadedTracks.size() == 1,
                "3D multi-slice must upload exactly 1 SP track to hardware");
        carta::UtocDiscState multiParsed{};
        REQUIRE(carta::parseUtocSectors(multiSnap.committedUtoc[0].data(),
                                        multiSnap.committedUtoc[1].data(), nullptr, nullptr,
                                        multiParsed, err) &&
                    multiParsed.trackCount == 2,
                "3D multi-slice UTOC must contain 2 tracks: " + err);

        REQUIRE(carta::resetNetMdTestMockSession(err),
                "Failed resetting mock session for 3D forced-gap test: " + err);

        carta::MdDisc gapDisc;
        gapDisc.medium = carta::MdMedium::md80;
        gapDisc.defaultCodec = carta::MdCodecMode::spStereo;
        gapDisc.halfWidthTitle = "ForcedGap";
        carta::MdTrack g1;
        g1.id = carta::makeMdTrackId();
        g1.halfWidthTitle = "Gap Left";
        g1.parts = {{0, kSegLen}};
        carta::MdTrack g2;
        g2.id = carta::makeMdTrackId();
        g2.halfWidthTitle = "Gap Right";
        g2.forceGapBefore = true;
        g2.parts = {{kSegLen, kSegLen * 2}};
        gapDisc.tracks = {g1, g2};

        carta::MdPhysicalPlan gapPlan{};
        REQUIRE(carta::planMdPhysicalAudio(gapDisc, gapPlan, err) && gapPlan.spans.size() == 2,
                "3D forced-gap disc must split abutting SP tracks into 2 spans: " + err);

        carta::NetMdRecordResult gapRec{};
        REQUIRE(carta::recordNetMdDisc(session, gapDisc, "mock:mz-n920", false, gapRec, err),
                "3D forced-gap recordNetMdDisc failed: " + err);
        REQUIRE(gapRec.uploadedPhysicalSegments == 2 && gapRec.logicalTracks == 2 &&
                    gapRec.committedUtoc,
                "3D forced-gap must upload 2 separate physical segments and 2 logical UTOC tracks");

        TestMockSnapshot gapSnap;
        REQUIRE(fetchMockSnapshot(gapSnap, err), "fetchMockSnapshot 3D gap failed: " + err);
        REQUIRE(gapSnap.uploadedTracks.size() == 2,
                "3D forced-gap must upload 2 separate SP tracks to hardware");
        carta::UtocDiscState gapParsed{};
        REQUIRE(carta::parseUtocSectors(gapSnap.committedUtoc[0].data(),
                                        gapSnap.committedUtoc[1].data(), nullptr, nullptr,
                                        gapParsed, err) &&
                    gapParsed.trackCount == 2 &&
                    gapParsed.fragments[gapParsed.trackMap[1]].start.toLinearGroup() !=
                        gapParsed.fragments[gapParsed.trackMap[2]].start.toLinearGroup(),
                "3D forced-gap UTOC tracks must reference separate physical spans: " + err);
    }

    REQUIRE(carta::setNetMdTestMockSessionEnabled(false, mockErr),
            "Failed stopping mock session: " + mockErr);

    // 4. Constrained 544 KB stack regression test inside the standalone `carta-netmd` binary
    {
        std::string selftestErr;
        REQUIRE(carta::runNetMdHelperStackSelftest(selftestErr),
                "carta-netmd selftest-stack failed: " + selftestErr);
    }

    std::cout << "carta-netmd: PASS\n";
    return 0;
}
