/**
 * @file HiMdTransportTests.cpp
 * Pure C++ offline unit tests for OS SCSI pass-through helpers, CDB
 * normalization, sense classification, multi-chunk READ(10)/WRITE(10)
 * splitting, USB BOT CBW/CSW + STALL/sense retry state machine, and safe
 * live Hi-MD enumeration (zero JUCE, zero Porffor).
 */

#include "HiMdScsiTransport.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

namespace {

#define REQUIRE(cond, msg)                                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] " << (msg) << "\n";         \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)

std::uint32_t readBe32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::uint16_t readBe16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8) | p[1]);
}

std::array<std::uint8_t, 13> makeCswBytes(std::uint32_t tag, std::uint32_t residue,
                                          std::uint8_t status, bool validSig = true) {
    std::array<std::uint8_t, 13> out{};
    out[0] = validSig ? 0x55u : 0x00u;
    out[1] = 0x53u;
    out[2] = 0x42u;
    out[3] = 0x53u;
    out[4] = static_cast<std::uint8_t>(tag & 0xFFu);
    out[5] = static_cast<std::uint8_t>((tag >> 8) & 0xFFu);
    out[6] = static_cast<std::uint8_t>((tag >> 16) & 0xFFu);
    out[7] = static_cast<std::uint8_t>((tag >> 24) & 0xFFu);
    out[8] = static_cast<std::uint8_t>(residue & 0xFFu);
    out[9] = static_cast<std::uint8_t>((residue >> 8) & 0xFFu);
    out[10] = static_cast<std::uint8_t>((residue >> 16) & 0xFFu);
    out[11] = static_cast<std::uint8_t>((residue >> 24) & 0xFFu);
    out[12] = status;
    return out;
}

std::array<std::uint8_t, 18> makeFixedSense(std::uint8_t sk, std::uint8_t asc, std::uint8_t ascq,
                                            std::uint8_t sksv0 = 0, std::uint8_t sksv1 = 0,
                                            std::uint8_t sksv2 = 0) {
    std::array<std::uint8_t, 18> s{};
    s[0] = 0x70u;
    s[2] = sk & 0x0Fu;
    s[7] = 10u;
    s[12] = asc;
    s[13] = ascq;
    s[15] = sksv0;
    s[16] = sksv1;
    s[17] = sksv2;
    return s;
}

class MockUsbBotPipe final : public carta::IUsbBotPipe {
  public:
    struct InStep {
        carta::UsbPipeStatus status = carta::UsbPipeStatus::Ok;
        std::vector<std::uint8_t> data;
        bool useLastTagForCsw = false;
        std::uint8_t cswStatus = 0;
        std::uint32_t cswResidue = 0;
        bool badCswSig = false;
    };

    struct OutStep {
        carta::UsbPipeStatus status = carta::UsbPipeStatus::Ok;
    };

    std::deque<InStep> inQueue;
    std::deque<OutStep> outQueue;
    std::vector<std::array<std::uint8_t, 31>> recordedCbws;
    std::vector<std::vector<std::uint8_t>> recordedOutPayloads;
    int clearHaltInCount = 0;
    int clearHaltOutCount = 0;
    int bomsResetCount = 0;
    int sleptMsTotal = 0;
    std::uint32_t lastCbwTag = 0;

    carta::UsbPipeStatus bulkOut(const std::uint8_t* data, int len, int& actualOut, int /*timeoutMs*/,
                                 std::string& error) override {
        carta::UsbPipeStatus st = carta::UsbPipeStatus::Ok;
        if (!outQueue.empty()) {
            st = outQueue.front().status;
            outQueue.pop_front();
        }
        if (st == carta::UsbPipeStatus::Ok) {
            if (len == 31 && data[0] == 0x55u && data[1] == 0x53u && data[2] == 0x42u &&
                data[3] == 0x43u) {
                std::array<std::uint8_t, 31> cbw{};
                std::memcpy(cbw.data(), data, 31);
                recordedCbws.push_back(cbw);
                lastCbwTag = static_cast<std::uint32_t>(data[4]) |
                             (static_cast<std::uint32_t>(data[5]) << 8) |
                             (static_cast<std::uint32_t>(data[6]) << 16) |
                             (static_cast<std::uint32_t>(data[7]) << 24);
            } else {
                recordedOutPayloads.emplace_back(data, data + len);
            }
            actualOut = len;
            return st;
        }
        actualOut = 0;
        error = "mock bulkOut error";
        return st;
    }

