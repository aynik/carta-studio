/**
 * @file NetMdHostBridge.cpp
 * Implementation of Porffor NetMD host C ABI callbacks, libusb transport,
 * CommonCrypto DES/3DES/RNG, iconv CP932 conversion, and Sony MZ-N920 Type-S
 * mock device emulation.
 *
 * Security & Hardware Safety:
 * - Real NetMD USB writes (mutating factory/vendor control OUT commands and all
 *   bulk OUT transfers) are blocked under `g_hostMutex` unless
 *   `g_hardwareWriteConfirmed` is active via `ScopedHardwareWriteConfirmation`
 *   or `setHardwareWriteConfirmed(true)`.
 * - `mock:mz-n920` simulates firmware `S1.600` patch peripheral slots at
 *   `0x03802000`, EKB Open Source Retail-MAC session keys, `SPUpload`
 *   2,332-byte + 100-byte DRAM padding removal, cluster allocation starting at
 *   `50.0.0`, and deferred UTOC commits triggered by `USBCodeExecution`.
 */

#include "NetMdHostBridge.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <iconv.h>
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <stdlib.h>
#else
#include <sys/random.h>
#endif
#endif

#if __has_include(<libusb-1.0/libusb.h>)
#include <libusb-1.0/libusb.h>
#else
#include <libusb.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace carta {
namespace {

// ============================================================================
// Internal Constants & Known Device Table
// ============================================================================

constexpr const char* kMockDeviceId = "mock:mz-n920";
constexpr std::uint16_t kMockVendorId = 0x054c;
constexpr std::uint16_t kMockProductId = 0x0188;
constexpr const char* kMockProductName = "Sony MZ-N920 (Mock Type-S)";

// PatchPeripheralBase::NETMD (58728448 = 0x03802000) + SPUpload variant/slot addresses ('S1.600')
constexpr std::uint32_t kPatchPeripheralBaseNetMd = 0x03802000u;
constexpr std::uint32_t kPatchControlAddr = kPatchPeripheralBaseNetMd + 8u * 0x10u;
constexpr std::uint32_t kMockFiqVariantAddr = 0x02003effu + 208u; // 0x02003fcf
constexpr std::uint32_t kMockHfecAddr = 0x000000C4u;
constexpr std::uint32_t kMockKillEepromAddr = 0x0001E58Cu;
constexpr std::uint32_t kMockUsbCodeExecAddr = 0x0000E69Cu;
constexpr std::uint32_t kMockSpUploadTrackTypeAddr = 0x000852B0u;
constexpr std::uint32_t kMockForceTocEditInFirmwareAddr = 0x00016B6Fu;

// EKBOpenSource root key from netmd-js/dist/netmd-ekb.js (Big Endian / raw bytes)
constexpr std::array<std::uint8_t, 16> kEkbOpenSourceRootKey = {
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0, 0x0f, 0xed, 0xcb, 0xa9, 0x87, 0x65, 0x43, 0x21};

struct KnownNetMdProduct {
    std::uint16_t vendorId;
    std::uint16_t productId;
    const char* name;
};

constexpr KnownNetMdProduct kKnownNetMdProducts[] = {
    {0x04dd, 0x7202, "Sharp IM-MT899H"},
    {0x04dd, 0x9013, "Sharp IM-DR400"},
    {0x04dd, 0x9014, "Sharp IM-DR80"},
    {0x054c, 0x0034, "Sony PCLK-XX"},
    {0x054c, 0x0036, "Sony"},
    {0x054c, 0x0075, "Sony MZ-N1"},
    {0x054c, 0x007c, "Sony"},
    {0x054c, 0x0080, "Sony LAM-1"},
    {0x054c, 0x0081, "Sony MDS-JB980/MDS-NT1/MDS-JE780"},
    {0x054c, 0x0084, "Sony MZ-N505"},
    {0x054c, 0x0085, "Sony MZ-S1"},
    {0x054c, 0x0086, "Sony MZ-N707"},
    {0x054c, 0x008e, "Sony CMT-C7NT"},
    {0x054c, 0x0097, "Sony PCGA-MDN1"},
    {0x054c, 0x00ad, "Sony CMT-L7HD"},
    {0x054c, 0x00c6, "Sony MZ-N10"},
    {0x054c, 0x00c7, "Sony MZ-N910"},
    {0x054c, 0x00c8, "Sony MZ-N710/NF810"},
    {0x054c, 0x00c9, "Sony MZ-N510/N610"},
    {0x054c, 0x00ca, "Sony MZ-NE410/NF520D"},
    {0x054c, 0x00e7, "Sony CMT-M333NT/M373NT"},
    {0x054c, 0x00eb, "Sony MZ-NE810/NE910"},
    {0x054c, 0x0101, "Sony LAM"},
    {0x054c, 0x0113, "Aiwa AM-NX1"},
    {0x054c, 0x011a, "Sony CMT-SE7"},
    {0x054c, 0x013f, "Sony MDS-S500"},
    {0x054c, 0x0148, "Sony MDS-A1"},
    {0x054c, 0x014c, "Aiwa AM-NX9"},
    {0x054c, 0x017e, "Sony MZ-NH1"},
    {0x054c, 0x0180, "Sony MZ-NH3D"},
    {0x054c, 0x0182, "Sony MZ-NH900"},
    {0x054c, 0x0184, "Sony MZ-NH700/NH800"},
    {0x054c, 0x0186, "Sony MZ-NH600"},
    {0x054c, 0x0187, "Sony MZ-NH600D"},
    {0x054c, 0x0188, "Sony MZ-N920"},
    {0x054c, 0x018a, "Sony LAM-3"},
    {0x054c, 0x01e9, "Sony MZ-DH10P"},
    {0x054c, 0x0219, "Sony MZ-RH10"},
    {0x054c, 0x021b, "Sony MZ-RH710/MZ-RH910"},
    {0x054c, 0x021d, "Sony CMT-AH10"},
    {0x054c, 0x022c, "Sony CMT-AH10"},
    {0x054c, 0x023c, "Sony DS-HMD1"},
    {0x054c, 0x0286, "Sony MZ-RH1"},
    {0x0b28, 0x1004, "Kenwood MDX-J9"},
    {0x04da, 0x23b3, "Panasonic SJ-MR250"},
    {0x04da, 0x23b6, "Panasonic SJ-MR270"},
    {0x0411, 0x0083, "Buffalo MD-HUSB"},
};

// ============================================================================
// Crypto, Encoding, & USB Identification Helpers
// ============================================================================

const char* lookupKnownNetMdName(std::uint16_t vid, std::uint16_t pid) noexcept {
    for (const auto& entry : kKnownNetMdProducts) {
        if (entry.vendorId == vid && entry.productId == pid) {
            return entry.name;
        }
    }
    return nullptr;
}

std::string jsonEscape(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (const char ch : input) {
        const unsigned char c = static_cast<unsigned char>(ch);
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

std::uint16_t readLe16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
}

std::uint32_t readLe32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t readBe16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8) | p[1]);
}

std::uint32_t readBe32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

void writeLe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

std::uint8_t intToBcdByte(int v) noexcept {
    v = std::clamp(v, 0, 99);
    return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10));
}

