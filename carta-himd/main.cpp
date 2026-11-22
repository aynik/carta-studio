/**
 * @file main.cpp
 * @brief Standalone GPL-2.0 Hi-MD UMSC / SCSI protocol helper CLI (`carta-himd`).
 *
 * Runs out-of-process from Carta Studio so `himd-js`, `nufatfs`, and
 * `node-mass-storage` (GPL-2.0) remain strictly isolated in a standalone GPL-2.0
 * executable communicating via CLI flags, temporary binary files, and NDJSON
 * progress/result lines on `stdout`.
 */

#include "HiMdHostBridge.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
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
#include <windows.h>
#include <sddl.h>
#else
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

extern "C" {
int himd_pf_list_devices(void);
int himd_pf_probe_device(const char* id_ptr, int id_len);
int himd_pf_upload_track(const char* id_ptr, int id_len, const char* codec_ptr, int codec_len,
                         const char* file_ptr, int file_len, const char* title_ptr, int title_len,
                         const char* artist_ptr, int artist_len, const char* album_ptr,
                         int album_len);
int himd_pf_dump_track(const char* id_ptr, int id_len, int track_index, const char* out_file_ptr,
                       int out_file_len, int raw_audio_only);
int himd_pf_rename_disc(const char* id_ptr, int id_len, const char* title_ptr, int title_len);
int himd_pf_rename_track(const char* id_ptr, int id_len, int track_index, int has_title,
                         const char* title_ptr, int title_len, int has_artist,
                         const char* artist_ptr, int artist_len, int has_album,
                         const char* album_ptr, int album_len);
int himd_pf_move_track(const char* id_ptr, int id_len, int from_index, int to_index);
int himd_pf_set_groups(const char* id_ptr, int id_len, const char* groups_json_ptr,
                       int groups_json_len);
int himd_pf_delete_tracks(const char* id_ptr, int id_len, const char* indices_json_ptr,
                          int indices_json_len);
int himd_pf_wipe_disc(const char* id_ptr, int id_len, int reformat);
int himd_pf_commit_batch(const char* id_ptr, int id_len, const char* manifest_ptr,
                         int manifest_len);
const unsigned char* himd_pf_output_ptr(void);
std::size_t himd_pf_output_len(void);
const char* himd_pf_last_error(void);
void himd_pf_release_scratch(void);
} // extern "C"

namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

std::string porfforErrorString(const char* fallback) {
    const char* err = himd_pf_last_error();
    if (err && err[0] != '\0') {
        return std::string(err);
    }
    const char* hostErr = carta_himd_host_last_error();
    if (hostErr && hostErr[0] != '\0') {
        return std::string(hostErr);
    }
    return fallback ? std::string(fallback) : std::string("Hi-MD Porffor bridge error");
}

std::string copyPorfforOutputString() {
    const auto* ptr = himd_pf_output_ptr();
    const auto len = himd_pf_output_len();
    if (!ptr || len == 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(ptr), len);
}

std::mutex& emitMutex() {
    static std::mutex m;
    return m;
}

#if defined(_WIN32)
HANDLE gSessionOutPipe = INVALID_HANDLE_VALUE;

bool writeAllWinPipe(HANDLE hPipe, const char* data, DWORD total) {
    DWORD writtenTotal = 0;
    while (writtenTotal < total) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            return false;
        }
        DWORD chunk = 0;
        const BOOL ok = WriteFile(hPipe, data + writtenTotal, total - writtenTotal, &chunk, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) {
                CloseHandle(ov.hEvent);
                return false;
            }
            if (WaitForSingleObject(ov.hEvent, 10000) != WAIT_OBJECT_0 ||
                !GetOverlappedResult(hPipe, &ov, &chunk, FALSE)) {
                CancelIo(hPipe);
                CloseHandle(ov.hEvent);
                return false;
            }
        }
        CloseHandle(ov.hEvent);
        if (chunk == 0) {
            return false;
        }
        writtenTotal += chunk;
    }
    return true;
}
#else
int gSessionOutSocketFd = -1;
bool gHasSessionClientUid = false;
uid_t gSessionClientUid = 0;
bool gHasSessionClientGid = false;
gid_t gSessionClientGid = 0;

