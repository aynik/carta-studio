/**
 * @file NetMdBridge.h
 * High-level C++ orchestration over the out-of-process `carta-netmd` CLI helper.
 *
 * Key Invariants:
 * - License & process isolation: USB NetMD protocol operations execute in the
 *   standalone GPL-2.0 `carta-netmd` helper executable via CLI arguments,
 *   temporary binary files, and NDJSON progress/result events on `stdout`.
 *   `Carta Studio` itself links zero GPL-2.0 or libusb code.
 * - Hardware write safety: Destructive operations (`eraseNetMdDisc`,
 *   `writeNetMdUtoc`, `recordNetMdDisc`) require `confirmHardware == true` when
 *   targeting real hardware. `recordNetMdDisc` checks confirmation BEFORE
 *   running ATRAC encodes and encodes all spans BEFORE erasing the disc.
 * - SP sector alignment: `carta1` appends 1 flush-tail frame (512 samples), so
 *   SP PCM is padded so `(pcmFrames + 1) % 11 == 0`, producing exact 2,332-byte
 *   physical MD sectors.
 * - Physical cluster `50.0.0` preservation: Real Sony hardware allocates audio
 *   starting around cluster 50 rather than logical cluster 1. `writeNetMdUtoc`
 *   preserves hardware Sector 0 physical cluster bounds and `P-FRA` chain via
 *   either the cached recorded span map or matching on-disc Sector 0 topology,
 *   writes Shift-JIS Sector 4 to peripheral index 3 on Sony hardware (`mask=0x0F`,
 *   never overwriting read-only P-TOC at index 4), or index 4 on legacy
 *   Rainbow Book 5-sector fixtures (`mask=0x17`), omitting Sector 4 (`mask=0x07`)
 *   when unused.
 * - Threading: Helper calls serialize on an internal bridge mutex; callers on
 *   background workers can poll `getNetMdProgress()` safely.
 */

#pragma once

#include "Core/Session.h"
#include "MiniDisc/MdAuthoring.h"
#include "MiniDisc/Utoc.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace carta {

// ============================================================================
// Progress & Device / Operation Data Structures
// ============================================================================

/** Thread-safe UI progress state reported by long-running NetMD operations. */
struct NetMdProgressSnapshot {
    bool active = false;
    std::string operation;
    std::string phase;
    double progress = 0.0;
    std::int64_t bytesDone = 0;
    std::int64_t bytesTotal = 0;
};

/** Sets active NetMD operation progress clamped to `[0.0, 1.0]`. */
void setNetMdProgress(const std::string& operation, const std::string& phase, double progress,
                      std::int64_t bytesDone = 0, std::int64_t bytesTotal = 0);

/** Clears the active NetMD progress snapshot back to idle. */
void clearNetMdProgress();

/** Returns a copy of the current thread-safe NetMD progress snapshot. */
NetMdProgressSnapshot getNetMdProgress();

/** Enumerated USB NetMD / Hi-MD device entry. */
struct NetMdDeviceEntry {
    std::string id;
    int vendorId = 0;
    int productId = 0;
    int bus = 0;
    int address = 0;
    std::string name;
    bool isMock = false;
};

/** Single on-disc track entry reported by a connected NetMD recorder. */
struct NetMdDeviceTrack {
    int index = 0;              ///< Zero-based track index on the device.
    std::string title;          ///< Half-width ASCII track title from UTOC Sector 1.
    std::string fullWidthTitle; ///< Full-width Shift-JIS track title from UTOC Sector 4 (UTF-8).
    std::string codec;          ///< Device-reported encoding label ("SP", "Mono", "LP2", "LP4").
    int durationSeconds = 0;    ///< Track length reported by the device in whole seconds.
};

/** Live capability and inserted-disc status probed from a NetMD recorder. */
struct NetMdDeviceStatus {
    std::string deviceId;
    std::string deviceName;
    std::string versionCode;
    bool supportsSpUpload = false;
    bool supportsForceTocEdit = false;
    bool discPresent = false;
    bool writable = false;
    bool writeProtected = false;
    bool hiMdFormatted = false;
    int trackCount = 0;
    int usedSeconds = 0;
    int totalSeconds = 0;
    int leftSeconds = 0;
    std::string discTitle;
    std::string fullWidthTitle;
    std::vector<NetMdDeviceTrack> tracks;
};

/** Combined UTOC budget and hardware readiness report for UI and CLI preflight. */
struct NetMdPreflightReport {
    bool ok = false;
    bool utocCompiled = false;
    bool deviceConnected = false;
    bool requiresSpUpload = false;
    bool requiresForceTocEdit = false;
    bool canWriteUtoc = false;
    bool canRecord = false;
    int uniqueUploadSegments = 0;
    int logicalTracks = 0;
    bool hasLoadedDisc = false;
    int preservedOnDiscTracks = 0;
    int onDiscClusters = 0;
    int newUploadClusters = 0;
    int reclaimedClusters = 0;
    bool needsPreUploadReclaim = false;
    UtocBudget budget{};
    NetMdDeviceStatus device{};
    std::string exploitBadge;
    std::string summary;
    std::string statusText;
    std::string blocker;
};

