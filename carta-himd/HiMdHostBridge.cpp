/**
 * @file HiMdHostBridge.cpp
 * Implementation of Porffor Hi-MD host C ABI callbacks, streaming file I/O,
 * portable DES/3DES & CSPRNG, iconv CP932 conversion, hardware write safety
 * guards, and an offline Mock Sony MZ-RH1 Hi-MD SCSI/FAT16 + DRM device.
 */

#include "HiMdHostBridge.h"
#include "HiMdScsiTransport.h"

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
#include <stdlib.h>
#else
#include <sys/random.h>
#endif
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace carta_des {
namespace {

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

} // namespace
} // namespace carta_des

namespace carta {
namespace {

// ============================================================================
// Internal Constants & Helpers
// ============================================================================

constexpr const char* kMockDeviceId = "mock:mz-rh1";
constexpr std::uint16_t kMockVendorId = 0x054c;
constexpr std::uint16_t kMockProductId = 0x0287;
constexpr const char* kMockProductName = "Sony MZ-RH1 (Mock Hi-MD)";

constexpr std::uint32_t kSectorBytes = 2048u;
constexpr std::uint32_t kTotalSectors = 16384u; // 32 MiB
constexpr std::uint32_t kSectorsPerCluster = 16u; // 32 KiB clusters
constexpr std::uint32_t kReservedSectors = 1u;
constexpr std::uint32_t kFatCount = 2u;
constexpr std::uint32_t kSectorsPerFat = 2u;
constexpr std::uint32_t kMaxRootEntries = 512u;
constexpr std::uint32_t kDataSectorOffset = kReservedSectors + kFatCount * kSectorsPerFat; // 5
constexpr std::uint32_t kRootDirSectors = (kMaxRootEntries * 32u) / kSectorBytes;        // 8
constexpr std::uint32_t kCluster2Sector = kDataSectorOffset + kRootDirSectors;           // 13

constexpr std::uint32_t kTrkIdxBytes = 0x50000u; // 327,680 bytes = 10 clusters
constexpr std::uint32_t kMcListBytes = 0x7D70u;  // 32,112 bytes = 1 cluster

// Sony Hi-MD EKB 0x00010012 root key (from himd-js/dist/encryption.js)
constexpr std::array<std::uint8_t, 24> kEkb00010012RootKey = {
    0xf5, 0x1e, 0xcb, 0x2a, 0x80, 0x8f, 0x15, 0xfd, 0x54, 0x2e, 0xf5, 0x12,
    0x3b, 0xcd, 0xbc, 0xa4, 0xf5, 0x1e, 0xcb, 0x2a, 0x80, 0x8f, 0x15, 0xfd,
};
constexpr std::array<std::uint8_t, 16> kStage2ExpectedEkbKey = {
    0x6a, 0x7a, 0x4c, 0x7d, 0x5f, 0x3f, 0x86, 0x84,
    0x28, 0x6d, 0x1a, 0x12, 0x32, 0x98, 0x22, 0x13,
};

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

template <std::size_t N>
std::string toHex(const std::array<std::uint8_t, N>& arr) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(N * 2);
    for (std::uint8_t b : arr) {
        out.push_back(kHex[(b >> 4) & 0x0Fu]);
        out.push_back(kHex[b & 0x0Fu]);
    }
    return out;
}

void writeLe16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

void writeLe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

std::uint16_t readBe16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8) | p[1]);
}

std::uint32_t readBe32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

void writeBe16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    p[1] = static_cast<std::uint8_t>(v & 0xFFu);
}

void writeBe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    p[3] = static_cast<std::uint8_t>(v & 0xFFu);
}