bool writeAllPosix(int fd, const char* data, std::size_t len) {
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
#endif

void emitJsonLine(const std::string& json) {
    const std::string line = json + "\n";
    const std::lock_guard<std::mutex> lk(emitMutex());
#if defined(_WIN32)
    if (gSessionOutPipe != INVALID_HANDLE_VALUE) {
        (void)writeAllWinPipe(gSessionOutPipe, line.data(), static_cast<DWORD>(line.size()));
        return;
    }
#else
    if (gSessionOutSocketFd >= 0) {
        (void)writeAllPosix(gSessionOutSocketFd, line.data(), line.size());
        return;
    }
#endif
    std::cout << line << std::flush;
}

void emitErrorResult(const std::string& msg) {
    emitJsonLine("{\"event\":\"result\",\"ok\":false,\"error\":\"" + jsonEscape(msg) + "\"}");
}

void installStdoutProgressEmitter() {
    carta::setHiMdProgressCallback([](const carta::HiMdProgressSnapshot& snap) {
        std::ostringstream oss;
        oss << "{\"event\":\"progress\",\"operation\":\"" << jsonEscape(snap.operation)
            << "\",\"phase\":\"" << jsonEscape(snap.phase) << "\",\"progress\":" << std::fixed
            << std::setprecision(4) << snap.progress << ",\"bytes_done\":" << snap.bytesDone
            << ",\"bytes_total\":" << snap.bytesTotal << "}";
        emitJsonLine(oss.str());
    });
}

int hexVal(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

std::string percentDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hexVal(s[i + 1]);
            const int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

bool needsHardwareConfirm(const std::string& deviceId) {
    return !carta::isHiMdMockDeviceId(deviceId) || carta::isHiMdSimulateRealHardwareGuardOnMock();
}

std::string normalizeIndicesJson(const std::string& csvOrJson) {
    std::string trimmed = csvOrJson;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }
    if (!trimmed.empty() && trimmed.front() == '[') {
        return trimmed;
    }
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    std::size_t pos = 0;
    while (pos < trimmed.size()) {
        const std::size_t comma = trimmed.find(',', pos);
        const std::string item =
            comma == std::string::npos ? trimmed.substr(pos) : trimmed.substr(pos, comma - pos);
        if (!item.empty()) {
            char* endp = nullptr;
            const long v = std::strtol(item.c_str(), &endp, 10);
            if (endp == item.c_str() || *endp != '\0' || v < 0) {
                return {};
            }
            if (!first) {
                oss << ",";
            }
            first = false;
            oss << v;
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    oss << "]";
    return first ? std::string() : oss.str();
}

// ============================================================================
// Core Operations
// ============================================================================

int cmdList() {
    if (himd_pf_list_devices() < 0) {
        emitErrorResult(porfforErrorString("Failed to enumerate Hi-MD devices"));
        return 1;
    }
    std::string raw = copyPorfforOutputString();
    if (raw.empty()) {
        raw = "[]";
    }
    emitJsonLine("{\"event\":\"result\",\"ok\":true,\"devices\":" + raw + "}");
    return 0;
}

int cmdProbe(const std::string& deviceId) {
    if (deviceId.empty()) {
        emitErrorResult("Hi-MD device ID is empty");
        return 1;
    }
    if (himd_pf_probe_device(deviceId.c_str(), static_cast<int>(deviceId.size())) < 0) {
        emitErrorResult(porfforErrorString("Failed to probe Hi-MD device"));
        return 1;
    }
    const std::string raw = copyPorfforOutputString();
    if (raw.empty()) {
        emitErrorResult("Empty probe JSON returned by Hi-MD bridge");
        return 1;
    }
    emitJsonLine("{\"event\":\"result\",\"ok\":true,\"status\":" + raw + "}");
    return 0;
}

int cmdUpload(const std::string& deviceId, const std::string& codecSpec, const std::string& inPath,
              const std::string& title, const std::string& artist, const std::string& album,
              bool confirmHardware) {
    if (deviceId.empty() || codecSpec.empty() || inPath.empty()) {
        emitErrorResult("upload requires <deviceId>, --codec <spec>, and --in <path>");
        return 1;
    }
    if (needsHardwareConfirm(deviceId) && !confirmHardware) {
        emitErrorResult("Refusing to upload to Hi-MD hardware without explicit confirmation "
                        "(--confirm-hardware).");
        return 1;
    }
    carta::ScopedHiMdHardwareWriteConfirmation guard(confirmHardware);
    carta::setHiMdProgress("upload", "Uploading track to Hi-MD disc...", 0.0, 0, 1);
    const int rc = himd_pf_upload_track(
        deviceId.c_str(), static_cast<int>(deviceId.size()), codecSpec.c_str(),
        static_cast<int>(codecSpec.size()), inPath.c_str(), static_cast<int>(inPath.size()),
        title.c_str(), static_cast<int>(title.size()), artist.c_str(),
        static_cast<int>(artist.size()), album.c_str(), static_cast<int>(album.size()));
    carta::clearHiMdProgress();
    if (rc < 0) {
        emitErrorResult(porfforErrorString("Failed uploading track to Hi-MD device"));
        return 1;
    }
    const std::string raw = copyPorfforOutputString();
    emitJsonLine("{\"event\":\"result\",\"ok\":true,\"upload\":" +
                 (raw.empty() ? std::string("{}") : raw) + "}");
    return 0;
}

int cmdDump(const std::string& deviceId, int trackIndex, const std::string& outPath, bool rawAudio) {
    if (deviceId.empty() || trackIndex < 0 || outPath.empty()) {
        emitErrorResult("dump requires <deviceId>, --track <idx>, and --out <path>");
        return 1;
    }
    carta::setHiMdProgress("dump", "Reading track from Hi-MD disc...", 0.0, 0, 1);
    const int rc = himd_pf_dump_track(deviceId.c_str(), static_cast<int>(deviceId.size()),
                                      trackIndex, outPath.c_str(), static_cast<int>(outPath.size()),
                                      rawAudio ? 1 : 0);
    carta::clearHiMdProgress();
    if (rc < 0) {
        emitErrorResult(porfforErrorString("Failed dumping track from Hi-MD device"));
        return 1;
    }
#if !defined(_WIN32)
    if (gHasSessionClientUid) {
        const gid_t targetGid =
            gHasSessionClientGid ? gSessionClientGid : static_cast<gid_t>(-1);
        if (::chown(outPath.c_str(), gSessionClientUid, targetGid) != 0) {
            emitErrorResult("Failed transferring ownership of Hi-MD dump output: " +
                            std::string(std::strerror(errno)));
            return 1;
        }
        (void)::chmod(outPath.c_str(), 0600);
    }
#endif
    const std::string raw = copyPorfforOutputString();
    emitJsonLine("{\"event\":\"result\",\"ok\":true,\"dump\":" +
                 (raw.empty() ? std::string("{}") : raw) + "}");
    return 0;
}

int cmdRenameDisc(const std::string& deviceId, const std::string& title, bool confirmHardware) {
    if (deviceId.empty()) {
        emitErrorResult("rename-disc requires <deviceId>");
        return 1;
    }
    if (needsHardwareConfirm(deviceId) && !confirmHardware) {
        emitErrorResult("Refusing to rename Hi-MD disc without explicit confirmation "
                        "(--confirm-hardware).");
        return 1;
    }
    carta::ScopedHiMdHardwareWriteConfirmation guard(confirmHardware);
    if (himd_pf_rename_disc(deviceId.c_str(), static_cast<int>(deviceId.size()), title.c_str(),
                            static_cast<int>(title.size())) < 0) {
        emitErrorResult(porfforErrorString("Failed renaming Hi-MD disc"));
        return 1;
    }
    emitJsonLine("{\"event\":\"result\",\"ok\":true}");
    return 0;
}

int cmdRenameTrack(const std::string& deviceId, int trackIndex, bool hasTitle,
                   const std::string& title, bool hasArtist, const std::string& artist,
                   bool hasAlbum, const std::string& album, bool confirmHardware) {
    if (deviceId.empty() || trackIndex < 0) {
        emitErrorResult("rename-track requires <deviceId> and --track <idx>");
        return 1;
    }
    if (needsHardwareConfirm(deviceId) && !confirmHardware) {
        emitErrorResult("Refusing to rename Hi-MD track without explicit confirmation "
                        "(--confirm-hardware).");
        return 1;
    }
    carta::ScopedHiMdHardwareWriteConfirmation guard(confirmHardware);
    if (himd_pf_rename_track(deviceId.c_str(), static_cast<int>(deviceId.size()), trackIndex,
                             hasTitle ? 1 : 0, title.c_str(), static_cast<int>(title.size()),
                             hasArtist ? 1 : 0, artist.c_str(), static_cast<int>(artist.size()),
                             hasAlbum ? 1 : 0, album.c_str(), static_cast<int>(album.size())) < 0) {
        emitErrorResult(porfforErrorString("Failed renaming Hi-MD track"));
        return 1;
    }
    emitJsonLine("{\"event\":\"result\",\"ok\":true}");
    return 0;
}

int cmdMoveTrack(const std::string& deviceId, int fromIndex, int toIndex, bool confirmHardware) {
    if (deviceId.empty() || fromIndex < 0 || toIndex < 0) {
        emitErrorResult("move-track requires <deviceId>, --from <idx>, and --to <idx>");
        return 1;
    }
    if (needsHardwareConfirm(deviceId) && !confirmHardware) {
        emitErrorResult("Refusing to move Hi-MD track without explicit confirmation "
                        "(--confirm-hardware).");
        return 1;
    }
    carta::ScopedHiMdHardwareWriteConfirmation guard(confirmHardware);
    if (himd_pf_move_track(deviceId.c_str(), static_cast<int>(deviceId.size()), fromIndex,
                           toIndex) < 0) {
        emitErrorResult(porfforErrorString("Failed moving Hi-MD track"));
        return 1;
    }
    emitJsonLine("{\"event\":\"result\",\"ok\":true}");
    return 0;
}

int cmdSetGroups(const std::string& deviceId, const std::string& groupsJson, bool confirmHardware) {
    if (deviceId.empty() || groupsJson.empty()) {
        emitErrorResult("set-groups requires <deviceId> and --groups-json <json>");
        return 1;
    }
    if (needsHardwareConfirm(deviceId) && !confirmHardware) {
        emitErrorResult("Refusing to set Hi-MD groups without explicit confirmation "
                        "(--confirm-hardware).");
        return 1;
    }
    carta::ScopedHiMdHardwareWriteConfirmation guard(confirmHardware);
    if (himd_pf_set_groups(deviceId.c_str(), static_cast<int>(deviceId.size()), groupsJson.c_str(),
                           static_cast<int>(groupsJson.size())) < 0) {
        emitErrorResult(porfforErrorString("Failed writing Hi-MD groups"));
        return 1;
    }
    emitJsonLine("{\"event\":\"result\",\"ok\":true}");
    return 0;
}

int cmdDeleteTracks(const std::string& deviceId, const std::string& tracksSpec,
                    bool confirmHardware) {
    const std::string indicesJson = normalizeIndicesJson(tracksSpec);
    if (deviceId.empty() || indicesJson.empty()) {
        emitErrorResult("delete-tracks requires <deviceId> and --tracks <csv>");
        return 1;
    }
    if (needsHardwareConfirm(deviceId) && !confirmHardware) {
        emitErrorResult("Refusing to delete Hi-MD tracks without explicit confirmation "
                        "(--confirm-hardware).");
        return 1;
    }
    carta::ScopedHiMdHardwareWriteConfirmation guard(confirmHardware);
    if (himd_pf_delete_tracks(deviceId.c_str(), static_cast<int>(deviceId.size()),
                              indicesJson.c_str(), static_cast<int>(indicesJson.size())) < 0) {
        emitErrorResult(porfforErrorString("Failed deleting Hi-MD tracks"));
        return 1;
    }
    emitJsonLine("{\"event\":\"result\",\"ok\":true}");
    return 0;
}

int cmdWipe(const std::string& deviceId, bool reformat, bool confirmHardware) {
    if (deviceId.empty()) {
        emitErrorResult("wipe requires <deviceId>");
        return 1;
    }
    if (needsHardwareConfirm(deviceId) && !confirmHardware) {
        emitErrorResult("Refusing to wipe Hi-MD disc without explicit confirmation "
                        "(--confirm-hardware).");
        return 1;
    }
    carta::ScopedHiMdHardwareWriteConfirmation guard(confirmHardware);
    if (himd_pf_wipe_disc(deviceId.c_str(), static_cast<int>(deviceId.size()), reformat ? 1 : 0) <
        0) {
        emitErrorResult(porfforErrorString("Failed wiping Hi-MD disc"));
        return 1;
    }
    emitJsonLine("{\"event\":\"result\",\"ok\":true}");
    return 0;
}

int cmdCommitBatch(const std::string& deviceId, const std::string& manifestPath,
                   bool confirmHardware) {
    if (deviceId.empty() || manifestPath.empty()) {
        emitErrorResult("commit-batch requires <deviceId> and --manifest <path>");
        return 1;
    }
    if (needsHardwareConfirm(deviceId) && !confirmHardware) {
        emitErrorResult("Refusing to modify Hi-MD hardware without explicit confirmation "
                        "(--confirm-hardware).");
        return 1;
    }
    carta::ScopedHiMdHardwareWriteConfirmation guard(confirmHardware);
    carta::setHiMdProgress("commit-batch", "Committing Hi-MD batch session...", 0.0, 0, 1);
    const int rc = himd_pf_commit_batch(deviceId.c_str(), static_cast<int>(deviceId.size()),
                                        manifestPath.c_str(),
                                        static_cast<int>(manifestPath.size()));
    carta::clearHiMdProgress();
    if (rc < 0) {
        emitErrorResult(porfforErrorString("Failed committing Hi-MD batch session"));
        return 1;
    }
    const std::string raw = copyPorfforOutputString();
    emitJsonLine("{\"event\":\"result\",\"ok\":true,\"batch\":" +
                 (raw.empty() ? std::string("{}") : raw) + "}");
    return 0;
}

int cmdSelftestStack() {
    carta::setMockHiMdEnabled(true);
    carta::resetMockHiMdState();

    namespace fs = std::filesystem;
    const auto ticks =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path inPath =
        fs::temp_directory_path() / ("carta_himd_stack_in_" + std::to_string(ticks) + ".pcm");
    const fs::path outPath =
        fs::temp_directory_path() / ("carta_himd_stack_out_" + std::to_string(ticks) + ".pcm");

    struct FileCleanup {
        fs::path a;
        fs::path b;
        ~FileCleanup() {
            std::error_code ec;
            fs::remove(a, ec);
            fs::remove(b, ec);
        }
    } cleanup{inPath, outPath};

    // 6 MiB = 6,291,456 bytes (98,304 virtual 64-byte LPCM frames -> 386 Hi-MD 16 KiB blocks).
    constexpr std::size_t kPayloadBytes = 6u * 1024u * 1024u;
    std::vector<std::uint8_t> pcm(kPayloadBytes);
    for (std::size_t i = 0; i < kPayloadBytes; ++i) {
        pcm[i] = static_cast<std::uint8_t>((i * 131u + (i >> 9) * 17u) & 0xFFu);
    }
    {
        std::ofstream f(inPath, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(reinterpret_cast<const char*>(pcm.data()),
                           static_cast<std::streamsize>(pcm.size()))) {
            emitErrorResult("Failed writing temporary selftest input file");
            return 1;
        }
    }

    struct StackCtx {
        std::string inPath;
        std::string outPath;
        int rcUpload = -1;
        int rcDump = -1;
        std::string error;
    } ctx{inPath.string(), outPath.string(), -1, -1, {}};

    auto runWorker = [](StackCtx* c) {
        const char dev[] = "mock:mz-rh1";
        const char codec[] = "pcm";
        const char title[] = "STACK TEST";
        c->rcUpload = himd_pf_upload_track(
            dev, static_cast<int>(sizeof(dev) - 1), codec, static_cast<int>(sizeof(codec) - 1),
            c->inPath.c_str(), static_cast<int>(c->inPath.size()), title,
            static_cast<int>(sizeof(title) - 1), "", 0, "", 0);
        if (c->rcUpload < 0) {
            c->error = porfforErrorString("himd_pf_upload_track failed");
            return;
        }
        c->rcDump = himd_pf_dump_track(dev, static_cast<int>(sizeof(dev) - 1), 0,
                                       c->outPath.c_str(), static_cast<int>(c->outPath.size()), 1);
        if (c->rcDump < 0) {
            c->error = porfforErrorString("himd_pf_dump_track failed");
        }
    };

#if defined(_WIN32)
    struct WinRunner {
        StackCtx* c;
        void (*fn)(StackCtx*);
        static DWORD WINAPI entry(LPVOID raw) {
            auto* r = static_cast<WinRunner*>(raw);
            r->fn(r->c);
            return 0;
        }
    } wr{&ctx, runWorker};
    HANDLE hThread = CreateThread(nullptr, 544u * 1024u, &WinRunner::entry, &wr,
                                  STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!hThread) {
        emitErrorResult("CreateThread failed");
        return 1;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
#else
    struct PosixRunner {
        StackCtx* c;
        decltype(runWorker) fn;
    } pr{&ctx, runWorker};
    auto threadFn = [](void* raw) -> void* {
        auto* r = static_cast<PosixRunner*>(raw);
        r->fn(r->c);
        return nullptr;
    };
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0 || pthread_attr_setstacksize(&attr, 544u * 1024u) != 0) {
        emitErrorResult("pthread_attr setup failed");
        return 1;
    }
    pthread_t tid{};
    if (pthread_create(&tid, &attr, threadFn, &pr) != 0) {
        pthread_attr_destroy(&attr);
        emitErrorResult("pthread_create failed");
        return 1;
    }
    pthread_attr_destroy(&attr);
    pthread_join(tid, nullptr);
#endif

    if (ctx.rcUpload < 0 || ctx.rcDump < 0) {
        emitErrorResult("6 MiB PCM Hi-MD upload/dump on 544 KB stack failed: " + ctx.error);
        return 1;
    }

    std::ifstream outF(outPath, std::ios::binary | std::ios::ate);
    if (!outF || static_cast<std::size_t>(outF.tellg()) != pcm.size()) {
        emitErrorResult("6 MiB PCM selftest output size mismatch");
        return 1;
    }
    outF.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> readBack(pcm.size());
    if (!outF.read(reinterpret_cast<char*>(readBack.data()),
                   static_cast<std::streamsize>(readBack.size())) ||
        readBack != pcm) {
        emitErrorResult("6 MiB PCM selftest byte-for-byte comparison failed");
        return 1;
    }

    emitJsonLine("{\"event\":\"result\",\"ok\":true,\"bytes\":" + std::to_string(pcm.size()) + "}");
    return 0;
}

int cmdMockSnapshot() {
    const auto snap = carta::getMockHiMdSnapshot();
    std::ostringstream oss;
    oss << "{\"event\":\"result\",\"ok\":true"
        << ",\"enabled\":" << (snap.enabled ? "true" : "false")
        << ",\"open\":" << (snap.open ? "true" : "false")
        << ",\"in_session\":" << (snap.inSession ? "true" : "false")
        << ",\"datanum\":" << snap.datanum
        << ",\"current_generation\":" << snap.currentGeneration
        << ",\"auth_started_count\":" << snap.authStartedCount
        << ",\"auth_completed_count\":" << snap.authCompletedCount
        << ",\"icv_read_count\":" << snap.icvReadCount
        << ",\"icv_write_count\":" << snap.icvWriteCount
        << ",\"sectors_read\":" << snap.sectorsRead
        << ",\"sectors_written\":" << snap.sectorsWritten
        << ",\"wipe_count\":" << snap.wipeCount << ",\"slept_ms\":" << snap.sleptMs
        << ",\"disc_id\":\"" << jsonEscape(snap.discIdHex) << "\""
        << ",\"last_icv\":\"" << jsonEscape(snap.lastIcvHex) << "\""
        << ",\"head_key\":\"" << jsonEscape(snap.headKeyHex) << "\""
        << ",\"body_key\":\"" << jsonEscape(snap.bodyKeyHex) << "\""
        << "}";
    emitJsonLine(oss.str());
    return 0;
}

std::vector<std::string> splitTabTokens(const std::string& line) {
    std::vector<std::string> tok;
    std::size_t pos = 0;
    while (true) {
        const std::size_t tab = line.find('\t', pos);
        if (tab == std::string::npos) {
            tok.push_back(line.substr(pos));
            break;
        }
        tok.push_back(line.substr(pos, tab - pos));
        pos = tab + 1;
    }
    return tok;
}

bool constantTimeEquals(const std::string& a, const std::string& b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool readAndUnlinkTokenFile(const std::string& tokenPath, std::string& outToken,
                            std::string& error) {
    outToken.clear();
    std::string content;
    {
        std::ifstream f(tokenPath, std::ios::binary);
        if (!f) {
            std::error_code ec;
            std::filesystem::remove(tokenPath, ec);
            error = "Failed opening Hi-MD session token file: " + tokenPath;
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        content = ss.str();
    }
    std::error_code rmEc;
    std::filesystem::remove(tokenPath, rmEc);

    while (!content.empty() &&
           (content.back() == '\n' || content.back() == '\r' || content.back() == ' ' ||
            content.back() == '\t')) {
        content.pop_back();
    }
    while (!content.empty() && (content.front() == ' ' || content.front() == '\t')) {
        content.erase(content.begin());
    }
    if (content.empty()) {
        error = "Hi-MD session token file was empty.";
        return false;
    }
    outToken = std::move(content);
    return true;
}

bool dispatchSessionLine(const std::string& rawLine, bool enableMock) {
    std::string line = rawLine;
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line.empty()) {
        return true;
    }
    const auto tok = splitTabTokens(line);
    if (tok.empty() || tok[0].empty()) {
        return true;
    }
    const auto& op = tok[0];
    if (op == "quit" || op == "exit") {
        emitJsonLine("{\"event\":\"result\",\"ok\":true}");
        return false;
    }
    if (op == "list") {
        cmdList();
    } else if (op == "probe" && tok.size() >= 2) {
        cmdProbe(percentDecode(tok[1]));
    } else if (op == "upload" && tok.size() >= 8) {
        cmdUpload(percentDecode(tok[1]), percentDecode(tok[2]), percentDecode(tok[3]),
                  percentDecode(tok[4]), percentDecode(tok[5]), percentDecode(tok[6]),
                  tok[7] == "1");
    } else if (op == "dump" && tok.size() >= 5) {
        cmdDump(percentDecode(tok[1]), std::atoi(tok[2].c_str()), percentDecode(tok[3]),
                tok[4] == "1");
    } else if (op == "rename-disc" && tok.size() >= 4) {
        cmdRenameDisc(percentDecode(tok[1]), percentDecode(tok[2]), tok[3] == "1");
    } else if (op == "rename-track" && tok.size() >= 10) {
        cmdRenameTrack(percentDecode(tok[1]), std::atoi(tok[2].c_str()), tok[3] == "1",
                       percentDecode(tok[4]), tok[5] == "1", percentDecode(tok[6]),
                       tok[7] == "1", percentDecode(tok[8]), tok[9] == "1");
    } else if (op == "move-track" && tok.size() >= 5) {
        cmdMoveTrack(percentDecode(tok[1]), std::atoi(tok[2].c_str()), std::atoi(tok[3].c_str()),
                     tok[4] == "1");
    } else if (op == "set-groups" && tok.size() >= 4) {
        cmdSetGroups(percentDecode(tok[1]), percentDecode(tok[2]), tok[3] == "1");
    } else if (op == "delete-tracks" && tok.size() >= 4) {
        cmdDeleteTracks(percentDecode(tok[1]), percentDecode(tok[2]), tok[3] == "1");
    } else if (op == "wipe" && tok.size() >= 4) {
        cmdWipe(percentDecode(tok[1]), tok[2] == "1", tok[3] == "1");
    } else if (op == "commit-batch" && tok.size() >= 4) {
        cmdCommitBatch(percentDecode(tok[1]), percentDecode(tok[2]), tok[3] == "1");
    } else if (op == "mock-reset" && enableMock) {
        carta::resetMockHiMdState();
        emitJsonLine("{\"event\":\"result\",\"ok\":true}");
    } else if (op == "mock-guard" && enableMock && tok.size() >= 2) {
        carta::setHiMdSimulateRealHardwareGuardOnMock(tok[1] == "1");
        emitJsonLine("{\"event\":\"result\",\"ok\":true}");
    } else if (op == "mock-snapshot" && enableMock) {
        cmdMockSnapshot();
    } else if (op == "mock-save-image" && enableMock && tok.size() >= 2) {
        std::string err;
        if (!carta::saveMockHiMdImage(percentDecode(tok[1]), err)) {
            emitErrorResult(err);
        } else {
            emitJsonLine("{\"event\":\"result\",\"ok\":true}");
        }
    } else if (op == "mock-load-image" && enableMock && tok.size() >= 2) {
        std::string err;
        if (!carta::loadMockHiMdImage(percentDecode(tok[1]), err)) {
            emitErrorResult(err);
        } else {
            emitJsonLine("{\"event\":\"result\",\"ok\":true}");
        }
    } else {
        emitErrorResult("Unknown or malformed session command: " + op);
    }
    return true;
}

int runSessionLoop(bool enableMock) {
    if (enableMock) {
        carta::setMockHiMdEnabled(true);
        carta::resetMockHiMdState();
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!dispatchSessionLine(line, enableMock)) {
            break;
        }
    }
    return 0;
}

#if !defined(_WIN32)
bool isPosixParentAlive(long long parentPid) noexcept {
    if (parentPid <= 0) {
        return true;
    }
    if (::kill(static_cast<pid_t>(parentPid), 0) == 0) {
        return true;
    }
    return errno != ESRCH;
}

bool readSocketLinePosix(int fd, std::string& carryBuf, std::string& outLine, long long parentPid,
                         int maxIdleMs, bool& outParentDied) {
    outLine.clear();
    outParentDied = false;
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        const std::size_t nl = carryBuf.find('\n');
        if (nl != std::string::npos) {
            outLine = carryBuf.substr(0, nl);
            carryBuf.erase(0, nl + 1);
            return true;
        }
        if (!isPosixParentAlive(parentPid)) {
            outParentDied = true;
            return false;
        }
        if (maxIdleMs >= 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            if (elapsed >= maxIdleMs) {
                return false;
            }
        }
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int prc = ::poll(&pfd, 1, 500);
        if (prc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (prc == 0) {
            continue;
        }
        char chunk[4096];
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            if (!carryBuf.empty()) {
                outLine.swap(carryBuf);
                return true;
            }
            return false;
        }
        carryBuf.append(chunk, static_cast<std::size_t>(n));
    }
}

int runSocketSessionLoop(bool enableMock, const std::string& socketPath, bool hasClientUid,
                         uid_t clientUid, bool hasClientGid, gid_t clientGid,
                         const std::string& tokenFile, long long parentPid) {
    std::signal(SIGPIPE, SIG_IGN);
    if (socketPath.empty()) {
        emitErrorResult("session --socket requires a non-empty path.");
        return 1;
    }

    std::string expectedToken;
    if (!tokenFile.empty()) {
        std::string tokErr;
        if (!readAndUnlinkTokenFile(tokenFile, expectedToken, tokErr)) {
            emitErrorResult(tokErr);
            return 1;
        }
    }

    sockaddr_un addr{};
    if (socketPath.size() >= sizeof(addr.sun_path)) {
        emitErrorResult("Hi-MD socket path exceeds Unix sockaddr_un limit.");
        return 1;
    }

    if (enableMock) {
        carta::setMockHiMdEnabled(true);
        carta::resetMockHiMdState();
    }

    (void)::unlink(socketPath.c_str());
    const int srvFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srvFd < 0) {
        emitErrorResult("socket() failed for Hi-MD session: " + std::string(std::strerror(errno)));
        return 1;
    }

    struct ServerCleanup {
        int srvFd = -1;
        int clientFd = -1;
        std::string socketPath;
        ~ServerCleanup() {
            gSessionOutSocketFd = -1;
            gHasSessionClientUid = false;
            gHasSessionClientGid = false;
            if (clientFd >= 0) {
                ::close(clientFd);
            }
            if (srvFd >= 0) {
                ::close(srvFd);
            }
            if (!socketPath.empty()) {
                ::unlink(socketPath.c_str());
            }
        }
    } cleanup{srvFd, -1, socketPath};

    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1u);

    const mode_t prevMask = ::umask(0177);
    const int bindRc =
        ::bind(srvFd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    const int bindErr = errno;
    ::umask(prevMask);
    if (bindRc != 0) {
        emitErrorResult("bind() failed on Hi-MD socket '" + socketPath +
                        "': " + std::string(std::strerror(bindErr)));
        return 1;
    }

    if (hasClientUid || hasClientGid) {
        if (::chown(socketPath.c_str(), hasClientUid ? clientUid : static_cast<uid_t>(-1),
                    hasClientGid ? clientGid : static_cast<gid_t>(-1)) != 0) {
            emitErrorResult("chown() failed on Hi-MD socket: " + std::string(std::strerror(errno)));
            return 1;
        }
    }
    (void)::chmod(socketPath.c_str(), 0600);

    if (::listen(srvFd, 2) != 0) {
        emitErrorResult("listen() failed on Hi-MD socket: " + std::string(std::strerror(errno)));
        return 1;
    }

    gHasSessionClientUid = hasClientUid;
    gSessionClientUid = clientUid;
    gHasSessionClientGid = hasClientGid;
    gSessionClientGid = clientGid;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    int authFailures = 0;
    std::string carryBuf;

    while (cleanup.clientFd < 0) {
        if (!isPosixParentAlive(parentPid)) {
            return 0;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            emitErrorResult("Timed out waiting for client connection on Hi-MD socket.");
            return 1;
        }
        struct pollfd pfd{};
        pfd.fd = srvFd;
        pfd.events = POLLIN;
        const int prc = ::poll(&pfd, 1, 500);
        if (prc < 0) {
            if (errno == EINTR) {
                continue;
            }
            emitErrorResult("poll() failed accepting Hi-MD socket client.");
            return 1;
        }
        if (prc == 0) {
            continue;
        }

        const int cfd = ::accept(srvFd, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            emitErrorResult("accept() failed on Hi-MD socket.");
            return 1;
        }

        bool peerAllowed = true;
        if (hasClientUid) {
#if defined(__APPLE__)
            uid_t peerUid = 0;
            gid_t peerGid = 0;
            if (::getpeereid(cfd, &peerUid, &peerGid) != 0 ||
                (peerUid != clientUid && peerUid != 0)) {
                peerAllowed = false;
            }
#elif defined(__linux__)
            struct ucred cred{};
            socklen_t credLen = sizeof(cred);
            if (::getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &credLen) != 0 ||
                (cred.uid != clientUid && cred.uid != 0)) {
                peerAllowed = false;
            }
#endif
        }
        if (!peerAllowed) {
            const char msg[] =
                "{\"event\":\"result\",\"ok\":false,\"error\":\"Unauthorized peer UID.\"}\n";
            (void)writeAllPosix(cfd, msg, sizeof(msg) - 1u);
            ::close(cfd);
            if (++authFailures >= 5) {
                return 1;
            }
            continue;
        }

        carryBuf.clear();
        if (!expectedToken.empty()) {
            std::string authLine;
            bool parentDied = false;
            if (!readSocketLinePosix(cfd, carryBuf, authLine, parentPid, 5000, parentDied)) {
                ::close(cfd);
                if (parentDied) {
                    return 0;
                }
                if (++authFailures >= 5) {
                    return 1;
                }
                continue;
            }
            if (!authLine.empty() && authLine.back() == '\r') {
                authLine.pop_back();
            }
            const auto authTok = splitTabTokens(authLine);
            if (authTok.size() != 2 || authTok[0] != "auth" ||
                !constantTimeEquals(percentDecode(authTok[1]), expectedToken)) {
                const char msg[] =
                    "{\"event\":\"result\",\"ok\":false,\"error\":\"Unauthorized session token.\"}\n";
                (void)writeAllPosix(cfd, msg, sizeof(msg) - 1u);
                ::close(cfd);
                if (++authFailures >= 5) {
                    return 1;
                }
                continue;
            }
            gSessionOutSocketFd = cfd;
            emitJsonLine("{\"event\":\"result\",\"ok\":true}");
        }

        cleanup.clientFd = cfd;
        gSessionOutSocketFd = cfd;
    }

    std::string rawLine;
    bool parentDied = false;
    while (readSocketLinePosix(cleanup.clientFd, carryBuf, rawLine, parentPid, -1, parentDied)) {
        if (!dispatchSessionLine(rawLine, enableMock)) {
            break;
        }
    }
    return 0;
}
#else
bool isWinParentAlive(HANDLE hParent) noexcept {
    if (!hParent) {
        return true;
    }
    return WaitForSingleObject(hParent, 0) == WAIT_TIMEOUT;
}

