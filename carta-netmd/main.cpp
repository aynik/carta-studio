/**
 * @file main.cpp
 * @brief Standalone GPL-2.0 NetMD USB & Type-S/Hi-MD protocol helper CLI (`carta-netmd`).
 *
 * Runs out-of-process from Carta Studio so `netmd-js` / `netmd-exploits` (GPL-2.0)
 * remain isolated in a standalone GPL-2.0 executable communicating over standard
 * CLI arguments, temporary binary files, and newline-delimited JSON (NDJSON)
 * progress/result events on `stdout`.
 */

#include "NetMdHostBridge.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
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
#include <sstream>
#include <string>
#include <vector>

extern "C" {
int netmd_pf_list_devices(void);
int netmd_pf_probe_device(const char* device_id_ptr, int device_id_len, int include_tracks);
int netmd_pf_erase_disc(const char* device_id_ptr, int device_id_len);
int netmd_pf_upload_track(const char* device_id_ptr, int device_id_len, int mode_code,
                          const unsigned char* wire_ptr, int wire_len, const char* hw_title_ptr,
                          int hw_title_len, const char* fw_title_ptr, int fw_title_len);
int netmd_pf_read_utoc_sectors(const char* device_id_ptr, int device_id_len);
int netmd_pf_write_utoc_sectors(const char* device_id_ptr, int device_id_len,
                                const unsigned char* sectors5_ptr, int sectors5_len,
                                int sectors_mask);
const unsigned char* netmd_pf_output_ptr(void);
std::size_t netmd_pf_output_len(void);
const char* netmd_pf_last_error(void);
void netmd_pf_release_scratch(void);
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
    const char* err = netmd_pf_last_error();
    if (err && err[0] != '\0') {
        return std::string(err);
    }
    const char* hostErr = carta_netmd_host_last_error();
    if (hostErr && hostErr[0] != '\0') {
        return std::string(hostErr);
    }
    return fallback ? std::string(fallback) : std::string("NetMD Porffor bridge error");
}

std::string copyPorfforOutputString() {
    const auto* ptr = netmd_pf_output_ptr();
    const auto len = netmd_pf_output_len();
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
    carta::setNetMdProgressCallback([](const carta::NetMdProgressSnapshot& snap) {
        std::ostringstream oss;
        oss << "{\"event\":\"progress\",\"operation\":\"" << jsonEscape(snap.operation)
            << "\",\"phase\":\"" << jsonEscape(snap.phase) << "\",\"progress\":" << std::fixed
            << std::setprecision(4) << snap.progress << ",\"bytes_done\":" << snap.bytesDone
            << ",\"bytes_total\":" << snap.bytesTotal << "}";
        emitJsonLine(oss.str());
    });
}

bool readBinaryFile(const std::string& path, std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        error = "Failed opening input file: " + path;
        return false;
    }
    const auto sz = f.tellg();
    if (sz < 0) {
        error = "Failed reading size of input file: " + path;
        return false;
    }
    out.resize(static_cast<std::size_t>(sz));
    f.seekg(0, std::ios::beg);
    if (sz > 0 && !f.read(reinterpret_cast<char*>(out.data()), sz)) {
        error = "Failed reading bytes from input file: " + path;
        return false;
    }
    return true;
}

bool writeBinaryFile(const std::string& path, const std::uint8_t* data, std::size_t size,
                     std::string& error) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "Failed opening output file: " + path;
        return false;
    }
    if (size > 0 &&
        !f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size))) {
        error = "Failed writing bytes to output file: " + path;
        return false;
    }
    return true;
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

// ============================================================================
// Core Operations
// ============================================================================

int cmdList() {
    if (netmd_pf_list_devices() < 0) {
        emitErrorResult(porfforErrorString("Failed to enumerate USB NetMD devices"));
        return 1;
    }
    std::string raw = copyPorfforOutputString();
    if (raw.empty()) {
        raw = "[]";
    }
    emitJsonLine("{\"event\":\"result\",\"ok\":true,\"devices\":" + raw + "}");
    return 0;
}

