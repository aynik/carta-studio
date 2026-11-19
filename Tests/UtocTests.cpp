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

#include <cstdlib>
#include <fstream>
#include <iostream>
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
    if (gFailures == 0) {
        std::cout << "UtocTests passed.\n";
        return 0;
    }
    return 1;
}