bool readPipeLineWin(HANDLE hPipe, std::string& carryBuf, std::string& outLine, HANDLE hParent,
                     int maxIdleMs, bool& outParentDied) {
    outLine.clear();
    outParentDied = false;
    const auto start = std::chrono::steady_clock::now();
    while (true) {
        const std::size_t nl = carryBuf.find('\n');
        if (nl != std::string::npos) {
            outLine = carryBuf.substr(0, nl);
            carryBuf.erase(0, nl + 1);
            return true;
        }
        if (!isWinParentAlive(hParent)) {
            outParentDied = true;
            return false;
        }
        if (maxIdleMs >= 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            if (elapsed >= maxIdleMs) {
                return false;
            }
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &avail, nullptr)) {
            if (!carryBuf.empty()) {
                outLine.swap(carryBuf);
                return true;
            }
            return false;
        }
        if (avail == 0) {
            Sleep(20);
            continue;
        }
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            return false;
        }
        char chunk[4096];
        DWORD readLen = 0;
        const BOOL ok = ReadFile(hPipe, chunk, sizeof(chunk), &readLen, &ov);
        if (!ok) {
            const DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                while (true) {
                    const DWORD wr = WaitForSingleObject(ov.hEvent, 250);
                    if (wr == WAIT_OBJECT_0) {
                        if (!GetOverlappedResult(hPipe, &ov, &readLen, FALSE)) {
                            CloseHandle(ov.hEvent);
                            return false;
                        }
                        break;
                    }
                    if (!isWinParentAlive(hParent)) {
                        CancelIo(hPipe);
                        CloseHandle(ov.hEvent);
                        outParentDied = true;
                        return false;
                    }
                }
            } else {
                CloseHandle(ov.hEvent);
                if (!carryBuf.empty()) {
                    outLine.swap(carryBuf);
                    return true;
                }
                return false;
            }
        }
        CloseHandle(ov.hEvent);
        if (readLen == 0) {
            if (!carryBuf.empty()) {
                outLine.swap(carryBuf);
                return true;
            }
            return false;
        }
        carryBuf.append(chunk, static_cast<std::size_t>(readLen));
    }
}