int cmdProbe(const std::string& deviceId, bool includeTracks = true) {
    if (deviceId.empty()) {
        emitErrorResult("NetMD device ID is empty");
        return 1;
    }
    if (netmd_pf_probe_device(deviceId.c_str(), static_cast<int>(deviceId.size()),
                              includeTracks ? 1 : 0) < 0) {
        emitErrorResult(porfforErrorString("Failed to probe NetMD device"));
        return 1;
    }
    const std::string raw = copyPorfforOutputString();
    if (raw.empty()) {
        emitErrorResult("Empty probe JSON returned by NetMD bridge");
        return 1;
    }
    emitJsonLine("{\"event\":\"result\",\"ok\":true,\"status\":" + raw + "}");
    return 0;
}

int cmdErase(const std::string& deviceId, bool confirmHardware) {
    if (deviceId.empty()) {
        emitErrorResult("NetMD device ID is empty");
        return 1;
    }
    const bool needsConfirm =
        !carta::isNetMdMockDeviceId(deviceId) || carta::isSimulateRealHardwareGuardOnMock();
    if (needsConfirm && !confirmHardware) {
        emitErrorResult("Refusing to erase real NetMD hardware without explicit confirmation "
                        "(--confirm-hardware).");
        return 1;
    }
    carta::ScopedHardwareWriteConfirmation guard(confirmHardware);
    carta::setNetMdProgress("erase", "Erasing MiniDisc (this may take 10-20s)...", 0.30);
    if (netmd_pf_erase_disc(deviceId.c_str(), static_cast<int>(deviceId.size())) < 0) {
        carta::clearNetMdProgress();
        emitErrorResult(porfforErrorString("Failed erasing MiniDisc"));
        return 1;
    }
    carta::clearNetMdProgress();
    emitJsonLine("{\"event\":\"result\",\"ok\":true}");
    return 0;
}

int cmdReadUtoc(const std::string& deviceId, const std::string& outPath) {
    if (deviceId.empty() || outPath.empty()) {
        emitErrorResult("read-utoc requires <deviceId> and --out <path>");
        return 1;
    }
    carta::setNetMdProgressWindow("read-utoc", "Reading UTOC Sectors 0/1/2/4 from MiniDisc...", 0.0,
                                  1.0);
    if (netmd_pf_read_utoc_sectors(deviceId.c_str(), static_cast<int>(deviceId.size())) < 0) {
        carta::clearNetMdProgress();
        emitErrorResult(porfforErrorString("Failed reading UTOC sectors from NetMD device"));
        return 1;
    }
    const auto* outPtr = netmd_pf_output_ptr();
    const auto outLen = netmd_pf_output_len();
    if (!outPtr || outLen != 5 * carta::kUtocSectorBytes) {
        carta::clearNetMdProgress();
        emitErrorResult("Unexpected UTOC sector payload length returned by NetMD bridge");
        return 1;
    }
    std::string ioErr;
    if (!writeBinaryFile(outPath, outPtr, outLen, ioErr)) {
        carta::clearNetMdProgress();
        emitErrorResult(ioErr);
        return 1;
    }
    carta::clearNetMdProgress();
    emitJsonLine("{\"event\":\"result\",\"ok\":true,\"bytes\":" + std::to_string(outLen) + "}");
    return 0;
}