bool retailMac8(const std::array<std::uint8_t, 16>& rootKey, const std::uint8_t* data,
                std::size_t len, std::array<std::uint8_t, 8>& outMac) {
    if (len < 16 || (len % 8) != 0) {
        return false;
    }
    const std::size_t headLen = len - 8;
    std::vector<std::uint8_t> step1(headLen);
    std::uint8_t zeroIv[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (carta_netmd_host_des_crypt(0, 1, 1, rootKey.data(), 8, zeroIv, data,
                                   static_cast<int>(headLen), step1.data()) < 0) {
        return false;
    }
    const std::uint8_t* iv2 = step1.data() + (headLen - 8);
    if (carta_netmd_host_des_crypt(1, 1, 1, rootKey.data(), 16, iv2, data + headLen, 8,
                                   outMac.data()) < 0) {
        return false;
    }
    return true;
}

std::string utf8ToCp932Lossy(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    std::vector<std::uint8_t> out(utf8.size() * 4 + 16);
    const int n = carta_netmd_host_cp932(0, reinterpret_cast<const std::uint8_t*>(utf8.data()),
                                         static_cast<int>(utf8.size()), out.data(),
                                         static_cast<int>(out.size()));
    if (n <= 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(out.data()), static_cast<std::size_t>(n));
}

std::string cp932ToUtf8Lossy(const std::vector<std::uint8_t>& cp932) {
    if (cp932.empty()) {
        return {};
    }
    std::vector<std::uint8_t> out(cp932.size() * 4 + 16);
    const int n = carta_netmd_host_cp932(1, cp932.data(), static_cast<int>(cp932.size()),
                                         out.data(), static_cast<int>(out.size()));
    if (n <= 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(out.data()), static_cast<std::size_t>(n));
}

// ============================================================================
// Mock Type-S Device State & Host Globals
// ============================================================================

struct PatchSlot {
    std::array<std::uint8_t, 4> control{};
    std::uint32_t address = 0;
    std::array<std::uint8_t, 4> value{};

    bool isActiveFor(std::uint32_t targetAddr) const noexcept {
        return (control[0] & 0x01u) != 0 && address == targetAddr;
    }
};

struct MockDeviceState {
    bool open = false;
    std::deque<std::vector<std::uint8_t>> replyQueue;
    std::array<PatchSlot, 8> patchSlots{};
    std::map<std::uint32_t, std::uint8_t> extraMappedMem;
    bool safetyPatchApplied = false;
    bool killEepromWriteApplied = false;
    bool usbCodeExecApplied = false;
    int spUploadUsedCount = 0;
    int tocCommitCount = 0;
    int tocPollCount = 0;
    int tocPollBusyRemaining = 0;
    std::int64_t sleptMs = 0;
    std::string displayOverride;

    bool inSecureSession = false;
    bool haveSessionKey = false;
    std::array<std::uint8_t, 8> sessionKey{};
    bool haveKek = false;
    std::array<std::uint8_t, 8> contentKek{};

    bool awaitingBulk = false;
    int pendingWireFormat = 0;
    int pendingDiscFormat = 0;
    int pendingFrameCount = 0;
    std::size_t expectedTotalBulkBytes = 0;
    std::vector<std::uint8_t> bulkBuffer;

    std::uint16_t nextFreeCluster = 50;
    std::string discTitleHalf;
    std::string discTitleFull;
    std::vector<NetMdMockUploadedTrack> uploadedTracks;
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> utocCache{};
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> committedUtoc{};

    void refreshPatchFlags() noexcept {
        safetyPatchApplied = false;
        usbCodeExecApplied = false;
        for (const auto& slot : patchSlots) {
            if (slot.isActiveFor(kMockHfecAddr)) {
                safetyPatchApplied = true;
            }
            if (slot.isActiveFor(kMockKillEepromAddr)) {
                killEepromWriteApplied = true;
            }
            if (slot.isActiveFor(kMockUsbCodeExecAddr)) {
                usbCodeExecApplied = true;
            }
        }
    }

    const PatchSlot* findActivePatch(std::uint32_t targetAddr) const noexcept {
        for (const auto& slot : patchSlots) {
            if (slot.isActiveFor(targetAddr)) {
                return &slot;
            }
        }
        return nullptr;
    }

    static void encodeUtocAddr(std::uint8_t* p, const UtocAddress& addr) noexcept {
        p[0] = static_cast<std::uint8_t>((addr.cluster >> 6) & 0xFFu);
        p[1] =
            static_cast<std::uint8_t>(((addr.cluster & 0x3Fu) << 2) | ((addr.sector & 0x30u) >> 4));
        p[2] = static_cast<std::uint8_t>(((addr.sector & 0x0Fu) << 4) | (addr.group & 0x0Fu));
    }

    static UtocAddress decodeUtocAddr(const std::uint8_t* p) noexcept {
        return {static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 6) | (p[1] >> 2)),
                static_cast<std::uint8_t>(((p[1] & 0x03u) << 4) | (p[2] >> 4)),
                static_cast<std::uint8_t>(p[2] & 0x0Fu)};
    }

    static std::uint8_t mockUtocModeForTrack(const std::string& mode) noexcept {
        // Writable (0x80) | SCMS unrestricted (0x40) | SP/Stereo bits.
        std::uint8_t m = 0xC0u;
        if (mode == "sp-stereo") {
            m |= 0x04u | 0x02u;
        } else if (mode == "sp-mono") {
            m |= 0x04u;
        } else if (mode == "lp2") {
            m |= 0x02u;
        }
        return m;
    }

    static std::string modeFromMockUtocByte(std::uint8_t m) {
        const bool stereo = (m & 0x02u) != 0;
        const bool sp = (m & 0x04u) != 0;
        if (sp && stereo) {
            return "sp-stereo";
        }
        if (sp) {
            return "sp-mono";
        }
        if (stereo) {
            return "lp2";
        }
        return "lp4";
    }

    static bool isMockPtocSector(const std::array<std::uint8_t, kUtocSectorBytes>& sec) noexcept {
        return sec[0x18] == 'M' && sec[0x19] == 'I' && sec[0x1A] == 'N' && sec[0x1B] == 'I';
    }

    std::size_t mockSector4Index() const noexcept {
        return isMockPtocSector(utocCache[4]) ? 3u : 4u;
    }

    static std::string readMockDiscTitle(const std::array<std::uint8_t, kUtocSectorBytes>& sec) {
        if (sec[0x130] == 0) {
            return {};
        }
        std::vector<std::uint8_t> raw;
        bool hitNul = false;
        for (int j = 0; j < 7; ++j) {
            const std::uint8_t b = sec[0x130 + static_cast<std::size_t>(j)];
            if (b == 0) {
                hitNul = true;
                break;
            }
            raw.push_back(b);
        }
        if (!hitNul) {
            std::uint8_t slot = sec[0x137];
            std::array<bool, 256> seen{};
            while (slot != 0 && !seen[slot]) {
                seen[slot] = true;
                const std::uint8_t* cell = sec.data() + 0x130 + static_cast<std::size_t>(slot) * 8u;
                bool done = false;
                for (int j = 0; j < 7; ++j) {
                    if (cell[j] == 0) {
                        done = true;
                        break;
                    }
                    raw.push_back(cell[j]);
                }
                if (done) {
                    break;
                }
                slot = cell[7];
            }
        }
        return cp932ToUtf8Lossy(raw);
    }

    static std::string readMockTitleChain(const std::array<std::uint8_t, kUtocSectorBytes>& sec,
                                          int slotIndex) {
        std::uint8_t slot = sec[0x30 + static_cast<std::size_t>(slotIndex)];
        std::array<bool, 256> seen{};
        std::vector<std::uint8_t> raw;
        while (slot != 0 && !seen[slot]) {
            seen[slot] = true;
            const std::uint8_t* cell = sec.data() + 0x130 + static_cast<std::size_t>(slot) * 8u;
            for (int j = 0; j < 7; ++j) {
                if (cell[j] == 0) {
                    slot = 0;
                    break;
                }
                raw.push_back(cell[j]);
            }
            if (slot != 0) {
                slot = cell[7];
            }
        }
        return cp932ToUtf8Lossy(raw);
    }

    static void initMockEmptyTitleSector(std::array<std::uint8_t, kUtocSectorBytes>& sec,
                                         std::uint8_t charCode) noexcept {
        sec.fill(0);
        sec[0x2B] = charCode;
        sec[0x2F] = 1;
        for (int i = 1; i < 255; ++i) {
            sec[0x130 + static_cast<std::size_t>(i) * 8u + 7u] = static_cast<std::uint8_t>(i + 1);
        }
        sec[0x130 + 255u * 8u + 7u] = 0;
    }

    static bool writeMockTitleToSector(std::array<std::uint8_t, kUtocSectorBytes>& sec,
                                       int slotIndex, const std::vector<std::uint8_t>& rawBytes) {
        auto freeChain = [&](std::uint8_t head) {
            std::array<bool, 256> seen{};
            std::uint8_t s = head;
            while (s != 0 && !seen[s]) {
                seen[s] = true;
                std::uint8_t* cell = sec.data() + 0x130 + static_cast<std::size_t>(s) * 8u;
                const std::uint8_t next = cell[7];
                std::fill(cell, cell + 7, 0);
                cell[7] = sec[0x2F];
                sec[0x2F] = s;
                s = next;
            }
        };
        auto allocChain = [&](const std::uint8_t* bytes, std::size_t len,
                              std::uint8_t& outHead) -> bool {
            outHead = 0;
            if (len == 0) {
                return true;
            }
            const std::size_t needed = (len + 6u) / 7u;
            std::vector<std::uint8_t> allocated;
            allocated.reserve(needed);
            for (std::size_t k = 0; k < needed; ++k) {
                std::uint8_t s = sec[0x2F];
                if (s == 0) {
                    return false;
                }
                std::uint8_t* cell = sec.data() + 0x130 + static_cast<std::size_t>(s) * 8u;
                sec[0x2F] = cell[7];
                std::fill(cell, cell + 8, 0);
                allocated.push_back(s);
            }
            for (std::size_t k = 0; k < allocated.size(); ++k) {
                std::uint8_t* cell =
                    sec.data() + 0x130 + static_cast<std::size_t>(allocated[k]) * 8u;
                const std::size_t off = k * 7u;
                for (std::size_t j = 0; j < 7u && off + j < len; ++j) {
                    cell[j] = bytes[off + j];
                }
                cell[7] = (k + 1u < allocated.size()) ? allocated[k + 1u] : 0;
            }
            outHead = allocated.front();
            return true;
        };

        if (slotIndex == 0) {
            if (sec[0x137] != 0) {
                freeChain(sec[0x137]);
            }
            std::fill(sec.begin() + 0x130, sec.begin() + 0x138, 0);
            if (rawBytes.empty()) {
                return true;
            }
            const std::size_t headLen = std::min<std::size_t>(7u, rawBytes.size());
            std::copy_n(rawBytes.begin(), headLen, sec.begin() + 0x130);
            if (rawBytes.size() <= 7u) {
                return true;
            }
            return allocChain(rawBytes.data() + 7u, rawBytes.size() - 7u, sec[0x137]);
        }

        if (slotIndex < 1 || slotIndex >= 256) {
            return false;
        }
        const std::uint8_t oldHead = sec[0x30 + static_cast<std::size_t>(slotIndex)];
        if (oldHead != 0) {
            freeChain(oldHead);
            sec[0x30 + static_cast<std::size_t>(slotIndex)] = 0;
        }
        if (rawBytes.empty()) {
            return true;
        }
        return allocChain(rawBytes.data(), rawBytes.size(),
                          sec[0x30 + static_cast<std::size_t>(slotIndex)]);
    }

    void syncMockTracksFromUtocCache() {
        const auto& sec0 = utocCache[0];
        const int nTracks = std::min<int>(sec0[0x1F], 255);
        const std::size_t sec4Idx = mockSector4Index();

        const auto prevTracks = std::move(uploadedTracks);
        uploadedTracks.clear();
        uploadedTracks.reserve(static_cast<std::size_t>(nTracks));

        discTitleHalf = readMockDiscTitle(utocCache[1]);
        discTitleFull = readMockDiscTitle(utocCache[sec4Idx]);

        std::uint16_t maxEndCluster = 49;
        for (int t = 1; t <= nTracks; ++t) {
            NetMdMockUploadedTrack trk;
            trk.trackIndex = t - 1;
            trk.halfWidthTitle = readMockTitleChain(utocCache[1], t);
            trk.fullWidthTitle = readMockTitleChain(utocCache[sec4Idx], t);
            std::uint8_t slot = sec0[0x30 + static_cast<std::size_t>(t)];
            std::array<bool, 256> seen{};
            std::int64_t totalUnits = 0;
            bool firstFrag = true;
            while (slot != 0 && !seen[slot]) {
                seen[slot] = true;
                const std::uint8_t* f = sec0.data() + 0x130 + static_cast<std::size_t>(slot) * 8u;
                const auto s = decodeUtocAddr(f);
                const auto e = decodeUtocAddr(f + 4);
                const auto modeByte = f[3];
                if (s.cluster != 0 || s.sector != 0 || s.group != 0 || e.cluster != 0 ||
                    e.sector != 0 || e.group != 0) {
                    if (firstFrag) {
                        trk.startAddr = s;
                        trk.mode = modeFromMockUtocByte(modeByte);
                        firstFrag = false;
                    }
                    trk.endAddr = e;
                    maxEndCluster = std::max(maxEndCluster, std::max(s.cluster, e.cluster));
                    if (e.toLinearGroup() >= s.toLinearGroup()) {
                        totalUnits += (e.toLinearGroup() - s.toLinearGroup() + 1);
                    }
                }
                slot = f[7];
            }
            if (trk.mode == "sp-stereo") {
                trk.frameCount = static_cast<int>(totalUnits / 2);
            } else if (trk.mode == "lp4") {
                trk.frameCount = static_cast<int>(totalUnits * 2);
            } else {
                trk.frameCount = static_cast<int>(totalUnits);
            }
            for (const auto& prev : prevTracks) {
                if (prev.startAddr.cluster == trk.startAddr.cluster &&
                    prev.startAddr.sector == trk.startAddr.sector &&
                    prev.startAddr.group == trk.startAddr.group &&
                    prev.endAddr.cluster == trk.endAddr.cluster &&
                    prev.endAddr.sector == trk.endAddr.sector &&
                    prev.endAddr.group == trk.endAddr.group && !prev.decryptedAudioBytes.empty()) {
                    trk.decryptedAudioBytes = prev.decryptedAudioBytes;
                    break;
                }
            }
            uploadedTracks.push_back(std::move(trk));
        }
        nextFreeCluster = std::max<std::uint16_t>(50, static_cast<std::uint16_t>(maxEndCluster + 1));
    }

    bool appendMockTrackToUtocCache(NetMdMockUploadedTrack& rec, std::size_t soundGroups,
                                      std::string& error) {
        auto& sec0 = utocCache[0];
        const int nTracks = sec0[0x1F];
        if (nTracks >= 255) {
            error = "MockNetMd: track table exceeded 255 tracks.";
            return false;
        }

        const std::uint16_t clustersNeeded =
            static_cast<std::uint16_t>(std::max<std::size_t>(1, (soundGroups + 351u) / 352u));

        // Find free slot for the new track fragment (from free list or scanning unused slots).
        std::uint8_t newSlot = sec0[0x2F];
        if (newSlot != 0) {
            sec0[0x2F] = sec0[0x130 + static_cast<std::size_t>(newSlot) * 8u + 7u];
        } else {
            std::array<bool, 256> used{};
            used[0] = true;
            auto markChain = [&](std::uint8_t head) {
                std::uint8_t s = head;
                while (s != 0 && !used[s]) {
                    used[s] = true;
                    s = sec0[0x130 + static_cast<std::size_t>(s) * 8u + 7u];
                }
            };
            markChain(sec0[0x2E]);
            markChain(sec0[0x30]);
            for (int t = 1; t <= nTracks; ++t) {
                markChain(sec0[0x30 + static_cast<std::size_t>(t)]);
            }
            for (int s = 1; s < 256; ++s) {
                if (!used[static_cast<std::size_t>(s)]) {
                    newSlot = static_cast<std::uint8_t>(s);
                    break;
                }
            }
        }
        if (newSlot == 0) {
            error = "MockNetMd: no free Sector 0 fragment slot available for upload.";
            return false;
        }

        std::uint16_t allocCluster = nextFreeCluster;
        bool allocatedFromPfra = false;
        std::uint8_t prevPfraSlot = 0;
        std::uint8_t pfraSlot = sec0[0x30];
        std::array<bool, 256> pfraSeen{};
        while (pfraSlot != 0 && !pfraSeen[pfraSlot]) {
            pfraSeen[pfraSlot] = true;
            std::uint8_t* f = sec0.data() + 0x130 + static_cast<std::size_t>(pfraSlot) * 8u;
            const auto s = decodeUtocAddr(f);
            const auto e = decodeUtocAddr(f + 4);
            const std::uint16_t firstWhole =
                (s.sector == 0 && s.group == 0) ? s.cluster : static_cast<std::uint16_t>(s.cluster + 1);
            const std::uint16_t lastWhole =
                (e.sector == 31 && e.group == 10)
                    ? e.cluster
                    : (e.cluster > 0 ? static_cast<std::uint16_t>(e.cluster - 1) : 0);
            if (lastWhole >= firstWhole &&
                static_cast<std::uint16_t>(lastWhole - firstWhole + 1) >= clustersNeeded) {
                allocCluster = firstWhole;
                allocatedFromPfra = true;
                const std::uint16_t remStart =
                    static_cast<std::uint16_t>(firstWhole + clustersNeeded);
                if (remStart <= e.cluster) {
                    encodeUtocAddr(f, UtocAddress{remStart, 0, 0});
                } else {
                    const std::uint8_t nextLink = f[7];
                    if (prevPfraSlot == 0) {
                        sec0[0x30] = nextLink;
                    } else {
                        sec0[0x130 + static_cast<std::size_t>(prevPfraSlot) * 8u + 7u] = nextLink;
                    }
                    f[0] = f[1] = f[2] = f[3] = f[4] = f[5] = f[6] = 0;
                    f[7] = sec0[0x2F];
                    sec0[0x2F] = pfraSlot;
                }
                break;
            }
            prevPfraSlot = pfraSlot;
            pfraSlot = f[7];
        }

        if (!allocatedFromPfra && sec0[0x30] != 0) {
            error = "MockNetMd: insufficient free clusters in P-FRA for upload.";
            return false;
        }

        const UtocAddress startAddr{allocCluster, 0, 0};
        const std::size_t lastGroupOffset = (soundGroups - 1u) % 352u;
        const UtocAddress endAddr{
            static_cast<std::uint16_t>(allocCluster + clustersNeeded - 1u),
            static_cast<std::uint8_t>(lastGroupOffset / 11u),
            static_cast<std::uint8_t>(lastGroupOffset % 11u)};
        nextFreeCluster = std::max<std::uint16_t>(
            nextFreeCluster, static_cast<std::uint16_t>(endAddr.cluster + 1u));

        rec.trackIndex = nTracks;
        rec.startAddr = startAddr;
        rec.endAddr = endAddr;

        std::uint8_t* dst = sec0.data() + 0x130 + static_cast<std::size_t>(newSlot) * 8u;
        encodeUtocAddr(dst, startAddr);
        dst[3] = mockUtocModeForTrack(rec.mode);
        encodeUtocAddr(dst + 4, endAddr);
        dst[7] = 0;

        sec0[0x30 + static_cast<std::size_t>(nTracks + 1)] = newSlot;
        sec0[0x1E] = 1;
        sec0[0x1F] = static_cast<std::uint8_t>(nTracks + 1);
        uploadedTracks.push_back(rec);
        return true;
    }

    void syncCacheUtocFromTracks() {
        for (auto& sec : utocCache) {
            sec.fill(0);
        }
        auto& sec0 = utocCache[0];
        sec0[0x1C] = static_cast<std::uint8_t>((kCartaUtocSignature >> 8) & 0xFFu);
        sec0[0x1D] = static_cast<std::uint8_t>(kCartaUtocSignature & 0xFFu);
        sec0[0x1E] = uploadedTracks.empty() ? 0u : 1u;
        const std::size_t nTracks = std::min<std::size_t>(uploadedTracks.size(), 254u);
        sec0[0x1F] = static_cast<std::uint8_t>(nTracks);
        sec0[0x23] = 0x07u;

        initMockEmptyTitleSector(utocCache[1], 0x00u);
        initMockEmptyTitleSector(utocCache[3], 0xFFu);
        auto& ptoc = utocCache[4];
        ptoc[0x0C] = 0xFFu;
        ptoc[0x0D] = 0xCAu;
        ptoc[0x0E] = 0x00u;
        ptoc[0x0F] = 0x11u;
        ptoc[0x18] = 'M';
        ptoc[0x19] = 'I';
        ptoc[0x1A] = 'N';
        ptoc[0x1B] = 'I';
        ptoc[0x20] = 0x09u;
        ptoc[0x21] = 0x7Cu;
        ptoc[0x2C] = 0x00u;
        ptoc[0x2D] = 0x32u;

        std::array<std::uint8_t, 256> links{};
        for (std::uint8_t i = 1; i < 255; ++i) {
            links[i] = static_cast<std::uint8_t>(i + 1);
        }
        links[255] = 0;
        std::uint8_t nextFree = 1;

        std::uint16_t maxEndCluster = 0x0031;
        for (std::size_t i = 0; i < nTracks && nextFree != 0; ++i) {
            const auto& trk = uploadedTracks[i];
            const std::uint8_t slot = nextFree;
            nextFree = links[slot];
            std::uint8_t* f = sec0.data() + 0x130 + static_cast<std::size_t>(slot) * 8u;
            encodeUtocAddr(f, trk.startAddr);
            f[3] = mockUtocModeForTrack(trk.mode);
            encodeUtocAddr(f + 4, trk.endAddr);
            f[7] = 0;
            sec0[0x30 + i + 1] = slot;
            if (trk.endAddr.cluster > maxEndCluster) {
                maxEndCluster = trk.endAddr.cluster;
            }
        }
        if (nextFree != 0 && maxEndCluster < 2280) {
            const std::uint8_t slot = nextFree;
            nextFree = links[slot];
            std::uint8_t* f = sec0.data() + 0x130 + static_cast<std::size_t>(slot) * 8u;
            encodeUtocAddr(f, UtocAddress{static_cast<std::uint16_t>(maxEndCluster + 1), 0, 0});
            f[3] = 0;
            encodeUtocAddr(f + 4, UtocAddress{2280, 31, 10});
            f[7] = 0;
            sec0[0x30 + 0] = slot;
        }
        sec0[0x2F] = nextFree;
        for (std::uint8_t s = nextFree; s != 0; s = links[s]) {
            sec0[0x130 + static_cast<std::size_t>(s) * 8u + 7u] = links[s];
        }
    }

    void resetAll() {
        open = false;
        replyQueue.clear();
        for (auto& slot : patchSlots) {
            slot = PatchSlot{};
        }
        extraMappedMem.clear();
        extraMappedMem[kMockFiqVariantAddr] = 0x00;
        safetyPatchApplied = false;
        killEepromWriteApplied = false;
        usbCodeExecApplied = false;
        spUploadUsedCount = 0;
        tocCommitCount = 0;
        tocPollCount = 0;
        tocPollBusyRemaining = 0;
        sleptMs = 0;
        displayOverride.clear();
        resetDiscAndSession();
    }

    void resetDiscAndSession() {
        inSecureSession = false;
        haveSessionKey = false;
        sessionKey.fill(0);
        haveKek = false;
        contentKek.fill(0);
        awaitingBulk = false;
        bulkBuffer.clear();
        expectedTotalBulkBytes = 0;
        nextFreeCluster = 50;
        discTitleHalf.clear();
        discTitleFull.clear();
        uploadedTracks.clear();
        syncCacheUtocFromTracks();
        committedUtoc = utocCache;
    }

    void eraseDiscOnly() {
        awaitingBulk = false;
        bulkBuffer.clear();
        expectedTotalBulkBytes = 0;
        nextFreeCluster = 50;
        discTitleHalf.clear();
        discTitleFull.clear();
        uploadedTracks.clear();
        syncCacheUtocFromTracks();
    }
};

