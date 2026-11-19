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
    return false;
}

int mdTotalClusters(MdMedium medium) noexcept {
    switch (medium) {
    case MdMedium::md60:
        return 1762;
    case MdMedium::md74:
        return 2173;
    case MdMedium::md80:
        return 2350;
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
        return 1;
    }
    return 2;
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
    if (static_cast<int>(disc.tracks.size()) > kMdMaxTracks) {
        error = "MiniDisc UTOC supports at most 255 tracks.";
        return false;
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
    out.totalClusters = mdTotalClusters(disc.medium);
    if (!validateMdDisc(disc, error)) {
        return false;
    }
    if (disc.tracks.empty()) {
        return true;
    }

    std::vector<RawPartRef> refs;
    std::size_t totalPlacements = 0;
    for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
        const auto& tr = disc.tracks[t];
        const auto codec = effectiveTrackCodec(disc, tr);
        const std::int64_t q = mdFrameQuantumSamples(codec);
        const int upq = mdUnitsPerQuantum(codec);
        for (std::size_t p = 0; p < tr.parts.size(); ++p) {
            const auto& part = tr.parts[p];
            RawPartRef r;
            r.trackIndex = static_cast<int>(t);
            r.partIndex = static_cast<int>(p);
            r.codec = codec;
            r.rawStart = part.startSample;
            r.rawEnd = part.endSample;
            r.qStart = (part.startSample / q) * q;
            r.qEnd = ((part.endSample + q - 1) / q) * q;
            refs.push_back(r);

            out.maxEndSample = std::max(out.maxEndSample, part.endSample);
            out.logicalPlaySamples += part.durationSamples();
            const int partUnits = static_cast<int>(((r.qEnd - r.qStart) / q) * upq);
            out.uncoalescedClusters += (partUnits + kMdUnitsPerCluster - 1) / kMdUnitsPerCluster;
            ++totalPlacements;
        }
    }

    // Group refs by codec and sort by outward-aligned interval [qStart, qEnd).
    std::map<MdCodecMode, std::vector<RawPartRef>> byCodec;
    for (const auto& r : refs) {
        byCodec[r.codec].push_back(r);
    }

    out.placements.resize(totalPlacements);
    std::map<std::pair<int, int>, std::size_t> placementIndexByTrackPart;
    std::size_t flatIdx = 0;
    for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
        for (std::size_t p = 0; p < disc.tracks[t].parts.size(); ++p) {
            placementIndexByTrackPart.emplace(
                std::make_pair(static_cast<int>(t), static_cast<int>(p)), flatIdx++);
        }
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

    // Order physical spans in track presentation order so sequential discs read sequentially on
    // cluster map.
    std::sort(pendingSpans.begin(), pendingSpans.end(),
              [](const PendingSpan& a, const PendingSpan& b) {
                  return std::tie(a.earliestTrackIndex, a.earliestPartIndex, a.startSample) <
                         std::tie(b.earliestTrackIndex, b.earliestPartIndex, b.startSample);
              });

    std::uint16_t nextCluster = kMdFirstAudioCluster;
    for (std::size_t sIdx = 0; sIdx < pendingSpans.size(); ++sIdx) {
        const auto& ps = pendingSpans[sIdx];
        const std::int64_t q = mdFrameQuantumSamples(ps.codec);
        const int upq = mdUnitsPerQuantum(ps.codec);
        const int totalQuanta = static_cast<int>((ps.endSample - ps.startSample) / q);
        const int spanUnits = totalQuanta * upq;
        const int spanClusters = (spanUnits + kMdUnitsPerCluster - 1) / kMdUnitsPerCluster;

        // Collect internal abutting boundary sample positions (where one part's exact rawEnd
        // equals another part's exact rawStart within the same coalesced span) so split points
        // land cleanly on one Sound Group boundary with zero gap and zero duplicated 11.6 ms frame.
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

            const auto slot = placementIndexByTrackPart.at({m.trackIndex, m.partIndex});
            out.placements[slot] = place;
        }

        nextCluster = static_cast<std::uint16_t>(nextCluster + spanClusters);
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
        if (startSample > lastPart.startSample && startSample < lastPart.endSample &&
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
    first.parts.resize(static_cast<std::size_t>(hitPart));
    first.parts.push_back({splitPart.startSample, timelineSample});

    second.parts.erase(second.parts.begin(), second.parts.begin() + hitPart + 1);
    second.parts.insert(second.parts.begin(), {timelineSample, splitPart.endSample});

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
    const std::int64_t lo = leftPart.startSample + 1;
    const std::int64_t hi = std::min(maxSample, rightPart.endSample - 1);
    if (hi < lo || newSample < lo || newSample > hi) {
        error = "Split boundary position must remain strictly inside the adjacent tracks.";
        return false;
    }
    leftPart.endSample = newSample;
    rightPart.startSample = newSample;
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
    if (rightEdge) {
        const std::int64_t clamped =
            std::clamp(newSample, part.startSample + 1, std::max(part.startSample + 1, maxSample));
        if (clamped <= part.startSample || clamped > maxSample) {
            error = "Part end sample is out of bounds.";
            return false;
        }
        part.endSample = clamped;
    } else {
        const std::int64_t clamped =
            std::clamp<std::int64_t>(newSample, 0, std::max<std::int64_t>(0, part.endSample - 1));
        if (clamped < 0 || clamped >= part.endSample) {
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
    if (len <= 0 || maxSample < len) {
        error = "Part length exceeds timeline range.";
        return false;
    }
    const std::int64_t clamped = std::clamp<std::int64_t>(newStartSample, 0, maxSample - len);
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
    candidate.tracks.clear();
    candidate.groups.clear();
    candidate.autoTrackOnImport = true;

    struct ClipEntry {
        std::int64_t start = 0;
        std::int64_t end = 0;
        int layerIndex = 0;
        std::string title;
    };
    auto collectClip = [](const Session& s, int layerIdx, int clipIdx,
                          std::vector<ClipEntry>& out) {
        if (layerIdx < 0 || layerIdx >= static_cast<int>(s.tracks.size())) {
            return;
        }
        const auto& clips = s.tracks[static_cast<std::size_t>(layerIdx)].clips;
        if (clipIdx < 0 || clipIdx >= static_cast<int>(clips.size())) {
            return;
        }
        juce::int64 st = 0;
        juce::int64 en = 0;
        if (!clipSampleRange(clips[static_cast<std::size_t>(clipIdx)], st, en) || en <= st) {
            return;
        }
        auto label =
            stripAudioExtensionIfPresent(clipDisplayName(clips[static_cast<std::size_t>(clipIdx)])
                                             .replaceCharacters("\r\n", "  ")
                                             .trim()
                                             .toStdString());
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

        if (static_cast<int>(chain.size()) > kMdMaxTracks) {
            error = "Selected clips produce more than 255 tracks; MiniDisc supports at most 255 "
                    "tracks.";
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

    if (!validateMdDisc(candidate, error)) {
        return false;
    }
    disc = std::move(candidate);
    return true;
}

bool syncTracksFromLayerClips(const Session& session, MdDisc& disc, std::string& error) {
    error.clear();
    MdDisc candidate = disc;
    candidate.tracks.clear();
    candidate.groups.clear();
    candidate.autoTrackOnImport = true;

    if (!session.tracks.empty()) {
        struct ClipEntry {
            std::int64_t start = 0;
            std::int64_t end = 0;
            std::string title;
        };
        std::vector<ClipEntry> items;
        for (const auto& clip : session.tracks[0].clips) {
            juce::int64 s = 0;
            juce::int64 e = 0;
            if (!clipSampleRange(clip, s, e) || e <= s) {
                continue;
            }
            auto label = stripAudioExtensionIfPresent(clipDisplayName(clip).trim().toStdString());
            if (label.empty()) {
                label = "Track " + std::to_string(items.size() + 1);
            }
            items.push_back(
                {static_cast<std::int64_t>(s), static_cast<std::int64_t>(e), std::move(label)});
        }
        std::sort(items.begin(), items.end(), [](const ClipEntry& a, const ClipEntry& b) {
            return std::tie(a.start, a.end) < std::tie(b.start, b.end);
        });
        if (static_cast<int>(items.size()) > kMdMaxTracks) {
            error = "Layer 1 has more than 255 clips; MiniDisc supports at most 255 tracks.";
            return false;
        }
        for (const auto& item : items) {
            MdTrack tr;
            tr.id = makeMdTrackId();
            tr.halfWidthTitle = item.title;
            tr.parts.push_back({item.start, item.end});
            candidate.tracks.push_back(std::move(tr));
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
