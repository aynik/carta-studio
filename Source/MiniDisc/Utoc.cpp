/**
 * @file Utoc.cpp
 * Implementation of Rainbow Book UTOC sector encoding/decoding, title & group
 * serialization, DAG fragment pooling, and authored-disc reconstruction.
 *
 * Binary Sector Offsets (2,352 bytes per sector):
 * - Sector 0: `0x00..0x1B` padding, `0x1C` signature (BE16), `0x1E` non-empty
 *   flag, `0x1F` track count, `0x20..0x2E` padding, `0x2F` next free slot,
 *   `0x30..0x12F` 256-byte track/P-FRA slot map, `0x130..0x92F` 256 8-byte
 *   `UtocFragment` entries (`start[3]`, `mode[1]`, `end[3]`, `link[1]`).
 * - Sectors 1/4: `0x00..0x2E` padding, `0x2F` next free cell, `0x30..0x12F`
 *   title slot map, `0x130..0x92F` 256 8-byte cells (`bytes[7]`, `link[1]`).
 * - Sector 2: `0x00..0x2E` padding, `0x2F` next free timestamp slot, `0x30..0x12F`
 *   slot map, `0x130..0x92F` 256 8-byte timestamps (`Y,M,D,H,M,S,sigBE16`).
 */

#include "MiniDisc/Utoc.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace carta {
namespace {

// ============================================================================
// Internal Binary, UTF-8/JIS, & Slot-Pool Helpers
// ============================================================================

std::uint16_t readBe16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8) | p[1]);
}

void writeBe16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<std::uint8_t>(v & 0xFF);
}

/** Unpacks a 3-byte Rainbow Book address (`cluster[14] | sector[6] | group[4]`). */
UtocAddress decodeAddr(const std::uint8_t* p) noexcept {
    UtocAddress addr;
    addr.cluster = static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 6) | (p[1] >> 2));
    addr.sector = static_cast<std::uint8_t>(((p[1] & 0x03) << 4) | (p[2] >> 4));
    addr.group = static_cast<std::uint8_t>(p[2] & 0x0F);
    return addr;
}

/** Packs a `UtocAddress` into 3 big-endian Rainbow Book address bytes. */
void encodeAddr(std::uint8_t* p, const UtocAddress& addr) noexcept {
    p[0] = static_cast<std::uint8_t>((addr.cluster >> 6) & 0xFF);
    p[1] = static_cast<std::uint8_t>(((addr.cluster & 0x3F) << 2) | ((addr.sector & 0x30) >> 4));
    p[2] = static_cast<std::uint8_t>(((addr.sector & 0x0F) << 4) | (addr.group & 0x0F));
}

bool decodeUtf8Next(const std::string& s, std::size_t& pos, std::uint32_t& cp) noexcept {
    if (pos >= s.size()) {
        return false;
    }
    const auto b0 = static_cast<unsigned char>(s[pos]);
    if (b0 < 0x80) {
        cp = b0;
        ++pos;
        return true;
    }
    if ((b0 & 0xE0) == 0xC0 && pos + 1 < s.size()) {
        const auto b1 = static_cast<unsigned char>(s[pos + 1]);
        if ((b1 & 0xC0) == 0x80) {
            cp = ((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu);
            pos += 2;
            return true;
        }
    }
    if ((b0 & 0xF0) == 0xE0 && pos + 2 < s.size()) {
        const auto b1 = static_cast<unsigned char>(s[pos + 1]);
        const auto b2 = static_cast<unsigned char>(s[pos + 2]);
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
            cp = ((b0 & 0x0Fu) << 12) | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu);
            pos += 3;
            return true;
        }
    }
    if ((b0 & 0xF8) == 0xF0 && pos + 3 < s.size()) {
        const auto b1 = static_cast<unsigned char>(s[pos + 1]);
        const auto b2 = static_cast<unsigned char>(s[pos + 2]);
        const auto b3 = static_cast<unsigned char>(s[pos + 3]);
        if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
            cp = ((b0 & 0x07u) << 18) | ((b1 & 0x3Fu) << 12) | ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu);
            pos += 4;
            return true;
        }
    }
    cp = 0xFFFD;
    ++pos;
    return true;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::vector<std::uint32_t> utf8ToCodepoints(const std::string& utf8) {
    std::vector<std::uint32_t> cps;
    std::size_t pos = 0;
    std::uint32_t cp = 0;
    while (decodeUtf8Next(utf8, pos, cp)) {
        cps.push_back(cp);
    }
    return cps;
}

struct KanaHalfWidthEntry {
    std::uint32_t cp;
    std::uint8_t mainByte;
    std::uint8_t markByte;
};

constexpr KanaHalfWidthEntry kKatakanaToHalfWidth[] = {
    {0x30A1, 0xA7, 0},    // ァ
    {0x30A2, 0xB1, 0},    // ア
    {0x30A3, 0xA8, 0},    // ィ
    {0x30A4, 0xB2, 0},    // イ
    {0x30A5, 0xA9, 0},    // ゥ
    {0x30A6, 0xB3, 0},    // ウ
    {0x30F4, 0xB3, 0xDE}, // ヴ
    {0x30A7, 0xAA, 0},    // ェ
    {0x30A8, 0xB4, 0},    // エ
    {0x30A9, 0xAB, 0},    // ォ
    {0x30AA, 0xB5, 0},    // オ
    {0x30AB, 0xB6, 0},    // カ
    {0x30AC, 0xB6, 0xDE}, // ガ
    {0x30AD, 0xB7, 0},    // キ
    {0x30AE, 0xB7, 0xDE}, // ギ
    {0x30AF, 0xB8, 0},    // ク
    {0x30B0, 0xB8, 0xDE}, // グ
    {0x30B1, 0xB9, 0},    // ケ
    {0x30B2, 0xB9, 0xDE}, // ゲ
    {0x30B3, 0xBA, 0},    // コ
    {0x30B4, 0xBA, 0xDE}, // ゴ
    {0x30B5, 0xBB, 0},    // サ
    {0x30B6, 0xBB, 0xDE}, // ザ
    {0x30B7, 0xBC, 0},    // シ
    {0x30B8, 0xBC, 0xDE}, // ジ
    {0x30B9, 0xBD, 0},    // ス
    {0x30BA, 0xBD, 0xDE}, // ズ
    {0x30BB, 0xBE, 0},    // セ
    {0x30BC, 0xBE, 0xDE}, // ゼ
    {0x30BD, 0xBF, 0},    // ソ
    {0x30BE, 0xBF, 0xDE}, // ゾ
    {0x30BF, 0xC0, 0},    // タ
    {0x30C0, 0xC0, 0xDE}, // ダ
    {0x30C1, 0xC1, 0},    // チ
    {0x30C2, 0xC1, 0xDE}, // ヂ
    {0x30C3, 0xAF, 0},    // ッ
    {0x30C4, 0xC2, 0},    // ツ
    {0x30C5, 0xC2, 0xDE}, // ヅ
    {0x30C6, 0xC3, 0},    // テ
    {0x30C7, 0xC3, 0xDE}, // デ
    {0x30C8, 0xC4, 0},    // ト
    {0x30C9, 0xC4, 0xDE}, // ド
    {0x30CA, 0xC5, 0},    // ナ
    {0x30CB, 0xC6, 0},    // ニ
    {0x30CC, 0xC7, 0},    // ヌ
    {0x30CD, 0xC8, 0},    // ネ
    {0x30CE, 0xC9, 0},    // ノ
    {0x30CF, 0xCA, 0},    // ハ
    {0x30D0, 0xCA, 0xDE}, // バ
    {0x30D1, 0xCA, 0xDF}, // パ
    {0x30D2, 0xCB, 0},    // ヒ
    {0x30D3, 0xCB, 0xDE}, // ビ
    {0x30D4, 0xCB, 0xDF}, // ピ
    {0x30D5, 0xCC, 0},    // フ
    {0x30D6, 0xCC, 0xDE}, // ブ
    {0x30D7, 0xCC, 0xDF}, // プ
    {0x30D8, 0xCD, 0},    // ヘ
    {0x30D9, 0xCD, 0xDE}, // ベ
    {0x30DA, 0xCD, 0xDF}, // ペ
    {0x30DB, 0xCE, 0},    // ホ
    {0x30DC, 0xCE, 0xDE}, // ボ
    {0x30DD, 0xCE, 0xDF}, // ポ
    {0x30DE, 0xCF, 0},    // マ
    {0x30DF, 0xD0, 0},    // ミ
    {0x30E0, 0xD1, 0},    // ム
    {0x30E1, 0xD2, 0},    // メ
    {0x30E2, 0xD3, 0},    // モ
    {0x30E3, 0xAC, 0},    // ャ
    {0x30E4, 0xD4, 0},    // ヤ
    {0x30E5, 0xAD, 0},    // ュ
    {0x30E6, 0xD5, 0},    // ユ
    {0x30E7, 0xAE, 0},    // ョ
    {0x30E8, 0xD6, 0},    // ヨ
    {0x30E9, 0xD7, 0},    // ラ
    {0x30EA, 0xD8, 0},    // リ
    {0x30EB, 0xD9, 0},    // ル
    {0x30EC, 0xDA, 0},    // レ
    {0x30ED, 0xDB, 0},    // ロ
    {0x30EE, 0xDC, 0},    // ヮ
    {0x30EF, 0xDC, 0},    // ワ
    {0x30F2, 0xA6, 0},    // ヲ
    {0x30F3, 0xDD, 0},    // ン
};

/** Converts a Hiragana or full-width Katakana codepoint to JIS X 0201 half-width bytes
 * (0xA1..0xDF). */
bool fullKanaToHalfBytes(std::uint32_t cp, std::uint8_t& outMain, std::uint8_t& outMark) noexcept {
    outMark = 0;
    if (cp >= 0xFF61 && cp <= 0xFF9F) {
        outMain = static_cast<std::uint8_t>(0xA1 + (cp - 0xFF61));
        return true;
    }
    if (cp == 0x3002) {
        outMain = 0xA1; // 。
        return true;
    }
    if (cp == 0x300C) {
        outMain = 0xA2; // 「
        return true;
    }
    if (cp == 0x300D) {
        outMain = 0xA3; // 」
        return true;
    }
    if (cp == 0x3001) {
        outMain = 0xA4; // 、
        return true;
    }
    if (cp == 0x30FB) {
        outMain = 0xA5; // ・
        return true;
    }
    if (cp == 0x30FC) {
        outMain = 0xB0; // ー
        return true;
    }
    if (cp == 0x309B || cp == 0x3099) {
        outMain = 0xDE;
        return true;
    }
    if (cp == 0x309C || cp == 0x309A) {
        outMain = 0xDF;
        return true;
    }

    // Fold Hiragana (0x3041..0x3093) to Katakana (0x30A1..0x30F3)
    if (cp >= 0x3041 && cp <= 0x3093) {
        cp += 0x60;
    }

    for (const auto& entry : kKatakanaToHalfWidth) {
        if (entry.cp == cp) {
            outMain = entry.mainByte;
            outMark = entry.markByte;
            return true;
        }
    }
    return false;
}

