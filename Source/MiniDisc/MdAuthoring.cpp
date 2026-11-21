/**
 * @file MdAuthoring.cpp
 * Implementation of MiniDisc authoring validation, physical span coalescing,
 * and non-destructive track/part/group/timeline mutations.
 *
 * Key Algorithms & Invariants:
 * - Physical span coalescing (`planMdPhysicalAudio`): Groups all parts by
 *   effective codec, aligns each `[startSample, endSample)` outward to frame
 *   quanta (512 SP / 1024 LP), merges overlapping or contiguous intervals per
 *   codec, and sorts spans by earliest track presentation order.
 * - Abutting split-point snapping: When two parts inside a coalesced span meet
 *   at an exact sample boundary (`rawEnd == rawStart`), that boundary rounds to
 *   the nearest frame quantum (`+ q/2`) so split points land cleanly on one
 *   Sound Group boundary with zero gap and zero duplicated 11.6 ms frame.
 * - Transactional mutations: Track, part, group, and sync mutators validate on
 *   a candidate `MdDisc` copy and swap atomically only upon success.
 */

#include "MiniDisc/MdAuthoring.h"
#include "Editing/Editing.h"
#include "Core/Session.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

namespace carta {
namespace {

// ============================================================================
// Internal Helpers
// ============================================================================

std::string toLowerAscii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

std::string stripAudioExtensionIfPresent(const std::string& name) {
    static constexpr const char* kExts[] = {".wav", ".aif", ".aiff", ".flac",
                                            ".ogg", ".mp3", ".aea",  ".oma"};
    const auto lower = toLowerAscii(name);
    for (const char* ext : kExts) {
        const std::size_t extLen = std::strlen(ext);
        if (lower.size() > extLen && lower.compare(lower.size() - extLen, extLen, ext) == 0) {
            return name.substr(0, name.size() - extLen);
        }
    }
    return name;
}

std::string normalizeAutoTrackTitle(const std::string& title, std::size_t fallbackTrackNum1) {
    std::string trimmed =
        juce::String::fromUTF8(title.c_str()).trim().replaceCharacters("\r\n", "  ").toStdString();
    trimmed = stripAudioExtensionIfPresent(trimmed);
    if (trimmed.empty()) {
        trimmed = "Track " + std::to_string(fallbackTrackNum1);
    }
    return trimmed;
}

bool validTimestampOrEmpty(const std::string& ts) {
    if (ts.empty()) {
        return true;
    }
    // Accept "YYYY-MM-DD HH:MM:SS" or "YYYY-MM-DDTHH:MM:SS"
    if (ts.size() != 19) {
        return false;
    }
    for (std::size_t i = 0; i < ts.size(); ++i) {
        if (i == 4 || i == 7) {
            if (ts[i] != '-') {
                return false;
            }
        } else if (i == 10) {
            if (ts[i] != ' ' && ts[i] != 'T') {
                return false;
            }
        } else if (i == 13 || i == 16) {
            if (ts[i] != ':') {
                return false;
            }
        } else if (!std::isdigit(static_cast<unsigned char>(ts[i]))) {
            return false;
        }
    }
    return true;
}

void shiftGroupsForInsert(std::vector<MdGroup>& groups, int oneBasedPos, bool splitExpandPrevious) {
    for (auto& g : groups) {
        if (splitExpandPrevious) {
            if (g.startTrack >= oneBasedPos) {
                ++g.startTrack;
                ++g.endTrack;
            } else if (g.endTrack >= oneBasedPos - 1) {
                ++g.endTrack;
            }
        } else {
            if (oneBasedPos <= g.startTrack) {
                ++g.startTrack;
                ++g.endTrack;
            } else if (oneBasedPos <= g.endTrack) {
                ++g.endTrack;
            }
        }
    }
}

void shiftGroupsForRemove(std::vector<MdGroup>& groups, int oneBasedRemoved) {
    std::vector<MdGroup> next;
    next.reserve(groups.size());
    for (auto g : groups) {
        if (oneBasedRemoved < g.startTrack) {
            --g.startTrack;
            --g.endTrack;
            next.push_back(std::move(g));
        } else if (oneBasedRemoved <= g.endTrack) {
            if (g.startTrack < g.endTrack) {
                --g.endTrack;
                next.push_back(std::move(g));
            }
        } else {
            next.push_back(std::move(g));
        }
    }
    groups = std::move(next);
}

void trimGroupsExcludingRange(std::vector<MdGroup>& groups, int startTrack1, int endTrack1) {
    std::vector<MdGroup> next;
    next.reserve(groups.size());
    for (auto g : groups) {
        if (g.endTrack < startTrack1 || g.startTrack > endTrack1) {
            next.push_back(std::move(g));
        } else if (g.startTrack < startTrack1 && g.endTrack <= endTrack1) {
            g.endTrack = startTrack1 - 1;
            next.push_back(std::move(g));
        } else if (g.startTrack >= startTrack1 && g.endTrack > endTrack1) {
            g.startTrack = endTrack1 + 1;
            next.push_back(std::move(g));
        }
        // Any group fully covered or split through its middle is dropped/replaced.
    }
    groups = std::move(next);
}

void normalizeGroups(std::vector<MdGroup>& groups) {
    std::sort(groups.begin(), groups.end(), [](const MdGroup& a, const MdGroup& b) {
        return std::tie(a.startTrack, a.endTrack) < std::tie(b.startTrack, b.endTrack);
    });
}

struct RawPartRef {
    int trackIndex = 0;
    int partIndex = 0;
    MdCodecMode codec = MdCodecMode::spStereo;
    std::int64_t rawStart = 0;
    std::int64_t rawEnd = 0;
    std::int64_t qStart = 0;
    std::int64_t qEnd = 0;
};

std::int64_t snapToOnDiscRegionGrid(const MdOnDiscRegion& r, std::int64_t sample) noexcept {
    const std::int64_t u = mdSamplesPerUnit(r.codec);
    const auto units = static_cast<std::int64_t>(
        std::llround(static_cast<double>(sample - r.timelineStartSample) / static_cast<double>(u)));
    return std::clamp(r.timelineStartSample + units * u, r.timelineStartSample, r.timelineEndSample);
}

bool appendUncoveredLoadedSyncTracks(const std::vector<std::pair<std::int64_t, std::int64_t>>& covered,
                                     const std::vector<std::tuple<std::int64_t, std::int64_t, std::string>>& items,
                                     MdDisc& candidate, std::string& error) {
    for (const auto& [st, en, title] : items) {
        if (findMdOnDiscRegionForPart(candidate.loadedDisc, {st, en}) != -1) {
            continue;
        }
        bool alreadyCovered = false;
        for (const auto& c : covered) {
            if (st >= c.first && en <= c.second) {
                alreadyCovered = true;
                break;
            }
        }
        if (alreadyCovered) {
            continue;
        }
        if (static_cast<int>(candidate.tracks.size()) >= kMdMaxTracks) {
            error = "Loaded disc plus new clips exceeds the maximum 255 tracks.";
            return false;
        }
        MdTrack tr;
        tr.id = makeMdTrackId();
        tr.halfWidthTitle = title.empty() ? ("Track " + std::to_string(candidate.tracks.size() + 1)) : title;
        tr.parts.push_back({st, en});
        candidate.tracks.push_back(std::move(tr));
    }
    return true;
}

std::vector<std::pair<std::int64_t, std::int64_t>> collectExistingPartBounds(const MdDisc& disc) {
    std::vector<std::pair<std::int64_t, std::int64_t>> bounds;
    for (const auto& tr : disc.tracks) {
        for (const auto& p : tr.parts) {
            bounds.emplace_back(p.startSample, p.endSample);
        }
    }
    return bounds;
}

} // namespace

// ============================================================================
// Medium & Codec Metadata
// ============================================================================

std::string mdMediumToString(MdMedium medium) {
    switch (medium) {
    case MdMedium::md60:
        return "md60";
    case MdMedium::md74:
        return "md74";
    case MdMedium::md80:
        return "md80";
    case MdMedium::himd300:
        return "himd300";
    case MdMedium::himd1gb:
        return "himd1gb";
    }
    return "md80";
}

bool parseMdMedium(const std::string& text, MdMedium& out) noexcept {
    const auto lower = toLowerAscii(text);
    if (lower == "md60" || lower == "60") {
        out = MdMedium::md60;
        return true;
    }
    if (lower == "md74" || lower == "74") {
        out = MdMedium::md74;
        return true;
    }
    if (lower == "md80" || lower == "80") {
        out = MdMedium::md80;
        return true;
    }
    if (lower == "himd300" || lower == "himd-300" || lower == "himd") {
        out = MdMedium::himd300;
        return true;
    }
    if (lower == "himd1gb" || lower == "himd-1gb" || lower == "1gb") {
        out = MdMedium::himd1gb;
        return true;
    }
    return false;
}

bool isHiMdMedium(MdMedium medium) noexcept {
    return medium == MdMedium::himd300 || medium == MdMedium::himd1gb;
}

bool isHiMdOnlyCodec(MdCodecMode mode) noexcept {
    return mode == MdCodecMode::hiSp || mode == MdCodecMode::hiLp || mode == MdCodecMode::pcm;
}

bool isStandardMdOnlyCodec(MdCodecMode mode) noexcept {
    return mode == MdCodecMode::spStereo || mode == MdCodecMode::spMono;
}

bool isCodecValidForMedium(MdMedium medium, MdCodecMode mode) noexcept {
    if (isHiMdMedium(medium)) {
        return !isStandardMdOnlyCodec(mode);
    }
    return !isHiMdOnlyCodec(mode);
}

std::int64_t hiMdMediumCapacityBytes(MdMedium medium) noexcept {
    switch (medium) {
    case MdMedium::himd300:
        return 283213824LL;
    case MdMedium::himd1gb:
        return 985000000LL;
    case MdMedium::md60:
    case MdMedium::md74:
    case MdMedium::md80:
        return 0;
    }
    return 0;
}

std::int64_t estimateHiMdTrackBytes(MdCodecMode mode, std::int64_t samples) noexcept {
    if (samples <= 0) {
        return 0;
    }
    std::int64_t rawBytes = 0;
    switch (mode) {
    case MdCodecMode::hiSp: {
        const std::int64_t frames = ((samples + 2047) / 2048) + 2;
        rawBytes = frames * 1488LL;
        break;
    }
    case MdCodecMode::hiLp: {
        const std::int64_t frames = ((samples + 2047) / 2048) + 2;
        rawBytes = frames * 376LL;
        break;
    }
    case MdCodecMode::lp2: {
        const std::int64_t frames = ((samples + 1023) / 1024) + 3;
        rawBytes = frames * 384LL;
        break;
    }
    case MdCodecMode::lp4: {
        const std::int64_t frames = ((samples + 1023) / 1024) + 3;
        rawBytes = frames * 192LL;
        break;
    }
    case MdCodecMode::pcm:
        rawBytes = samples * 4LL;
        break;
    case MdCodecMode::spStereo:
    case MdCodecMode::spMono:
        return 0;
    }
    if (rawBytes <= 0) {
        return 0;
    }
    constexpr std::int64_t kHiMdPayloadPerBlock = 16320LL;
    constexpr std::int64_t kHiMdBlockBytes = 16384LL;
    constexpr std::int64_t kFatClusterBytes = 32768LL;
    const std::int64_t blocks = (rawBytes + kHiMdPayloadPerBlock - 1) / kHiMdPayloadPerBlock;
    const std::int64_t atdataBytes = blocks * kHiMdBlockBytes;
    return ((atdataBytes + kFatClusterBytes - 1) / kFatClusterBytes) * kFatClusterBytes;
}

int mdTotalClusters(MdMedium medium) noexcept {
    switch (medium) {
    case MdMedium::md60:
        return 1762;
    case MdMedium::md74:
        return 2173;
    case MdMedium::md80:
        return 2350;
    case MdMedium::himd300:
    case MdMedium::himd1gb:
        return 0;
    }
    return 2350;
}

std::string mdCodecModeToString(MdCodecMode mode) {
    switch (mode) {
    case MdCodecMode::spStereo:
        return "sp-stereo";
    case MdCodecMode::spMono:
        return "sp-mono";
    case MdCodecMode::lp2:
        return "lp2";
    case MdCodecMode::lp4:
        return "lp4";
    case MdCodecMode::hiSp:
        return "hi-sp";
    case MdCodecMode::hiLp:
        return "hi-lp";
    case MdCodecMode::pcm:
        return "pcm";
    }
    return "sp-stereo";
}

bool parseMdCodecMode(const std::string& text, MdCodecMode& out) noexcept {
    const auto lower = toLowerAscii(text);
    if (lower == "sp-stereo" || lower == "sp") {
        out = MdCodecMode::spStereo;
        return true;
    }
    if (lower == "sp-mono" || lower == "mono") {
        out = MdCodecMode::spMono;
        return true;
    }
    if (lower == "lp2") {
        out = MdCodecMode::lp2;
        return true;
    }
    if (lower == "lp4") {
        out = MdCodecMode::lp4;
        return true;
    }
    if (lower == "hi-sp" || lower == "hisp" || lower == "at3p-256") {
        out = MdCodecMode::hiSp;
        return true;
    }
    if (lower == "hi-lp" || lower == "hilp" || lower == "at3p-64") {
        out = MdCodecMode::hiLp;
        return true;
    }
    if (lower == "pcm" || lower == "lpcm") {
        out = MdCodecMode::pcm;
        return true;
    }
    return false;
}

std::int64_t mdFrameQuantumSamples(MdCodecMode mode) noexcept {
    switch (mode) {
    case MdCodecMode::spStereo:
    case MdCodecMode::spMono:
        return 512;
    case MdCodecMode::lp2:
    case MdCodecMode::lp4:
        return 1024;
    case MdCodecMode::hiSp:
    case MdCodecMode::hiLp:
        return 2048;
    case MdCodecMode::pcm:
        return 1;
    }
    return 512;
}

int mdUnitsPerQuantum(MdCodecMode mode) noexcept {
    switch (mode) {
    case MdCodecMode::spStereo:
    case MdCodecMode::lp2:
        return 2;
    case MdCodecMode::spMono:
    case MdCodecMode::lp4:
    case MdCodecMode::hiSp:
    case MdCodecMode::hiLp:
    case MdCodecMode::pcm:
        return 1;
    }
    return 2;
}

std::int64_t mdSamplesPerUnit(MdCodecMode mode) noexcept {
    switch (mode) {
    case MdCodecMode::spStereo:
        return 256;
    case MdCodecMode::spMono:
    case MdCodecMode::lp2:
        return 512;
    case MdCodecMode::lp4:
        return 1024;
    case MdCodecMode::hiSp:
    case MdCodecMode::hiLp:
        return 2048;
    case MdCodecMode::pcm:
        return 1;
    }
    return 256;
}

int mdChannelsForCodec(MdCodecMode mode) noexcept {
    return mode == MdCodecMode::spMono ? 1 : 2;
}

MdCodecMode effectiveTrackCodec(const MdDisc& disc, const MdTrack& track) noexcept {
    return track.codecOverride.value_or(disc.defaultCodec);
}

// ============================================================================
// Validation & Physical Span Planning
// ============================================================================

bool isDefaultMdDisc(const MdDisc& disc) noexcept {
    if (disc.loadedDisc.active) {
        return false;
    }
    for (const auto& s : disc.importedUtocSectors) {
        if (!s.empty()) {
            return false;
        }
    }
    return disc.medium == MdMedium::md80 && disc.defaultCodec == MdCodecMode::spStereo &&
           disc.halfWidthTitle.empty() && disc.fullWidthTitle.empty() &&
           disc.discTimestamp.empty() && disc.autoTrackOnImport && disc.tracks.empty() &&
           disc.groups.empty();
}

int findMdOnDiscRegionForPart(const MdLoadedDiscState& loaded, const MdPart& part) noexcept {
    if (!loaded.active || part.endSample <= part.startSample) {
        return -1;
    }
    for (std::size_t i = 0; i < loaded.regions.size(); ++i) {
        const auto& r = loaded.regions[i];
        if (part.endSample <= r.timelineStartSample || part.startSample >= r.timelineEndSample) {
            continue;
        }
        if (part.startSample >= r.timelineStartSample && part.endSample <= r.timelineEndSample) {
            return static_cast<int>(i);
        }
        return -2;
    }
    return -1;
}

bool mdTrackUsesOnDiscAudio(const MdDisc& disc, const MdTrack& track) noexcept {
    if (!disc.loadedDisc.active) {
        return false;
    }
    for (const auto& p : track.parts) {
        if (findMdOnDiscRegionForPart(disc.loadedDisc, p) >= 0) {
            return true;
        }
    }
    return false;
}

int countWholeFreeClusters(const std::vector<MdPhysRange>& ranges) noexcept {
    int total = 0;
    for (const auto& r : ranges) {
        const std::uint16_t firstWhole =
            (r.start.sector == 0 && r.start.group == 0)
                ? r.start.cluster
                : static_cast<std::uint16_t>(r.start.cluster + 1);
        const std::uint16_t lastWhole =
            (r.end.sector == kMdSectorsPerCluster - 1 && r.end.group == kMdUnitsPerSector - 1)
                ? r.end.cluster
                : (r.end.cluster > 0 ? static_cast<std::uint16_t>(r.end.cluster - 1) : 0);
        if (lastWhole >= firstWhole) {
            total += static_cast<int>(lastWhole - firstWhole + 1);
        }
    }
    return total;
}

std::string makeMdTrackId() {
    static std::atomic<std::uint64_t> seq{1};
    const auto n = seq.fetch_add(1, std::memory_order_relaxed);
    return "md-" +
           juce::Uuid()
               .toString()
               .removeCharacters("-")
               .toLowerCase()
               .substring(0, 8)
               .toStdString() +
           "-" + std::to_string(n);
}

bool validateMdDisc(const MdDisc& disc, std::string& error) {
    error.clear();
    if (!validTimestampOrEmpty(disc.discTimestamp)) {
        error = "Disc timestamp must be empty or formatted YYYY-MM-DD HH:MM:SS.";
        return false;
    }
    if (!isCodecValidForMedium(disc.medium, disc.defaultCodec)) {
        error = "Codec '" + mdCodecModeToString(disc.defaultCodec) +
                "' is not supported on medium '" + mdMediumToString(disc.medium) + "'.";
        return false;
    }
    if (static_cast<int>(disc.tracks.size()) > kMdMaxTracks) {
        error = "MiniDisc UTOC supports at most 255 tracks.";
        return false;
    }

    if (disc.loadedDisc.active) {
        std::vector<std::pair<std::int64_t, std::int64_t>> tlIntervals;
        tlIntervals.reserve(disc.loadedDisc.regions.size());
        for (std::size_t i = 0; i < disc.loadedDisc.regions.size(); ++i) {
            const auto& r = disc.loadedDisc.regions[i];
            if (isHiMdMedium(disc.medium)) {
                if (r.id.empty() || r.timelineStartSample < 0 ||
                    r.timelineEndSample <= r.timelineStartSample) {
                    error = "Loaded Hi-MD region " + std::to_string(i + 1) +
                            " is empty or has invalid timeline bounds.";
                    return false;
                }
                tlIntervals.emplace_back(r.timelineStartSample, r.timelineEndSample);
                continue;
            }
            if (r.id.empty() || r.unitCount <= 0) {
                error = "Loaded disc region " + std::to_string(i + 1) + " is empty or missing ID.";
                return false;
            }
            const std::int64_t pStart = r.physStart.toLinearUnits();
            const std::int64_t pEnd = r.physEnd.toLinearUnits();
            if (pEnd < pStart || (pEnd - pStart + 1) != r.unitCount) {
                error = "Loaded disc region '" + r.id + "' has inconsistent physical bounds.";
                return false;
            }
            const std::int64_t expectedSamples =
                static_cast<std::int64_t>(r.unitCount) * mdSamplesPerUnit(r.codec);
            if (r.timelineStartSample < 0 ||
                (r.timelineEndSample - r.timelineStartSample) != expectedSamples) {
                error = "Loaded disc region '" + r.id + "' has inconsistent timeline sample range.";
                return false;
            }
            tlIntervals.emplace_back(r.timelineStartSample, r.timelineEndSample);
        }
        std::sort(tlIntervals.begin(), tlIntervals.end());
        for (std::size_t i = 1; i < tlIntervals.size(); ++i) {
            if (tlIntervals[i].first < tlIntervals[i - 1].second) {
                error = "Loaded disc regions overlap on the timeline.";
                return false;
            }
        }
    }

    std::set<std::string> seenIds;
    std::size_t totalParts = 0;
    for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
        const auto& tr = disc.tracks[t];
        if (tr.id.empty()) {
            error = "Track " + std::to_string(t + 1) + " is missing a stable ID.";
            return false;
        }
        if (!seenIds.insert(tr.id).second) {
            error = "Duplicate MD track ID '" + tr.id + "'.";
            return false;
        }
        if (!validTimestampOrEmpty(tr.timestamp)) {
            error = "Track " + std::to_string(t + 1) +
                    " timestamp must be empty or formatted YYYY-MM-DD HH:MM:SS.";
            return false;
        }
        if (!isCodecValidForMedium(disc.medium, effectiveTrackCodec(disc, tr))) {
            error = "Track " + std::to_string(t + 1) + " codec '" +
                    mdCodecModeToString(effectiveTrackCodec(disc, tr)) +
                    "' is not supported on medium '" + mdMediumToString(disc.medium) + "'.";
            return false;
        }
        if (tr.parts.empty()) {
            error = "Track " + std::to_string(t + 1) + " must contain at least one timeline part.";
            return false;
        }
        for (std::size_t p = 0; p < tr.parts.size(); ++p) {
            const auto& part = tr.parts[p];
            if (part.startSample < 0 || part.endSample <= part.startSample) {
                error = "Track " + std::to_string(t + 1) + " part " + std::to_string(p + 1) +
                        " must have 0 <= startSample < endSample.";
                return false;
            }
            if (disc.loadedDisc.active) {
                const int rIdx = findMdOnDiscRegionForPart(disc.loadedDisc, part);
                if (rIdx == -2) {
                    error = "Track " + std::to_string(t + 1) + " part " + std::to_string(p + 1) +
                            " spans across an on-disc region boundary; split into separate parts.";
                    return false;
                }
                if (rIdx >= 0) {
                    const auto regionCodec =
                        disc.loadedDisc.regions[static_cast<std::size_t>(rIdx)].codec;
                    if (effectiveTrackCodec(disc, tr) != regionCodec) {
                        error = "On-disc track " + std::to_string(t + 1) +
                                " cannot change codec (" + mdCodecModeToString(regionCodec) +
                                ") without re-encoding.";
                        return false;
                    }
                }
            }
            ++totalParts;
        }
    }
    if (totalParts > 254 && disc.tracks.size() >= 255) {
        // Fine check happens during UTOC DAG fragment compilation; still catch impossible totals
        // early.
    }