bool retailMac8(const std::uint8_t* key16, const std::uint8_t* data, std::size_t len,
                std::array<std::uint8_t, 8>& outMac) {
    if (len == 0 || (len % 8) != 0) {
        return false;
    }
    std::vector<std::uint8_t> cbcOut(len);
    std::uint8_t zeroIv[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (carta_himd_host_des_crypt(0, 1, 1, key16, 8, zeroIv, data, static_cast<int>(len),
                                  cbcOut.data()) < 0) {
        return false;
    }
    std::uint8_t step2[8]{};
    if (carta_himd_host_des_crypt(0, 0, 0, key16 + 8, 8, nullptr, cbcOut.data() + (len - 8), 8,
                                  step2) < 0) {
        return false;
    }
    if (carta_himd_host_des_crypt(0, 1, 0, key16, 8, nullptr, step2, 8, outMac.data()) < 0) {
        return false;
    }
    return true;
}

bool createIcvMac8(const std::uint8_t* sessionKey8, const std::uint8_t* headerAndIcv24,
                   std::array<std::uint8_t, 8>& outMac) {
    std::uint8_t zeroIv[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::array<std::uint8_t, 24> out{};
    if (carta_himd_host_des_crypt(0, 1, 1, sessionKey8, 8, zeroIv, headerAndIcv24, 24,
                                  out.data()) < 0) {
        return false;
    }
    std::memcpy(outMac.data(), out.data() + 16, 8);
    return true;
}

// ============================================================================
// Mutable Globals & Mock MZ-RH1 State
// ============================================================================

struct ScsiHandle {
    bool isMock = false;
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    std::shared_ptr<PhysicalHiMdHandle> physical;
};

struct MockHiMdState {
    bool initialized = false;
    bool open = false;
    bool inSession = false;
    bool stage1Done = false;
    bool stage2Done = false;
    bool stage3Done = false;
    int wipeBusyPollsRemaining = 0;
    int datanum = 1;
    std::uint32_t currentGeneration = 1;
    int authStartedCount = 0;
    int authCompletedCount = 0;
    int icvReadCount = 0;
    int icvWriteCount = 0;
    int sectorsRead = 0;
    int sectorsWritten = 0;
    int wipeCount = 0;
    std::int64_t sleptMs = 0;

    std::array<std::uint8_t, 8> deviceLeafId{0x02, 0x80, 0x00, 0x00, 0x52, 0x48, 0x31, 0x01};
    std::array<std::uint8_t, 16> discId{0x43, 0x41, 0x52, 0x54, 0x41, 0x48, 0x49, 0x4d,
                                        0x44, 0x4d, 0x4f, 0x43, 0x4b, 0x30, 0x30, 0x31};
    std::array<std::uint8_t, 16> headKey{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                         0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x01};
    std::array<std::uint8_t, 16> bodyKey{0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x07, 0x18,
                                         0x29, 0x3a, 0x4b, 0x5c, 0x6d, 0x7e, 0x8f, 0x90};

    std::array<std::uint8_t, 8> hostLeafId{};
    std::array<std::uint8_t, 8> hostNonce{};
    std::array<std::uint8_t, 8> deviceNonce{0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe};
    std::array<std::uint8_t, 8> deviceMac{};
    std::array<std::uint8_t, 8> sessionKey{};

    std::array<std::uint8_t, 8> currentIcvHeader{};
    std::array<std::uint8_t, 16> currentIcv{};

    std::vector<std::uint8_t> image; // kTotalSectors * kSectorBytes
};

std::mutex g_hostMutex;
std::string g_lastError;
std::map<int, ScsiHandle> g_handles;
int g_nextHandle = 1;

std::mutex g_fileMutex;
std::map<int, std::FILE*> g_openFiles;
int g_nextFileFd = 100;

std::mutex g_progressMutex;
HiMdProgressSnapshot g_progress;
HiMdProgressCallback g_progressCallback;

std::atomic<bool> g_mockEnabled{false};
std::atomic<bool> g_simulateRealGuardOnMock{false};
std::atomic<bool> g_hardwareWriteConfirmed{false};
MockHiMdState g_mock;

void setErrorLocked(const std::string& msg) {
    g_lastError = msg;
}

void writeDirEntry(std::uint8_t* dst, const char* name83, std::uint8_t attr,
                   std::uint32_t firstCluster, std::uint32_t fileSize) {
    std::memset(dst, 0, 32);
    std::memcpy(dst, name83, 11);
    dst[11] = attr;
    writeLe16(dst + 20, static_cast<std::uint16_t>((firstCluster >> 16) & 0xFFFFu));
    writeLe16(dst + 26, static_cast<std::uint16_t>(firstCluster & 0xFFFFu));
    writeLe32(dst + 28, fileSize);
}

void formatCleanMockImageLocked() {
    g_mock.image.assign(static_cast<std::size_t>(kTotalSectors) * kSectorBytes, 0);
    auto* img = g_mock.image.data();

    // 1. FAT16 Boot Sector (sector 0) matching nufatfs structFormatUnpack layouts
    auto* bs = img;
    bs[0] = 0xEB;
    bs[1] = 0x3C;
    bs[2] = 0x90;
    std::memcpy(bs + 3, "MSDOS5.0", 8);
    writeLe16(bs + 0x0B, static_cast<std::uint16_t>(kSectorBytes));
    bs[0x0D] = static_cast<std::uint8_t>(kSectorsPerCluster);
    writeLe16(bs + 0x0E, static_cast<std::uint16_t>(kReservedSectors));
    bs[0x10] = static_cast<std::uint8_t>(kFatCount);
    writeLe16(bs + 0x11, static_cast<std::uint16_t>(kMaxRootEntries));
    writeLe16(bs + 0x13, static_cast<std::uint16_t>(kTotalSectors));
    bs[0x15] = 0xF8;
    writeLe16(bs + 0x16, static_cast<std::uint16_t>(kSectorsPerFat));
    writeLe16(bs + 0x18, 32);
    writeLe16(bs + 0x1A, 64);
    writeLe32(bs + 0x1C, 0);
    writeLe32(bs + 0x20, 0);
    bs[0x24] = 0x80;
    bs[0x25] = 0x00;
    bs[0x26] = 0x29;
    writeLe32(bs + 0x27, 0x48494D44u);
    std::memcpy(bs + 0x2B, "HIMD MOCK  ", 11);
    std::memcpy(bs + 0x36, "FAT16   ", 8);
    bs[510] = 0x55;
    bs[511] = 0xAA;

    // 2. FAT1 & FAT2 (sectors 1..2 and 3..4)
    auto writeFatEntry = [&](std::uint16_t cluster, std::uint16_t value) {
        const std::size_t off1 = static_cast<std::size_t>(kReservedSectors) * kSectorBytes +
                                 static_cast<std::size_t>(cluster) * 2u;
        const std::size_t off2 =
            static_cast<std::size_t>(kReservedSectors + kSectorsPerFat) * kSectorBytes +
            static_cast<std::size_t>(cluster) * 2u;
        writeLe16(img + off1, value);
        writeLe16(img + off2, value);
    };
    writeFatEntry(0, 0xFFF8u);
    writeFatEntry(1, 0xFFFFu);
    writeFatEntry(2, 0xFFFFu); // /HMDHIFI/ directory
    for (std::uint16_t c = 3; c <= 11; ++c) {
        writeFatEntry(c, static_cast<std::uint16_t>(c + 1)); // TRKIDX01.HMA clusters 3..12
    }
    writeFatEntry(12, 0xFFFFu);
    writeFatEntry(13, 0xFFFFu); // MCLIST01.HMA cluster 13

    // 3. Root Directory (sector 5) -> HMDHIFI directory at cluster 2
    auto* rootDir = img + static_cast<std::size_t>(kDataSectorOffset) * kSectorBytes;
    writeDirEntry(rootDir, "HMDHIFI    ", 0x10u, 2u, 0u);

    // 4. /HMDHIFI/ Directory (cluster 2 -> sector 13)
    auto clusterOffset = [&](std::uint32_t cluster) -> std::size_t {
        const std::uint32_t sec = kCluster2Sector + (cluster - 2u) * kSectorsPerCluster;
        return static_cast<std::size_t>(sec) * kSectorBytes;
    };
    auto* hmdDir = img + clusterOffset(2u);
    writeDirEntry(hmdDir + 0 * 32, ".          ", 0x10u, 2u, 0u);
    writeDirEntry(hmdDir + 1 * 32, "..         ", 0x10u, 0u, 0u);
    writeDirEntry(hmdDir + 2 * 32, "ATDATA01HMA", 0x20u, 0u, 0u);
    writeDirEntry(hmdDir + 3 * 32, "TRKIDX01HMA", 0x20u, 3u, kTrkIdxBytes);
    writeDirEntry(hmdDir + 4 * 32, "MCLIST01HMA", 0x20u, 13u, kMcListBytes);

    // 5. TRKIDX01.HMA (clusters 3..12 = contiguous 327,680 bytes)
    auto* trkIdx = img + clusterOffset(3u);
    std::memcpy(trkIdx, "TIF ", 4);
    writeBe16(trkIdx + 0x0100, 0);
    // Track freelist: slot[i] -> next free slot at offset 38 (BE u16)
    for (std::uint16_t i = 0; i < 2047; ++i) {
        writeBe16(trkIdx + 0x8000u + static_cast<std::size_t>(i) * 0x50u + 38u,
                  static_cast<std::uint16_t>(i + 1u));
    }
    writeBe16(trkIdx + 0x8000u + 2047u * 0x50u + 38u, 0);
    // Fragment freelist: frag[i] -> next free frag at offset 14 (BE u16)
    for (std::uint16_t i = 0; i < 4095; ++i) {
        writeBe16(trkIdx + 0x30000u + static_cast<std::size_t>(i) * 0x10u + 14u,
                  static_cast<std::uint16_t>(i + 1u));
    }
    writeBe16(trkIdx + 0x30000u + 4095u * 0x10u + 14u, 0);
    // String chunk freelist: str[i] -> type 0, next link i+1 at offset 14 (BE u16)
    for (std::uint16_t i = 0; i < 4095; ++i) {
        writeBe16(trkIdx + 0x40000u + static_cast<std::size_t>(i) * 0x10u + 14u,
                  static_cast<std::uint16_t>(i + 1u));
    }
    writeBe16(trkIdx + 0x40000u + 4095u * 0x10u + 14u, 0);

    // 6. MCLIST01.HMA (cluster 13, 32,112 bytes)
    auto* mclist = img + clusterOffset(13u);
    std::memcpy(mclist, "MCLT", 4);
    (void)carta_himd_host_des_crypt(1, 1, 0, kEkb00010012RootKey.data(), 24, nullptr,
                                    g_mock.headKey.data(), 16, mclist + 0x10);
    writeBe32(mclist + 0x20, g_mock.currentGeneration);
    mclist[0x38] = 0x00;
    mclist[0x39] = 0x01;
    mclist[0x3A] = 0x00;
    mclist[0x3B] = 0x12;
    std::memcpy(mclist + 0x40, g_mock.discId.data(), 16);
    (void)carta_himd_host_des_crypt(1, 1, 0, kEkb00010012RootKey.data(), 24, nullptr,
                                    g_mock.bodyKey.data(), 16, mclist + 0x60);

    // 7. Initialize valid ICV & ICV header for generation 1
    std::array<std::uint8_t, 64> head{};
    writeBe32(head.data(), g_mock.currentGeneration);
    head[24] = 0x00;
    head[25] = 0x01;
    head[26] = 0x00;
    head[27] = 0x12;
    std::memcpy(head.data() + 32, g_mock.discId.data(), 16);
    std::vector<std::uint8_t> zeroMacs(32000, 0);
    std::array<std::uint8_t, 8> icvLo{};
    std::array<std::uint8_t, 8> icvHi{};
    (void)retailMac8(g_mock.headKey.data(), head.data(), head.size(), icvLo);
    (void)retailMac8(g_mock.bodyKey.data(), zeroMacs.data(), zeroMacs.size(), icvHi);
    std::memcpy(g_mock.currentIcv.data(), icvLo.data(), 8);
    std::memcpy(g_mock.currentIcv.data() + 8, icvHi.data(), 8);
    g_mock.currentIcvHeader.fill(0);
    writeBe32(g_mock.currentIcvHeader.data() + 4, g_mock.currentGeneration);
}

void resetMockStateLocked() {
    g_mock.open = false;
    g_mock.inSession = false;
    g_mock.stage1Done = false;
    g_mock.stage2Done = false;
    g_mock.stage3Done = false;
    g_mock.wipeBusyPollsRemaining = 0;
    g_mock.datanum = 1;
    g_mock.currentGeneration = 1;
    g_mock.authStartedCount = 0;
    g_mock.authCompletedCount = 0;
    g_mock.icvReadCount = 0;
    g_mock.icvWriteCount = 0;
    g_mock.sectorsRead = 0;
    g_mock.sectorsWritten = 0;
    g_mock.wipeCount = 0;
    g_mock.sleptMs = 0;
    g_mock.hostLeafId.fill(0);
    g_mock.hostNonce.fill(0);
    g_mock.deviceMac.fill(0);
    g_mock.sessionKey.fill(0);
    formatCleanMockImageLocked();
    g_mock.initialized = true;
}

void ensureMockInitLocked() {
    if (!g_mock.initialized) {
        resetMockStateLocked();
    }
}

int handleMockScsiCommandLocked(const std::uint8_t* cdb, int cdbLen, int direction,
                                const std::uint8_t* dataIn, int dataInLen, std::uint8_t* dataOut,
                                int dataOutMaxLen) {
    ensureMockInitLocked();
    if (!cdb || cdbLen < 6) {
        setErrorLocked("MockHiMd: invalid SCSI CDB length.");
        return -1;
    }

    const std::uint8_t op = cdb[0];

    // Hardware write confirmation guard check
    if (g_simulateRealGuardOnMock.load() && !g_hardwareWriteConfirmed.load() &&
        isMutatingHiMdScsiCommand(cdb, cdbLen)) {
        setErrorLocked(
            "Hardware write blocked: explicit confirmation is required before modifying a Hi-MD disc.");
        return -1;
    }

    switch (op) {
    case 0x00: // TEST UNIT READY
    case 0x1E: // PREVENT ALLOW MEDIUM REMOVAL
        return 0;

    case 0x03: { // REQUEST SENSE
        if (direction != 1 || !dataOut || dataOutMaxLen < 18) {
            setErrorLocked("MockHiMd: REQUEST SENSE invalid buffer.");
            return -1;
        }
        const int n = std::min(dataOutMaxLen, 18);
        std::memset(dataOut, 0, static_cast<std::size_t>(n));
        dataOut[0] = 0x70;
        if (g_mock.wipeBusyPollsRemaining > 0) {
            --g_mock.wipeBusyPollsRemaining;
            dataOut[15] = 0x01;
            dataOut[16] = 0x00;
            dataOut[17] = 0x01;
        }
        return n;
    }

    case 0x12: { // INQUIRY
        if (direction != 1 || !dataOut || dataOutMaxLen < 36) {
            setErrorLocked("MockHiMd: INQUIRY invalid buffer.");
            return -1;
        }
        const int n = std::min(dataOutMaxLen, 36);
        std::memset(dataOut, 0, static_cast<std::size_t>(n));
        dataOut[0] = 0x00;
        dataOut[1] = 0x80; // removable
        dataOut[2] = 0x04;
        dataOut[3] = 0x02;
        dataOut[4] = 31;
        std::memcpy(dataOut + 8, "SONY    ", 8);
        std::memcpy(dataOut + 16, "Hi-MD           ", 16);
        std::memcpy(dataOut + 32, "1.00", 4);
        return n;
    }

    case 0x25: { // READ CAPACITY(10)
        if (direction != 1 || !dataOut || dataOutMaxLen < 8) {
            setErrorLocked("MockHiMd: READ CAPACITY invalid buffer.");
            return -1;
        }
        writeBe32(dataOut + 0, kTotalSectors - 1u);
        writeBe32(dataOut + 4, kSectorBytes);
        return 8;
    }

    case 0x28: { // READ(10)
        if (cdbLen < 10 || direction != 1 || !dataOut) {
            setErrorLocked("MockHiMd: invalid READ(10) arguments.");
            return -1;
        }
        const std::uint32_t lba = readBe32(cdb + 2);
        const std::uint32_t count = readBe16(cdb + 7);
        const std::size_t wantBytes = static_cast<std::size_t>(count) * kSectorBytes;
        if (lba + count > kTotalSectors || wantBytes > static_cast<std::size_t>(dataOutMaxLen)) {
            setErrorLocked("MockHiMd: READ(10) LBA/length out of bounds.");
            return -1;
        }
        if (wantBytes > 0) {
            std::memcpy(dataOut, g_mock.image.data() + static_cast<std::size_t>(lba) * kSectorBytes,
                        wantBytes);
        }
        g_mock.sectorsRead += static_cast<int>(count);
        return static_cast<int>(wantBytes);
    }

    case 0x2A: { // WRITE(10)
        if (cdbLen < 10 || direction != 2 || (dataInLen > 0 && !dataIn)) {
            setErrorLocked("MockHiMd: invalid WRITE(10) arguments.");
            return -1;
        }
        const std::uint32_t lba = readBe32(cdb + 2);
        const std::uint32_t count = readBe16(cdb + 7);
        const std::size_t needBytes = static_cast<std::size_t>(count) * kSectorBytes;
        if (lba + count > kTotalSectors || needBytes != static_cast<std::size_t>(dataInLen)) {
            setErrorLocked("MockHiMd: WRITE(10) LBA/length mismatch.");
            return -1;
        }
        if (needBytes > 0) {
            std::memcpy(g_mock.image.data() + static_cast<std::size_t>(lba) * kSectorBytes, dataIn,
                        needBytes);
        }
        g_mock.sectorsWritten += static_cast<int>(count);
        return static_cast<int>(needBytes);
    }

    case 0xA4: { // REPORT KEY (Sony Hi-MD DRM read)
        if (cdbLen < 12 || direction != 1 || !dataOut) {
            setErrorLocked("MockHiMd: invalid 0xA4 REPORT KEY arguments.");
            return -1;
        }
        const std::uint8_t formatClass = cdb[7];
        const std::uint16_t allocLen = readBe16(cdb + 8);
        const std::uint8_t param = cdb[10];
        if (formatClass != 0xBDu || allocLen > static_cast<std::uint16_t>(dataOutMaxLen)) {
            setErrorLocked("MockHiMd: unsupported 0xA4 format class or buffer size.");
            return -1;
        }
        std::memset(dataOut, 0, allocLen);
        if (param == 0x3Bu && allocLen >= 0x0A) { // getLeafID
            writeBe16(dataOut, 8);
            std::memcpy(dataOut + 2, g_mock.deviceLeafId.data(), 8);
            return allocLen;
        }
        if (param == 0x3Du && allocLen >= 0x12) { // getDiscID
            writeBe16(dataOut, 16);
            std::memcpy(dataOut + 2, g_mock.discId.data(), 16);
            return allocLen;
        }
        if (param == 0x31u && allocLen >= 0x43C) { // Auth Stage 2
            if (!g_mock.stage1Done) {
                setErrorLocked("MockHiMd: Auth Stage 2 requested before Stage 1.");
                return -1;
            }
            std::array<std::uint8_t, 32> macInput{};
            std::memcpy(macInput.data() + 0, g_mock.discId.data(), 16);
            std::memcpy(macInput.data() + 16, g_mock.hostNonce.data(), 8);
            std::memcpy(macInput.data() + 24, g_mock.deviceNonce.data(), 8);
            if (!retailMac8(kEkb00010012RootKey.data(), macInput.data(), macInput.size(),
                            g_mock.deviceMac)) {
                setErrorLocked("MockHiMd: failed computing stage 2 device MAC.");
                return -1;
            }
            writeBe16(dataOut, static_cast<std::uint16_t>(allocLen - 2));
            dataOut[2] = 0x00;
            dataOut[3] = 0x00;
            std::memcpy(dataOut + 4, g_mock.discId.data(), 16);
            std::memcpy(dataOut + 20, g_mock.deviceMac.data(), 8);
            std::memcpy(dataOut + 28, g_mock.deviceLeafId.data(), 8);
            std::memcpy(dataOut + 36, g_mock.deviceNonce.data(), 8);
            writeBe32(dataOut + 44, 0x00000001u); // keyType: one-device
            writeBe32(dataOut + 48, 0x00000009u); // keyLevel: 9
            writeBe32(dataOut + 52, 0x00010012u); // ekbid
            writeBe32(dataOut + 56, 0x00000000u);
            std::memcpy(dataOut + 60, kStage2ExpectedEkbKey.data(), 16);
            g_mock.stage2Done = true;
            return allocLen;
        }
        if (param == 0x33u && allocLen >= 0x404) { // Read ICV
            if (!g_mock.stage3Done) {
                setErrorLocked("MockHiMd: Read ICV requested before Stage 3 auth.");
                return -1;
            }
            std::array<std::uint8_t, 24> icvData{};
            std::memcpy(icvData.data() + 0, g_mock.currentIcvHeader.data(), 8);
            std::memcpy(icvData.data() + 8, g_mock.currentIcv.data(), 16);
            std::array<std::uint8_t, 8> icvMac{};
            if (!createIcvMac8(g_mock.sessionKey.data(), icvData.data(), icvMac)) {
                setErrorLocked("MockHiMd: failed computing readICV MAC.");
                return -1;
            }
            writeBe16(dataOut, static_cast<std::uint16_t>(allocLen - 2));
            dataOut[2] = 0x00;
            dataOut[3] = 0x00;
            std::memcpy(dataOut + 4, g_mock.currentIcvHeader.data(), 8);
            std::memcpy(dataOut + 12, g_mock.currentIcv.data(), 16);
            std::memcpy(dataOut + 28, icvMac.data(), 8);
            g_mock.inSession = true;
            ++g_mock.icvReadCount;
            return allocLen;
        }
        setErrorLocked("MockHiMd: unsupported 0xA4 REPORT KEY parameter.");
        return -1;
    }

    case 0xA3: { // SEND KEY (Sony Hi-MD DRM write)
        if (cdbLen < 12 || direction != 2 || !dataIn) {
            setErrorLocked("MockHiMd: invalid 0xA3 SEND KEY arguments.");
            return -1;
        }
        const std::uint8_t formatClass = cdb[7];
        const std::uint8_t param = cdb[10];
        if (formatClass != 0xBDu) {
            setErrorLocked("MockHiMd: unsupported 0xA3 format class.");
            return -1;
        }
        if (param == 0x30u && dataInLen >= 20) { // Auth Stage 1: Host Leaf ID + Nonce
            std::memcpy(g_mock.hostLeafId.data(), dataIn + 4, 8);
            std::memcpy(g_mock.hostNonce.data(), dataIn + 12, 8);
            g_mock.stage1Done = true;
            g_mock.stage2Done = false;
            g_mock.stage3Done = false;
            ++g_mock.authStartedCount;
            return dataInLen;
        }
        if (param == 0x32u && dataInLen >= 12) { // Auth Stage 3: Verify host MAC & derive session key
            if (!g_mock.stage2Done) {
                setErrorLocked("MockHiMd: Auth Stage 3 sent before Stage 2.");
                return -1;
            }
            std::array<std::uint8_t, 32> hostMacIn{};
            std::memcpy(hostMacIn.data() + 0, g_mock.discId.data(), 16);
            std::memcpy(hostMacIn.data() + 16, g_mock.deviceNonce.data(), 8);
            std::memcpy(hostMacIn.data() + 24, g_mock.hostNonce.data(), 8);
            std::array<std::uint8_t, 8> expectedHostMac{};
            if (!retailMac8(kEkb00010012RootKey.data(), hostMacIn.data(), hostMacIn.size(),
                            expectedHostMac)) {
                setErrorLocked("MockHiMd: failed computing expected host MAC.");
                return -1;
            }
            if (std::memcmp(dataIn + 4, expectedHostMac.data(), 8) != 0) {
                setErrorLocked("MockHiMd: Auth Stage 3 host MAC verification failed.");
                return -1;
            }
            std::array<std::uint8_t, 32> sessIn{};
            std::memcpy(sessIn.data() + 0, g_mock.discId.data(), 16);
            std::memcpy(sessIn.data() + 16, g_mock.deviceMac.data(), 8);
            std::memcpy(sessIn.data() + 24, expectedHostMac.data(), 8);
            if (!retailMac8(kEkb00010012RootKey.data(), sessIn.data(), sessIn.size(),
                            g_mock.sessionKey)) {
                setErrorLocked("MockHiMd: failed deriving Hi-MD session key.");
                return -1;
            }
            g_mock.stage3Done = true;
            ++g_mock.authCompletedCount;
            return dataInLen;
        }
        if (param == 0x34u && dataInLen >= 36) { // Write ICV
            if (!g_mock.inSession || !g_mock.stage3Done) {
                setErrorLocked("MockHiMd: Write ICV sent outside active authenticated session.");
                return -1;
            }
            const std::uint8_t* hdr = dataIn + 4;
            const std::uint8_t* icv = dataIn + 12;
            const std::uint8_t* mac = dataIn + 28;
            if (hdr[1] != 0x20u) {
                setErrorLocked("MockHiMd: invalid Write ICV header flag.");
                return -1;
            }
            const std::uint32_t newGen = readBe32(hdr + 4);
            if (newGen != g_mock.currentGeneration + 1u) {
                setErrorLocked("MockHiMd: Write ICV generation did not increment monotonically.");
                return -1;
            }
            std::array<std::uint8_t, 24> verifyIn{};
            std::memcpy(verifyIn.data() + 0, hdr, 8);
            std::memcpy(verifyIn.data() + 8, icv, 16);
            std::array<std::uint8_t, 8> expectedIcvMac{};
            if (!createIcvMac8(g_mock.sessionKey.data(), verifyIn.data(), expectedIcvMac) ||
                std::memcmp(mac, expectedIcvMac.data(), 8) != 0) {
                setErrorLocked("MockHiMd: Write ICV MAC verification failed.");
                return -1;
            }
            g_mock.currentGeneration = newGen;
            g_mock.datanum = static_cast<int>(newGen % 16u);
            std::memcpy(g_mock.currentIcv.data(), icv, 16);
            g_mock.currentIcvHeader.fill(0);
            writeBe32(g_mock.currentIcvHeader.data() + 4, g_mock.currentGeneration);
            g_mock.inSession = false;
            ++g_mock.icvWriteCount;
            return dataInLen;
        }
        setErrorLocked("MockHiMd: unsupported 0xA3 SEND KEY parameter.");
        return -1;
    }

    case 0xC2: { // Sony Hi-MD vendor control (wipe / reformat / clock)
        const std::uint8_t sub = cdb[3];
        if (sub == 0x00u || sub == 0x01u) {
            const int prevWipeCount = g_mock.wipeCount;
            const int prevAuthStarted = g_mock.authStartedCount;
            const int prevAuthCompleted = g_mock.authCompletedCount;
            const int prevIcvRead = g_mock.icvReadCount;
            const int prevIcvWrite = g_mock.icvWriteCount;
            const int prevRead = g_mock.sectorsRead;
            const int prevWritten = g_mock.sectorsWritten;
            const std::int64_t prevSlept = g_mock.sleptMs;
            resetMockStateLocked();
            g_mock.open = true;
            g_mock.wipeBusyPollsRemaining = 1;
            g_mock.wipeCount = prevWipeCount + 1;
            g_mock.authStartedCount = prevAuthStarted;
            g_mock.authCompletedCount = prevAuthCompleted;
            g_mock.icvReadCount = prevIcvRead;
            g_mock.icvWriteCount = prevIcvWrite;
            g_mock.sectorsRead = prevRead;
            g_mock.sectorsWritten = prevWritten;
            g_mock.sleptMs = prevSlept;
            return 0;
        }
        if (sub == 0x50u && direction == 1 && dataOut && dataOutMaxLen >= 7) {
            std::memset(dataOut, 0, static_cast<std::size_t>(std::min(7, dataOutMaxLen)));
            return std::min(7, dataOutMaxLen);
        }
        return 0;
    }

    default:
        setErrorLocked("MockHiMd: unsupported SCSI opcode.");
        return -1;
    }
}

void notifyProgressCallback(const HiMdProgressSnapshot& snap, const HiMdProgressCallback& cb) {
    if (cb && snap.active) {
        cb(snap);
    }
}

} // namespace

void setHiMdProgress(const std::string& operation, const std::string& phase, double progress,
                     std::int64_t bytesDone, std::int64_t bytesTotal) {
    HiMdProgressSnapshot snapCopy;
    HiMdProgressCallback cbCopy;
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
        snapCopy = g_progress;
        cbCopy = g_progressCallback;
    }
    notifyProgressCallback(snapCopy, cbCopy);
}