int cmdWriteUtoc(const std::string& deviceId, const std::string& inPath, int mask,
                 bool confirmHardware) {
    if (deviceId.empty() || inPath.empty()) {
        emitErrorResult("write-utoc requires <deviceId> and --in <path>");
        return 1;
    }
    const bool needsConfirm =
        !carta::isNetMdMockDeviceId(deviceId) || carta::isSimulateRealHardwareGuardOnMock();
    if (needsConfirm && !confirmHardware) {
        emitErrorResult("Refusing to modify real NetMD hardware without explicit confirmation "
                        "(--confirm-hardware).");
        return 1;
    }
    std::vector<std::uint8_t> sectors;
    std::string ioErr;
    if (!readBinaryFile(inPath, sectors, ioErr)) {
        emitErrorResult(ioErr);
        return 1;
    }
    if (sectors.size() != 5 * carta::kUtocSectorBytes) {
        emitErrorResult("Expected 11760-byte 5-sector UTOC input file");
        return 1;
    }
    carta::ScopedHardwareWriteConfirmation guard(confirmHardware);
    carta::setNetMdProgressWindow("write-utoc", "Writing UTOC Sectors 0/1/2/4 & running TOCWRT...",
                                  0.0, 1.0);
    if (netmd_pf_write_utoc_sectors(deviceId.c_str(), static_cast<int>(deviceId.size()),
                                    sectors.data(), static_cast<int>(sectors.size()), mask) < 0) {
        carta::clearNetMdProgress();
        emitErrorResult(porfforErrorString("Failed writing UTOC sectors via ForceTOCEdit"));
        return 1;
    }
    carta::clearNetMdProgress();
    emitJsonLine("{\"event\":\"result\",\"ok\":true}");
    return 0;
}

int cmdUpload(const std::string& deviceId, int modeCode, const std::string& inPath,
              const std::string& hwTitle, const std::string& fwTitle, bool confirmHardware) {
    if (deviceId.empty() || inPath.empty() || modeCode < 0 || modeCode > 3) {
        emitErrorResult("upload requires <deviceId>, --mode <0..3>, and --in <path>");
        return 1;
    }
    const bool needsConfirm =
        !carta::isNetMdMockDeviceId(deviceId) || carta::isSimulateRealHardwareGuardOnMock();
    if (needsConfirm && !confirmHardware) {
        emitErrorResult("Refusing to upload to real NetMD hardware without explicit confirmation "
                        "(--confirm-hardware).");
        return 1;
    }
    std::vector<std::uint8_t> wire;
    std::string ioErr;
    if (!readBinaryFile(inPath, wire, ioErr)) {
        emitErrorResult(ioErr);
        return 1;
    }
    if (wire.empty()) {
        emitErrorResult("Upload wire payload file is empty");
        return 1;
    }
    const auto wireLen = static_cast<std::int64_t>(wire.size());
    carta::ScopedHardwareWriteConfirmation guard(confirmHardware);
    carta::setNetMdUploadSpanProgressWindow("upload", "Uploading audio span...", 0.0, 1.0, 0,
                                            wireLen, wireLen, wireLen);
    const int trkIdx = netmd_pf_upload_track(deviceId.c_str(), static_cast<int>(deviceId.size()),
                                             modeCode, wire.data(), static_cast<int>(wire.size()),
                                             hwTitle.c_str(), static_cast<int>(hwTitle.size()),
                                             fwTitle.c_str(), static_cast<int>(fwTitle.size()));
    carta::clearNetMdProgress();
    if (trkIdx < 0) {
        emitErrorResult(porfforErrorString("Failed uploading NetMD audio span"));
        return 1;
    }
    emitJsonLine("{\"event\":\"result\",\"ok\":true,\"track_index\":" + std::to_string(trkIdx) +
                 "}");
    return 0;
}

