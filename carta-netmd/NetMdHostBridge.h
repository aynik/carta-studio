/**
 * @file NetMdHostBridge.h
 * Native C ABI host callbacks and mock/safety guards for Porffor NetMD protocol execution.
 *
 * Responsibilities:
 * - Exports `carta_netmd_host_*` C symbols consumed by the Porffor-compiled
 *   NetMD protocol stack: CommonCrypto DES/3DES & CSPRNG, iconv CP932/UTF-8
 *   conversion, libusb-1.0 USB control/bulk I/O, and progress/sleep hooks.
 * - Provides an in-memory Type-S Sony MZ-N920 mock (`mock:mz-n920`) modeling
 *   patch peripheral slots, EKB session key exchange, SPUpload 2332+100-byte
 *   de-padding, physical cluster placement starting at cluster `50.0.0`, and
 *   cached vs committed UTOC state.
 * - Enforces hardware write safety: non-read-only USB control OUT and all bulk
 *   OUT writes against real USB hardware (or mock with guard simulation enabled)
 *   are rejected unless `ScopedHardwareWriteConfirmation` or
 *   `setHardwareWriteConfirmed(true)` is active.
 *
 * Threading:
 * - USB handles, mock state, and `carta_netmd_host_last_error()` serialize
 *   under an internal host mutex; progress updates use a separate progress mutex
 *   so background bulk transfers can update UI progress without blocking.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace carta {

inline constexpr std::size_t kUtocSectorBytes = 2352;
inline constexpr std::uint16_t kCartaUtocSignature = 0x5601;

/** MiniDisc physical Cluster.Sector.Group address (32 sectors/cluster, 11 groups/sector). */
struct UtocAddress {
    std::uint16_t cluster = 0;
    std::uint8_t sector = 0;
    std::uint8_t group = 0;

    std::int64_t toLinearGroup() const noexcept {
        return (static_cast<std::int64_t>(cluster) * 32 + sector) * 11 + group;
    }
};

// ============================================================================
// Mock Inspection & Progress Snapshots
// ============================================================================

/** One track captured in the mock NetMD recorder after bulk decryption. */
struct NetMdMockUploadedTrack {
    int trackIndex = 0; // 0-based track slot on NetMD
    std::string mode;   // "sp-stereo", "sp-mono", "lp2", "lp4"
    int wireFormat = 0;
    int discFormat = 0;
    int frameCount = 0;
    UtocAddress startAddr{};
    UtocAddress endAddr{};
    std::vector<std::uint8_t> decryptedAudioBytes;
    std::string halfWidthTitle;
    std::string fullWidthTitle;
};

/** Full snapshot of the mock Type-S NetMD recorder state for tests & diagnostics. */
struct NetMdMockSnapshot {
    bool enabled = false;
    bool open = false;
    bool safetyPatchApplied = false;
    bool killEepromWriteApplied = false;
    bool usbCodeExecApplied = false;
    int spUploadUsedCount = 0;
    int tocCommitCount = 0;
    int tocPollCount = 0;
    std::int64_t sleptMs = 0;
    std::string displayOverride;
    std::vector<NetMdMockUploadedTrack> uploadedTracks;
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> utocCache{};
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> committedUtoc{};
};

