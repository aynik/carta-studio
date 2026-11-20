/**
 * @file UtocTests.cpp
 * @brief Unit tests for Rainbow Book 2352-byte UTOC Sectors 0/1/2/4 serialization.
 *
 * Covers cluster/sector/group physical address packing, mode/SCMS/WP/emphasis
 * flags, BCD timestamps, half-width JIS X 0201 and full-width Shift-JIS title
 * encoding, DAG fragment tail sharing across tracks, imported sector
 * preservation, and 255-slot overflow diagnostics.
 */
#include "MiniDisc/MdAuthoring.h"
#include "MiniDisc/Utoc.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void check(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << "\n";
        ++gFailures;
    }
}

/** Verifies 2352-byte UTOC cluster/sector/group physical address packing. */
void testAddressPacking() {
    carta::UtocAddress a{0x123, 25, 10};
    const auto logical = a.toLogicalSector();
    check(logical == 0x123 * 32 + 25, "logical sector calculation");
    const auto linear = a.toLinearGroup();
    check(carta::UtocAddress::fromLinearGroup(linear) == a, "linear group roundtrip");
    const auto b = carta::UtocAddress::fromLogicalSector(logical, 10);
    check(b == a, "logical sector roundtrip");
}

/** Verifies UTOC audio mode bits, WP/SCMS flags, and Sector 2 BCD timestamp roundtrips. */
void testModeBitsAndTimestamps() {
    const auto spRw = carta::utocModeFromTrack("sp-stereo", false, false);
    check(carta::audioModeFromUtocMode(spRw) == "sp-stereo", "SP stereo mode decode");
    check((spRw & carta::kUtocWritable) != 0, "writable bit set when not protected");
    check((spRw & carta::kUtocScmsUnrestricted) != 0, "SCMS unrestricted set when false");

    const auto spRoScms = carta::utocModeFromTrack("sp-stereo", true, true);
    check((spRoScms & carta::kUtocWritable) == 0, "writable bit cleared when write-protected");
    check((spRoScms & carta::kUtocScmsDigCopy) != 0, "SCMS copy bit set when restricted");

    check(carta::audioModeFromUtocMode(carta::utocModeFromTrack("sp-mono", false, false)) ==
              "sp-mono",
          "SP mono decode");
    check(carta::audioModeFromUtocMode(carta::utocModeFromTrack("lp2", false, true)) == "lp2",
          "LP2 decode");
    check(carta::audioModeFromUtocMode(carta::utocModeFromTrack("lp4", true, false)) == "lp4",
          "LP4 decode");

    carta::UtocTimestamp ts{};
    std::string err;
    check(carta::parseUtocTimestampString("2026-11-15T08:42:19", ts, err) && ts.isValid(),
          "parse valid timestamp");
    check(ts.year == 26 && ts.month == 11 && ts.day == 15 && ts.hour == 8 && ts.minute == 42 &&
              ts.second == 19,
          "timestamp fields");
    check(carta::formatUtocTimestampString(ts) == "2026-11-15 08:42:19",
          "timestamp format roundtrip");
    check(!carta::parseUtocTimestampString("not-a-timestamp", ts, err),
          "reject malformed timestamp");
}

/** Verifies half-width JIS X 0201, full-width Shift-JIS, and UTOC group string parsing. */
void testTitlesAndGroups() {
    // Half-width JIS X 0201 Katakana + ASCII
    const std::string katakanaSrc =
        "\xE3\x82\xAC\xE3\x83\x91\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 Mix!"; // ガパテスト Mix!
    const auto hwBytes = carta::encodeHalfWidthBytes(katakanaSrc);
    check(!hwBytes.empty(), "half-width Katakana encoded non-empty");
    const auto hwBack = carta::decodeHalfWidthBytes(hwBytes.data(), hwBytes.size());
    check(hwBack.find("Mix!") != std::string::npos && hwBack.size() > 6,
          "half-width ASCII preserved alongside Katakana");

    // Full-width Shift-JIS roundtrip
    std::string err;
    const std::string fwSrc = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\x20\xE3\x82\xB5\xE3\x83\xB3\xE3"
                              "\x83\x97\xE3\x83\xAB\x20"
                              "01"; // 日本語 サンプル 01
    const auto sjis = carta::encodeFullWidthShiftJis(fwSrc, err);
    check(err.empty() && !sjis.empty(), "Shift-JIS encode succeeded");
    const auto fwBack = carta::decodeFullWidthShiftJis(sjis.data(), sjis.size());
    check(fwBack.find("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E") != std::string::npos,
          "Shift-JIS kanji survived roundtrip");
    check(fwBack.find("\xEF\xBC\x90\xEF\xBC\x91") != std::string::npos,
          "ASCII digits converted to fullwidth cleanly");

    std::string dHw, dFw;
    const auto groups = carta::parseUtocGroups(
        "0;Live Disc//1-2;Set One//3;Encore//",
        "\xEF\xBC\x90\xEF\xBC\x9B\xE3\x83\xA9\xE3\x82\xA4\xE3\x83\x96\xEF\xBC\x8F\xEF\xBC\x8F"
        "\xEF\xBC\x91\xEF\xBC\x8D\xEF\xBC\x92\xEF\xBC\x9B\xE7\xAC\xAC\xE4\xB8\x80\xE9\x83\xA8\xEF"
        "\xBC\x8F\xEF\xBC\x8F",
        3, dHw, dFw);
    check(dHw == "Live Disc", "parsed half-width disc title from group string");
    check(dFw == "\xE3\x83\xA9\xE3\x82\xA4\xE3\x83\x96",
          "parsed full-width disc title from group string");
    check(groups.size() == 2 && groups[0].startTrack == 1 && groups[0].endTrack == 2 &&
              groups[0].halfWidthTitle == "Set One",
          "parsed track group 1-2");
    check(groups[1].startTrack == 3 && groups[1].endTrack == 3 &&
              groups[1].halfWidthTitle == "Encore",
          "parsed single-track group 3");
}