int cmdSelftestStack() {
    carta::setMockNetMdEnabled(true);
    carta::resetMockNetMdState();

    constexpr std::size_t kLargeSectors = 8600; // 8600 * 2332 = 20,055,200 bytes
    std::vector<std::uint8_t> largeSpWire(kLargeSectors * 2332u, 0);
    for (std::size_t g = 0; g < largeSpWire.size(); g += 212u) {
        largeSpWire[g + 0] = static_cast<std::uint8_t>((g / 212u) & 0xFFu);
        largeSpWire[g + 1] = 0x15u;
        largeSpWire[g + 100] = static_cast<std::uint8_t>((g / 424u) & 0xFFu);
        largeSpWire[g + 210] = largeSpWire[g + 1];
        largeSpWire[g + 211] = largeSpWire[g + 0];
    }

    struct StackUploadCtx {
        const std::vector<std::uint8_t>* wire = nullptr;
        int trkIdx = -1;
        std::string error;
    } ctx{&largeSpWire, -1, {}};

#if defined(_WIN32)
    auto threadFn = [](LPVOID raw) -> DWORD WINAPI {
        auto* c = static_cast<StackUploadCtx*>(raw);
        const char dev[] = "mock:mz-n920";
        const char title[] = "LARGE SP";
        c->trkIdx = netmd_pf_upload_track(dev, static_cast<int>(sizeof(dev) - 1), 0,
                                          c->wire->data(), static_cast<int>(c->wire->size()), title,
                                          static_cast<int>(sizeof(title) - 1), "", 0);
        if (c->trkIdx < 0) {
            c->error = porfforErrorString("netmd_pf_upload_track failed");
        }
        return 0;
    };
    HANDLE hThread = CreateThread(nullptr, 544u * 1024u, threadFn, &ctx,
                                  STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!hThread) {
        emitErrorResult("CreateThread failed");
        return 1;
    }
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
#else
    auto threadFn = [](void* raw) -> void* {
        auto* c = static_cast<StackUploadCtx*>(raw);
        const char dev[] = "mock:mz-n920";
        const char title[] = "LARGE SP";
        c->trkIdx = netmd_pf_upload_track(dev, static_cast<int>(sizeof(dev) - 1), 0,
                                          c->wire->data(), static_cast<int>(c->wire->size()), title,
                                          static_cast<int>(sizeof(title) - 1), "", 0);
        if (c->trkIdx < 0) {
            c->error = porfforErrorString("netmd_pf_upload_track failed");
        }
        return nullptr;
    };

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0 || pthread_attr_setstacksize(&attr, 544u * 1024u) != 0) {
        emitErrorResult("pthread_attr setup failed");
        return 1;
    }
    pthread_t tid{};
    if (pthread_create(&tid, &attr, threadFn, &ctx) != 0) {
        pthread_attr_destroy(&attr);
        emitErrorResult("pthread_create failed");
        return 1;
    }
    pthread_attr_destroy(&attr);
    pthread_join(tid, nullptr);
#endif

    if (ctx.trkIdx != 0) {
        emitErrorResult("20 MB SP upload on 544 KB stack failed: " + ctx.error);
        return 1;
    }
    const auto snap = carta::getMockNetMdSnapshot();
    if (snap.uploadedTracks.size() != 1 ||
        snap.uploadedTracks[0].decryptedAudioBytes != largeSpWire) {
        emitErrorResult("20 MB SP decrypted payload mismatch");
        return 1;
    }
    emitJsonLine(
        "{\"event\":\"result\",\"ok\":true,\"bytes\":" + std::to_string(largeSpWire.size()) + "}");
    return 0;
}