std::mutex g_hostMutex;
std::string g_lastError;
std::atomic<bool> g_mockEnabled{false};
std::atomic<bool> g_simulateRealGuardOnMock{false};
std::atomic<bool> g_hardwareWriteConfirmed{false};
MockDeviceState g_mock;
bool g_mockInitialized = false;

std::mutex g_progressMutex;
NetMdProgressSnapshot g_progress;
NetMdProgressCallback g_progressCallback;
double g_progressBase = 0.0;
double g_progressSpan = 0.0;
std::int64_t g_uploadSpanStartBytes = 0;
std::int64_t g_uploadSpanWireBytes = 0;
std::int64_t g_uploadSpanExpectedBulkBytes = 0;
std::int64_t g_uploadSpanBulkSent = 0;

void notifyProgressCallback(const NetMdProgressSnapshot& snap, const NetMdProgressCallback& cb) {
    if (cb) {
        cb(snap);
    }
}

void resetUploadSpanTrackingLocked() noexcept {
    g_uploadSpanStartBytes = 0;
    g_uploadSpanWireBytes = 0;
    g_uploadSpanExpectedBulkBytes = 0;
    g_uploadSpanBulkSent = 0;
}

void recordUploadBulkProgress(int transferred) {
    if (transferred <= 0) {
        return;
    }
    NetMdProgressSnapshot snapCopy;
    NetMdProgressCallback cbCopy;
    {
        std::lock_guard<std::mutex> pLock(g_progressMutex);
        if (!g_progress.active || g_progress.bytesTotal <= 0) {
            return;
        }
        g_uploadSpanBulkSent += static_cast<std::int64_t>(transferred);
        if (g_uploadSpanWireBytes > 0 && g_uploadSpanExpectedBulkBytes > 0) {
            const double spanFrac =
                std::clamp(static_cast<double>(g_uploadSpanBulkSent) /
                               static_cast<double>(g_uploadSpanExpectedBulkBytes),
                           0.0, 1.0);
            const std::int64_t spanEnd =
                std::min(g_progress.bytesTotal, g_uploadSpanStartBytes + g_uploadSpanWireBytes);
            const std::int64_t mapped = g_uploadSpanStartBytes +
                                        static_cast<std::int64_t>(std::llround(
                                            spanFrac * static_cast<double>(g_uploadSpanWireBytes)));
            g_progress.bytesDone = std::clamp(mapped, g_uploadSpanStartBytes, spanEnd);
        } else {
            g_progress.bytesDone =
                std::clamp(g_progress.bytesDone + static_cast<std::int64_t>(transferred),
                           std::int64_t{0}, g_progress.bytesTotal);
        }
        if (g_progressSpan > 0.0) {
            const double totalFrac = std::clamp(static_cast<double>(g_progress.bytesDone) /
                                                    static_cast<double>(g_progress.bytesTotal),
                                                0.0, 1.0);
            g_progress.progress = std::clamp(g_progressBase + g_progressSpan * totalFrac, 0.0, 1.0);
        }
        snapCopy = g_progress;
        cbCopy = g_progressCallback;
    }
    notifyProgressCallback(snapCopy, cbCopy);
}

std::string formatUsbDeviceId(libusb_device* dev, const libusb_device_descriptor& desc) {
    char prefix[24];
    std::snprintf(prefix, sizeof(prefix), "usb:%04x:%04x:", desc.idVendor, desc.idProduct);
    std::ostringstream oss;
    oss << prefix;
    std::uint8_t ports[8]{};
    const int nPorts = libusb_get_port_numbers(dev, ports, static_cast<int>(sizeof(ports)));
    if (nPorts > 0) {
        oss << 'p';
        for (int i = 0; i < nPorts; ++i) {
            if (i > 0) {
                oss << '.';
            }
            oss << static_cast<unsigned>(ports[i]);
        }
    } else {
        oss << static_cast<unsigned>(libusb_get_bus_number(dev)) << ':'
            << static_cast<unsigned>(libusb_get_device_address(dev));
    }
    return oss.str();
}

std::string formatLegacyUsbDeviceId(libusb_device* dev, const libusb_device_descriptor& desc) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "usb:%04x:%04x:%u:%u", desc.idVendor, desc.idProduct,
                  static_cast<unsigned>(libusb_get_bus_number(dev)),
                  static_cast<unsigned>(libusb_get_device_address(dev)));
    return std::string(buf);
}

struct UsbOpenHandle {
    bool isMock = false;
    libusb_device_handle* usbHandle = nullptr;
    libusb_context* usbCtx = nullptr;
    int interfaceNumber = 0;
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    bool hadIoError = false;
#if defined(__APPLE__)
    IOPMAssertionID sleepAssertionId = kIOPMNullAssertionID;
#endif
};

std::map<int, UsbOpenHandle> g_handles;
int g_nextHandle = 1;

bool isRecoverableUsbIoError(int rc) noexcept {
    return rc == LIBUSB_ERROR_IO || rc == LIBUSB_ERROR_PIPE || rc == LIBUSB_ERROR_TIMEOUT ||
           rc == LIBUSB_ERROR_NO_DEVICE;
}

bool openAndClaimUsbDeviceLocked(libusb_context* ctx, const std::string& targetId, unsigned vid,
                                 unsigned pid, libusb_device_handle** outHandle,
                                 std::string& outErr) {
    *outHandle = nullptr;
    outErr.clear();

    libusb_device** devs = nullptr;
    const ssize_t cnt = libusb_get_device_list(ctx, &devs);
    libusb_device* exactMatch = nullptr;
    std::vector<libusb_device*> vidPidMatches;
    for (ssize_t i = 0; i < cnt; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(devs[i], &desc) != 0) {
            continue;
        }
        if (desc.idVendor == vid && desc.idProduct == pid) {
            vidPidMatches.push_back(devs[i]);
            if (formatUsbDeviceId(devs[i], desc) == targetId ||
                formatLegacyUsbDeviceId(devs[i], desc) == targetId) {
                exactMatch = devs[i];
                break;
            }
        }
    }

    libusb_device* chosen =
        exactMatch ? exactMatch : (vidPidMatches.size() == 1 ? vidPidMatches.front() : nullptr);
    libusb_device_handle* usbHandle = nullptr;
    int opened = -1;
    if (chosen != nullptr) {
        opened = libusb_open(chosen, &usbHandle);
    }
    if (devs) {
        libusb_free_device_list(devs, 1);
    }

    if (opened != 0 || !usbHandle) {
        if (usbHandle) {
            libusb_close(usbHandle);
        }
        outErr = "Failed to open NetMD USB device " + targetId +
                 (chosen != nullptr && opened != 0 ? (std::string(": ") + libusb_error_name(opened))
                                                   : "");
        return false;
    }

#ifdef LIBUSB_API_VERSION
    libusb_set_auto_detach_kernel_driver(usbHandle, 1);
#endif
    const int claimRc = libusb_claim_interface(usbHandle, 0);
    if (claimRc != 0) {
        libusb_close(usbHandle);
        outErr = std::string("Failed claiming USB interface 0: ") + libusb_error_name(claimRc);
        return false;
    }

    *outHandle = usbHandle;
    return true;
}

void ensureMockInitLocked() {
    if (!g_mockInitialized) {
        g_mock.resetAll();
        g_mockInitialized = true;
    }
}

void setErrorLocked(const std::string& msg) {
    g_lastError = msg;
}

// ============================================================================
// Hardware Write Guard Whitelist
// ============================================================================

/** Permits only non-mutating factory/vendor USB control queries when write confirmation is off. */
bool isReadOnlyControlCommandAllowed(int request, const std::uint8_t* data, int len) {
    if (!data || len < 3) {
        return false;
    }
    if (request == 0xff) {
        // Factory command: data[0]=status (0x00/0x02), data[1..2] = opcode
        if (data[1] == 0x18 && data[2] == 0x01) {
            return true; // auth (NetMD)
        }
        if (data[1] == 0x18 && data[2] == 0x02) {
            return true; // auth (HiMD / MZ-RH1)
        }
        if (data[1] == 0x18 && data[2] == 0x12) {
            return true; // getDeviceCode
        }
        if (data[1] == 0x18 && data[2] == 0x13) {
            return true; // getDeviceVersion
        }
        if (data[1] == 0x18 && data[2] == 0x20 && len >= 11) {
            // changeMemoryState: 0x00=CLOSE, 0x01=READ allowed; WRITE (2/3) blocked
            return data[10] == 0x00 || data[10] == 0x01;
        }
        if (data[1] == 0x18 && data[2] == 0x2b && len >= 11) {
            // HiMD changeMemoryState: 0x00=CLOSE, 0x01=READ allowed; WRITE blocked
            return data[10] == 0x00 || data[10] == 0x01;
        }
        if (data[1] == 0x18 && data[2] == 0x21) {
            return true; // read
        }
        if (data[1] == 0x18 && data[2] == 0x2c) {
            return true; // read (HiMD)
        }
        if (data[1] == 0x18 && data[2] == 0x24) {
            return true; // readMetadataPeripheral
        }
        return false;
    }
    if (request == 0x80) {
        // Vendor AV/C command: allow read-only descriptors, queries, acquire/release
        if (data[1] == 0xFF && data[2] == 0x01) {
            return true; // acquire/release
        }
        if (data[1] == 0x18 && data[2] == 0x08) {
            // changeDescriptorState: allow openRead (0x01) or close (0x00); block openWrite (0x03)
            const std::uint8_t action = data[len - 2];
            return action == 0x00 || action == 0x01;
        }
        if (data[1] == 0x18 && data[2] == 0x06) {
            return true; // read descriptors / disc / track queries
        }
        if (data[1] == 0x18 && data[2] == 0x09) {
            return true; // status queries
        }
        return false;
    }
    return false;
}

// ============================================================================
// Mock Type-S Protocol Responders
// ============================================================================

void pushReply(std::vector<std::uint8_t> reply) {
    g_mock.replyQueue.push_back(std::move(reply));
}

bool handleMockFactoryControlOut(const std::uint8_t* data, int len) {
    if (len < 4 || data[1] != 0x18) {
        setErrorLocked("MockNetMd: malformed factory control OUT packet.");
        return false;
    }
    const std::uint8_t cmd = data[2];
    if (cmd == 0x01) { // auth
        std::vector<std::uint8_t> r(data, data + len);
        r[0] = 0x09;
        r[3] = 0x00;
        pushReply(std::move(r));
        return true;
    }
    if (cmd == 0x12) { // getDeviceCode -> Type-S 'S1.600'
        pushReply({0x09, 0x18, 0x12, 0x00, 0x21, 0x09, 0x00, 0x16});
        return true;
    }
    if (cmd == 0x13) { // getDeviceVersion
        pushReply({0x09, 0x18, 0x13, 0x00, 0x00, 0x16});
        return true;
    }
    if (cmd == 0x20) { // changeMemoryState
        pushReply({0x09, 0x18, 0x20, 0x00});
        return true;
    }
    if (cmd == 0x21 && len >= 10) { // read
        const std::uint8_t memType = data[4];
        const std::uint32_t addr = readLe32(data + 5);
        const std::uint8_t readLen = data[9];
        std::vector<std::uint8_t> r{0x09,    0x18,    0x21,    0x00,    memType, data[5],
                                    data[6], data[7], data[8], readLen, 0x00,    0x00};
        for (int i = 0; i < readLen; ++i) {
            const std::uint32_t a = addr + static_cast<std::uint32_t>(i);
            std::uint8_t byteVal = 0;
            if (a >= kPatchPeripheralBaseNetMd && a < kPatchControlAddr) {
                const std::uint32_t rel = a - kPatchPeripheralBaseNetMd;
                const std::size_t slotIdx = rel / 0x10u;
                const std::uint32_t slotOff = rel % 0x10u;
                if (slotOff < 4) {
                    byteVal = g_mock.patchSlots[slotIdx].control[slotOff];
                } else if (slotOff < 8) {
                    std::uint8_t ab[4];
                    writeLe32(ab, g_mock.patchSlots[slotIdx].address);
                    byteVal = ab[slotOff - 4];
                } else if (slotOff < 12) {
                    byteVal = g_mock.patchSlots[slotIdx].value[slotOff - 8];
                }
            } else {
                const auto it = g_mock.extraMappedMem.find(a);
                if (it != g_mock.extraMappedMem.end()) {
                    byteVal = it->second;
                }
            }
            r.push_back(byteVal);
        }
        // Append 2-byte trailing checksum field (stripped by NetMDFactoryInterface.read)
        r.push_back(0x00);
        r.push_back(0x00);
        pushReply(std::move(r));
        return true;
    }
    if (cmd == 0x22 &&
        len >= 12) { // write: type(4), addr(5..8), len(9), 0000(10..11), data..., crc(2)
        const std::uint8_t memType = data[4];
        const std::uint32_t addr = readLe32(data + 5);
        const std::uint8_t writeLen = data[9];
        if (12 + writeLen > len) {
            setErrorLocked("MockNetMd: truncated factory write packet.");
            return false;
        }
        for (int i = 0; i < writeLen; ++i) {
            const std::uint32_t a = addr + static_cast<std::uint32_t>(i);
            const std::uint8_t b = data[12 + i];
            if (a >= kPatchPeripheralBaseNetMd && a < kPatchControlAddr) {
                const std::uint32_t rel = a - kPatchPeripheralBaseNetMd;
                const std::size_t slotIdx = rel / 0x10u;
                const std::uint32_t slotOff = rel % 0x10u;
                if (slotOff < 4) {
                    g_mock.patchSlots[slotIdx].control[slotOff] = b;
                } else if (slotOff < 8) {
                    const std::uint32_t shift = (slotOff - 4u) * 8u;
                    g_mock.patchSlots[slotIdx].address =
                        (g_mock.patchSlots[slotIdx].address & ~(0xFFu << shift)) |
                        (static_cast<std::uint32_t>(b) << shift);
                } else if (slotOff < 12) {
                    g_mock.patchSlots[slotIdx].value[slotOff - 8] = b;
                }
            } else {
                g_mock.extraMappedMem[a] = b;
            }
        }
        g_mock.refreshPatchFlags();
        pushReply({0x09, 0x18, 0x22, 0x00, memType, data[5], data[6], data[7], data[8], writeLen});
        return true;
    }
    if (cmd == 0x24 && len >= 9) { // readMetadataPeripheral
        const std::uint16_t sector = readLe16(data + 4);
        const std::uint16_t offset = readLe16(data + 6);
        const std::uint8_t readLen = data[8];
        std::vector<std::uint8_t> r{0x09,    0x18,    0x24,    0x00,   data[4],
                                    data[5], data[6], data[7], readLen};
        if (sector < g_mock.utocCache.size() &&
            static_cast<std::size_t>(offset) + readLen <= kUtocSectorBytes) {
            const auto& sec = g_mock.utocCache[sector];
            r.insert(r.end(), sec.begin() + offset, sec.begin() + offset + readLen);
        } else {
            r.resize(r.size() + readLen, 0);
        }
        pushReply(std::move(r));
        return true;
    }
    if (cmd == 0x25 && len >= 9) { // writeMetadataPeripheral
        const std::uint16_t sector = readLe16(data + 4);
        const std::uint16_t offset = readLe16(data + 6);
        const std::uint8_t writeLen = data[8];
        if (9 + writeLen <= len && sector < g_mock.utocCache.size() &&
            static_cast<std::size_t>(offset) + writeLen <= kUtocSectorBytes) {
            std::copy_n(data + 9, writeLen, g_mock.utocCache[sector].begin() + offset);
        }
        pushReply({0x09, 0x18, 0x25, 0x00, data[4], data[5], data[6], data[7], writeLen});
        return true;
    }
    if (cmd == 0x51) { // setDisplayMode
        pushReply({0x09, 0x18, 0x51, 0x00});
        return true;
    }
    if (cmd == 0x52) { // setDisplayOverride
        if (len > 7) {
            std::vector<std::uint8_t> raw;
            for (int i = 7; i < len && data[i] != 0; ++i) {
                raw.push_back(data[i]);
            }
            g_mock.displayOverride = cp932ToUtf8Lossy(raw);
        }
        pushReply({0x09, 0x18, 0x52, 0x00});
        return true;
    }

    std::vector<std::uint8_t> fallback(data, data + len);
    fallback[0] = 0x09;
    if (fallback.size() > 3) {
        fallback[3] = 0x00;
    }
    pushReply(std::move(fallback));
    return true;
}