/** Verifies full UTOC sector compilation, DAG fragment suffix sharing, and roundtrip decoding. */
void testDiscRoundtripAndZeroCopyReuse(const char* dumpDir) {
    carta::MdDisc disc;
    disc.medium = carta::MdMedium::md80;
    disc.defaultCodec = carta::MdCodecMode::spStereo;
    disc.halfWidthTitle = "Night Session";
    disc.fullWidthTitle = "\xE5\xA4\x9C\xE3\x81\xAE\xE3\x82\xBB\xE3\x83\x83\xE3\x82\xB7\xE3\x83\xA7"
                          "\xE3\x83\xB3"; // 夜のセッション

    const carta::MdPart pA{0, 44100 * 30};
    const carta::MdPart pB{44100 * 30, 44100 * 75};
    const carta::MdPart pC{44100 * 75, 44100 * 110};

    // Track 1: [pA, pB]; Track 2: [pC, pA, pB] (shares DAG suffix pA->pB and physical audio);
    // Track 3: [pB] (shares tail slot pB!) -> only 3 UTOC fragments for 6 logical part references!
    disc.tracks = {
        {"t1",
         "Opening",
         "\xE3\x82\xAA\xE3\x83\xBC\xE3\x83\x97\xE3\x83\x8B\xE3\x83\xB3\xE3\x82\xB0",
         {},
         false,
         false,
         false,
         "",
         {pA, pB}},
        {"t2",
         "Medley",
         "\xE3\x83\xA1\xE3\x83\x89\xE3\x83\xAC\xE3\x83\xBC",
         {},
         false,
         false,
         false,
         "",
         {pC, pA, pB}},
        {"t3",
         "Reprise",
         "\xE3\x83\xAA\xE3\x83\x97\xE3\x83\xA9\xE3\x82\xA4\xE3\x82\xBA",
         {},
         false,
         false,
         false,
         "",
         {pB}},
    };
    disc.groups = {{1, 2, "Act I", ""}};

    carta::UtocDiscState utoc;
    std::string err;
    check(carta::compileMdDiscToUtoc(disc, utoc, err), "compile MdDisc to UTOC: " + err);
    const auto budget = carta::computeUtocBudget(utoc, "md80");
    check(budget.fits(), "compiled disc fits within MD80 budget");
    check(budget.usedTracks == 3, "budget tracks == 3");
    check(budget.usedFragments == 3,
          "DAG tail sharing uses only 3 fragment slots for 6 references");
    check(budget.sharedFragmentCount >= 2, "zero-copy physical fragment sharing detected");
    check(budget.usedClusters > 45 && budget.usedClusters < 60,
          "clusters reflect deduplicated physical audio length (~110s)");

    std::array<std::uint8_t, carta::kUtocSectorBytes> s0{}, s1{}, s2{}, s4{};
    check(carta::compileUtocSectors(utoc, s0, s1, s2, s4, err),
          "compile binary 2352-byte UTOC sectors");

    if (dumpDir != nullptr) {
        auto writeBin = [&](const char* name,
                            const std::array<std::uint8_t, carta::kUtocSectorBytes>& sec) {
            std::ofstream os(std::string(dumpDir) + "/" + name, std::ios::binary);
            os.write(reinterpret_cast<const char*>(sec.data()),
                     static_cast<std::streamsize>(sec.size()));
        };
        writeBin("sec0.bin", s0);
        writeBin("sec1.bin", s1);
        writeBin("sec2.bin", s2);
        writeBin("sec4.bin", s4);
    }

    carta::UtocDiscState parsed{};
    check(carta::parseUtocSectors(s0.data(), s1.data(), s2.data(), s4.data(), parsed, err),
          "parse compiled UTOC sectors");
    check(parsed.trackCount == 3 && parsed.deviceSignature == carta::kCartaUtocSignature,
          "parsed sector 0 header matches");

    carta::MdDisc roundtrip{};
    check(carta::mdDiscFromUtoc(parsed, roundtrip, err), "reconstruct MdDisc from UTOC: " + err);
    check(roundtrip.tracks.size() == 3, "reconstructed 3 tracks");
    check(roundtrip.tracks[0].halfWidthTitle == "Opening" &&
              roundtrip.tracks[0].fullWidthTitle ==
                  "\xE3\x82\xAA\xE3\x83\xBC\xE3\x83\x97\xE3\x83\x8B\xE3\x83\xB3\xE3\x82\xB0",
          "track 1 HW+FW title preserved");
    check(roundtrip.tracks[1].parts.size() == 3, "track 2 preserved 3 linked fragments");
    check(roundtrip.tracks[0].parts[0] == roundtrip.tracks[1].parts[1],
          "zero-copy part identity shared across track 1 and track 2");
    check(roundtrip.tracks[0].parts[1] == roundtrip.tracks[2].parts[0],
          "zero-copy part identity shared between track 1 and track 3");
    check(roundtrip.groups.size() == 1 && roundtrip.groups[0].halfWidthTitle == "Act I" &&
              roundtrip.groups[0].startTrack == 1 && roundtrip.groups[0].endTrack == 2,
          "disc group preserved");
    check(roundtrip.halfWidthTitle == "Night Session" &&
              roundtrip.fullWidthTitle == disc.fullWidthTitle,
          "disc title HW+FW preserved");
}