char foldLatinFallback(std::uint32_t cp) noexcept {
    if (cp >= 0xC0 && cp <= 0xC5) {
        return 'A';
    }
    if (cp == 0xC7) {
        return 'C';
    }
    if (cp >= 0xC8 && cp <= 0xCB) {
        return 'E';
    }
    if (cp >= 0xCC && cp <= 0xCF) {
        return 'I';
    }
    if (cp == 0xD1) {
        return 'N';
    }
    if (cp >= 0xD2 && cp <= 0xD6) {
        return 'O';
    }
    if (cp >= 0xD9 && cp <= 0xDC) {
        return 'U';
    }
    if (cp == 0xDD) {
        return 'Y';
    }
    if (cp >= 0xE0 && cp <= 0xE5) {
        return 'a';
    }
    if (cp == 0xE7) {
        return 'c';
    }
    if (cp >= 0xE8 && cp <= 0xEB) {
        return 'e';
    }
    if (cp >= 0xEC && cp <= 0xEF) {
        return 'i';
    }
    if (cp == 0xF1) {
        return 'n';
    }
    if (cp >= 0xF2 && cp <= 0xF6) {
        return 'o';
    }
    if (cp >= 0xF9 && cp <= 0xFC) {
        return 'u';
    }
    if (cp == 0xFD || cp == 0xFF) {
        return 'y';
    }
    if (cp == 0x2013 || cp == 0x2014 || cp == 0xFF0D) {
        return '-';
    }
    return ' ';
}

std::uint32_t halfKanaToFullKanaBase(std::uint8_t byte) noexcept {
    static constexpr std::uint32_t kTable[63] = {
        0x3002, 0x300C, 0x300D, 0x3001, 0x30FB, 0x30F2, 0x30A1, 0x30A3, // A1..A8
        0x30A5, 0x30A7, 0x30A9, 0x30E3, 0x30E5, 0x30E7, 0x30C3, 0x30FC, // A9..B0
        0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA, 0x30AB, 0x30AD, 0x30AF, // B1..B8
        0x30B1, 0x30B3, 0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD, 0x30BF, // B9..C0
        0x30C1, 0x30C4, 0x30C6, 0x30C8, 0x30CA, 0x30CB, 0x30CC, 0x30CD, // C1..C8
        0x30CE, 0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x30DB, 0x30DE, 0x30DF, // C9..D0
        0x30E0, 0x30E1, 0x30E2, 0x30E4, 0x30E6, 0x30E8, 0x30E9, 0x30EA, // D1..D8
        0x30EB, 0x30EC, 0x30ED, 0x30EF, 0x30F3, 0x309B, 0x309C          // D9..DF
    };
    if (byte < 0xA1 || byte > 0xDF) {
        return 0x30FB;
    }
    return kTable[byte - 0xA1];
}

std::string normalizeToFullWidthUtf8(const std::string& utf8) {
    const auto cps = utf8ToCodepoints(utf8);
    std::string out;
    out.reserve(utf8.size() * 3);
    for (std::size_t i = 0; i < cps.size(); ++i) {
        const auto cp = cps[i];
        if (cp == ' ') {
            appendUtf8(out, 0x3000);
        } else if (cp >= 0x21 && cp <= 0x7E) {
            appendUtf8(out, 0xFF01 + (cp - 0x21));
        } else if (cp == 0x301C) {
            appendUtf8(out, 0xFF5E);
        } else if (cp >= 0xFF61 && cp <= 0xFF9F) {
            const auto byte = static_cast<std::uint8_t>(0xA1 + (cp - 0xFF61));
            std::uint32_t mapped = halfKanaToFullKanaBase(byte);
            if (i + 1 < cps.size() &&
                (cps[i + 1] == 0xFF9E || cps[i + 1] == 0x309B || cps[i + 1] == 0x3099)) {
                if (mapped == 0x30A6) {
                    mapped = 0x30F4;
                    ++i;
                } else if ((mapped >= 0x30AB && mapped <= 0x30C1 && ((mapped - 0x30AB) % 2 == 0)) ||
                           mapped == 0x30C4 || mapped == 0x30C6 || mapped == 0x30C8 ||
                           mapped == 0x30CF || mapped == 0x30D2 || mapped == 0x30D5 ||
                           mapped == 0x30D8 || mapped == 0x30DB) {
                    mapped += 1;
                    ++i;
                }
            } else if (i + 1 < cps.size() &&
                       (cps[i + 1] == 0xFF9F || cps[i + 1] == 0x309C || cps[i + 1] == 0x309A)) {
                if (mapped == 0x30CF || mapped == 0x30D2 || mapped == 0x30D5 || mapped == 0x30D8 ||
                    mapped == 0x30DB) {
                    mapped += 2;
                    ++i;
                }
            }
            appendUtf8(out, mapped);
        } else {
            appendUtf8(out, cp);
        }
    }
    return out;
}

bool runIconv(const char* toEnc, const char* fromEnc, const std::uint8_t* inData, std::size_t inLen,
              std::vector<std::uint8_t>& outData, std::string& error) {
    outData.clear();
    if (inLen == 0) {
        return true;
    }
#if defined(_WIN32)
    const auto cpForName = [](const char* enc) noexcept -> UINT {
        if (enc && (std::strcmp(enc, "CP932") == 0 || std::strcmp(enc, "SHIFT_JIS") == 0)) {
            return 932u;
        }
        return CP_UTF8;
    };
    const UINT srcCp = cpForName(fromEnc);
    const UINT dstCp = cpForName(toEnc);
    const int wideLen = MultiByteToWideChar(srcCp, 0, reinterpret_cast<const char*>(inData),
                                            static_cast<int>(inLen), nullptr, 0);
    if (wideLen <= 0) {
        error = std::string("MultiByteToWideChar failed for ") + fromEnc + " -> " + toEnc;
        return false;
    }
    std::vector<wchar_t> wide(static_cast<std::size_t>(wideLen));
    if (MultiByteToWideChar(srcCp, 0, reinterpret_cast<const char*>(inData),
                            static_cast<int>(inLen), wide.data(), wideLen) != wideLen) {
        error =
            std::string("MultiByteToWideChar conversion failed for ") + fromEnc + " -> " + toEnc;
        return false;
    }
    const int outLen =
        WideCharToMultiByte(dstCp, 0, wide.data(), wideLen, nullptr, 0, nullptr, nullptr);
    if (outLen <= 0) {
        error = std::string("WideCharToMultiByte sizing failed for ") + fromEnc + " -> " + toEnc;
        return false;
    }
    outData.resize(static_cast<std::size_t>(outLen));
    if (WideCharToMultiByte(dstCp, 0, wide.data(), wideLen, reinterpret_cast<char*>(outData.data()),
                            outLen, nullptr, nullptr) != outLen) {
        outData.clear();
        error =
            std::string("WideCharToMultiByte conversion failed for ") + fromEnc + " -> " + toEnc;
        return false;
    }
    return true;
#else
    iconv_t cd = iconv_open(toEnc, fromEnc);
    if (cd == reinterpret_cast<iconv_t>(-1)) {
        error = std::string("iconv_open failed for ") + fromEnc + " -> " + toEnc;
        return false;
    }
    std::vector<std::uint8_t> buf(std::max<std::size_t>(inLen * 4 + 16, 64));
    char* inPtr = const_cast<char*>(reinterpret_cast<const char*>(inData));
    std::size_t inLeft = inLen;
    while (inLeft > 0) {
        char* outPtr = reinterpret_cast<char*>(buf.data());
        std::size_t outLeft = buf.size();
        const std::size_t res = iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft);
        const std::size_t produced = buf.size() - outLeft;
        outData.insert(outData.end(), buf.data(), buf.data() + produced);
        if (res == static_cast<std::size_t>(-1)) {
            if (errno == E2BIG) {
                continue;
            }
            // Skip invalid/unrepresentable sequence gently to avoid truncating whole titles.
            if (inLeft > 0) {
                ++inPtr;
                --inLeft;
            }
        }
    }
    // Flush any shift state.
    char* outPtr = reinterpret_cast<char*>(buf.data());
    std::size_t outLeft = buf.size();
    iconv(cd, nullptr, nullptr, &outPtr, &outLeft);
    outData.insert(outData.end(), buf.data(), buf.data() + (buf.size() - outLeft));
    iconv_close(cd);
    return true;
#endif
}

std::string fullWidthRangeToHalf(const std::string& s) {
    const auto cps = utf8ToCodepoints(s);
    std::string out;
    for (auto cp : cps) {
        if (cp >= 0xFF10 && cp <= 0xFF19) {
            out.push_back(static_cast<char>('0' + (cp - 0xFF10)));
        } else if (cp == 0xFF0D || cp == 0x2212 || cp == 0x2013 || cp == 0x30FC) {
            out.push_back('-');
        } else if (cp == 0xFF0F) {
            out.push_back('/');
        } else if (cp == 0xFF1B) {
            out.push_back(';');
        } else if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        }
    }
    return out;
}

bool parsePositiveInt(const std::string& s, int& v) noexcept {
    if (s.empty()) {
        return false;
    }
    long acc = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        acc = acc * 10 + (c - '0');
        if (acc > 10000) {
            return false;
        }
    }
    v = static_cast<int>(acc);
    return true;
}

std::vector<std::uint8_t> readCellChain(const std::array<UtocTitleCell, kUtocSlots>& cells,
                                        std::uint8_t startSlot) {
    std::vector<std::uint8_t> bytes;
    if (startSlot == 0) {
        return bytes;
    }
    std::array<bool, kUtocSlots> seen{};
    std::uint8_t slot = startSlot;
    for (int depth = 0; slot != 0 && depth < 256; ++depth) {
        if (seen[slot]) {
            break;
        }
        seen[slot] = true;
        const auto& cell = cells[slot];
        for (std::size_t i = 0; i < 7; ++i) {
            if (cell.bytes[i] == 0) {
                return bytes;
            }
            bytes.push_back(cell.bytes[i]);
        }
        slot = cell.link;
    }
    return bytes;
}

