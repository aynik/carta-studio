/**
 * @file Utoc.h
 * Rainbow Book MiniDisc User Table of Contents (UTOC) binary codec & planner.
 *
 * Sector & Slot Architecture:
 * - Each raw UTOC sector is `kUtocSectorBytes` (2,352 bytes): 48-byte header
 *   (`0x00..0x2F`), 256-entry slot pointer table (`0x30..0x12F`), and 256 8-byte
 *   slots (`0x130..0x92F`, where slot 0 is null terminator and slots 1..255 are
 *   allocatable).
 * - Sector 0: Audio fragment DAG (`trackMap[0]` = P-FRA free-area pointer,
 *   `trackMap[1..N]` = first fragment slot per track) using 24-bit packed
 *   `(cluster:14, sector:6, group:4)` addresses (32 sectors/cluster, 11 sound
 *   groups/sector = 352 units/cluster).
 * - Sector 1: Half-width ASCII / JIS X 0201 Kana titles and Sony group syntax
 *   (`0;discTitle//1-3;groupTitle//`) in 7-byte linked `UtocTitleCell` chains.
 * - Sector 2: Recording timestamps for disc header (`map[0]`) and tracks.
 * - Sector 4: Full-width Shift-JIS (CP932) title chains (`０；...／／`).
 *
 * Invariants & Threading:
 * - Pure stateless serialization/deserialization; safe to invoke concurrently on
 *   distinct output structures. Output parameters are cleared/reset on failure.
 */

#pragma once

#include "MiniDisc/MdAuthoring.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace carta {

// ============================================================================
// Rainbow Book UTOC Constants & Flags
// ============================================================================

inline constexpr std::size_t kUtocSectorBytes = 2352;
inline constexpr std::size_t kUtocSlots = 256;
inline constexpr int kUtocMaxTracks = 255;
inline constexpr int kUtocMaxFragments = 255;
inline constexpr int kUtocMaxTitleCells = 255;
inline constexpr std::uint16_t kCartaUtocSignature = 0x8CA2;

/** Bitmask for UTOC Sector 0 fragment `mode` byte (codec, copy protection, emphasis). */
enum UtocModeFlag : std::uint8_t {
    kUtocEmphasis = 0x01,
    kUtocStereo = 0x02,
    kUtocSpMode = 0x04,
    kUtocReservedCodec = 0x08,
    kUtocNonAudio = 0x10,
    kUtocScmsDigCopy = 0x20,
    kUtocScmsUnrestricted = 0x40,
    kUtocWritable = 0x80
};

// ============================================================================
// Decoded UTOC Structures
// ============================================================================

/** Packed 24-bit Rainbow Book cluster/sector/sound-group address. */
struct UtocAddress {
    std::uint16_t cluster = 0;
    std::uint8_t sector = 0;
    std::uint8_t group = 0;

    std::int64_t toLogicalSector() const noexcept {
        return static_cast<std::int64_t>(cluster) * 32 + sector;
    }

    std::int64_t toLinearGroup() const noexcept { return toLogicalSector() * 11 + group; }

    static UtocAddress fromLogicalSector(std::int64_t logical, std::uint8_t grp = 0) noexcept {
        return {
            static_cast<std::uint16_t>(logical / 32),
            static_cast<std::uint8_t>(logical % 32),
            static_cast<std::uint8_t>(grp & 0x0F),
        };
    }

    static UtocAddress fromLinearGroup(std::int64_t linear) noexcept {
        const auto logical = linear / 11;
        return {
            static_cast<std::uint16_t>(logical / 32),
            static_cast<std::uint8_t>(logical % 32),
            static_cast<std::uint8_t>(linear % 11),
        };
    }

    bool isZero() const noexcept { return cluster == 0 && sector == 0 && group == 0; }

    bool operator==(const UtocAddress& o) const noexcept {
        return cluster == o.cluster && sector == o.sector && group == o.group;
    }
    bool operator!=(const UtocAddress& o) const noexcept { return !(*this == o); }

    bool operator<(const UtocAddress& o) const noexcept {
        if (cluster != o.cluster) {
            return cluster < o.cluster;
        }
        if (sector != o.sector) {
            return sector < o.sector;
        }
        return group < o.group;
    }
};