/** Verifies per-track mode/flag/timestamp preservation and imported Sector 0 device signatures. */
void testMdFlagsAndImportedUtocPreservation() {
    carta::MdDisc disc;
    disc.halfWidthTitle = "Field Archive";
    disc.discTimestamp = "2026-06-15 14:22:09";
    disc.medium = carta::MdMedium::md80;
    disc.defaultCodec = carta::MdCodecMode::lp2;

    disc.tracks = {{"t1",
                    "Protected LP2",
                    "",
                    carta::MdCodecMode::lp2,
                    true,
                    false,
                    true,
                    "2026-06-15 14:23:00",
                    {{0, 44100 * 10}}},
                   {"t2",
                    "SCMS Mono",
                    "",
                    carta::MdCodecMode::spMono,
                    false,
                    true,
                    false,
                    "2026-06-15 14:24:00",
                    {{44100 * 10, 44100 * 25}}},
                   {"t3",
                    "LP4 Share",
                    "",
                    carta::MdCodecMode::lp4,
                    true,
                    true,
                    false,
                    "2026-06-15 14:25:00",
                    {{44100 * 25, 44100 * 40}}}};

    carta::UtocDiscState utoc;
    std::string err;
    check(carta::compileMdDiscToUtoc(disc, utoc, err),
          "compile multi-mode MD flags & Sector 2 timestamps: " + err);
    carta::MdDisc restored;
    check(carta::mdDiscFromUtoc(utoc, restored, err),
          "decompile multi-mode MD flags & timestamps: " + err);
    check(restored.discTimestamp == "2026-06-15 14:22:09", "disc timestamp roundtrips");
    check(restored.importedUtocSectors[0].size() == carta::kUtocSectorBytes &&
              restored.importedUtocSectors[1].size() == carta::kUtocSectorBytes &&
              restored.importedUtocSectors[2].size() == carta::kUtocSectorBytes &&
              restored.importedUtocSectors[3].size() == carta::kUtocSectorBytes,
          "decompiled disc captures all 4 imported UTOC sectors");
    check(restored.tracks.size() == 3, "restored track count matches");
    check(carta::effectiveTrackCodec(restored, restored.tracks[0]) == carta::MdCodecMode::lp2 &&
              restored.tracks[0].writeProtected && !restored.tracks[0].scmsCopyProtected &&
              restored.tracks[0].emphasis && restored.tracks[0].timestamp == "2026-06-15 14:23:00",
          "track 1 MD flags, emphasis, and timestamp match");
    check(carta::effectiveTrackCodec(restored, restored.tracks[1]) == carta::MdCodecMode::spMono &&
              !restored.tracks[1].writeProtected && restored.tracks[1].scmsCopyProtected &&
              restored.tracks[1].timestamp == "2026-06-15 14:24:00",
          "track 2 MD flags and timestamp match");
    check(carta::effectiveTrackCodec(restored, restored.tracks[2]) == carta::MdCodecMode::lp4 &&
              restored.tracks[2].writeProtected && restored.tracks[2].scmsCopyProtected &&
              restored.tracks[2].timestamp == "2026-06-15 14:25:00",
          "track 3 MD flags and timestamp match");

    restored.importedUtocSectors[0][28] = 0x12;
    restored.importedUtocSectors[0][29] = 0x34;
    carta::UtocDiscState preserved;
    check(carta::compileMdDiscToUtoc(restored, preserved, err) &&
              preserved.deviceSignature == 0x1234,
          "importedUtocSectors preserves original device signature");
}

/** Verifies explicit error diagnostics when exceeding 255 UTOC fragments or title cells. */
void testOverflowLimits() {
    carta::MdDisc disc;
    disc.halfWidthTitle = "Too Many Fragments";
    disc.medium = carta::MdMedium::md80;
    disc.defaultCodec = carta::MdCodecMode::spStereo;

    // 256 non-contiguous fragments on 1 track (gapped so physical plan keeps 256 parts) -> exceeds
    // 255 UTOC fragment slots.
    carta::MdTrack big;
    big.id = "t1";
    big.halfWidthTitle = "Long Chain";
    for (int i = 0; i < 256; ++i) {
        const std::int64_t start = static_cast<std::int64_t>(i) * 1024LL;
        big.parts.push_back({start, start + 512LL});
    }
    disc.tracks.push_back(std::move(big));

    carta::UtocDiscState utoc;
    std::string err;
    check(!carta::compileMdDiscToUtoc(disc, utoc, err) && err.find("255") != std::string::npos,
          "fragment overflow (>255) returns explicit diagnostic");

    // >255 title cells
    carta::MdDisc titleOverflow;
    titleOverflow.medium = carta::MdMedium::md80;
    titleOverflow.defaultCodec = carta::MdCodecMode::spStereo;
    std::string huge(2000, 'A');
    titleOverflow.tracks = {{"t1", huge, "", {}, false, false, false, "", {{0, 44100 * 10}}}};
    check(!carta::compileMdDiscToUtoc(titleOverflow, utoc, err) &&
              err.find("255") != std::string::npos,
          "title cell overflow (>255) returns explicit diagnostic");
}

