/**
 * @file HiMdTests.cpp
 * @brief End-to-end offline tests for the standalone GPL-2.0 `carta-himd`
 * Porffor helper (`himd-js` + `nufatfs` + `node-mass-storage`) against Mock MZ-RH1.
 *
 * Verifies:
 * 1. Device discovery and clean FAT16 probe (`mock:mz-rh1`).
 * 2. Real codec (`carta5` Hi-SP/Hi-LP, `carta3` LP2/LP4) + BE LPCM streaming
 *    uploads with ASCII, Latin-1, Japanese CP932, and UTF-16BE metadata.
 * 3. Sony 0xA3/0xA4 3-stage mutual auth, EKB 0x00010012 key wrapping, and
 *    monotonic ICV MAC verification.
 * 4. Raw and containerized (`EA3`, `WAV`) track dump bit-for-bit readback.
 * 5. Independent cross-verification using unmodified upstream Node.js `himd-js`
 *    + `nufatfs` directly on the saved 32 MiB FAT16 `.img` and ICV computation.
 * 6. Disc/track rename, move, group rewriting, partial & full track deletion,
 *    and disc wipe/reformat.
 * 7. Hardware write safety confirmation guard (`mock-guard 1`).
 * 8. Constrained 544 KB thread stack safety (`carta-himd selftest-stack`).
 */
#include "MiniDisc/NativeCodecs.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

#define REQUIRE(cond, msg)                                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] " << (msg) << "\n";         \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

std::string percentEncode(const std::string& s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
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

std::uint32_t readLe32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
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

bool writeFileBytes(const juce::File& file, const std::vector<std::uint8_t>& data,
                    std::string& error) {
    if (!file.replaceWithData(data.data(), data.size())) {
        error = "Failed writing file: " + file.getFullPathName().toStdString();
        return false;
    }
    return true;
}

class HiMdMockSession {
  public:
    ~HiMdMockSession() { stop(); }

    bool start(const std::string& helperPath, std::string& error) {
        stop();
#if defined(_WIN32)
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE inRead = nullptr;
        HANDLE inWrite = nullptr;
        HANDLE outRead = nullptr;
        HANDLE outWrite = nullptr;
        if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0)) {
            error = "CreatePipe failed";
            return false;
        }
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inRead;
        si.hStdOutput = outWrite;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        std::string cmdLine = "\"" + helperPath + "\" session --mock";
        if (!CreateProcessA(nullptr, cmdLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                            &si, &pi)) {
            CloseHandle(inRead);
            CloseHandle(inWrite);
            CloseHandle(outRead);
            CloseHandle(outWrite);
            error = "CreateProcessA failed";
            return false;
        }
        CloseHandle(inRead);
        CloseHandle(outWrite);
        CloseHandle(pi.hThread);
        hProc_ = pi.hProcess;
        const int inFd = _open_osfhandle(reinterpret_cast<intptr_t>(inWrite), _O_WRONLY | _O_TEXT);
        const int outFd = _open_osfhandle(reinterpret_cast<intptr_t>(outRead), _O_RDONLY | _O_TEXT);
        stdinFile_ = inFd >= 0 ? _fdopen(inFd, "w") : nullptr;
        stdoutFile_ = outFd >= 0 ? _fdopen(outFd, "r") : nullptr;
        if (!stdinFile_ || !stdoutFile_) {
            stop();
            error = "Failed opening stdio streams on Windows";
            return false;
        }
        return true;
#else
        int inPipe[2] = {-1, -1};
        int outPipe[2] = {-1, -1};
        if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0) {
            if (inPipe[0] >= 0) ::close(inPipe[0]);
            if (inPipe[1] >= 0) ::close(inPipe[1]);
            if (outPipe[0] >= 0) ::close(outPipe[0]);
            if (outPipe[1] >= 0) ::close(outPipe[1]);
            error = "Failed creating pipes for carta-himd";
            return false;
        }
        const pid_t child = ::fork();
        if (child < 0) {
            ::close(inPipe[0]);
            ::close(inPipe[1]);
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            error = "Failed forking carta-himd";
            return false;
        }
        if (child == 0) {
            ::dup2(inPipe[0], STDIN_FILENO);
            ::dup2(outPipe[1], STDOUT_FILENO);
            ::close(inPipe[0]);
            ::close(inPipe[1]);
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            char* argv[] = {const_cast<char*>(helperPath.c_str()), const_cast<char*>("session"),
                            const_cast<char*>("--mock"), nullptr};
            ::execv(helperPath.c_str(), argv);
            ::_exit(127);
        }
        ::close(inPipe[0]);
        ::close(outPipe[1]);
        pid_ = child;
        stdinFile_ = ::fdopen(inPipe[1], "w");
        stdoutFile_ = ::fdopen(outPipe[0], "r");
        if (!stdinFile_ || !stdoutFile_) {
            stop();
            error = "Failed opening stdio streams for carta-himd";
            return false;
        }
        return true;
#endif
    }

    void stop() {
        if (stdinFile_ != nullptr) {
            std::fputs("quit\n", stdinFile_);
            std::fflush(stdinFile_);
            std::fclose(stdinFile_);
            stdinFile_ = nullptr;
        }
        if (stdoutFile_ != nullptr) {
            std::fclose(stdoutFile_);
            stdoutFile_ = nullptr;
        }
#if defined(_WIN32)
        if (hProc_ != nullptr) {
            WaitForSingleObject(hProc_, 5000);
            CloseHandle(hProc_);
            hProc_ = nullptr;
        }
#else
        if (pid_ > 0) {
            int st = 0;
            ::waitpid(pid_, &st, 0);
            pid_ = -1;
        }
#endif
    }

    bool send(const std::vector<std::string>& fields, juce::var& outResult, std::string& error,
              bool* outSawProgress = nullptr) {
        outResult = juce::var();
        if (outSawProgress) {
            *outSawProgress = false;
        }
        if (!stdinFile_ || !stdoutFile_) {
            error = "Mock session not running";
            return false;
        }
        std::string line;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) {
                line.push_back('\t');
            }
            line += (i == 0 ? fields[i] : percentEncode(fields[i]));
        }
        line.push_back('\n');
        if (std::fputs(line.c_str(), stdinFile_) == EOF || std::fflush(stdinFile_) != 0) {
            error = "Failed writing command to carta-himd session";
            return false;
        }

        std::string buf;
        char chunk[4096];
        while (std::fgets(chunk, sizeof(chunk), stdoutFile_) != nullptr) {
            buf += chunk;
            if (buf.empty() || buf.back() != '\n') {
                continue;
            }
            while (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r')) {
                buf.pop_back();
            }
            if (buf.empty()) {
                continue;
            }
            juce::var parsed;
            if (juce::JSON::parse(juce::String::fromUTF8(buf.c_str()), parsed).failed() ||
                !parsed.isObject()) {
                error = "Malformed JSON line from carta-himd: " + buf;
                return false;
            }
            const auto* obj = parsed.getDynamicObject();
            const auto ev = obj->getProperty("event").toString();
            if (ev == "progress") {
                if (outSawProgress) {
                    *outSawProgress = true;
                }
                buf.clear();
                continue;
            }
            if (ev == "result") {
                outResult = parsed;
                const bool ok = static_cast<bool>(obj->getProperty("ok"));
                if (!ok) {
                    error = obj->getProperty("error").toString().toStdString();
                    return false;
                }
                return true;
            }
            buf.clear();
        }
        error = "carta-himd session closed unexpectedly";
        return false;
    }

  private:
#if defined(_WIN32)
    HANDLE hProc_ = nullptr;
#else
    pid_t pid_ = -1;
#endif
    std::FILE* stdinFile_ = nullptr;
    std::FILE* stdoutFile_ = nullptr;
};