void clearHiMdProgress() {
    std::lock_guard<std::mutex> lock(g_progressMutex);
    g_progress = {};
}

HiMdProgressSnapshot getHiMdProgress() {
    std::lock_guard<std::mutex> lock(g_progressMutex);
    return g_progress;
}

void setHiMdProgressCallback(HiMdProgressCallback cb) {
    std::lock_guard<std::mutex> lock(g_progressMutex);
    g_progressCallback = std::move(cb);
}

void setMockHiMdEnabled(bool enabled) {
    g_mockEnabled.store(enabled);
}

bool isMockHiMdEnabled() noexcept {
    return g_mockEnabled.load();
}

void resetMockHiMdState() {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    resetMockStateLocked();
}

HiMdMockSnapshot getMockHiMdSnapshot() {
    std::lock_guard<std::mutex> lock(g_hostMutex);
    ensureMockInitLocked();
    HiMdMockSnapshot snap;
    snap.enabled = g_mockEnabled.load();
    snap.open = g_mock.open;
    snap.inSession = g_mock.inSession;
    snap.datanum = g_mock.datanum;
    snap.currentGeneration = g_mock.currentGeneration;
    snap.authStartedCount = g_mock.authStartedCount;
    snap.authCompletedCount = g_mock.authCompletedCount;
    snap.icvReadCount = g_mock.icvReadCount;
    snap.icvWriteCount = g_mock.icvWriteCount;
    snap.sectorsRead = g_mock.sectorsRead;
    snap.sectorsWritten = g_mock.sectorsWritten;
    snap.wipeCount = g_mock.wipeCount;
    snap.sleptMs = g_mock.sleptMs;
    snap.discIdHex = toHex(g_mock.discId);
    snap.lastIcvHex = toHex(g_mock.currentIcv);
    snap.headKeyHex = toHex(g_mock.headKey);
    snap.bodyKeyHex = toHex(g_mock.bodyKey);
    return snap;
}