std::string readHalfWidthTitleSlot(const UtocDiscState& state, int slotIndex) {
    if (!state.hasSector1) {
        return {};
    }
    const auto bytes = readCellChain(state.halfTitleCells,
                                     state.halfTitleMap[static_cast<std::size_t>(slotIndex)]);
    return decodeHalfWidthBytes(bytes.data(), bytes.size());
}

std::string readFullWidthTitleSlot(const UtocDiscState& state, int slotIndex) {
    if (!state.hasSector4) {
        return {};
    }
    const auto bytes = readCellChain(state.fullTitleCells,
                                     state.fullTitleMap[static_cast<std::size_t>(slotIndex)]);
    return decodeFullWidthShiftJis(bytes.data(), bytes.size());
}

bool allocateTitleCells(std::array<UtocTitleCell, kUtocSlots>& cells, std::uint8_t& nextFree,
                        const std::vector<std::uint8_t>& bytes, std::uint8_t& outStartSlot,
                        std::string& error) {
    outStartSlot = 0;
    if (bytes.empty()) {
        return true;
    }
    const int needed = static_cast<int>((bytes.size() + 6) / 7);
    std::vector<std::uint8_t> slots;
    slots.reserve(static_cast<std::size_t>(needed));
    for (int i = 0; i < needed; ++i) {
        if (nextFree == 0) {
            error = "Title cell table exceeded 255 cells.";
            return false;
        }
        const std::uint8_t slot = nextFree;
        nextFree = cells[slot].link;
        cells[slot] = {};
        slots.push_back(slot);
    }
    for (std::size_t i = 0; i < slots.size(); ++i) {
        auto& cell = cells[slots[i]];
        const std::size_t start = i * 7;
        for (std::size_t j = 0; j < 7 && start + j < bytes.size(); ++j) {
            cell.bytes[j] = bytes[start + j];
        }
        cell.link = (i + 1 < slots.size()) ? slots[i + 1] : 0;
    }
    outStartSlot = slots.front();
    return true;
}

void initFreeCellPool(std::array<UtocTitleCell, kUtocSlots>& cells, std::uint8_t& nextFree) {
    cells.fill({});
    nextFree = 1;
    for (int i = 1; i < 255; ++i) {
        cells[static_cast<std::size_t>(i)].link = static_cast<std::uint8_t>(i + 1);
    }
    cells[255].link = 0;
}

void initFreeFragmentPool(std::array<UtocFragment, kUtocSlots>& frags, std::uint8_t& nextFree) {
    frags.fill({});
    nextFree = 1;
    for (int i = 1; i < 255; ++i) {
        frags[static_cast<std::size_t>(i)].link = static_cast<std::uint8_t>(i + 1);
    }
    frags[255].link = 0;
}

struct GroupRawEntry {
    int startTrack = 0;
    int endTrack = 0;
    std::string title;
};

void parseGroupTokens(const std::string& raw, bool fullWidthDelims, std::string& discTitleOut,
                      std::vector<GroupRawEntry>& groupsOut) {
    discTitleOut = raw;
    groupsOut.clear();
    const std::string semi = fullWidthDelims ? "\xEF\xBC\x9B" : ";";
    const std::string slash2 = fullWidthDelims ? "\xEF\xBC\x8F\xEF\xBC\x8F" : "//";

    if (raw.find(slash2) == std::string::npos) {
        return;
    }

    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const auto next = raw.find(slash2, pos);
        if (next == std::string::npos) {
            const auto tail = raw.substr(pos);
            if (!tail.empty()) {
                parts.push_back(tail);
            }
            break;
        }
        parts.push_back(raw.substr(pos, next - pos));
        pos = next + slash2.size();
    }
    if (parts.empty()) {
        return;
    }

    bool foundDiscHeader = false;
    std::string collectedDiscTitle;
    for (const auto& part : parts) {
        const auto sPos = part.find(semi);
        if (sPos == std::string::npos) {
            continue;
        }
        const auto rangeRaw = fullWidthRangeToHalf(part.substr(0, sPos));
        const auto label = part.substr(sPos + semi.size());
        if (rangeRaw == "0") {
            collectedDiscTitle = label;
            foundDiscHeader = true;
            continue;
        }
        const auto dash = rangeRaw.find('-');
        int startT = 0;
        int endT = 0;
        if (dash == std::string::npos) {
            if (!parsePositiveInt(rangeRaw, startT) || startT <= 0) {
                continue;
            }
            endT = startT;
        } else {
            if (!parsePositiveInt(rangeRaw.substr(0, dash), startT) ||
                !parsePositiveInt(rangeRaw.substr(dash + 1), endT) || startT <= 0 ||
                endT < startT) {
                continue;
            }
        }
        groupsOut.push_back({startT, endT, label});
    }
    if (foundDiscHeader || !groupsOut.empty()) {
        discTitleOut = collectedDiscTitle;
    }
}

std::string fullWidthDigitsForRange(int startTrack, int endTrack) {
    const std::string ascii = startTrack == endTrack
                                  ? std::to_string(startTrack)
                                  : (std::to_string(startTrack) + "-" + std::to_string(endTrack));
    return normalizeToFullWidthUtf8(ascii);
}

/** Converts UTOC sound-group counts back to exact 44.1 kHz timeline samples per codec mode. */
std::int64_t samplesForUnits(int units, const std::string& mode) noexcept {
    if (units <= 0) {
        return 0;
    }
    if (mode == "sp-stereo") {
        return static_cast<std::int64_t>(units) * 256;
    }
    if (mode == "lp4") {
        return static_cast<std::int64_t>(units) * 1024;
    }
    return static_cast<std::int64_t>(units) * 512;
}

std::uint8_t modeBytePreservingNonFlags(std::uint8_t rawModeByte, MdCodecMode codec,
                                         bool writeProtected, bool scms, bool emphasis) {
    if (rawModeByte == 0) {
        return utocModeFromTrack(mdCodecModeToString(codec), writeProtected, scms, emphasis);
    }
    const bool origWp = (rawModeByte & kUtocWritable) == 0;
    const bool origScms = (rawModeByte & kUtocScmsDigCopy) != 0;
    const bool origEmph = (rawModeByte & kUtocEmphasis) != 0;
    if (writeProtected == origWp && scms == origScms && emphasis == origEmph) {
        return rawModeByte;
    }
    std::uint8_t m = static_cast<std::uint8_t>(
        rawModeByte &
        ~static_cast<std::uint8_t>(kUtocWritable | kUtocScmsDigCopy | kUtocEmphasis));
    if (!writeProtected) {
        m |= kUtocWritable;
    }
    if (scms) {
        m |= kUtocScmsDigCopy;
        m &= static_cast<std::uint8_t>(~kUtocScmsUnrestricted);
    }
    if (emphasis) {
        m |= kUtocEmphasis;
    }
    return m;
}

std::vector<MdPhysRange> readPhysRangeChain(const UtocDiscState& state, std::uint8_t headSlot) {
    std::vector<MdPhysRange> ranges;
    std::set<std::uint8_t> visited;
    std::uint8_t slot = headSlot;
    while (slot != 0 && visited.insert(slot).second) {
        const auto& f = state.fragments[slot];
        if (f.isValid() && f.end.toLinearGroup() >= f.start.toLinearGroup()) {
            ranges.push_back({{f.start.cluster, f.start.sector, f.start.group},
                              {f.end.cluster, f.end.sector, f.end.group}});
        }
        slot = f.link;
    }
    return ranges;
}

} // namespace

// ============================================================================
// Mode, Title, & Timestamp Codecs
// ============================================================================

int discMediumClusterCapacity(const std::string& medium) noexcept {
    if (medium == "md60") {
        return 1762;
    }
    if (medium == "md74") {
        return 2173;
    }
    if (medium == "md80") {
        return 2350;
    }
    return 0;
}

std::string audioModeFromUtocMode(std::uint8_t mode) {
    const bool stereo = (mode & kUtocStereo) != 0;
    const bool sp = (mode & kUtocSpMode) != 0;
    if (sp && stereo) {
        return "sp-stereo";
    }
    if (sp && !stereo) {
        return "sp-mono";
    }
    if (!sp && stereo) {
        return "lp2";
    }
    return "lp4";
}

std::uint8_t utocModeFromTrack(const std::string& audioMode, bool writeProtected, bool scms,
                               bool emphasis) {
    std::uint8_t m = 0;
    if (audioMode == "sp-mono") {
        m |= kUtocSpMode;
    } else if (audioMode == "lp2") {
        m |= kUtocStereo;
    } else if (audioMode == "lp4") {
        m |= 0;
    } else {
        m |= static_cast<std::uint8_t>(kUtocSpMode | kUtocStereo); // sp-stereo default
    }

    if (emphasis) {
        m |= kUtocEmphasis;
    }
    if (!writeProtected) {
        m |= kUtocWritable;
    }
    if (scms) {
        m |= kUtocScmsDigCopy;
    } else {
        m |= kUtocScmsUnrestricted;
    }
    return m;
}

std::vector<std::uint8_t> encodeHalfWidthBytes(const std::string& utf8) {
    const auto cps = utf8ToCodepoints(utf8);
    std::vector<std::uint8_t> out;
    out.reserve(cps.size() * 2);
    for (auto cp : cps) {
        if (cp == 0x3000) {
            out.push_back(' ');
            continue;
        }
        if (cp >= 0xFF01 && cp <= 0xFF5E) {
            out.push_back(static_cast<std::uint8_t>(0x21 + (cp - 0xFF01)));
            continue;
        }
        if (cp >= 0x20 && cp <= 0x7E) {
            out.push_back(static_cast<std::uint8_t>(cp));
            continue;
        }
        std::uint8_t mainB = 0;
        std::uint8_t markB = 0;
        if (fullKanaToHalfBytes(cp, mainB, markB)) {
            out.push_back(mainB);
            if (markB != 0) {
                out.push_back(markB);
            }
            continue;
        }
        out.push_back(static_cast<std::uint8_t>(foldLatinFallback(cp)));
    }
    return out;
}

std::string decodeHalfWidthBytes(const std::uint8_t* data, std::size_t size) {
    std::string out;
    for (std::size_t i = 0; i < size; ++i) {
        const auto b = data[i];
        if (b == 0) {
            break;
        }
        if (b >= 0x20 && b <= 0x7E) {
            out.push_back(static_cast<char>(b));
        } else if (b >= 0xA1 && b <= 0xDF) {
            appendUtf8(out, 0xFF61u + (b - 0xA1u));
        } else {
            out.push_back('?');
        }
    }
    return out;
}

std::vector<std::uint8_t> encodeFullWidthShiftJis(const std::string& utf8, std::string& error) {
    error.clear();
    if (utf8.empty()) {
        return {};
    }
    const auto full = normalizeToFullWidthUtf8(utf8);
    std::vector<std::uint8_t> sjis;
    if (!runIconv("CP932", "UTF-8", reinterpret_cast<const std::uint8_t*>(full.data()), full.size(),
                  sjis, error)) {
        return {};
    }
    return sjis;
}