int cmdMockSnapshot(const std::string& outDir) {
    const auto snap = carta::getMockNetMdSnapshot();
    std::string ioErr;
    if (!outDir.empty()) {
        std::vector<std::uint8_t> committed(5 * carta::kUtocSectorBytes, 0);
        for (std::size_t s = 0; s < 5; ++s) {
            std::memcpy(committed.data() + s * carta::kUtocSectorBytes,
                        snap.committedUtoc[s].data(), carta::kUtocSectorBytes);
        }
        if (!writeBinaryFile(outDir + "/committed_utoc.bin", committed.data(), committed.size(),
                             ioErr)) {
            emitErrorResult(ioErr);
            return 1;
        }
        for (std::size_t i = 0; i < snap.uploadedTracks.size(); ++i) {
            const auto& tr = snap.uploadedTracks[i];
            if (!writeBinaryFile(outDir + "/track_" + std::to_string(i) + ".bin",
                                 tr.decryptedAudioBytes.data(), tr.decryptedAudioBytes.size(),
                                 ioErr)) {
                emitErrorResult(ioErr);
                return 1;
            }
        }
    }

    std::ostringstream oss;
    oss << "{\"event\":\"result\",\"ok\":true"
        << ",\"sp_upload_used_count\":" << snap.spUploadUsedCount
        << ",\"safety_patch_applied\":" << (snap.safetyPatchApplied ? "true" : "false")
        << ",\"toc_commit_count\":" << snap.tocCommitCount
        << ",\"toc_poll_count\":" << snap.tocPollCount << ",\"slept_ms\":" << snap.sleptMs
        << ",\"uploaded_tracks\":[";
    for (std::size_t i = 0; i < snap.uploadedTracks.size(); ++i) {
        const auto& tr = snap.uploadedTracks[i];
        if (i > 0) {
            oss << ",";
        }
        oss << "{\"index\":" << tr.trackIndex << ",\"mode\":\"" << jsonEscape(tr.mode) << "\""
            << ",\"bytes\":" << tr.decryptedAudioBytes.size()
            << ",\"start_linear\":" << tr.startAddr.toLinearGroup()
            << ",\"end_linear\":" << tr.endAddr.toLinearGroup() << "}";
    }
    oss << "]}";
    emitJsonLine(oss.str());
    return 0;
}

int runSessionLoop(bool enableMock) {
    if (enableMock) {
        carta::setMockNetMdEnabled(true);
        carta::resetMockNetMdState();
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
            const bool includeTracks = (tok.size() >= 3) ? (tok[2] != "0") : true;
            cmdProbe(percentDecode(tok[1]), includeTracks);
        } else if (op == "erase" && tok.size() >= 3) {
            cmdErase(percentDecode(tok[1]), tok[2] == "1");
        } else if (op == "read-utoc" && tok.size() >= 3) {
            cmdReadUtoc(percentDecode(tok[1]), percentDecode(tok[2]));
        } else if (op == "write-utoc" && tok.size() >= 5) {
            cmdWriteUtoc(percentDecode(tok[1]), percentDecode(tok[2]), std::atoi(tok[3].c_str()),
                         tok[4] == "1");
        } else if (op == "upload" && tok.size() >= 7) {
            cmdUpload(percentDecode(tok[1]), std::atoi(tok[2].c_str()), percentDecode(tok[3]),
                      percentDecode(tok[4]), percentDecode(tok[5]), tok[6] == "1");
        } else if (op == "mock-reset" && enableMock) {
            carta::resetMockNetMdState();
            emitJsonLine("{\"event\":\"result\",\"ok\":true}");
        } else if (op == "mock-seed-utoc" && enableMock && tok.size() >= 2) {
            std::vector<std::uint8_t> raw;
            std::string ioErr;
            const std::string inPath = percentDecode(tok[1]);
            if (!readBinaryFile(inPath, raw, ioErr)) {
                emitErrorResult(ioErr);
                continue;
            }
            if (raw.size() != 5 * carta::kUtocSectorBytes) {
                emitErrorResult("Expected 11760-byte (5-sector) UTOC binary for mock-seed-utoc");
                continue;
            }
            std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> secs{};
            for (std::size_t s = 0; s < 5; ++s) {
                std::memcpy(secs[s].data(), raw.data() + s * carta::kUtocSectorBytes,
                            carta::kUtocSectorBytes);
            }
            carta::seedMockNetMdUtoc(secs);
            emitJsonLine("{\"event\":\"result\",\"ok\":true}");
        } else if (op == "mock-guard" && enableMock && tok.size() >= 2) {
            carta::setSimulateRealHardwareGuardOnMock(tok[1] == "1");
            emitJsonLine("{\"event\":\"result\",\"ok\":true}");
        } else if (op == "mock-snapshot" && enableMock) {
            cmdMockSnapshot(tok.size() >= 2 ? percentDecode(tok[1]) : std::string());
        } else {
            emitErrorResult("Unknown or malformed session command: " + op);
        }
    }
    return 0;
}