bool saveMockHiMdImage(const std::string& path, std::string& error) {
    std::vector<std::uint8_t> copy;
    {
        std::lock_guard<std::mutex> lock(g_hostMutex);
        ensureMockInitLocked();
        copy = g_mock.image;
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "Failed opening mock image output path: " + path;
        return false;
    }
    if (!copy.empty() && !f.write(reinterpret_cast<const char*>(copy.data()),
                                  static_cast<std::streamsize>(copy.size()))) {
        error = "Failed writing mock image to: " + path;
        return false;
    }
    return true;
}

bool loadMockHiMdImage(const std::string& path, std::string& error) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        error = "Failed opening mock image input path: " + path;
        return false;
    }
    const auto sz = f.tellg();
    const auto expected = static_cast<std::streamsize>(kTotalSectors) * kSectorBytes;
    if (sz != expected) {
        error = "Expected 33554432-byte Hi-MD FAT16 image";
        return false;
    }
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(expected));
    f.seekg(0, std::ios::beg);
    if (!f.read(reinterpret_cast<char*>(raw.data()), expected)) {
        error = "Failed reading mock image bytes";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_hostMutex);
    resetMockStateLocked();
    g_mock.image = std::move(raw);
    return true;
}

void setHiMdSimulateRealHardwareGuardOnMock(bool enabled) {
    g_simulateRealGuardOnMock.store(enabled);
}

