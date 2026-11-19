/**
 * @file PorfforCodecTests.cpp
 * @brief Parity, handle-isolation, and stream-stability tests for Porffor ATRAC codecs.
 *
 * Compares native `libcarta{1,3,5}_porffor.dylib` container and frame output
 * bit-for-bit against Node.js reference implementations in `third_party/`,
 * checks concurrent stream handle isolation, and exercises 16-second streams.
 */
#include "MiniDisc/NativeCodecs.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <juce_core/juce_core.h>
#include <string>
#include <vector>

#ifndef CARTA_STUDIO_SOURCE_DIR
#define CARTA_STUDIO_SOURCE_DIR "/Users/pablo/Projects/carta/carta-studio"
#endif

namespace {

std::string findNodeBinary() {
    if (const char* env = std::getenv("NODE_BIN")) {
        if (juce::File(juce::String::fromUTF8(env)).existsAsFile())
            return std::string(env);
    }
    const char* candidates[] = {
        "/opt/local/bin/node",
        "/opt/homebrew/bin/node",
        "/usr/local/bin/node",
        "/usr/bin/node",
    };
    for (const char* path : candidates) {
        if (juce::File(juce::String::fromUTF8(path)).existsAsFile())
            return std::string(path);
    }
    return "node";
}

void generateDeterministicStereoSignal(int sampleCount, double baseFreqL, double baseFreqR,
                                       std::vector<float>& left, std::vector<float>& right) {
    left.resize(static_cast<std::size_t>(sampleCount));
    right.resize(static_cast<std::size_t>(sampleCount));
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kRate = 44100.0;
    for (int i = 0; i < sampleCount; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const double env = 0.25 + 0.45 * std::sin(2.0 * kPi * 0.45 * t);
        const double chirpL = std::sin(2.0 * kPi * (baseFreqL + 180.0 * t) * t);
        const double chordL = 0.55 * chirpL + 0.25 * std::sin(2.0 * kPi * (baseFreqL * 2.5) * t) +
                              0.15 * std::cos(2.0 * kPi * 110.0 * t);
        const double chirpR = std::sin(2.0 * kPi * (baseFreqR + 240.0 * t) * t + 0.4);
        const double chordR = 0.55 * chirpR + 0.22 * std::sin(2.0 * kPi * (baseFreqR * 1.75) * t) +
                              0.18 * std::sin(2.0 * kPi * 146.83 * t);
        double impL = 0.0;
        double impR = 0.0;
        if (i == 4410 || i == 44100 || i == 98000)
            impL = 0.22;
        if (i == 8820 || i == 55125 || i == 110250)
            impR = -0.22;
        left[static_cast<std::size_t>(i)] = static_cast<float>(env * chordL + impL);
        right[static_cast<std::size_t>(i)] = static_cast<float>((0.9 * env) * chordR + impR);
    }
}

bool writeFloatFile(const juce::File& file, const std::vector<float>& data) {
    return file.replaceWithData(data.data(), data.size() * sizeof(float));
}

bool readBinaryFile(const juce::File& file, std::vector<std::uint8_t>& out) {
    juce::MemoryBlock mb;
    if (!file.loadFileAsData(mb))
        return false;
    out.resize(mb.getSize());
    if (!out.empty())
        std::memcpy(out.data(), mb.getData(), mb.getSize());
    return true;
}

bool readFloatFile(const juce::File& file, std::vector<float>& out) {
    juce::MemoryBlock mb;
    if (!file.loadFileAsData(mb))
        return false;
    if ((mb.getSize() % sizeof(float)) != 0)
        return false;
    out.resize(mb.getSize() / sizeof(float));
    if (!out.empty())
        std::memcpy(out.data(), mb.getData(), mb.getSize());
    return true;
}

double computeRmsDifference(const std::vector<float>& a, const std::vector<float>& b) {
    const std::size_t n = std::min(a.size(), b.size());
    if (n == 0)
        return 0.0;
    double sumSq = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        sumSq += d * d;
    }
    return std::sqrt(sumSq / static_cast<double>(n));
}

/** Verifies bit-exact encoded bytes and decoded float PCM parity vs Node.js
 * `third_party/carta{1,3,5}`. */