    carta::UsbPipeStatus bulkIn(std::uint8_t* data, int maxLen, int& actualOut, int /*timeoutMs*/,
                                std::string& error) override {
        if (inQueue.empty()) {
            actualOut = 0;
            error = "mock inQueue underflow";
            return carta::UsbPipeStatus::IoError;
        }
        InStep step = std::move(inQueue.front());
        inQueue.pop_front();
        if (step.status != carta::UsbPipeStatus::Ok) {
            actualOut = 0;
            error = "mock bulkIn non-OK";
            return step.status;
        }
        if (step.useLastTagForCsw) {
            auto csw = makeCswBytes(lastCbwTag, step.cswResidue, step.cswStatus, !step.badCswSig);
            const int n = std::min(maxLen, static_cast<int>(csw.size()));
            std::memcpy(data, csw.data(), static_cast<std::size_t>(n));
            actualOut = n;
            return carta::UsbPipeStatus::Ok;
        }
        const int n = std::min(maxLen, static_cast<int>(step.data.size()));
        if (n > 0) {
            std::memcpy(data, step.data.data(), static_cast<std::size_t>(n));
        }
        actualOut = n;
        return carta::UsbPipeStatus::Ok;
    }

    bool clearHaltIn(std::string& /*error*/) override {
        ++clearHaltInCount;
        return true;
    }
    bool clearHaltOut(std::string& /*error*/) override {
        ++clearHaltOutCount;
        return true;
    }
    bool bomsReset(std::string& /*error*/) override {
        ++bomsResetCount;
        return true;
    }
    void sleepMs(int ms) override { sleptMsTotal += ms; }
};

int testKnownHiMdProducts() {
    std::size_t count = 0;
    const auto* table = carta::knownHiMdProducts(count);
    REQUIRE(count == 14, "Expected all 14 Hi-MD VID/PID entries from himd-js");
    for (std::size_t i = 0; i < count; ++i) {
        const auto* hit = carta::findKnownHiMdProduct(table[i].vendorId, table[i].productId);
        REQUIRE(hit != nullptr && std::strcmp(hit->name, table[i].name) == 0,
                "Lookup mismatch in knownHiMdProducts");
    }
    REQUIRE(carta::findKnownHiMdProduct(0x054cu, 0x0287u) != nullptr, "Missing MZ-RH1");
    REQUIRE(carta::findKnownHiMdProduct(0x054cu, 0x017fu) != nullptr, "Missing MZ-NH1");
    REQUIRE(carta::findKnownHiMdProduct(0x0411u, 0x007fu) != nullptr, "Missing Buffalo MD-HUSB");
    // NetMD-only and non-Hi-MD devices must be excluded
    REQUIRE(carta::findKnownHiMdProduct(0x054cu, 0x0084u) == nullptr,
            "MZ-N505 NetMD must NOT match Hi-MD table");
    REQUIRE(carta::findKnownHiMdProduct(0x054cu, 0x0188u) == nullptr,
            "MZ-N920 NetMD must NOT match Hi-MD table");
    REQUIRE(carta::findKnownHiMdProduct(0x0781u, 0x5580u) == nullptr,
            "Generic USB flash drive must NOT match Hi-MD table");
    return 0;
}