bool containsLe32Pattern(const std::uint8_t* haystack, int len, std::uint32_t val) noexcept {
    for (int i = 0; i + 4 <= len; ++i) {
        if (readLe32(haystack + i) == val) {
            return true;
        }
    }
    return false;
}

bool handleMockVendorControlOut(const std::uint8_t* data, int len) {
    if (len < 3) {
        setErrorLocked("MockNetMd: malformed vendor control OUT packet.");
        return false;
    }

    // USBCodeExecution: 00 18 d2 ff <code...>
    if (len >= 4 && data[1] == 0x18 && data[2] == 0xd2 && data[3] == 0xff) {
        std::vector<std::uint8_t> r(data, data + len);
        r[0] = 0x08; // Not Implemented status expected by USBCodeExecution.execute!
        if (g_mock.usbCodeExecApplied && len > 4) {
            static constexpr std::uint8_t kProbePrefix[12] = {0x01, 0x00, 0xa0, 0xe3, 0x00, 0x00,
                                                              0xcf, 0xe5, 0x1e, 0xff, 0x2f, 0xe1};
            if (len >= 17 && std::memcmp(data + 4, kProbePrefix, 12) == 0) {
                r.back() = 0x01; // patch active probe
            } else if (containsLe32Pattern(data + 4, len - 4, kMockForceTocEditInFirmwareAddr)) {
                g_mock.committedUtoc = g_mock.utocCache;
                g_mock.tocCommitCount += 1;
                g_mock.tocPollBusyRemaining = 2;
            } else if (len == 4 + 36 && readLe32(data + 4 + 0x00) == 0xe59f0014u &&
                       readLe32(data + 4 + 0x18) == 0xe12fff1eu) {
                g_mock.tocPollCount += 1;
                if (g_mock.tocPollBusyRemaining >= 2) {
                    writeLe32(r.data() + 4 + 0x1c, 0x20u);
                    writeLe32(r.data() + 4 + 0x20, 0x00u);
                    --g_mock.tocPollBusyRemaining;
                } else if (g_mock.tocPollBusyRemaining == 1) {
                    writeLe32(r.data() + 4 + 0x1c, 0x00u);
                    writeLe32(r.data() + 4 + 0x20, 0x01u);
                    --g_mock.tocPollBusyRemaining;
                } else {
                    writeLe32(r.data() + 4 + 0x1c, 0x00u);
                    writeLe32(r.data() + 4 + 0x20, 0x00u);
                }
            }
        }
        pushReply(std::move(r));
        return true;
    }

    // acquire / release
    if (data[1] == 0xff && data[2] == 0x01) {
        std::vector<std::uint8_t> r(data, data + len);
        r[0] = 0x09;
        pushReply(std::move(r));
        return true;
    }

    // changeDescriptorState (18 08)
    if (data[1] == 0x18 && data[2] == 0x08) {
        std::vector<std::uint8_t> r(data, data + len);
        r[0] = 0x09;
        pushReply(std::move(r));
        return true;
    }

    // eraseDisc (18 40)
    if (data[1] == 0x18 && data[2] == 0x40) {
        g_mock.eraseDiscOnly();
        pushReply({0x09, 0x18, 0x40, 0x00, 0x00, 0x00});
        return true;
    }

    // operatingStatus / status queries (18 09 ...)
    if (data[1] == 0x18 && data[2] == 0x09 && len >= 6) {
        if (len >= 10 && data[3] == 0x00 && data[4] == 0xFF) {
            // _getDiscSubunitIdentifier: 1809 00 1000 %?%? %?%? %w %b %b %b %b %w %*
            pushReply({
                0x0C, 0x18, 0x09, 0x00, 0x10, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x1A, // descriptorLength
                0x00,                               // generationID
                0x02,                               // sizeOfListID
                0x02,                               // sizeOfObjectID
                0x02,                               // sizeOfObjectPosition
                0x00, 0x01,                         // amtOfRootObjectLists = 1
                0x10, 0x00,                         // rootObject[0]
                0x00, 0x0E,                         // subunitDependentLength
                0x00, 0x0A,                         // subunitFieldsLength
                0x00,                               // attributes
                0x10,                               // discSubunitVersion
                0x01,                               // amtSupportedMediaTypes = 1
                0x03, 0x01,                         // supportedMediaType = 0x0301 (769)
                0x20,       // implentationProfileID = NetMDLevel.level1 (32)
                0x00,       // mediaTypeAttributes
                0x00, 0x02, // typeDepLength
                0x00,       // mdAudioVersion
                0x00,       // supportsMDClip
                0x00, 0x00  // manufacturerDepLength = 0
            });
            return true;
        }
        if (len >= 12 && data[3] == 0x80 && data[4] == 0x01 && data[5] == 0x02 && data[7] == 0x88 &&
            data[8] == 0x00) {
            // getStatus
            pushReply({0x0C, 0x18, 0x09, 0x80, 0x01, 0x02, 0x30, 0x88, 0x00, 0x00, 0x30,
                       0x88, 0x04, 0x00, 0x10, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x09,
                       0x00, 0x04, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00});
            return true;
        }
        if (len >= 18 && data[3] == 0x80 && data[4] == 0x01 && data[5] == 0x03 && data[7] == 0x88 &&
            (data[8] == 0x02 || data[8] == 0x01)) {
            // getPlaybackStatus2 / getFullOperatingStatus: 0xFFFF = discBlank, 0xC5FF = ready
            // (50687)
            const std::uint8_t stHi = g_mock.uploadedTracks.empty() ? 0xFF : 0xC5;
            const std::uint8_t stLo = 0xFF;
            pushReply({0x0C, 0x18, 0x09, 0x80, 0x01, 0x03, 0x30,     data[7],  data[8],
                       0x00, 0x30, 0x88, 0x05, 0x00, 0x30, data[15], data[16], 0x00,
                       0x10, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00,     0x06,     0x88,
                       0x06, 0x00, 0x03, stHi, stLo, 0x00});
            return true;
        }
        if (len >= 16 && data[3] == 0x80 && data[4] == 0x01 && data[5] == 0x04) {
            // getPosition: empty / ready
            pushReply({0x0C, 0x18, 0x09, 0x80, 0x01, 0x04, 0x30, 0x88, 0x02, 0x00, 0x30,
                       0x88, 0x05, 0x00, 0x30, 0x00, 0x03, 0x00, 0x30, 0x00, 0x02, 0x00,
                       0x10, 0x00, 0x00, 0x0D, 0x00, 0x00, 0x00, 0x0B, 0x00, 0x02, 0x00,
                       0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
            return true;
        }
    }

    // descriptor queries (18 06 ...)
    if (data[1] == 0x18 && data[2] == 0x06 && len >= 7) {
        // getDiscFlags: 1806 01101000 ff00 0001000b
        if (len >= 13 && data[3] == 0x01 && data[4] == 0x10 && data[5] == 0x10 && data[6] == 0x00) {
            pushReply({0x0C, 0x18, 0x06, 0x01, 0x10, 0x10, 0x00, 0x10, 0x00, 0x00, 0x01, 0x00, 0x0B,
                       0x10});
            return true;
        }
        // getTrackCount: 1806 02101001 3000 1000 ff00 00000000
        if (len >= 13 && data[3] == 0x02 && data[4] == 0x10 && data[5] == 0x10 && data[6] == 0x01) {
            const auto count =
                static_cast<std::uint8_t>(std::min<std::size_t>(255, g_mock.uploadedTracks.size()));
            pushReply({0x0C, 0x18, 0x06, 0x02, 0x10, 0x10, 0x01, 0x30, 0x00, 0x10, 0x00, 0x10, 0x00,
                       0x00, 0x09, 0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x00, 0x02, 0x00, count});
            return true;
        }
        // getDiscCapacity: 1806 02101000 3080 0300 ff00 00000000
        if (len >= 13 && data[3] == 0x02 && data[4] == 0x10 && data[5] == 0x10 && data[6] == 0x00) {
            int usedSeconds = 0;
            for (const auto& t : g_mock.uploadedTracks) {
                usedSeconds += (t.frameCount * 512) / 44100;
            }
            const int totalSeconds = 80 * 60;
            const int leftSeconds = std::max(0, totalSeconds - usedSeconds);
            auto pushBcdTime = [](std::vector<std::uint8_t>& v, int secs) {
                const int hh = secs / 3600;
                const int mm = (secs % 3600) / 60;
                const int ss = secs % 60;
                v.push_back(0x00);
                v.push_back(0x05);
                v.push_back(0x00);
                v.push_back(intToBcdByte(hh));
                v.push_back(intToBcdByte(mm));
                v.push_back(intToBcdByte(ss));
                v.push_back(0x00);
            };
            std::vector<std::uint8_t> r{0x0C, 0x18, 0x06, 0x02, 0x10, 0x10, 0x00, 0x30, 0x80,
                                        0x03, 0x00, 0x10, 0x00, 0x00, 0x1D, 0x00, 0x00, 0x00,
                                        0x1B, 0x00, 0x03, 0x00, 0x17, 0x80, 0x00};
            pushBcdTime(r, usedSeconds);
            pushBcdTime(r, totalSeconds);
            pushBcdTime(r, leftSeconds);
            pushReply(std::move(r));
            return true;
        }
        // _getDiscTitle: 1806 02201801 00%b 3000 0a00 ff00 %w%w
        if (len >= 13 && data[3] == 0x02 && data[4] == 0x20 && data[5] == 0x18 && data[6] == 0x01) {
            const bool isFull = data[8] != 0;
            const std::string raw =
                utf8ToCp932Lossy(isFull ? g_mock.discTitleFull : g_mock.discTitleHalf);
            const std::uint16_t done = len >= 19 ? readBe16(data + 17) : 0;
            const std::size_t total = raw.size();
            const std::size_t start = std::min<std::size_t>(done, total);
            const std::size_t chunkBytes = std::min<std::size_t>(128, total - start);
            std::vector<std::uint8_t> r{0x0C,    0x18, 0x06, 0x02, 0x20, 0x18, 0x01, 0x00,
                                        data[8], 0x30, 0x00, 0x0A, 0x00, 0x10, 0x00};
            if (done == 0) {
                const auto chunkField = static_cast<std::uint16_t>(6 + chunkBytes);
                r.push_back(static_cast<std::uint8_t>((chunkField >> 8) & 0xFF));
                r.push_back(static_cast<std::uint8_t>(chunkField & 0xFF));
                r.push_back(0x00);
                r.push_back(0x00);
                r.push_back(0x00);
                r.push_back(static_cast<std::uint8_t>(chunkField & 0xFF));
                r.push_back(0x00);
                r.push_back(0x0A);
                r.push_back(static_cast<std::uint8_t>((total >> 8) & 0xFF));
                r.push_back(static_cast<std::uint8_t>(total & 0xFF));
            } else {
                r.push_back(static_cast<std::uint8_t>((chunkBytes >> 8) & 0xFF));
                r.push_back(static_cast<std::uint8_t>(chunkBytes & 0xFF));
                r.push_back(0x00);
                r.push_back(0x00);
            }
            for (std::size_t i = 0; i < chunkBytes; ++i) {
                r.push_back(static_cast<std::uint8_t>(raw[start + i]));
            }
            pushReply(std::move(r));
            return true;
        }
        // getTrackTitle: 1806 022018%b %w 3000 0a00 ff00 00000000
        if (len >= 15 && data[3] == 0x02 && data[4] == 0x20 && data[5] == 0x18 &&
            (data[6] == 0x02 || data[6] == 0x03)) {
            const std::uint16_t trk = readBe16(data + 7);
            if (trk >= g_mock.uploadedTracks.size()) {
                std::vector<std::uint8_t> r(data, data + len);
                r[0] = 0x0A; // NetMDRejected
                pushReply(std::move(r));
                return true;
            }
            const bool isFull = (data[6] == 0x03);
            const auto& rec = g_mock.uploadedTracks[trk];
            const std::string raw =
                utf8ToCp932Lossy(isFull ? rec.fullWidthTitle : rec.halfWidthTitle);
            const auto sz = raw.size();
            std::vector<std::uint8_t> r{
                0x0C,
                0x18,
                0x06,
                0x02,
                0x20,
                0x18,
                data[6],
                data[7],
                data[8],
                0x30,
                0x00,
                0x0A,
                0x00,
                0x10,
                0x00,
                0x00,
                static_cast<std::uint8_t>(std::min<std::size_t>(255, 6 + sz)),
                0x00,
                0x00,
                0x00,
                static_cast<std::uint8_t>(std::min<std::size_t>(255, 4 + sz)),
                0x00,
                0x0A,
                static_cast<std::uint8_t>((sz >> 8) & 0xFF),
                static_cast<std::uint8_t>(sz & 0xFF)};
            for (char ch : raw) {
                r.push_back(static_cast<std::uint8_t>(ch));
            }
            pushReply(std::move(r));
            return true;
        }
        // getTrackFlags: 1806 01201001 %w ff00 00010008
        if (len >= 13 && data[3] == 0x01 && data[4] == 0x20 && data[5] == 0x10 && data[6] == 0x01) {
            pushReply({0x0C, 0x18, 0x06, 0x01, 0x20, 0x10, 0x01, data[7], data[8], 0x10, 0x00, 0x00,
                       0x01, 0x00, 0x08, 0x00});
            return true;
        }
        // _getTrackInfo: 1806 02201001 %w %w %w ff00 00000000
        if (len >= 15 && data[3] == 0x02 && data[4] == 0x20 && data[5] == 0x10 && data[6] == 0x01) {
            const std::uint16_t trk = readBe16(data + 7);
            const std::uint16_t p1 = readBe16(data + 9);
            const std::uint16_t p2 = readBe16(data + 11);
            std::vector<std::uint8_t> r{
                0x0C,     0x18,     0x06,     0x02, 0x20, 0x10, 0x01, data[7], data[8], data[9],
                data[10], data[11], data[12], 0x10, 0x00, 0x00, 0x0C, 0x00,    0x00};
            const NetMdMockUploadedTrack* found =
                trk < g_mock.uploadedTracks.size() ? &g_mock.uploadedTracks[trk] : nullptr;
            if (p1 == 0x3000 && p2 == 0x0100) {
                const int secs = found ? (found->frameCount * 512) / 44100 : 0;
                r.push_back(0x00);
                r.push_back(0x0A);
                r.push_back(0x00);
                r.push_back(0x01);
                r.push_back(0x00);
                r.push_back(0x06);
                r.push_back(0x00);
                r.push_back(0x00);
                r.push_back(intToBcdByte(secs / 3600));
                r.push_back(intToBcdByte((secs % 3600) / 60));
                r.push_back(intToBcdByte(secs % 60));
                r.push_back(0x00);
            } else if (p1 == 0x3080 && p2 == 0x0700) {
                std::uint8_t enc = 0x90; // Encoding.sp (144)
                std::uint8_t ch = 0x00;  // stereo
                if (found) {
                    if (found->mode == "lp2") {
                        enc = 0x92; // 146
                    } else if (found->mode == "lp4") {
                        enc = 0x93; // 147
                    } else if (found->mode == "sp-mono") {
                        ch = 0x01;
                    }
                }
                r.push_back(0x00);
                r.push_back(0x08);
                r.push_back(0x80);
                r.push_back(0x07);
                r.push_back(0x00);
                r.push_back(0x04);
                r.push_back(0x01);
                r.push_back(0x10);
                r.push_back(enc);
                r.push_back(ch);
            }
            pushReply(std::move(r));
            return true;
        }
    }

    // setTitle (18 07 ...)
    if (data[1] == 0x18 && data[2] == 0x07 && len >= 21) {
        const std::uint16_t subObj = readBe16(data + 5);
        std::vector<std::uint8_t> raw;
        for (int i = 21; i < len; ++i) {
            if (data[i] != 0) {
                raw.push_back(data[i]);
            }
        }
        const std::string titleUtf8 = cp932ToUtf8Lossy(raw);
        const std::size_t sec4Idx = g_mock.mockSector4Index();
        if (subObj == 0x1801) {
            if (data[8] != 0) {
                g_mock.discTitleFull = titleUtf8;
                MockDeviceState::writeMockTitleToSector(g_mock.utocCache[sec4Idx], 0, raw);
            } else {
                g_mock.discTitleHalf = titleUtf8;
                MockDeviceState::writeMockTitleToSector(g_mock.utocCache[1], 0, raw);
            }
        } else {
            const bool isFull = (subObj == 0x1803);
            const std::uint16_t slot = readBe16(data + 7);
            if (slot < g_mock.uploadedTracks.size()) {
                if (isFull) {
                    g_mock.uploadedTracks[slot].fullWidthTitle = titleUtf8;
                    MockDeviceState::writeMockTitleToSector(
                        g_mock.utocCache[sec4Idx], static_cast<int>(slot) + 1, raw);
                } else {
                    g_mock.uploadedTracks[slot].halfWidthTitle = titleUtf8;
                    MockDeviceState::writeMockTitleToSector(
                        g_mock.utocCache[1], static_cast<int>(slot) + 1, raw);
                }
            }
        }
        const auto& fwSec = g_mock.utocCache[sec4Idx];
        bool anyFw = fwSec[0x130] != 0;
        for (std::size_t i = 1; i < 256 && !anyFw; ++i) {
            anyFw = fwSec[0x30 + i] != 0;
        }
        g_mock.utocCache[sec4Idx][0x2B] = anyFw ? 0x02u : 0xFFu;
        if (anyFw) {
            g_mock.utocCache[0][0x23] |= 0x10u;
        } else {
            g_mock.utocCache[0][0x23] = static_cast<std::uint8_t>(g_mock.utocCache[0][0x23] & ~0x10u);
        }
        std::vector<std::uint8_t> r(data, data + 21);
        r[0] = 0x09;
        pushReply(std::move(r));
        return true;
    }

    // NetMD 18 00 08 00 46 f0 03 01 03 <opcode> ...
    if (len >= 12 && data[1] == 0x18 && data[2] == 0x00 && data[3] == 0x08 && data[4] == 0x00 &&
        data[5] == 0x46 && data[6] == 0xf0 && data[7] == 0x03 && data[8] == 0x01 &&
        data[9] == 0x03) {
        const std::uint8_t op = data[10];
        if (op == 0x80) { // enterSecureSession
            g_mock.inSecureSession = true;
            pushReply({0x09, 0x18, 0x00, 0x08, 0x00, 0x46, 0xf0, 0x03, 0x01, 0x03, 0x80, 0x00});
            return true;
        }
        if (op == 0x81) { // leaveSecureSession
            g_mock.inSecureSession = false;
            pushReply({0x09, 0x18, 0x00, 0x08, 0x00, 0x46, 0xf0, 0x03, 0x01, 0x03, 0x81, 0x00});
            return true;
        }
        if (op == 0x2b) { // disableNewTrackProtection
            pushReply({0x09, 0x18, 0x00, 0x08, 0x00, 0x46, 0xf0, 0x03, 0x01, 0x03, 0x2b, 0x00, 0x00,
                       0x00});
            return true;
        }
        if (op == 0x11) { // getLeafID
            pushReply({0x09, 0x18, 0x00, 0x08, 0x00, 0x46, 0xf0, 0x03, 0x01, 0x03,
                       0x11, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08});
            return true;
        }
        if (op == 0x12) { // sendKeyData
            pushReply({0x09, 0x18, 0x00, 0x08, 0x00, 0x46, 0xf0, 0x03, 0x01, 0x03, 0x12, 0x01, 0x00,
                       0x00, 0x00, 0x00, 0x00, 0x00});
            return true;
        }
        if (op == 0x20 && len >= 23) {
            // sessionKeyExchange: 00 1800 080046 f0030103 20 ff 000000 <hostnonce:8>
            std::array<std::uint8_t, 8> devNonce = {0x31, 0x41, 0x59, 0x26, 0x53, 0x58, 0x97, 0x93};
            std::array<std::uint8_t, 16> combined{};
            std::copy_n(data + 15, 8, combined.begin());
            std::copy_n(devNonce.begin(), 8, combined.begin() + 8);
            if (!retailMac8(kEkbOpenSourceRootKey, combined.data(), combined.size(),
                            g_mock.sessionKey)) {
                setErrorLocked("MockNetMd: retailMac8 failed during sessionKeyExchange.");
                return false;
            }
            g_mock.haveSessionKey = true;
            std::vector<std::uint8_t> r{0x09, 0x18, 0x00, 0x08, 0x00, 0x46, 0xf0, 0x03,
                                        0x01, 0x03, 0x20, 0x00, 0x00, 0x00, 0x00};
            r.insert(r.end(), devNonce.begin(), devNonce.end());
            pushReply(std::move(r));
            return true;
        }
        if (op == 0x21) { // sessionKeyForget
            g_mock.haveSessionKey = false;
            g_mock.sessionKey.fill(0);
            pushReply({0x09, 0x18, 0x00, 0x08, 0x00, 0x46, 0xf0, 0x03, 0x01, 0x03, 0x21, 0x00, 0x00,
                       0x00, 0x00});
            return true;
        }
        if (op == 0x22 && len >= 46) { // setupDownload
            std::array<std::uint8_t, 32> decrypted{};
            std::uint8_t zeroIv[8] = {0};
            if (!g_mock.haveSessionKey ||
                carta_netmd_host_des_crypt(0, 0, 1, g_mock.sessionKey.data(), 8, zeroIv, data + 14,
                                           32, decrypted.data()) < 0) {
                setErrorLocked("MockNetMd: setupDownload decrypt failed.");
                return false;
            }
            std::copy_n(decrypted.begin() + 24, 8, g_mock.contentKek.begin());
            g_mock.haveKek = true;
            pushReply({0x09, 0x18, 0x00, 0x08, 0x00, 0x46, 0xf0, 0x03, 0x01, 0x03, 0x22, 0x00, 0x00,
                       0x00});
            return true;
        }
        if (op == 0x28 && len >= 30) { // sendTrack setup
            g_mock.pendingWireFormat = data[20];
            g_mock.pendingDiscFormat = data[21];
            g_mock.pendingFrameCount = static_cast<int>(readBe32(data + 22));
            g_mock.expectedTotalBulkBytes = readBe32(data + 26);
            g_mock.bulkBuffer.clear();
            g_mock.awaitingBulk = true;
            pushReply({0x09, 0x18, 0x00, 0x08, 0x00, 0x46, 0xf0, 0x03, 0x01, 0x03,     0x28,
                       0x00, 0x00, 0x01, 0x00, 0x10, 0x01, 0x00, 0x00, 0x00, data[20], data[21]});
            return true;
        }
        if (op == 0x48 && len >= 17) { // commitTrack
            pushReply({0x09, 0x18, 0x00, 0x08, 0x00, 0x46, 0xf0, 0x03, 0x01, 0x03, 0x48, 0x00, 0x00,
                       0x10, 0x01, data[15], data[16]});
            return true;
        }
        if (op == 0x2a) { // terminate
            pushReply(
                {0x09, 0x18, 0x00, 0x08, 0x00, 0x46, 0xf0, 0x03, 0x01, 0x03, 0x2a, 0x00, 0x00});
            return true;
        }
    }

    std::vector<std::uint8_t> echo(data, data + len);
    echo[0] = 0x09;
    pushReply(std::move(echo));
    return true;
}

bool finalizeMockBulkUpload() {
    if (g_mock.bulkBuffer.size() < 24 || !g_mock.haveKek || !g_mock.haveSessionKey) {
        setErrorLocked("MockNetMd: bulk upload incomplete or missing keys.");
        return false;
    }
    const std::uint32_t pktSize = readBe32(g_mock.bulkBuffer.data() + 4);
    if (24u + pktSize > g_mock.bulkBuffer.size()) {
        setErrorLocked("MockNetMd: bulk payload truncated.");
        return false;
    }
    const std::uint8_t* wireKey = g_mock.bulkBuffer.data() + 8;
    const std::uint8_t* iv = g_mock.bulkBuffer.data() + 16;
    const std::uint8_t* cipher = g_mock.bulkBuffer.data() + 24;

    // Sender did key = DES_ECB_decrypt(rawKey, kek); recover rawKey = DES_ECB_encrypt(wireKey,
    // kek).
    std::array<std::uint8_t, 8> rawKey{};
    if (carta_netmd_host_des_crypt(0, 1, 0, g_mock.contentKek.data(), 8, nullptr, wireKey, 8,
                                   rawKey.data()) < 0) {
        setErrorLocked("MockNetMd: failed to unwrap bulk packet DES key.");
        return false;
    }

    std::vector<std::uint8_t> decrypted(pktSize);
    if (pktSize > 0 &&
        carta_netmd_host_des_crypt(0, 0, 1, rawKey.data(), 8, iv, cipher, static_cast<int>(pktSize),
                                   decrypted.data()) < 0) {
        setErrorLocked("MockNetMd: failed to decrypt bulk audio payload.");
        return false;
    }

    std::string modeStr = "lp2";
    std::vector<std::uint8_t> finalAudio;
    const auto* spPatch = g_mock.findActivePatch(kMockSpUploadTrackTypeAddr);
    if (g_mock.pendingWireFormat == 144 && spPatch != nullptr) {
        g_mock.spUploadUsedCount += 1;
        modeStr = (spPatch->value[1] == 4) ? "sp-mono" : "sp-stereo";
        const std::size_t expectedSpBytes =
            static_cast<std::size_t>(g_mock.pendingFrameCount) * 212u;
        // Strip 100-byte DRAM alignment padding appended after each 2332-byte slice in SPUpload.
        constexpr std::size_t kDataSlice = 2332;
        constexpr std::size_t kPadSlice = 100;
        constexpr std::size_t kStride = kDataSlice + kPadSlice;
        std::size_t off = 0;
        while (off < decrypted.size() && finalAudio.size() < expectedSpBytes) {
            const std::size_t remain = decrypted.size() - off;
            const std::size_t take = std::min<std::size_t>(
                kDataSlice, std::min<std::size_t>(remain, expectedSpBytes - finalAudio.size()));
            finalAudio.insert(finalAudio.end(),
                              decrypted.begin() + static_cast<std::ptrdiff_t>(off),
                              decrypted.begin() + static_cast<std::ptrdiff_t>(off + take));
            off += kStride;
        }
        if (finalAudio.size() > expectedSpBytes) {
            finalAudio.resize(expectedSpBytes);
        }
    } else if (g_mock.pendingWireFormat == 148) {
        modeStr = "lp2";
        const std::size_t expectedBytes = static_cast<std::size_t>(g_mock.pendingFrameCount) * 192u;
        finalAudio.assign(decrypted.begin(),
                          decrypted.begin() + static_cast<std::ptrdiff_t>(
                                                  std::min(decrypted.size(), expectedBytes)));
    } else if (g_mock.pendingWireFormat == 168) {
        modeStr = "lp4";
        const std::size_t expectedBytes = static_cast<std::size_t>(g_mock.pendingFrameCount) * 96u;
        finalAudio.assign(decrypted.begin(),
                          decrypted.begin() + static_cast<std::ptrdiff_t>(
                                                  std::min(decrypted.size(), expectedBytes)));
    } else {
        finalAudio = std::move(decrypted);
    }

    std::size_t soundGroups = 1;
    if (modeStr == "sp-stereo" || modeStr == "sp-mono") {
        soundGroups = std::max<std::size_t>(1, finalAudio.size() / 212u);
    } else if (modeStr == "lp2" || modeStr == "lp4") {
        soundGroups = std::max<std::size_t>(1, finalAudio.size() / 192u);
    }

    NetMdMockUploadedTrack rec;
    rec.mode = modeStr;
    rec.wireFormat = g_mock.pendingWireFormat;
    rec.discFormat = g_mock.pendingDiscFormat;
    rec.frameCount = g_mock.pendingFrameCount;
    rec.decryptedAudioBytes = std::move(finalAudio);
    std::string mockAllocErr;
    if (!g_mock.appendMockTrackToUtocCache(rec, soundGroups, mockAllocErr)) {
        g_mock.awaitingBulk = false;
        setErrorLocked(mockAllocErr);
        return false;
    }
    const int trackIndex = rec.trackIndex;
    g_mock.awaitingBulk = false;

    // Build post-bulk reply expected by netmd-interface.js:1618-1623:
    // 09 1800 080046 f0030103 28 00 000100 1001 %w(trk) 00 <10B>? <32B DES-CBC>
    std::array<std::uint8_t, 32> replyPlain{};
    std::array<std::uint8_t, 32> replyCipher{};
    char plainStr[33];
    std::snprintf(plainStr, sizeof(plainStr), "UUID%04d0000CCIDCARTANETMD%06d", trackIndex % 10000,
                  trackIndex % 1000000);
    std::memcpy(replyPlain.data(), plainStr,
                32); // ends with ASCII digit >= 0x30 so CryptoJS Pkcs7 never unpads
    std::uint8_t zeroIv[8] = {0};
    if (carta_netmd_host_des_crypt(0, 1, 1, g_mock.sessionKey.data(), 8, zeroIv, replyPlain.data(),
                                   32, replyCipher.data()) < 0) {
        setErrorLocked("MockNetMd: failed to encrypt post-bulk track reply.");
        return false;
    }

    std::vector<std::uint8_t> postReply{0x09,
                                        0x18,
                                        0x00,
                                        0x08,
                                        0x00,
                                        0x46,
                                        0xf0,
                                        0x03,
                                        0x01,
                                        0x03,
                                        0x28,
                                        0x00,
                                        0x00,
                                        0x01,
                                        0x00,
                                        0x10,
                                        0x01,
                                        static_cast<std::uint8_t>((trackIndex >> 8) & 0xFF),
                                        static_cast<std::uint8_t>(trackIndex & 0xFF),
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00,
                                        0x00};
    postReply.insert(postReply.end(), replyCipher.begin(), replyCipher.end());
    pushReply(std::move(postReply));
    return true;
}

} // namespace

// ============================================================================
// C++ Mock, Guard, & Progress Control API
// ============================================================================

void setMockNetMdEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    ensureMockInitLocked();
    g_mockEnabled.store(enabled);
}

