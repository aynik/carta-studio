/**
 * @file HiMdHostBridge.h
 * Native C ABI host callbacks, file-chunk streaming, and Mock MZ-RH1 SCSI/FAT16
 * emulation for Porffor Hi-MD protocol execution (`himd-js` + `nufatfs`).
 *
 * Responsibilities:
 * - Exports `carta_himd_host_*` C symbols consumed by `libhimd_porffor`:
 *   DES/3DES & CSPRNG, iconv CP932/UTF-8 conversion, sleep/progress hooks,
 *   streaming host file I/O (so large PCM/ATRAC uploads stay O(1) in Porffor's
 *   bump arena), and SCSI command transport.
 * - Provides an offline 32 MiB (16,384 x 2,048-byte sectors) Mock Hi-MD SCSI
 *   device (`mock:mz-rh1`) formatted as FAT16 superfloppy with `/HMDHIFI/`
 *   (`ATDATA01.HMA`, `TRKIDX01.HMA`, `MCLIST01.HMA`) and full 3-stage Sony
 *   `0xA3`/`0xA4` mutual authentication + ICV MAC verification + `0xC2` wipe.
 * - Enforces hardware write safety: mutating SCSI opcodes (`0x2A` WRITE(10),
 *   `0xA3` SEND KEY, `0xC2` Hi-MD control) are blocked against real hardware (or
 *   mock with guard simulation enabled) unless explicit hardware confirmation is
 *   active.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace carta {

// ============================================================================
// Mock Inspection & Progress Snapshots
// ============================================================================

struct HiMdMockSnapshot {
    bool enabled = false;
    bool open = false;
    bool inSession = false;
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
    std::string discIdHex;
    std::string lastIcvHex;
    std::string headKeyHex;
    std::string bodyKeyHex;
};

struct HiMdProgressSnapshot {
    bool active = false;
    std::string operation;
    std::string phase;
    double progress = 0.0;
    std::int64_t bytesDone = 0;
    std::int64_t bytesTotal = 0;
};

// ============================================================================
// Progress Reporting
// ============================================================================

void setHiMdProgress(const std::string& operation, const std::string& phase, double progress,
                     std::int64_t bytesDone = 0, std::int64_t bytesTotal = 0);
void clearHiMdProgress();
HiMdProgressSnapshot getHiMdProgress();

using HiMdProgressCallback = std::function<void(const HiMdProgressSnapshot&)>;
void setHiMdProgressCallback(HiMdProgressCallback cb);

// ============================================================================
// Mock Device & Hardware Write Safety Controls
// ============================================================================

void setMockHiMdEnabled(bool enabled);
bool isMockHiMdEnabled() noexcept;

void resetMockHiMdState();
HiMdMockSnapshot getMockHiMdSnapshot();

bool saveMockHiMdImage(const std::string& path, std::string& error);
bool loadMockHiMdImage(const std::string& path, std::string& error);

void setHiMdSimulateRealHardwareGuardOnMock(bool enabled);
bool isHiMdSimulateRealHardwareGuardOnMock() noexcept;

void setHiMdHardwareWriteConfirmed(bool confirmed);
bool isHiMdHardwareWriteConfirmed() noexcept;

class ScopedHiMdHardwareWriteConfirmation {
  public:
    explicit ScopedHiMdHardwareWriteConfirmation(bool confirm = true);
    ~ScopedHiMdHardwareWriteConfirmation();
    ScopedHiMdHardwareWriteConfirmation(const ScopedHiMdHardwareWriteConfirmation&) = delete;
    ScopedHiMdHardwareWriteConfirmation& operator=(const ScopedHiMdHardwareWriteConfirmation&) =
        delete;

  private:
    bool previous_ = false;
};

bool isHiMdMockDeviceId(const std::string& deviceId) noexcept;

} // namespace carta

// ============================================================================
// Porffor Hi-MD Host C ABI
// ============================================================================

#if defined(_WIN32)
#define CARTA_HIMD_HOST_EXPORT
#else
#define CARTA_HIMD_HOST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

CARTA_HIMD_HOST_EXPORT int carta_himd_host_des_crypt(int is_triple_des, int is_encrypt, int is_cbc,
                                                      const std::uint8_t* key_ptr, int key_len,
                                                      const std::uint8_t* iv_ptr,
                                                      const std::uint8_t* in_ptr, int in_len,
                                                      std::uint8_t* out_ptr);

CARTA_HIMD_HOST_EXPORT int carta_himd_host_random_bytes(std::uint8_t* out_ptr, int len);

CARTA_HIMD_HOST_EXPORT int carta_himd_host_cp932(int to_utf8, const std::uint8_t* in_ptr,
                                                  int in_len, std::uint8_t* out_ptr, int out_cap);

CARTA_HIMD_HOST_EXPORT void carta_himd_host_sleep_ms(int ms);

CARTA_HIMD_HOST_EXPORT void carta_himd_host_report_step(int done, int total);

CARTA_HIMD_HOST_EXPORT double carta_himd_host_now_ms(void);

CARTA_HIMD_HOST_EXPORT int carta_himd_host_file_open(const char* path_ptr, int write_mode);

CARTA_HIMD_HOST_EXPORT double carta_himd_host_file_size(int fd);

CARTA_HIMD_HOST_EXPORT int carta_himd_host_file_read(int fd, std::uint8_t* out_ptr, int max_len);

CARTA_HIMD_HOST_EXPORT int carta_himd_host_file_write(int fd, const std::uint8_t* data_ptr,
                                                      int len);

CARTA_HIMD_HOST_EXPORT int carta_himd_host_file_close(int fd);

CARTA_HIMD_HOST_EXPORT int carta_himd_host_scsi_list(std::uint8_t* json_out, int json_cap);

CARTA_HIMD_HOST_EXPORT int carta_himd_host_scsi_open(const char* device_id);

CARTA_HIMD_HOST_EXPORT int carta_himd_host_scsi_close(int handle);

CARTA_HIMD_HOST_EXPORT int carta_himd_host_scsi_reset(int handle);

CARTA_HIMD_HOST_EXPORT int carta_himd_host_scsi_command(
    int handle, const std::uint8_t* cdb_ptr, int cdb_len, int direction,
    const std::uint8_t* data_in_ptr, int data_in_len, std::uint8_t* data_out_ptr,
    int data_out_max_len);

CARTA_HIMD_HOST_EXPORT const char* carta_himd_host_last_error(void);

} // extern "C"
