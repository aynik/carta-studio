/**
 * @file HiMdScsiTransport.h
 * Native OS SCSI pass-through and USB Mass Storage Bulk-Only Transport (BOT)
 * fallback for physical Sony Hi-MD hardware (`carta-himd`).
 *
 * Transport matrix:
 * - macOS: DiskArbitration unmount of whole BSD disk + IOKit
 *   `SCSITaskDeviceInterface` (`SCSITaskUserClient`) when attached, otherwise
 *   `libusb-1.0` BOT fallback (`IOSCSIPeripheralDevice` fallback diagnostics).
 * - Linux: `/sys/class/scsi_generic/sg*` (`SG_IO`) with mounted-volume check on
 *   sibling block device + `libusb-1.0` BOT fallback.
 * - Windows: `SetupAPI`/`CM_Get_Parent` -> `\\.\PhysicalDriveN`
 *   (`IOCTL_SCSI_PASS_THROUGH_DIRECT` with volume lock/dismount before
 *   mutating commands) + `libusb-1.0` WinUSB BOT fallback.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace carta {

// ============================================================================
// Known Hi-MD USB Devices & Pure Protocol Helpers
// ============================================================================

struct KnownHiMdProduct {
    std::uint16_t vendorId;
    std::uint16_t productId;
    const char* name;
};

/** Look up a known Hi-MD USB VID/PID pair, or nullptr for NetMD-only/other USB devices. */
const KnownHiMdProduct* findKnownHiMdProduct(std::uint16_t vendorId,
                                               std::uint16_t productId) noexcept;

/** Return all known Hi-MD USB VID/PID entries (matches `himd-js` `DevicesIds`). */
const KnownHiMdProduct* knownHiMdProducts(std::size_t& outCount) noexcept;

/** Return true if a SCSI CDB mutates disc media or triggers DRM/control writes. */
bool isMutatingHiMdScsiCommand(const std::uint8_t* cdb, int cdbLen) noexcept;

/**
 * Strip SFF-8070i 12-byte zero padding back to standard SCSI CDB length
 * (6-byte group 0, 10-byte group 1, 12-byte `0xA3`/`0xA4`) for OS SCSI layers.
 */
int normalizeOsScsiCdbLength(const std::uint8_t* cdb, int cdbLen) noexcept;

enum class ScsiSenseDisposition {
    Ok,
    RetryUnitAttention,
    RetryBecomingReady,
    NoMedium,
    WriteProtected,
    Fatal
};

struct ParsedScsiSense {
    std::uint8_t responseCode = 0;
    std::uint8_t senseKey = 0;
    std::uint8_t asc = 0;
    std::uint8_t ascq = 0;
    ScsiSenseDisposition disposition = ScsiSenseDisposition::Fatal;
};

/** Parse fixed (`0x70`/`0x71`) or descriptor (`0x72`/`0x73`) SCSI sense bytes. */
ParsedScsiSense parseAndClassifyScsiSense(const std::uint8_t* sense, std::size_t len) noexcept;

/** Format a human-readable SCSI sense error diagnostic. */
std::string formatScsiSenseError(const std::string& context, const ParsedScsiSense& sense);

constexpr std::size_t kDefaultMaxScsiTransferBytes = 65536u;

using RawScsiExecFn = std::function<int(const std::uint8_t* cdb, int cdbLen, int direction,
                                        const std::uint8_t* dataIn, int dataInLen,
                                        std::uint8_t* dataOut, int dataOutMaxLen,
                                        std::string& error)>;

/**
 * Split multi-sector `READ(10)` (`0x28`) / `WRITE(10)` (`0x2A`) transfers
 * exceeding `maxTransferBytes`, updating big-endian LBA and count per chunk.
 */
int executeChunkedScsiCommand(const std::uint8_t* cdb, int cdbLen, int direction,
                              const std::uint8_t* dataIn, int dataInLen, std::uint8_t* dataOut,
                              int dataOutMaxLen, std::size_t maxTransferBytes,
                              const RawScsiExecFn& execFn, std::string& error);