/** Summary returned upon completing a sync / record operation on NetMD hardware. */
struct NetMdRecordResult {
    std::string deviceId;
    int uploadedPhysicalSegments = 0;
    int logicalTracks = 0;
    int groups = 0;
    std::int64_t encodedBytes = 0;
    bool committedUtoc = false;
    int preservedOnDiscTracks = 0;
    int reclaimedClusters = 0;
    bool needsReloadToFinishUpload = false;
    std::string nextActionHint;
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> committedRawSectors{};
    bool hasCommittedRawSectors = false;
    std::string committedSector0Hash;
    UtocBudget budget{};
};

// ============================================================================
// Device Discovery, Preflight, & Disc Operations
// ============================================================================

/** Enumerates connected USB NetMD devices (and mock device if enabled). Clears `out` on failure. */
bool listNetMdDevices(std::vector<NetMdDeviceEntry>& out, std::string& error);

/** Probes device firmware capabilities and inserted disc status. Resets `out` on failure. */
bool probeNetMdDevice(const std::string& deviceId, NetMdDeviceStatus& out, std::string& error);

/** Evaluates UTOC compilation, cluster budget, timeline bounds, and device capabilities without
 * mutating hardware. */
NetMdPreflightReport preflightNetMdRecording(const Session& session, const MdDisc& disc,
                                             const std::string& preferredDeviceId);

/** Evaluates preflight from an already-probed `NetMdDeviceStatus` with zero USB calls. */
NetMdPreflightReport preflightNetMdRecordingWithStatus(const Session& session, const MdDisc& disc,
                                                       const NetMdDeviceStatus& deviceStatus);

/**
 * Erases the inserted MiniDisc (converting Hi-MD formatted discs back to Standard MD).
 * Requires `confirmHardware` or active `ScopedHardwareWriteConfirmation` on real hardware.
 */
bool eraseNetMdDisc(const std::string& deviceId, bool confirmHardware,
                    NetMdDeviceStatus& outAfterStatus, std::string& error);

/**
 * Reads all five raw 2,352-byte UTOC sectors (0..4) and parses Sectors 0, 1, 2, 4 into `outState`.
 * Zeroes `rawSectors` and resets `outState` on failure.
 */
bool readNetMdDiscUtoc(const std::string& deviceId,
                       std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5>& rawSectors,
                       UtocDiscState& outState, std::string& error);

/**
 * Probes the device, reads physical UTOC Sectors 0/1/2/3/4, and decodes the inserted MiniDisc
 * into an editable `MdDisc` with `loadedDisc.active = true` and `baselineSector0Hash` recorded.
 */
bool loadNetMdDisc(const std::string& deviceId, MdDisc& outDisc, NetMdDeviceStatus* outStatus,
                   std::string& error);

/**
 * Updates UTOC Sectors 0, 1, 2, and 4 via ForceTOCEdit without erasing recorded audio.
 * Preserves hardware physical cluster allocations and Sector 3 verbatim.
 */
bool writeNetMdUtoc(
    const MdDisc& disc, const std::string& deviceId, bool confirmHardware, UtocBudget& outBudget,
    std::string& error, std::string* outCommittedSector0Hash = nullptr,
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5>* outCommittedRawSectors = nullptr);

/**
 * Synchronizes the authored MiniDisc onto hardware non-destructively when loaded from a disc
 * (or writes to a blank disc when starting from an empty project; refuses to overwrite non-empty
 * unloaded discs without an explicit erase).
 */
bool recordNetMdDisc(const Session& session, const MdDisc& disc, const std::string& deviceId,
                     bool confirmHardware, NetMdRecordResult& outResult, std::string& error);

// ============================================================================
// Helper Path & Out-of-Process Mock Test Session Controls
// ============================================================================

/** Returns true when `deviceId` refers to an in-memory mock NetMD device (`mock:...`). */
bool isNetMdMockDeviceId(const std::string& deviceId) noexcept;

/** Returns whether simulated real-hardware write confirmation guard is active for mock tests. */
bool isNetMdTestMockGuardEnabled() noexcept;

/** Overrides the `carta-netmd` executable path (empty restores automatic discovery). */
void setNetMdHelperPathOverride(const std::string& path);

/** Starts or stops a persistent `carta-netmd session --mock` helper process for tests. */
bool setNetMdTestMockSessionEnabled(bool enabled, std::string& error);

/** Resets mock device state inside an active test mock session. */
bool resetNetMdTestMockSession(std::string& error);

/** Seeds 5 raw UTOC sectors into the active test mock session (for read-only & non-destructive tests). */
bool seedNetMdTestMockUtoc(
    const std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5>& rawSectors,
    std::string& error);

/** Enables or disables simulated real-hardware confirmation guard in an active test mock session.
 */
bool setNetMdTestMockGuard(bool enabled, std::string& error);

/** Writes mock committed UTOC and uploaded tracks into `outDir` and returns metadata JSON. */
bool fetchNetMdTestMockSnapshot(const std::string& outDir, std::string& outJson,
                                std::string& error);

/** Runs `carta-netmd selftest-stack` (20 MB SP upload on a 544 KB thread stack) out of process. */
bool runNetMdHelperStackSelftest(std::string& error);

} // namespace carta