bool testNodeParity(const juce::File& tempDir) {
    constexpr int kSamples = 154350; // 3.5s @ 44.1 kHz
    std::vector<float> left;
    std::vector<float> right;
    generateDeterministicStereoSignal(kSamples, 220.0, 329.63, left, right);

    const juce::File leftBin = tempDir.getChildFile("in_left.f32");
    const juce::File rightBin = tempDir.getChildFile("in_right.f32");
    if (!writeFloatFile(leftBin, left) || !writeFloatFile(rightBin, right)) {
        std::cerr << "FAIL: Could not write raw input float files.\n";
        return false;
    }

    const juce::File nodeScript = tempDir.getChildFile("run_node_codecs.mjs");
    const juce::String srcRoot = juce::String::fromUTF8(CARTA_STUDIO_SOURCE_DIR);
    const juce::String script = R"JS(
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const [workDir, thirdPartyDir] = process.argv.slice(2);
const readF32 = (name) => {
  const buf = fs.readFileSync(path.join(workDir, name));
  const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
  return new Float32Array(ab);
};
const writeU8 = (name, u8) => fs.writeFileSync(path.join(workDir, name), Buffer.from(u8.buffer, u8.byteOffset, u8.byteLength));
const writeF32 = (name, f32) => fs.writeFileSync(path.join(workDir, name), Buffer.from(f32.buffer, f32.byteOffset, f32.byteLength));

const left = readF32('in_left.f32');
const right = readF32('in_right.f32');

const c1 = await import(pathToFileURL(path.join(thirdPartyDir, 'carta1/codec/index.js')).href);
const c3 = await import(pathToFileURL(path.join(thirdPartyDir, 'carta3/codec/index.js')).href);
const c5 = await import(pathToFileURL(path.join(thirdPartyDir, 'carta5/codec/index.js')).href);

const spStereoEnc = await c1.encodeAeaPcm([left, right]);
writeU8('sp-stereo.bin', spStereoEnc);
const spStereoDec = await c1.decodeAeaPcm(spStereoEnc);
writeF32('sp-stereo_dec_l.f32', spStereoDec[0]);
writeF32('sp-stereo_dec_r.f32', spStereoDec[1]);

const spMonoEnc = await c1.encodeAeaPcm([left]);
writeU8('sp-mono.bin', spMonoEnc);
const spMonoDec = await c1.decodeAeaPcm(spMonoEnc);
writeF32('sp-mono_dec_l.f32', spMonoDec[0]);

const lp2Enc = c3.encodeWavePcm([left, right], { bitrateKbps: 132 });
writeU8('lp2.bin', lp2Enc);
const lp2Dec = c3.decodeWavePcm(lp2Enc);
writeF32('lp2_dec_l.f32', lp2Dec[0]);
writeF32('lp2_dec_r.f32', lp2Dec[1]);

const lp4Enc = c3.encodeWavePcm([left, right], { bitrateKbps: 66 });
writeU8('lp4.bin', lp4Enc);
const lp4Dec = c3.decodeWavePcm(lp4Enc);
writeF32('lp4_dec_l.f32', lp4Dec[0]);
writeF32('lp4_dec_r.f32', lp4Dec[1]);

const hispEnc = c5.encodeWavePcm([left, right], { bitrateKbps: 256, channels: 2, sampleRate: 44100 });
writeU8('hi-sp.bin', hispEnc);
const hispDecRaw = c5.decodeWavePcm(hispEnc);
const hispDec = Array.isArray(hispDecRaw) ? hispDecRaw : hispDecRaw.channels;
writeF32('hi-sp_dec_l.f32', hispDec[0]);
writeF32('hi-sp_dec_r.f32', hispDec[1]);

const hilpEnc = c5.encodeWavePcm([left, right], { bitrateKbps: 64, channels: 2, sampleRate: 44100 });
writeU8('hi-lp.bin', hilpEnc);
const hilpDecRaw = c5.decodeWavePcm(hilpEnc);
const hilpDec = Array.isArray(hilpDecRaw) ? hilpDecRaw : hilpDecRaw.channels;
writeF32('hi-lp_dec_l.f32', hilpDec[0]);
writeF32('hi-lp_dec_r.f32', hilpDec[1]);
)JS";
    if (!nodeScript.replaceWithText(script)) {
        std::cerr << "FAIL: Could not write Node parity runner.\n";
        return false;
    }

    juce::ChildProcess proc;
    const juce::StringArray cmd{
        juce::String::fromUTF8(findNodeBinary().c_str()),
        nodeScript.getFullPathName(),
        tempDir.getFullPathName(),
        srcRoot + "/third_party",
    };
    if (!proc.start(cmd)) {
        std::cerr << "FAIL: Failed to start Node process for codec reference.\n";
        return false;
    }
    const juce::String nodeOutput = proc.readAllProcessOutput();
    if (!proc.waitForProcessToFinish(120000) || proc.getExitCode() != 0) {
        std::cerr << "FAIL: Node reference codec execution failed (exit " << proc.getExitCode()
                  << "):\n"
                  << nodeOutput.toStdString() << "\n";
        return false;
    }

    struct ModeCase {
        const char* id;
        int inputChannels;
        int expectedOutChannels;
    };
    const ModeCase cases[] = {
        {"sp-stereo", 2, 2}, {"sp-mono", 1, 1}, {"lp2", 2, 2},
        {"lp4", 2, 2},       {"hi-sp", 2, 2},   {"hi-lp", 2, 2},
    };

    for (const auto& mc : cases) {
        std::vector<std::uint8_t> nodeContainer;
        std::vector<float> nodeDecL, nodeDecR;
        if (!readBinaryFile(tempDir.getChildFile(juce::String(mc.id) + ".bin"), nodeContainer) ||
            !readFloatFile(tempDir.getChildFile(juce::String(mc.id) + "_dec_l.f32"), nodeDecL)) {
            std::cerr << "FAIL: Missing Node output files for " << mc.id << "\n";
            return false;
        }
        if (mc.expectedOutChannels == 2 &&
            !readFloatFile(tempDir.getChildFile(juce::String(mc.id) + "_dec_r.f32"), nodeDecR)) {
            std::cerr << "FAIL: Missing Node right channel output for " << mc.id << "\n";
            return false;
        }

        std::vector<std::uint8_t> nativeContainer;
        std::string err;
        if (!carta::encodeAtracContainer(mc.id, left.data(),
                                         mc.inputChannels == 2 ? right.data() : nullptr,
                                         mc.inputChannels, kSamples, nativeContainer, err)) {
            std::cerr << "FAIL: encodeAtracContainer(" << mc.id << "): " << err << "\n";
            return false;
        }

        if (nativeContainer.size() != nodeContainer.size()) {
            std::cerr << "FAIL: Encoded container size mismatch vs Node for " << mc.id
                      << " (native=" << nativeContainer.size() << ", node=" << nodeContainer.size()
                      << ")\n";
            return false;
        }
        const bool containerBitExact =
            (std::memcmp(nativeContainer.data(), nodeContainer.data(), nodeContainer.size()) == 0);
#if !defined(_WIN32)
        if (!containerBitExact) {
            std::cerr << "FAIL: Encoded container mismatch vs Node for " << mc.id
                      << " (native=" << nativeContainer.size() << ", node=" << nodeContainer.size()
                      << ")\n";
            return false;
        }
#endif

        if (mc.expectedOutChannels == 2 || std::string(mc.id) == "sp-mono") {
            if (std::string(mc.id) == "sp-stereo" || std::string(mc.id) == "sp-mono" ||
                std::string(mc.id) == "lp2" || std::string(mc.id) == "lp4") {
                std::vector<std::uint8_t> wireBytes;
                if (!carta::extractNetMdWirePayload(mc.id, nativeContainer, wireBytes, err) ||
                    wireBytes.empty()) {
                    std::cerr << "FAIL: extractNetMdWirePayload(" << mc.id << "): " << err << "\n";
                    return false;
                }
            }
        }

        std::vector<float> nativeDecL, nativeDecR;
        int outCh = 0;
        if (!carta::decodeAtracContainer(mc.id, nativeContainer.data(), nativeContainer.size(),
                                         nativeDecL, nativeDecR, outCh, err)) {
            std::cerr << "FAIL: decodeAtracContainer(" << mc.id << "): " << err << "\n";
            return false;
        }

        if (outCh != mc.expectedOutChannels || nativeDecL.size() != nodeDecL.size()) {
            std::cerr << "FAIL: Decoded geometry mismatch for " << mc.id << "\n";
            return false;
        }
#if defined(_WIN32)
        const double nodeDiffL = computeRmsDifference(nativeDecL, nodeDecL);
        if (!(nodeDiffL < 1e-3)) {
            std::cerr << "FAIL: Decoded left channel diverged from Node for " << mc.id
                      << " (rms=" << nodeDiffL << ")\n";
            return false;
        }
        if (outCh == 2) {
            const double nodeDiffR = computeRmsDifference(nativeDecR, nodeDecR);
            if (nativeDecR.size() != nodeDecR.size() || !(nodeDiffR < 1e-3)) {
                std::cerr << "FAIL: Decoded right channel diverged from Node for " << mc.id
                          << " (rms=" << nodeDiffR << ")\n";
                return false;
            }
        }
#else
        if (std::memcmp(nativeDecL.data(), nodeDecL.data(), nativeDecL.size() * sizeof(float)) !=
            0) {
            std::cerr << "FAIL: Decoded left channel bit-pattern mismatch vs Node for " << mc.id
                      << "\n";
            return false;
        }
        if (outCh == 2) {
            if (nativeDecR.size() != nodeDecR.size() ||
                std::memcmp(nativeDecR.data(), nodeDecR.data(),
                            nativeDecR.size() * sizeof(float)) != 0) {
                std::cerr << "FAIL: Decoded right channel bit-pattern mismatch vs Node for "
                          << mc.id << "\n";
                return false;
            }
        }
#endif

        const double rmsL = computeRmsDifference(left, nativeDecL);
        if (!(rmsL > 0.0 && rmsL < 0.25)) {
            std::cerr << "FAIL: Unexpected decode RMS error for " << mc.id << ": " << rmsL << "\n";
            return false;
        }
        std::cout << "PASS parity [" << mc.id << "] bytes=" << nativeContainer.size()
                  << " samples=" << nativeDecL.size()
                  << " bit-exact=" << (containerBitExact ? "YES" : "UCRT-RMS-OK") << "\n";
    }
    return true;
}