    const int nTracks = static_cast<int>(disc.tracks.size());
    std::vector<MdGroup> sorted = disc.groups;
    normalizeGroups(sorted);
    int prevEnd = 0;
    for (const auto& g : sorted) {
        if (g.startTrack < 1 || g.endTrack < g.startTrack || g.endTrack > nTracks) {
            error = "Group track range " + std::to_string(g.startTrack) + "-" +
                    std::to_string(g.endTrack) + " is outside valid track indices 1.." +
                    std::to_string(nTracks) + ".";
            return false;
        }
        if (g.startTrack <= prevEnd) {
            error = "Track groups must cover disjoint track ranges.";
            return false;
        }
        prevEnd = g.endTrack;
    }
    return true;
}

bool planMdPhysicalAudio(const MdDisc& disc, MdPhysicalPlan& out, std::string& error) {
    out = {};
    const int physicalTotal =
        disc.loadedDisc.active ? disc.loadedDisc.physicalTotalClusters() : 0;
    out.totalClusters = physicalTotal > 0 ? physicalTotal : mdTotalClusters(disc.medium);
    if (!validateMdDisc(disc, error)) {
        return false;
    }

    if (isHiMdMedium(disc.medium)) {
        std::vector<std::pair<std::int64_t, std::int64_t>> intervals;
        for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
            const auto& tr = disc.tracks[t];
            const auto codec = effectiveTrackCodec(disc, tr);
            for (std::size_t p = 0; p < tr.parts.size(); ++p) {
                const auto& part = tr.parts[p];
                out.maxEndSample = std::max(out.maxEndSample, part.endSample);
                out.logicalPlaySamples += part.durationSamples();
                intervals.emplace_back(part.startSample, part.endSample);

                MdPhysicalSpan span;
                span.codec = codec;
                span.startSample = part.startSample;
                span.endSample = part.endSample;
                span.unitCount = 0;
                span.startCluster = 0;
                span.clusterCount = 0;
                const int spanIdx = static_cast<int>(out.spans.size());
                out.spans.push_back(span);

                MdPartPlacement place;
                place.trackIndex = static_cast<int>(t);
                place.partIndex = static_cast<int>(p);
                place.spanIndex = spanIdx;
                out.placements.push_back(place);
            }
        }
        if (!intervals.empty()) {
            std::sort(intervals.begin(), intervals.end());
            std::int64_t curS = intervals.front().first;
            std::int64_t curE = intervals.front().second;
            for (std::size_t i = 1; i < intervals.size(); ++i) {
                if (intervals[i].first <= curE) {
                    curE = std::max(curE, intervals[i].second);
                } else {
                    out.uniquePhysicalSamples += (curE - curS);
                    curS = intervals[i].first;
                    curE = intervals[i].second;
                }
            }
            out.uniquePhysicalSamples += (curE - curS);
        }
        return true;
    }

    std::vector<RawPartRef> localRefs;
    std::vector<std::vector<std::pair<int, int>>> onDiscIntervalsByRegion(
        disc.loadedDisc.active ? disc.loadedDisc.regions.size() : 0);
    std::set<std::uint16_t> keptOccupiedClusters;

    std::size_t totalPlacements = 0;
    for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
        totalPlacements += disc.tracks[t].parts.size();
    }
    out.placements.resize(totalPlacements);

    std::size_t flatIdx = 0;
    for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
        const auto& tr = disc.tracks[t];
        const auto codec = effectiveTrackCodec(disc, tr);
        const std::int64_t q = mdFrameQuantumSamples(codec);
        const int upq = mdUnitsPerQuantum(codec);

        for (std::size_t p = 0; p < tr.parts.size(); ++p) {
            const auto& part = tr.parts[p];
            out.maxEndSample = std::max(out.maxEndSample, part.endSample);
            out.logicalPlaySamples += part.durationSamples();

            const int rIdx = disc.loadedDisc.active
                                 ? findMdOnDiscRegionForPart(disc.loadedDisc, part)
                                 : -1;
            if (rIdx >= 0) {
                const auto& reg = disc.loadedDisc.regions[static_cast<std::size_t>(rIdx)];
                const std::int64_t unitSamples = mdSamplesPerUnit(reg.codec);
                const std::int64_t relStart = part.startSample - reg.timelineStartSample;
                const std::int64_t relEnd = part.endSample - reg.timelineStartSample;
                const int uStart = static_cast<int>(
                    std::clamp<std::int64_t>(relStart / unitSamples, 0, reg.unitCount - 1));
                const int uEndExcl = static_cast<int>(std::clamp<std::int64_t>(
                    (relEnd + unitSamples - 1) / unitSamples, uStart + 1, reg.unitCount));
                const int partUnits = uEndExcl - uStart;
                const std::int64_t regBaseLin = reg.physStart.toLinearUnits();
                const auto sAddr = MdPhysAddr::fromLinearUnits(regBaseLin + uStart);
                const auto eAddr = MdPhysAddr::fromLinearUnits(regBaseLin + uEndExcl - 1);

                MdPartPlacement place;
                place.trackIndex = static_cast<int>(t);
                place.partIndex = static_cast<int>(p);
                place.spanIndex = -1;
                place.unitOffsetInSpan = 0;
                place.unitCount = partUnits;
                place.startAddr = sAddr;
                place.endAddr = eAddr;
                place.isOnDisc = true;
                place.onDiscRegionIndex = rIdx;
                place.rawModeByte = reg.rawModeByte;
                out.placements[flatIdx++] = place;

                out.uncoalescedClusters +=
                    (partUnits + kMdUnitsPerCluster - 1) / kMdUnitsPerCluster;
                onDiscIntervalsByRegion[static_cast<std::size_t>(rIdx)].emplace_back(uStart,
                                                                                     uEndExcl);
                for (std::uint16_t c = sAddr.cluster; c <= eAddr.cluster; ++c) {
                    keptOccupiedClusters.insert(c);
                }
            } else {
                RawPartRef r;
                r.trackIndex = static_cast<int>(t);
                r.partIndex = static_cast<int>(p);
                r.codec = codec;
                r.rawStart = part.startSample;
                r.rawEnd = part.endSample;
                r.qStart = (part.startSample / q) * q;
                r.qEnd = ((part.endSample + q - 1) / q) * q;
                localRefs.push_back(r);

                const int partUnits = static_cast<int>(((r.qEnd - r.qStart) / q) * upq);
                out.uncoalescedClusters +=
                    (partUnits + kMdUnitsPerCluster - 1) / kMdUnitsPerCluster;
                ++flatIdx;
            }
        }
    }

    if (disc.loadedDisc.active) {
        out.onDiscClusters = static_cast<int>(keptOccupiedClusters.size());
        out.usedClusters = out.onDiscClusters;
        for (std::size_t ri = 0; ri < onDiscIntervalsByRegion.size(); ++ri) {
            auto& ivs = onDiscIntervalsByRegion[ri];
            if (ivs.empty()) {
                continue;
            }
            std::sort(ivs.begin(), ivs.end());
            int curS = ivs.front().first;
            int curE = ivs.front().second;
            int regionUniqueUnits = 0;
            for (std::size_t j = 1; j < ivs.size(); ++j) {
                if (ivs[j].first <= curE) {
                    curE = std::max(curE, ivs[j].second);
                } else {
                    regionUniqueUnits += (curE - curS);
                    curS = ivs[j].first;
                    curE = ivs[j].second;
                }
            }
            regionUniqueUnits += (curE - curS);
            out.uniquePhysicalSamples += static_cast<std::int64_t>(regionUniqueUnits) *
                                         mdSamplesPerUnit(disc.loadedDisc.regions[ri].codec);
        }
    }

    // Group new local refs by codec and sort by outward-aligned interval [qStart, qEnd).
    if (!localRefs.empty()) {
        std::map<std::pair<int, int>, std::size_t> placementIndexByTrackPart;
        std::size_t idx = 0;
        for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
            for (std::size_t p = 0; p < disc.tracks[t].parts.size(); ++p) {
                placementIndexByTrackPart.emplace(
                    std::make_pair(static_cast<int>(t), static_cast<int>(p)), idx++);
            }
        }

        std::map<MdCodecMode, std::vector<RawPartRef>> byCodec;
        for (const auto& r : localRefs) {
            byCodec[r.codec].push_back(r);
        }

        struct PendingSpan {
            MdCodecMode codec;
            std::int64_t startSample = 0;
            std::int64_t endSample = 0;
            int earliestTrackIndex = 0;
            int earliestPartIndex = 0;
            std::vector<RawPartRef> members;
        };
        std::vector<PendingSpan> pendingSpans;

        for (auto& [codec, list] : byCodec) {
            std::sort(list.begin(), list.end(), [](const RawPartRef& a, const RawPartRef& b) {
                return std::tie(a.qStart, a.qEnd, a.trackIndex, a.partIndex) <
                       std::tie(b.qStart, b.qEnd, b.trackIndex, b.partIndex);
            });
            for (const auto& r : list) {
                if (pendingSpans.empty() || pendingSpans.back().codec != codec ||
                    r.qStart > pendingSpans.back().endSample) {
                    PendingSpan s;
                    s.codec = codec;
                    s.startSample = r.qStart;
                    s.endSample = r.qEnd;
                    s.earliestTrackIndex = r.trackIndex;
                    s.earliestPartIndex = r.partIndex;
                    s.members.push_back(r);
                    pendingSpans.push_back(std::move(s));
                } else {
                    auto& s = pendingSpans.back();
                    s.endSample = std::max(s.endSample, r.qEnd);
                    if (std::tie(r.trackIndex, r.partIndex) <
                        std::tie(s.earliestTrackIndex, s.earliestPartIndex)) {
                        s.earliestTrackIndex = r.trackIndex;
                        s.earliestPartIndex = r.partIndex;
                    }
                    s.members.push_back(r);
                }
            }
        }

        // Order physical spans in track presentation order so sequential discs read sequentially
        // on cluster map.
        std::sort(pendingSpans.begin(), pendingSpans.end(),
                  [](const PendingSpan& a, const PendingSpan& b) {
                      return std::tie(a.earliestTrackIndex, a.earliestPartIndex, a.startSample) <
                             std::tie(b.earliestTrackIndex, b.earliestPartIndex, b.startSample);
                  });

        std::uint16_t nextCluster =
            disc.loadedDisc.active
                ? static_cast<std::uint16_t>(disc.loadedDisc.physicalStartCluster +
                                             out.onDiscClusters)
                : kMdFirstAudioCluster;
        for (std::size_t sIdx = 0; sIdx < pendingSpans.size(); ++sIdx) {
            const auto& ps = pendingSpans[sIdx];
            const std::int64_t q = mdFrameQuantumSamples(ps.codec);
            const int upq = mdUnitsPerQuantum(ps.codec);
            const int totalQuanta = static_cast<int>((ps.endSample - ps.startSample) / q);
            const int spanUnits = totalQuanta * upq;
            const int spanClusters = (spanUnits + kMdUnitsPerCluster - 1) / kMdUnitsPerCluster;

            // Collect internal abutting boundary sample positions (where one part's exact rawEnd
            // equals another part's exact rawStart within the same coalesced span) so split points
            // land cleanly on one Sound Group boundary with zero gap and zero duplicated 11.6 ms
            // frame.
            std::set<std::int64_t> rawStarts;
            std::set<std::int64_t> rawEnds;
            std::set<std::int64_t> exactSplits;
            for (const auto& m : ps.members) {
                rawStarts.insert(m.rawStart);
                rawEnds.insert(m.rawEnd);
            }
            for (const auto e : rawEnds) {
                if (rawStarts.count(e)) {
                    exactSplits.insert(e);
                }
            }

            MdPhysicalSpan span;
            span.codec = ps.codec;
            span.startSample = ps.startSample;
            span.endSample = ps.endSample;
            span.unitCount = spanUnits;
            span.startCluster = nextCluster;
            span.clusterCount = spanClusters;
            out.spans.push_back(span);

            out.uniquePhysicalSamples += (ps.endSample - ps.startSample);
            out.newUploadClusters += spanClusters;
            out.usedClusters += spanClusters;

            const std::int64_t spanBaseLinearUnit =
                static_cast<std::int64_t>(nextCluster) * kMdUnitsPerCluster;

            for (const auto& m : ps.members) {
                int startQuanta = 0;
                if (exactSplits.count(m.rawStart)) {
                    startQuanta = static_cast<int>((m.rawStart - ps.startSample + q / 2) / q);
                    startQuanta = std::clamp(startQuanta, 0, std::max(0, totalQuanta - 1));
                } else {
                    startQuanta = static_cast<int>((m.rawStart - ps.startSample) / q);
                }

                int endQuanta = totalQuanta;
                if (exactSplits.count(m.rawEnd)) {
                    endQuanta = static_cast<int>((m.rawEnd - ps.startSample + q / 2) / q);
                    endQuanta = std::clamp(endQuanta, startQuanta + 1, totalQuanta);
                } else {
                    endQuanta = static_cast<int>((m.rawEnd - ps.startSample + q - 1) / q);
                    endQuanta = std::clamp(endQuanta, startQuanta + 1, totalQuanta);
                }

                const int partUnitOffset = startQuanta * upq;
                const int partUnitCount = (endQuanta - startQuanta) * upq;

                MdPartPlacement place;
                place.trackIndex = m.trackIndex;
                place.partIndex = m.partIndex;
                place.spanIndex = static_cast<int>(sIdx);
                place.unitOffsetInSpan = partUnitOffset;
                place.unitCount = partUnitCount;
                place.startAddr = MdPhysAddr::fromLinearUnits(spanBaseLinearUnit + partUnitOffset);
                place.endAddr = MdPhysAddr::fromLinearUnits(spanBaseLinearUnit + partUnitOffset +
                                                            partUnitCount - 1);
                place.isOnDisc = false;
                place.onDiscRegionIndex = -1;
                place.rawModeByte = 0;

                const auto slot = placementIndexByTrackPart.at({m.trackIndex, m.partIndex});
                out.placements[slot] = place;
            }

            nextCluster = static_cast<std::uint16_t>(nextCluster + spanClusters);
        }
    }

    if (disc.loadedDisc.active) {
        std::set<std::uint16_t> initiallyUsedClusters;
        for (const auto& r : disc.loadedDisc.regions) {
            for (std::uint16_t c = r.physStart.cluster; c <= r.physEnd.cluster; ++c) {
                initiallyUsedClusters.insert(c);
            }
        }
        std::set<std::uint16_t> defectiveClusters;
        for (const auto& d : disc.loadedDisc.defectiveRanges) {
            for (std::uint16_t c = d.start.cluster; c <= d.end.cluster; ++c) {
                defectiveClusters.insert(c);
            }
        }
        std::set<std::uint16_t> reclaimedClusterSet;
        for (std::uint16_t c : initiallyUsedClusters) {
            if (!keptOccupiedClusters.count(c) && !defectiveClusters.count(c)) {
                reclaimedClusterSet.insert(c);
            }
        }
        out.reclaimedClusters = static_cast<int>(reclaimedClusterSet.size());

        std::vector<std::pair<std::int64_t, std::int64_t>> candidateFree;
        for (const auto& fr : disc.loadedDisc.initialFreeRanges) {
            const std::int64_t sLin = fr.start.toLinearUnits();
            const std::int64_t eLin = fr.end.toLinearUnits();
            if (eLin >= sLin) {
                candidateFree.emplace_back(sLin, eLin);
            }
        }
        if (!reclaimedClusterSet.empty()) {
            auto it = reclaimedClusterSet.begin();
            std::uint16_t runStart = *it;
            std::uint16_t runEnd = *it;
            for (++it; it != reclaimedClusterSet.end(); ++it) {
                if (*it == runEnd + 1) {
                    runEnd = *it;
                } else {
                    const MdPhysAddr a{runStart, 0, 0};
                    const MdPhysAddr b{runEnd, kMdSectorsPerCluster - 1, kMdUnitsPerSector - 1};
                    candidateFree.emplace_back(a.toLinearUnits(), b.toLinearUnits());
                    runStart = *it;
                    runEnd = *it;
                }
            }
            const MdPhysAddr a{runStart, 0, 0};
            const MdPhysAddr b{runEnd, kMdSectorsPerCluster - 1, kMdUnitsPerSector - 1};
            candidateFree.emplace_back(a.toLinearUnits(), b.toLinearUnits());
        }
        std::sort(candidateFree.begin(), candidateFree.end());
        for (const auto& iv : candidateFree) {
            if (out.reclaimedFreeRanges.empty() ||
                iv.first > out.reclaimedFreeRanges.back().end.toLinearUnits() + 1) {
                out.reclaimedFreeRanges.push_back(
                    {MdPhysAddr::fromLinearUnits(iv.first),
                     MdPhysAddr::fromLinearUnits(iv.second)});
            } else if (iv.second > out.reclaimedFreeRanges.back().end.toLinearUnits()) {
                out.reclaimedFreeRanges.back().end = MdPhysAddr::fromLinearUnits(iv.second);
            }
        }
        out.initialFreeClusters = countWholeFreeClusters(disc.loadedDisc.initialFreeRanges);
        out.freeClustersAfterReclaim = countWholeFreeClusters(out.reclaimedFreeRanges);
    }

    out.savedClusters = std::max(0, out.uncoalescedClusters - out.usedClusters);
    return true;
}