std::string decodeFullWidthShiftJis(const std::uint8_t* data, std::size_t size) {
    std::size_t len = 0;
    while (len < size && data[len] != 0) {
        ++len;
    }
    if (len == 0) {
        return {};
    }
    std::vector<std::uint8_t> utf8Bytes;
    std::string ignored;
    if (!runIconv("UTF-8", "CP932", data, len, utf8Bytes, ignored)) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(utf8Bytes.data()), utf8Bytes.size());
}

bool parseUtocTimestampString(const std::string& iso, UtocTimestamp& out, std::string& error) {
    out = {};
    if (iso.empty()) {
        return true;
    }
    int y = 0;
    int mo = 0;
    int d = 0;
    int h = 0;
    int mi = 0;
    int s = 0;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6 &&
        std::sscanf(iso.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) {
        error = "Expected timestamp as YYYY-MM-DDTHH:MM:SS.";
        return false;
    }
    if (y < 1980 || y > 2255 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 ||
        mi > 59 || s < 0 || s > 59) {
        error = "Timestamp values are out of MiniDisc UTOC range.";
        return false;
    }
    out.year = static_cast<std::uint8_t>(y >= 2000 ? (y - 2000) : (y - 1900));
    out.month = static_cast<std::uint8_t>(mo);
    out.day = static_cast<std::uint8_t>(d);
    out.hour = static_cast<std::uint8_t>(h);
    out.minute = static_cast<std::uint8_t>(mi);
    out.second = static_cast<std::uint8_t>(s);
    out.signature = kCartaUtocSignature;
    return true;
}

std::string formatUtocTimestampString(const UtocTimestamp& ts) {
    if (!ts.isValid()) {
        return {};
    }
    const int fullYear = (ts.year >= 80) ? (1900 + ts.year) : (2000 + ts.year);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02u:%02u:%02u", fullYear, ts.month, ts.day,
                  ts.hour, ts.minute, ts.second);
    return buf;
}

std::vector<UtocGroupEntry> parseUtocGroups(const std::string& rawHalfTitle,
                                            const std::string& rawFullTitle, int trackCount,
                                            std::string& discHalfTitleOut,
                                            std::string& discFullTitleOut) {
    std::vector<GroupRawEntry> hwGroups;
    std::vector<GroupRawEntry> fwGroups;
    parseGroupTokens(rawHalfTitle, false, discHalfTitleOut, hwGroups);
    parseGroupTokens(rawFullTitle, true, discFullTitleOut, fwGroups);

    // Build merged ordered list keyed by (startTrack, endTrack).
    std::vector<UtocGroupEntry> result;
    auto mergeRange = [&](int s, int e, const std::string& hw, const std::string& fw) {
        if (s < 1 || e < s || (trackCount > 0 && e > trackCount)) {
            return;
        }
        for (auto& g : result) {
            if (g.startTrack == s && g.endTrack == e) {
                if (g.halfWidthTitle.empty()) {
                    g.halfWidthTitle = hw;
                }
                if (g.fullWidthTitle.empty()) {
                    g.fullWidthTitle = fw;
                }
                return;
            }
        }
        result.push_back({s, e, hw, fw});
    };
    for (const auto& g : hwGroups) {
        mergeRange(g.startTrack, g.endTrack, g.title, {});
    }
    for (const auto& g : fwGroups) {
        mergeRange(g.startTrack, g.endTrack, {}, g.title);
    }
    return result;
}

// ============================================================================
// Binary Sector Parser & Compiler
// ============================================================================

bool parseUtocSectors(const std::uint8_t* sector0, const std::uint8_t* sector1,
                      const std::uint8_t* sector2, const std::uint8_t* sector4, UtocDiscState& out,
                      std::string& error) {
    error.clear();
    out = {};
    if (!sector0 && !sector1 && !sector2 && !sector4) {
        error = "At least one 2352-byte UTOC sector must be provided.";
        return false;
    }

    if (sector0) {
        out.hasSector0 = true;
        std::memcpy(out.sec0Padding.data(), sector0, 28);
        out.deviceSignature = readBe16(sector0 + 0x1C);
        out.discNonEmpty = sector0[0x1E];
        out.trackCount = sector0[0x1F];
        std::memcpy(out.sec0aPadding.data(), sector0 + 0x20, 15);
        out.nextFreeTrackSlot = sector0[0x2F];
        std::memcpy(out.trackMap.data(), sector0 + 0x30, 256);
        const std::uint8_t* base = sector0 + 0x130;
        for (std::size_t i = 0; i < kUtocSlots; ++i) {
            const std::uint8_t* f = base + i * 8;
            out.fragments[i].start = decodeAddr(f);
            out.fragments[i].mode = f[3];
            out.fragments[i].end = decodeAddr(f + 4);
            out.fragments[i].link = f[7];
        }
    }

    if (sector1) {
        out.hasSector1 = true;
        std::memcpy(out.sec1Padding.data(), sector1, 47);
        out.nextFreeHalfTitleSlot = sector1[0x2F];
        std::memcpy(out.halfTitleMap.data(), sector1 + 0x30, 256);
        const std::uint8_t* base = sector1 + 0x130;
        for (std::size_t i = 0; i < kUtocSlots; ++i) {
            std::memcpy(out.halfTitleCells[i].bytes.data(), base + i * 8, 7);
            out.halfTitleCells[i].link = base[i * 8 + 7];
        }
    }

    if (sector2) {
        out.hasSector2 = true;
        std::memcpy(out.sec2Padding.data(), sector2, 47);
        out.nextFreeTimestampSlot = sector2[0x2F];
        std::memcpy(out.timestampMap.data(), sector2 + 0x30, 256);
        const std::uint8_t* base = sector2 + 0x130;
        for (std::size_t i = 0; i < kUtocSlots; ++i) {
            const std::uint8_t* t = base + i * 8;
            out.timestamps[i].year = t[0];
            out.timestamps[i].month = t[1];
            out.timestamps[i].day = t[2];
            out.timestamps[i].hour = t[3];
            out.timestamps[i].minute = t[4];
            out.timestamps[i].second = t[5];
            out.timestamps[i].signature = readBe16(t + 6);
        }
    }

    if (sector4) {
        out.hasSector4 = true;
        std::memcpy(out.sec4Padding.data(), sector4, 47);
        out.nextFreeFullTitleSlot = sector4[0x2F];
        std::memcpy(out.fullTitleMap.data(), sector4 + 0x30, 256);
        const std::uint8_t* base = sector4 + 0x130;
        for (std::size_t i = 0; i < kUtocSlots; ++i) {
            std::memcpy(out.fullTitleCells[i].bytes.data(), base + i * 8, 7);
            out.fullTitleCells[i].link = base[i * 8 + 7];
        }
    }

    return true;
}

bool compileUtocSectors(const UtocDiscState& state,
                        std::array<std::uint8_t, kUtocSectorBytes>& sector0,
                        std::array<std::uint8_t, kUtocSectorBytes>& sector1,
                        std::array<std::uint8_t, kUtocSectorBytes>& sector2,
                        std::array<std::uint8_t, kUtocSectorBytes>& sector4, std::string& error) {
    error.clear();
    sector0.fill(0);
    sector1.fill(0);
    sector2.fill(0);
    sector4.fill(0);

    // Sector 0: Track map, P-FRA pointer (slot 0), and audio fragment slots.
    std::memcpy(sector0.data(), state.sec0Padding.data(), 28);
    writeBe16(sector0.data() + 0x1C, state.deviceSignature);
    sector0[0x1E] = state.discNonEmpty;
    sector0[0x1F] = state.trackCount;
    std::memcpy(sector0.data() + 0x20, state.sec0aPadding.data(), 15);
    sector0[0x2F] = state.nextFreeTrackSlot;
    std::memcpy(sector0.data() + 0x30, state.trackMap.data(), 256);
    for (std::size_t i = 0; i < kUtocSlots; ++i) {
        std::uint8_t* f = sector0.data() + 0x130 + i * 8;
        encodeAddr(f, state.fragments[i].start);
        f[3] = state.fragments[i].mode;
        encodeAddr(f + 4, state.fragments[i].end);
        f[7] = state.fragments[i].link;
    }

    // Sector 1: Half-width ASCII / JIS X 0201 title chains.
    std::memcpy(sector1.data(), state.sec1Padding.data(), 47);
    sector1[0x2F] = state.nextFreeHalfTitleSlot;
    std::memcpy(sector1.data() + 0x30, state.halfTitleMap.data(), 256);
    for (std::size_t i = 0; i < kUtocSlots; ++i) {
        std::uint8_t* c = sector1.data() + 0x130 + i * 8;
        std::memcpy(c, state.halfTitleCells[i].bytes.data(), 7);
        c[7] = state.halfTitleCells[i].link;
    }

    // Sector 2: Recording timestamps.
    std::memcpy(sector2.data(), state.sec2Padding.data(), 47);
    sector2[0x2F] = state.nextFreeTimestampSlot;
    std::memcpy(sector2.data() + 0x30, state.timestampMap.data(), 256);
    for (std::size_t i = 0; i < kUtocSlots; ++i) {
        std::uint8_t* t = sector2.data() + 0x130 + i * 8;
        t[0] = state.timestamps[i].year;
        t[1] = state.timestamps[i].month;
        t[2] = state.timestamps[i].day;
        t[3] = state.timestamps[i].hour;
        t[4] = state.timestamps[i].minute;
        t[5] = state.timestamps[i].second;
        writeBe16(t + 6, state.timestamps[i].signature);
    }

    // Sector 4: Full-width Shift-JIS (CP932) title chains.
    std::memcpy(sector4.data(), state.sec4Padding.data(), 47);
    sector4[0x2F] = state.nextFreeFullTitleSlot;
    std::memcpy(sector4.data() + 0x30, state.fullTitleMap.data(), 256);
    for (std::size_t i = 0; i < kUtocSlots; ++i) {
        std::uint8_t* c = sector4.data() + 0x130 + i * 8;
        std::memcpy(c, state.fullTitleCells[i].bytes.data(), 7);
        c[7] = state.fullTitleCells[i].link;
    }
    return true;
}

// ============================================================================
// Budget, Authoring Conversion, & Inspection
// ============================================================================