void printUsage() {
    std::cerr << "carta-netmd 0.3.0 (GPL-2.0) - Standalone NetMD USB helper\n"
              << "Built with netmd-js & netmd-exploits (GPL-2.0) and libusb-1.0 (LGPL-2.1).\n"
              << "Usage:\n"
              << "  carta-netmd list\n"
              << "  carta-netmd probe <device-id> [--no-tracks]\n"
              << "  carta-netmd erase <device-id> [--confirm-hardware]\n"
              << "  carta-netmd read-utoc <device-id> --out <sectors5.bin>\n"
              << "  carta-netmd write-utoc <device-id> --in <sectors5.bin> [--mask 23] "
                 "[--confirm-hardware]\n"
              << "  carta-netmd upload <device-id> --mode <0..3> --in <wire.bin> [--hw-title T] "
                 "[--fw-title T] [--confirm-hardware]\n";
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
        std::cout << "carta-netmd 0.3.0 (GPL-2.0)\n";
        return 0;
    }
    if (cmd == "--help" || cmd == "-h") {
        printUsage();
        return 0;
    }
    if (cmd == "list") {
        return cmdList();
    }
    if (cmd == "probe") {
        if (argc < 3) {
            emitErrorResult("Usage: carta-netmd probe <device-id> [--no-tracks]");
            return 2;
        }
        bool noTracks = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--no-tracks") == 0) {
                noTracks = true;
            }
        }
        return cmdProbe(argv[2], !noTracks);
    }
    if (cmd == "erase") {
        if (argc < 3) {
            emitErrorResult("Usage: carta-netmd erase <device-id> [--confirm-hardware]");
            return 2;
        }
        bool confirm = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--confirm-hardware") == 0) {
                confirm = true;
            }
        }
        return cmdErase(argv[2], confirm);
    }
    if (cmd == "read-utoc") {
        if (argc < 3) {
            emitErrorResult("Usage: carta-netmd read-utoc <device-id> --out <path>");
            return 2;
        }
        std::string outPath;
        for (int i = 3; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--out") == 0) {
                outPath = argv[++i];
            }
        }
        return cmdReadUtoc(argv[2], outPath);
    }
    if (cmd == "write-utoc") {
        if (argc < 3) {
            emitErrorResult("Usage: carta-netmd write-utoc <device-id> --in <path> [--mask 23] "
                            "[--confirm-hardware]");
            return 2;
        }
        std::string inPath;
        int mask = 0x17;
        bool confirm = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--in") == 0 && i + 1 < argc) {
                inPath = argv[++i];
            } else if (std::strcmp(argv[i], "--mask") == 0 && i + 1 < argc) {
                mask = std::atoi(argv[++i]);
            } else if (std::strcmp(argv[i], "--confirm-hardware") == 0) {
                confirm = true;
            }
        }
        return cmdWriteUtoc(argv[2], inPath, mask, confirm);
    }
    if (cmd == "upload") {
        if (argc < 3) {
            emitErrorResult("Usage: carta-netmd upload <device-id> --mode <0..3> --in <path> "
                            "[--hw-title T] [--fw-title T] [--confirm-hardware]");
            return 2;
        }
        int modeCode = -1;
        std::string inPath;
        std::string hwTitle;
        std::string fwTitle;
        bool confirm = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
                modeCode = std::atoi(argv[++i]);
            } else if (std::strcmp(argv[i], "--in") == 0 && i + 1 < argc) {
                inPath = argv[++i];
            } else if (std::strcmp(argv[i], "--hw-title") == 0 && i + 1 < argc) {
                hwTitle = argv[++i];
            } else if (std::strcmp(argv[i], "--fw-title") == 0 && i + 1 < argc) {
                fwTitle = argv[++i];
            } else if (std::strcmp(argv[i], "--confirm-hardware") == 0) {
                confirm = true;
            }
        }
        return cmdUpload(argv[2], modeCode, inPath, hwTitle, fwTitle, confirm);
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