/** 8-byte UTOC Sector 0 audio fragment slot (start/end inclusive, mode flags, next link). */
struct UtocFragment {
    UtocAddress start{};
    std::uint8_t mode = 0;
    UtocAddress end{};
    std::uint8_t link = 0;

    bool isValid() const noexcept { return !start.isZero() || !end.isZero(); }
};

/** 8-byte UTOC Sector 1/4 title cell (7 payload bytes + 1-byte chain link). */
struct UtocTitleCell {
    std::array<std::uint8_t, 7> bytes{};
    std::uint8_t link = 0;
};

/** 8-byte UTOC Sector 2 recording timestamp entry + 16-bit maker/device signature. */
struct UtocTimestamp {
    std::uint8_t year = 0;
    std::uint8_t month = 0;
    std::uint8_t day = 0;
    std::uint8_t hour = 0;
    std::uint8_t minute = 0;
    std::uint8_t second = 0;
    std::uint16_t signature = 0;

    bool isValid() const noexcept {
        return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59 &&
               second <= 59;
    }
};

/** Parsed MDLP group entry recovered from UTOC Sector 1 and Sector 4 disc headers. */
struct UtocGroupEntry {
    int startTrack = 1;
    int endTrack = 1;
    std::string halfWidthTitle;
    std::string fullWidthTitle;
};

/** Resource consumption summary against Rainbow Book 255-slot and cluster limits. */
struct UtocBudget {
    int usedClusters = 0;
    int totalClusters = 0;
    int usedTracks = 0;
    int maxTracks = kUtocMaxTracks;
    int usedFragments = 0;
    int maxFragments = kUtocMaxFragments;
    int usedHalfWidthCells = 0;
    int maxHalfWidthCells = kUtocMaxTitleCells;
    int usedFullWidthCells = 0;
    int maxFullWidthCells = kUtocMaxTitleCells;
    int sharedFragmentCount = 0;

    bool fits() const noexcept {
        return (totalClusters <= 0 || usedClusters <= totalClusters) && usedTracks <= maxTracks &&
               usedFragments <= maxFragments && usedHalfWidthCells <= maxHalfWidthCells &&
               usedFullWidthCells <= maxFullWidthCells;
    }
};

/** Parsed in-memory representation of UTOC Sectors 0, 1, 2, and 4. */
struct UtocDiscState {
    std::array<std::uint8_t, 28> sec0Padding{};
    std::uint16_t deviceSignature = kCartaUtocSignature;
    std::uint8_t discNonEmpty = 0;
    std::uint8_t trackCount = 0;
    std::array<std::uint8_t, 15> sec0aPadding{};
    std::uint8_t nextFreeTrackSlot = 0;
    std::array<std::uint8_t, kUtocSlots> trackMap{};
    std::array<UtocFragment, kUtocSlots> fragments{};

    std::array<std::uint8_t, 47> sec1Padding{};
    std::uint8_t nextFreeHalfTitleSlot = 0;
    std::array<std::uint8_t, kUtocSlots> halfTitleMap{};
    std::array<UtocTitleCell, kUtocSlots> halfTitleCells{};

    std::array<std::uint8_t, 47> sec2Padding{};
    std::uint8_t nextFreeTimestampSlot = 0;
    std::array<std::uint8_t, kUtocSlots> timestampMap{};
    std::array<UtocTimestamp, kUtocSlots> timestamps{};

    std::array<std::uint8_t, 47> sec4Padding{};
    std::uint8_t nextFreeFullTitleSlot = 0;
    std::array<std::uint8_t, kUtocSlots> fullTitleMap{};
    std::array<UtocTitleCell, kUtocSlots> fullTitleCells{};

    bool hasSector0 = false;
    bool hasSector1 = false;
    bool hasSector2 = false;
    bool hasSector4 = false;
};

// ============================================================================
// Encoding, Decoding, & Inspection Helpers
// ============================================================================

/** Returns standard user cluster capacity for "md60", "md74", or "md80" (0 if unknown). */
int discMediumClusterCapacity(const std::string& medium) noexcept;