// ============================================================================
// USB Mass Storage Bulk-Only Transport (BOT) Wire Protocol & Session
// ============================================================================

/** Serialize a 31-byte USB Mass Storage CBW (`USBC`, little-endian fields). */
std::array<std::uint8_t, 31> buildUsbBotCbw(std::uint32_t tag, std::uint32_t dataTransferLen,
                                            int direction, std::uint8_t lun,
                                            const std::uint8_t* cdb, int cdbLen) noexcept;

struct UsbBotCsw {
    bool validSignature = false;
    std::uint32_t tag = 0;
    std::uint32_t dataResidue = 0;
    std::uint8_t status = 0xFFu;
};

/** Parse a 13-byte USB Mass Storage CSW (`USBS`, little-endian fields). */
UsbBotCsw parseUsbBotCsw(const std::uint8_t* buf, std::size_t len) noexcept;

enum class UsbPipeStatus { Ok, Stall, Timeout, IoError };

/** Abstract USB BOT bulk/control pipe used by `UsbBotSession` and unit tests. */
class IUsbBotPipe {
  public:
    virtual ~IUsbBotPipe() = default;
    virtual UsbPipeStatus bulkOut(const std::uint8_t* data, int len, int& actualOut,
                                  int timeoutMs, std::string& error) = 0;
    virtual UsbPipeStatus bulkIn(std::uint8_t* data, int maxLen, int& actualOut, int timeoutMs,
                                 std::string& error) = 0;
    virtual bool clearHaltIn(std::string& error) = 0;
    virtual bool clearHaltOut(std::string& error) = 0;
    virtual bool bomsReset(std::string& error) = 0;
    virtual void sleepMs(int ms) = 0;
};

/** Stateful USB Bulk-Only Transport SCSI session with STALL & sense recovery. */
class UsbBotSession {
  public:
    explicit UsbBotSession(IUsbBotPipe& pipe, std::uint8_t lun = 0) noexcept;

    bool reset(std::string& error);

    int executeCommand(const std::uint8_t* cdb, int cdbLen, int direction,
                       const std::uint8_t* dataIn, int dataInLen, std::uint8_t* dataOut,
                       int dataOutMaxLen, std::string& error);

  private:
    int runRawTransaction(const std::uint8_t* cdb, int cdbLen, int direction,
                          const std::uint8_t* dataIn, int dataInLen, std::uint8_t* dataOut,
                          int dataOutMaxLen, std::uint8_t& outCswStatus, bool& outRetryableTransport,
                          std::string& error);

    IUsbBotPipe& pipe_;
    std::uint8_t lun_ = 0;
    std::uint32_t nextTag_ = 1;
};

// ============================================================================
// Physical OS Hi-MD Device Discovery & Handle
// ============================================================================

struct HiMdPhysicalDeviceInfo {
    std::string id;
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    std::string name;
    std::string transport;
    std::string systemPath;
};

/** Enumerate physically attached Hi-MD devices across OS SCSI and USB BOT. */
std::vector<HiMdPhysicalDeviceInfo> listPhysicalHiMdDevices();

/** RAII transport handle for a physical Hi-MD SCSI/USB device. */
class PhysicalHiMdHandle {
  public:
    struct Impl;

    explicit PhysicalHiMdHandle(std::unique_ptr<Impl> impl) noexcept;
    ~PhysicalHiMdHandle();

    PhysicalHiMdHandle(const PhysicalHiMdHandle&) = delete;
    PhysicalHiMdHandle& operator=(const PhysicalHiMdHandle&) = delete;

    static std::unique_ptr<PhysicalHiMdHandle> open(const std::string& deviceId,
                                                    std::string& error);

    int reset(std::string& error);
    int executeCommand(const std::uint8_t* cdb, int cdbLen, int direction,
                       const std::uint8_t* dataIn, int dataInLen, std::uint8_t* dataOut,
                       int dataOutMaxLen, std::string& error);

  private:
    std::unique_ptr<Impl> impl_;
};

} // namespace carta
