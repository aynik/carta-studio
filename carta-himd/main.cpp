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
#else
#include <pthread.h>
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

void emitJsonLine(const std::string& json) {
    std::cout << json << "\n" << std::flush;
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
        decltype(runWorker) fn;
    } wr{&ctx, runWorker};
    auto threadFn = [](LPVOID raw) -> DWORD WINAPI {
        auto* r = static_cast<WinRunner*>(raw);
        r->fn(r->c);
        return 0;
    };
    HANDLE hThread = CreateThread(nullptr, 544u * 1024u, threadFn, &wr,
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

int runSessionLoop(bool enableMock) {
    if (enableMock) {
        carta::setMockHiMdEnabled(true);
        carta::resetMockHiMdState();
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
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
        if (tok.empty() || tok[0].empty()) {
            continue;
        }
        const auto& op = tok[0];
        if (op == "quit" || op == "exit") {
            emitJsonLine("{\"event\":\"result\",\"ok\":true}");
            break;
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
    }
    return 0;
}

void printUsage() {
    std::cerr
        << "carta-himd 0.1.0 (GPL-2.0) - Standalone Hi-MD SCSI/UMSC protocol helper\n"
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
        << "  carta-himd selftest-stack\n"
        << "  carta-himd session [--mock]\n";
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
        std::cout << "carta-himd 0.1.0 (GPL-2.0)\n";
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
    if (cmd == "selftest-stack") {
        return cmdSelftestStack();
    }
    if (cmd == "session") {
        bool mock = false;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--mock") == 0) {
                mock = true;
            }
        }
        return runSessionLoop(mock);
    }

    emitErrorResult("Unknown subcommand: " + cmd);
    return 2;
}