/** Thread-safe UI progress state reported by long-running NetMD operations. */
struct NetMdProgressSnapshot {
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

/** Sets active NetMD operation progress clamped to `[0.0, 1.0]`. */
void setNetMdProgress(const std::string& operation, const std::string& phase, double progress,
                      std::int64_t bytesDone = 0, std::int64_t bytesTotal = 0);

/** Sets a bounded `[baseProgress, baseProgress + spanProgress]` sub-phase window. */
void setNetMdProgressWindow(const std::string& operation, const std::string& phase,
                            double baseProgress, double spanProgress, std::int64_t bytesDone = 0,
                            std::int64_t bytesTotal = 0);

/**
 * Sets a bounded USB bulk-upload window where raw transfer bytes (including
 * encryption headers and SP sector alignment padding) map monotonically onto
 * `[spanStartWireBytes, spanStartWireBytes + spanWireBytes]` within `totalWireBytes`.
 */
void setNetMdUploadSpanProgressWindow(const std::string& operation, const std::string& phase,
                                      double baseProgress, double spanProgress,
                                      std::int64_t spanStartWireBytes, std::int64_t spanWireBytes,
                                      std::int64_t expectedBulkBytes, std::int64_t totalWireBytes);

/** Advances the current progress window using `localFraction` in `[0.0, 1.0]`. */
void updateNetMdProgressPhase(const std::string& phase, double localFraction);

/** Clears the active NetMD progress snapshot back to idle. */
void clearNetMdProgress();

/** Returns a copy of the current thread-safe NetMD progress snapshot. */
NetMdProgressSnapshot getNetMdProgress();

/** Callback invoked with a snapshot whenever active NetMD progress updates. */
using NetMdProgressCallback = std::function<void(const NetMdProgressSnapshot&)>;

/** Registers (or clears when empty) a listener for live progress snapshots. */
void setNetMdProgressCallback(NetMdProgressCallback cb);

// ============================================================================
// Mock Device & Hardware Write Safety Controls
// ============================================================================

/** Enables or disables enumeration and opening of the `mock:mz-n920` device. */
void setMockNetMdEnabled(bool enabled);
bool isMockNetMdEnabled() noexcept;

/** Resets all mock patch slots, tracks, keys, and UTOC cache/committed buffers. */
void resetMockNetMdState();

/** Seeds mock UTOC cache & committed buffers from 5 raw 2,352-byte sectors and syncs tracks. */
void seedMockNetMdUtoc(const std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5>& sectors);

/** Returns a consistent copy of the mock NetMD recorder state. */
NetMdMockSnapshot getMockNetMdSnapshot();

/** Enables hardware write-confirmation enforcement on the mock device for safety testing. */
void setSimulateRealHardwareGuardOnMock(bool enabled);
bool isSimulateRealHardwareGuardOnMock() noexcept;

/** Sets or queries the explicit hardware write confirmation flag. */
void setHardwareWriteConfirmed(bool confirmed);
bool isHardwareWriteConfirmed() noexcept;

/** RAII guard that temporarily sets hardware write confirmation and restores prior state on exit.
 */
class ScopedHardwareWriteConfirmation {
  public:
    explicit ScopedHardwareWriteConfirmation(bool confirm = true);
    ~ScopedHardwareWriteConfirmation();
    ScopedHardwareWriteConfirmation(const ScopedHardwareWriteConfirmation&) = delete;
    ScopedHardwareWriteConfirmation& operator=(const ScopedHardwareWriteConfirmation&) = delete;

  private:
    bool previous_ = false;
};

/** Returns true when `deviceId` identifies the built-in mock NetMD device. */
bool isNetMdMockDeviceId(const std::string& deviceId) noexcept;

} // namespace carta

// ============================================================================
// Porffor NetMD Host C ABI
// ============================================================================

#if defined(_WIN32)
#define CARTA_NETMD_HOST_EXPORT
#else
#define CARTA_NETMD_HOST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_des_crypt(int is_triple_des, int is_encrypt,
                                                       int is_cbc, const std::uint8_t* key_ptr,
                                                       int key_len, const std::uint8_t* iv_ptr,
                                                       const std::uint8_t* in_ptr, int in_len,
                                                       std::uint8_t* out_ptr);

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_random_bytes(std::uint8_t* out_ptr, int len);

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_cp932(int to_utf8, const std::uint8_t* in_ptr,
                                                   int in_len, std::uint8_t* out_ptr, int out_cap);

CARTA_NETMD_HOST_EXPORT void carta_netmd_host_sleep_ms(int ms);

CARTA_NETMD_HOST_EXPORT void carta_netmd_host_report_step(int done, int total);

CARTA_NETMD_HOST_EXPORT double carta_netmd_host_now_ms(void);

CARTA_NETMD_HOST_EXPORT void carta_netmd_host_report_toc_wait(int elapsed_ms, int max_ms,
                                                              int done);

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_list(std::uint8_t* json_out, int json_cap);

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_open(const char* device_id);

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_close(int handle);

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_reset(int handle);

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_control_out(int handle, int request, int value,
                                                             int index, const std::uint8_t* data,
                                                             int len);

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_control_in(int handle, int request, int value,
                                                            int index, std::uint8_t* out,
                                                            int max_len);

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_bulk_out(int handle, int endpoint,
                                                          const std::uint8_t* data, int len);

CARTA_NETMD_HOST_EXPORT int carta_netmd_host_usb_bulk_in(int handle, int endpoint,
                                                         std::uint8_t* out, int max_len);

CARTA_NETMD_HOST_EXPORT const char* carta_netmd_host_last_error(void);

} // extern "C"