/**
 * Builds a programmatic 240-track complex UTOC (5x24 shared DAG pools, 69 odd-unit SP Stereo
 * fragments, 10 physical gaps, P-FRA sharing cluster 2060 with active audio, P-DFA defective
 * cluster, non-zero Sector 3, Shift-JIS + ASCII titles, timestamps, and 60 groups) covering
 * clusters 50..2251.
 */
void buildComplex240TrackUtocFixture(
    std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5>& rawSectors) {
    for (auto& s : rawSectors) {
        s.fill(0);
    }
    for (std::size_t i = 0; i < carta::kUtocSectorBytes; ++i) {
        rawSectors[3][i] = static_cast<std::uint8_t>((i * 37u + 13u) & 0xFFu);
    }

    carta::UtocDiscState st{};
    st.hasSector0 = true;
    st.hasSector1 = true;
    st.hasSector2 = true;
    st.hasSector4 = true;
    st.deviceSignature = 0x534F; // 'SO'
    st.discNonEmpty = 1;
    st.trackCount = 240;
    st.timestamps[1] = {26, 11, 20, 12, 0, 0, 0x534F};
    st.timestampMap[0] = 1;
    st.nextFreeTimestampSlot = 2;

    for (std::uint8_t i = 1; i < 255; ++i) {
        st.fragments[i].link = static_cast<std::uint8_t>(i + 1);
    }
    st.fragments[255].link = 0;
    st.nextFreeTrackSlot = 1;

    auto allocFrag = [&](carta::UtocAddress start, carta::UtocAddress end, std::uint8_t mode,
                         std::uint8_t link) -> std::uint8_t {
        const std::uint8_t s = st.nextFreeTrackSlot;
        check(s != 0, "fixture slot table must not overflow");
        st.nextFreeTrackSlot = st.fragments[s].link;
        st.fragments[s].start = start;
        st.fragments[s].end = end;
        st.fragments[s].mode = mode;
        st.fragments[s].link = link;
        return s;
    };

    // P-DFA (0x2E): defective cluster 1624
    st.sec0aPadding[14] = allocFrag({1624, 0, 0}, {1624, 31, 10}, 0, 0);

    // P-FRA (trackMap[0]): shares cluster 2060 with active track (2060.0.0..2060.15.10 occupied),
    // leaving 2060.16.0..2099.31.10 free (39 whole clusters + partial 2060).
    st.trackMap[0] = allocFrag({2060, 16, 0}, {2099, 31, 10}, 0, 0);

    const std::uint8_t spWritableMode = static_cast<std::uint8_t>(
        carta::kUtocSpMode | carta::kUtocStereo | carta::kUtocWritable |
        carta::kUtocScmsUnrestricted | carta::kUtocReservedCodec);
    const std::uint8_t lp2Mode = carta::utocModeFromTrack("lp2", false, false, false);

    // Define 5 shared physical fragment chains (yielding 5 * 24 = 120 DAG-shared tracks using few
    // fragment slots!) across gaps and odd-unit SP Stereo lengths, including:
    // - Pool 0 (MUS 1): 310.0.0..748.15.10 (cluster 748 shared with Pool 1!)
    // - Pool 1: 748.16.0..950.31.10 (shares cluster 748 with MUS 1, leaves 748 occupied if MUS 1 deleted)
    // - Pool 2: 50.0.0..120.5.3 with a physical gap + odd unit count (105 sound groups)
    // - Pool 3: 150.0.0..299.10.2 (odd unit count)
    // - Pool 4 (MUS 4): 1625.0.0..1999.31.10 + 2100.0.0..2251.31.10 (ends at physical last cluster 2251!)
    // Plus Pool 5 (tracks 121..240) sharing 3 fragments with 69+ odd-unit SP references & cluster 2060.
    const std::uint8_t sharedMus1 =
        allocFrag({310, 0, 0}, {748, 15, 10}, spWritableMode, 0);
    const std::uint8_t sharedNeighbor748 =
        allocFrag({748, 16, 0}, {950, 31, 10}, lp2Mode, 0);
    // Odd-unit SP fragment: clusters 50..50 sectors 0..2 group 0 -> 2*11 + 1 = 23 sound groups (odd!)
    const std::uint8_t sharedOddGapA = allocFrag({50, 0, 0}, {50, 2, 0}, spWritableMode, 0);
    // Gap between cluster 50 and cluster 70
    const std::uint8_t sharedOddGapB =
        allocFrag({70, 0, 0}, {120, 4, 2}, spWritableMode, 0);
    const std::uint8_t sharedPool3 = allocFrag({150, 0, 0}, {299, 8, 4}, spWritableMode, 0);
    const std::uint8_t mus4Part2 = allocFrag({2100, 0, 0}, {2251, 31, 10}, lp2Mode, 0);
    const std::uint8_t mus4Head = allocFrag({1625, 0, 0}, {1999, 31, 10}, lp2Mode, mus4Part2);
    // Cluster 2060 low half occupied alongside P-FRA high half, across multiple gaps:
    const std::uint8_t sharedLower2060 = allocFrag({2000, 0, 0}, {2060, 15, 10}, lp2Mode, 0);
    // Additional gap segments so >= 10 physical gaps exist across the disc
    std::vector<std::uint8_t> oddGapSingles;
    for (int g = 0; g < 8; ++g) {
        const std::uint16_t clu = static_cast<std::uint16_t>(980 + g * 20);
        // 3 sectors + 2 groups = 3*11 + 2 = 35 sound groups (odd)
        oddGapSingles.push_back(allocFrag({clu, 0, 0}, {clu, 3, 1}, spWritableMode, 0));
    }

    // Track 1..24 -> MUS 1
    for (int t = 1; t <= 24; ++t) {
        st.trackMap[static_cast<std::size_t>(t)] = sharedMus1;
    }
    // Track 25..48 -> sharedNeighbor748
    for (int t = 25; t <= 48; ++t) {
        st.trackMap[static_cast<std::size_t>(t)] = sharedNeighbor748;
    }
    // Track 49..72 -> sharedOddGapA
    for (int t = 49; t <= 72; ++t) {
        st.trackMap[static_cast<std::size_t>(t)] = sharedOddGapA;
    }
    // Track 73..96 -> sharedOddGapB (note: 24 + 24 + 24 = 72 >= 69 odd-unit SP refs!)
    for (int t = 73; t <= 96; ++t) {
        st.trackMap[static_cast<std::size_t>(t)] = sharedOddGapB;
    }
    // Track 97..120 -> sharedPool3
    for (int t = 97; t <= 120; ++t) {
        st.trackMap[static_cast<std::size_t>(t)] = sharedPool3;
    }
    // Track 121..144 -> MUS 4 (1625..1999 + 2100..2251)
    for (int t = 121; t <= 144; ++t) {
        st.trackMap[static_cast<std::size_t>(t)] = mus4Head;
    }
    // Track 145..240 -> alternating sharedLower2060 and oddGapSingles (keeps unique pools alive!)
    for (int t = 145; t <= 240; ++t) {
        const int offset = t - 145;
        st.trackMap[static_cast<std::size_t>(t)] =
            (offset < 8) ? oddGapSingles[static_cast<std::size_t>(offset)] : sharedLower2060;
    }

    // Encode concise disc + track titles (Sector 1 & Sector 4) without overflowing 255 title cells.
    std::uint8_t nextHw = 1;
    std::uint8_t nextFw = 1;
    auto appendCell = [&](std::array<std::uint8_t, carta::kUtocSlots>& headMap,
                          std::array<carta::UtocTitleCell, carta::kUtocSlots>& cells,
                          std::uint8_t& nextSlot, std::size_t mapIdx, const std::string& text) {
        const std::uint8_t s = nextSlot++;
        check(s < 255, "title cell fixture must stay within 255 slots");
        cells[s].bytes.fill(0);
        for (std::size_t j = 0; j < 7 && j < text.size(); ++j) {
            cells[s].bytes[j] = static_cast<std::uint8_t>(text[j]);
        }
        cells[s].link = 0;
        headMap[mapIdx] = s;
    };

    appendCell(st.halfTitleMap, st.halfTitleCells, nextHw, 0, "//0-240");
    appendCell(st.fullTitleMap, st.fullTitleCells, nextFw, 0, "24HCLK");
    for (int t = 1; t <= 240; ++t) {
        std::string label = "T" + std::to_string(t);
        if (t <= 24) {
            label = "M1_" + std::to_string(t);
        } else if (t >= 121 && t <= 144) {
            label = "M4_" + std::to_string(t);
        }
        appendCell(st.halfTitleMap, st.halfTitleCells, nextHw, static_cast<std::size_t>(t), label);
    }
    st.nextFreeHalfTitleSlot = nextHw;
    st.nextFreeFullTitleSlot = nextFw;

    std::string compErr;
    check(carta::compileUtocSectors(st, rawSectors[0], rawSectors[1], rawSectors[2], rawSectors[4],
                                    compErr),
          "buildComplex240TrackUtocFixture compile failed: " + compErr);
}