// ============================================================================
// Track, Part, & Group Mutations
// ============================================================================

int findMdTrackIndex(const MdDisc& disc, const std::string& idOrOneBasedIndex) noexcept {
    if (idOrOneBasedIndex.empty()) {
        return -1;
    }
    for (std::size_t i = 0; i < disc.tracks.size(); ++i) {
        if (disc.tracks[i].id == idOrOneBasedIndex) {
            return static_cast<int>(i);
        }
    }

    bool allDigits = true;
    for (char ch : idOrOneBasedIndex) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            allDigits = false;
            break;
        }
    }
    if (allDigits) {
        try {
            const long long num = std::stoll(idOrOneBasedIndex);
            if (num >= 1 && num <= static_cast<long long>(disc.tracks.size())) {
                return static_cast<int>(num - 1);
            }
        } catch (...) {
            return -1;
        }
    }
    return -1;
}

bool appendAutoTrackForClip(MdDisc& disc, const std::string& title, std::int64_t startSample,
                            std::int64_t endSample) {
    if (!disc.autoTrackOnImport || static_cast<int>(disc.tracks.size()) >= kMdMaxTracks) {
        return false;
    }
    if (startSample < 0 || endSample <= startSample) {
        return false;
    }

    MdTrack tr;
    tr.id = makeMdTrackId();
    tr.halfWidthTitle = normalizeAutoTrackTitle(title, disc.tracks.size() + 1);
    tr.parts.push_back({startSample, endSample});
    disc.tracks.push_back(std::move(tr));
    return true;
}

