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
            tr.startAddr = carta::UtocAddress::fromLinearGroup(
                static_cast<std::int64_t>(trObj->getProperty("start_linear")));
            tr.endAddr = carta::UtocAddress::fromLinearGroup(
                static_cast<std::int64_t>(trObj->getProperty("end_linear")));
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