UtocBudget computeUtocBudget(const UtocDiscState& state, const std::string& medium,
                             int totalClustersOverride) {
    UtocBudget budget;
    budget.totalClusters =
        totalClustersOverride > 0 ? totalClustersOverride : discMediumClusterCapacity(medium);
    budget.usedTracks = static_cast<int>(state.trackCount);

    std::set<int> usedFragSlots;
    std::set<std::uint16_t> occupiedClusters;
    std::map<std::tuple<std::uint16_t, std::uint8_t, std::uint8_t, std::uint16_t, std::uint8_t,
                        std::uint8_t>,
             int>
        physUseCount;

    for (int t = 1; t <= budget.usedTracks; ++t) {
        std::uint8_t slot = state.trackMap[static_cast<std::size_t>(t)];
        std::set<std::uint8_t> loopGuard;
        while (slot != 0 && !loopGuard.count(slot)) {
            loopGuard.insert(slot);
            usedFragSlots.insert(slot);
            const auto& frag = state.fragments[slot];
            if (frag.isValid()) {
                for (std::uint16_t c = frag.start.cluster; c <= frag.end.cluster; ++c) {
                    occupiedClusters.insert(c);
                }
                const auto key =
                    std::make_tuple(frag.start.cluster, frag.start.sector, frag.start.group,
                                    frag.end.cluster, frag.end.sector, frag.end.group);
                ++physUseCount[key];
            }
            slot = frag.link;
        }
    }
    budget.usedFragments = static_cast<int>(usedFragSlots.size());
    budget.usedClusters = static_cast<int>(occupiedClusters.size());
    for (const auto& [_, cnt] : physUseCount) {
        if (cnt > 1) {
            ++budget.sharedFragmentCount;
        }
    }

    auto countChainCells = [&](const std::array<std::uint8_t, kUtocSlots>& map,
                               const std::array<UtocTitleCell, kUtocSlots>& cells) {
        std::set<int> used;
        for (int idx = 0; idx <= budget.usedTracks; ++idx) {
            std::uint8_t slot = map[static_cast<std::size_t>(idx)];
            std::set<std::uint8_t> guard;
            while (slot != 0 && !guard.count(slot)) {
                guard.insert(slot);
                used.insert(slot);
                slot = cells[slot].link;
            }
        }
        return static_cast<int>(used.size());
    };
    budget.usedHalfWidthCells = countChainCells(state.halfTitleMap, state.halfTitleCells);
    budget.usedFullWidthCells = countChainCells(state.fullTitleMap, state.fullTitleCells);
    return budget;
}

std::string computeUtocSectorSha256Hex(const std::uint8_t* sector2352) {
    if (!sector2352) {
        return {};
    }
    return juce::SHA256(sector2352, kUtocSectorBytes).toHexString().toLowerCase().toStdString();
}