/** Verifies concurrent stream encoder/decoder handles remain isolated across interleaved calls. */
bool testStreamHandleIsolation(const char* mode) {
    const auto* spec = carta::findAtracModeSpec(mode);
    if (!spec) {
        std::cerr << "FAIL: Unknown mode in stream test: " << mode << "\n";
        return false;
    }
    constexpr int kTotalSamples = 86016; // Exact multiple of 512, 1024, and 2048 (~1.95s)
    const int frameSamples = spec->samplesPerFrame;
    const int numFrames = kTotalSamples / frameSamples;

    std::vector<float> sigAL, sigAR, sigBL, sigBR;
    generateDeterministicStereoSignal(kTotalSamples, 196.0, 293.66, sigAL, sigAR);
    generateDeterministicStereoSignal(kTotalSamples, 523.25, 659.25, sigBL, sigBR);

    auto encodeIsolated = [&](const std::vector<float>& l, const std::vector<float>& r,
                              std::vector<std::uint8_t>& outEncoded, std::vector<float>& outDecL,
                              std::vector<float>& outDecR) -> bool {
        carta::NativeAtracStreamEncoder enc;
        carta::NativeAtracStreamDecoder dec;
        std::string err;
        if (!enc.open(mode, err) || !dec.open(mode, err)) {
            std::cerr << "FAIL: Open stream (" << mode << "): " << err << "\n";
            return false;
        }
        outEncoded.clear();
        outDecL.clear();
        outDecR.clear();
        std::vector<std::uint8_t> pktBuf(static_cast<std::size_t>(spec->bytesPerFrame));
        std::vector<float> decBufL(static_cast<std::size_t>(frameSamples));
        std::vector<float> decBufR(static_cast<std::size_t>(frameSamples));
        for (int f = 0; f < numFrames; ++f) {
            const std::size_t pcmOff = static_cast<std::size_t>(f * frameSamples);
            const int encBytes = enc.encodeFrame(l.data() + pcmOff,
                                                 spec->channels == 2 ? r.data() + pcmOff : nullptr,
                                                 pktBuf.data(), pktBuf.size(), err);
            if (encBytes < 0 || (encBytes != 0 && encBytes != spec->bytesPerFrame)) {
                std::cerr << "FAIL: Isolated encodeFrame(" << mode << "): " << err << "\n";
                return false;
            }
            if (encBytes == 0) {
                continue;
            }
            outEncoded.insert(outEncoded.end(), pktBuf.begin(), pktBuf.end());
            if (dec.decodeFrame(pktBuf.data(), pktBuf.size(), decBufL.data(),
                                spec->channels == 2 ? decBufR.data() : nullptr,
                                err) != frameSamples) {
                std::cerr << "FAIL: Isolated decodeFrame(" << mode << "): " << err << "\n";
                return false;
            }
            outDecL.insert(outDecL.end(), decBufL.begin(), decBufL.end());
            if (spec->channels == 2) {
                outDecR.insert(outDecR.end(), decBufR.begin(), decBufR.end());
            }
        }
        return !outEncoded.empty();
    };

    std::vector<std::uint8_t> isoEncA, isoEncB;
    std::vector<float> isoDecAL, isoDecAR, isoDecBL, isoDecBR;
    if (!encodeIsolated(sigAL, sigAR, isoEncA, isoDecAL, isoDecAR) ||
        !encodeIsolated(sigBL, sigBR, isoEncB, isoDecBL, isoDecBR)) {
        return false;
    }

    carta::NativeAtracStreamEncoder encA, encB;
    carta::NativeAtracStreamDecoder decA, decB;
    std::string err;
    if (!encA.open(mode, err) || !encB.open(mode, err) || !decA.open(mode, err) ||
        !decB.open(mode, err)) {
        std::cerr << "FAIL: Open interleaved streams (" << mode << "): " << err << "\n";
        return false;
    }

    std::vector<std::uint8_t> intEncA, intEncB;
    std::vector<float> intDecAL, intDecAR, intDecBL, intDecBR;
    std::vector<std::uint8_t> pktBufA(static_cast<std::size_t>(spec->bytesPerFrame));
    std::vector<std::uint8_t> pktBufB(static_cast<std::size_t>(spec->bytesPerFrame));
    std::vector<float> decBufAL(static_cast<std::size_t>(frameSamples));
    std::vector<float> decBufAR(static_cast<std::size_t>(frameSamples));
    std::vector<float> decBufBL(static_cast<std::size_t>(frameSamples));
    std::vector<float> decBufBR(static_cast<std::size_t>(frameSamples));

    for (int f = 0; f < numFrames; ++f) {
        const std::size_t pcmOff = static_cast<std::size_t>(f * frameSamples);
        const int encBytesA = encA.encodeFrame(
            sigAL.data() + pcmOff, spec->channels == 2 ? sigAR.data() + pcmOff : nullptr,
            pktBufA.data(), pktBufA.size(), err);
        if (encBytesA < 0 || (encBytesA != 0 && encBytesA != spec->bytesPerFrame)) {
            std::cerr << "FAIL: Interleaved encodeFrame A(" << mode << "): " << err << "\n";
            return false;
        }
        const int encBytesB = encB.encodeFrame(
            sigBL.data() + pcmOff, spec->channels == 2 ? sigBR.data() + pcmOff : nullptr,
            pktBufB.data(), pktBufB.size(), err);
        if (encBytesB < 0 || (encBytesB != 0 && encBytesB != spec->bytesPerFrame) ||
            encBytesA != encBytesB) {
            std::cerr << "FAIL: Interleaved encodeFrame B(" << mode << "): " << err << "\n";
            return false;
        }
        if (encBytesA == 0) {
            continue;
        }
        intEncA.insert(intEncA.end(), pktBufA.begin(), pktBufA.end());
        intEncB.insert(intEncB.end(), pktBufB.begin(), pktBufB.end());
        if (decA.decodeFrame(pktBufA.data(), pktBufA.size(), decBufAL.data(),
                             spec->channels == 2 ? decBufAR.data() : nullptr,
                             err) != frameSamples ||
            decB.decodeFrame(pktBufB.data(), pktBufB.size(), decBufBL.data(),
                             spec->channels == 2 ? decBufBR.data() : nullptr,
                             err) != frameSamples) {
            std::cerr << "FAIL: Interleaved decodeFrame(" << mode << "): " << err << "\n";
            return false;
        }
        intDecAL.insert(intDecAL.end(), decBufAL.begin(), decBufAL.end());
        intDecBL.insert(intDecBL.end(), decBufBL.begin(), decBufBL.end());
        if (spec->channels == 2) {
            intDecAR.insert(intDecAR.end(), decBufAR.begin(), decBufAR.end());
            intDecBR.insert(intDecBR.end(), decBufBR.begin(), decBufBR.end());
        }
    }

    if (intEncA != isoEncA || intEncB != isoEncB) {
        std::cerr << "FAIL: Handle state leaked between concurrent encoders for " << mode << "\n";
        return false;
    }
    if (std::memcmp(intDecAL.data(), isoDecAL.data(), intDecAL.size() * sizeof(float)) != 0 ||
        std::memcmp(intDecBL.data(), isoDecBL.data(), intDecBL.size() * sizeof(float)) != 0 ||
        (spec->channels == 2 &&
         (std::memcmp(intDecAR.data(), isoDecAR.data(), intDecAR.size() * sizeof(float)) != 0 ||
          std::memcmp(intDecBR.data(), isoDecBR.data(), intDecBR.size() * sizeof(float)) != 0))) {
        std::cerr << "FAIL: Handle state leaked between concurrent decoders for " << mode << "\n";
        return false;
    }

    std::cout << "PASS isolation [" << mode << "] frames=" << numFrames << "\n";
    return true;
}