bool encodeTestTrackWire(const std::string& mode, int trackSeed, int sampleCount,
                         std::vector<std::uint8_t>& outWire, std::string& error) {
    outWire.clear();
    std::vector<float> left(static_cast<std::size_t>(sampleCount));
    std::vector<float> right(static_cast<std::size_t>(sampleCount));
    const double twoPi = 6.28318530717958647692;
    const double fL = 220.0 + 77.0 * trackSeed;
    const double fR = 330.0 + 91.0 * trackSeed;
    for (int i = 0; i < sampleCount; ++i) {
        left[static_cast<std::size_t>(i)] =
            0.40f * static_cast<float>(std::sin(twoPi * fL * static_cast<double>(i) / 44100.0));
        right[static_cast<std::size_t>(i)] =
            0.35f * static_cast<float>(std::sin(twoPi * fR * static_cast<double>(i) / 44100.0));
    }
    std::vector<std::uint8_t> container;
    if (!carta::encodeAtracContainer(mode, left.data(), right.data(), 2, sampleCount, container,
                                     error)) {
        return false;
    }
    return carta::extractNetMdWirePayload(mode, container, outWire, error);
}

std::vector<std::uint8_t> makeBigEndianPcmPayload(std::size_t bytes) {
    std::vector<std::uint8_t> out(bytes);
    for (std::size_t i = 0; i < bytes; i += 4) {
        const std::uint32_t idx = static_cast<std::uint32_t>(i / 4u);
        const std::int16_t sL = static_cast<std::int16_t>((idx * 173u + 500u) & 0x7FFFu);
        const std::int16_t sR = static_cast<std::int16_t>(0 - ((idx * 197u + 300u) & 0x3FFFu));
        const auto uL = static_cast<std::uint16_t>(sL);
        const auto uR = static_cast<std::uint16_t>(sR);
        out[i + 0] = static_cast<std::uint8_t>((uL >> 8) & 0xFFu);
        out[i + 1] = static_cast<std::uint8_t>(uL & 0xFFu);
        out[i + 2] = static_cast<std::uint8_t>((uR >> 8) & 0xFFu);
        out[i + 3] = static_cast<std::uint8_t>(uR & 0xFFu);
    }
    return out;
}

} // namespace