/** Maps a UTOC fragment mode byte to "sp-stereo", "sp-mono", "lp2", or "lp4". */
std::string audioModeFromUtocMode(std::uint8_t mode);

/** Builds a UTOC fragment mode byte from audio mode and track protection/emphasis flags. */
std::uint8_t utocModeFromTrack(const std::string& audioMode, bool writeProtected, bool scms,
                               bool emphasis = false);

/** Encodes UTF-8 text into Sector 1 ASCII / JIS X 0201 half-width katakana bytes. */
std::vector<std::uint8_t> encodeHalfWidthBytes(const std::string& utf8);

/** Decodes NUL-terminated Sector 1 half-width bytes back to UTF-8. */
std::string decodeHalfWidthBytes(const std::uint8_t* data, std::size_t size);

/** Converts UTF-8 text to full-width CP932 (Shift-JIS) bytes for Sector 4; sets `error` on iconv
 * failure. */
std::vector<std::uint8_t> encodeFullWidthShiftJis(const std::string& utf8, std::string& error);

/** Decodes NUL-terminated Sector 4 CP932 bytes back to UTF-8 (returns empty string on error). */
std::string decodeFullWidthShiftJis(const std::uint8_t* data, std::size_t size);

/** Parses "YYYY-MM-DD[T| ]HH:MM:SS" into a UTOC timestamp; resets `out` and sets `error` on
 * failure. */
bool parseUtocTimestampString(const std::string& iso, UtocTimestamp& out, std::string& error);

/** Formats a valid UTOC timestamp as "YYYY-MM-DD HH:MM:SS" (empty if invalid). */
std::string formatUtocTimestampString(const UtocTimestamp& ts);

/** Parses Sony group delimiter tokens from Sector 1 and Sector 4 disc headers. */
std::vector<UtocGroupEntry> parseUtocGroups(const std::string& rawHalfTitle,
                                            const std::string& rawFullTitle, int trackCount,
                                            std::string& discHalfTitleOut,
                                            std::string& discFullTitleOut);

// ============================================================================
// Binary Sector Codec & Authoring Conversion
// ============================================================================

/**
 * Parses provided non-null 2,352-byte UTOC sectors (0, 1, 2, 4) into `out`.
 * Requires at least one non-null sector pointer. Resets `out` and populates `error` on failure.
 */
bool parseUtocSectors(const std::uint8_t* sector0, const std::uint8_t* sector1,
                      const std::uint8_t* sector2, const std::uint8_t* sector4, UtocDiscState& out,
                      std::string& error);

/** Serializes `state` into four 2,352-byte UTOC binary sectors (0, 1, 2, 4). */
bool compileUtocSectors(const UtocDiscState& state,
                        std::array<std::uint8_t, kUtocSectorBytes>& sector0,
                        std::array<std::uint8_t, kUtocSectorBytes>& sector1,
                        std::array<std::uint8_t, kUtocSectorBytes>& sector2,
                        std::array<std::uint8_t, kUtocSectorBytes>& sector4, std::string& error);

/** Computes cluster, track, fragment DAG, and title-cell usage for `state`. */
UtocBudget computeUtocBudget(const UtocDiscState& state, const std::string& medium = "md80");

/**
 * Compiles an authored `MdDisc` into `UtocDiscState` (merging identical DAG fragment tails).
 * Resets `out` and populates `error` on failure.
 */
bool compileMdDiscToUtoc(const MdDisc& disc, UtocDiscState& out, std::string& error);

/**
 * Reconstructs an authored `MdDisc` from parsed UTOC sectors while preserving shared fragments.
 * Requires Sector 0. Resets `out` and populates `error` on failure.
 */
bool mdDiscFromUtoc(const UtocDiscState& state, MdDisc& out, std::string& error);

/** Returns true if any reachable fragment carries `kUtocNonAudio` (e.g., Hi-MD data track). */
bool utocHasNonAudioTrack(const UtocDiscState& state) noexcept;

/** Formats a multi-line human-readable inspection summary of UTOC slots and fragments. */
std::string formatUtocInspectionReport(const UtocDiscState& state,
                                       const std::string& medium = "md80");

} // namespace carta