bool compileMdDiscToUtoc(const MdDisc& disc, UtocDiscState& out, std::string& error) {
    error.clear();
    if (isHiMdMedium(disc.medium) || isHiMdOnlyCodec(disc.defaultCodec)) {
        error = "Hi-MD discs use FAT16/TRKIDX instead of Standard MD UTOC sectors.";
        return false;
    }
    for (const auto& track : disc.tracks) {
        if (isHiMdOnlyCodec(effectiveTrackCodec(disc, track))) {
            error = "Hi-MD discs use FAT16/TRKIDX instead of Standard MD UTOC sectors.";
            return false;
        }
    }
    MdPhysicalPlan plan;
    if (!planMdPhysicalAudio(disc, plan, error)) {
        return false;
    }

    if (disc.tracks.size() > static_cast<std::size_t>(kUtocMaxTracks)) {
        error = "MiniDisc UTOC supports at most 255 tracks.";
        return false;
    }

    out = {};
    const bool hasImportedBase =
        std::all_of(disc.importedUtocSectors.begin(), disc.importedUtocSectors.end(),
                    [](const auto& s) { return s.size() == kUtocSectorBytes; });
    if (hasImportedBase) {
        UtocDiscState base{};
        std::string baseErr;
        if (!parseUtocSectors(disc.importedUtocSectors[0].data(),
                              disc.importedUtocSectors[1].data(),
                              disc.importedUtocSectors[2].data(),
                              disc.importedUtocSectors[3].data(), base, baseErr)) {
            error = baseErr;
            return false;
        }
        out.sec0Padding = base.sec0Padding;
        out.deviceSignature =
            base.deviceSignature != 0 ? base.deviceSignature : kCartaUtocSignature;
        out.sec0aPadding = base.sec0aPadding;
        out.sec1Padding = base.sec1Padding;
        out.sec2Padding = base.sec2Padding;
        out.sec4Padding = base.sec4Padding;
    } else {
        out.deviceSignature = kCartaUtocSignature;
    }
    if (disc.loadedDisc.active && disc.loadedDisc.deviceSignature != 0) {
        out.deviceSignature = disc.loadedDisc.deviceSignature;
    }
    out.hasSector0 = true;
    out.hasSector1 = true;
    out.hasSector2 = true;
    out.hasSector4 = false;
    out.discNonEmpty = disc.tracks.empty() ? 0 : 1;
    out.trackCount = static_cast<std::uint8_t>(disc.tracks.size());

    initFreeFragmentPool(out.fragments, out.nextFreeTrackSlot);
    initFreeCellPool(out.halfTitleCells, out.nextFreeHalfTitleSlot);
    initFreeCellPool(out.fullTitleCells, out.nextFreeFullTitleSlot);
    out.nextFreeTimestampSlot = 1;

    // 1. Build Sector 0 fragment chains per track from plan placements, merging identical tails
    // (DAG).
    struct FragKey {
        std::uint16_t sc;
        std::uint8_t ss;
        std::uint8_t sg;
        std::uint8_t mode;
        std::uint16_t ec;
        std::uint8_t es;
        std::uint8_t eg;
        std::uint8_t link;
        bool operator<(const FragKey& o) const noexcept {
            return std::tie(sc, ss, sg, mode, ec, es, eg, link) <
                   std::tie(o.sc, o.ss, o.sg, o.mode, o.ec, o.es, o.eg, o.link);
        }
    };
    std::map<FragKey, std::uint8_t> fragPool;

    auto allocFragmentSlot = [&](const UtocFragment& frag, std::uint8_t& outSlot) -> bool {
        const FragKey k{frag.start.cluster, frag.start.sector, frag.start.group, frag.mode,
                        frag.end.cluster,   frag.end.sector,   frag.end.group,   frag.link};
        if (const auto it = fragPool.find(k); it != fragPool.end()) {
            outSlot = it->second;
            return true;
        }
        if (out.nextFreeTrackSlot == 0) {
            error = "UTOC Sector 0 fragment table exceeded 255 slots.";
            return false;
        }
        const std::uint8_t slot = out.nextFreeTrackSlot;
        out.nextFreeTrackSlot = out.fragments[slot].link;
        out.fragments[slot] = frag;
        fragPool.emplace(k, slot);
        outSlot = slot;
        return true;
    };

    std::vector<std::vector<const MdPartPlacement*>> byTrack(disc.tracks.size());
    for (const auto& place : plan.placements) {
        if (place.trackIndex >= 0 && place.trackIndex < static_cast<int>(disc.tracks.size())) {
            byTrack[static_cast<std::size_t>(place.trackIndex)].push_back(&place);
        }
    }

    for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
        const auto& tr = disc.tracks[t];
        const auto codec = effectiveTrackCodec(disc, tr);
        const auto modeStr = mdCodecModeToString(codec);
        const std::uint8_t defaultModeByte =
            utocModeFromTrack(modeStr, tr.writeProtected, tr.scmsCopyProtected, tr.emphasis);
        std::uint8_t nextLink = 0;
        const auto& placements = byTrack[t];
        for (auto it = placements.rbegin(); it != placements.rend(); ++it) {
            const auto* p = *it;
            UtocFragment frag;
            frag.start = {p->startAddr.cluster, p->startAddr.sector, p->startAddr.group};
            frag.end = {p->endAddr.cluster, p->endAddr.sector, p->endAddr.group};
            frag.mode =
                p->isOnDisc
                    ? modeBytePreservingNonFlags(p->rawModeByte, codec, tr.writeProtected,
                                                 tr.scmsCopyProtected, tr.emphasis)
                    : defaultModeByte;
            frag.link = nextLink;
            std::uint8_t assigned = 0;
            if (!allocFragmentSlot(frag, assigned)) {
                return false;
            }
            nextLink = assigned;
        }
        out.trackMap[t + 1] = nextLink;
    }

    // Reserve P-DFA (0x2E) and free-area pointer P-FRA (trackMap[0]).
    if (disc.loadedDisc.active) {
        out.sec0aPadding[14] = 0;
        auto countRemainingSlots = [&]() {
            std::size_t count = 0;
            std::array<bool, kUtocSlots> seen{};
            for (std::uint8_t s = out.nextFreeTrackSlot; s != 0 && count < kUtocSlots;
                 s = out.fragments[s].link) {
                if (seen[s]) {
                    break;
                }
                seen[s] = true;
                ++count;
            }
            return count;
        };
        auto allocRangeChain = [&](const std::vector<MdPhysRange>& ranges, std::uint8_t& outHead) {
            outHead = 0;
            std::uint8_t prevSlot = 0;
            for (const auto& r : ranges) {
                if (out.nextFreeTrackSlot == 0) {
                    break;
                }
                const std::uint8_t slot = out.nextFreeTrackSlot;
                out.nextFreeTrackSlot = out.fragments[slot].link;
                out.fragments[slot].start = {r.start.cluster, r.start.sector, r.start.group};
                out.fragments[slot].mode = 0;
                out.fragments[slot].end = {r.end.cluster, r.end.sector, r.end.group};
                out.fragments[slot].link = 0;
                if (prevSlot == 0) {
                    outHead = slot;
                } else {
                    out.fragments[prevSlot].link = slot;
                }
                prevSlot = slot;
            }
        };

        if (!disc.loadedDisc.defectiveRanges.empty()) {
            if (countRemainingSlots() < disc.loadedDisc.defectiveRanges.size()) {
                error = "UTOC Sector 0 fragment table exceeded 255 slots (insufficient space for "
                        "defective area P-DFA).";
                return false;
            }
            allocRangeChain(disc.loadedDisc.defectiveRanges, out.sec0aPadding[14]);
        }

        if (!plan.reclaimedFreeRanges.empty()) {
            const std::size_t freeSlots = countRemainingSlots();
            if (freeSlots > 0) {
                std::vector<MdPhysRange> toLink = plan.reclaimedFreeRanges;
                if (toLink.size() > freeSlots) {
                    std::sort(toLink.begin(), toLink.end(),
                              [](const MdPhysRange& a, const MdPhysRange& b) {
                                  return (a.end.toLinearUnits() - a.start.toLinearUnits()) >
                                         (b.end.toLinearUnits() - b.start.toLinearUnits());
                              });
                    toLink.resize(freeSlots);
                    std::sort(toLink.begin(), toLink.end(),
                              [](const MdPhysRange& a, const MdPhysRange& b) {
                                  return a.start < b.start;
                              });
                }
                allocRangeChain(toLink, out.trackMap[0]);
            }
        }
    } else {
        const int capClusters = mdTotalClusters(disc.medium);
        const std::uint16_t nextFreeCluster =
            static_cast<std::uint16_t>(kMdFirstAudioCluster + plan.usedClusters);
        if (capClusters > 0 &&
            nextFreeCluster < static_cast<std::uint16_t>(capClusters + kMdFirstAudioCluster) &&
            out.nextFreeTrackSlot != 0) {
            const std::uint8_t slot = out.nextFreeTrackSlot;
            out.nextFreeTrackSlot = out.fragments[slot].link;
            out.fragments[slot].start = {nextFreeCluster, 0, 0};
            out.fragments[slot].mode = 0;
            out.fragments[slot].end = {
                static_cast<std::uint16_t>(capClusters + kMdFirstAudioCluster - 1), 31, 10};
            out.fragments[slot].link = 0;
            out.trackMap[0] = slot;
        }
    }

    // 2. Sector 1 (half-width) & Sector 4 (full-width) titles and groups.
    const std::string discHwSrc =
        !disc.halfWidthTitle.empty() ? disc.halfWidthTitle : disc.fullWidthTitle;
    const auto discHwPlainBytes = encodeHalfWidthBytes(discHwSrc);

    bool needFullWidth = !disc.fullWidthTitle.empty();
    for (const auto& t : disc.tracks) {
        if (!t.fullWidthTitle.empty()) {
            needFullWidth = true;
            break;
        }
    }
    if (!needFullWidth) {
        for (const auto& g : disc.groups) {
            if (!g.fullWidthTitle.empty()) {
                needFullWidth = true;
                break;
            }
        }
    }

    std::vector<std::uint8_t> discHwEncoded;
    std::string discFwHeader;
    if (!disc.groups.empty()) {
        std::string hw =
            "0;" + decodeHalfWidthBytes(discHwPlainBytes.data(), discHwPlainBytes.size()) + "//";
        const std::string discFwSrc =
            !disc.fullWidthTitle.empty() ? disc.fullWidthTitle : disc.halfWidthTitle;
        std::string fw = "\xEF\xBC\x90\xEF\xBC\x9B" + discFwSrc + "\xEF\xBC\x8F\xEF\xBC\x8F";
        for (const auto& g : disc.groups) {
            const auto rangeStr =
                (g.startTrack == g.endTrack)
                    ? std::to_string(g.startTrack)
                    : (std::to_string(g.startTrack) + "-" + std::to_string(g.endTrack));
            const std::string gHw = !g.halfWidthTitle.empty() ? g.halfWidthTitle : g.fullWidthTitle;
            const std::string gFw = !g.fullWidthTitle.empty() ? g.fullWidthTitle : g.halfWidthTitle;
            hw += rangeStr + ";" + gHw + "//";
            fw += fullWidthDigitsForRange(g.startTrack, g.endTrack) + "\xEF\xBC\x9B" + gFw +
                  "\xEF\xBC\x8F\xEF\xBC\x8F";
        }
        discHwEncoded = encodeHalfWidthBytes(hw);
        discFwHeader = fw;
    } else {
        discHwEncoded = discHwPlainBytes;
        discFwHeader = disc.fullWidthTitle;
    }

    if (!allocateTitleCells(out.halfTitleCells, out.nextFreeHalfTitleSlot, discHwEncoded,
                            out.halfTitleMap[0], error)) {
        error = "UTOC Sector 1 half-width title cells exceeded 255 slots.";
        return false;
    }
    if (needFullWidth && !discFwHeader.empty()) {
        out.hasSector4 = true;
        const auto fwBytes = encodeFullWidthShiftJis(discFwHeader, error);
        if (!error.empty()) {
            return false;
        }
        if (!allocateTitleCells(out.fullTitleCells, out.nextFreeFullTitleSlot, fwBytes,
                                out.fullTitleMap[0], error)) {
            error = "UTOC Sector 4 full-width title cells exceeded 255 slots.";
            return false;
        }
    }

    std::map<std::string, std::uint8_t> sharedHwTitleSlotByAudio;
    std::map<std::string, std::uint8_t> sharedFwTitleSlotByAudio;
    for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
        const auto& tr = disc.tracks[t];
        const bool shareByAudio = !tr.audioId.empty();

        if (shareByAudio) {
            if (const auto it = sharedHwTitleSlotByAudio.find(tr.audioId);
                it != sharedHwTitleSlotByAudio.end()) {
                out.halfTitleMap[t + 1] = it->second;
            } else {
                const std::string hwSrc =
                    !tr.halfWidthTitle.empty() ? tr.halfWidthTitle : tr.fullWidthTitle;
                const auto hwBytes = encodeHalfWidthBytes(hwSrc);
                if (!allocateTitleCells(out.halfTitleCells, out.nextFreeHalfTitleSlot, hwBytes,
                                        out.halfTitleMap[t + 1], error)) {
                    error = "UTOC Sector 1 half-width title cells exceeded 255 slots.";
                    return false;
                }
                sharedHwTitleSlotByAudio.emplace(tr.audioId, out.halfTitleMap[t + 1]);
            }
        } else {
            const std::string hwSrc =
                !tr.halfWidthTitle.empty() ? tr.halfWidthTitle : tr.fullWidthTitle;
            const auto hwBytes = encodeHalfWidthBytes(hwSrc);
            if (!allocateTitleCells(out.halfTitleCells, out.nextFreeHalfTitleSlot, hwBytes,
                                    out.halfTitleMap[t + 1], error)) {
                error = "UTOC Sector 1 half-width title cells exceeded 255 slots.";
                return false;
            }
        }

        if (needFullWidth && !tr.fullWidthTitle.empty()) {
            out.hasSector4 = true;
            if (shareByAudio) {
                if (const auto it = sharedFwTitleSlotByAudio.find(tr.audioId);
                    it != sharedFwTitleSlotByAudio.end()) {
                    out.fullTitleMap[t + 1] = it->second;
                } else {
                    const auto fwBytes = encodeFullWidthShiftJis(tr.fullWidthTitle, error);
                    if (!error.empty()) {
                        return false;
                    }
                    if (!allocateTitleCells(out.fullTitleCells, out.nextFreeFullTitleSlot, fwBytes,
                                            out.fullTitleMap[t + 1], error)) {
                        error = "UTOC Sector 4 full-width title cells exceeded 255 slots.";
                        return false;
                    }
                    sharedFwTitleSlotByAudio.emplace(tr.audioId, out.fullTitleMap[t + 1]);
                }
            } else {
                const auto fwBytes = encodeFullWidthShiftJis(tr.fullWidthTitle, error);
                if (!error.empty()) {
                    return false;
                }
                if (!allocateTitleCells(out.fullTitleCells, out.nextFreeFullTitleSlot, fwBytes,
                                        out.fullTitleMap[t + 1], error)) {
                    error = "UTOC Sector 4 full-width title cells exceeded 255 slots.";
                    return false;
                }
            }
        }
    }

    // 3. Sector 2 timestamps (disc header in map[0], tracks in map[1..N]).
    int nextTsSlot = 1;
    auto allocTimestampSlot = [&](const std::string& iso, std::uint8_t& outSlot) -> bool {
        outSlot = 0;
        if (iso.empty()) {
            return true;
        }
        UtocTimestamp ts{};
        if (!parseUtocTimestampString(iso, ts, error)) {
            return false;
        }
        if (ts.signature == 0) {
            ts.signature = out.deviceSignature;
        }
        for (int s = 1; s < nextTsSlot; ++s) {
            const auto& cur = out.timestamps[static_cast<std::size_t>(s)];
            if (cur.year == ts.year && cur.month == ts.month && cur.day == ts.day &&
                cur.hour == ts.hour && cur.minute == ts.minute && cur.second == ts.second &&
                cur.signature == ts.signature) {
                outSlot = static_cast<std::uint8_t>(s);
                return true;
            }
        }
        if (nextTsSlot > 255) {
            error = "UTOC Sector 2 timestamp table exceeded 255 slots.";
            return false;
        }
        const std::uint8_t slot = static_cast<std::uint8_t>(nextTsSlot++);
        out.timestamps[slot] = ts;
        outSlot = slot;
        return true;
    };
    if (!allocTimestampSlot(disc.discTimestamp, out.timestampMap[0])) {
        return false;
    }
    for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
        if (!allocTimestampSlot(disc.tracks[t].timestamp, out.timestampMap[t + 1])) {
            return false;
        }
    }
    out.nextFreeTimestampSlot =
        (nextTsSlot <= 255) ? static_cast<std::uint8_t>(nextTsSlot) : static_cast<std::uint8_t>(0);

    return true;
}