int main() {
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
#endif
    const std::string himdBin = CARTA_HIMD_BINARY;
    const std::string nodeBin = CARTA_NODE_BINARY;
    const std::string srcDir = CARTA_STUDIO_SOURCE_DIR;
    REQUIRE(juce::File(juce::String::fromUTF8(himdBin.c_str())).existsAsFile(),
            "Missing carta-himd binary at " + himdBin);

    const auto tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("carta_himd_test_" +
                                          juce::String(juce::Time::getHighResolutionTicks()));
    REQUIRE(tmpDir.createDirectory(), "Failed creating test temporary directory");
    struct DirCleanup {
        juce::File dir;
        ~DirCleanup() { dir.deleteRecursively(); }
    } cleanup{tmpDir};

    HiMdMockSession sess;
    std::string err;
    REQUIRE(sess.start(himdBin, err), "Failed starting carta-himd mock session: " + err);

    // 1. Discovery & clean FAT16 probe
    {
        juce::var res;
        REQUIRE(sess.send({"list"}, res, err), "list failed: " + err);
        const auto* devs = res.getDynamicObject()->getProperty("devices").getArray();
        REQUIRE(devs != nullptr, "Missing devices array");
        const juce::DynamicObject* mockDev = nullptr;
        for (const auto& entry : *devs) {
            const auto* obj = entry.getDynamicObject();
            if (obj != nullptr && obj->getProperty("id").toString() == "mock:mz-rh1") {
                mockDev = obj;
                break;
            }
        }
        REQUIRE(mockDev != nullptr, "Expected mock:mz-rh1 in listed Hi-MD devices");
        REQUIRE(static_cast<bool>(mockDev->getProperty("isMock")), "Expected isMock == true");

        REQUIRE(sess.send({"probe", "mock:mz-rh1"}, res, err), "probe failed: " + err);
        const auto* st = res.getDynamicObject()->getProperty("status").getDynamicObject();
        REQUIRE(st != nullptr, "Missing probe status object");
        REQUIRE(st->getProperty("deviceId").toString() == "mock:mz-rh1", "Unexpected deviceId");
        const auto* inq = st->getProperty("inquiry").getDynamicObject();
        REQUIRE(inq != nullptr && inq->getProperty("vid").toString() == "SONY" &&
                    inq->getProperty("pid").toString() == "Hi-MD",
                "Unexpected SCSI inquiry metadata");
        const auto* cap = st->getProperty("capacity").getDynamicObject();
        REQUIRE(cap != nullptr && static_cast<int>(cap->getProperty("blockSize")) == 2048 &&
                    static_cast<juce::int64>(cap->getProperty("deviceSize")) == 33554432LL,
                "Unexpected SCSI capacity geometry");
        const auto* fsObj = st->getProperty("filesystem").getDynamicObject();
        REQUIRE(fsObj != nullptr &&
                    static_cast<juce::int64>(fsObj->getProperty("total")) > 0 &&
                    static_cast<juce::int64>(fsObj->getProperty("left")) > 30000000LL,
                "Unexpected FAT16 filesystem stats");
        REQUIRE(static_cast<int>(st->getProperty("datanum")) == 1, "Expected initial datanum == 1");
        REQUIRE(static_cast<int>(st->getProperty("trackCount")) == 0,
                "Expected initial trackCount == 0");
    }

    // 2. Encode 4 ATRAC modes via Carta Porffor codecs + raw BE PCM, then upload all 5
    struct TrackCase {
        std::string codecSpec;
        std::string expectedEnc;
        int expectedBitrate = 0;
        std::string title;
        std::string artist;
        std::string album;
        std::vector<std::uint8_t> rawPayload;
        juce::File inFile;
    };

    constexpr int kSamples = 131072; // 64 * 2048 (~2.97s at 44.1 kHz)
    std::vector<TrackCase> cases(5);
    cases[0] = {"hi-sp", "A3+", 256, "Hi-SP Track", "Carta", "Test Album", {},
                tmpDir.getChildFile("in0_hisp.bin")};
    cases[1] = {"hi-lp", "A3+", 64, "Caf\xC3\xA9 Hi-LP", "Ren\xC3\xA9", "\xC3\x89t\xC3\xA9", {},
                tmpDir.getChildFile("in1_hilp.bin")};
    cases[2] = {"lp2", "AT3", 132, "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88\xE6\x9B\xB2LP2",
                "\xE8\xA9\xA6\xE9\xA8\x93", "\xE3\x82\xA2\xE3\x83\xAB\xE3\x83\x90\xE3\x83\xA0", {},
                tmpDir.getChildFile("in2_lp2.bin")};
    cases[3] = {"lp4", "AT3", 66, "LP4 Mix \xF0\x9F\x8E\xB5", "Unicode \xCE\x94", "Mix", {},
                tmpDir.getChildFile("in3_lp4.bin")};
    cases[4] = {"pcm", "PCM", 1411, "PCM Master", "Studio", "Raw", {},
                tmpDir.getChildFile("in4_pcm.bin")};

    REQUIRE(encodeTestTrackWire("hi-sp", 0, kSamples, cases[0].rawPayload, err), err);
    REQUIRE(encodeTestTrackWire("hi-lp", 1, kSamples, cases[1].rawPayload, err), err);
    REQUIRE(encodeTestTrackWire("lp2", 2, kSamples, cases[2].rawPayload, err), err);
    REQUIRE(encodeTestTrackWire("lp4", 3, kSamples, cases[3].rawPayload, err), err);
    cases[4].rawPayload = makeBigEndianPcmPayload(97920u); // 6 * 16320 = 3 FAT clusters (1530 virtual frames)

    for (std::size_t i = 0; i < cases.size(); ++i) {
        REQUIRE(!cases[i].rawPayload.empty(), "Empty encoded payload for track " + std::to_string(i));
        REQUIRE(writeFileBytes(cases[i].inFile, cases[i].rawPayload, err), err);

        juce::var res;
        bool sawProg = false;
        REQUIRE(sess.send({"upload", "mock:mz-rh1", cases[i].codecSpec,
                           cases[i].inFile.getFullPathName().toStdString(), cases[i].title,
                           cases[i].artist, cases[i].album, "0"},
                          res, err, &sawProg),
                "upload failed on track " + std::to_string(i) + ": " + err);
        REQUIRE(sawProg, "Expected upload progress events");
        const auto* up = res.getDynamicObject()->getProperty("upload").getDynamicObject();
        REQUIRE(up != nullptr && static_cast<int>(up->getProperty("trackIndex")) == static_cast<int>(i),
                "Unexpected uploaded trackIndex");
    }

    // 3. Verify DRM/ICV snapshot & probed track metadata
    std::string snapDiscIdHex;
    std::string snapLastIcvHex;
    std::string snapHeadKeyHex;
    std::string snapBodyKeyHex;
    {
        juce::var res;
        REQUIRE(sess.send({"mock-snapshot"}, res, err), "mock-snapshot failed: " + err);
        const auto* obj = res.getDynamicObject();
        REQUIRE(static_cast<int>(obj->getProperty("auth_completed_count")) == 5,
                "Expected 5 completed Sony 3-stage auths");
        REQUIRE(static_cast<int>(obj->getProperty("icv_write_count")) == 5,
                "Expected 5 verified ICV writes");
        REQUIRE(static_cast<int>(obj->getProperty("current_generation")) == 6,
                "Expected current_generation == 6");
        REQUIRE(static_cast<int>(obj->getProperty("datanum")) == 6, "Expected datanum == 6");
        REQUIRE(!static_cast<bool>(obj->getProperty("in_session")),
                "Expected in_session == false after finalizeSession");
        snapDiscIdHex = obj->getProperty("disc_id").toString().toStdString();
        snapLastIcvHex = obj->getProperty("last_icv").toString().toStdString();
        snapHeadKeyHex = obj->getProperty("head_key").toString().toStdString();
        snapBodyKeyHex = obj->getProperty("body_key").toString().toStdString();

        REQUIRE(sess.send({"probe", "mock:mz-rh1"}, res, err), "probe after uploads failed: " + err);
        const auto* st = res.getDynamicObject()->getProperty("status").getDynamicObject();
        REQUIRE(static_cast<int>(st->getProperty("trackCount")) == 5, "Expected trackCount == 5");
        REQUIRE(static_cast<int>(st->getProperty("datanum")) == 6, "Expected datanum == 6");
        const auto* tracks = st->getProperty("tracks").getArray();
        REQUIRE(tracks != nullptr && tracks->size() == 5, "Expected 5 tracks in probe response");
        for (int i = 0; i < 5; ++i) {
            const auto* tr = (*tracks)[i].getDynamicObject();
            REQUIRE(tr != nullptr, "Null track object");
            const auto& tc = cases[static_cast<std::size_t>(i)];
            REQUIRE(tr->getProperty("encoding").toString().toStdString() == tc.expectedEnc,
                    "Encoding mismatch on track " + std::to_string(i));
            REQUIRE(static_cast<int>(tr->getProperty("bitrate")) == tc.expectedBitrate,
                    "Bitrate mismatch on track " + std::to_string(i));
            REQUIRE(tr->getProperty("title").toString().toStdString() == tc.title,
                    "Title roundtrip mismatch on track " + std::to_string(i));
            REQUIRE(tr->getProperty("artist").toString().toStdString() == tc.artist,
                    "Artist roundtrip mismatch on track " + std::to_string(i));
            REQUIRE(tr->getProperty("album").toString().toStdString() == tc.album,
                    "Album roundtrip mismatch on track " + std::to_string(i));
        }
    }

    // 4. Raw & Container (`EA3` and `WAV`) dump verification
    for (int i = 0; i < 5; ++i) {
        const auto dumpFile = tmpDir.getChildFile("dump_raw_" + juce::String(i) + ".bin");
        juce::var res;
        REQUIRE(sess.send({"dump", "mock:mz-rh1", std::to_string(i),
                           dumpFile.getFullPathName().toStdString(), "1"},
                          res, err),
                "raw dump failed for track " + std::to_string(i) + ": " + err);
        std::vector<std::uint8_t> got;
        REQUIRE(readFileBytes(dumpFile, got, err), err);
        REQUIRE(got == cases[static_cast<std::size_t>(i)].rawPayload,
                "Raw decrypted payload mismatch on track " + std::to_string(i));
    }

    // Track 0 EA3 container dump
    {
        const auto ea3File = tmpDir.getChildFile("dump_hisp.oma");
        juce::var res;
        REQUIRE(sess.send({"dump", "mock:mz-rh1", "0", ea3File.getFullPathName().toStdString(), "0"},
                          res, err),
                "EA3 container dump failed: " + err);
        std::vector<std::uint8_t> got;
        REQUIRE(readFileBytes(ea3File, got, err), err);
        const auto& exp = cases[0].rawPayload;
        REQUIRE(got.size() == 96u + exp.size(), "Unexpected EA3 container size");
        REQUIRE(got[0] == 'E' && got[1] == 'A' && got[2] == '3' && got[3] == 1 && got[4] == 0 &&
                    got[5] == 0x60,
                "Invalid EA3 header prefix");
        REQUIRE(std::equal(exp.begin(), exp.end(), got.begin() + 96),
                "EA3 container payload bytes mismatch");
    }

    // Track 4 WAV container dump (44-byte header + BE->LE byte-swapped PCM)
    {
        const auto wavFile = tmpDir.getChildFile("dump_pcm.wav");
        juce::var res;
        REQUIRE(sess.send({"dump", "mock:mz-rh1", "4", wavFile.getFullPathName().toStdString(), "0"},
                          res, err),
                "WAV container dump failed: " + err);
        std::vector<std::uint8_t> got;
        REQUIRE(readFileBytes(wavFile, got, err), err);
        const auto& expBe = cases[4].rawPayload;
        REQUIRE(got.size() == 44u + expBe.size(), "Unexpected WAV container size");
        REQUIRE(std::memcmp(got.data(), "RIFF", 4) == 0 &&
                    std::memcmp(got.data() + 8, "WAVE", 4) == 0 &&
                    std::memcmp(got.data() + 36, "data", 4) == 0,
                "Invalid WAV container chunk markers");
        REQUIRE(readLe32(got.data() + 4) == static_cast<std::uint32_t>(36u + expBe.size()),
                "Invalid WAV RIFF chunk size");
        REQUIRE(readLe32(got.data() + 40) == static_cast<std::uint32_t>(expBe.size()),
                "Invalid WAV data chunk size");
        for (std::size_t k = 0; k < expBe.size(); k += 2) {
            REQUIRE(got[44u + k] == expBe[k + 1] && got[44u + k + 1] == expBe[k],
                    "WAV PCM BE->LE byte swap mismatch at offset " + std::to_string(k));
        }
    }

    // 5. Independent cross-verification using UNMODIFIED upstream Node.js `himd-js` + `nufatfs`
    {
        const auto imgFile = tmpDir.getChildFile("himd_verify.img");
        juce::var res;
        REQUIRE(sess.send({"mock-save-image", imgFile.getFullPathName().toStdString()}, res, err),
                "mock-save-image failed: " + err);

        const auto verifierScript = tmpDir.getChildFile("verify_upstream_himd.mjs");
        const juce::File depsModules = juce::File(juce::String::fromUTF8(srcDir.c_str()))
                                           .getChildFile("build/porffor/himd-deps/node_modules");

        std::ostringstream js;
        js << "import fs from 'node:fs'\n"
           << "import { createRequire } from 'node:module'\n"
           << "const require = createRequire(" << juce::JSON::toString(depsModules.getChildFile("_anchor.js").getFullPathName()).toStdString() << ")\n"
           << "const himdJs = require(" << juce::JSON::toString(depsModules.getChildFile("himd-js/dist/index.js").getFullPathName()).toStdString() << ")\n"
           << "const nufatfs = require(" << juce::JSON::toString(depsModules.getChildFile("nufatfs/dist/index.js").getFullPathName()).toStdString() << ")\n"
           << "const imgBuf = fs.readFileSync(" << juce::JSON::toString(imgFile.getFullPathName()).toStdString() << ")\n"
           << "const sectorSize = 2048\n"
           << "const driver = {\n"
           << "  sectorSize,\n"
           << "  numSectors: Math.floor(imgBuf.length / sectorSize),\n"
           << "  async readSectors(idx, cnt) {\n"
           << "    const slice = imgBuf.subarray(idx * sectorSize, (idx + cnt) * sectorSize)\n"
           << "    return new Uint8Array(slice)\n"
           << "  },\n"
           << "  writeSectors: null,\n"
           << "}\n"
           << "const fatfs = await nufatfs.FatFilesystem.create(driver, false)\n"
           << "class RawImgFile {\n"
           << "  constructor(h) { this.h = h }\n"
           << "  get length() { return this.h.length }\n"
           << "  seek(off) { return this.h.seek(off) }\n"
           << "  read(len) { return this.h.read(len === undefined ? this.length : len) }\n"
           << "  async close() { await this.h.close() }\n"
           << "}\n"
           << "class RawImgFs extends himdJs.HiMDFilesystem {\n"
           << "  async _list(p) {\n"
           << "    const list = await fatfs.listDir('/' + p.replace(/^\\/+/, ''))\n"
           << "    if (!list) return []\n"
           << "    return list.map(f => ({\n"
           << "      name: ('/' + p.replace(/^\\/+/, '') + '/' + f.replace(/\\/$/, '')).replace(/\\/+/, '/'),\n"
           << "      type: f.endsWith('/') ? 'directory' : 'file'\n"
           << "    }))\n"
           << "  }\n"
           << "  async open(fp) {\n"
           << "    const valid = await this.transformToValidCase(fp)\n"
           << "    const h = await fatfs.open(valid, false)\n"
           << "    if (!h) throw new Error('Cannot open ' + valid)\n"
           << "    return new RawImgFile(h)\n"
           << "  }\n"
           << "  getSize(p) { return fatfs.getSizeOf(p) }\n"
           << "  getTotalSpace() { return imgBuf.length }\n"
           << "  getName() { return 'Raw Image' }\n"
           << "}\n"
           << "const himd = await himdJs.HiMD.init(new RawImgFs())\n"
           << "if (himd.datanum !== 6) throw new Error('Expected datanum=6, got ' + himd.datanum)\n"
           << "const tracks = himdJs.getAllTracks(himd)\n"
           << "if (tracks.length !== 5) throw new Error('Expected 5 tracks, got ' + tracks.length)\n"
           << "const expected = [\n";
        for (int i = 0; i < 5; ++i) {
            const auto& tc = cases[static_cast<std::size_t>(i)];
            js << "  { file: " << juce::JSON::toString(tc.inFile.getFullPathName()).toStdString()
               << ", enc: " << juce::JSON::toString(juce::String::fromUTF8(tc.expectedEnc.c_str())).toStdString()
               << ", bitrate: " << tc.expectedBitrate
               << ", title: " << juce::JSON::toString(juce::String::fromUTF8(tc.title.c_str())).toStdString()
               << ", artist: " << juce::JSON::toString(juce::String::fromUTF8(tc.artist.c_str())).toStdString()
               << ", album: " << juce::JSON::toString(juce::String::fromUTF8(tc.album.c_str())).toStdString()
               << " },\n";
        }
        js << "]\n"
           << "for (let i = 0; i < expected.length; i++) {\n"
           << "  const tr = tracks[i], ex = expected[i]\n"
           << "  if (tr.encoding !== ex.enc || tr.bitrate !== ex.bitrate || tr.title !== ex.title || tr.artist !== ex.artist || tr.album !== ex.album) {\n"
           << "    throw new Error('Metadata mismatch at ' + i + ': ' + JSON.stringify(tr) + ' vs ' + JSON.stringify(ex))\n"
           << "  }\n"
           << "  const slot = himd.trackIndexToTrackSlot(i)\n"
           << "  const st = await himd.openNonMP3Stream(slot)\n"
           << "  const parts = []\n"
           << "  let b\n"
           << "  while ((b = await st.readBlock()) !== null) parts.push(Buffer.from(b.block))\n"
           << "  await st.blockStream.close()\n"
           << "  const got = Buffer.concat(parts)\n"
           << "  const want = fs.readFileSync(ex.file)\n"
           << "  if (Buffer.compare(got, want) !== 0) {\n"
           << "    throw new Error('Upstream audio decryption mismatch at track ' + i + ': got ' + got.length + ' bytes vs want ' + want.length)\n"
           << "  }\n"
           << "}\n"
           << "const mclistH = await himd.openMaclistForReading()\n"
           << "const mclistRaw = await mclistH.read()\n"
           << "await mclistH.close()\n"
           << "const { decryptMaclistKey, retailMac } = require(" << juce::JSON::toString(depsModules.getChildFile("himd-js/dist/encryption.js").getFullPathName()).toStdString() << ")\n"
           << "const headKey = decryptMaclistKey(mclistRaw.subarray(0x10, 0x20))\n"
           << "const bodyKey = decryptMaclistKey(mclistRaw.subarray(0x60, 0x70))\n"
           << "if (Buffer.from(headKey).toString('hex') !== " << juce::JSON::toString(juce::String(snapHeadKeyHex)).toStdString() << ") throw new Error('headKey mismatch')\n"
           << "if (Buffer.from(bodyKey).toString('hex') !== " << juce::JSON::toString(juce::String(snapBodyKeyHex)).toStdString() << ") throw new Error('bodyKey mismatch')\n"
           << "const genBytes = mclistRaw.subarray(0x20, 0x24)\n"
           << "const discId = mclistRaw.subarray(0x40, 0x50)\n"
           << "const allMacs = mclistRaw.subarray(0x70, 0x70 + 32000)\n"
           << "if (Buffer.from(discId).toString('hex') !== " << juce::JSON::toString(juce::String(snapDiscIdHex)).toStdString() << ") throw new Error('discId mismatch')\n"
           << "const head = Buffer.concat([genBytes, Buffer.alloc(20), Buffer.from([0,1,0,0x12]), Buffer.alloc(4), discId, Buffer.alloc(16)])\n"
           << "const icv = Buffer.concat([Buffer.from(retailMac(head, headKey)), Buffer.from(retailMac(allMacs, bodyKey))]).toString('hex')\n"
           << "if (icv !== " << juce::JSON::toString(juce::String(snapLastIcvHex)).toStdString() << ") throw new Error('ICV mismatch: ' + icv + ' vs expected')\n"
           << "console.log('UPSTREAM_HIMD_JS_VERIFY_OK')\n";

        REQUIRE(verifierScript.replaceWithText(juce::String::fromUTF8(js.str().c_str())),
                "Failed writing upstream verification script");

        juce::ChildProcess nodeProc;
        juce::StringArray cmd{juce::String::fromUTF8(nodeBin.c_str()),
                              verifierScript.getFullPathName()};
        REQUIRE(nodeProc.start(cmd), "Failed launching node for upstream himd-js check");
        const auto nodeOut = nodeProc.readAllProcessOutput();
        REQUIRE(nodeProc.waitForProcessToFinish(30000) && nodeProc.getExitCode() == 0,
                "Upstream Node.js himd-js check failed:\n" + nodeOut.toStdString());
        REQUIRE(nodeOut.contains("UPSTREAM_HIMD_JS_VERIFY_OK"),
                "Unexpected Node.js verifier output: " + nodeOut.toStdString());
    }

    // 6. Metadata edits, track reordering, groups
    {
        juce::var res;
        REQUIRE(sess.send({"rename-disc", "mock:mz-rh1", "Carta Hi-MD Disc", "0"}, res, err),
                "rename-disc failed: " + err);
        REQUIRE(sess.send({"rename-track", "mock:mz-rh1", "0", "1", "Renamed Hi-SP", "0", "", "0",
                           "", "0"},
                          res, err),
                "rename-track failed: " + err);
        REQUIRE(sess.send({"move-track", "mock:mz-rh1", "4", "0", "0"}, res, err),
                "move-track failed: " + err);
        REQUIRE(sess.send({"set-groups", "mock:mz-rh1",
                           "[{\"title\":\"Side A\",\"indices\":[0,1]},{\"title\":\"Side B\",\"indices\":[2,3,4]}]",
                           "0"},
                          res, err),
                "set-groups failed: " + err);

        REQUIRE(sess.send({"probe", "mock:mz-rh1"}, res, err), "probe after metadata edits failed");
        const auto* st = res.getDynamicObject()->getProperty("status").getDynamicObject();
        REQUIRE(st->getProperty("discTitle").toString() == "Carta Hi-MD Disc",
                "Expected updated discTitle");
        const auto* tracks = st->getProperty("tracks").getArray();
        REQUIRE(tracks != nullptr && tracks->size() == 5, "Expected 5 tracks after move-track");
        REQUIRE((*tracks)[0].getDynamicObject()->getProperty("title").toString() == "PCM Master",
                "Expected PCM Master moved to index 0");
        REQUIRE((*tracks)[1].getDynamicObject()->getProperty("title").toString() == "Renamed Hi-SP",
                "Expected Renamed Hi-SP at index 1");
        REQUIRE((*tracks)[1].getDynamicObject()->getProperty("artist").toString() == "Carta",
                "Expected rename-track to preserve unchanged artist");
        REQUIRE((*tracks)[1].getDynamicObject()->getProperty("album").toString() == "Test Album",
                "Expected rename-track to preserve unchanged album");

        const auto* groups = st->getProperty("groups").getArray();
        REQUIRE(groups != nullptr && groups->size() >= 2, "Expected >= 2 groups returned");
        bool sawSideA = false;
        bool sawSideB = false;
        for (int i = 0; i < groups->size(); ++i) {
            const auto* g = (*groups)[i].getDynamicObject();
            if (!g) {
                continue;
            }
            const auto gTitle = g->getProperty("title").toString();
            const int gStart = static_cast<int>(g->getProperty("startIndex"));
            const int gCount = static_cast<int>(g->getProperty("trackCount"));
            if (gTitle == "Side A" && gStart == 0 && gCount == 2) {
                sawSideA = true;
            } else if (gTitle == "Side B" && gStart == 2 && gCount == 3) {
                sawSideB = true;
            }
        }
        REQUIRE(sawSideA && sawSideB, "Group layout mismatch after set-groups");
    }

    // 7. Track deletion: delete multi-cluster middle track (index 1 = Hi-SP) then all remaining
    {
        juce::var res;
        REQUIRE(sess.send({"delete-tracks", "mock:mz-rh1", "1", "0"}, res, err),
                "delete-tracks (1) failed: " + err);
        REQUIRE(sess.send({"mock-snapshot"}, res, err), "mock-snapshot failed");
        REQUIRE(static_cast<int>(res.getDynamicObject()->getProperty("icv_write_count")) == 6,
                "Expected icv_write_count == 6 after partial delete");

        REQUIRE(sess.send({"probe", "mock:mz-rh1"}, res, err), "probe after delete(1) failed");
        REQUIRE(static_cast<int>(res.getDynamicObject()->getProperty("status").getDynamicObject()->getProperty("trackCount")) == 4,
                "Expected 4 tracks remaining");

        // Remaining order: [PCM(case 4), Hi-LP(case 1), LP2(case 2), LP4(case 3)]
        const int expectedCaseIdx[4] = {4, 1, 2, 3};
        for (int i = 0; i < 4; ++i) {
            const auto f = tmpDir.getChildFile("after_del_" + juce::String(i) + ".bin");
            REQUIRE(sess.send({"dump", "mock:mz-rh1", std::to_string(i),
                               f.getFullPathName().toStdString(), "1"},
                              res, err),
                    "dump after partial delete failed");
            std::vector<std::uint8_t> got;
            REQUIRE(readFileBytes(f, got, err), err);
            REQUIRE(got == cases[static_cast<std::size_t>(expectedCaseIdx[i])].rawPayload,
                    "Post-deletion audio shift corruption at track " + std::to_string(i));
        }

        // Delete all remaining tracks (empty ATDATA clusterChain edge case)
        REQUIRE(sess.send({"delete-tracks", "mock:mz-rh1", "0,1,2,3", "0"}, res, err),
                "delete-tracks (0,1,2,3) failed: " + err);
        REQUIRE(sess.send({"mock-snapshot"}, res, err), "mock-snapshot failed");
        REQUIRE(static_cast<int>(res.getDynamicObject()->getProperty("icv_write_count")) == 7,
                "Expected icv_write_count == 7 after full delete");
        REQUIRE(sess.send({"probe", "mock:mz-rh1"}, res, err), "probe after full delete failed");
        REQUIRE(static_cast<int>(res.getDynamicObject()->getProperty("status").getDynamicObject()->getProperty("trackCount")) == 0,
                "Expected trackCount == 0 after deleting all tracks");
    }

    // 8. Wipe & Reformat
    {
        juce::var res;
        REQUIRE(sess.send({"upload", "mock:mz-rh1", "pcm",
                           cases[4].inFile.getFullPathName().toStdString(), "Before Wipe", "", "",
                           "0"},
                          res, err),
                "upload before wipe failed: " + err);
        REQUIRE(sess.send({"wipe", "mock:mz-rh1", "0", "0"}, res, err), "wipe(0) failed: " + err);
        REQUIRE(sess.send({"mock-snapshot"}, res, err), "mock-snapshot failed");
        auto* snap = res.getDynamicObject();
        REQUIRE(static_cast<int>(snap->getProperty("wipe_count")) == 1, "Expected wipe_count == 1");
        REQUIRE(static_cast<int>(snap->getProperty("datanum")) == 1, "Expected datanum == 1 after wipe");
        REQUIRE(static_cast<juce::int64>(snap->getProperty("slept_ms")) >= 200,
                "Expected awaitSystemReady sleep polling during wipe");
        REQUIRE(sess.send({"probe", "mock:mz-rh1"}, res, err), "probe after wipe(0) failed");
        REQUIRE(static_cast<int>(res.getDynamicObject()->getProperty("status").getDynamicObject()->getProperty("trackCount")) == 0,
                "Expected 0 tracks after wipe(0)");

        REQUIRE(sess.send({"upload", "mock:mz-rh1", "pcm",
                           cases[4].inFile.getFullPathName().toStdString(), "Before Reformat", "",
                           "", "0"},
                          res, err),
                "upload before reformat failed: " + err);
        REQUIRE(sess.send({"wipe", "mock:mz-rh1", "1", "0"}, res, err),
                "wipe(1) reformat failed: " + err);
        REQUIRE(sess.send({"mock-snapshot"}, res, err), "mock-snapshot failed");
        snap = res.getDynamicObject();
        REQUIRE(static_cast<int>(snap->getProperty("wipe_count")) == 2, "Expected wipe_count == 2");
        REQUIRE(static_cast<int>(snap->getProperty("datanum")) == 1,
                "Expected datanum == 1 after reformat");
        REQUIRE(sess.send({"probe", "mock:mz-rh1"}, res, err), "probe after wipe(1) failed");
        REQUIRE(static_cast<int>(res.getDynamicObject()->getProperty("status").getDynamicObject()->getProperty("trackCount")) == 0,
                "Expected 0 tracks after reformat");
    }

    // 9. Hardware write confirmation safety guard
    {
        juce::var res;
        REQUIRE(sess.send({"mock-guard", "1"}, res, err), "mock-guard 1 failed: " + err);
        REQUIRE(sess.send({"mock-snapshot"}, res, err), "mock-snapshot failed");
        const int beforeSectorsWritten =
            static_cast<int>(res.getDynamicObject()->getProperty("sectors_written"));
        const int beforeIcvWrites =
            static_cast<int>(res.getDynamicObject()->getProperty("icv_write_count"));

        std::string gErr;
        REQUIRE(!sess.send({"upload", "mock:mz-rh1", "pcm",
                            cases[4].inFile.getFullPathName().toStdString(), "Blocked", "", "", "0"},
                           res, gErr) &&
                    gErr.find("confirmation") != std::string::npos,
                "Expected unconfirmed upload to be blocked");
        gErr.clear();
        REQUIRE(!sess.send({"rename-disc", "mock:mz-rh1", "Blocked Disc", "0"}, res, gErr) &&
                    gErr.find("confirmation") != std::string::npos,
                "Expected unconfirmed rename-disc to be blocked");
        gErr.clear();
        REQUIRE(!sess.send({"delete-tracks", "mock:mz-rh1", "0", "0"}, res, gErr) &&
                    gErr.find("confirmation") != std::string::npos,
                "Expected unconfirmed delete-tracks to be blocked");
        gErr.clear();
        REQUIRE(!sess.send({"wipe", "mock:mz-rh1", "0", "0"}, res, gErr) &&
                    gErr.find("confirmation") != std::string::npos,
                "Expected unconfirmed wipe to be blocked");

        REQUIRE(sess.send({"mock-snapshot"}, res, err), "mock-snapshot failed");
        REQUIRE(static_cast<int>(res.getDynamicObject()->getProperty("sectors_written")) ==
                    beforeSectorsWritten,
                "Blocked guarded operations must not write sectors");
        REQUIRE(static_cast<int>(res.getDynamicObject()->getProperty("icv_write_count")) ==
                    beforeIcvWrites,
                "Blocked guarded operations must not update ICV");

        REQUIRE(sess.send({"upload", "mock:mz-rh1", "pcm",
                           cases[4].inFile.getFullPathName().toStdString(), "Confirmed", "", "",
                           "1"},
                          res, err),
                "Expected confirmed upload to succeed under guard: " + err);
        REQUIRE(sess.send({"mock-guard", "0"}, res, err), "mock-guard 0 failed: " + err);
    }

    // 10. 240-track, 125-unique-audio, 24-group 24HCLK Mechanism-1 batch commit,
    //     reference-counted deletion & group overwrite check
    {
        juce::var res;
        REQUIRE(sess.send({"mock-reset"}, res, err), "mock-reset failed: " + err);

        const auto hclkDir = tmpDir.getChildFile("hclk_batch");
        REQUIRE(hclkDir.createDirectory(), "Failed creating 24HCLK temp dir");

        juce::Array<juce::var> audioUploads;
        juce::Array<juce::var> playOrder;
        juce::Array<juce::var> groups;
        std::map<std::string, std::vector<std::uint8_t>> expectedPayloads;
        std::int64_t expectedTotalBytes = 0;

        // 4 shared music loops (9 * 2048 = 18,432 samples -> 11 frames -> 2 blocks each)
        for (int m = 0; m < 4; ++m) {
            std::vector<std::uint8_t> wire;
            REQUIRE(encodeTestTrackWire("hi-sp", m, 9 * 2048, wire, err), err);
            const auto f = hclkDir.getChildFile("music_" + juce::String(m) + ".bin");
            REQUIRE(writeFileBytes(f, wire, err), err);
            const std::string id = "music_" + std::to_string(m);
            expectedTotalBytes += static_cast<std::int64_t>(wire.size());
            expectedPayloads.emplace(id, std::move(wire));

            auto* uo = new juce::DynamicObject();
            uo->setProperty("id", juce::String::fromUTF8(id.c_str()));
            uo->setProperty("codec", "hi-sp");
            uo->setProperty("file", f.getFullPathName());
            uo->setProperty("title", juce::String("MUS ") + juce::String(m + 1));
            uo->setProperty("artist", "Maybe Pablo");
            uo->setProperty("album", "24HCLK");
            uo->setProperty("durationSeconds", 0.42);
            audioUploads.add(juce::var(uo));
        }

        // 1 shared bridge ("AND NOW", 2 * 2048 = 4,096 samples -> 4 frames -> 1 block)
        {
            std::vector<std::uint8_t> wire;
            REQUIRE(encodeTestTrackWire("hi-sp", 7, 2 * 2048, wire, err), err);
            const auto f = hclkDir.getChildFile("bridge.bin");
            REQUIRE(writeFileBytes(f, wire, err), err);
            expectedTotalBytes += static_cast<std::int64_t>(wire.size());
            expectedPayloads.emplace("bridge", std::move(wire));

            auto* uo = new juce::DynamicObject();
            uo->setProperty("id", "bridge");
            uo->setProperty("codec", "hi-sp");
            uo->setProperty("file", f.getFullPathName());
            uo->setProperty("title", "AND NOW");
            uo->setProperty("artist", "Nora");
            uo->setProperty("album", "24HCLK");
            uo->setProperty("durationSeconds", 0.18);
            audioUploads.add(juce::var(uo));
        }

        // 120 unique voice announcements + 240 Play Order entries + 24 hour groups
        for (int h = 0; h < 24; ++h) {
            const int groupStart = playOrder.size();
            for (int s = 0; s < 5; ++s) {
                const int seed = 20 + h * 5 + s;
                std::vector<std::uint8_t> wire;
                REQUIRE(encodeTestTrackWire("hi-sp", seed, 2 * 2048, wire, err), err);
                const std::string vid = "v_" + std::to_string(h) + "_" + std::to_string(s);
                const auto f = hclkDir.getChildFile(juce::String::fromUTF8((vid + ".bin").c_str()));
                REQUIRE(writeFileBytes(f, wire, err), err);
                expectedTotalBytes += static_cast<std::int64_t>(wire.size());
                expectedPayloads.emplace(vid, std::move(wire));

                const juce::String vTitle =
                    (s < 4) ? (juce::String::formatted("%02d:%02d", h, s * 15))
                            : (juce::String::formatted("MSG %02d", h));
                auto* uo = new juce::DynamicObject();
                uo->setProperty("id", juce::String::fromUTF8(vid.c_str()));
                uo->setProperty("codec", "hi-sp");
                uo->setProperty("file", f.getFullPathName());
                uo->setProperty("title", vTitle);
                uo->setProperty("artist", "Nora");
                uo->setProperty("album", "24HCLK");
                uo->setProperty("durationSeconds", 0.18);
                audioUploads.add(juce::var(uo));

                auto* pVoice = new juce::DynamicObject();
                pVoice->setProperty("uploadId", juce::String::fromUTF8(vid.c_str()));
                playOrder.add(juce::var(pVoice));

                auto* pSecond = new juce::DynamicObject();
                if (s < 4) {
                    pSecond->setProperty("uploadId", juce::String("music_") + juce::String(s));
                } else {
                    pSecond->setProperty("uploadId", "bridge");
                }
                playOrder.add(juce::var(pSecond));
            }
            auto* go = new juce::DynamicObject();
            go->setProperty("title", juce::String::formatted("%02d:00", h));
            go->setProperty("startIndex", groupStart);
            go->setProperty("endIndex", playOrder.size());
            groups.add(juce::var(go));
        }

        auto* manifestObj = new juce::DynamicObject();
        manifestObj->setProperty("requireEmpty", true);
        manifestObj->setProperty("discTitle", "24HCLK");
        manifestObj->setProperty("audioUploads", juce::var(audioUploads));
        manifestObj->setProperty("playOrder", juce::var(playOrder));
        manifestObj->setProperty("groups", juce::var(groups));

        const auto manifestFile = hclkDir.getChildFile("manifest.json");
        REQUIRE(manifestFile.replaceWithText(juce::JSON::toString(juce::var(manifestObj), true),
                                             false, false, "\n"),
                "Failed writing 24HCLK batch manifest");

        REQUIRE(sess.send({"commit-batch", "mock:mz-rh1",
                           manifestFile.getFullPathName().toStdString(), "0"},
                          res, err),
                "commit-batch 24HCLK failed: " + err);
        const auto* batchObj = res.getDynamicObject()->getProperty("batch").getDynamicObject();
        REQUIRE(batchObj != nullptr && static_cast<int>(batchObj->getProperty("trackCount")) == 240 &&
                    static_cast<int>(batchObj->getProperty("uniqueAudioUploaded")) == 125 &&
                    static_cast<bool>(batchObj->getProperty("icvUpdated")) &&
                    static_cast<juce::int64>(batchObj->getProperty("bytesWritten")) ==
                        expectedTotalBytes,
                "Unexpected commit-batch 24HCLK summary");

        REQUIRE(sess.send({"mock-snapshot"}, res, err), "mock-snapshot after commit-batch failed");
        const int authCountAfterBatch =
            static_cast<int>(res.getDynamicObject()->getProperty("auth_completed_count"));
        const int icvCountAfterBatch =
            static_cast<int>(res.getDynamicObject()->getProperty("icv_write_count"));
        REQUIRE(authCountAfterBatch == 1 && icvCountAfterBatch == 1,
                "24HCLK commit-batch must authenticate and sign ICV in one single session");
        const std::string m1HeadKey =
            res.getDynamicObject()->getProperty("head_key").toString().toStdString();
        const std::string m1BodyKey =
            res.getDynamicObject()->getProperty("body_key").toString().toStdString();
        const std::string m1DiscId =
            res.getDynamicObject()->getProperty("disc_id").toString().toStdString();
        const std::string m1Icv =
            res.getDynamicObject()->getProperty("last_icv").toString().toStdString();

        REQUIRE(sess.send({"probe", "mock:mz-rh1"}, res, err), "probe after commit-batch failed");
        const auto* st = res.getDynamicObject()->getProperty("status").getDynamicObject();
        REQUIRE(static_cast<int>(st->getProperty("trackCount")) == 240 &&
                    st->getProperty("discTitle").toString() == "24HCLK",
                "Expected 240 tracks and discTitle 24HCLK after commit-batch");
        const auto* tracksArr = st->getProperty("tracks").getArray();
        REQUIRE(tracksArr != nullptr && tracksArr->size() == 240, "Expected 240 tracks in probe");
        std::vector<int> uniqueSlots;
        auto hasSlot = [&](int s) {
            for (int x : uniqueSlots) {
                if (x == s) return true;
            }
            return false;
        };
        for (int i = 0; i < 240; ++i) {
            const int s = static_cast<int>((*tracksArr)[i].getDynamicObject()->getProperty("slot"));
            REQUIRE(s >= 1, "Expected valid non-zero slot on Play Order track");
            if (!hasSlot(s)) {
                uniqueSlots.push_back(s);
            }
        }
        REQUIRE(uniqueSlots.size() == 125u, "Expected exactly 125 unique TrackInfo slots for 24HCLK");
        const int mus1Slot = static_cast<int>((*tracksArr)[1].getDynamicObject()->getProperty("slot"));
        const int bridgeSlot =
            static_cast<int>((*tracksArr)[9].getDynamicObject()->getProperty("slot"));
        for (int h = 0; h < 24; ++h) {
            REQUIRE(static_cast<int>((*tracksArr)[h * 10 + 1].getDynamicObject()->getProperty("slot")) ==
                        mus1Slot,
                    "Expected MUS 1 Play Order aliasing across all 24 hours");
            REQUIRE(static_cast<int>((*tracksArr)[h * 10 + 9].getDynamicObject()->getProperty("slot")) ==
                        bridgeSlot,
                    "Expected AND NOW Play Order aliasing across all 24 hours");
        }
        const auto* grpArr = st->getProperty("groups").getArray();
        int nonNegGroups = 0;
        if (grpArr != nullptr) {
            for (const auto& gVar : *grpArr) {
                if (static_cast<int>(gVar.getDynamicObject()->getProperty("groupIndex")) >= 0) {
                    ++nonNegGroups;
                }
            }
        }
        REQUIRE(nonNegGroups == 24, "Expected 24 groups in 24HCLK probe response");

        // Dump aliased tracks (1 vs 231, 9 vs 239) and unique voice tracks (0, 230)
        const std::pair<int, std::string> dumpChecks[] = {
            {0, "v_0_0"},
            {1, "music_0"},
            {231, "music_0"},
            {9, "bridge"},
            {239, "bridge"},
            {230, "v_23_0"},
        };
        for (const auto& dc : dumpChecks) {
            const auto outF = hclkDir.getChildFile("dump_" + juce::String(dc.first) + ".bin");
            REQUIRE(sess.send({"dump", "mock:mz-rh1", std::to_string(dc.first),
                               outF.getFullPathName().toStdString(), "1"},
                              res, err),
                    "24HCLK dump failed at index " + std::to_string(dc.first) + ": " + err);
            std::vector<std::uint8_t> got;
            REQUIRE(readFileBytes(outF, got, err), err);
            REQUIRE(got == expectedPayloads.at(dc.second),
                    "24HCLK audio payload mismatch at track index " + std::to_string(dc.first));
        }

        // Cross-verify 240-track FAT16 image & ICV with unmodified upstream Node.js `himd-js`
        const auto m1ImgFile = hclkDir.getChildFile("himd_24hclk_m1.img");
        REQUIRE(sess.send({"mock-save-image", m1ImgFile.getFullPathName().toStdString()}, res, err),
                "mock-save-image for 24HCLK failed: " + err);
        {
            const juce::File depsModules = juce::File(juce::String::fromUTF8(srcDir.c_str()))
                                               .getChildFile("build/porffor/himd-deps/node_modules");
            const auto vScript = hclkDir.getChildFile("verify_m1_24hclk.mjs");
            std::ostringstream js;
            js << "import fs from 'node:fs'\n"
               << "import { createRequire } from 'node:module'\n"
               << "const require = createRequire(" << juce::JSON::toString(depsModules.getChildFile("_anchor.js").getFullPathName()).toStdString() << ")\n"
               << "const himdJs = require(" << juce::JSON::toString(depsModules.getChildFile("himd-js/dist/index.js").getFullPathName()).toStdString() << ")\n"
               << "const nufatfs = require(" << juce::JSON::toString(depsModules.getChildFile("nufatfs/dist/index.js").getFullPathName()).toStdString() << ")\n"
               << "const { decryptMaclistKey, retailMac } = require(" << juce::JSON::toString(depsModules.getChildFile("himd-js/dist/encryption.js").getFullPathName()).toStdString() << ")\n"
               << "const imgBuf = fs.readFileSync(" << juce::JSON::toString(m1ImgFile.getFullPathName()).toStdString() << ")\n"
               << "const sectorSize = 2048\n"
               << "const fatfs = await nufatfs.FatFilesystem.create({ sectorSize, numSectors: Math.floor(imgBuf.length / sectorSize), async readSectors(i, c) { return new Uint8Array(imgBuf.subarray(i*sectorSize, (i+c)*sectorSize)) }, writeSectors: null }, false)\n"
               << "class F { constructor(h){this.h=h} get length(){return this.h.length} seek(o){return this.h.seek(o)} read(l){return this.h.read(l===undefined?this.length:l)} async close(){await this.h.close()} }\n"
               << "class R extends himdJs.HiMDFilesystem {\n"
               << "  async _list(p){ const l = await fatfs.listDir('/'+p.replace(/^\\/+/, '')); return l ? l.map(f=>({ name:('/'+p.replace(/^\\/+/, '')+'/'+f.replace(/\\/$/, '')).replace(/\\/+/, '/'), type: f.endsWith('/')?'directory':'file' })) : [] }\n"
               << "  async open(fp){ const v = await this.transformToValidCase(fp); const h = await fatfs.open(v, false); if(!h) throw new Error('open '+v); return new F(h) }\n"
               << "  getSize(p){return fatfs.getSizeOf(p)} getTotalSpace(){return imgBuf.length} getName(){return 'Raw'}\n"
               << "}\n"
               << "const himd = await himdJs.HiMD.init(new R())\n"
               << "const trs = himdJs.getAllTracks(himd)\n"
               << "if (trs.length !== 240) throw new Error('Expected 240 tracks, got ' + trs.length)\n"
               << "if (himd.trackIndexToTrackSlot(1) !== himd.trackIndexToTrackSlot(231)) throw new Error('MUS 1 alias slot mismatch')\n"
               << "if (himd.trackIndexToTrackSlot(9) !== himd.trackIndexToTrackSlot(239)) throw new Error('Bridge alias slot mismatch')\n"
               << "const readSlot = async (slot) => { const st = await himd.openNonMP3Stream(slot); const ps = []; let b; while((b = await st.readBlock()) !== null) ps.push(Buffer.from(b.block)); await st.blockStream.close(); return Buffer.concat(ps); }\n"
               << "if (Buffer.compare(await readSlot(himd.trackIndexToTrackSlot(1)), fs.readFileSync(" << juce::JSON::toString(hclkDir.getChildFile("music_0.bin").getFullPathName()).toStdString() << ")) !== 0) throw new Error('music_0 audio mismatch')\n"
               << "if (Buffer.compare(await readSlot(himd.trackIndexToTrackSlot(9)), fs.readFileSync(" << juce::JSON::toString(hclkDir.getChildFile("bridge.bin").getFullPathName()).toStdString() << ")) !== 0) throw new Error('bridge audio mismatch')\n"
               << "if (Buffer.compare(await readSlot(himd.trackIndexToTrackSlot(0)), fs.readFileSync(" << juce::JSON::toString(hclkDir.getChildFile("v_0_0.bin").getFullPathName()).toStdString() << ")) !== 0) throw new Error('v_0_0 audio mismatch')\n"
               << "const mh = await himd.openMaclistForReading(); const mraw = await mh.read(); await mh.close();\n"
               << "const hk = decryptMaclistKey(mraw.subarray(0x10, 0x20)); const bk = decryptMaclistKey(mraw.subarray(0x60, 0x70));\n"
               << "const head = Buffer.concat([mraw.subarray(0x20,0x24), Buffer.alloc(20), Buffer.from([0,1,0,0x12]), Buffer.alloc(4), mraw.subarray(0x40,0x50), Buffer.alloc(16)])\n"
               << "const icv = Buffer.concat([Buffer.from(retailMac(head, hk)), Buffer.from(retailMac(mraw.subarray(0x70, 0x70+32000), bk))]).toString('hex')\n"
               << "if (icv !== " << juce::JSON::toString(juce::String(m1Icv)).toStdString() << ") throw new Error('24HCLK ICV mismatch')\n"
               << "if (Buffer.from(hk).toString('hex') !== " << juce::JSON::toString(juce::String(m1HeadKey)).toStdString() << " || Buffer.from(bk).toString('hex') !== " << juce::JSON::toString(juce::String(m1BodyKey)).toStdString() << " || Buffer.from(mraw.subarray(0x40,0x50)).toString('hex') !== " << juce::JSON::toString(juce::String(m1DiscId)).toStdString() << ") throw new Error('key/discId mismatch')\n"
               << "console.log('UPSTREAM_24HCLK_M1_OK')\n";
            REQUIRE(vScript.replaceWithText(juce::String::fromUTF8(js.str().c_str())),
                    "Failed writing 24HCLK upstream verification script");
            juce::ChildProcess np;
            REQUIRE(np.start(juce::StringArray{juce::String::fromUTF8(nodeBin.c_str()),
                                               vScript.getFullPathName()}),
                    "Failed launching node for 24HCLK check");
            const auto out = np.readAllProcessOutput();
            REQUIRE(np.waitForProcessToFinish(30000) && np.getExitCode() == 0 &&
                        out.contains("UPSTREAM_24HCLK_M1_OK"),
                    "Upstream Node.js 24HCLK M1 verification failed:\n" + out.toStdString());
        }

        // Reference-counted deletion: delete 1 alias of AND NOW (index 9) -> keeps slot & skips ICV bump
        REQUIRE(sess.send({"delete-tracks", "mock:mz-rh1", "9", "0"}, res, err),
                "delete-tracks single alias failed: " + err);
        REQUIRE(sess.send({"mock-snapshot"}, res, err), "mock-snapshot failed");
        REQUIRE(static_cast<int>(res.getDynamicObject()->getProperty("icv_write_count")) == 1,
                "Deleting one alias of a shared slot must NOT free the slot or rewrite ICV");

        REQUIRE(sess.send({"probe", "mock:mz-rh1"}, res, err), "probe after alias delete failed");
        const auto* stAfter1 = res.getDynamicObject()->getProperty("status").getDynamicObject();
        REQUIRE(static_cast<int>(stAfter1->getProperty("trackCount")) == 239,
                "Expected 239 tracks after deleting 1 alias");
        {
            const auto outF = hclkDir.getChildFile("dump_remaining_bridge.bin");
            REQUIRE(sess.send({"dump", "mock:mz-rh1", "238", outF.getFullPathName().toStdString(),
                               "1"},
                              res, err),
                    "dump remaining bridge alias failed: " + err);
            std::vector<std::uint8_t> got;
            REQUIRE(readFileBytes(outF, got, err), err);
            REQUIRE(got == expectedPayloads.at("bridge"),
                    "Remaining AND NOW alias audio corrupted after partial alias delete");
        }

        // Now delete ALL 23 remaining aliases of AND NOW -> frees the shared slot cleanly
        std::ostringstream remCsv;
        for (int h = 1; h < 24; ++h) {
            if (h > 1) remCsv << ",";
            remCsv << (10 * h + 8);
        }
        REQUIRE(sess.send({"delete-tracks", "mock:mz-rh1", remCsv.str(), "0"}, res, err),
                "delete-tracks remaining 23 aliases failed: " + err);
        REQUIRE(sess.send({"mock-snapshot"}, res, err), "mock-snapshot failed");
        REQUIRE(static_cast<int>(res.getDynamicObject()->getProperty("icv_write_count")) == 2,
                "Deleting last references of shared slot must free slot and update ICV");
        REQUIRE(sess.send({"probe", "mock:mz-rh1"}, res, err), "probe after freeing bridge failed");
        REQUIRE(static_cast<int>(res.getDynamicObject()->getProperty("status").getDynamicObject()->getProperty("trackCount")) ==
                    216,
                "Expected 216 remaining tracks after freeing all 24 AND NOW aliases");

        // Remaining tracks (including still-aliased MUS 1 at new index 10 and v_23_0 at index 207) are intact
        {
            const auto outMus = hclkDir.getChildFile("dump_mus1_after_free.bin");
            REQUIRE(sess.send({"dump", "mock:mz-rh1", "10", outMus.getFullPathName().toStdString(),
                               "1"},
                              res, err),
                    "dump MUS 1 after bridge free failed: " + err);
            std::vector<std::uint8_t> gotMus;
            REQUIRE(readFileBytes(outMus, gotMus, err), err);
            REQUIRE(gotMus == expectedPayloads.at("music_0"),
                    "MUS 1 corrupted after freeing AND NOW slot");

            const auto outVoice = hclkDir.getChildFile("dump_v23_after_free.bin");
            REQUIRE(sess.send({"dump", "mock:mz-rh1", "207",
                               outVoice.getFullPathName().toStdString(), "1"},
                              res, err),
                    "dump v_23_0 after bridge free failed: " + err);
            std::vector<std::uint8_t> gotVoice;
            REQUIRE(readFileBytes(outVoice, gotVoice, err), err);
            REQUIRE(gotVoice == expectedPayloads.at("v_23_0"),
                    "v_23_0 shifted fragment offset corrupted after freeing AND NOW slot");
        }

        // Overwrite 24 groups with 2 groups (verifying rewriteGroups reverse deletion fix)
        REQUIRE(sess.send({"set-groups", "mock:mz-rh1",
                           "[{\"title\":\"First\",\"indices\":[0,1,2]},{\"title\":\"Second\",\"indices\":[3,4]}]",
                           "0"},
                          res, err),
                "set-groups overwrite 24->2 failed: " + err);
        REQUIRE(sess.send({"probe", "mock:mz-rh1"}, res, err), "probe after set-groups failed");
        const auto* ovGrps = res.getDynamicObject()
                                 ->getProperty("status")
                                 .getDynamicObject()
                                 ->getProperty("groups")
                                 .getArray();
        int ovCount = 0;
        if (ovGrps != nullptr) {
            for (const auto& gv : *ovGrps) {
                if (static_cast<int>(gv.getDynamicObject()->getProperty("groupIndex")) >= 0) {
                    ++ovCount;
                }
            }
        }
        REQUIRE(ovCount == 2,
                "Expected exactly 2 groups after overwriting 24 groups (got " +
                    std::to_string(ovCount) + ")");
    }

    sess.stop();

    // 11. Constrained 544 KB thread stack self-test (6 MiB PCM roundtrip = 386 Hi-MD blocks)
    {
        juce::ChildProcess stackProc;
        juce::StringArray cmd{juce::String::fromUTF8(himdBin.c_str()), "selftest-stack"};
        REQUIRE(stackProc.start(cmd), "Failed launching carta-himd selftest-stack");
        const auto out = stackProc.readAllProcessOutput();
        REQUIRE(stackProc.waitForProcessToFinish(60000) && stackProc.getExitCode() == 0,
                "carta-himd selftest-stack failed:\n" + out.toStdString());
        REQUIRE(out.contains("\"ok\":true"),
                "Unexpected selftest-stack output: " + out.toStdString());
    }

    std::cout << "[HiMdTests] All 11 offline Hi-MD protocol, 24HCLK Mechanism-1, & upstream cross-verification phases passed.\n";
    return 0;
}
