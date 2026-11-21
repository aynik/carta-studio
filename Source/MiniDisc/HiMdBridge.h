/**
 * @file HiMdBridge.h
 * Out-of-process interface to the isolated GPL-2.0 `carta-himd` helper executable.
 *
 * GPL-2.0 / libusb Isolation Boundary:
 *   Carta Studio and its test targets never link `libhimd_porffor` or `libusb` directly.
 *   All USB SCSI / UMS device discovery, ATRAC3plus/ATRAC3/PCM track uploads, metadata edits,
 *   and Sony OMGAUDIO / ICV DRM signing run inside the separate `carta-himd` helper process
 *   and communicate exclusively over pipes using tab-separated requests and NDJSON responses.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../Core/Session.h"
#include "MdAuthoring.h"

namespace carta {

/** Live transfer/encoding progress snapshot published by Hi-MD operations. */
struct HiMdProgressSnapshot {
    std::string operation;
    std::string phase;
    double progress = 0.0;
    std::int64_t bytesDone = 0;
    std::int64_t bytesTotal = 0;
};

/** Returns a thread-safe copy of the current Hi-MD helper progress state. */
HiMdProgressSnapshot getHiMdProgress();

/** Publishes a clamped Hi-MD progress update across threads. */
void setHiMdProgress(const std::string& operation, const std::string& phase, double progress,
                     std::int64_t bytesDone = 0, std::int64_t bytesTotal = 0);

/** Resets Hi-MD progress tracking back to idle. */
void clearHiMdProgress();

/** Discovered USB or mock Hi-MD recorder descriptor returned by `listHiMdDevices`. */
struct HiMdDeviceEntry {
    std::string id;
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    std::string name;
    bool isMock = false;
    std::string transport;
    std::string systemPath;
};

/** Single track entry reported by `probeHiMdDevice`. */
struct HiMdDeviceTrack {
    int index = 0;
    int slot = 0;
    std::string title;
    std::string artist;
    std::string album;
    std::string encoding;
    int bitrate = 0;
    double durationSeconds = 0.0;
};

/** Contiguous track group reported by `probeHiMdDevice`. */
struct HiMdDeviceGroup {
    int groupIndex = 0;
    std::string title;
    int startIndex = 0;
    int trackCount = 0;
};

/** Full Hi-MD recorder + FAT16/OMGAUDIO filesystem + track table status snapshot. */
struct HiMdDeviceStatus {
    std::string deviceId;
    std::string deviceName;
    std::string inquiryVid;
    std::string inquiryPid;
    std::string inquiryRev;
    std::int64_t maxLba = 0;
    int blockSize = 2048;
    std::int64_t deviceSize = 0;
    std::int64_t fsTotalBytes = 0;
    std::int64_t fsUsedBytes = 0;
    std::int64_t fsFreeBytes = 0;
    int datanum = 0;
    std::string discId;
    std::string discTitle;
    int trackCount = 0;
    std::vector<HiMdDeviceTrack> tracks;
    std::vector<HiMdDeviceGroup> groups;
};

/** Pre-recording space and codec compatibility check against `MdDisc` and optional device status. */
struct HiMdPreflightReport {
    bool valid = false;
    int tracksToUpload = 0;
    int uniqueAudioTracksToUpload = 0;
    int preservedOnDiscTracks = 0;
    int newUniqueAudioTracks = 0;
    std::int64_t estimatedAudioBytes = 0;
    std::int64_t unaliasedAudioBytes = 0;
    std::int64_t savedBytes = 0;
    std::int64_t requiredBytes = 0;
    std::int64_t discTotalBytes = 0;
    std::int64_t discFreeBytes = 0;
    std::string error;
};

/** Summary of a completed `recordHiMdDisc` or `writeHiMdIndex` operation. */
struct HiMdRecordResult {
    bool ok = false;
    bool hardwareConfirmed = false;
    std::string deviceId;
    std::string deviceName;
    std::string discTitle;
    int uploadedTracks = 0;
    int uniqueAudioTracksUploaded = 0;
    int preservedAudioSlots = 0;
    int freedAudioSlots = 0;
    int gaplessRuns = 0;
    std::int64_t uploadedBytes = 0;
    std::int64_t blocksWritten = 0;
    int datanum = 0;
    bool icvUpdated = false;
    std::string error;
};

/** Returns true when `deviceId` identifies the offline mock Sony MZ-RH1 device (`mock:...`). */
bool isHiMdMockDeviceId(const std::string& deviceId) noexcept;

/** Returns true when `error` indicates OS permission / exclusive-driver denial requiring elevation. */
bool isHiMdPermissionError(const std::string& error) noexcept;

/** Controls whether `HiMdBridge` may trigger an interactive OS administrator prompt on permission errors. */
void setHiMdElevationPromptEnabled(bool enabled) noexcept;

/** Returns true when interactive OS administrator prompting is enabled for Hi-MD operations. */
bool isHiMdElevationPromptEnabled() noexcept;

/** Returns true when the persistent elevated `carta-himd` session socket/pipe is currently connected. */
bool isHiMdElevatedSessionActive() noexcept;