int testMutatingScsiCommandGuard() {
    const std::uint8_t tur[12] = {0x00};
    const std::uint8_t reqSense[12] = {0x03, 0, 0, 0, 0x12};
    const std::uint8_t inquiry[12] = {0x12, 0, 0, 0, 0x24};
    const std::uint8_t prevent[12] = {0x1E, 0, 0, 0, 0xFF};
    const std::uint8_t readCap[12] = {0x25};
    const std::uint8_t read10[12] = {0x28, 0, 0, 0, 0, 0, 0, 0, 1};
    const std::uint8_t write10[12] = {0x2A, 0, 0, 0, 0, 0, 0, 0, 1};
    const std::uint8_t sendKey[16] = {0xA3, 0, 0, 0, 0, 0, 0, 0xBD, 0, 0x12, 0x30};
    const std::uint8_t reportKey[16] = {0xA4, 0, 0, 0, 0, 0, 0, 0xBD, 0, 0x12, 0x3D};
    const std::uint8_t sonyGetTime[12] = {0xC2, 0x00, 0x00, 0x50, 0, 0, 0, 0, 7};
    const std::uint8_t sonyWipe[12] = {0xC2, 0x00, 0x00, 0x00, 0x03};
    const std::uint8_t sonyReformat[12] = {0xC2, 0x00, 0x00, 0x01, 0x03};
    const std::uint8_t sonySetTime[16] = {0xC2, 0x00, 0x00, 0x90, 0};

    REQUIRE(!carta::isMutatingHiMdScsiCommand(tur, 12), "TUR should not be mutating");
    REQUIRE(!carta::isMutatingHiMdScsiCommand(reqSense, 12), "REQUEST SENSE should not be mutating");
    REQUIRE(!carta::isMutatingHiMdScsiCommand(inquiry, 12), "INQUIRY should not be mutating");
    REQUIRE(!carta::isMutatingHiMdScsiCommand(prevent, 12), "PREVENT/ALLOW should not be mutating");
    REQUIRE(!carta::isMutatingHiMdScsiCommand(readCap, 12), "READ CAPACITY should not be mutating");
    REQUIRE(!carta::isMutatingHiMdScsiCommand(read10, 12), "READ(10) should not be mutating");
    REQUIRE(!carta::isMutatingHiMdScsiCommand(reportKey, 16), "REPORT KEY 0xA4 should not be mutating");
    REQUIRE(!carta::isMutatingHiMdScsiCommand(sonyGetTime, 12), "Sony getTime should not be mutating");

    REQUIRE(carta::isMutatingHiMdScsiCommand(write10, 12), "WRITE(10) must be mutating");
    REQUIRE(carta::isMutatingHiMdScsiCommand(sendKey, 16), "SEND KEY 0xA3 must be mutating");
    REQUIRE(carta::isMutatingHiMdScsiCommand(sonyWipe, 12), "Sony wipe 0xC2 must be mutating");
    REQUIRE(carta::isMutatingHiMdScsiCommand(sonyReformat, 12), "Sony reformat 0xC2 must be mutating");
    REQUIRE(carta::isMutatingHiMdScsiCommand(sonySetTime, 16), "Sony setTime 0xC2 must be mutating");
    REQUIRE(carta::isMutatingHiMdScsiCommand(nullptr, 0), "Empty/null CDB must fail closed");
    return 0;
}

int testNormalizeCdbLength() {
    const std::uint8_t pad6[12] = {0x12, 0x00, 0x00, 0x00, 0x24, 0x00, 0, 0, 0, 0, 0, 0};
    const std::uint8_t pad10[12] = {0x28, 0x00, 0, 0, 0, 1, 0x00, 0x00, 0x04, 0x00, 0, 0};
    const std::uint8_t nonZeroTail12[12] = {0x28, 0, 0, 0, 0, 1, 0, 0, 4, 0, 0, 1};
    const std::uint8_t pad12A4[16] = {0xA4, 0, 0, 0, 0, 0, 0, 0xBD, 0x04, 0x3C, 0x31, 0, 0, 0, 0, 0};
    const std::uint8_t full16[16] = {0xA3, 0, 0, 0, 0, 0, 0, 0xBD, 0, 0x12, 0x30, 0, 1, 0, 0, 0};
    const std::uint8_t c2[12] = {0xC2, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0};

    REQUIRE(carta::normalizeOsScsiCdbLength(pad6, 12) == 6, "Expected INQUIRY 12->6");
    REQUIRE(carta::normalizeOsScsiCdbLength(pad10, 12) == 10, "Expected READ(10) 12->10");
    REQUIRE(carta::normalizeOsScsiCdbLength(nonZeroTail12, 12) == 12,
            "Non-zero bytes 10..11 must keep length 12");
    REQUIRE(carta::normalizeOsScsiCdbLength(pad12A4, 16) == 12, "Expected 0xA4 16->12");
    REQUIRE(carta::normalizeOsScsiCdbLength(full16, 16) == 16, "Non-zero 16-byte CDB stays 16");
    REQUIRE(carta::normalizeOsScsiCdbLength(c2, 12) == 12, "Sony 0xC2 stays 12");
    return 0;
}