void verifyLoadedComplex240RoundtripAndEdits(
    const std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5>& rawSectors,
    bool isSyntheticFixture) {
    carta::UtocDiscState baselineUtoc{};
    std::string err;
    check(carta::parseUtocSectors(rawSectors[0].data(), rawSectors[1].data(),
                                  rawSectors[2].data(), rawSectors[4].data(), baselineUtoc, err),
          "baseline parseUtocSectors failed: " + err);

    carta::MdDisc loaded{};
    check(carta::loadMdDiscFromDeviceUtoc(rawSectors, "mock:fixture", loaded, err),
          "loadMdDiscFromDeviceUtoc failed on complex 240-track UTOC: " + err);
    check(loaded.loadedDisc.active, "loadedDisc.active must be true");
    check(loaded.tracks.size() == 240,
          "expected 240 tracks, got " + std::to_string(loaded.tracks.size()));
    check(loaded.medium == carta::MdMedium::md74, "2202-cluster fixture must detect MdMedium::md74");
    check(loaded.loadedDisc.physicalStartCluster == 50 &&
              loaded.loadedDisc.physicalLastCluster == 2251,
          "expected physical cluster span 50..2251");
    check(!loaded.loadedDisc.baselineSector0Hash.empty(),
          "baselineSector0Hash must be populated");
    check(loaded.loadedDisc.rawSector3 ==
              std::vector<std::uint8_t>(rawSectors[3].begin(), rawSectors[3].end()),
          "raw Sector 3 bytes must be preserved verbatim on load");

    // Verify odd-unit SP Stereo exact timeline alignment & 0 sample drift on roundtrip
    int oddUnitSpParts = 0;
    for (const auto& tr : loaded.tracks) {
        for (const auto& part : tr.parts) {
            if (carta::effectiveTrackCodec(loaded, tr) == carta::MdCodecMode::spStereo) {
                const auto dur = part.durationSamples();
                check(dur > 0 && (dur % 256) == 0,
                      "SP Stereo loaded part duration must align cleanly to 256 samples");
                if ((dur % 512) == 256) {
                    ++oddUnitSpParts;
                }
            }
        }
    }
    check(oddUnitSpParts >= 48,
          "expected odd-unit SP Stereo parts preserved cleanly, got " +
              std::to_string(oddUnitSpParts));

    // Compile back cleanly and check exact fragment cluster.sector.group roundtrip + budget + P-DFA + P-FRA
    carta::UtocDiscState rtUtoc{};
    check(carta::compileMdDiscToUtoc(loaded, rtUtoc, err),
          "compileMdDiscToUtoc failed on 240-track loaded disc: " + err);
    const auto budget = carta::computeUtocBudget(
        rtUtoc, carta::mdMediumToString(loaded.medium), loaded.loadedDisc.physicalTotalClusters());
    check(budget.fits(), "240-track roundtrip must fit within 255-slot & physical cluster budget");
    check(rtUtoc.trackCount == 240, "roundtrip trackCount must remain 240");

    // Verify every track's physical fragment start/end addresses & flags match baseline identically
    for (int t = 1; t <= 240; ++t) {
        std::uint8_t a = baselineUtoc.trackMap[static_cast<std::size_t>(t)];
        std::uint8_t b = rtUtoc.trackMap[static_cast<std::size_t>(t)];
        std::set<std::uint8_t> ga, gb;
        while (a != 0 && b != 0 && ga.insert(a).second && gb.insert(b).second) {
            const auto& fa = baselineUtoc.fragments[a];
            const auto& fb = rtUtoc.fragments[b];
            check(fa.start == fb.start && fa.end == fb.end && fa.mode == fb.mode,
                  "exact fragment start/end/mode mismatch at track " + std::to_string(t));
            a = fa.link;
            b = fb.link;
        }
        check(a == 0 && b == 0, "fragment chain length mismatch at track " + std::to_string(t));
    }

    // Verify P-DFA and P-FRA preserved
    auto collectChainRanges = [](const carta::UtocDiscState& s, std::uint8_t head) {
        std::vector<std::pair<carta::UtocAddress, carta::UtocAddress>> out;
        std::set<std::uint8_t> seen;
        for (std::uint8_t cur = head; cur != 0 && seen.insert(cur).second;
             cur = s.fragments[cur].link) {
            out.emplace_back(s.fragments[cur].start, s.fragments[cur].end);
        }
        return out;
    };
    check(collectChainRanges(rtUtoc, rtUtoc.sec0aPadding[14]) ==
              collectChainRanges(baselineUtoc, baselineUtoc.sec0aPadding[14]),
          "P-DFA defective cluster list must roundtrip verbatim");
    check(!collectChainRanges(rtUtoc, rtUtoc.trackMap[0]).empty(),
          "P-FRA free range list must roundtrip");

    // Complex edit #1: rename, reorder, flag toggle preserving non-flag bits, alias duplicate, odd-unit split
    {
        carta::MdDisc edited = loaded;
        edited.halfWidthTitle = "Edited 24H";
        edited.tracks[0].halfWidthTitle = "Renamed T1";
        edited.tracks[0].writeProtected = !edited.tracks[0].writeProtected;
        std::swap(edited.tracks[0], edited.tracks[1]);
        carta::UtocDiscState editedUtoc{};
        check(carta::compileMdDiscToUtoc(edited, editedUtoc, err),
              "compile after rename/reorder/WP toggle failed: " + err);
        const auto s1 = editedUtoc.trackMap[1];
        const auto s2 = editedUtoc.trackMap[2];
        check(s1 != 0 && s2 != 0 &&
                  editedUtoc.fragments[s2].mode != baselineUtoc.fragments[baselineUtoc.trackMap[1]].mode,
              "swapped Track 1->2 reflects toggled flag");
        if (isSyntheticFixture) {
            check((editedUtoc.fragments[s2].mode & carta::kUtocReservedCodec) != 0,
                  "non-flag mode bit kUtocReservedCodec preserved across WP toggle");
        }

        // Odd-unit SP split on Track 49 (23 units = 23*256 = 5888 samples) -> snaps to 256-sample boundary
        if (edited.tracks.size() > 49 && edited.tracks[49].parts.size() == 1 &&
            carta::effectiveTrackCodec(edited, edited.tracks[49]) ==
                carta::MdCodecMode::spStereo) {
            const auto p0 = edited.tracks[49].parts[0];
            if (p0.durationSamples() > 1024) {
                const auto mid = p0.startSample + 700; // unaligned -> snaps to multiple of 256!
                check(carta::splitMdTrackAtSample(edited, 49, mid, err),
                      "odd-unit SP split failed: " + err);
                const auto& leftP = edited.tracks[49].parts.back();
                const auto& rightP = edited.tracks[50].parts.front();
                check(leftP.endSample == rightP.startSample &&
                          ((leftP.endSample - p0.startSample) % 256) == 0,
                      "SP split must snap strictly to 256-sample unit grid with zero gap");
            }
        }
    }

    // Complex edit #2: Append a new local LP2 track beyond loaded timeline end -> plans cleanly
    {
        carta::MdDisc appended = loaded;
        const std::int64_t endTimeline = loaded.loadedDisc.regions.back().timelineEndSample;
        carta::MdTrack newTrk;
        newTrk.id = carta::makeMdTrackId();
        newTrk.halfWidthTitle = "New Bonus";
        newTrk.codecOverride = carta::MdCodecMode::lp2;
        newTrk.parts.push_back({endTimeline, endTimeline + 44100LL * 4});
        appended.tracks.push_back(std::move(newTrk));
        // Only if <= 255 tracks (241 <= 255)
        carta::MdPhysicalPlan plan{};
        check(carta::planMdPhysicalAudio(appended, plan, err),
              "planMdPhysicalAudio with appended local LP2 track failed: " + err);
        check(plan.spans.size() == 1 && plan.newUploadClusters > 0 && plan.onDiscClusters > 0,
              "appended LP2 track produces exactly 1 new upload span preserving onDiscClusters");
    }

    if (isSyntheticFixture) {
        // Delete 1 of 24 tracks sharing MUS 1 -> 0 reclaimed clusters
        {
            carta::MdDisc d1 = loaded;
            d1.tracks.erase(d1.tracks.begin());
            carta::MdPhysicalPlan p1{};
            check(carta::planMdPhysicalAudio(d1, p1, err),
                  "plan after deleting 1 of 24 MUS 1 tracks failed: " + err);
            check(p1.reclaimedClusters == 0,
                  "deleting 1 of 24 shared DAG tracks must reclaim 0 clusters");
        }

        // Delete all 24 MUS 1 tracks (clusters 310..748 where 748 is shared) -> reclaims 310..747 ONLY!
        {
            carta::MdDisc dMus1 = loaded;
            dMus1.tracks.erase(dMus1.tracks.begin(), dMus1.tracks.begin() + 24);
            carta::MdPhysicalPlan pMus1{};
            check(carta::planMdPhysicalAudio(dMus1, pMus1, err),
                  "plan after deleting all 24 MUS 1 tracks failed: " + err);
            check(pMus1.reclaimedClusters == (747 - 310 + 1),
                  "expected 310..747 (438 clusters) reclaimed leaving shared cluster 748 untouched, "
                  "got " +
                      std::to_string(pMus1.reclaimedClusters));
            carta::UtocDiscState uMus1{};
            check(carta::compileMdDiscToUtoc(dMus1, uMus1, err),
                  "compile after deleting MUS 1 failed: " + err);
            bool saw310to747 = false;
            bool touched748 = false;
            for (std::uint8_t cur = uMus1.trackMap[0]; cur != 0; cur = uMus1.fragments[cur].link) {
                const auto& f = uMus1.fragments[cur];
                if (f.start.cluster <= 310 && f.end.cluster >= 747) {
                    saw310to747 = true;
                }
                if (f.start.cluster <= 748 && f.end.cluster >= 748) {
                    touched748 = true;
                }
            }
            check(saw310to747 && !touched748,
                  "P-FRA after deleting MUS 1 must cover 310..747 and NOT include shared cluster 748");
        }

        // Delete all 24 MUS 4 tracks (indices 120..143 -> 1625..1999 & 2100..2251)
        {
            carta::MdDisc dMus4 = loaded;
            dMus4.tracks.erase(dMus4.tracks.begin() + 120, dMus4.tracks.begin() + 144);
            carta::MdPhysicalPlan pMus4{};
            check(carta::planMdPhysicalAudio(dMus4, pMus4, err),
                  "plan after deleting all 24 MUS 4 tracks failed: " + err);
            const int expectedMus4Clu = (1999 - 1625 + 1) + (2251 - 2100 + 1);
            check(pMus4.reclaimedClusters == expectedMus4Clu,
                  "expected MUS 4 reclaim of " + std::to_string(expectedMus4Clu) + " clusters, got " +
                      std::to_string(pMus4.reclaimedClusters));
        }
    }
}