bool isMockNetMdEnabled() noexcept {
    return g_mockEnabled.load();
}

void resetMockNetMdState() {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    g_mock.resetAll();
    g_mockInitialized = true;
}

void seedMockNetMdUtoc(const std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5>& sectors) {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    g_mock.resetAll();
    g_mockInitialized = true;
    g_mock.utocCache = sectors;
    g_mock.committedUtoc = sectors;
    g_mock.syncMockTracksFromUtocCache();
}

NetMdMockSnapshot getMockNetMdSnapshot() {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    ensureMockInitLocked();
    NetMdMockSnapshot snap;
    snap.enabled = g_mockEnabled.load();
    snap.open = g_mock.open;
    snap.safetyPatchApplied = g_mock.safetyPatchApplied;
    snap.killEepromWriteApplied = g_mock.killEepromWriteApplied;
    snap.usbCodeExecApplied = g_mock.usbCodeExecApplied;
    snap.spUploadUsedCount = g_mock.spUploadUsedCount;
    snap.tocCommitCount = g_mock.tocCommitCount;
    snap.tocPollCount = g_mock.tocPollCount;
    snap.sleptMs = g_mock.sleptMs;
    snap.displayOverride = g_mock.displayOverride;
    snap.uploadedTracks = g_mock.uploadedTracks;
    snap.utocCache = g_mock.utocCache;
    snap.committedUtoc = g_mock.committedUtoc;
    return snap;
}