int runPipeSessionLoop(bool enableMock, const std::string& pipeName, const std::string& tokenFile,
                       long long parentPid) {
    if (pipeName.empty()) {
        emitErrorResult("session --pipe requires a non-empty pipe name.");
        return 1;
    }

    std::string expectedToken;
    if (!tokenFile.empty()) {
        std::string tokErr;
        if (!readAndUnlinkTokenFile(tokenFile, expectedToken, tokErr)) {
            emitErrorResult(tokErr);
            return 1;
        }
    }

    if (enableMock) {
        carta::setMockHiMdEnabled(true);
        carta::resetMockHiMdState();
    }

    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(), -1, nullptr, 0);
    if (wideLen <= 0) {
        emitErrorResult("Invalid UTF-8 Hi-MD pipe name.");
        return 1;
    }
    std::vector<wchar_t> widePipe(static_cast<std::size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, pipeName.c_str(), -1, widePipe.data(), wideLen);

    PSECURITY_DESCRIPTOR pSd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    SECURITY_ATTRIBUTES* pSa = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GRGW;;;SY)(A;;GRGW;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &pSd, nullptr)) {
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = pSd;
        sa.bInheritHandle = FALSE;
        pSa = &sa;
    }

    HANDLE hPipe = CreateNamedPipeW(
        widePipe.data(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, pSa);
    if (pSd) {
        LocalFree(pSd);
    }
    if (hPipe == INVALID_HANDLE_VALUE) {
        emitErrorResult("CreateNamedPipeW failed for Hi-MD session (error " +
                        std::to_string(GetLastError()) + ").");
        return 1;
    }

    HANDLE hParent =
        parentPid > 0 ? OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parentPid)) : nullptr;
    struct PipeCleanup {
        HANDLE hPipe = INVALID_HANDLE_VALUE;
        HANDLE hParent = nullptr;
        ~PipeCleanup() {
            gSessionOutPipe = INVALID_HANDLE_VALUE;
            if (hPipe != INVALID_HANDLE_VALUE) {
                FlushFileBuffers(hPipe);
                DisconnectNamedPipe(hPipe);
                CloseHandle(hPipe);
            }
            if (hParent) {
                CloseHandle(hParent);
            }
        }
    } cleanup{hPipe, hParent};

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    bool authenticated = false;
    int authFailures = 0;
    std::string carryBuf;

    while (!authenticated) {
        if (!isWinParentAlive(hParent)) {
            return 0;
        }
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            return 1;
        }
        BOOL connected = ConnectNamedPipe(hPipe, &ov);
        if (!connected) {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            } else if (err == ERROR_IO_PENDING) {
                while (true) {
                    const DWORD wr = WaitForSingleObject(ov.hEvent, 250);
                    if (wr == WAIT_OBJECT_0) {
                        DWORD dummy = 0;
                        connected = GetOverlappedResult(hPipe, &ov, &dummy, FALSE);
                        break;
                    }
                    if (!isWinParentAlive(hParent) ||
                        std::chrono::steady_clock::now() >= deadline) {
                        CancelIo(hPipe);
                        CloseHandle(ov.hEvent);
                        return 0;
                    }
                }
            }
        }
        CloseHandle(ov.hEvent);
        if (!connected) {
            return 1;
        }

        carryBuf.clear();
        if (!expectedToken.empty()) {
            std::string authLine;
            bool parentDied = false;
            if (!readPipeLineWin(hPipe, carryBuf, authLine, hParent, 5000, parentDied)) {
                DisconnectNamedPipe(hPipe);
                if (parentDied) {
                    return 0;
                }
                if (++authFailures >= 5) {
                    return 1;
                }
                continue;
            }
            if (!authLine.empty() && authLine.back() == '\r') {
                authLine.pop_back();
            }
            const auto authTok = splitTabTokens(authLine);
            if (authTok.size() != 2 || authTok[0] != "auth" ||
                !constantTimeEquals(percentDecode(authTok[1]), expectedToken)) {
                const char msg[] =
                    "{\"event\":\"result\",\"ok\":false,\"error\":\"Unauthorized session token.\"}\n";
                (void)writeAllWinPipe(hPipe, msg, static_cast<DWORD>(sizeof(msg) - 1u));
                FlushFileBuffers(hPipe);
                DisconnectNamedPipe(hPipe);
                if (++authFailures >= 5) {
                    return 1;
                }
                continue;
            }
            gSessionOutPipe = hPipe;
            emitJsonLine("{\"event\":\"result\",\"ok\":true}");
        }
        gSessionOutPipe = hPipe;
        authenticated = true;
    }

    std::string rawLine;
    bool parentDied = false;
    while (readPipeLineWin(hPipe, carryBuf, rawLine, hParent, -1, parentDied)) {
        if (!dispatchSessionLine(rawLine, enableMock)) {
            break;
        }
    }
    return 0;
}
#endif