bool isHiMdSimulateRealHardwareGuardOnMock() noexcept {
    return g_simulateRealGuardOnMock.load();
}

void setHiMdHardwareWriteConfirmed(bool confirmed) {
    g_hardwareWriteConfirmed.store(confirmed);
}

bool isHiMdHardwareWriteConfirmed() noexcept {
    return g_hardwareWriteConfirmed.load();
}

ScopedHiMdHardwareWriteConfirmation::ScopedHiMdHardwareWriteConfirmation(bool confirm)
    : previous_(g_hardwareWriteConfirmed.exchange(confirm)) {}

ScopedHiMdHardwareWriteConfirmation::~ScopedHiMdHardwareWriteConfirmation() {
    g_hardwareWriteConfirmed.store(previous_);
}

bool isHiMdMockDeviceId(const std::string& deviceId) noexcept {
    return deviceId == kMockDeviceId;
}

} // namespace carta

// ============================================================================
// Porffor Hi-MD Host C ABI Implementation
// ============================================================================

extern "C" {

CARTA_HIMD_HOST_EXPORT int carta_himd_host_des_crypt(int is_triple_des, int is_encrypt, int is_cbc,
                                                      const std::uint8_t* key_ptr, int key_len,
                                                      const std::uint8_t* iv_ptr,
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
        } else if (key_len >= 24) {
            sk1 = carta_des::makeSubkeys(key_ptr);
            sk2 = carta_des::makeSubkeys(key_ptr + 8);
            sk3 = carta_des::makeSubkeys(key_ptr + 16);
        } else {
            std::lock_guard<std::mutex> lock(carta::g_hostMutex);
            carta::setErrorLocked("3DES key must be 16 or 24 bytes.");
            return -1;
        }
    } else if (key_len >= 8) {
        // Note: himd-js may pass a 16-byte key to single-DES; CryptoJS uses only the first 8 bytes.
        sk1 = carta_des::makeSubkeys(key_ptr);
    } else {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked("DES key must be at least 8 bytes.");
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

CARTA_HIMD_HOST_EXPORT int carta_himd_host_random_bytes(std::uint8_t* out_ptr, int len) {
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

CARTA_HIMD_HOST_EXPORT int carta_himd_host_cp932(int to_utf8, const std::uint8_t* in_ptr,
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
                return -1;
            }
            if (errno == EILSEQ || errno == EINVAL) {
                if (dstLeft == 0) {
                    iconv_close(cd);
                    return -1;
                }
                *dst++ = '?';
                --dstLeft;
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

CARTA_HIMD_HOST_EXPORT void carta_himd_host_sleep_ms(int ms) {
    if (ms <= 0) {
        return;
    }
    bool hasRealOpen = false;
    {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::ensureMockInitLocked();
        for (const auto& kv : carta::g_handles) {
            if (!kv.second.isMock) {
                hasRealOpen = true;
                break;
            }
        }
        if (!hasRealOpen) {
            carta::g_mock.sleptMs += ms;
            return;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(std::clamp(ms, 0, 120000)));
}

CARTA_HIMD_HOST_EXPORT void carta_himd_host_report_step(int done, int total) {
    if (total <= 0) {
        return;
    }
    carta::HiMdProgressSnapshot snapCopy;
    carta::HiMdProgressCallback cbCopy;
    {
        std::lock_guard<std::mutex> lock(carta::g_progressMutex);
        if (!carta::g_progress.active) {
            return;
        }
        const double frac =
            std::clamp(static_cast<double>(done) / static_cast<double>(total), 0.0, 1.0);
        carta::g_progress.progress = frac;
        snapCopy = carta::g_progress;
        cbCopy = carta::g_progressCallback;
    }
    carta::notifyProgressCallback(snapCopy, cbCopy);
}

CARTA_HIMD_HOST_EXPORT double carta_himd_host_now_ms(void) {
    static const auto kMonotonicStart = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                     kMonotonicStart)
        .count();
}

CARTA_HIMD_HOST_EXPORT int carta_himd_host_file_open(const char* path_ptr, int write_mode) {
    if (!path_ptr || !*path_ptr) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked("Empty file path.");
        return -1;
    }
    std::FILE* f = std::fopen(path_ptr, write_mode ? "wb" : "rb");
    if (!f) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked(std::string("Failed opening file: ") + path_ptr);
        return -1;
    }
    std::lock_guard<std::mutex> fLock(carta::g_fileMutex);
    const int fd = carta::g_nextFileFd++;
    carta::g_openFiles[fd] = f;
    return fd;
}

CARTA_HIMD_HOST_EXPORT double carta_himd_host_file_size(int fd) {
    std::FILE* f = nullptr;
    {
        std::lock_guard<std::mutex> fLock(carta::g_fileMutex);
        auto it = carta::g_openFiles.find(fd);
        if (it == carta::g_openFiles.end()) {
            return -1.0;
        }
        f = it->second;
    }
#if defined(_WIN32)
    const __int64 cur = _ftelli64(f);
    if (_fseeki64(f, 0, SEEK_END) != 0) {
        return -1.0;
    }
    const __int64 end = _ftelli64(f);
    _fseeki64(f, cur, SEEK_SET);
    return static_cast<double>(end);
#else
    const long long cur = ftello(f);
    if (fseeko(f, 0, SEEK_END) != 0) {
        return -1.0;
    }
    const long long end = ftello(f);
    fseeko(f, cur, SEEK_SET);
    return static_cast<double>(end);
#endif
}

CARTA_HIMD_HOST_EXPORT int carta_himd_host_file_read(int fd, std::uint8_t* out_ptr, int max_len) {
    if (!out_ptr || max_len <= 0) {
        return max_len == 0 ? 0 : -1;
    }
    std::FILE* f = nullptr;
    {
        std::lock_guard<std::mutex> fLock(carta::g_fileMutex);
        auto it = carta::g_openFiles.find(fd);
        if (it == carta::g_openFiles.end()) {
            return -1;
        }
        f = it->second;
    }
    std::size_t total = 0;
    while (total < static_cast<std::size_t>(max_len)) {
        const std::size_t n =
            std::fread(out_ptr + total, 1, static_cast<std::size_t>(max_len) - total, f);
        total += n;
        if (n == 0) {
            if (std::ferror(f)) {
                std::lock_guard<std::mutex> lock(carta::g_hostMutex);
                carta::setErrorLocked("File read error.");
                return -1;
            }
            break; // EOF
        }
    }
    return static_cast<int>(total);
}

CARTA_HIMD_HOST_EXPORT int carta_himd_host_file_write(int fd, const std::uint8_t* data_ptr,
                                                      int len) {
    if (len < 0 || (len > 0 && !data_ptr)) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    std::FILE* f = nullptr;
    {
        std::lock_guard<std::mutex> fLock(carta::g_fileMutex);
        auto it = carta::g_openFiles.find(fd);
        if (it == carta::g_openFiles.end()) {
            return -1;
        }
        f = it->second;
    }
    const std::size_t written = std::fwrite(data_ptr, 1, static_cast<std::size_t>(len), f);
    if (written != static_cast<std::size_t>(len)) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked("File write error.");
        return -1;
    }
    return static_cast<int>(written);
}