void setSimulateRealHardwareGuardOnMock(bool enabled) {
    g_simulateRealGuardOnMock.store(enabled);
}

bool isSimulateRealHardwareGuardOnMock() noexcept {
    return g_simulateRealGuardOnMock.load();
}

void setHardwareWriteConfirmed(bool confirmed) {
    g_hardwareWriteConfirmed.store(confirmed);
}

bool isHardwareWriteConfirmed() noexcept {
    return g_hardwareWriteConfirmed.load();
}

ScopedHardwareWriteConfirmation::ScopedHardwareWriteConfirmation(bool confirm)
    : previous_(g_hardwareWriteConfirmed.exchange(confirm)) {
}

ScopedHardwareWriteConfirmation::~ScopedHardwareWriteConfirmation() {
    g_hardwareWriteConfirmed.store(previous_);
}

bool isNetMdMockDeviceId(const std::string& deviceId) noexcept {
    return deviceId == kMockDeviceId;
}

void setNetMdProgress(const std::string& operation, const std::string& phase, double progress,
                      std::int64_t bytesDone, std::int64_t bytesTotal) {
    NetMdProgressSnapshot snapCopy;
    NetMdProgressCallback cbCopy;
    {
        std::lock_guard<std::mutex> lock(g_progressMutex);
        g_progress.active = true;
        g_progress.operation = operation;
        g_progress.phase = phase;
        g_progress.progress = std::clamp(progress, 0.0, 1.0);
        g_progress.bytesTotal = std::max<std::int64_t>(0, bytesTotal);
        g_progress.bytesDone = g_progress.bytesTotal > 0
                                   ? std::clamp(bytesDone, std::int64_t{0}, g_progress.bytesTotal)
                                   : std::max<std::int64_t>(0, bytesDone);
        g_progressBase = g_progress.progress;
        g_progressSpan = 0.0;
        resetUploadSpanTrackingLocked();
        snapCopy = g_progress;
        cbCopy = g_progressCallback;
    }
    notifyProgressCallback(snapCopy, cbCopy);
}

void setNetMdProgressWindow(const std::string& operation, const std::string& phase,
                            double baseProgress, double spanProgress, std::int64_t bytesDone,
                            std::int64_t bytesTotal) {
    NetMdProgressSnapshot snapCopy;
    NetMdProgressCallback cbCopy;
    {
        std::lock_guard<std::mutex> lock(g_progressMutex);
        g_progress.active = true;
        g_progress.operation = operation;
        g_progress.phase = phase;
        g_progressBase = std::clamp(baseProgress, 0.0, 1.0);
        g_progressSpan = std::clamp(spanProgress, 0.0, 1.0 - g_progressBase);
        g_progress.bytesTotal = std::max<std::int64_t>(0, bytesTotal);
        g_progress.bytesDone = g_progress.bytesTotal > 0
                                   ? std::clamp(bytesDone, std::int64_t{0}, g_progress.bytesTotal)
                                   : std::max<std::int64_t>(0, bytesDone);
        resetUploadSpanTrackingLocked();
        if (g_progress.bytesTotal > 0) {
            const double frac = std::clamp(static_cast<double>(g_progress.bytesDone) /
                                               static_cast<double>(g_progress.bytesTotal),
                                           0.0, 1.0);
            g_progress.progress = std::clamp(g_progressBase + g_progressSpan * frac, 0.0, 1.0);
        } else {
            g_progress.progress = g_progressBase;
        }
        snapCopy = g_progress;
        cbCopy = g_progressCallback;
    }
    notifyProgressCallback(snapCopy, cbCopy);
}

void setNetMdUploadSpanProgressWindow(const std::string& operation, const std::string& phase,
                                      double baseProgress, double spanProgress,
                                      std::int64_t spanStartWireBytes, std::int64_t spanWireBytes,
                                      std::int64_t expectedBulkBytes, std::int64_t totalWireBytes) {
    NetMdProgressSnapshot snapCopy;
    NetMdProgressCallback cbCopy;
    {
        std::lock_guard<std::mutex> lock(g_progressMutex);
        g_progress.active = true;
        g_progress.operation = operation;
        g_progress.phase = phase;
        g_progressBase = std::clamp(baseProgress, 0.0, 1.0);
        g_progressSpan = std::clamp(spanProgress, 0.0, 1.0 - g_progressBase);
        g_progress.bytesTotal = std::max<std::int64_t>(1, totalWireBytes);
        g_uploadSpanStartBytes =
            std::clamp(spanStartWireBytes, std::int64_t{0}, g_progress.bytesTotal);
        g_uploadSpanWireBytes = std::max<std::int64_t>(0, spanWireBytes);
        g_uploadSpanExpectedBulkBytes = std::max<std::int64_t>(1, expectedBulkBytes);
        g_uploadSpanBulkSent = 0;
        g_progress.bytesDone = g_uploadSpanStartBytes;
        const double frac = std::clamp(static_cast<double>(g_progress.bytesDone) /
                                           static_cast<double>(g_progress.bytesTotal),
                                       0.0, 1.0);
        g_progress.progress = std::clamp(g_progressBase + g_progressSpan * frac, 0.0, 1.0);
        snapCopy = g_progress;
        cbCopy = g_progressCallback;
    }
    notifyProgressCallback(snapCopy, cbCopy);
}

void updateNetMdProgressPhase(const std::string& phase, double localFraction) {
    NetMdProgressSnapshot snapCopy;
    NetMdProgressCallback cbCopy;
    {
        std::lock_guard<std::mutex> lock(g_progressMutex);
        if (!g_progress.active) {
            return;
        }
        if (!phase.empty()) {
            g_progress.phase = phase;
        }
        const double clamped = std::clamp(localFraction, 0.0, 1.0);
        if (g_progressSpan > 0.0) {
            g_progress.progress = std::clamp(g_progressBase + g_progressSpan * clamped, 0.0, 1.0);
        }
        snapCopy = g_progress;
        cbCopy = g_progressCallback;
    }
    notifyProgressCallback(snapCopy, cbCopy);
}

void clearNetMdProgress() {
    std::lock_guard<std::mutex> lock(g_progressMutex);
    g_progress = {};
    g_progressBase = 0.0;
    g_progressSpan = 0.0;
    resetUploadSpanTrackingLocked();
}

NetMdProgressSnapshot getNetMdProgress() {
    std::lock_guard<std::mutex> lock(g_progressMutex);
    return g_progress;
}

void setNetMdProgressCallback(NetMdProgressCallback cb) {
    std::lock_guard<std::mutex> lock(g_progressMutex);
    g_progressCallback = std::move(cb);
}

} // namespace carta

namespace carta_des {

constexpr std::uint8_t kPC1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18,
                                   10, 2,  59, 51, 43, 35, 27, 19, 11, 3,  60, 52, 44, 36,
                                   63, 55, 47, 39, 31, 23, 15, 7,  62, 54, 46, 38, 30, 22,
                                   14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};

constexpr std::uint8_t kPC2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
                                   26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
                                   51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};

constexpr std::uint8_t kShifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};

constexpr std::uint8_t kIP[64] = {58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
                                  62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
                                  57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
                                  61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};

constexpr std::uint8_t kFP[64] = {40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
                                  38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
                                  36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
                                  34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25};

constexpr std::uint8_t kE[48] = {32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11,
                                 12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
                                 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};

constexpr std::uint8_t kP[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
                                 2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25};

constexpr std::uint8_t kS[8][64] = {
    {14, 4,  13, 1, 2,  15, 11, 8, 3, 10, 6, 12, 5,  9,  0,  7,  0,  15, 7,  4,  14, 2,
     13, 1,  10, 6, 12, 11, 9,  5, 3, 8,  4, 1,  14, 8,  13, 6,  2,  11, 15, 12, 9,  7,
     3,  10, 5,  0, 15, 12, 8,  2, 4, 9,  1, 7,  5,  11, 3,  14, 10, 0,  6,  13},
    {15, 1,  8,  14, 6,  11, 3,  4, 9,  7,  2, 13, 12, 0,  5,  10, 3,  13, 4,  7, 15, 2,
     8,  14, 12, 0,  1,  10, 6,  9, 11, 5,  0, 14, 7,  11, 10, 4,  13, 1,  5,  8, 12, 6,
     9,  3,  2,  15, 13, 8,  10, 1, 3,  15, 4, 2,  11, 6,  7,  12, 0,  5,  14, 9},
    {10, 0,  9,  14, 6, 3,  15, 5,  1,  13, 12, 7, 11, 4,  2,  8,  13, 7, 0,  9, 3, 4,
     6,  10, 2,  8,  5, 14, 12, 11, 15, 1,  13, 6, 4,  9,  8,  15, 3,  0, 11, 1, 2, 12,
     5,  10, 14, 7,  1, 10, 13, 0,  6,  9,  8,  7, 4,  15, 14, 3,  11, 5, 2,  12},
    {7, 13, 14, 3, 0, 6,  9, 10, 1,  2, 8,  5, 11, 12, 4,  15, 13, 8,  11, 5, 6, 15,
     0, 3,  4,  7, 2, 12, 1, 10, 14, 9, 10, 6, 9,  0,  12, 11, 7,  13, 15, 1, 3, 14,
     5, 2,  8,  4, 3, 15, 0, 6,  10, 1, 13, 8, 9,  4,  5,  11, 12, 7,  2,  14},
    {2,  12, 4, 1,  7,  10, 11, 6, 8, 5,  3, 15, 13, 0,  14, 9,  14, 11, 2,  12, 4,  7,
     13, 1,  5, 0,  15, 10, 3,  9, 8, 6,  4, 2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,
     6,  3,  0, 14, 11, 8,  12, 7, 1, 14, 2, 13, 6,  15, 0,  9,  10, 4,  5,  3},
    {12, 1,  10, 15, 9,  2,  6, 8,  0, 13, 3,  4,  14, 7,  5, 11, 10, 15, 4, 2, 7, 12,
     9,  5,  6,  1,  13, 14, 0, 11, 3, 8,  9,  14, 15, 5,  2, 8,  12, 3,  7, 0, 4, 10,
     1,  13, 11, 6,  4,  3,  2, 12, 9, 5,  15, 10, 11, 14, 1, 7,  6,  0,  8, 13},
    {4, 11, 2,  14, 15, 0,  8,  13, 3, 12, 9,  7, 5,  10, 6,  1,  13, 0,  11, 7,  4, 9,
     1, 10, 14, 3,  5,  12, 2,  15, 8, 6,  1,  4, 11, 13, 12, 3,  7,  14, 10, 15, 6, 8,
     0, 5,  9,  2,  6,  11, 13, 8,  1, 4,  10, 7, 9,  5,  0,  15, 14, 2,  3,  12},
    {13, 2, 8,  4, 6, 15, 11, 1,  10, 9,  3, 14, 5,  0,  12, 7,  1,  15, 13, 8, 10, 3,
     7,  4, 12, 5, 6, 11, 0,  14, 9,  2,  7, 11, 4,  1,  9,  12, 14, 2,  0,  6, 10, 13,
     15, 3, 5,  8, 2, 1,  14, 7,  4,  10, 8, 13, 15, 12, 9,  0,  3,  5,  6,  11},
};

std::uint64_t loadBe64(const std::uint8_t* p) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<std::uint64_t>(p[i]);
    }
    return v;
}

void storeBe64(std::uint8_t* p, std::uint64_t v) noexcept {
    for (int i = 7; i >= 0; --i) {
        p[i] = static_cast<std::uint8_t>(v & 0xFFu);
        v >>= 8;
    }
}