void testComplexLoadedDisc240TracksAndReclaim() {
    std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> rawSectors{};
    buildComplex240TrackUtocFixture(rawSectors);
    verifyLoadedComplex240RoundtripAndEdits(rawSectors, true);

    if (const char* fixturePath = std::getenv("CARTA_REAL_UTOC_FIXTURE");
        fixturePath != nullptr && fixturePath[0] != '\0') {
        std::ifstream in(fixturePath, std::ios::binary);
        check(in.good(), std::string("failed opening CARTA_REAL_UTOC_FIXTURE: ") + fixturePath);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        check(bytes.size() == 5 * carta::kUtocSectorBytes,
              "real UTOC fixture must be 11760 bytes (5 sectors)");
        std::array<std::array<std::uint8_t, carta::kUtocSectorBytes>, 5> realSecs{};
        for (std::size_t s = 0; s < 5; ++s) {
            std::memcpy(realSecs[s].data(), bytes.data() + s * carta::kUtocSectorBytes,
                        carta::kUtocSectorBytes);
        }
        verifyLoadedComplex240RoundtripAndEdits(realSecs, false);

        // Verify MUS 1 (title == "MUS 1") & MUS 4 (title == "MUS 4") deletion behavior on the real hardware dump
        carta::MdDisc realLoaded{};
        std::string err;
        check(carta::loadMdDiscFromDeviceUtoc(realSecs, "real:ro-fixture", realLoaded, err),
              "loadMdDiscFromDeviceUtoc failed on real dump: " + err);
        {
            carta::MdDisc dMus1 = realLoaded;
            dMus1.tracks.erase(
                std::remove_if(dMus1.tracks.begin(), dMus1.tracks.end(),
                               [](const carta::MdTrack& t) { return t.halfWidthTitle == "MUS 1"; }),
                dMus1.tracks.end());
            check(dMus1.tracks.size() == 216,
                  "expected 24 'MUS 1' tracks removed (216 left), got " +
                      std::to_string(dMus1.tracks.size()));
            carta::MdPhysicalPlan pMus1{};
            check(carta::planMdPhysicalAudio(dMus1, pMus1, err),
                  "real dump MUS 1 delete plan failed: " + err);
            check(pMus1.reclaimedClusters == (747 - 310 + 1),
                  "real dump deleting MUS 1 must reclaim clusters 310..747 (438) leaving "
                  "shared 748 untouched, got " +
                      std::to_string(pMus1.reclaimedClusters));
            bool has310to747 = false;
            bool touches748 = false;
            for (const auto& fr : pMus1.reclaimedFreeRanges) {
                if (fr.start.cluster <= 310 && fr.end.cluster >= 747) {
                    has310to747 = true;
                }
                if (fr.start.cluster <= 748 && fr.end.cluster >= 748) {
                    touches748 = true;
                }
            }
            check(has310to747 && !touches748,
                  "real dump MUS 1 reclaim ranges must cover 310..747 and leave shared 748 untouched");
        }
        {
            carta::MdDisc dMus4 = realLoaded;
            dMus4.tracks.erase(
                std::remove_if(dMus4.tracks.begin(), dMus4.tracks.end(),
                               [](const carta::MdTrack& t) { return t.halfWidthTitle == "MUS 4"; }),
                dMus4.tracks.end());
            check(dMus4.tracks.size() == 216,
                  "expected 24 'MUS 4' tracks removed (216 left), got " +
                      std::to_string(dMus4.tracks.size()));
            carta::MdPhysicalPlan pMus4{};
            check(carta::planMdPhysicalAudio(dMus4, pMus4, err),
                  "real dump MUS 4 delete plan failed: " + err);
            check(pMus4.reclaimedClusters == 436,
                  "real dump deleting MUS 4 must reclaim clusters 1625..2060 (436) leaving shared "
                  "1624 untouched, got " +
                      std::to_string(pMus4.reclaimedClusters));
            bool merged1625to2251 = false;
            bool touches1624 = false;
            for (const auto& fr : pMus4.reclaimedFreeRanges) {
                if (fr.start.cluster <= 1625 && fr.end.cluster >= 2251) {
                    merged1625to2251 = true;
                }
                if (fr.start.cluster <= 1624 && fr.end.cluster >= 1624) {
                    touches1624 = true;
                }
            }
            check(merged1625to2251 && !touches1624 && pMus4.freeClustersAfterReclaim == 627,
                  "real dump MUS 4 reclaim must merge with P-FRA into 1625..2251 (627 whole "
                  "clusters, leaving 1624 untouched), got whole=" +
                      std::to_string(pMus4.freeClustersAfterReclaim));
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* dumpDir = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--dump")
            dumpDir = argv[i + 1];
    }
    testAddressPacking();
    testModeBitsAndTimestamps();
    testTitlesAndGroups();
    testDiscRoundtripAndZeroCopyReuse(dumpDir);
    testMdFlagsAndImportedUtocPreservation();
    testOverflowLimits();
    testComplexLoadedDisc240TracksAndReclaim();
    if (gFailures == 0) {
        std::cout << "UtocTests passed.\n";
        return 0;
    }
    return 1;
}