CARTA_HIMD_HOST_EXPORT int carta_himd_host_file_close(int fd) {
    std::FILE* f = nullptr;
    {
        std::lock_guard<std::mutex> fLock(carta::g_fileMutex);
        auto it = carta::g_openFiles.find(fd);
        if (it == carta::g_openFiles.end()) {
            return -1;
        }
        f = it->second;
        carta::g_openFiles.erase(it);
    }
    if (f) {
        std::fclose(f);
    }
    return 0;
}

CARTA_HIMD_HOST_EXPORT int carta_himd_host_scsi_list(std::uint8_t* json_out, int json_cap) {
    if (!json_out || json_cap <= 0) {
        return -1;
    }
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    if (carta::g_mockEnabled.load()) {
        oss << "{\"id\":\"" << carta::kMockDeviceId << "\",\"vendorId\":" << carta::kMockVendorId
            << ",\"productId\":" << carta::kMockProductId << ",\"name\":\""
            << carta::jsonEscape(carta::kMockProductName)
            << "\",\"isMock\":true,\"transport\":\"mock\"}";
        first = false;
    }
    for (const auto& dev : carta::listPhysicalHiMdDevices()) {
        if (!first) {
            oss << ",";
        }
        first = false;
        oss << "{\"id\":\"" << carta::jsonEscape(dev.id) << "\",\"vendorId\":" << dev.vendorId
            << ",\"productId\":" << dev.productId << ",\"name\":\"" << carta::jsonEscape(dev.name)
            << "\",\"isMock\":false,\"transport\":\"" << carta::jsonEscape(dev.transport)
            << "\",\"systemPath\":\"" << carta::jsonEscape(dev.systemPath) << "\"}";
    }
    oss << "]";
    const std::string json = oss.str();
    if (static_cast<int>(json.size()) > json_cap) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked("SCSI device list JSON exceeds output buffer.");
        return -1;
    }
    std::memcpy(json_out, json.data(), json.size());
    return static_cast<int>(json.size());
}