bool applyAutoTrackForPlacedClip(MdDisc& disc, const std::string& title, std::int64_t startSample,
                                 std::int64_t endSample) {
    if (!disc.autoTrackOnImport || static_cast<int>(disc.tracks.size()) >= kMdMaxTracks) {
        return false;
    }
    if (startSample < 0 || endSample <= startSample) {
        return false;
    }
    if (disc.loadedDisc.active &&
        findMdOnDiscRegionForPart(disc.loadedDisc, {startSample, endSample}) != -1) {
        return false;
    }

    std::int64_t maxMdEnd = 0;
    for (const auto& t : disc.tracks) {
        for (const auto& p : t.parts) {
            maxMdEnd = std::max(maxMdEnd, p.endSample);
        }
    }

    const std::string trimmed = normalizeAutoTrackTitle(title, disc.tracks.size() + 1);

    MdDisc candidate = disc;
    if (!candidate.tracks.empty() && !candidate.tracks.back().parts.empty()) {
        auto& lastPart = candidate.tracks.back().parts.back();
        const bool lastIsOnDisc =
            candidate.loadedDisc.active &&
            findMdOnDiscRegionForPart(candidate.loadedDisc, lastPart) >= 0;
        if (!lastIsOnDisc && startSample > lastPart.startSample && startSample < lastPart.endSample &&
            endSample > maxMdEnd) {
            lastPart.endSample = startSample;
        }
    }

    MdTrack tr;
    tr.id = makeMdTrackId();
    tr.halfWidthTitle = trimmed;
    tr.parts.push_back({startSample, endSample});
    candidate.tracks.push_back(std::move(tr));

    std::string ignoredErr;
    if (!validateMdDisc(candidate, ignoredErr)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool insertMdTrack(MdDisc& disc, int oneBasedPosition, MdTrack track, std::string& error) {
    error.clear();
    const int count = static_cast<int>(disc.tracks.size());
    if (count >= kMdMaxTracks) {
        error = "MiniDisc already has the maximum 255 tracks.";
        return false;
    }
    if (oneBasedPosition < 1 || oneBasedPosition > count + 1) {
        error = "Insert position must be between 1 and " + std::to_string(count + 1) + ".";
        return false;
    }
    if (track.id.empty()) {
        track.id = makeMdTrackId();
    }

    MdDisc candidate = disc;
    shiftGroupsForInsert(candidate.groups, oneBasedPosition, false);
    candidate.tracks.insert(candidate.tracks.begin() + (oneBasedPosition - 1), std::move(track));
    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool splitMdTrackAtSample(MdDisc& disc, int trackIndex0, std::int64_t timelineSample,
                          std::string& error) {
    error.clear();
    const int count = static_cast<int>(disc.tracks.size());
    if (trackIndex0 < 0 || trackIndex0 >= count) {
        error = "Track index out of range.";
        return false;
    }
    if (count >= kMdMaxTracks) {
        error = "MiniDisc already has the maximum 255 tracks.";
        return false;
    }

    const auto& src = disc.tracks[static_cast<std::size_t>(trackIndex0)];
    int hitPart = -1;
    for (std::size_t p = 0; p < src.parts.size(); ++p) {
        if (src.parts[p].startSample < timelineSample && timelineSample < src.parts[p].endSample) {
            hitPart = static_cast<int>(p);
            break;
        }
    }
    if (hitPart < 0) {
        error = "Playhead sample does not lie strictly inside any timeline part of track " +
                std::to_string(trackIndex0 + 1) + ".";
        return false;
    }

    MdDisc candidate = disc;
    auto& first = candidate.tracks[static_cast<std::size_t>(trackIndex0)];
    MdTrack second = first;
    second.id = makeMdTrackId();

    const MdPart splitPart = first.parts[static_cast<std::size_t>(hitPart)];
    std::int64_t splitSample = timelineSample;
    if (disc.loadedDisc.active) {
        const int rIdx = findMdOnDiscRegionForPart(disc.loadedDisc, splitPart);
        if (rIdx >= 0) {
            const auto& reg = disc.loadedDisc.regions[static_cast<std::size_t>(rIdx)];
            const std::int64_t u = mdSamplesPerUnit(reg.codec);
            splitSample = snapToOnDiscRegionGrid(reg, timelineSample);
            if (splitSample <= splitPart.startSample) {
                splitSample = splitPart.startSample + u;
            }
            if (splitSample >= splitPart.endSample) {
                splitSample = splitPart.endSample - u;
            }
            if (splitSample <= splitPart.startSample || splitSample >= splitPart.endSample) {
                error = "Part is too short (1 sound group) to split on on-disc audio.";
                return false;
            }
        }
    }
    first.parts.resize(static_cast<std::size_t>(hitPart));
    first.parts.push_back({splitPart.startSample, splitSample});

    second.parts.erase(second.parts.begin(), second.parts.begin() + hitPart + 1);
    second.parts.insert(second.parts.begin(), {splitSample, splitPart.endSample});

    if (!second.halfWidthTitle.empty()) {
        second.halfWidthTitle += " (2)";
    }
    if (!second.fullWidthTitle.empty()) {
        second.fullWidthTitle += " (2)";
    }

    const int insertPos1 = trackIndex0 + 2;
    shiftGroupsForInsert(candidate.groups, insertPos1, true);
    candidate.tracks.insert(candidate.tracks.begin() + trackIndex0 + 1, std::move(second));

    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool mergeMdTrackWithNext(MdDisc& disc, int trackIndex0, std::string& error) {
    error.clear();
    const int count = static_cast<int>(disc.tracks.size());
    if (trackIndex0 < 0 || trackIndex0 + 1 >= count) {
        error = "Track merge requires an adjacent following track.";
        return false;
    }

    MdDisc candidate = disc;
    auto& first = candidate.tracks[static_cast<std::size_t>(trackIndex0)];
    const auto second = candidate.tracks[static_cast<std::size_t>(trackIndex0 + 1)];
    for (const auto& p : second.parts) {
        if (!first.parts.empty() && first.parts.back().endSample == p.startSample) {
            first.parts.back().endSample = p.endSample;
        } else {
            first.parts.push_back(p);
        }
    }
    candidate.tracks.erase(candidate.tracks.begin() + trackIndex0 + 1);
    shiftGroupsForRemove(candidate.groups, trackIndex0 + 2);
    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool moveMdSharedBoundary(MdDisc& disc, int leftTrack0, int leftPart0, int rightTrack0,
                          int rightPart0, std::int64_t newSample, std::int64_t maxSample,
                          std::string& error) {
    error.clear();
    const int count = static_cast<int>(disc.tracks.size());
    if (leftTrack0 < 0 || leftTrack0 >= count || rightTrack0 < 0 || rightTrack0 >= count) {
        error = "Shared split track index out of range.";
        return false;
    }
    const auto& leftParts = disc.tracks[static_cast<std::size_t>(leftTrack0)].parts;
    const auto& rightParts = disc.tracks[static_cast<std::size_t>(rightTrack0)].parts;
    if (leftPart0 < 0 || leftPart0 >= static_cast<int>(leftParts.size()) || rightPart0 < 0 ||
        rightPart0 >= static_cast<int>(rightParts.size())) {
        error = "Shared split part index out of range.";
        return false;
    }
    if (leftTrack0 == rightTrack0 && leftPart0 == rightPart0) {
        error = "Shared split requires two distinct parts.";
        return false;
    }

    MdDisc candidate = disc;
    auto& leftPart = candidate.tracks[static_cast<std::size_t>(leftTrack0)]
                         .parts[static_cast<std::size_t>(leftPart0)];
    auto& rightPart = candidate.tracks[static_cast<std::size_t>(rightTrack0)]
                          .parts[static_cast<std::size_t>(rightPart0)];
    std::int64_t adjustedSample = newSample;
    if (disc.loadedDisc.active) {
        const int lr = findMdOnDiscRegionForPart(disc.loadedDisc, leftPart);
        const int rr = findMdOnDiscRegionForPart(disc.loadedDisc, rightPart);
        if (lr != rr && (lr >= 0 || rr >= 0)) {
            error = "Split boundary lies on a physical on-disc region edge and cannot cross "
                    "regions.";
            return false;
        }
        if (lr >= 0) {
            const auto& reg = disc.loadedDisc.regions[static_cast<std::size_t>(lr)];
            const std::int64_t u = mdSamplesPerUnit(reg.codec);
            adjustedSample = snapToOnDiscRegionGrid(reg, newSample);
            const std::int64_t lo = leftPart.startSample + u;
            const std::int64_t hi = std::min(maxSample, rightPart.endSample - u);
            if (hi < lo || adjustedSample < lo || adjustedSample > hi) {
                error = "Split boundary position must remain strictly inside the adjacent tracks.";
                return false;
            }
            leftPart.endSample = adjustedSample;
            rightPart.startSample = adjustedSample;
            if (!validateMdDisc(candidate, error)) {
                return false;
            }
            disc = std::move(candidate);
            return true;
        }
    }
    const std::int64_t lo = leftPart.startSample + 1;
    const std::int64_t hi = std::min(maxSample, rightPart.endSample - 1);
    if (hi < lo || adjustedSample < lo || adjustedSample > hi) {
        error = "Split boundary position must remain strictly inside the adjacent tracks.";
        return false;
    }
    leftPart.endSample = adjustedSample;
    rightPart.startSample = adjustedSample;
    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool trimMdPartEdge(MdDisc& disc, int trackIndex0, int partIndex0, bool rightEdge,
                    std::int64_t newSample, std::int64_t maxSample, std::string& error) {
    error.clear();
    if (trackIndex0 < 0 || trackIndex0 >= static_cast<int>(disc.tracks.size())) {
        error = "Track index out of range.";
        return false;
    }
    const auto& srcParts = disc.tracks[static_cast<std::size_t>(trackIndex0)].parts;
    if (partIndex0 < 0 || partIndex0 >= static_cast<int>(srcParts.size())) {
        error = "Part index out of range.";
        return false;
    }

    MdDisc candidate = disc;
    auto& part = candidate.tracks[static_cast<std::size_t>(trackIndex0)]
                     .parts[static_cast<std::size_t>(partIndex0)];
    std::int64_t minBound = 0;
    std::int64_t maxBound = maxSample;
    std::int64_t step = 1;
    std::int64_t adjustedSample = newSample;
    if (disc.loadedDisc.active) {
        const int rIdx = findMdOnDiscRegionForPart(disc.loadedDisc, part);
        if (rIdx >= 0) {
            const auto& reg = disc.loadedDisc.regions[static_cast<std::size_t>(rIdx)];
            step = mdSamplesPerUnit(reg.codec);
            minBound = reg.timelineStartSample;
            maxBound = std::min(maxSample, reg.timelineEndSample);
            adjustedSample = snapToOnDiscRegionGrid(reg, newSample);
        }
    }
    if (rightEdge) {
        const std::int64_t lo = part.startSample + step;
        const std::int64_t clamped = std::clamp(adjustedSample, lo, std::max(lo, maxBound));
        if (clamped <= part.startSample || clamped > maxBound) {
            error = "Part end sample is out of bounds.";
            return false;
        }
        part.endSample = clamped;
    } else {
        const std::int64_t hi = std::max(minBound, part.endSample - step);
        const std::int64_t clamped = std::clamp(adjustedSample, minBound, hi);
        if (clamped < minBound || clamped >= part.endSample) {
            error = "Part start sample is out of bounds.";
            return false;
        }
        part.startSample = clamped;
    }
    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool slideMdPartRange(MdDisc& disc, int trackIndex0, int partIndex0, std::int64_t newStartSample,
                      std::int64_t maxSample, std::string& error) {
    error.clear();
    if (trackIndex0 < 0 || trackIndex0 >= static_cast<int>(disc.tracks.size())) {
        error = "Track index out of range.";
        return false;
    }
    const auto& srcParts = disc.tracks[static_cast<std::size_t>(trackIndex0)].parts;
    if (partIndex0 < 0 || partIndex0 >= static_cast<int>(srcParts.size())) {
        error = "Part index out of range.";
        return false;
    }

    MdDisc candidate = disc;
    auto& part = candidate.tracks[static_cast<std::size_t>(trackIndex0)]
                     .parts[static_cast<std::size_t>(partIndex0)];
    const std::int64_t len = part.endSample - part.startSample;
    std::int64_t minBound = 0;
    std::int64_t maxBound = maxSample;
    std::int64_t adjustedStart = newStartSample;
    if (disc.loadedDisc.active) {
        const int rIdx = findMdOnDiscRegionForPart(disc.loadedDisc, part);
        if (rIdx >= 0) {
            const auto& reg = disc.loadedDisc.regions[static_cast<std::size_t>(rIdx)];
            minBound = reg.timelineStartSample;
            maxBound = std::min(maxSample, reg.timelineEndSample);
            adjustedStart = snapToOnDiscRegionGrid(reg, newStartSample);
        }
    }
    if (len <= 0 || maxBound < minBound + len) {
        error = "Part length exceeds timeline range.";
        return false;
    }
    const std::int64_t clamped = std::clamp<std::int64_t>(adjustedStart, minBound, maxBound - len);
    part.startSample = clamped;
    part.endSample = clamped + len;
    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool duplicateMdTrack(MdDisc& disc, int trackIndex0, std::string& error) {
    error.clear();
    const int count = static_cast<int>(disc.tracks.size());
    if (trackIndex0 < 0 || trackIndex0 >= count) {
        error = "Track index out of range.";
        return false;
    }
    if (count >= kMdMaxTracks) {
        error = "MiniDisc already has the maximum 255 tracks.";
        return false;
    }

    MdDisc candidate = disc;
    MdTrack copy = candidate.tracks[static_cast<std::size_t>(trackIndex0)];
    copy.id = makeMdTrackId();
    const int insertPos1 = trackIndex0 + 2;
    shiftGroupsForInsert(candidate.groups, insertPos1, true);
    candidate.tracks.insert(candidate.tracks.begin() + trackIndex0 + 1, std::move(copy));
    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool cloneMdTrackToPosition(MdDisc& disc, int srcIndex0, int destIndex0, std::string& error) {
    error.clear();
    const int count = static_cast<int>(disc.tracks.size());
    if (srcIndex0 < 0 || srcIndex0 >= count) {
        error = "Source track index out of range.";
        return false;
    }
    if (destIndex0 < 0 || destIndex0 > count) {
        error = "Destination track index out of range.";
        return false;
    }
    MdTrack copy = disc.tracks[static_cast<std::size_t>(srcIndex0)];
    copy.id = makeMdTrackId();
    return insertMdTrack(disc, destIndex0 + 1, std::move(copy), error);
}

bool moveMdTrackPart(MdDisc& disc, int trackIndex0, int fromPart0, int toPart0, std::string& error) {
    error.clear();
    if (trackIndex0 < 0 || trackIndex0 >= static_cast<int>(disc.tracks.size())) {
        error = "Track index out of range.";
        return false;
    }
    const int partCount =
        static_cast<int>(disc.tracks[static_cast<std::size_t>(trackIndex0)].parts.size());
    if (fromPart0 < 0 || fromPart0 >= partCount || toPart0 < 0 || toPart0 >= partCount) {
        error = "Part index out of range.";
        return false;
    }
    if (fromPart0 == toPart0) {
        return true;
    }

    MdDisc candidate = disc;
    auto& parts = candidate.tracks[static_cast<std::size_t>(trackIndex0)].parts;
    MdPart moved = parts[static_cast<std::size_t>(fromPart0)];
    parts.erase(parts.begin() + fromPart0);
    parts.insert(parts.begin() + toPart0, moved);
    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool moveMdTrackBlock(MdDisc& disc, int fromStart0, int fromEnd0, int destStart0,
                      std::string& error) {
    error.clear();
    const int count = static_cast<int>(disc.tracks.size());
    if (fromStart0 < 0 || fromEnd0 < fromStart0 || fromEnd0 >= count) {
        error = "Track block range out of range.";
        return false;
    }
    const int blockLen = fromEnd0 - fromStart0 + 1;
    if (destStart0 < 0 || destStart0 > count - blockLen) {
        error = "Track block destination out of range.";
        return false;
    }
    if (destStart0 == fromStart0) {
        return true;
    }

    std::vector<int> perm(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        perm[static_cast<std::size_t>(i)] = i + 1;
    }
    std::vector<int> movedPerm(perm.begin() + fromStart0, perm.begin() + fromEnd0 + 1);
    perm.erase(perm.begin() + fromStart0, perm.begin() + fromEnd0 + 1);
    perm.insert(perm.begin() + destStart0, movedPerm.begin(), movedPerm.end());

    std::vector<int> newPosByOrig1(static_cast<std::size_t>(count + 1), 0);
    for (int newIdx0 = 0; newIdx0 < count; ++newIdx0) {
        newPosByOrig1[static_cast<std::size_t>(perm[static_cast<std::size_t>(newIdx0)])] =
            newIdx0 + 1;
    }

    MdDisc candidate = disc;
    std::vector<MdTrack> movedTracks;
    movedTracks.reserve(static_cast<std::size_t>(blockLen));
    for (int i = fromStart0; i <= fromEnd0; ++i) {
        movedTracks.push_back(std::move(candidate.tracks[static_cast<std::size_t>(i)]));
    }
    candidate.tracks.erase(candidate.tracks.begin() + fromStart0,
                           candidate.tracks.begin() + fromEnd0 + 1);
    candidate.tracks.insert(candidate.tracks.begin() + destStart0,
                            std::make_move_iterator(movedTracks.begin()),
                            std::make_move_iterator(movedTracks.end()));

    std::vector<MdGroup> nextGroups;
    for (const auto& g : candidate.groups) {
        const int origLen = g.endTrack - g.startTrack + 1;
        int movedCount = 0;
        int otherCount = 0;
        int minMoved = count + 1;
        int maxMoved = 0;
        int minOther = count + 1;
        int maxOther = 0;
        for (int orig1 = g.startTrack; orig1 <= g.endTrack; ++orig1) {
            const int np = newPosByOrig1[static_cast<std::size_t>(orig1)];
            if (orig1 >= fromStart0 + 1 && orig1 <= fromEnd0 + 1) {
                ++movedCount;
                minMoved = std::min(minMoved, np);
                maxMoved = std::max(maxMoved, np);
            } else {
                ++otherCount;
                minOther = std::min(minOther, np);
                maxOther = std::max(maxOther, np);
            }
        }
        if (otherCount == 0 && maxMoved - minMoved + 1 == origLen) {
            MdGroup ng = g;
            ng.startTrack = minMoved;
            ng.endTrack = maxMoved;
            nextGroups.push_back(std::move(ng));
        } else if (movedCount == 0 && maxOther >= minOther) {
            const int spanLen = maxOther - minOther + 1;
            if (spanLen == otherCount || spanLen == otherCount + blockLen) {
                MdGroup ng = g;
                ng.startTrack = minOther;
                ng.endTrack = maxOther;
                nextGroups.push_back(std::move(ng));
            }
        } else if (movedCount > 0 && otherCount > 0) {
            const int allLo = std::min(minMoved, minOther);
            const int allHi = std::max(maxMoved, maxOther);
            if (allHi - allLo + 1 == origLen) {
                MdGroup ng = g;
                ng.startTrack = allLo;
                ng.endTrack = allHi;
                nextGroups.push_back(std::move(ng));
            } else if (maxOther - minOther + 1 == otherCount) {
                MdGroup ng = g;
                ng.startTrack = minOther;
                ng.endTrack = maxOther;
                nextGroups.push_back(std::move(ng));
            }
        }
    }
    normalizeGroups(nextGroups);
    candidate.groups = std::move(nextGroups);
    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool moveMdTrack(MdDisc& disc, int fromIndex0, int toIndex0, std::string& error) {
    error.clear();
    const int count = static_cast<int>(disc.tracks.size());
    if (fromIndex0 < 0 || fromIndex0 >= count || toIndex0 < 0 || toIndex0 >= count) {
        error = "Track move index out of range.";
        return false;
    }
    if (fromIndex0 == toIndex0) {
        return true;
    }

    // Track original 1-based positions across reorder to keep group ranges valid and disjoint.
    std::vector<int> perm(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        perm[static_cast<std::size_t>(i)] = i + 1;
    }
    const int movedOrig1 = perm[static_cast<std::size_t>(fromIndex0)];
    perm.erase(perm.begin() + fromIndex0);
    perm.insert(perm.begin() + toIndex0, movedOrig1);

    std::vector<int> newPosByOrig1(static_cast<std::size_t>(count + 1), 0);
    for (int newIdx0 = 0; newIdx0 < count; ++newIdx0) {
        newPosByOrig1[static_cast<std::size_t>(perm[static_cast<std::size_t>(newIdx0)])] =
            newIdx0 + 1;
    }

    MdDisc candidate = disc;
    auto movedTrack = std::move(candidate.tracks[static_cast<std::size_t>(fromIndex0)]);
    candidate.tracks.erase(candidate.tracks.begin() + fromIndex0);
    candidate.tracks.insert(candidate.tracks.begin() + toIndex0, std::move(movedTrack));

    std::vector<MdGroup> nextGroups;
    const int dest1 = toIndex0 + 1;
    for (const auto& g : candidate.groups) {
        const bool movedBelonged = (movedOrig1 >= g.startTrack && movedOrig1 <= g.endTrack);
        int minOther = count + 1;
        int maxOther = 0;
        for (int orig1 = g.startTrack; orig1 <= g.endTrack; ++orig1) {
            if (orig1 == movedOrig1) {
                continue;
            }
            const int np = newPosByOrig1[static_cast<std::size_t>(orig1)];
            minOther = std::min(minOther, np);
            maxOther = std::max(maxOther, np);
        }
        if (movedBelonged &&
            (g.startTrack == g.endTrack || (dest1 >= minOther - 1 && dest1 <= maxOther + 1))) {
            // Single-track group follows its track; multi-track group retains moved track if still
            // contiguous.
            const int lo = g.startTrack == g.endTrack ? dest1 : std::min(minOther, dest1);
            const int hi = g.startTrack == g.endTrack ? dest1 : std::max(maxOther, dest1);
            MdGroup ng = g;
            ng.startTrack = lo;
            ng.endTrack = hi;
            nextGroups.push_back(std::move(ng));
            continue;
        }
        if (maxOther >= minOther) {
            // Retain the remaining contiguous span (and absorb an external track moved strictly
            // inside it).
            const int otherCount = (g.endTrack - g.startTrack + 1) - (movedBelonged ? 1 : 0);
            const int spanLen = maxOther - minOther + 1;
            if (spanLen == otherCount || (!movedBelonged && spanLen == otherCount + 1)) {
                MdGroup ng = g;
                ng.startTrack = minOther;
                ng.endTrack = maxOther;
                nextGroups.push_back(std::move(ng));
            }
        }
    }
    normalizeGroups(nextGroups);
    candidate.groups = std::move(nextGroups);
    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool removeMdTrack(MdDisc& disc, int trackIndex0, std::string& error) {
    error.clear();
    const int count = static_cast<int>(disc.tracks.size());
    if (trackIndex0 < 0 || trackIndex0 >= count) {
        error = "Track index out of range.";
        return false;
    }
    MdDisc candidate = disc;
    candidate.tracks.erase(candidate.tracks.begin() + trackIndex0);
    shiftGroupsForRemove(candidate.groups, trackIndex0 + 1);
    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool setMdGroup(MdDisc& disc, int startTrack1, int endTrack1, const std::string& halfWidthTitle,
                const std::string& fullWidthTitle, std::string& error) {
    error.clear();
    const int count = static_cast<int>(disc.tracks.size());
    if (startTrack1 < 1 || endTrack1 < startTrack1 || endTrack1 > count) {
        error = "Group range must lie within 1.." + std::to_string(count) + ".";
        return false;
    }
    if (halfWidthTitle.empty() && fullWidthTitle.empty()) {
        error = "Group must have a half-width or full-width title.";
        return false;
    }

    MdDisc candidate = disc;
    trimGroupsExcludingRange(candidate.groups, startTrack1, endTrack1);
    candidate.groups.push_back({startTrack1, endTrack1, halfWidthTitle, fullWidthTitle});
    normalizeGroups(candidate.groups);
    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool ungroupMdRange(MdDisc& disc, int startTrack1, int endTrack1, std::string& error) {
    error.clear();
    if (startTrack1 < 1 || endTrack1 < startTrack1) {
        error = "Invalid track range for ungrouping.";
        return false;
    }
    MdDisc candidate = disc;
    trimGroupsExcludingRange(candidate.groups, startTrack1, endTrack1);
    normalizeGroups(candidate.groups);
    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

// ============================================================================
// Session Synchronization
// ============================================================================

bool syncTracksFromClips(const Session& session, MdDisc& disc, std::string& error,
                         const std::vector<std::pair<int, int>>& explicitClips) {
    error.clear();
    MdDisc candidate = disc;
    const bool loadedActive = candidate.loadedDisc.active;
    if (!loadedActive) {
        candidate.tracks.clear();
        candidate.groups.clear();
    }
    candidate.autoTrackOnImport = true;

    struct ClipEntry {
        std::int64_t start = 0;
        std::int64_t end = 0;
        int layerIndex = 0;
        std::string title;
    };
    auto collectClip = [&](const Session& s, int layerIdx, int clipIdx,
                           std::vector<ClipEntry>& out) {
        if (layerIdx < 0 || layerIdx >= static_cast<int>(s.tracks.size())) {
            return;
        }
        const auto& clips = s.tracks[static_cast<std::size_t>(layerIdx)].clips;
        if (clipIdx < 0 || clipIdx >= static_cast<int>(clips.size())) {
            return;
        }
        const auto& clip = clips[static_cast<std::size_t>(clipIdx)];
        if (loadedActive && clip.lockedOnDisc) {
            return;
        }
        juce::int64 st = 0;
        juce::int64 en = 0;
        if (!clipSampleRange(clip, st, en) || en <= st) {
            return;
        }
        auto label = stripAudioExtensionIfPresent(
            clipDisplayName(clip).replaceCharacters("\r\n", "  ").trim().toStdString());
        out.push_back({static_cast<std::int64_t>(st), static_cast<std::int64_t>(en), layerIdx,
                       std::move(label)});
    };

    std::vector<ClipEntry> items;
    if (!explicitClips.empty()) {
        for (const auto& [layerIdx, clipIdx] : explicitClips) {
            collectClip(session, layerIdx, clipIdx, items);
        }
    } else {
        bool anyUnmutedWithClips = false;
        for (const auto& tr : session.tracks) {
            if (!tr.muted && !tr.clips.empty()) {
                anyUnmutedWithClips = true;
                break;
            }
        }
        for (int li = 0; li < static_cast<int>(session.tracks.size()); ++li) {
            const auto& layer = session.tracks[static_cast<std::size_t>(li)];
            if (anyUnmutedWithClips && layer.muted) {
                continue;
            }
            for (int ci = 0; ci < static_cast<int>(layer.clips.size()); ++ci) {
                collectClip(session, li, ci, items);
            }
        }
    }

    if (!items.empty()) {
        std::sort(items.begin(), items.end(), [](const ClipEntry& a, const ClipEntry& b) {
            if (a.start != b.start) {
                return a.start < b.start;
            }
            if (a.end != b.end) {
                return a.end > b.end;
            }
            return a.layerIndex < b.layerIndex;
        });

        struct SpanEntry {
            std::int64_t start = 0;
            std::int64_t end = 0;
            std::string title;
        };
        std::vector<SpanEntry> chain;
        chain.push_back({items.front().start, items.front().end, items.front().title});
        for (std::size_t i = 1; i < items.size(); ++i) {
            const auto& c = items[i];
            auto& prev = chain.back();
            if (c.end <= prev.end) {
                continue;
            }
            if (c.start < prev.end) {
                if (c.start > prev.start) {
                    prev.end = c.start;
                    chain.push_back({c.start, c.end, c.title});
                } else {
                    prev.end = c.end;
                }
            } else {
                chain.push_back({c.start, c.end, c.title});
            }
        }

        if (loadedActive) {
            const auto covered = collectExistingPartBounds(candidate);
            std::vector<std::tuple<std::int64_t, std::int64_t, std::string>> tuples;
            tuples.reserve(chain.size());
            for (const auto& sp : chain) {
                tuples.emplace_back(sp.start, sp.end, sp.title);
            }
            if (!appendUncoveredLoadedSyncTracks(covered, tuples, candidate, error)) {
                return false;
            }
        } else {
            if (static_cast<int>(chain.size()) > kMdMaxTracks) {
                error = "Selected clips produce more than 255 tracks; MiniDisc supports at most "
                        "255 tracks.";
                return false;
            }
            for (std::size_t i = 0; i < chain.size(); ++i) {
                MdTrack tr;
                tr.id = makeMdTrackId();
                tr.halfWidthTitle =
                    chain[i].title.empty() ? ("Track " + std::to_string(i + 1)) : chain[i].title;
                tr.parts.push_back({chain[i].start, chain[i].end});
                candidate.tracks.push_back(std::move(tr));
            }
        }
    }

    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool syncTracksFromLayerClips(const Session& session, MdDisc& disc, std::string& error) {
    error.clear();
    MdDisc candidate = disc;
    const bool loadedActive = candidate.loadedDisc.active;
    if (!loadedActive) {
        candidate.tracks.clear();
        candidate.groups.clear();
    }
    candidate.autoTrackOnImport = true;

    if (!session.tracks.empty()) {
        struct ClipEntry {
            std::int64_t start = 0;
            std::int64_t end = 0;
            std::string title;
        };
        std::vector<ClipEntry> items;
        for (const auto& clip : session.tracks[0].clips) {
            if (loadedActive && clip.lockedOnDisc) {
                continue;
            }
            juce::int64 s = 0;
            juce::int64 e = 0;
            if (!clipSampleRange(clip, s, e) || e <= s) {
                continue;
            }
            auto label = stripAudioExtensionIfPresent(clipDisplayName(clip).trim().toStdString());
            items.push_back(
                {static_cast<std::int64_t>(s), static_cast<std::int64_t>(e), std::move(label)});
        }
        std::sort(items.begin(), items.end(), [](const ClipEntry& a, const ClipEntry& b) {
            return std::tie(a.start, a.end) < std::tie(b.start, b.end);
        });
        if (loadedActive) {
            const auto covered = collectExistingPartBounds(candidate);
            std::vector<std::tuple<std::int64_t, std::int64_t, std::string>> tuples;
            tuples.reserve(items.size());
            for (const auto& item : items) {
                tuples.emplace_back(item.start, item.end, item.title);
            }
            if (!appendUncoveredLoadedSyncTracks(covered, tuples, candidate, error)) {
                return false;
            }
        } else {
            if (static_cast<int>(items.size()) > kMdMaxTracks) {
                error = "Layer 1 has more than 255 clips; MiniDisc supports at most 255 tracks.";
                return false;
            }
            for (std::size_t idx = 0; idx < items.size(); ++idx) {
                MdTrack tr;
                tr.id = makeMdTrackId();
                tr.halfWidthTitle = items[idx].title.empty()
                                        ? ("Track " + std::to_string(idx + 1))
                                        : items[idx].title;
                tr.parts.push_back({items[idx].start, items[idx].end});
                candidate.tracks.push_back(std::move(tr));
            }
        }
    }

    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool syncTracksFromMarkers(const Session& session, MdDisc& disc, std::string& error) {
    error.clear();
    if (disc.loadedDisc.active) {
        error = "Marker sync cannot overwrite a loaded MiniDisc layout; split on-disc tracks or "
                "manage new local tracks individually.";
        return false;
    }
    const std::int64_t masterEnd = projectEndSample(session);
    if (masterEnd <= 0) {
        MdDisc candidate = disc;
        candidate.tracks.clear();
        candidate.groups.clear();
        disc = std::move(candidate);
        return true;
    }

    struct Boundary {
        std::int64_t sample = 0;
        std::string title;
    };
    std::vector<Boundary> sorted;
    for (const auto& m : session.markers) {
        if (!std::isfinite(m.position)) {
            continue;
        }
        const std::int64_t pos =
            static_cast<std::int64_t>(std::llround(m.position * kMdSampleRate));
        if (pos < 0 || pos >= masterEnd) {
            continue;
        }
        sorted.push_back({pos, m.name.trim().toStdString()});
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const Boundary& a, const Boundary& b) { return a.sample < b.sample; });
    // Deduplicate identical sample positions keeping first marker title.
    std::vector<Boundary> unique;
    for (const auto& b : sorted) {
        if (unique.empty() || unique.back().sample != b.sample) {
            unique.push_back(b);
        }
    }

    MdDisc candidate = disc;
    candidate.tracks.clear();
    candidate.groups.clear();

    if (unique.empty()) {
        MdTrack tr;
        tr.id = makeMdTrackId();
        tr.halfWidthTitle = "Track 1";
        tr.parts.push_back({0, masterEnd});
        candidate.tracks.push_back(std::move(tr));
    } else {
        if (unique.front().sample > 0) {
            MdTrack intro;
            intro.id = makeMdTrackId();
            intro.halfWidthTitle = "Track 1";
            intro.parts.push_back({0, unique.front().sample});
            candidate.tracks.push_back(std::move(intro));
        }
        for (std::size_t i = 0; i < unique.size(); ++i) {
            const std::int64_t start = unique[i].sample;
            const std::int64_t end = (i + 1 < unique.size()) ? unique[i + 1].sample : masterEnd;
            if (end <= start) {
                continue;
            }
            MdTrack tr;
            tr.id = makeMdTrackId();
            tr.halfWidthTitle = unique[i].title.empty()
                                    ? ("Track " + std::to_string(candidate.tracks.size() + 1))
                                    : unique[i].title;
            tr.parts.push_back({start, end});
            candidate.tracks.push_back(std::move(tr));
        }
    }

    if (static_cast<int>(candidate.tracks.size()) > kMdMaxTracks) {
        error =
            "Timeline markers produce more than 255 tracks; MiniDisc supports at most 255 tracks.";
        return false;
    }
    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

} // namespace carta
