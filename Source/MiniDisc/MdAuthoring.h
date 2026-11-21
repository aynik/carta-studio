/**
 * @file MdAuthoring.h
 * MiniDisc authoring domain model, physical span planner, and timeline edits.
 *
 * Units & Coordinates:
 * - Timeline references (`MdPart`) use half-open `[startSample, endSample)`
 *   ranges at `kMdSampleRate` (44,100 Hz).
 * - Codec quanta align outward to 512 samples for ATRAC1 SP Stereo/Mono (2/1
 *   sound groups per frame) or 1024 samples for ATRAC3 LP2/LP4 (2/1 sound
 *   groups per frame).
 * - Physical MiniDisc coordinates (`MdPhysAddr`) address `(cluster, sector,
 *   group)` with 32 sectors/cluster and 11 sound groups/sector (352 sound
 *   groups/cluster). Logical planning starts audio at `kMdFirstAudioCluster`
 *   (cluster 1); NetMD hardware later relocates audio to physical cluster
 *   `50.0.0` while preserving relative sound-group offsets.
 *
 * Invariants & Threading:
 * - Pure synchronous value transformations; all mutating functions validate on
 *   a candidate copy and leave caller `MdDisc` unchanged on failure.
 */

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace carta {

struct Session;

// ============================================================================
// Rainbow Book Physical Constants & Enums
// ============================================================================

inline constexpr std::int64_t kMdSampleRate = 44100;
inline constexpr int kMdMaxTracks = 255;
inline constexpr int kMdMaxStandardTracks = 255;
inline constexpr int kHiMdMaxTracks = 2047;
inline constexpr int kHiMdMaxAudioSlots = 2047;
inline constexpr int kMdMaxGroups = 255;
inline constexpr int kMdSectorsPerCluster = 32;
inline constexpr int kMdUnitsPerSector = 11;
inline constexpr int kMdUnitsPerCluster = kMdSectorsPerCluster * kMdUnitsPerSector; // 352
inline constexpr std::uint16_t kMdFirstAudioCluster = 1;

/** Standard MiniDisc and Hi-MD physical medium capacities. */
enum class MdMedium { md60, md74, md80, himd300, himd1gb };

/** MiniDisc and Hi-MD recording codec modes supported by Carta Studio. */
enum class MdCodecMode { spStereo, spMono, lp2, lp4, hiSp, hiLp, pcm };

// ============================================================================
// Authoring Data Structures
// ============================================================================

/** Physical cluster/sector/sound-group address on a standard MiniDisc. */
struct MdPhysAddr {
    std::uint16_t cluster = 0;
    std::uint8_t sector = 0;
    std::uint8_t group = 0;

    std::int64_t toLinearUnits() const noexcept {
        return (static_cast<std::int64_t>(cluster) * kMdSectorsPerCluster + sector) *
                   kMdUnitsPerSector +
               group;
    }

    static MdPhysAddr fromLinearUnits(std::int64_t units) noexcept {
        if (units <= 0) {
            return {};
        }
        const auto sectorIdx = units / kMdUnitsPerSector;
        return {
            static_cast<std::uint16_t>(sectorIdx / kMdSectorsPerCluster),
            static_cast<std::uint8_t>(sectorIdx % kMdSectorsPerCluster),
            static_cast<std::uint8_t>(units % kMdUnitsPerSector),
        };
    }

    bool operator==(const MdPhysAddr& o) const noexcept {
        return cluster == o.cluster && sector == o.sector && group == o.group;
    }
    bool operator!=(const MdPhysAddr& o) const noexcept { return !(*this == o); }
    bool operator<(const MdPhysAddr& o) const noexcept {
        return toLinearUnits() < o.toLinearUnits();
    }
};

/** Inclusive physical sound-group address interval `[start, end]`. */
struct MdPhysRange {
    MdPhysAddr start{};
    MdPhysAddr end{};

    bool operator==(const MdPhysRange& o) const noexcept {
        return start == o.start && end == o.end;
    }
    bool operator!=(const MdPhysRange& o) const noexcept { return !(*this == o); }
};

/** One coalesced contiguous on-disc audio region mapped onto timeline `[startSample, endSample)`. */
struct MdOnDiscRegion {
    std::string id;
    MdCodecMode codec = MdCodecMode::spStereo;
    std::uint8_t rawModeByte = 0;
    MdPhysAddr physStart{};
    MdPhysAddr physEnd{};
    int unitCount = 0;
    std::int64_t timelineStartSample = 0;
    std::int64_t timelineEndSample = 0;
    bool audioDownloaded = false;

    bool operator==(const MdOnDiscRegion& o) const noexcept {
        return id == o.id && codec == o.codec && rawModeByte == o.rawModeByte &&
               physStart == o.physStart && physEnd == o.physEnd && unitCount == o.unitCount &&
               timelineStartSample == o.timelineStartSample &&
               timelineEndSample == o.timelineEndSample &&
               audioDownloaded == o.audioDownloaded;
    }
    bool operator!=(const MdOnDiscRegion& o) const noexcept { return !(*this == o); }
};

/** Physical topology & safety baseline captured when loading an existing MiniDisc. */
struct MdLoadedDiscState {
    bool active = false;
    std::string sourceDeviceId;
    std::string baselineSector0Hash;
    std::uint16_t deviceSignature = 0;
    std::uint16_t physicalStartCluster = 50;
    std::uint16_t physicalLastCluster = 2251;
    std::vector<MdOnDiscRegion> regions;
    std::vector<MdPhysRange> initialFreeRanges;
    std::vector<MdPhysRange> defectiveRanges;
    std::vector<std::uint8_t> rawSector3;

    int physicalTotalClusters() const noexcept {
        return physicalLastCluster >= physicalStartCluster
                   ? static_cast<int>(physicalLastCluster - physicalStartCluster + 1)
                   : 0;
    }

    bool operator==(const MdLoadedDiscState& o) const noexcept {
        return active == o.active && sourceDeviceId == o.sourceDeviceId &&
               baselineSector0Hash == o.baselineSector0Hash &&
               deviceSignature == o.deviceSignature &&
               physicalStartCluster == o.physicalStartCluster &&
               physicalLastCluster == o.physicalLastCluster && regions == o.regions &&
               initialFreeRanges == o.initialFreeRanges &&
               defectiveRanges == o.defectiveRanges && rawSector3 == o.rawSector3;
    }
    bool operator!=(const MdLoadedDiscState& o) const noexcept { return !(*this == o); }
};

/** Half-open timeline audio reference `[startSample, endSample)` at 44.1 kHz. */
struct MdPart {
    std::int64_t startSample = 0;
    std::int64_t endSample = 0;

    std::int64_t durationSamples() const noexcept {
        return endSample > startSample ? endSample - startSample : 0;
    }

    bool operator==(const MdPart& o) const noexcept {
        return startSample == o.startSample && endSample == o.endSample;
    }
    bool operator!=(const MdPart& o) const noexcept { return !(*this == o); }
};

/** Logical UTOC track entry composed of one or more timeline parts and MD metadata. */
struct MdTrack {
    std::string id;
    std::string halfWidthTitle;
    std::string fullWidthTitle;
    std::optional<MdCodecMode> codecOverride;
    bool writeProtected = false;
    bool scmsCopyProtected = false;
    bool emphasis = false;
    std::string timestamp; // "YYYY-MM-DD HH:MM:SS" or empty
    std::vector<MdPart> parts;
    std::string artist{};
    std::string album{};
    std::string audioId{};

    std::int64_t totalSamples() const noexcept {
        std::int64_t total = 0;
        for (const auto& part : parts) {
            total += part.durationSamples();
        }
        return total;
    }

    std::string displayTitle() const {
        if (!halfWidthTitle.empty()) {
            return halfWidthTitle;
        }
        if (!fullWidthTitle.empty()) {
            return fullWidthTitle;
        }
        return {};
    }

    bool operator==(const MdTrack& o) const noexcept {
        return id == o.id && halfWidthTitle == o.halfWidthTitle &&
               fullWidthTitle == o.fullWidthTitle && artist == o.artist && album == o.album &&
               audioId == o.audioId && codecOverride == o.codecOverride &&
               writeProtected == o.writeProtected && scmsCopyProtected == o.scmsCopyProtected &&
               emphasis == o.emphasis && timestamp == o.timestamp && parts == o.parts;
    }
    bool operator!=(const MdTrack& o) const noexcept { return !(*this == o); }
};

/** Disjoint inclusive 1-based MD track group range. */
struct MdGroup {
    int startTrack = 1;
    int endTrack = 1;
    std::string halfWidthTitle;
    std::string fullWidthTitle;

    bool operator==(const MdGroup& o) const noexcept {
        return startTrack == o.startTrack && endTrack == o.endTrack &&
               halfWidthTitle == o.halfWidthTitle && fullWidthTitle == o.fullWidthTitle;
    }
    bool operator!=(const MdGroup& o) const noexcept { return !(*this == o); }
};

/** Authoring document defining UTOC tracks, groups, and physical recording preferences. */
struct MdDisc {
    MdMedium medium = MdMedium::md80;
    MdCodecMode defaultCodec = MdCodecMode::spStereo;
    std::string halfWidthTitle;
    std::string fullWidthTitle;
    std::string discTimestamp;
    bool autoTrackOnImport = true;
    std::vector<MdTrack> tracks;
    std::vector<MdGroup> groups;
    /** Optional raw UTOC Sectors 0, 1, 2, 4 imported from a physical disc or bundle. */
    std::array<std::vector<std::uint8_t>, 4> importedUtocSectors;
    /** Loaded-disc physical topology when editing an existing MiniDisc non-destructively. */
    MdLoadedDiscState loadedDisc;

    bool operator==(const MdDisc& o) const noexcept {
        return medium == o.medium && defaultCodec == o.defaultCodec &&
               halfWidthTitle == o.halfWidthTitle && fullWidthTitle == o.fullWidthTitle &&
               discTimestamp == o.discTimestamp && autoTrackOnImport == o.autoTrackOnImport &&
               tracks == o.tracks && groups == o.groups &&
               importedUtocSectors == o.importedUtocSectors && loadedDisc == o.loadedDisc;
    }
    bool operator!=(const MdDisc& o) const noexcept { return !(*this == o); }
};

/** One contiguous codec-aligned physical audio upload on disc. */
struct MdPhysicalSpan {
    MdCodecMode codec = MdCodecMode::spStereo;
    std::int64_t startSample = 0; // aligned to mdFrameQuantumSamples(codec)
    std::int64_t endSample = 0;   // aligned to mdFrameQuantumSamples(codec)
    int unitCount = 0;
    std::uint16_t startCluster = kMdFirstAudioCluster;
    int clusterCount = 0;
};

/** Physical placement of a single MdPart inside a coalesced upload span or on-disc region. */
struct MdPartPlacement {
    int trackIndex = 0; // 0-based
    int partIndex = 0;  // 0-based
    int spanIndex = -1; // 0-based into MdPhysicalPlan::spans, or -1 if on-disc
    int unitOffsetInSpan = 0;
    int unitCount = 0;
    MdPhysAddr startAddr{};
    MdPhysAddr endAddr{}; // inclusive last unit
    bool isOnDisc = false;
    int onDiscRegionIndex = -1;
    std::uint8_t rawModeByte = 0;
};

/** Complete physical allocation and UTOC fragment plan derived from an MdDisc. */
struct MdPhysicalPlan {
    std::vector<MdPhysicalSpan> spans;
    std::vector<MdPartPlacement> placements;
    std::int64_t maxEndSample = 0;
    std::int64_t logicalPlaySamples = 0;
    std::int64_t uniquePhysicalSamples = 0;
    int totalClusters = 0;
    int usedClusters = 0;
    int uncoalescedClusters = 0;
    int savedClusters = 0;
    int onDiscClusters = 0;
    int newUploadClusters = 0;
    int reclaimedClusters = 0;
    int initialFreeClusters = 0;
    int freeClustersAfterReclaim = 0;
    std::vector<MdPhysRange> reclaimedFreeRanges;
    int playOrderTrackCount = 0;
    int uniqueAudioTrackCount = 0;
    int aliasedTrackCount = 0;
    std::int64_t hiMdUniqueBytes = 0;
    std::int64_t hiMdUnaliasedBytes = 0;
    std::int64_t hiMdSavedBytes = 0;
};

// ============================================================================
// Medium & Codec Metadata Helpers
// ============================================================================

/** Returns canonical string tag ("md60", "md74", "md80", "himd300", or "himd1gb"). */
std::string mdMediumToString(MdMedium medium);

/** Parses medium text or minute count; leaves `out` unchanged on failure. */
bool parseMdMedium(const std::string& text, MdMedium& out) noexcept;

/** Returns true when `medium` uses Hi-MD FAT16/TRKIDX format rather than Standard MD UTOC. */
bool isHiMdMedium(MdMedium medium) noexcept;

/** Returns true when `mode` is a Hi-MD-only codec (`hiSp`, `hiLp`, `pcm`). */
bool isHiMdOnlyCodec(MdCodecMode mode) noexcept;

/** Returns true when `mode` is only valid on Standard MD / NetMD (`spStereo`, `spMono`). */
bool isStandardMdOnlyCodec(MdCodecMode mode) noexcept;

/** Returns true when `mode` is supported on `medium`. */
bool isCodecValidForMedium(MdMedium medium, MdCodecMode mode) noexcept;

/** Returns nominal Hi-MD filesystem usable capacity in bytes (0 for Standard MD). */
std::int64_t hiMdMediumCapacityBytes(MdMedium medium) noexcept;

/** Estimates Hi-MD cluster-aligned byte storage required for `samples` in `mode`. */
std::int64_t estimateHiMdTrackBytes(MdCodecMode mode, std::int64_t samples) noexcept;

/** Returns standard writable user cluster capacity for `medium` (0 for Hi-MD). */
int mdTotalClusters(MdMedium medium) noexcept;

/** Returns maximum Play Order tracks supported on `medium` (255 Standard MD, 2047 Hi-MD). */
int mdMaxTracks(MdMedium medium) noexcept;

/** Returns maximum unique physical audio items supported on `medium` (255 Standard MD, 2047 Hi-MD). */
int mdMaxAudioTracks(MdMedium medium) noexcept;

/** Returns canonical codec mode tag ("sp-stereo", "sp-mono", "lp2", "lp4", "hi-sp", "hi-lp", "pcm"). */
std::string mdCodecModeToString(MdCodecMode mode);

/** Parses codec mode alias; leaves `out` unchanged on failure. */
bool parseMdCodecMode(const std::string& text, MdCodecMode& out) noexcept;

/** Returns 44.1 kHz PCM samples per codec frame quantum (512 SP, 1024 LP2/LP4). */
std::int64_t mdFrameQuantumSamples(MdCodecMode mode) noexcept;

/** Returns UTOC sound groups consumed per codec quantum (2 SP-Stereo/LP2, 1 SP-Mono/LP4). */
int mdUnitsPerQuantum(MdCodecMode mode) noexcept;

/** Returns exact 44.1 kHz PCM samples per single UTOC sound group (256 SP-St, 512 SP-Mo/LP2, 1024
 * LP4). */
std::int64_t mdSamplesPerUnit(MdCodecMode mode) noexcept;

/** Returns encoded audio channel count (1 for SP Mono, 2 otherwise). */
int mdChannelsForCodec(MdCodecMode mode) noexcept;

/** Resolves track codec override or falls back to `disc.defaultCodec`. */
MdCodecMode effectiveTrackCodec(const MdDisc& disc, const MdTrack& track) noexcept;

// ============================================================================
// Validation & Physical Span Planning
// ============================================================================

/** Returns true when `disc` matches a freshly initialized empty authoring state. */
bool isDefaultMdDisc(const MdDisc& disc) noexcept;

/**
 * Resolves which loaded-disc region completely encloses `part`.
 * Returns `>= 0` (region index), `-1` (completely outside all regions), or `-2` (spans/partial
 * boundary overlap).
 */
int findMdOnDiscRegionForPart(const MdLoadedDiscState& loaded, const MdPart& part) noexcept;

/** Returns true when `disc.loadedDisc.active` and at least one part lies on disc. */
bool mdTrackUsesOnDiscAudio(const MdDisc& disc, const MdTrack& track) noexcept;

/** Counts whole clusters completely covered by physical free ranges `ranges`. */
int countWholeFreeClusters(const std::vector<MdPhysRange>& ranges) noexcept;

/** Generates a globally unique stable track ID ("md-<uuid8>-<seq>"). */
std::string makeMdTrackId();

/** Generates a globally unique Audio Pool item ID ("mda-<uuid8>-<seq>"). */
std::string makeMdAudioId();

/** Returns the effective Audio Pool key for `track` (`audioId` when set, else `id`). */
std::string effectiveMdAudioId(const MdTrack& track);

/** Returns the number of Play Order entries referencing `audioKey` in `disc`. */
int countMdAudioRefs(const MdDisc& disc, const std::string& audioKey) noexcept;

/** Returns all 0-based Play Order indices referencing `audioKey` in `disc`. */
std::vector<int> findMdPlayOrderIndicesForAudio(const MdDisc& disc, const std::string& audioKey);

/** Validates timestamps, unique IDs, part ranges, and disjoint groups; populates `error` on
 * failure. */
bool validateMdDisc(const MdDisc& disc, std::string& error);

/**
 * Coalesces overlapping/abutting same-codec parts into physical spans and UTOC
 * placements. On failure, resets `out` to empty with `totalClusters` set and
 * populates `error`.
 */
bool planMdPhysicalAudio(const MdDisc& disc, MdPhysicalPlan& out, std::string& error);

// ============================================================================
// Track, Part, & Group Mutations
// ============================================================================

/** Resolves a stable track ID or 1-based numeric index string to a 0-based track index, or -1. */
int findMdTrackIndex(const MdDisc& disc, const std::string& idOrOneBasedIndex) noexcept;

/** Appends an auto-generated track for an imported clip when `autoTrackOnImport` is enabled. */
bool appendAutoTrackForClip(MdDisc& disc, const std::string& title, std::int64_t startSample,
                            std::int64_t endSample);

/**
 * Appends or splits-tail auto-track for a clip placed at `[startSample, endSample)`.
 * Leaves `disc` unchanged if disabled, invalid, or track limit is reached.
 */
bool applyAutoTrackForPlacedClip(MdDisc& disc, const std::string& title, std::int64_t startSample,
                                 std::int64_t endSample);

/** Inserts `track` at 1-based position (`1..count+1`) and shifts groups; leaves `disc` unchanged on
 * error. */
bool insertMdTrack(MdDisc& disc, int oneBasedPosition, MdTrack track, std::string& error);

/** Splits `trackIndex0` at `timelineSample` if strictly inside one part; leaves `disc` unchanged on
 * error. */
bool splitMdTrackAtSample(MdDisc& disc, int trackIndex0, std::int64_t timelineSample,
                          std::string& error);

/** Merges adjacent tracks `trackIndex0` and `trackIndex0 + 1`; leaves `disc` unchanged on error. */
bool mergeMdTrackWithNext(MdDisc& disc, int trackIndex0, std::string& error);

/** Duplicates `trackIndex0` directly after itself with a fresh ID; leaves `disc` unchanged on
 * error. */
bool duplicateMdTrack(MdDisc& disc, int trackIndex0, std::string& error);

/** Clones `srcIndex0` to 0-based `destIndex0` (`0..count`) as a Play Order alias sharing `audioId`;
 * leaves `disc` unchanged on error. */
bool cloneMdTrackToPosition(MdDisc& disc, int srcIndex0, int destIndex0, std::string& error);

/** Synchronizes audio-item fields from `sourceTrackIndex0` across all aliases sharing its `audioId`. */
void syncAliasedMdTracks(MdDisc& disc, int sourceTrackIndex0);

/** Inserts a Play Order alias of `srcIndex0` at 0-based `destIndex0` (`0..count`) sharing `audioId`. */
bool aliasMdTrackAtPosition(MdDisc& disc, int srcIndex0, int destIndex0, std::string& error);

/** Inserts a Play Order alias for Audio Pool key `audioKey` at 0-based `destIndex0`. */
bool insertMdAliasByAudioId(MdDisc& disc, const std::string& audioKey, int destIndex0,
                            std::string& error);

/** Detaches aliased Play Order entry `trackIndex0` into its own independent audio item ("Make Unique"). */
bool detachMdTrackAlias(MdDisc& disc, int trackIndex0, std::string& error);

/** Sets the timeline range `[startSample, endSample)` of `trackIndex0` and syncs any aliases. */
bool setMdTrackRange(MdDisc& disc, int trackIndex0, std::int64_t startSample,
                     std::int64_t endSample, std::string& error);

/** Reorders `fromIndex0` to `toIndex0` while maintaining valid disjoint groups; leaves `disc`
 * unchanged on error. */
bool moveMdTrack(MdDisc& disc, int fromIndex0, int toIndex0, std::string& error);

/** Moves contiguous track block `[fromStart0..fromEnd0]` to final start `destStart0` while
 * preserving valid disjoint groups; leaves `disc` unchanged on error. */
bool moveMdTrackBlock(MdDisc& disc, int fromStart0, int fromEnd0, int destStart0,
                      std::string& error);

/** Removes `trackIndex0` and shifts group indices; leaves `disc` unchanged on error. */
bool removeMdTrack(MdDisc& disc, int trackIndex0, std::string& error);

/** Moves a shared split boundary `newSample` between two distinct parts; leaves `disc` unchanged on
 * error. */
bool moveMdSharedBoundary(MdDisc& disc, int leftTrack0, int leftPart0, int rightTrack0,
                          int rightPart0, std::int64_t newSample, std::int64_t maxSample,
                          std::string& error);

/** Trims a part's start or end edge clamped within `[0, maxSample]`; leaves `disc` unchanged on
 * error. */
bool trimMdPartEdge(MdDisc& disc, int trackIndex0, int partIndex0, bool rightEdge,
                    std::int64_t newSample, std::int64_t maxSample, std::string& error);

/** Slides a part preserving duration within `[0, maxSample]`; leaves `disc` unchanged on error. */
bool slideMdPartRange(MdDisc& disc, int trackIndex0, int partIndex0, std::int64_t newStartSample,
                      std::int64_t maxSample, std::string& error);

/** Assigns a named group over inclusive 1-based `[startTrack1, endTrack1]`, trimming overlaps;
 * leaves `disc` unchanged on error. */
bool setMdGroup(MdDisc& disc, int startTrack1, int endTrack1, const std::string& halfWidthTitle,
                const std::string& fullWidthTitle, std::string& error);

/** Clears or trims any groups overlapping inclusive `[startTrack1, endTrack1]`; leaves `disc`
 * unchanged on error. */
bool ungroupMdRange(MdDisc& disc, int startTrack1, int endTrack1, std::string& error);

// ============================================================================
// Timeline Synchronization
// ============================================================================

/** Rebuilds tracks from explicit `(layer, clip)` pairs or active unmuted timeline clips; leaves
 * `disc` unchanged on error. */
bool syncTracksFromClips(const Session& session, MdDisc& disc, std::string& error,
                         const std::vector<std::pair<int, int>>& explicitClips = {});

/** Rebuilds tracks directly from Layer 1 clips sorted chronologically; leaves `disc` unchanged on
 * error. */
bool syncTracksFromLayerClips(const Session& session, MdDisc& disc, std::string& error);

/** Rebuilds contiguous tracks bounded by session timeline markers; leaves `disc` unchanged on
 * error. */
bool syncTracksFromMarkers(const Session& session, MdDisc& disc, std::string& error);

} // namespace carta
