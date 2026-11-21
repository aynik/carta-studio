/**
 * @file HiMdBridgeTests.cpp
 * Phase A verification suite for Carta Studio's out-of-process Hi-MD bridge and ATRAC3plus/PCM
 * streaming upload encoder:
 *
 * 1. Bit-for-bit parity of `encodeHiMdTrackPayload("hi-sp"|"hi-lp"|"lp2"|"lp4")` against
 *    `encodeAtracContainer` + `extractNetMdWirePayload` across exact-2048 and non-2048 lengths.
 * 2. Big-endian 16-bit LPCM output verification (`"pcm"` / `"lpcm"`), mono duplication, and
 *    progress callbacks.
 * 3. Helper auto-discovery, mock session lifecycle, `listHiMdDevices`, `probeHiMdDevice`.
 * 4. Hardware write confirmation guard checked BEFORE rendering/encoding.
 * 5. Multi-codec 5-track `recordHiMdDisc` (`hi-sp`, `hi-lp`, `lp2`, `lp4`, `pcm`) with metadata &
 *    2 groups, preflight validation, and `probeHiMdDevice` readback.
 * 6. `dumpHiMdTrack` (raw + `.oma` EA3 + `.wav`), `renameHiMdDisc`, `renameHiMdTrack`,
 *    `moveHiMdTrack`, `setHiMdGroups`, `deleteHiMdTracks`, `loadHiMdDiscIntoSession`,
 *    `wipeHiMdDisc(false/true)`, and `runHiMdHelperStackSelftest`.
 */

#include "Core/Session.h"
#include "Editing/Editing.h"
#include "MiniDisc/HiMdBridge.h"
#include "MiniDisc/NativeCodecs.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

#define REQUIRE(cond, msg)                                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " << (msg) << "\n";        \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

void fillTestTone(int sampleCount, int seed, std::vector<float>& left, std::vector<float>& right) {
    left.resize(static_cast<std::size_t>(sampleCount));
    right.resize(static_cast<std::size_t>(sampleCount));
    constexpr double kTwoPi = 6.28318530717958647692;
    const double fL = 220.0 + 73.0 * static_cast<double>(seed);
    const double fR = 330.0 + 97.0 * static_cast<double>(seed);
    for (int i = 0; i < sampleCount; ++i) {
        left[static_cast<std::size_t>(i)] =
            0.40f * static_cast<float>(std::sin(kTwoPi * fL * static_cast<double>(i) / 44100.0));
        right[static_cast<std::size_t>(i)] =
            0.35f * static_cast<float>(std::sin(kTwoPi * fR * static_cast<double>(i) / 44100.0));
    }
}

bool readFileBytes(const juce::File& file, std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    juce::MemoryBlock mb;
    if (!file.loadFileAsData(mb)) {
        error = "Failed reading file: " + file.getFullPathName().toStdString();
        return false;
    }
    out.resize(mb.getSize());
    if (mb.getSize() > 0) {
        std::memcpy(out.data(), mb.getData(), mb.getSize());
    }
    return true;
}

std::uint32_t readLe32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

#if !defined(_WIN32)
std::string percentEncodeForSession(const std::string& input) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() + 8);
    for (char ch : input) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '/' || c == ':') {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0Fu]);
            out.push_back(kHex[c & 0x0Fu]);
        }
    }
    return out;
}

std::string findTestHelperPath() {
    static constexpr const char* kName = "carta-himd";
    const auto exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    std::vector<juce::File> candidates;
#if defined(__APPLE__)
    candidates.push_back(exeDir.getParentDirectory().getChildFile("Helpers").getChildFile(kName));
#endif
    candidates.push_back(exeDir.getChildFile(kName));
    candidates.push_back(exeDir.getParentDirectory().getChildFile(kName));
    auto probeDir = exeDir;
    for (int depth = 0; depth < 4; ++depth) {
        candidates.push_back(probeDir.getChildFile(kName));
        candidates.push_back(probeDir.getChildFile("build").getChildFile(kName));
        const auto parent = probeDir.getParentDirectory();
        if (parent == probeDir) {
            break;
        }
        probeDir = parent;
    }
    for (const auto& c : candidates) {
        if (c.existsAsFile()) {
            return c.getFullPathName().toStdString();
        }
    }
    return {};
}

bool writeAllFd(int fd, const char* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        const ssize_t n = ::write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool readLineFd(int fd, std::string& carry, std::string& outLine) {
    outLine.clear();
    while (true) {
        const std::size_t nl = carry.find('\n');
        if (nl != std::string::npos) {
            outLine = carry.substr(0, nl);
            carry.erase(0, nl + 1);
            return true;
        }
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int prc = ::poll(&pfd, 1, 8000);
        if (prc <= 0) {
            return false;
        }
        char buf[4096];
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
            return false;
        }
        carry.append(buf, static_cast<std::size_t>(n));
    }
}