int testScsiSenseParsing() {
    const auto okSense = makeFixedSense(0x00, 0x00, 0x00);
    const auto uaSense = makeFixedSense(0x06, 0x28, 0x00);
    const auto readySense = makeFixedSense(0x02, 0x04, 0x01);
    const auto noMedSense = makeFixedSense(0x02, 0x3A, 0x00);
    const auto wpSense = makeFixedSense(0x07, 0x27, 0x00);
    const auto fatalSense = makeFixedSense(0x05, 0x24, 0x00);
    const std::uint8_t descUa[8] = {0x72, 0x06, 0x29, 0x00, 0, 0, 0, 0};

    REQUIRE(carta::parseAndClassifyScsiSense(okSense.data(), okSense.size()).disposition ==
                carta::ScsiSenseDisposition::Ok,
            "SK 0x00 -> Ok");
    REQUIRE(carta::parseAndClassifyScsiSense(uaSense.data(), uaSense.size()).disposition ==
                carta::ScsiSenseDisposition::RetryUnitAttention,
            "SK 0x06 -> RetryUnitAttention");
    REQUIRE(carta::parseAndClassifyScsiSense(readySense.data(), readySense.size()).disposition ==
                carta::ScsiSenseDisposition::RetryBecomingReady,
            "SK 0x02/ASC 0x04 -> RetryBecomingReady");
    const auto noMed = carta::parseAndClassifyScsiSense(noMedSense.data(), noMedSense.size());
    REQUIRE(noMed.disposition == carta::ScsiSenseDisposition::NoMedium &&
                carta::formatScsiSenseError("Test", noMed).find("no Hi-MD disc") != std::string::npos,
            "SK 0x02/ASC 0x3A -> NoMedium");
    const auto wp = carta::parseAndClassifyScsiSense(wpSense.data(), wpSense.size());
    REQUIRE(wp.disposition == carta::ScsiSenseDisposition::WriteProtected &&
                carta::formatScsiSenseError("Test", wp).find("write-protected") != std::string::npos,
            "SK 0x07 -> WriteProtected");
    REQUIRE(carta::parseAndClassifyScsiSense(fatalSense.data(), fatalSense.size()).disposition ==
                carta::ScsiSenseDisposition::Fatal,
            "SK 0x05 -> Fatal");
    REQUIRE(carta::parseAndClassifyScsiSense(descUa, sizeof(descUa)).disposition ==
                carta::ScsiSenseDisposition::RetryUnitAttention,
            "Descriptor format UA -> RetryUnitAttention");
    return 0;
}