namespace {

void populateDiscHeaderAndGroups(const UtocDiscState& state, int nTracks, MdDisc& out) {
    const std::string rawDiscHw = readHalfWidthTitleSlot(state, 0);
    const std::string rawDiscFw = readFullWidthTitleSlot(state, 0);

    std::string discHw;
    std::string discFw;
    const auto groups = parseUtocGroups(rawDiscHw, rawDiscFw, nTracks, discHw, discFw);
    out.halfWidthTitle = discHw;
    out.fullWidthTitle = discFw;

    if (state.hasSector2 && state.timestampMap[0] != 0) {
        const auto& ts = state.timestamps[state.timestampMap[0]];
        if (ts.isValid()) {
            out.discTimestamp = formatUtocTimestampString(ts);
        }
    }

    for (const auto& g : groups) {
        MdGroup mg;
        mg.startTrack = g.startTrack;
        mg.endTrack = g.endTrack;
        mg.halfWidthTitle = g.halfWidthTitle;
        mg.fullWidthTitle = g.fullWidthTitle;
        out.groups.push_back(std::move(mg));
    }
}

void readTrackTitleAndTimestamp(const UtocDiscState& state, int oneBasedTrack, MdTrack& tr) {
    tr.id = makeMdTrackId();
    tr.halfWidthTitle = readHalfWidthTitleSlot(state, oneBasedTrack);
    tr.fullWidthTitle = readFullWidthTitleSlot(state, oneBasedTrack);
    if (state.hasSector2 &&
        state.timestampMap[static_cast<std::size_t>(oneBasedTrack)] != 0) {
        const auto& ts =
            state.timestamps[state.timestampMap[static_cast<std::size_t>(oneBasedTrack)]];
        if (ts.isValid()) {
            tr.timestamp = formatUtocTimestampString(ts);
        }
    }
}

void assignSharedAudioIdsFromIdenticalTracks(MdDisc& disc) {
    struct CanonicalKey {
        MdCodecMode codec = MdCodecMode::spStereo;
        bool writeProtected = false;
        bool scmsCopyProtected = false;
        bool emphasis = false;
        std::string hw;
        std::string fw;
        std::string artist;
        std::string album;
        std::string ts;
        std::vector<std::pair<std::int64_t, std::int64_t>> parts;

        bool operator<(const CanonicalKey& o) const noexcept {
            return std::tie(codec, writeProtected, scmsCopyProtected, emphasis, hw, fw, artist,
                            album, ts, parts) <
                   std::tie(o.codec, o.writeProtected, o.scmsCopyProtected, o.emphasis, o.hw, o.fw,
                            o.artist, o.album, o.ts, o.parts);
        }
    };

    std::map<CanonicalKey, std::vector<std::size_t>> grouped;
    for (std::size_t i = 0; i < disc.tracks.size(); ++i) {
        const auto& tr = disc.tracks[i];
        CanonicalKey key;
        key.codec = effectiveTrackCodec(disc, tr);
        key.writeProtected = tr.writeProtected;
        key.scmsCopyProtected = tr.scmsCopyProtected;
        key.emphasis = tr.emphasis;
        key.hw = tr.halfWidthTitle;
        key.fw = tr.fullWidthTitle;
        key.artist = tr.artist;
        key.album = tr.album;
        key.ts = tr.timestamp;
        key.parts.reserve(tr.parts.size());
        for (const auto& p : tr.parts) {
            key.parts.emplace_back(p.startSample, p.endSample);
        }
        grouped[key].push_back(i);
    }

    for (const auto& [key, indices] : grouped) {
        if (indices.size() <= 1) {
            continue;
        }
        const std::string aid = makeMdAudioId();
        for (const std::size_t idx : indices) {
            disc.tracks[idx].audioId = aid;
        }
    }
}

} // namespace

bool mdDiscFromUtoc(const UtocDiscState& state, MdDisc& out, std::string& error) {
    error.clear();
    out = {};
    if (!state.hasSector0) {
        error = "UTOC Sector 0 is required to reconstruct an MD disc.";
        return false;
    }
    out.medium = MdMedium::md80;
    out.defaultCodec = MdCodecMode::spStereo;

    const int nTracks = static_cast<int>(state.trackCount);
    populateDiscHeaderAndGroups(state, nTracks, out);

    // Assign each unique physical fragment range (start, end, codecMode) a deterministic
    // non-overlapping timeline slice so shared UTOC fragments remain shared in MdDisc.
    struct PhysKey {
        std::uint16_t sc;
        std::uint8_t ss;
        std::uint8_t sg;
        MdCodecMode codec;
        std::uint16_t ec;
        std::uint8_t es;
        std::uint8_t eg;
        bool operator<(const PhysKey& o) const noexcept {
            const int cA = static_cast<int>(codec);
            const int cB = static_cast<int>(o.codec);
            return std::tie(sc, ss, sg, ec, es, eg, cA) <
                   std::tie(o.sc, o.ss, o.sg, o.ec, o.es, o.eg, cB);
        }
    };
    std::map<PhysKey, MdPart> partByPhys;
    std::int64_t cursorSamples = 0;

    std::set<MdCodecMode> seenCodecs;
    std::vector<MdCodecMode> trackCodecs(static_cast<std::size_t>(nTracks), MdCodecMode::spStereo);

    for (int t = 1; t <= nTracks; ++t) {
        std::uint8_t slot = state.trackMap[static_cast<std::size_t>(t)];
        std::set<std::uint8_t> guard;
        while (slot != 0 && !guard.count(slot)) {
            guard.insert(slot);
            const auto& frag = state.fragments[slot];
            if (frag.isValid()) {
                const auto modeStr = audioModeFromUtocMode(frag.mode);
                MdCodecMode codec = MdCodecMode::spStereo;
                parseMdCodecMode(modeStr, codec);
                seenCodecs.insert(codec);
                const PhysKey k{frag.start.cluster, frag.start.sector, frag.start.group, codec,
                                frag.end.cluster,   frag.end.sector,   frag.end.group};
                if (!partByPhys.count(k)) {
                    const int units = std::max(1, static_cast<int>(frag.end.toLinearGroup() -
                                                                   frag.start.toLinearGroup() + 1));
                    const std::int64_t dur = samplesForUnits(units, modeStr);
                    partByPhys.emplace(k, MdPart{cursorSamples, cursorSamples + dur});
                    cursorSamples += dur;
                }
            }
            slot = frag.link;
        }
    }

    for (int t = 1; t <= nTracks; ++t) {
        MdTrack tr;
        readTrackTitleAndTimestamp(state, t, tr);

        std::uint8_t slot = state.trackMap[static_cast<std::size_t>(t)];
        std::set<std::uint8_t> guard;
        bool hasFragMode = false;
        std::uint8_t firstFragMode = 0;
        MdCodecMode trackCodec = MdCodecMode::spStereo;
        while (slot != 0 && !guard.count(slot)) {
            guard.insert(slot);
            const auto& frag = state.fragments[slot];
            if (frag.isValid()) {
                const auto modeStr = audioModeFromUtocMode(frag.mode);
                MdCodecMode codec = MdCodecMode::spStereo;
                parseMdCodecMode(modeStr, codec);
                if (!hasFragMode) {
                    firstFragMode = frag.mode;
                    trackCodec = codec;
                    hasFragMode = true;
                }
                const PhysKey k{frag.start.cluster, frag.start.sector, frag.start.group, codec,
                                frag.end.cluster,   frag.end.sector,   frag.end.group};
                if (const auto it = partByPhys.find(k); it != partByPhys.end()) {
                    tr.parts.push_back(it->second);
                }
            }
            slot = frag.link;
        }
        if (tr.parts.empty()) {
            error = "UTOC track " + std::to_string(t) + " has no valid audio fragments.";
            return false;
        }
        if (hasFragMode) {
            trackCodecs[static_cast<std::size_t>(t - 1)] = trackCodec;
            tr.writeProtected = (firstFragMode & kUtocWritable) == 0;
            tr.scmsCopyProtected = (firstFragMode & kUtocScmsDigCopy) != 0;
            tr.emphasis = (firstFragMode & kUtocEmphasis) != 0;
        }
        out.tracks.push_back(std::move(tr));
    }

    if (seenCodecs.size() == 1) {
        out.defaultCodec = *seenCodecs.begin();
    } else if (!trackCodecs.empty()) {
        out.defaultCodec = trackCodecs[0];
        for (std::size_t i = 0; i < out.tracks.size(); ++i) {
            if (trackCodecs[i] != out.defaultCodec) {
                out.tracks[i].codecOverride = trackCodecs[i];
            }
        }
    }

    std::array<std::uint8_t, kUtocSectorBytes> s0{};
    std::array<std::uint8_t, kUtocSectorBytes> s1{};
    std::array<std::uint8_t, kUtocSectorBytes> s2{};
    std::array<std::uint8_t, kUtocSectorBytes> s4{};
    std::string ignoredErr;
    if (compileUtocSectors(state, s0, s1, s2, s4, ignoredErr)) {
        out.importedUtocSectors[0].assign(s0.begin(), s0.end());
        out.importedUtocSectors[1].assign(s1.begin(), s1.end());
        out.importedUtocSectors[2].assign(s2.begin(), s2.end());
        out.importedUtocSectors[3].assign(s4.begin(), s4.end());
    }

    assignSharedAudioIdsFromIdenticalTracks(out);
    return validateMdDisc(out, error);
}