void printUsage() {
    std::cerr
        << "carta-himd 0.4.0 (GPL-2.0) - Standalone Hi-MD SCSI/UMSC protocol helper\n"
        << "Built with himd-js, nufatfs, node-mass-storage (GPL-2.0) and Porffor (MIT).\n"
        << "Usage:\n"
        << "  carta-himd list [--mock]\n"
        << "  carta-himd probe <device-id>\n"
        << "  carta-himd upload <device-id> --codec <spec> --in <path> [--title T] [--artist A] "
           "[--album B] [--confirm-hardware]\n"
        << "  carta-himd dump <device-id> --track <idx> --out <path> [--raw]\n"
        << "  carta-himd rename-disc <device-id> --title <T> [--confirm-hardware]\n"
        << "  carta-himd rename-track <device-id> --track <idx> [--title T] [--artist A] "
           "[--album B] [--confirm-hardware]\n"
        << "  carta-himd move-track <device-id> --from <idx> --to <idx> [--confirm-hardware]\n"
        << "  carta-himd set-groups <device-id> --groups-json <json> [--confirm-hardware]\n"
        << "  carta-himd delete-tracks <device-id> --tracks <csv> [--confirm-hardware]\n"
        << "  carta-himd wipe <device-id> [--reformat] [--confirm-hardware]\n"
        << "  carta-himd commit-batch <device-id> --manifest <path> [--confirm-hardware]\n"
        << "  carta-himd selftest-stack\n"
        << "  carta-himd session [--mock] [--socket <path> | --pipe <name>] [--client-uid <uid>] "
           "[--client-gid <gid>] [--token-file <path>] [--parent-pid <pid>]\n";
}

} // namespace