int testChunkedScsiReadWrite() {
    // 1. READ(10) of 160 x 2048-byte sectors (320 KiB) split into 5 x 32-sector (64 KiB) chunks
    {
        const std::uint8_t readCdb[10] = {0x28, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 160, 0};
        std::vector<std::uint8_t> out(160u * 2048u, 0);
        struct ChunkCall {
            std::uint32_t lba;
            std::uint16_t count;
            int bytes;
        };
        std::vector<ChunkCall> calls;
        std::string err;
        const int rc = carta::executeChunkedScsiCommand(
            readCdb, 10, 1, nullptr, 0, out.data(), static_cast<int>(out.size()),
            carta::kDefaultMaxScsiTransferBytes,
            [&](const std::uint8_t* cdb, int cdbLen, int dir, const std::uint8_t* /*in*/,
                int /*inLen*/, std::uint8_t* dst, int maxLen, std::string& /*e*/) {
                if (cdbLen != 10 || dir != 1) {
                    return -1;
                }
                const std::uint32_t lba = readBe32(cdb + 2);
                const std::uint16_t cnt = readBe16(cdb + 7);
                calls.push_back({lba, cnt, maxLen});
                for (std::uint16_t s = 0; s < cnt; ++s) {
                    const auto tag = static_cast<std::uint8_t>((lba + s) & 0xFFu);
                    std::memset(dst + static_cast<std::size_t>(s) * 2048u, tag, 2048u);
                }
                return maxLen;
            },
            err);
        REQUIRE(rc == static_cast<int>(out.size()), "Expected full 320 KiB read return");
        REQUIRE(calls.size() == 5, "Expected 5 chunks of 32 sectors");
        for (std::size_t i = 0; i < calls.size(); ++i) {
            REQUIRE(calls[i].lba == 0x10u + i * 32u && calls[i].count == 32u &&
                        calls[i].bytes == 65536,
                    "Chunk LBA/count mismatch in 160-sector READ(10)");
        }
        for (std::uint32_t s = 0; s < 160u; ++s) {
            REQUIRE(out[s * 2048u] == static_cast<std::uint8_t>((0x10u + s) & 0xFFu),
                    "Reassembled READ(10) sector payload mismatch");
        }
    }

    // 2. Non-multiple WRITE(10) of 75 sectors -> 32 + 32 + 11 sectors
    {
        const std::uint8_t writeCdb[10] = {0x2A, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 75, 0};
        std::vector<std::uint8_t> in(75u * 2048u);
        for (std::size_t i = 0; i < in.size(); ++i) {
            in[i] = static_cast<std::uint8_t>((i * 17u) & 0xFFu);
        }
        std::vector<std::uint8_t> reassembled;
        std::vector<std::uint16_t> counts;
        std::string err;
        const int rc = carta::executeChunkedScsiCommand(
            writeCdb, 10, 2, in.data(), static_cast<int>(in.size()), nullptr, 0,
            carta::kDefaultMaxScsiTransferBytes,
            [&](const std::uint8_t* cdb, int /*len*/, int dir, const std::uint8_t* src,
                int srcLen, std::uint8_t* /*dst*/, int /*maxLen*/, std::string& /*e*/) {
                if (dir != 2) {
                    return -1;
                }
                counts.push_back(readBe16(cdb + 7));
                reassembled.insert(reassembled.end(), src, src + srcLen);
                return srcLen;
            },
            err);
        REQUIRE(rc == static_cast<int>(in.size()), "Expected full 75-sector write return");
        REQUIRE((counts == std::vector<std::uint16_t>{32, 32, 11}),
                "Expected 32+32+11 sector write split");
        REQUIRE(reassembled == in, "Chunked WRITE(10) data mismatch");
    }

    // 3. Large non-read/write vendor transfer passes straight through without chunking
    {
        const std::uint8_t reportCdb[12] = {0xA4, 0, 0, 0, 0, 0, 0, 0xBD, 0x04, 0x04, 0x33, 0};
        std::vector<std::uint8_t> out(0x404, 0);
        int callCount = 0;
        std::string err;
        const int rc = carta::executeChunkedScsiCommand(
            reportCdb, 12, 1, nullptr, 0, out.data(), static_cast<int>(out.size()), 512,
            [&](const std::uint8_t* /*cdb*/, int /*len*/, int /*dir*/, const std::uint8_t* /*in*/,
                int /*inLen*/, std::uint8_t* /*dst*/, int maxLen, std::string& /*e*/) {
                ++callCount;
                return maxLen;
            },
            err);
        REQUIRE(callCount == 1 && rc == 0x404, "Non-block opcodes must pass through unchunked");
    }
    return 0;
}