bool sendSocketCommand(int fd, std::string& carry, const std::string& rawLine, juce::var& outResult,
                       std::string& error) {
    outResult = juce::var();
    if (!writeAllFd(fd, rawLine.data(), rawLine.size())) {
        error = "Failed writing command to test socket.";
        return false;
    }
    std::string line;
    while (readLineFd(fd, carry, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        juce::var parsed;
        if (juce::JSON::parse(juce::String::fromUTF8(line.c_str()), parsed).failed() ||
            !parsed.isObject()) {
            error = "Malformed JSON line: " + line;
            return false;
        }
        const auto ev = parsed.getDynamicObject()->getProperty("event").toString();
        if (ev == "progress") {
            continue;
        }
        if (ev == "result") {
            outResult = parsed;
            if (!static_cast<bool>(parsed.getDynamicObject()->getProperty("ok"))) {
                error = parsed.getDynamicObject()->getProperty("error").toString().toStdString();
                return false;
            }
            return true;
        }
    }
    error = "EOF reading result from test socket.";
    return false;
}
#endif

struct ScopedElevationEnv {
    explicit ScopedElevationEnv(const char* value) {
#if defined(_WIN32)
        const char* prev = std::getenv("CARTA_HIMD_ELEVATION");
        if (prev != nullptr) {
            hadPrev = true;
            prevValue = prev;
        }
        _putenv_s("CARTA_HIMD_ELEVATION", value ? value : "");
#else
        const char* prev = std::getenv("CARTA_HIMD_ELEVATION");
        if (prev != nullptr) {
            hadPrev = true;
            prevValue = prev;
        }
        if (value != nullptr) {
            ::setenv("CARTA_HIMD_ELEVATION", value, 1);
        } else {
            ::unsetenv("CARTA_HIMD_ELEVATION");
        }
#endif
    }
    ~ScopedElevationEnv() {
#if defined(_WIN32)
        _putenv_s("CARTA_HIMD_ELEVATION", hadPrev ? prevValue.c_str() : "");
#else
        if (hadPrev) {
            ::setenv("CARTA_HIMD_ELEVATION", prevValue.c_str(), 1);
        } else {
            ::unsetenv("CARTA_HIMD_ELEVATION");
        }
#endif
    }
    bool hadPrev = false;
    std::string prevValue;
};

carta::Session makeFiveTrackHiMdSession(const std::vector<int>& segSamples) {
    int totalSamples = 0;
    for (int n : segSamples) {
        totalSamples += n;
    }
    auto audio = std::make_shared<carta::Audio>();
    audio->sampleRate = carta::sessionRate;
    audio->samples.setSize(2, totalSamples);
    auto* L = audio->samples.getWritePointer(0);
    auto* R = audio->samples.getWritePointer(1);

    constexpr double kTwoPi = 6.28318530717958647692;
    int offset = 0;
    for (std::size_t idx = 0; idx < segSamples.size(); ++idx) {
        const int n = segSamples[idx];
        const double fL = 220.0 + 95.0 * static_cast<double>(idx);
        const double fR = 310.0 + 110.0 * static_cast<double>(idx);
        for (int i = 0; i < n; ++i) {
            L[offset + i] =
                0.42f *
                static_cast<float>(std::sin(kTwoPi * fL * static_cast<double>(i) / 44100.0));
            R[offset + i] =
                0.37f *
                static_cast<float>(std::sin(kTwoPi * fR * static_cast<double>(i) / 44100.0));
        }
        offset += n;
    }
    carta::buildWaveformPeaks(*audio);
    carta::Session session;
    carta::addImportedAudioTrack(session, audio, 0);
    return session;
}

} // namespace