template <std::size_t N>
std::uint64_t permute(std::uint64_t in, int inBits, const std::uint8_t (&tbl)[N]) noexcept {
    std::uint64_t out = 0;
    for (std::size_t i = 0; i < N; ++i) {
        const std::uint64_t bit = (in >> (inBits - tbl[i])) & 1ULL;
        out = (out << 1) | bit;
    }
    return out;
}

struct Subkeys {
    std::uint64_t k[16];
};

Subkeys makeSubkeys(const std::uint8_t* key8) noexcept {
    const std::uint64_t k56 = permute(loadBe64(key8), 64, kPC1);
    std::uint32_t c = static_cast<std::uint32_t>((k56 >> 28) & 0x0FFFFFFFULL);
    std::uint32_t d = static_cast<std::uint32_t>(k56 & 0x0FFFFFFFULL);
    Subkeys sk{};
    for (int r = 0; r < 16; ++r) {
        const int s = kShifts[r];
        c = ((c << s) | (c >> (28 - s))) & 0x0FFFFFFFu;
        d = ((d << s) | (d >> (28 - s))) & 0x0FFFFFFFu;
        const std::uint64_t cd = (static_cast<std::uint64_t>(c) << 28) | d;
        sk.k[r] = permute(cd, 56, kPC2);
    }
    return sk;
}

std::uint64_t cryptBlock(std::uint64_t block, const Subkeys& sk, bool encrypt) noexcept {
    const std::uint64_t ip = permute(block, 64, kIP);
    std::uint32_t l = static_cast<std::uint32_t>(ip >> 32);
    std::uint32_t r = static_cast<std::uint32_t>(ip & 0xFFFFFFFFu);
    for (int i = 0; i < 16; ++i) {
        const std::uint64_t rk = sk.k[encrypt ? i : (15 - i)];
        const std::uint64_t er = permute(r, 32, kE) ^ rk;
        std::uint32_t sOut = 0;
        for (int b = 0; b < 8; ++b) {
            const unsigned chunk = static_cast<unsigned>((er >> (42 - b * 6)) & 0x3Fu);
            const unsigned row = ((chunk & 0x20u) >> 4) | (chunk & 0x01u);
            const unsigned col = (chunk >> 1) & 0x0Fu;
            sOut = (sOut << 4) | kS[b][row * 16u + col];
        }
        const std::uint32_t f = static_cast<std::uint32_t>(permute(sOut, 32, kP));
        const std::uint32_t nextR = l ^ f;
        l = r;
        r = nextR;
    }
    const std::uint64_t preOut = (static_cast<std::uint64_t>(r) << 32) | l;
    return permute(preOut, 64, kFP);
}

} // namespace carta_des

// ============================================================================
// Porffor NetMD Host C ABI Implementation
// ============================================================================

extern "C" {

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_des_crypt(int is_triple_des, int is_encrypt,
                                                       int is_cbc, const std::uint8_t* key_ptr,
                                                       int key_len, const std::uint8_t* iv_ptr,
                                                       const std::uint8_t* in_ptr, int in_len,
                                                       std::uint8_t* out_ptr) {
    if (!key_ptr || !out_ptr || in_len < 0 || (in_len > 0 && !in_ptr)) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked("Invalid DES/3DES arguments.");
        return -1;
    }
    if (in_len == 0) {
        return 0;
    }
    if ((in_len % 8) != 0) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked("DES/3DES input length must be a multiple of 8 bytes.");
        return -1;
    }

    carta_des::Subkeys sk1{};
    carta_des::Subkeys sk2{};
    carta_des::Subkeys sk3{};
    if (is_triple_des) {
        if (key_len == 16) {
            sk1 = carta_des::makeSubkeys(key_ptr);
            sk2 = carta_des::makeSubkeys(key_ptr + 8);
            sk3 = sk1;
        } else if (key_len == 24) {
            sk1 = carta_des::makeSubkeys(key_ptr);
            sk2 = carta_des::makeSubkeys(key_ptr + 8);
            sk3 = carta_des::makeSubkeys(key_ptr + 16);
        } else {
            std::lock_guard<std::mutex> lock(carta::g_hostMutex);
            carta::setErrorLocked("3DES key must be 16 or 24 bytes.");
            return -1;
        }
    } else if (key_len == 8) {
        sk1 = carta_des::makeSubkeys(key_ptr);
    } else {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked("DES key must be 8 bytes.");
        return -1;
    }

    std::uint64_t chain = (is_cbc && iv_ptr) ? carta_des::loadBe64(iv_ptr) : 0ULL;
    const bool enc = (is_encrypt != 0);
    for (int off = 0; off < in_len; off += 8) {
        const std::uint64_t inBlock = carta_des::loadBe64(in_ptr + off);
        std::uint64_t x = (is_cbc && enc) ? (inBlock ^ chain) : inBlock;
        if (is_triple_des) {
            if (enc) {
                x = carta_des::cryptBlock(x, sk1, true);
                x = carta_des::cryptBlock(x, sk2, false);
                x = carta_des::cryptBlock(x, sk3, true);
            } else {
                x = carta_des::cryptBlock(x, sk3, false);
                x = carta_des::cryptBlock(x, sk2, true);
                x = carta_des::cryptBlock(x, sk1, false);
            }
        } else {
            x = carta_des::cryptBlock(x, sk1, enc);
        }
        if (is_cbc) {
            if (enc) {
                chain = x;
            } else {
                x ^= chain;
                chain = inBlock;
            }
        }
        carta_des::storeBe64(out_ptr + off, x);
    }
    return in_len;
}

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_random_bytes(std::uint8_t* out_ptr, int len) {
    if (!out_ptr || len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
#if defined(__APPLE__)
    arc4random_buf(out_ptr, static_cast<size_t>(len));
    return len;
#elif defined(_WIN32)
    if (BCryptGenRandom(nullptr, out_ptr, static_cast<ULONG>(len),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked("BCryptGenRandom failed.");
        return -1;
    }
    return len;
#else
    size_t filled = 0;
    while (filled < static_cast<size_t>(len)) {
        const ssize_t n = ::getrandom(out_ptr + filled, static_cast<size_t>(len) - filled, 0);
        if (n > 0) {
            filled += static_cast<size_t>(n);
        } else if (errno == EINTR) {
            continue;
        } else {
            std::FILE* urand = std::fopen("/dev/urandom", "rb");
            if (!urand) {
                std::lock_guard<std::mutex> lock(carta::g_hostMutex);
                carta::setErrorLocked("Failed generating random bytes.");
                return -1;
            }
            const size_t got =
                std::fread(out_ptr + filled, 1, static_cast<size_t>(len) - filled, urand);
            std::fclose(urand);
            if (got != static_cast<size_t>(len) - filled) {
                std::lock_guard<std::mutex> lock(carta::g_hostMutex);
                carta::setErrorLocked("Short read from /dev/urandom.");
                return -1;
            }
            break;
        }
    }
    return len;
#endif
}

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_cp932(int to_utf8, const std::uint8_t* in_ptr,
                                                   int in_len, std::uint8_t* out_ptr, int out_cap) {
    if (!out_ptr || out_cap < 0 || in_len < 0 || (in_len > 0 && !in_ptr)) {
        return -1;
    }
    if (in_len == 0) {
        return 0;
    }

#if defined(_WIN32)
    const UINT srcCp = to_utf8 ? 932u : CP_UTF8;
    const UINT dstCp = to_utf8 ? CP_UTF8 : 932u;
    const int wideLen =
        MultiByteToWideChar(srcCp, 0, reinterpret_cast<const char*>(in_ptr), in_len, nullptr, 0);
    if (wideLen <= 0) {
        return -1;
    }
    std::vector<wchar_t> wide(static_cast<std::size_t>(wideLen));
    if (MultiByteToWideChar(srcCp, 0, reinterpret_cast<const char*>(in_ptr), in_len, wide.data(),
                            wideLen) != wideLen) {
        return -1;
    }
    const int outLen =
        WideCharToMultiByte(dstCp, 0, wide.data(), wideLen, reinterpret_cast<char*>(out_ptr),
                            out_cap, nullptr, nullptr);
    if (outLen <= 0) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked("CP932 conversion failed or output buffer capacity exceeded.");
        return -1;
    }
    return outLen;
#else
    const char* fromCode = to_utf8 ? "CP932" : "UTF-8";
    const char* toCode = to_utf8 ? "UTF-8" : "CP932";
    iconv_t cd = iconv_open(toCode, fromCode);
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        cd = iconv_open(to_utf8 ? "UTF-8" : "SHIFT_JIS", to_utf8 ? "SHIFT_JIS" : "UTF-8");
    }
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked("iconv_open failed for CP932 conversion.");
        return -1;
    }

    char* src = const_cast<char*>(reinterpret_cast<const char*>(in_ptr));
    size_t srcLeft = static_cast<size_t>(in_len);
    char* dst = reinterpret_cast<char*>(out_ptr);
    size_t dstLeft = static_cast<size_t>(out_cap);

    while (srcLeft > 0) {
        const size_t rc = iconv(cd, &src, &srcLeft, &dst, &dstLeft);
        if (rc == static_cast<size_t>(-1)) {
            if (errno == E2BIG) {
                iconv_close(cd);
                std::lock_guard<std::mutex> lock(carta::g_hostMutex);
                carta::setErrorLocked("CP932 output buffer capacity exceeded.");
                return -1;
            }
            if (errno == EILSEQ || errno == EINVAL) {
                if (dstLeft == 0) {
                    iconv_close(cd);
                    return -1;
                }
                *dst++ = '?';
                --dstLeft;
                // Advance past invalid/unrepresentable sequence.
                if (!to_utf8 && (static_cast<unsigned char>(*src) & 0x80u) != 0) {
                    ++src;
                    --srcLeft;
                    while (srcLeft > 0 && (static_cast<unsigned char>(*src) & 0xC0u) == 0x80u) {
                        ++src;
                        --srcLeft;
                    }
                } else {
                    ++src;
                    --srcLeft;
                }
                continue;
            }
            iconv_close(cd);
            return -1;
        }
    }
    iconv_close(cd);
    return static_cast<int>(static_cast<size_t>(out_cap) - dstLeft);
#endif
}

CARTA_NETMD_HOST_EXPORT void carta_netmd_host_sleep_ms(int ms) {
    if (ms <= 0) {
        return;
    }
    bool hasRealUsbOpen = false;
    {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::ensureMockInitLocked();
        for (const auto& kv : carta::g_handles) {
            if (!kv.second.isMock) {
                hasRealUsbOpen = true;
                break;
            }
        }
        if (!hasRealUsbOpen) {
            carta::g_mock.sleptMs += ms;
            return;
        }
    }

    const int totalMs = std::clamp(ms, 0, 120000);
    if (totalMs >= 5000) {
        double baseP = 0.0;
        double endP = 1.0;
        {
            std::lock_guard<std::mutex> pLock(carta::g_progressMutex);
            baseP = carta::g_progress.progress;
            endP = std::max(baseP, carta::g_progressBase + carta::g_progressSpan);
            carta::g_progressBase = baseP;
            carta::g_progressSpan = std::max(0.0, endP - baseP);
        }
        int elapsed = 0;
        while (elapsed < totalMs) {
            const int remMs = totalMs - elapsed;
            const int remSec = std::max(1, (remMs + 999) / 1000);
            const double frac = static_cast<double>(elapsed) / static_cast<double>(totalMs);
            carta::updateNetMdProgressPhase("Committing UTOC to disc (TOCWRT — " +
                                                std::to_string(remSec) + "s remaining)...",
                                            frac);
            const int slice = std::min(250, remMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            elapsed += slice;
        }
        carta::updateNetMdProgressPhase("Committing UTOC to disc (TOCWRT)...", 1.0);
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(totalMs));
}

CARTA_NETMD_HOST_EXPORT void carta_netmd_host_report_step(int done, int total) {
    if (total <= 0) {
        return;
    }
    const double frac = static_cast<double>(done) / static_cast<double>(total);
    carta::updateNetMdProgressPhase({}, frac);
}

CARTA_NETMD_HOST_EXPORT double carta_netmd_host_now_ms(void) {
    bool hasRealUsbOpen = false;
    std::int64_t mockMs = 0;
    {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::ensureMockInitLocked();
        for (const auto& kv : carta::g_handles) {
            if (!kv.second.isMock) {
                hasRealUsbOpen = true;
                break;
            }
        }
        mockMs = carta::g_mock.sleptMs;
    }
    if (!hasRealUsbOpen) {
        return static_cast<double>(mockMs);
    }
    static const auto kMonotonicStart = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                     kMonotonicStart)
        .count();
}