int main(int argc, char** argv) {
    installStdoutProgressEmitter();

    if (argc < 2) {
        printUsage();
        return 2;
    }

    const std::string cmd = argv[1];
    if (cmd == "--version" || cmd == "-v") {
        std::cout << "carta-himd 0.4.0 (GPL-2.0)\n";
        return 0;
    }
    if (cmd == "--help" || cmd == "-h") {
        printUsage();
        return 0;
    }
    if (cmd == "list") {
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--mock") == 0) {
                carta::setMockHiMdEnabled(true);
            }
        }
        return cmdList();
    }
    if (cmd == "probe") {
        if (argc < 3) {
            emitErrorResult("Usage: carta-himd probe <device-id>");
            return 2;
        }
        if (carta::isHiMdMockDeviceId(argv[2])) {
            carta::setMockHiMdEnabled(true);
        }
        return cmdProbe(argv[2]);
    }
    if (cmd == "upload") {
        if (argc < 3) {
            emitErrorResult("Usage: carta-himd upload <device-id> --codec <spec> --in <path>");
            return 2;
        }
        std::string codecSpec;
        std::string inPath;
        std::string title;
        std::string artist;
        std::string album;
        bool confirm = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
                codecSpec = argv[++i];
            } else if (std::strcmp(argv[i], "--in") == 0 && i + 1 < argc) {
                inPath = argv[++i];
            } else if (std::strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
                title = argv[++i];
            } else if (std::strcmp(argv[i], "--artist") == 0 && i + 1 < argc) {
                artist = argv[++i];
            } else if (std::strcmp(argv[i], "--album") == 0 && i + 1 < argc) {
                album = argv[++i];
            } else if (std::strcmp(argv[i], "--confirm-hardware") == 0) {
                confirm = true;
            }
        }
        if (carta::isHiMdMockDeviceId(argv[2])) {
            carta::setMockHiMdEnabled(true);
        }
        return cmdUpload(argv[2], codecSpec, inPath, title, artist, album, confirm);
    }
    if (cmd == "dump") {
        if (argc < 3) {
            emitErrorResult("Usage: carta-himd dump <device-id> --track <idx> --out <path>");
            return 2;
        }
        int trackIdx = -1;
        std::string outPath;
        bool raw = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--track") == 0 && i + 1 < argc) {
                trackIdx = std::atoi(argv[++i]);
            } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
                outPath = argv[++i];
            } else if (std::strcmp(argv[i], "--raw") == 0) {
                raw = true;
            }
        }
        if (carta::isHiMdMockDeviceId(argv[2])) {
            carta::setMockHiMdEnabled(true);
        }
        return cmdDump(argv[2], trackIdx, outPath, raw);
    }
    if (cmd == "rename-disc") {
        if (argc < 3) {
            emitErrorResult("Usage: carta-himd rename-disc <device-id> --title <T>");
            return 2;
        }
        std::string title;
        bool confirm = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
                title = argv[++i];
            } else if (std::strcmp(argv[i], "--confirm-hardware") == 0) {
                confirm = true;
            }
        }
        if (carta::isHiMdMockDeviceId(argv[2])) {
            carta::setMockHiMdEnabled(true);
        }
        return cmdRenameDisc(argv[2], title, confirm);
    }
    if (cmd == "rename-track") {
        if (argc < 3) {
            emitErrorResult("Usage: carta-himd rename-track <device-id> --track <idx> ...");
            return 2;
        }
        int trackIdx = -1;
        bool hasTitle = false;
        bool hasArtist = false;
        bool hasAlbum = false;
        std::string title;
        std::string artist;
        std::string album;
        bool confirm = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--track") == 0 && i + 1 < argc) {
                trackIdx = std::atoi(argv[++i]);
            } else if (std::strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
                hasTitle = true;
                title = argv[++i];
            } else if (std::strcmp(argv[i], "--artist") == 0 && i + 1 < argc) {
                hasArtist = true;
                artist = argv[++i];
            } else if (std::strcmp(argv[i], "--album") == 0 && i + 1 < argc) {
                hasAlbum = true;
                album = argv[++i];
            } else if (std::strcmp(argv[i], "--confirm-hardware") == 0) {
                confirm = true;
            }
        }
        if (carta::isHiMdMockDeviceId(argv[2])) {
            carta::setMockHiMdEnabled(true);
        }
        return cmdRenameTrack(argv[2], trackIdx, hasTitle, title, hasArtist, artist, hasAlbum, album,
                              confirm);
    }
    if (cmd == "move-track") {
        if (argc < 3) {
            emitErrorResult("Usage: carta-himd move-track <device-id> --from <idx> --to <idx>");
            return 2;
        }
        int fromIdx = -1;
        int toIdx = -1;
        bool confirm = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
                fromIdx = std::atoi(argv[++i]);
            } else if (std::strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
                toIdx = std::atoi(argv[++i]);
            } else if (std::strcmp(argv[i], "--confirm-hardware") == 0) {
                confirm = true;
            }
        }
        if (carta::isHiMdMockDeviceId(argv[2])) {
            carta::setMockHiMdEnabled(true);
        }
        return cmdMoveTrack(argv[2], fromIdx, toIdx, confirm);
    }
    if (cmd == "set-groups") {
        if (argc < 3) {
            emitErrorResult("Usage: carta-himd set-groups <device-id> --groups-json <json>");
            return 2;
        }
        std::string groupsJson;
        bool confirm = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--groups-json") == 0 && i + 1 < argc) {
                groupsJson = argv[++i];
            } else if (std::strcmp(argv[i], "--confirm-hardware") == 0) {
                confirm = true;
            }
        }
        if (carta::isHiMdMockDeviceId(argv[2])) {
            carta::setMockHiMdEnabled(true);
        }
        return cmdSetGroups(argv[2], groupsJson, confirm);
    }
    if (cmd == "delete-tracks") {
        if (argc < 3) {
            emitErrorResult("Usage: carta-himd delete-tracks <device-id> --tracks <csv>");
            return 2;
        }
        std::string tracksCsv;
        bool confirm = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--tracks") == 0 && i + 1 < argc) {
                tracksCsv = argv[++i];
            } else if (std::strcmp(argv[i], "--confirm-hardware") == 0) {
                confirm = true;
            }
        }
        if (carta::isHiMdMockDeviceId(argv[2])) {
            carta::setMockHiMdEnabled(true);
        }
        return cmdDeleteTracks(argv[2], tracksCsv, confirm);
    }
    if (cmd == "wipe") {
        if (argc < 3) {
            emitErrorResult("Usage: carta-himd wipe <device-id> [--reformat] [--confirm-hardware]");
            return 2;
        }
        bool reformat = false;
        bool confirm = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--reformat") == 0) {
                reformat = true;
            } else if (std::strcmp(argv[i], "--confirm-hardware") == 0) {
                confirm = true;
            }
        }
        if (carta::isHiMdMockDeviceId(argv[2])) {
            carta::setMockHiMdEnabled(true);
        }
        return cmdWipe(argv[2], reformat, confirm);
    }
    if (cmd == "commit-batch") {
        if (argc < 3) {
            emitErrorResult(
                "Usage: carta-himd commit-batch <device-id> --manifest <path> [--confirm-hardware]");
            return 2;
        }
        std::string manifestPath;
        bool confirm = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) {
                manifestPath = argv[++i];
            } else if (std::strcmp(argv[i], "--confirm-hardware") == 0) {
                confirm = true;
            }
        }
        if (carta::isHiMdMockDeviceId(argv[2])) {
            carta::setMockHiMdEnabled(true);
        }
        return cmdCommitBatch(argv[2], manifestPath, confirm);
    }
    if (cmd == "selftest-stack") {
        return cmdSelftestStack();
    }
    if (cmd == "session") {
        bool mock = false;
        std::string socketPath;
        std::string pipeName;
        std::string tokenFile;
        long long parentPid = 0;
#if !defined(_WIN32)
        bool hasClientUid = false;
        uid_t clientUid = 0;
        bool hasClientGid = false;
        gid_t clientGid = 0;
#endif
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--mock") == 0) {
                mock = true;
            } else if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
                socketPath = argv[++i];
            } else if (std::strcmp(argv[i], "--pipe") == 0 && i + 1 < argc) {
                pipeName = argv[++i];
            } else if (std::strcmp(argv[i], "--token-file") == 0 && i + 1 < argc) {
                tokenFile = argv[++i];
            } else if (std::strcmp(argv[i], "--parent-pid") == 0 && i + 1 < argc) {
                parentPid = std::atoll(argv[++i]);
            } else if (std::strcmp(argv[i], "--client-uid") == 0 && i + 1 < argc) {
#if !defined(_WIN32)
                clientUid = static_cast<uid_t>(std::strtoul(argv[++i], nullptr, 10));
                hasClientUid = true;
#else
                ++i;
#endif
            } else if (std::strcmp(argv[i], "--client-gid") == 0 && i + 1 < argc) {
#if !defined(_WIN32)
                clientGid = static_cast<gid_t>(std::strtoul(argv[++i], nullptr, 10));
                hasClientGid = true;
#else
                ++i;
#endif
            }
        }
#if defined(_WIN32)
        if (!pipeName.empty()) {
            return runPipeSessionLoop(mock, pipeName, tokenFile, parentPid);
        }
#else
        if (!socketPath.empty()) {
            return runSocketSessionLoop(mock, socketPath, hasClientUid, clientUid, hasClientGid,
                                        clientGid, tokenFile, parentPid);
        }
#endif
        return runSessionLoop(mock);
    }

    emitErrorResult("Unknown subcommand: " + cmd);
    return 2;
}