CARTA_HIMD_HOST_EXPORT int carta_himd_host_scsi_open(const char* device_id) {
    if (!device_id || !*device_id) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked("Empty Hi-MD device id.");
        return -1;
    }
    const std::string targetId(device_id);
    if (targetId == carta::kMockDeviceId) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::ensureMockInitLocked();
        if (!carta::g_mockEnabled.load()) {
            carta::setErrorLocked("Mock Hi-MD device is disabled.");
            return -1;
        }
        carta::g_mock.open = true;
        const int h = carta::g_nextHandle++;
        carta::ScsiHandle entry;
        entry.isMock = true;
        entry.vendorId = carta::kMockVendorId;
        entry.productId = carta::kMockProductId;
        carta::g_handles[h] = entry;
        return h;
    }

    std::string physErr;
    std::unique_ptr<carta::PhysicalHiMdHandle> phys =
        carta::PhysicalHiMdHandle::open(targetId, physErr);
    if (!phys) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked(physErr.empty()
                                  ? ("Failed opening physical Hi-MD device: " + targetId)
                                  : physErr);
        return -1;
    }

    std::lock_guard<std::mutex> lock(carta::g_hostMutex);
    const int h = carta::g_nextHandle++;
    carta::ScsiHandle entry;
    entry.isMock = false;
    entry.physical = std::shared_ptr<carta::PhysicalHiMdHandle>(std::move(phys));
    carta::g_handles[h] = std::move(entry);
    return h;
}