/** Ensures the persistent elevated `carta-himd` session is running (prompting once if needed). */
bool ensureHiMdElevatedSession(std::string& error);

/** Gracefully terminates and cleans up any running persistent elevated `carta-himd` session. */
void stopHiMdElevatedSession();

/** Returns true when hardware-confirmation enforcement is armed on the persistent mock session. */
bool isHiMdTestMockGuardEnabled() noexcept;

/** Overrides the `carta-himd` helper executable path (empty restores standard discovery). */
void setHiMdHelperPathOverride(const std::string& path);

/** Starts or tears down the persistent offline `carta-himd session --mock` helper process. */
bool setHiMdTestMockSessionEnabled(bool enabled, std::string& error);

/** Resets mock Hi-MD disc state inside the active persistent mock session. */
bool resetHiMdTestMockSession(std::string& error);

/** Arms or disarms hardware-confirmation blocking inside the persistent mock session. */
bool setHiMdTestMockGuard(bool enabled, std::string& error);

/** Retrieves the JSON diagnostic snapshot object from the persistent mock session. */
bool fetchHiMdTestMockSnapshot(std::string& outJson, std::string& error);

/** Lists connected USB Hi-MD recorders (or mock MZ-RH1 when the mock session is enabled). */
bool listHiMdDevices(std::vector<HiMdDeviceEntry>& out, std::string& error);

/** Probes device identity, capacity, OMGAUDIO filesystem space, tracks, and groups. */
bool probeHiMdDevice(const std::string& deviceId, HiMdDeviceStatus& out, std::string& error,
                     bool allowElevationPrompt = true);

/**
 * Validates `disc` for Hi-MD recording (codec compatibility, non-empty track list, and capacity
 * against `deviceStatus` if provided or `hiMdMediumCapacityBytes(disc.medium)` otherwise).
 */
HiMdPreflightReport preflightHiMdRecording(const MdDisc& disc,
                                           const HiMdDeviceStatus* deviceStatus = nullptr);

/**
 * Renders, encodes, and commits all unique audio items, Play Order aliases,
 * gapless continuous runs, disc title, and groups in `disc` from `session` to `deviceId`
 * in one atomic session. Hardware confirmation is verified BEFORE rendering or encoding.
 */
bool recordHiMdDisc(const Session& session, const MdDisc& disc, const std::string& deviceId,
                    bool confirmHardware, HiMdRecordResult& outResult, std::string& error);

/**
 * Commits non-destructive Play Order, slot metadata, disc title, group, and deletion changes
 * for a loaded Hi-MD disc (`disc.loadedDisc.active == true`) without re-encoding or uploading
 * new local audio. Hardware confirmation is verified BEFORE any device write.
 */
bool writeHiMdIndex(const MdDisc& disc, const std::string& deviceId, bool confirmHardware,
                    HiMdRecordResult& outResult, std::string& error);

/** Dumps track `trackIndex` (0-based) to `outPath` as raw decrypted audio (`rawAudio=true`) or
 * EA3/WAV container (`rawAudio=false`). */
bool dumpHiMdTrack(const std::string& deviceId, int trackIndex, const std::string& outPath,
                   bool rawAudio, std::string& error);

/** Updates the Hi-MD disc title. Enforces hardware confirmation unless unguarded mock. */
bool renameHiMdDisc(const std::string& deviceId, const std::string& title, bool confirmHardware,
                    std::string& error);

/** Updates selective title/artist/album metadata fields for 0-based track `trackIndex`. */
bool renameHiMdTrack(const std::string& deviceId, int trackIndex, bool hasTitle,
                     const std::string& title, bool hasArtist, const std::string& artist,
                     bool hasAlbum, const std::string& album, bool confirmHardware,
                     std::string& error);

/** Moves 0-based track `fromIndex` to `toIndex`. */
bool moveHiMdTrack(const std::string& deviceId, int fromIndex, int toIndex, bool confirmHardware,
                   std::string& error);

/**
 * Replaces Hi-MD groups on `deviceId`. Converts 1-based inclusive `MdGroup::startTrack..endTrack`
 * ranges to 0-based indices for `carta-himd`.
 */
bool setHiMdGroups(const std::string& deviceId, const std::vector<MdGroup>& groups, int trackCount,
                   bool confirmHardware, std::string& error);

/** Deletes 0-based track indices in `trackIndices`. */
bool deleteHiMdTracks(const std::string& deviceId, const std::vector<int>& trackIndices,
                      bool confirmHardware, std::string& error);

/** Erases all tracks (`reformat=false`) or reformats FAT16 OMGAUDIO (`reformat=true`). */
bool wipeHiMdDisc(const std::string& deviceId, bool reformat, bool confirmHardware,
                  std::string& error);

/** Probes `deviceId` and populates `outDisc` (`loadedDisc.active = true`, tracks, metadata, groups). */
bool loadHiMdDiscIntoSession(const std::string& deviceId, MdDisc& outDisc,
                             HiMdDeviceStatus* outStatus, std::string& error);

/** Runs the out-of-process `carta-himd selftest-stack` stress check (6 MiB on 544 KB stack). */
bool runHiMdHelperStackSelftest(std::string& error);

} // namespace carta