int testUsbBotWireAndRecovery() {
    // 1. CBW/CSW wire layout verification
    const std::uint8_t cdb[10] = {0x28, 0, 0, 0, 0, 5, 0, 0, 2, 0};
    const auto cbw = carta::buildUsbBotCbw(0x11223344u, 4096u, 1, 0, cdb, 10);
    REQUIRE(cbw[0] == 0x55 && cbw[1] == 0x53 && cbw[2] == 0x42 && cbw[3] == 0x43,
            "CBW magic mismatch");
    REQUIRE(cbw[4] == 0x44 && cbw[5] == 0x33 && cbw[6] == 0x22 && cbw[7] == 0x11,
            "CBW little-endian tag mismatch");
    REQUIRE(cbw[8] == 0x00 && cbw[9] == 0x10 && cbw[10] == 0x00 && cbw[11] == 0x00,
            "CBW little-endian dataTransferLen mismatch");
    REQUIRE(cbw[12] == 0x80 && cbw[13] == 0 && cbw[14] == 10, "CBW flags/LUN/length mismatch");

    // 2. Bulk IN STALL -> clearHaltIn -> CSW=1 -> REQUEST SENSE UNIT ATTENTION -> retry -> OK
    {
        MockUsbBotPipe pipe;
        carta::UsbBotSession sess(pipe, 0);
        // Attempt 1 INQUIRY: bulkIn data STALL -> CSW status=1 -> REQUEST SENSE returns SK=0x06
        pipe.inQueue.push_back({carta::UsbPipeStatus::Stall, {}, false, 0, 0, false});
        pipe.inQueue.push_back({carta::UsbPipeStatus::Ok, {}, true, 1, 36, false});
        const auto ua = makeFixedSense(0x06, 0x28, 0x00);
        pipe.inQueue.push_back({carta::UsbPipeStatus::Ok,
                                std::vector<std::uint8_t>(ua.begin(), ua.end()), false, 0, 0, false});
        pipe.inQueue.push_back({carta::UsbPipeStatus::Ok, {}, true, 0, 0, false});
        // Attempt 2 INQUIRY: succeeds with 36 bytes + CSW status=0
        std::vector<std::uint8_t> inqReply(36, 0x41);
        pipe.inQueue.push_back({carta::UsbPipeStatus::Ok, inqReply, false, 0, 0, false});
        pipe.inQueue.push_back({carta::UsbPipeStatus::Ok, {}, true, 0, 0, false});

        std::uint8_t inqBuf[12] = {0x12, 0, 0, 0, 36};
        std::vector<std::uint8_t> out(36, 0);
        std::string err;
        const int rc = sess.executeCommand(inqBuf, 12, 1, nullptr, 0, out.data(), 36, err);
        REQUIRE(rc == 36, "Expected BOT retry after STALL+UA to succeed: " + err);
        REQUIRE(pipe.clearHaltInCount == 1, "Expected 1 clearHaltIn after data STALL");
        REQUIRE(pipe.recordedCbws.size() == 3, "Expected CBWs: INQUIRY, REQUEST SENSE, INQUIRY");
        REQUIRE(pipe.recordedCbws[1][15] == 0x03u, "Expected internal REQUEST SENSE command");
    }

    // 3. CSW Phase Error (status=2) -> bomsReset + retry -> OK
    {
        MockUsbBotPipe pipe;
        carta::UsbBotSession sess(pipe, 0);
        pipe.inQueue.push_back({carta::UsbPipeStatus::Ok, {}, true, 2, 0, false});
        pipe.inQueue.push_back({carta::UsbPipeStatus::Ok, {}, true, 0, 0, false});
        const std::uint8_t tur[12] = {0x00};
        std::string err;
        const int rc = sess.executeCommand(tur, 12, 0, nullptr, 0, nullptr, 0, err);
        REQUIRE(rc == 0 && pipe.bomsResetCount == 1, "Expected BOMSR recovery on Phase Error");
    }

    // 4. Write-protected CHECK CONDITION returns clear error immediately
    {
        MockUsbBotPipe pipe;
        carta::UsbBotSession sess(pipe, 0);
        pipe.inQueue.push_back({carta::UsbPipeStatus::Ok, {}, true, 1, 0, false});
        const auto wp = makeFixedSense(0x07, 0x27, 0x00);
        pipe.inQueue.push_back({carta::UsbPipeStatus::Ok,
                                std::vector<std::uint8_t>(wp.begin(), wp.end()), false, 0, 0, false});
        pipe.inQueue.push_back({carta::UsbPipeStatus::Ok, {}, true, 0, 0, false});
        const std::uint8_t w10[10] = {0x2A, 0, 0, 0, 0, 1, 0, 0, 1, 0};
        std::vector<std::uint8_t> sec(2048, 0xAA);
        std::string err;
        const int rc = sess.executeCommand(w10, 10, 2, sec.data(), 2048, nullptr, 0, err);
        REQUIRE(rc < 0 && err.find("write-protected") != std::string::npos,
                "Expected write-protected error message, got: " + err);
    }

    // 5. CSW tag mismatch must fail cleanly
    {
        MockUsbBotPipe pipe;
        carta::UsbBotSession sess(pipe, 0);
        const auto badCsw = makeCswBytes(9999u, 0, 0, true);
        pipe.inQueue.push_back({carta::UsbPipeStatus::Ok,
                                std::vector<std::uint8_t>(badCsw.begin(), badCsw.end()), false, 0, 0,
                                false});
        const std::uint8_t tur[12] = {0x00};
        std::string err;
        const int rc = sess.executeCommand(tur, 12, 0, nullptr, 0, nullptr, 0, err);
        REQUIRE(rc < 0 && err.find("invalid CSW") != std::string::npos,
                "Expected invalid CSW error on tag mismatch, got: " + err);
    }

    // 6. Explicit REQUEST SENSE (0x03) passes raw bytes (progress bytes 15..17) straight through
    {
        MockUsbBotPipe pipe;
        carta::UsbBotSession sess(pipe, 0);
        const auto rawSense = makeFixedSense(0x02, 0x04, 0x07, 0x80u, 0x12u, 0x34u);
        pipe.inQueue.push_back({carta::UsbPipeStatus::Ok,
                                std::vector<std::uint8_t>(rawSense.begin(), rawSense.end()), false,
                                0, 0, false});
        pipe.inQueue.push_back({carta::UsbPipeStatus::Ok, {}, true, 0, 0, false});
        const std::uint8_t rsCdb[12] = {0x03, 0, 0, 0, 18};
        std::array<std::uint8_t, 18> got{};
        std::string err;
        const int rc = sess.executeCommand(rsCdb, 12, 1, nullptr, 0, got.data(), 18, err);
        REQUIRE(rc == 18 && got[15] == 0x80u && got[16] == 0x12u && got[17] == 0x34u,
                "Explicit REQUEST SENSE progress bytes must pass through");
    }

    // 7. Chunked READ(10) over UsbBotSession splits 160 sectors (320 KiB) into 5 x 64 KiB USB BOT ops
    {
        MockUsbBotPipe pipe;
        carta::UsbBotSession sess(pipe, 0);
        for (int chunk = 0; chunk < 5; ++chunk) {
            std::vector<std::uint8_t> chunkData(32u * 2048u);
            for (int s = 0; s < 32; ++s) {
                std::memset(chunkData.data() + static_cast<std::size_t>(s) * 2048u,
                            static_cast<std::uint8_t>((chunk * 32 + s + 10) & 0xFF), 2048u);
            }
            pipe.inQueue.push_back({carta::UsbPipeStatus::Ok, std::move(chunkData), false, 0, 0, false});
            pipe.inQueue.push_back({carta::UsbPipeStatus::Ok, {}, true, 0, 0, false});
        }
        const std::uint8_t readCdb[10] = {0x28, 0, 0, 0, 0, 10, 0, 0, 160, 0};
        std::vector<std::uint8_t> out(160u * 2048u, 0);
        std::string err;
        const int rc = carta::executeChunkedScsiCommand(
            readCdb, 10, 1, nullptr, 0, out.data(), static_cast<int>(out.size()),
            carta::kDefaultMaxScsiTransferBytes,
            [&](const std::uint8_t* cdbPtr, int cdbLen, int dir, const std::uint8_t* in,
                int inLen, std::uint8_t* dst, int maxLen, std::string& e) {
                return sess.executeCommand(cdbPtr, cdbLen, dir, in, inLen, dst, maxLen, e);
            },
            err);
        REQUIRE(rc == static_cast<int>(out.size()),
                "Chunked UsbBotSession READ(10) failed: " + err);
        REQUIRE(pipe.recordedCbws.size() == 5,
                "Expected 5 separate 64 KiB USB BOT transactions for 160 sectors");
        for (int chunk = 0; chunk < 5; ++chunk) {
            REQUIRE(readBe32(pipe.recordedCbws[static_cast<std::size_t>(chunk)].data() + 15 + 2) ==
                            static_cast<std::uint32_t>(10 + chunk * 32) &&
                        readBe16(pipe.recordedCbws[static_cast<std::size_t>(chunk)].data() + 15 +
                                 7) == 32u,
                    "Sequential chunk LBA/count mismatch in USB BOT CBWs");
        }
        REQUIRE(out[0] == 10u && out[159u * 2048u] == static_cast<std::uint8_t>((159 + 10) & 0xFF),
                "Chunked USB BOT reassembled payload mismatch");
    }

    return 0;
}

int testLiveDiscoveryReadOnly() {
    const auto devs = carta::listPhysicalHiMdDevices();
    for (const auto& d : devs) {
        REQUIRE(!d.id.empty() && !d.transport.empty(), "Physical device entry must have id/transport");
        REQUIRE(d.vendorId != 0x054cu || d.productId != 0x0084u,
                "Connected NetMD MZ-N505 must never be reported as a Hi-MD device");
    }

    std::string err;
    auto bad = carta::PhysicalHiMdHandle::open("usb:999:999", err);
    REQUIRE(bad == nullptr && !err.empty(),
            "Opening non-existent physical device must fail cleanly with error");
    return 0;
}

} // namespace

int main() {
    if (testKnownHiMdProducts() != 0) return 1;
    if (testMutatingScsiCommandGuard() != 0) return 1;
    if (testNormalizeCdbLength() != 0) return 1;
    if (testScsiSenseParsing() != 0) return 1;
    if (testChunkedScsiReadWrite() != 0) return 1;
    if (testUsbBotWireAndRecovery() != 0) return 1;
    if (testLiveDiscoveryReadOnly() != 0) return 1;
    std::cout << "[HiMdTransportTests] All OS SCSI & USB BOT transport tests passed.\n";
    return 0;
}