/** Exercises 16-second stereo encode/decode streams for GC and heap stability. */
bool testLongStream(const char* mode) {
    constexpr int kSamples = 705600; // 16.0 seconds @ 44.1 kHz
    std::vector<float> left, right;
    generateDeterministicStereoSignal(kSamples, 261.63, 392.0, left, right);

    std::vector<std::uint8_t> container;
    std::string err;
    if (!carta::encodeAtracContainer(mode, left.data(), right.data(), 2, kSamples, container,
                                     err)) {
        std::cerr << "FAIL: 16s encode(" << mode << "): " << err << "\n";
        return false;
    }
    std::vector<float> decL, decR;
    int ch = 0;
    if (!carta::decodeAtracContainer(mode, container.data(), container.size(), decL, decR, ch,
                                     err)) {
        std::cerr << "FAIL: 16s decode(" << mode << "): " << err << "\n";
        return false;
    }
    if (ch != 2 || static_cast<int>(decL.size()) != kSamples ||
        static_cast<int>(decR.size()) != kSamples) {
        std::cerr << "FAIL: 16s sample count mismatch(" << mode << ")\n";
        return false;
    }
    std::cout << "PASS 16s-stream [" << mode << "] bytes=" << container.size() << "\n";
    return true;
}

} // namespace

int main() {
    const juce::File tempDir =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("carta_porffor_codec_test_" +
                          juce::String(juce::Time::getMillisecondCounterHiRes()));
    if (!tempDir.createDirectory()) {
        std::cerr << "FAIL: Could not create temp test directory.\n";
        return 1;
    }

    bool ok = true;
    ok = ok && testNodeParity(tempDir);
    ok = ok && testStreamHandleIsolation("sp-stereo");
    ok = ok && testStreamHandleIsolation("sp-mono");
    ok = ok && testStreamHandleIsolation("lp2");
    ok = ok && testStreamHandleIsolation("hi-sp");
    ok = ok && testLongStream("sp-stereo");
    ok = ok && testLongStream("lp2");
    ok = ok && testLongStream("hi-sp");

    tempDir.deleteRecursively();
    return ok ? 0 : 1;
}