CARTA_NETMD_HOST_EXPORT void carta_netmd_host_report_toc_wait(int elapsed_ms, int max_ms,
                                                              int done) {
    if (done != 0) {
        carta::updateNetMdProgressPhase("Committing UTOC to disc (TOCWRT)...", 1.0);
        return;
    }
    const int elapsedSec = std::max(0, elapsed_ms) / 1000;
    const double targetMs = (max_ms > 0) ? std::min<double>(max_ms, 15000.0) : 15000.0;
    const double pollFrac =
        std::clamp(static_cast<double>(std::max(0, elapsed_ms)) / targetMs, 0.0, 0.95);
    const double overallFrac = 0.5 + 0.45 * pollFrac;
    carta::updateNetMdProgressPhase(
        "Committing UTOC to disc (TOCWRT — " + std::to_string(elapsedSec) + "s)...",
        overallFrac);
}

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_list(std::uint8_t* json_out, int json_cap) {
    if (!json_out || json_cap <= 0) {
        return -1;
    }
    std::ostringstream oss;
    oss << "[";
    bool first = true;

    if (carta::g_mockEnabled.load()) {
        oss << "{\"id\":\"" << carta::kMockDeviceId << "\",\"vendorId\":" << carta::kMockVendorId
            << ",\"productId\":" << carta::kMockProductId << ",\"bus\":0,\"address\":0"
            << ",\"name\":\"" << carta::jsonEscape(carta::kMockProductName)
            << "\",\"isMock\":true}";
        first = false;
    }

    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) == 0 && ctx != nullptr) {
        libusb_device** devs = nullptr;
        const ssize_t cnt = libusb_get_device_list(ctx, &devs);
        for (ssize_t i = 0; i < cnt; ++i) {
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(devs[i], &desc) != 0) {
                continue;
            }
            const char* known = carta::lookupKnownNetMdName(desc.idVendor, desc.idProduct);
            if (!known) {
                continue;
            }
            const std::string devId = carta::formatUsbDeviceId(devs[i], desc);
            const unsigned bus = static_cast<unsigned>(libusb_get_bus_number(devs[i]));
            const unsigned addr = static_cast<unsigned>(libusb_get_device_address(devs[i]));
            if (!first) {
                oss << ",";
            }
            first = false;
            oss << "{\"id\":\"" << carta::jsonEscape(devId) << "\",\"vendorId\":" << desc.idVendor
                << ",\"productId\":" << desc.idProduct << ",\"bus\":" << bus
                << ",\"address\":" << addr << ",\"name\":\"" << carta::jsonEscape(known)
                << "\",\"isMock\":false}";
        }
        if (devs) {
            libusb_free_device_list(devs, 1);
        }
        libusb_exit(ctx);
    }

    oss << "]";
    const std::string json = oss.str();
    if (static_cast<int>(json.size()) > json_cap) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked("USB list JSON exceeds output buffer.");
        return -1;
    }
    std::memcpy(json_out, json.data(), json.size());
    return static_cast<int>(json.size());
}

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_open(const char* device_id) {
    std::lock_guard<std::mutex> lock(carta::g_hostMutex);
    carta::ensureMockInitLocked();
    if (!device_id || !*device_id) {
        carta::setErrorLocked("Empty NetMD device id.");
        return -1;
    }

    const std::string targetId(device_id);
    if (targetId == carta::kMockDeviceId) {
        if (!carta::g_mockEnabled.load()) {
            carta::setErrorLocked("Mock NetMD device is disabled.");
            return -1;
        }
        carta::g_mock.open = true;
        carta::g_mock.replyQueue.clear();
        carta::g_mock.awaitingBulk = false;
        carta::g_mock.bulkBuffer.clear();
        const int h = carta::g_nextHandle++;
        carta::UsbOpenHandle entry;
        entry.isMock = true;
        entry.vendorId = carta::kMockVendorId;
        entry.productId = carta::kMockProductId;
        carta::g_handles[h] = entry;
        return h;
    }

    unsigned vid = 0;
    unsigned pid = 0;
    if (std::sscanf(targetId.c_str(), "usb:%x:%x", &vid, &pid) != 2) {
        carta::setErrorLocked("Unsupported NetMD device id: " + targetId);
        return -1;
    }

    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0 || !ctx) {
        carta::setErrorLocked("libusb_init failed.");
        return -1;
    }

    libusb_device_handle* usbHandle = nullptr;
    std::string openErr;
    if (!carta::openAndClaimUsbDeviceLocked(ctx, targetId, vid, pid, &usbHandle, openErr)) {
        libusb_exit(ctx);
        carta::setErrorLocked(openErr);
        return -1;
    }

    // Read-only NetMD EP0 health check (0xC1 0x01 getReplyLength). If a prior session or
    // hardware wake left EP0 wedged (e.g. Darwin kIOReturnNotResponding / LIBUSB_ERROR_IO),
    // resetting and reopening the USB port clears the condition without touching the disc.
    std::uint8_t probeBuf[4]{};
    const uint8_t probeReq = static_cast<uint8_t>(
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE);
    int probeRc = libusb_control_transfer(usbHandle, probeReq, 0x01, 0, 0, probeBuf,
                                          static_cast<uint16_t>(sizeof(probeBuf)), 2000);
    if (probeRc < 0 && carta::isRecoverableUsbIoError(probeRc)) {
        (void)libusb_reset_device(usbHandle);
        libusb_release_interface(usbHandle, 0);
        libusb_close(usbHandle);
        usbHandle = nullptr;
        libusb_exit(ctx);
        ctx = nullptr;

        bool recovered = false;
        for (int attempt = 0; attempt < 6; ++attempt) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(attempt == 0 ? 450 : 250));
            if (libusb_init(&ctx) != 0 || !ctx) {
                ctx = nullptr;
                continue;
            }
            std::string retryErr;
            if (!carta::openAndClaimUsbDeviceLocked(ctx, targetId, vid, pid, &usbHandle,
                                                    retryErr)) {
                libusb_exit(ctx);
                ctx = nullptr;
                continue;
            }
            probeRc = libusb_control_transfer(usbHandle, probeReq, 0x01, 0, 0, probeBuf,
                                              static_cast<uint16_t>(sizeof(probeBuf)), 2000);
            if (probeRc >= 0) {
                recovered = true;
                break;
            }
            libusb_release_interface(usbHandle, 0);
            libusb_close(usbHandle);
            usbHandle = nullptr;
            libusb_exit(ctx);
            ctx = nullptr;
        }
        if (!recovered) {
            carta::setErrorLocked("NetMD USB device not responding after port reset: " +
                                  std::string(libusb_error_name(probeRc)));
            return -1;
        }
    } else if (probeRc < 0) {
        libusb_release_interface(usbHandle, 0);
        libusb_close(usbHandle);
        libusb_exit(ctx);
        carta::setErrorLocked(std::string("USB control IN failed: ") + libusb_error_name(probeRc));
        return -1;
    }

    const int h = carta::g_nextHandle++;
    carta::UsbOpenHandle entry;
    entry.isMock = false;
    entry.usbHandle = usbHandle;
    entry.usbCtx = ctx;
    entry.interfaceNumber = 0;
    entry.vendorId = static_cast<std::uint16_t>(vid);
    entry.productId = static_cast<std::uint16_t>(pid);
#if defined(__APPLE__)
    IOPMAssertionID assertionId = kIOPMNullAssertionID;
    if (IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep,
                                    kIOPMAssertionLevelOn,
                                    CFSTR("Carta Studio NetMD USB transfer"),
                                    &assertionId) == kIOReturnSuccess) {
        entry.sleepAssertionId = assertionId;
    }
#endif
    carta::g_handles[h] = entry;
    return h;
}

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_close(int handle) {
    std::lock_guard<std::mutex> lock(carta::g_hostMutex);
    auto it = carta::g_handles.find(handle);
    if (it == carta::g_handles.end()) {
        return -1;
    }
    if (it->second.isMock) {
        carta::g_mock.open = false;
    } else {
#if defined(__APPLE__)
        if (it->second.sleepAssertionId != kIOPMNullAssertionID) {
            IOPMAssertionRelease(it->second.sleepAssertionId);
            it->second.sleepAssertionId = kIOPMNullAssertionID;
        }
#endif
        if (it->second.usbHandle) {
            if (it->second.hadIoError) {
                (void)libusb_reset_device(it->second.usbHandle);
            }
            libusb_release_interface(it->second.usbHandle, it->second.interfaceNumber);
            libusb_close(it->second.usbHandle);
        }
        if (it->second.usbCtx) {
            libusb_exit(it->second.usbCtx);
        }
    }
    carta::g_handles.erase(it);
    return 0;
}

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_reset(int handle) {
    std::lock_guard<std::mutex> lock(carta::g_hostMutex);
    auto it = carta::g_handles.find(handle);
    if (it == carta::g_handles.end()) {
        return -1;
    }
    if (it->second.isMock) {
        carta::g_mock.replyQueue.clear();
        carta::g_mock.awaitingBulk = false;
        carta::g_mock.bulkBuffer.clear();
        return 0;
    }
    if (!it->second.usbHandle) {
        return -1;
    }
    const int rc = libusb_reset_device(it->second.usbHandle);
    if (rc != 0) {
        carta::setErrorLocked(std::string("USB reset failed: ") + libusb_error_name(rc));
        return -1;
    }
    return 0;
}

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_control_out(int handle, int request, int value,
                                                             int index, const std::uint8_t* data,
                                                             int len) {
    std::lock_guard<std::mutex> lock(carta::g_hostMutex);
    auto it = carta::g_handles.find(handle);
    if (it == carta::g_handles.end() || len < 0 || (len > 0 && !data)) {
        carta::setErrorLocked("Invalid USB control OUT arguments.");
        return -1;
    }

    const bool enforceGuard = !it->second.isMock || carta::g_simulateRealGuardOnMock.load();
    if (enforceGuard && !carta::g_hardwareWriteConfirmed.load()) {
        if (!carta::isReadOnlyControlCommandAllowed(request, data, len)) {
            carta::setErrorLocked("Hardware write blocked: explicit confirmation is required "
                                  "before modifying a MiniDisc.");
            return -1;
        }
    }

    // NetMD sendTrack setup carries exact total bulk byte size (including packet headers and
    // SPUpload calibration padding) in bytes 26..29.
    if (request != 0xff && data != nullptr && len >= 30 && data[1] == 0x18 && data[2] == 0x00 &&
        data[6] == 0xf0 && data[7] == 0x03 && data[8] == 0x01 && data[9] == 0x03 &&
        data[10] == 0x28) {
        const auto exactBulkBytes = static_cast<std::int64_t>(carta::readBe32(data + 26));
        if (exactBulkBytes > 0) {
            std::lock_guard<std::mutex> pLock(carta::g_progressMutex);
            carta::g_uploadSpanExpectedBulkBytes = exactBulkBytes;
            carta::g_uploadSpanBulkSent = 0;
        }
    }

    if (it->second.isMock) {
        const bool ok = (request == 0xff) ? carta::handleMockFactoryControlOut(data, len)
                                          : carta::handleMockVendorControlOut(data, len);
        return ok ? len : -1;
    }

    const uint8_t bmReq = static_cast<uint8_t>(LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                                               LIBUSB_RECIPIENT_INTERFACE);
    const int rc =
        libusb_control_transfer(it->second.usbHandle, bmReq, static_cast<uint8_t>(request),
                                static_cast<uint16_t>(value), static_cast<uint16_t>(index),
                                const_cast<unsigned char*>(data), static_cast<uint16_t>(len), 30000);
    if (rc < 0) {
        if (rc == LIBUSB_ERROR_IO || rc == LIBUSB_ERROR_PIPE || rc == LIBUSB_ERROR_TIMEOUT) {
            it->second.hadIoError = true;
        }
        carta::setErrorLocked(std::string("USB control OUT failed: ") + libusb_error_name(rc));
        return -1;
    }
    return rc;
}

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_control_in(int handle, int request, int value,
                                                            int index, std::uint8_t* out,
                                                            int max_len) {
    std::lock_guard<std::mutex> lock(carta::g_hostMutex);
    auto it = carta::g_handles.find(handle);
    if (it == carta::g_handles.end() || !out || max_len < 0) {
        carta::setErrorLocked("Invalid USB control IN arguments.");
        return -1;
    }

    if (it->second.isMock) {
        if (request == 0x01) {
            std::uint8_t replyLen = 0;
            if (!carta::g_mock.replyQueue.empty()) {
                replyLen = static_cast<std::uint8_t>(
                    std::min<std::size_t>(255u, carta::g_mock.replyQueue.front().size()));
            }
            const std::uint8_t hdr[4] = {0x00, 0x00, replyLen, 0x00};
            const int n = std::min(max_len, 4);
            std::memcpy(out, hdr, static_cast<std::size_t>(n));
            return n;
        }
        if (carta::g_mock.replyQueue.empty()) {
            return 0;
        }
        const auto front = std::move(carta::g_mock.replyQueue.front());
        carta::g_mock.replyQueue.pop_front();
        const int n = std::min<int>(max_len, static_cast<int>(front.size()));
        if (n > 0) {
            std::memcpy(out, front.data(), static_cast<std::size_t>(n));
        }
        return n;
    }

    const uint8_t bmReq = static_cast<uint8_t>(LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
                                               LIBUSB_RECIPIENT_INTERFACE);
    const int rc = libusb_control_transfer(
        it->second.usbHandle, bmReq, static_cast<uint8_t>(request), static_cast<uint16_t>(value),
        static_cast<uint16_t>(index), out, static_cast<uint16_t>(max_len), 30000);
    if (rc < 0) {
        if (rc == LIBUSB_ERROR_IO || rc == LIBUSB_ERROR_PIPE || rc == LIBUSB_ERROR_TIMEOUT) {
            it->second.hadIoError = true;
        }
        carta::setErrorLocked(std::string("USB control IN failed: ") + libusb_error_name(rc));
        return -1;
    }
    return rc;
}

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_bulk_out(int handle, int endpoint,
                                                          const std::uint8_t* data, int len) {
    std::lock_guard<std::mutex> lock(carta::g_hostMutex);
    auto it = carta::g_handles.find(handle);
    if (it == carta::g_handles.end() || len < 0 || (len > 0 && !data)) {
        carta::setErrorLocked("Invalid USB bulk OUT arguments.");
        return -1;
    }

    const bool enforceGuard = !it->second.isMock || carta::g_simulateRealGuardOnMock.load();
    if (enforceGuard && !carta::g_hardwareWriteConfirmed.load()) {
        carta::setErrorLocked("Hardware write blocked: explicit confirmation is required before "
                              "modifying a MiniDisc.");
        return -1;
    }

    if (it->second.isMock) {
        if (!carta::g_mock.awaitingBulk) {
            carta::setErrorLocked("MockNetMd: unexpected bulk OUT transfer.");
            return -1;
        }
        if (len > 0) {
            carta::g_mock.bulkBuffer.insert(carta::g_mock.bulkBuffer.end(), data, data + len);
        }
        if (carta::g_mock.bulkBuffer.size() >= carta::g_mock.expectedTotalBulkBytes) {
            if (!carta::finalizeMockBulkUpload()) {
                return -1;
            }
        }
        carta::recordUploadBulkProgress(len);
        return len;
    }

    int totalSent = 0;
    const uint8_t ep =
        static_cast<uint8_t>((static_cast<unsigned int>(endpoint) & 0x0Fu) | LIBUSB_ENDPOINT_OUT);
    while (totalSent < len) {
        const int chunkLen = std::min(16384, len - totalSent);
        int transferred = 0;
        const int rc = libusb_bulk_transfer(it->second.usbHandle, ep,
                                            const_cast<unsigned char*>(data + totalSent), chunkLen,
                                            &transferred, 120000);
        if (rc != 0 || transferred <= 0) {
            if (rc == LIBUSB_ERROR_IO || rc == LIBUSB_ERROR_PIPE || rc == LIBUSB_ERROR_TIMEOUT) {
                it->second.hadIoError = true;
            }
            carta::setErrorLocked(std::string("USB bulk OUT failed: ") + libusb_error_name(rc));
            return -1;
        }
        totalSent += transferred;
        carta::recordUploadBulkProgress(transferred);
    }
    return totalSent;
}

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_bulk_in(int handle, int endpoint,
                                                         std::uint8_t* out, int max_len) {
    std::lock_guard<std::mutex> lock(carta::g_hostMutex);
    auto it = carta::g_handles.find(handle);
    if (it == carta::g_handles.end() || !out || max_len < 0) {
        carta::setErrorLocked("Invalid USB bulk IN arguments.");
        return -1;
    }

    if (it->second.isMock) {
        return 0;
    }

    int transferred = 0;
    const uint8_t ep =
        static_cast<uint8_t>((static_cast<unsigned int>(endpoint) & 0x0Fu) | LIBUSB_ENDPOINT_IN);
    const int rc =
        libusb_bulk_transfer(it->second.usbHandle, ep, out, max_len, &transferred, 30000);
    if (rc != 0) {
        if (rc == LIBUSB_ERROR_IO || rc == LIBUSB_ERROR_PIPE || rc == LIBUSB_ERROR_TIMEOUT) {
            it->second.hadIoError = true;
        }
        carta::setErrorLocked(std::string("USB bulk IN failed: ") + libusb_error_name(rc));
        return -1;
    }
    return transferred;
}

CARTA_NETMD_HOST_EXPORT const char* carta_netmd_host_last_error(void) {
    std::lock_guard<std::mutex> lock(carta::g_hostMutex);
    return carta::g_lastError.c_str();
}

} // extern "C"