CARTA_HIMD_HOST_EXPORT int carta_himd_host_scsi_close(int handle) {
    std::shared_ptr<carta::PhysicalHiMdHandle> physToClose;
    {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        auto it = carta::g_handles.find(handle);
        if (it == carta::g_handles.end()) {
            return -1;
        }
        if (it->second.isMock) {
            carta::g_mock.open = false;
        } else {
            physToClose = std::move(it->second.physical);
        }
        carta::g_handles.erase(it);
    }
    physToClose.reset();
    return 0;
}

CARTA_HIMD_HOST_EXPORT int carta_himd_host_scsi_reset(int handle) {
    std::shared_ptr<carta::PhysicalHiMdHandle> phys;
    {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        auto it = carta::g_handles.find(handle);
        if (it == carta::g_handles.end()) {
            return -1;
        }
        if (it->second.isMock) {
            return 0;
        }
        phys = it->second.physical;
    }
    if (!phys) {
        return -1;
    }
    std::string err;
    const int rc = phys->reset(err);
    if (rc < 0 && !err.empty()) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked(err);
    }
    return rc;
}

CARTA_HIMD_HOST_EXPORT int carta_himd_host_scsi_command(
    int handle, const std::uint8_t* cdb_ptr, int cdb_len, int direction,
    const std::uint8_t* data_in_ptr, int data_in_len, std::uint8_t* data_out_ptr,
    int data_out_max_len) {
    std::shared_ptr<carta::PhysicalHiMdHandle> phys;
    {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        auto it = carta::g_handles.find(handle);
        if (it == carta::g_handles.end()) {
            carta::setErrorLocked("Invalid Hi-MD SCSI handle.");
            return -1;
        }
        if (it->second.isMock) {
            return carta::handleMockScsiCommandLocked(cdb_ptr, cdb_len, direction, data_in_ptr,
                                                      data_in_len, data_out_ptr, data_out_max_len);
        }
        if (!carta::g_hardwareWriteConfirmed.load() &&
            carta::isMutatingHiMdScsiCommand(cdb_ptr, cdb_len)) {
            carta::setErrorLocked(
                "Hardware write blocked: explicit confirmation is required before modifying a Hi-MD disc.");
            return -1;
        }
        phys = it->second.physical;
    }
    if (!phys) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked("Physical Hi-MD SCSI handle is closed.");
        return -1;
    }
    std::string err;
    const int rc = phys->executeCommand(cdb_ptr, cdb_len, direction, data_in_ptr, data_in_len,
                                        data_out_ptr, data_out_max_len, err);
    if (rc < 0) {
        std::lock_guard<std::mutex> lock(carta::g_hostMutex);
        carta::setErrorLocked(err.empty() ? "Physical Hi-MD SCSI command failed." : err);
    }
    return rc;
}

CARTA_HIMD_HOST_EXPORT const char* carta_himd_host_last_error(void) {
    std::lock_guard<std::mutex> lock(carta::g_hostMutex);
    return carta::g_lastError.c_str();
}

} // extern "C"