bool loadMdDiscFromDeviceUtoc(
    const std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5>& rawSectors,
    const std::string& sourceDeviceId, MdDisc& out, std::string& error) {
    error.clear();
    out = {};

    UtocDiscState state{};
    if (!parseUtocSectors(rawSectors[0].data(), rawSectors[1].data(), rawSectors[2].data(),
                          rawSectors[4].data(), state, error)) {
        return false;
    }
    if (!state.hasSector0) {
        error = "UTOC Sector 0 is required to load a MiniDisc.";
        return false;
    }
    if (utocHasNonAudioTrack(state)) {
        error = "Inserted disc contains Hi-MD / non-audio data tracks; erase to reformat for "
                "standard MiniDisc.";
        return false;
    }

    const int nTracks = static_cast<int>(state.trackCount);
    populateDiscHeaderAndGroups(state, nTracks, out);

    out.loadedDisc.active = true;
    out.loadedDisc.sourceDeviceId = sourceDeviceId;
    out.loadedDisc.baselineSector0Hash = computeUtocSectorSha256Hex(rawSectors[0].data());
    out.loadedDisc.deviceSignature = state.deviceSignature;
    out.loadedDisc.initialFreeRanges = readPhysRangeChain(state, state.trackMap[0]);
    out.loadedDisc.defectiveRanges = readPhysRangeChain(state, state.sec0aPadding[14]);
    out.loadedDisc.rawSector3.assign(rawSectors[3].begin(), rawSectors[3].end());
    out.importedUtocSectors[0].assign(rawSectors[0].begin(), rawSectors[0].end());
    out.importedUtocSectors[1].assign(rawSectors[1].begin(), rawSectors[1].end());
    out.importedUtocSectors[2].assign(rawSectors[2].begin(), rawSectors[2].end());
    out.importedUtocSectors[3].assign(rawSectors[4].begin(), rawSectors[4].end());

    struct RawFragInterval {
        std::int64_t startLin = 0;
        std::int64_t endLin = 0;
        MdCodecMode codec = MdCodecMode::spStereo;
        std::uint8_t rawModeByte = 0;
    };
    std::vector<RawFragInterval> intervals;

    std::uint16_t minCluster = 0;
    std::uint16_t maxCluster = 0;
    auto noteClusterSpan = [&](std::uint16_t sCluster, std::uint16_t eCluster) {
        if (sCluster > 0) {
            minCluster = (minCluster == 0) ? sCluster : std::min(minCluster, sCluster);
        }
        maxCluster = std::max(maxCluster, eCluster);
    };
    for (const auto& r : out.loadedDisc.initialFreeRanges) {
        noteClusterSpan(r.start.cluster, r.end.cluster);
    }
    for (const auto& r : out.loadedDisc.defectiveRanges) {
        noteClusterSpan(r.start.cluster, r.end.cluster);
    }

    for (int t = 1; t <= nTracks; ++t) {
        std::uint8_t slot = state.trackMap[static_cast<std::size_t>(t)];
        std::set<std::uint8_t> guard;
        int fragCount = 0;
        while (slot != 0 && guard.insert(slot).second) {
            const auto& frag = state.fragments[slot];
            if (frag.isValid()) {
                const std::int64_t sLin = frag.start.toLinearGroup();
                const std::int64_t eLin = frag.end.toLinearGroup();
                if (eLin < sLin) {
                    error = "UTOC track " + std::to_string(t) + " has inverted fragment range.";
                    out = {};
                    return false;
                }
                const auto modeStr = audioModeFromUtocMode(frag.mode);
                MdCodecMode codec = MdCodecMode::spStereo;
                parseMdCodecMode(modeStr, codec);
                intervals.push_back({sLin, eLin, codec, frag.mode});
                noteClusterSpan(frag.start.cluster, frag.end.cluster);
                ++fragCount;
            }
            slot = frag.link;
        }
        if (fragCount == 0) {
            error = "UTOC track " + std::to_string(t) + " has no valid audio fragments.";
            out = {};
            return false;
        }
    }

    if (maxCluster == 0) {
        out.loadedDisc.physicalStartCluster = 50;
        out.loadedDisc.physicalLastCluster =
            static_cast<std::uint16_t>(50 + mdTotalClusters(MdMedium::md80) - 1);
    } else {
        out.loadedDisc.physicalStartCluster = std::max<std::uint16_t>(1, minCluster);
        out.loadedDisc.physicalLastCluster =
            std::max(out.loadedDisc.physicalStartCluster, maxCluster);
    }
    const int physTotal = out.loadedDisc.physicalTotalClusters();
    out.medium = (physTotal <= 1900)
                     ? MdMedium::md60
                     : ((physTotal <= 2260) ? MdMedium::md74 : MdMedium::md80);

    auto nonFlagModeBits = [](std::uint8_t m) noexcept -> std::uint8_t {
        return static_cast<std::uint8_t>(
            m & ~static_cast<std::uint8_t>(kUtocWritable | kUtocScmsDigCopy | kUtocEmphasis));
    };

    std::sort(intervals.begin(), intervals.end(),
              [](const RawFragInterval& a, const RawFragInterval& b) {
                  return std::tie(a.startLin, a.endLin) < std::tie(b.startLin, b.endLin);
              });

    std::vector<RawFragInterval> coalesced;
    for (const auto& iv : intervals) {
        if (coalesced.empty()) {
            coalesced.push_back(iv);
            continue;
        }
        auto& cur = coalesced.back();
        const bool sameFormat =
            (iv.codec == cur.codec && nonFlagModeBits(iv.rawModeByte) == nonFlagModeBits(cur.rawModeByte));
        if (iv.startLin <= cur.endLin && !sameFormat) {
            error = "UTOC contains overlapping physical fragments with conflicting audio codecs.";
            out = {};
            return false;
        }
        if (iv.startLin <= cur.endLin + 1 && sameFormat) {
            cur.endLin = std::max(cur.endLin, iv.endLin);
        } else {
            coalesced.push_back(iv);
        }
    }

    std::int64_t timelineCursor = 0;
    out.loadedDisc.regions.reserve(coalesced.size());
    for (std::size_t i = 0; i < coalesced.size(); ++i) {
        const auto& c = coalesced[i];
        MdOnDiscRegion reg;
        reg.id = "od-" + std::to_string(i);
        reg.codec = c.codec;
        reg.rawModeByte = c.rawModeByte;
        const auto ps = UtocAddress::fromLinearGroup(c.startLin);
        const auto pe = UtocAddress::fromLinearGroup(c.endLin);
        reg.physStart = {ps.cluster, ps.sector, ps.group};
        reg.physEnd = {pe.cluster, pe.sector, pe.group};
        reg.unitCount = static_cast<int>(c.endLin - c.startLin + 1);
        reg.timelineStartSample = timelineCursor;
        reg.timelineEndSample =
            timelineCursor + static_cast<std::int64_t>(reg.unitCount) * mdSamplesPerUnit(c.codec);
        timelineCursor = reg.timelineEndSample;
        out.loadedDisc.regions.push_back(std::move(reg));
    }

    std::map<MdCodecMode, int> codecCounts;
    for (int t = 1; t <= nTracks; ++t) {
        MdTrack tr;
        readTrackTitleAndTimestamp(state, t, tr);

        std::uint8_t slot = state.trackMap[static_cast<std::size_t>(t)];
        std::set<std::uint8_t> guard;
        bool hasFragMode = false;
        std::uint8_t firstFragMode = 0;
        MdCodecMode trackCodec = MdCodecMode::spStereo;

        while (slot != 0 && guard.insert(slot).second) {
            const auto& frag = state.fragments[slot];
            if (frag.isValid()) {
                const auto modeStr = audioModeFromUtocMode(frag.mode);
                MdCodecMode codec = MdCodecMode::spStereo;
                parseMdCodecMode(modeStr, codec);
                if (!hasFragMode) {
                    firstFragMode = frag.mode;
                    trackCodec = codec;
                    hasFragMode = true;
                } else if (codec != trackCodec) {
                    error = "UTOC track " + std::to_string(t) +
                            " mixes incompatible audio codecs across fragments.";
                    out = {};
                    return false;
                }

                const std::int64_t sLin = frag.start.toLinearGroup();
                const std::int64_t eLin = frag.end.toLinearGroup();
                bool mapped = false;
                for (const auto& reg : out.loadedDisc.regions) {
                    const std::int64_t rStart = reg.physStart.toLinearUnits();
                    const std::int64_t rEnd = reg.physEnd.toLinearUnits();
                    if (sLin >= rStart && eLin <= rEnd && reg.codec == codec) {
                        const std::int64_t uSamples = mdSamplesPerUnit(reg.codec);
                        tr.parts.push_back(
                            {reg.timelineStartSample + (sLin - rStart) * uSamples,
                             reg.timelineStartSample + (eLin - rStart + 1) * uSamples});
                        mapped = true;
                        break;
                    }
                }
                if (!mapped) {
                    error = "Internal error mapping UTOC track " + std::to_string(t) +
                            " fragment to loaded region.";
                    out = {};
                    return false;
                }
            }
            slot = frag.link;
        }

        if (hasFragMode) {
            tr.codecOverride = trackCodec;
            tr.writeProtected = (firstFragMode & kUtocWritable) == 0;
            tr.scmsCopyProtected = (firstFragMode & kUtocScmsDigCopy) != 0;
            tr.emphasis = (firstFragMode & kUtocEmphasis) != 0;
            ++codecCounts[trackCodec];
        }
        out.tracks.push_back(std::move(tr));
    }

    if (!codecCounts.empty()) {
        MdCodecMode bestCodec = MdCodecMode::spStereo;
        int bestCount = -1;
        for (const auto& [c, cnt] : codecCounts) {
            if (cnt > bestCount) {
                bestCount = cnt;
                bestCodec = c;
            }
        }
        out.defaultCodec = bestCodec;
    }

    assignSharedAudioIdsFromIdenticalTracks(out);
    if (!validateMdDisc(out, error)) {
        out = {};
        return false;
    }
    return true;
}

bool utocHasNonAudioTrack(const UtocDiscState& state) noexcept {
    if (!state.hasSector0) {
        return false;
    }
    const int n = std::min<int>(state.trackCount, kUtocMaxTracks);
    for (int t = 1; t <= n; ++t) {
        std::uint8_t slot = state.trackMap[static_cast<std::size_t>(t)];
        std::set<std::uint8_t> guard;
        while (slot != 0 && !guard.count(slot)) {
            guard.insert(slot);
            const auto& f = state.fragments[slot];
            if (f.isValid() && (f.mode & kUtocNonAudio) != 0) {
                return true;
            }
            slot = f.link;
        }
    }
    return false;
}

std::string formatUtocInspectionReport(const UtocDiscState& state, const std::string& medium) {
    const auto b = computeUtocBudget(state, medium);
    std::ostringstream ss;
    ss << "UTOC Signature: 0x" << std::hex << state.deviceSignature << std::dec
       << "  Tracks: " << b.usedTracks << "/" << b.maxTracks << "  Fragments: " << b.usedFragments
       << "/" << b.maxFragments << "  Shared Ranges: " << b.sharedFragmentCount
       << "  Clusters: " << b.usedClusters
       << (b.totalClusters > 0 ? "/" + std::to_string(b.totalClusters) : "")
       << "  HW Cells: " << b.usedHalfWidthCells << "/" << b.maxHalfWidthCells
       << "  FW Cells: " << b.usedFullWidthCells << "/" << b.maxFullWidthCells;
    if (state.hasSector2 && state.timestampMap[0] != 0) {
        const auto discTs = formatUtocTimestampString(state.timestamps[state.timestampMap[0]]);
        if (!discTs.empty()) {
            ss << "  Disc TS: @" << discTs;
        }
    }
    ss << "\n";

    for (int t = 1; t <= b.usedTracks; ++t) {
        const auto hw = readHalfWidthTitleSlot(state, t);
        const auto fw = readFullWidthTitleSlot(state, t);
        ss << "  #" << t << " [" << (hw.empty() ? "(untitled)" : hw) << "]";
        if (!fw.empty()) {
            ss << " / FW[" << fw << "]";
        }
        if (state.hasSector2 && state.timestampMap[static_cast<std::size_t>(t)] != 0) {
            const auto ts = formatUtocTimestampString(
                state.timestamps[state.timestampMap[static_cast<std::size_t>(t)]]);
            if (!ts.empty()) {
                ss << "  @" << ts;
            }
        }
        ss << "\n";

        std::uint8_t slot = state.trackMap[static_cast<std::size_t>(t)];
        std::set<std::uint8_t> guard;
        while (slot != 0 && !guard.count(slot)) {
            guard.insert(slot);
            const auto& f = state.fragments[slot];
            const std::string modeStr =
                (f.mode & kUtocNonAudio) != 0 ? "data/himd" : audioModeFromUtocMode(f.mode);
            ss << "    slot " << static_cast<int>(slot) << ": " << f.start.cluster << "."
               << static_cast<int>(f.start.sector) << "." << static_cast<int>(f.start.group)
               << " -> " << f.end.cluster << "." << static_cast<int>(f.end.sector) << "."
               << static_cast<int>(f.end.group) << " [" << modeStr
               << ((f.mode & kUtocWritable) ? "" : " RO")
               << ((f.mode & kUtocScmsDigCopy) ? " SCMS" : "")
               << ((f.mode & kUtocEmphasis) ? " EMPH" : "") << "] next=" << static_cast<int>(f.link)
               << "\n";
            slot = f.link;
        }
    }
    return ss.str();
}

} // namespace carta