int main() {
    std::string err;

    // 1. Streaming `encodeHiMdTrackPayload` bit-for-bit parity vs `encodeAtracContainer` +
    //    `extractNetMdWirePayload` across short, boundary (1680 vs 1681), and exact-2048 lengths.
    {
        const int testLengths[] = {100, 1680, 1681, 2048, 2048 + 100, 2048 + 1680, 2048 + 1681, 65536};
        const char* carta5Modes[] = {"hi-sp", "hi-lp"};
        for (const char* mode : carta5Modes) {
            for (int len : testLengths) {
                std::vector<float> left;
                std::vector<float> right;
                fillTestTone(len, len % 7, left, right);

                std::vector<std::uint8_t> container;
                std::vector<std::uint8_t> expectedWire;
                REQUIRE(carta::encodeAtracContainer(mode, left.data(), right.data(), 2, len,
                                                    container, err),
                        std::string("encodeAtracContainer failed for ") + mode + " len=" +
                            std::to_string(len) + ": " + err);
                REQUIRE(carta::extractNetMdWirePayload(mode, container, expectedWire, err),
                        std::string("extractNetMdWirePayload failed: ") + err);

                int lastDone = -1;
                int lastTotal = -1;
                int progressCalls = 0;
                std::vector<std::uint8_t> actualPayload;
                REQUIRE(carta::encodeHiMdTrackPayload(
                            mode, left.data(), right.data(), 2, len, actualPayload,
                            [&](int done, int total) {
                                lastDone = done;
                                lastTotal = total;
                                ++progressCalls;
                            },
                            err),
                        std::string("encodeHiMdTrackPayload failed for ") + mode + " len=" +
                            std::to_string(len) + ": " + err);
                REQUIRE(actualPayload == expectedWire,
                        std::string("Bit-for-bit mismatch for ") + mode + " len=" +
                            std::to_string(len) + " (actual=" +
                            std::to_string(actualPayload.size()) +
                            ", expected=" + std::to_string(expectedWire.size()) + ")");
                REQUIRE(progressCalls >= 2 && lastTotal > 0 && lastDone == lastTotal,
                        "Expected complete progress callbacks for " + std::string(mode));
            }
        }

        // ATRAC3 NetMD modes routed via encodeHiMdTrackPayload must also match bit-for-bit.
        const char* carta3Modes[] = {"lp2", "lp4"};
        for (const char* mode : carta3Modes) {
            std::vector<float> left;
            std::vector<float> right;
            fillTestTone(16384, 3, left, right);
            std::vector<std::uint8_t> container;
            std::vector<std::uint8_t> expectedWire;
            std::vector<std::uint8_t> actualPayload;
            REQUIRE(carta::encodeAtracContainer(mode, left.data(), right.data(), 2, 16384,
                                                container, err),
                    err);
            REQUIRE(carta::extractNetMdWirePayload(mode, container, expectedWire, err), err);
            REQUIRE(carta::encodeHiMdTrackPayload(mode, left.data(), right.data(), 2, 16384,
                                                  actualPayload, {}, err),
                    err);
            REQUIRE(actualPayload == expectedWire,
                    std::string("Bit-for-bit mismatch for ") + mode);
        }

        // BE LPCM encoding check (stereo values + mono duplication).
        const float pcmL[4] = {0.0f, 1.0f, -1.0f, 0.5f};
        const float pcmR[4] = {0.5f, -0.5f, 0.0f, -1.0f};
        std::vector<std::uint8_t> bePcm;
        int lpcmDone = -1;
        int lpcmTotal = -1;
        REQUIRE(carta::encodeHiMdTrackPayload(
                    "pcm", pcmL, pcmR, 2, 4, bePcm,
                    [&](int d, int t) {
                        lpcmDone = d;
                        lpcmTotal = t;
                    },
                    err),
                "PCM encode failed: " + err);
        REQUIRE(lpcmDone == 4 && lpcmTotal == 4, "PCM progress did not complete");
        const std::vector<std::uint8_t> expectedBe{
            0x00, 0x00, 0x40, 0x00, // 0, +16384
            0x7F, 0xFF, 0xC0, 0x00, // +32767, -16384
            0x80, 0x01, 0x00, 0x00, // -32767, 0
            0x40, 0x00, 0x80, 0x01, // +16384, -32767
        };
        REQUIRE(bePcm == expectedBe, "Big-endian LPCM byte layout mismatch");

        std::vector<std::uint8_t> monoDup;
        REQUIRE(carta::encodeHiMdTrackPayload("lpcm", pcmL, nullptr, 1, 2, monoDup, {}, err), err);
        const std::vector<std::uint8_t> expectedMonoDup{
            0x00, 0x00, 0x00, 0x00,
            0x7F, 0xFF, 0x7F, 0xFF,
        };
        REQUIRE(monoDup == expectedMonoDup, "Mono LPCM duplication mismatch");
    }

    // 2. Start persistent mock session via auto-discovered `carta-himd`.
    struct MockSessionCleanup final {
        ~MockSessionCleanup() {
            std::string ignoreErr;
            carta::setHiMdTestMockSessionEnabled(false, ignoreErr);
        }
    } cleanupGuard;

    REQUIRE(carta::setHiMdTestMockSessionEnabled(true, err),
            "Failed enabling Hi-MD mock session: " + err);
    REQUIRE(carta::resetHiMdTestMockSession(err), "Failed resetting Hi-MD mock session: " + err);

    std::vector<carta::HiMdDeviceEntry> devs;
    REQUIRE(carta::listHiMdDevices(devs, err), "listHiMdDevices failed: " + err);
    bool foundMock = false;
    for (const auto& d : devs) {
        if (d.id == "mock:mz-rh1" && d.isMock) {
            foundMock = true;
            break;
        }
    }
    REQUIRE(foundMock, "Expected mock:mz-rh1 in listed Hi-MD devices");

    carta::HiMdDeviceStatus initStatus;
    REQUIRE(carta::probeHiMdDevice("mock:mz-rh1", initStatus, err),
            "probeHiMdDevice failed: " + err);
    REQUIRE(initStatus.trackCount == 0 && initStatus.blockSize == 2048 && initStatus.fsFreeBytes > 0,
            "Unexpected initial Hi-MD probe status");

    // 3. Build 5-track multi-codec session & authored `MdDisc`.
    const std::vector<int> segLens = {
        65536 + 100, // Track 0: Hi-SP (non-multiple tail <= 1680)
        65536 + 1800, // Track 1: Hi-LP (non-multiple tail > 1680)
        65536,       // Track 2: LP2
        65536,       // Track 3: LP4
        24480,       // Track 4: PCM (24480 * 4 = 97920 = 6 * 16320)
    };
    const carta::Session session = makeFiveTrackHiMdSession(segLens);

    carta::MdDisc disc;
    disc.medium = carta::MdMedium::himd300;
    disc.defaultCodec = carta::MdCodecMode::hiSp;
    disc.halfWidthTitle = "Studio Hi-MD";

    const carta::MdCodecMode codecs[5] = {
        carta::MdCodecMode::hiSp, carta::MdCodecMode::hiLp, carta::MdCodecMode::lp2,
        carta::MdCodecMode::lp4,  carta::MdCodecMode::pcm,
    };
    const char* titles[5] = {
        "Hi-SP Master",
        "Caf\xC3\xA9 Hi-LP",
        "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88LP2",
        "LP4 Mix \xF0\x9F\x8E\xB5",
        "Studio PCM",
    };
    const char* artists[5] = {"Carta", "Ren\xC3\xA9", "\xE8\xA9\xA6\xE9\xA8\x93", "Delta", "Studio"};
    const char* albums[5] = {"Album A", "\xC3\x89t\xC3\xA9", "\xE3\x82\xA2\xE3\x83\xAB\xE3\x83\x90\xE3\x83\xA0", "Mix", "Raw"};

    std::int64_t samplePos = 0;
    for (int i = 0; i < 5; ++i) {
        carta::MdTrack tr;
        tr.id = carta::makeMdTrackId();
        tr.halfWidthTitle = titles[i];
        tr.artist = artists[i];
        tr.album = albums[i];
        if (i != 0) {
            tr.codecOverride = codecs[i];
        }
        tr.parts = {{samplePos, samplePos + segLens[static_cast<std::size_t>(i)]}};
        samplePos += segLens[static_cast<std::size_t>(i)];
        disc.tracks.push_back(std::move(tr));
    }
    disc.groups = {
        {1, 2, "Set A", ""},
        {3, 5, "Set B", ""},
    };

    const auto preflight = carta::preflightHiMdRecording(disc, &initStatus);
    REQUIRE(preflight.valid && preflight.tracksToUpload == 5 &&
                preflight.estimatedAudioBytes > 0 &&
                preflight.requiredBytes <= preflight.discFreeBytes,
            "preflightHiMdRecording failed: " + preflight.error);

    {
        auto badCodecDisc = disc;
        badCodecDisc.tracks[0].codecOverride = carta::MdCodecMode::spStereo;
        const auto badPre = carta::preflightHiMdRecording(badCodecDisc, &initStatus);
        REQUIRE(!badPre.valid, "preflightHiMdRecording must reject sp-stereo track");
    }

    // 4. Hardware write confirmation guard checked BEFORE rendering/encoding.
    {
        REQUIRE(carta::setHiMdTestMockGuard(true, err), "setHiMdTestMockGuard(1) failed: " + err);
        std::string beforeSnap;
        REQUIRE(carta::fetchHiMdTestMockSnapshot(beforeSnap, err), err);
        juce::var beforeVar;
        REQUIRE(juce::JSON::parse(juce::String::fromUTF8(beforeSnap.c_str()), beforeVar).wasOk(),
                "Failed parsing before snapshot");

        carta::HiMdRecordResult guardedRes;
        std::string guardErr;
        REQUIRE(!carta::recordHiMdDisc(session, disc, "mock:mz-rh1", false, guardedRes, guardErr) &&
                    guardErr.find("confirmation") != std::string::npos,
                "recordHiMdDisc should reject unconfirmed hardware before encoding");
        REQUIRE(carta::getHiMdProgress().operation.empty(),
                "Unconfirmed recordHiMdDisc must abort before publishing encoding progress");

        guardErr.clear();
        REQUIRE(!carta::renameHiMdDisc("mock:mz-rh1", "Blocked", false, guardErr) &&
                    guardErr.find("confirmation") != std::string::npos,
                "renameHiMdDisc guard failure mismatch");
        guardErr.clear();
        REQUIRE(!carta::renameHiMdTrack("mock:mz-rh1", 0, true, "X", false, "", false, "", false,
                                        guardErr) &&
                    guardErr.find("confirmation") != std::string::npos,
                "renameHiMdTrack guard failure mismatch");
        guardErr.clear();
        REQUIRE(!carta::moveHiMdTrack("mock:mz-rh1", 0, 1, false, guardErr) &&
                    guardErr.find("confirmation") != std::string::npos,
                "moveHiMdTrack guard failure mismatch");
        guardErr.clear();
        REQUIRE(!carta::setHiMdGroups("mock:mz-rh1", disc.groups, 5, false, guardErr) &&
                    guardErr.find("confirmation") != std::string::npos,
                "setHiMdGroups guard failure mismatch");
        guardErr.clear();
        REQUIRE(!carta::deleteHiMdTracks("mock:mz-rh1", {0}, false, guardErr) &&
                    guardErr.find("confirmation") != std::string::npos,
                "deleteHiMdTracks guard failure mismatch");
        guardErr.clear();
        REQUIRE(!carta::wipeHiMdDisc("mock:mz-rh1", false, false, guardErr) &&
                    guardErr.find("confirmation") != std::string::npos,
                "wipeHiMdDisc guard failure mismatch");

        std::string afterSnap;
        REQUIRE(carta::fetchHiMdTestMockSnapshot(afterSnap, err), err);
        juce::var afterVar;
        REQUIRE(juce::JSON::parse(juce::String::fromUTF8(afterSnap.c_str()), afterVar).wasOk(),
                "Failed parsing after snapshot");
        REQUIRE(static_cast<int>(afterVar["sectors_written"]) ==
                    static_cast<int>(beforeVar["sectors_written"]) &&
                static_cast<int>(afterVar["icv_write_count"]) ==
                    static_cast<int>(beforeVar["icv_write_count"]),
                "Guarded rejections must not mutate mock hardware");
        REQUIRE(carta::setHiMdTestMockGuard(false, err), "setHiMdTestMockGuard(0) failed: " + err);
    }

    // 5. Full 5-track `recordHiMdDisc` + metadata + 2 groups + probe readback.
    carta::HiMdRecordResult recRes;
    REQUIRE(carta::recordHiMdDisc(session, disc, "mock:mz-rh1", false, recRes, err),
            "recordHiMdDisc failed: " + err);
    REQUIRE(recRes.ok && recRes.uploadedTracks == 5 && recRes.uploadedBytes > 0 &&
                recRes.blocksWritten > 0 && recRes.discTitle == "Studio Hi-MD",
            "Unexpected recordHiMdDisc result");

    carta::HiMdDeviceStatus afterRec;
    REQUIRE(carta::probeHiMdDevice("mock:mz-rh1", afterRec, err), "probe after record failed: " + err);
    REQUIRE(afterRec.discTitle == "Studio Hi-MD" && afterRec.trackCount == 5 &&
                afterRec.tracks.size() == 5,
            "Probed disc title or track count mismatch after recordHiMdDisc");

    const char* expectedEnc[5] = {"A3+", "A3+", "AT3", "AT3", "PCM"};
    const int expectedBitrate[5] = {256, 64, 132, 66, 1411};
    for (int i = 0; i < 5; ++i) {
        const auto& t = afterRec.tracks[static_cast<std::size_t>(i)];
        REQUIRE(t.title == titles[i] && t.artist == artists[i] && t.album == albums[i],
                "Metadata mismatch on recorded track " + std::to_string(i));
        REQUIRE(t.encoding == expectedEnc[i] && t.bitrate == expectedBitrate[i],
                "Encoding/bitrate mismatch on recorded track " + std::to_string(i));
    }
    REQUIRE(afterRec.groups.size() == 2 && afterRec.groups[0].title == "Set A" &&
                afterRec.groups[0].startIndex == 0 && afterRec.groups[0].trackCount == 2 &&
                afterRec.groups[1].title == "Set B" && afterRec.groups[1].startIndex == 2 &&
                afterRec.groups[1].trackCount == 3,
            "Group layout mismatch after recordHiMdDisc");

    // 6. Dump verification (raw, EA3 `.oma`, and `.wav`).
    const auto tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("carta_himd_bridge_test_" +
                                          juce::String(juce::Time::getHighResolutionTicks()));
    REQUIRE(tmpDir.createDirectory(), "Failed creating temp dir");
    struct DirCleanup final {
        juce::File dir;
        ~DirCleanup() { dir.deleteRecursively(); }
    } dirCleanup{tmpDir};

    std::vector<float> tr0L, tr0R;
    REQUIRE(carta::renderSessionMixRange(session, disc.tracks[0].parts[0].startSample,
                                         disc.tracks[0].parts[0].endSample, 2, tr0L, tr0R, err),
            err);
    std::vector<std::uint8_t> expectedTr0Raw;
    REQUIRE(carta::encodeHiMdTrackPayload("hi-sp", tr0L.data(), tr0R.data(), 2,
                                          static_cast<int>(tr0L.size()), expectedTr0Raw, {}, err),
            err);

    const auto rawFile = tmpDir.getChildFile("track0_raw.bin");
    REQUIRE(carta::dumpHiMdTrack("mock:mz-rh1", 0, rawFile.getFullPathName().toStdString(), true,
                                 err),
            "dumpHiMdTrack raw failed: " + err);
    std::vector<std::uint8_t> gotRaw;
    REQUIRE(readFileBytes(rawFile, gotRaw, err), err);
    REQUIRE(gotRaw == expectedTr0Raw, "Raw Hi-SP dump bytes mismatch");

    const auto omaFile = tmpDir.getChildFile("track0.oma");
    REQUIRE(carta::dumpHiMdTrack("mock:mz-rh1", 0, omaFile.getFullPathName().toStdString(), false,
                                 err),
            "dumpHiMdTrack EA3 failed: " + err);
    std::vector<std::uint8_t> gotOma;
    REQUIRE(readFileBytes(omaFile, gotOma, err), err);
    REQUIRE(gotOma.size() == 96u + expectedTr0Raw.size() && gotOma[0] == 'E' &&
                gotOma[1] == 'A' && gotOma[2] == '3' && gotOma[3] == 1 && gotOma[5] == 0x60 &&
                std::equal(expectedTr0Raw.begin(), expectedTr0Raw.end(), gotOma.begin() + 96),
            "EA3 container dump mismatch");

    const auto wavFile = tmpDir.getChildFile("track4.wav");
    REQUIRE(carta::dumpHiMdTrack("mock:mz-rh1", 4, wavFile.getFullPathName().toStdString(), false,
                                 err),
            "dumpHiMdTrack WAV failed: " + err);
    std::vector<std::uint8_t> gotWav;
    REQUIRE(readFileBytes(wavFile, gotWav, err), err);
    REQUIRE(gotWav.size() == 44u + static_cast<std::size_t>(segLens[4]) * 4u &&
                std::memcmp(gotWav.data(), "RIFF", 4) == 0 &&
                std::memcmp(gotWav.data() + 8, "WAVE", 4) == 0 &&
                readLe32(gotWav.data() + 40) == static_cast<std::uint32_t>(segLens[4]) * 4u,
            "PCM WAV container header/size mismatch");

    // 7. Rename / move / groups / delete / loadHiMdDiscIntoSession / wipe / selftest-stack.
    REQUIRE(carta::renameHiMdDisc("mock:mz-rh1", "Renamed Disc", false, err),
            "renameHiMdDisc failed: " + err);
    REQUIRE(carta::renameHiMdTrack("mock:mz-rh1", 0, true, "Renamed Tr0", false, "", false, "",
                                   false, err),
            "renameHiMdTrack failed: " + err);
    REQUIRE(carta::moveHiMdTrack("mock:mz-rh1", 4, 0, false, err),
            "moveHiMdTrack failed: " + err);
    REQUIRE(carta::deleteHiMdTracks("mock:mz-rh1", {2}, false, err),
            "deleteHiMdTracks failed: " + err);
    REQUIRE(carta::setHiMdGroups("mock:mz-rh1", {{1, 2, "Pair", ""}, {3, 4, "Rest", ""}}, 4, false,
                                 err),
            "setHiMdGroups failed: " + err);

    carta::MdDisc loadedDisc;
    carta::HiMdDeviceStatus loadedStatus;
    REQUIRE(carta::loadHiMdDiscIntoSession("mock:mz-rh1", loadedDisc, &loadedStatus, err),
            "loadHiMdDiscIntoSession failed: " + err);
    REQUIRE(loadedDisc.loadedDisc.active && carta::isHiMdMedium(loadedDisc.medium) &&
                loadedDisc.halfWidthTitle == "Renamed Disc" && loadedDisc.tracks.size() == 4 &&
                loadedDisc.tracks[0].halfWidthTitle == "Studio PCM" &&
                loadedDisc.tracks[1].halfWidthTitle == "Renamed Tr0" &&
                loadedDisc.tracks[1].artist == "Carta" &&
                loadedDisc.tracks[1].album == "Album A" && loadedDisc.groups.size() == 2 &&
                loadedDisc.groups[0].halfWidthTitle == "Pair" &&
                loadedDisc.groups[0].startTrack == 1 && loadedDisc.groups[0].endTrack == 2 &&
                loadedDisc.groups[1].halfWidthTitle == "Rest" &&
                loadedDisc.groups[1].startTrack == 3 && loadedDisc.groups[1].endTrack == 4,
            "loadHiMdDiscIntoSession state mismatch");
    REQUIRE(carta::validateMdDisc(loadedDisc, err),
            "validateMdDisc on loaded Hi-MD disc failed: " + err);

    REQUIRE(carta::wipeHiMdDisc("mock:mz-rh1", false, false, err),
            "wipeHiMdDisc(false) failed: " + err);
    REQUIRE(carta::probeHiMdDevice("mock:mz-rh1", loadedStatus, err) &&
                loadedStatus.trackCount == 0,
            "Expected 0 tracks after wipe(false)");

    REQUIRE(carta::wipeHiMdDisc("mock:mz-rh1", true, false, err),
            "wipeHiMdDisc(true) failed: " + err);
    REQUIRE(carta::probeHiMdDevice("mock:mz-rh1", loadedStatus, err) &&
                loadedStatus.trackCount == 0,
            "Expected 0 tracks after wipe(true)");

    REQUIRE(carta::runHiMdHelperStackSelftest(err),
            "runHiMdHelperStackSelftest failed: " + err);

    // 8. Permission error classification & default non-prompting elevation state.
    REQUIRE(!carta::isHiMdElevationPromptEnabled(),
            "Elevation prompts must default to disabled for headless/test safety");
    carta::setHiMdElevationPromptEnabled(true);
    REQUIRE(carta::isHiMdElevationPromptEnabled(), "Expected elevation prompt enable toggle");
    carta::setHiMdElevationPromptEnabled(false);
    REQUIRE(!carta::isHiMdElevationPromptEnabled(), "Expected elevation prompt disable toggle");

    REQUIRE(carta::isHiMdPermissionError(
                "Failed claiming Hi-MD USB Mass Storage interface #0: Access denied "
                "(insufficient permissions). On macOS, IOUSBMassStorageDriver locks the "
                "interface exclusively.") &&
                carta::isHiMdPermissionError("Failed opening /dev/sg2: Permission denied") &&
                carta::isHiMdPermissionError("Failed opening device: EACCES") &&
                carta::isHiMdPermissionError(
                    "ERROR_ACCESS_DENIED: Run as Administrator for direct SCSI access.") &&
                !carta::isHiMdPermissionError("") &&
                !carta::isHiMdPermissionError("Physical Hi-MD device is no longer connected.") &&
                !carta::isHiMdPermissionError("Track index out of range."),
            "isHiMdPermissionError classification mismatch");

#if !defined(_WIN32)
    // 9. POSIX Unix domain socket session authentication, command roundtrip & cleanup.
    {
        const std::string helperPath = findTestHelperPath();
        REQUIRE(!helperPath.empty(), "Could not locate carta-himd helper binary for socket tests");

        const juce::File tokFile = tmpDir.getChildFile("session_tok.txt");
        const std::string secret = "0123456789abcdef0123456789fedcba";
        REQUIRE(tokFile.replaceWithText(secret, false, false, "\n"),
                "Failed writing session token test file");

        const juce::File sockPcmFile = tmpDir.getChildFile("socket_test.pcm");
        {
            std::vector<std::uint8_t> pcm(44096u * 4u, 0x11u);
            REQUIRE(sockPcmFile.replaceWithData(pcm.data(), pcm.size()),
                    "Failed writing socket PCM payload");
        }
        const juce::File sockOmaFile = tmpDir.getChildFile("socket_dump.oma");

        (void)::chmod(tokFile.getFullPathName().toRawUTF8(), 0600);
        const std::string sockPath =
            "/tmp/carta-himd-test-" + std::to_string(static_cast<long long>(::getpid())) + "-" +
            std::to_string(static_cast<unsigned long long>(juce::Time::getHighResolutionTicks())) +
            ".sock";
        (void)::unlink(sockPath.c_str());

        const std::string uidStr = std::to_string(static_cast<unsigned long>(::getuid()));
        const std::string gidStr = std::to_string(static_cast<unsigned long>(::getgid()));
        const std::string pidStr = std::to_string(static_cast<long long>(::getpid()));
        const std::string tokPath = tokFile.getFullPathName().toStdString();

        const pid_t child = ::fork();
        REQUIRE(child >= 0, "fork() failed spawning carta-himd --socket test server");
        if (child == 0) {
            char* argv[] = {const_cast<char*>(helperPath.c_str()),
                            const_cast<char*>("session"),
                            const_cast<char*>("--socket"),
                            const_cast<char*>(sockPath.c_str()),
                            const_cast<char*>("--client-uid"),
                            const_cast<char*>(uidStr.c_str()),
                            const_cast<char*>("--client-gid"),
                            const_cast<char*>(gidStr.c_str()),
                            const_cast<char*>("--token-file"),
                            const_cast<char*>(tokPath.c_str()),
                            const_cast<char*>("--parent-pid"),
                            const_cast<char*>(pidStr.c_str()),
                            const_cast<char*>("--mock"),
                            nullptr};
            ::execv(helperPath.c_str(), argv);
            ::_exit(127);
        }

        auto connectSock = [&]() -> int {
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 8000.0;
            while (juce::Time::getMillisecondCounterHiRes() < deadline) {
                const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd < 0) {
                    return -1;
                }
                sockaddr_un addr{};
                addr.sun_family = AF_UNIX;
                std::memcpy(addr.sun_path, sockPath.c_str(), sockPath.size() + 1u);
                if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
                    return fd;
                }
                ::close(fd);
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
            return -1;
        };

        // First client rejects invalid auth token and verifies token file was unlinked on startup.
        const int badFd = connectSock();
        REQUIRE(badFd >= 0, "Failed connecting first client to --socket session");
        REQUIRE(!tokFile.existsAsFile(), "Token file must be removed immediately after server read");
        std::string carry;
        juce::var authBadRes;
        std::string sockErr;
        REQUIRE(!sendSocketCommand(badFd, carry, "auth\twrongtoken\n", authBadRes, sockErr) &&
                    sockErr.find("Unauthorized") != std::string::npos,
                "Wrong token on --socket session should be rejected: " + sockErr);
        ::close(badFd);

        // Second client authenticates with valid token and exercises commands + dump + quit.
        const int goodFd = connectSock();
        REQUIRE(goodFd >= 0, "Failed connecting authenticated client after bad-token rejection");
        carry.clear();
        juce::var okRes;
        REQUIRE(sendSocketCommand(goodFd, carry, "auth\t" + secret + "\n", okRes, sockErr),
                "Valid auth token failed: " + sockErr);
        REQUIRE(sendSocketCommand(goodFd, carry, "probe\tmock%3Amz-rh1\n", okRes, sockErr),
                "Socket probe failed: " + sockErr);
        REQUIRE(okRes.getDynamicObject() != nullptr &&
                    static_cast<int>(okRes.getDynamicObject()
                                         ->getProperty("status")
                                         .getDynamicObject()
                                         ->getProperty("trackCount")) == 0,
                "Expected blank mock trackCount over --socket session");

        const std::string encPcm = percentEncodeForSession(sockPcmFile.getFullPathName().toStdString());
        const std::string encOma = percentEncodeForSession(sockOmaFile.getFullPathName().toStdString());
        REQUIRE(sendSocketCommand(goodFd, carry,
                                  "upload\tmock%3Amz-rh1\tpcm\t" + encPcm +
                                      "\tSocket%20Tr0\tSocket%20Artist\tSocket%20Album\t0\n",
                                  okRes, sockErr),
                "Socket upload failed: " + sockErr);
        REQUIRE(sendSocketCommand(goodFd, carry, "dump\tmock%3Amz-rh1\t0\t" + encOma + "\t0\n",
                                  okRes, sockErr),
                "Socket dump failed: " + sockErr);
        std::vector<std::uint8_t> dumped;
        REQUIRE(sockOmaFile.existsAsFile() && readFileBytes(sockOmaFile, dumped, sockErr) &&
                    dumped.size() == 44u + 44096u * 4u &&
                    std::memcmp(dumped.data(), "RIFF", 4) == 0 &&
                    std::memcmp(dumped.data() + 8, "WAVE", 4) == 0,
                "Expected valid WAV/RIFF dump for PCM track over --socket session");
        REQUIRE(sendSocketCommand(goodFd, carry, "rename-disc\tmock%3Amz-rh1\tSocket%20Disc\t0\n",
                                  okRes, sockErr),
                "Socket rename-disc failed: " + sockErr);
        REQUIRE(sendSocketCommand(goodFd, carry, "wipe\tmock%3Amz-rh1\t0\t0\n", okRes, sockErr),
                "Socket wipe failed: " + sockErr);
        REQUIRE(sendSocketCommand(goodFd, carry, "quit\n", okRes, sockErr),
                "Socket quit failed: " + sockErr);
        ::close(goodFd);

        int st = 0;
        REQUIRE(::waitpid(child, &st, 0) == child && WIFEXITED(st) && WEXITSTATUS(st) == 0,
                "carta-himd --socket server did not exit cleanly with code 0");
        REQUIRE(!juce::File(juce::String::fromUTF8(sockPath.c_str())).exists(),
                "Socket file must be unlinked cleanly when server exits");
    }
#endif

    // 10. HiMdBridge CARTA_HIMD_ELEVATION=direct end-to-end session + temp-copy dump test.
    {
        struct ScopedDirectCleanup {
            ~ScopedDirectCleanup() {
                std::string ignoreErr;
                carta::stopHiMdElevatedSession();
                carta::setHiMdTestMockSessionEnabled(false, ignoreErr);
            }
        } directCleanup;
        ScopedElevationEnv env("direct");

        REQUIRE(carta::setHiMdTestMockSessionEnabled(true, err) &&
                    carta::resetHiMdTestMockSession(err),
                "Failed initializing mock before direct elevation test: " + err);
        REQUIRE(carta::ensureHiMdElevatedSession(err) && carta::isHiMdElevatedSessionActive(),
                "ensureHiMdElevatedSession(direct) failed: " + err);

        const std::vector<int> seg{44100};
        const auto sess = makeFiveTrackHiMdSession(seg);
        carta::MdDisc d;
        d.medium = carta::MdMedium::himd300;
        d.defaultCodec = carta::MdCodecMode::hiSp;
        d.halfWidthTitle = "Direct Session";
        carta::MdTrack tr;
        tr.id = carta::makeMdTrackId();
        tr.halfWidthTitle = "Direct Track";
        tr.artist = "Direct Artist";
        tr.album = "Direct Album";
        tr.parts = {{0, 44100}};
        d.tracks.push_back(std::move(tr));

        carta::HiMdRecordResult directRecRes;
        REQUIRE(carta::recordHiMdDisc(sess, d, "mock:mz-rh1", false, directRecRes, err) &&
                    directRecRes.ok,
                "recordHiMdDisc over direct socket/pipe session failed: " + err);
        carta::HiMdDeviceStatus directStatus;
        REQUIRE(carta::probeHiMdDevice("mock:mz-rh1", directStatus, err) &&
                    directStatus.trackCount == 1 && directStatus.discTitle == "Direct Session",
                "probeHiMdDevice over direct socket/pipe session mismatch: " + err);

        const juce::File directOma = tmpDir.getChildFile("direct_session_dump.oma");
        REQUIRE(carta::dumpHiMdTrack("mock:mz-rh1", 0, directOma.getFullPathName().toStdString(),
                                     false, err),
                "dumpHiMdTrack over direct socket/pipe session failed: " + err);
        std::vector<std::uint8_t> directBytes;
        REQUIRE(directOma.existsAsFile() && readFileBytes(directOma, directBytes, err) &&
                    directBytes.size() > 96u && directBytes[0] == 'E' && directBytes[1] == 'A' &&
                    directBytes[2] == '3',
                "Expected valid EA3 .oma file copied from direct elevated session");

        carta::stopHiMdElevatedSession();
        REQUIRE(!carta::isHiMdElevatedSessionActive(),
                "stopHiMdElevatedSession should clear active session flag");
    }

    std::cout << "[HiMdBridgeTests] All Hi-MD bridge, payload encoder, guard, and lifecycle tests passed.\n";
    return 0;
}
