#!/usr/bin/env node
/**
 * @fileoverview Build isolated Porffor alpha-19 shared libraries for carta1
 * (ATRAC1), carta3 (ATRAC3), and carta5 (ATRAC3plus) inside
 * `carta-studio/build/porffor/lib/` without modifying `third_party/carta{1,3,5}`.
 *
 * Each codec compiles into its own dylib (`lib<codec>_porffor.dylib`) compiled
 * with `-fvisibility=hidden` and `-fno-fast-math -ffp-contract=off` so native
 * reconstruction matches JavaScript bit-for-bit. Only the `<codec>_pf_*` C ABI
 * symbols are exported (verified via `nm -gU`).
 *
 * Usage:
 *   node Tools/build_porffor_codecs.mjs [--force] [--only=carta1,carta3,carta5]
 */
import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

// ---------------------------------------------------------------------------
// Paths, CLI flags, and toolchain configuration
// ---------------------------------------------------------------------------

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)
const studioDir = path.resolve(__dirname, '..')
const thirdPartyDir = path.join(studioDir, 'third_party')
const buildRoot = path.join(studioDir, 'build', 'porffor')
const libOutDir = path.join(buildRoot, 'lib')
const generatedDir = path.join(buildRoot, 'generated')
const workOutDir = path.join(buildRoot, 'codecs-work')
const toolsDir = path.join(buildRoot, 'tools')
const toolsNodeModules = path.join(toolsDir, 'node_modules')
const prebuiltToolsModules = path.join(
  studioDir,
  'experiments',
  'carta1-frame',
  'build',
  'tools',
  'node_modules'
)

const args = new Set(process.argv.slice(2))
const forceRebuild = args.has('--force')
const emitCOnly = args.has('--emit-c-only')
const compileOnlyArg = args.has('--compile-only')
const onlyArg = process.argv.find((a) => a.startsWith('--only='))
const selectedCodecs = onlyArg
  ? new Set(onlyArg.slice('--only='.length).split(',').filter(Boolean))
  : new Set(['carta1', 'carta3', 'carta5'])

const hostPlatform = process.platform
const isMac = hostPlatform === 'darwin'
const isWin = hostPlatform === 'win32'

const porfBin =
  process.env.PORFFOR || path.join(buildRoot, 'toolchain', 'porf')
const clangBin =
  process.env.CC ||
  (isMac && fs.existsSync('/usr/bin/clang') ? '/usr/bin/clang' : 'clang')
const nmBin = fs.existsSync('/usr/bin/nm') ? '/usr/bin/nm' : 'nm'

function codecOutputSpec(codecName) {
  if (isMac) {
    return {
      libFile: `lib${codecName}_porffor.dylib`,
      impLibFile: null,
    }
  }
  if (isWin) {
    return {
      libFile: `${codecName}_porffor.dll`,
      impLibFile: `${codecName}_porffor.lib`,
    }
  }
  return {
    libFile: `lib${codecName}_porffor.so`,
    impLibFile: null,
  }
}

function shouldCompileOnly(codecName) {
  if (emitCOnly) return false
  if (compileOnlyArg || isWin) return true
  const prebuiltC = path.join(generatedDir, `${codecName}_patched.c`)
  return !fs.existsSync(porfBin) && fs.existsSync(prebuiltC)
}

// ---------------------------------------------------------------------------
// Process execution and toolchain bootstrapping
// ---------------------------------------------------------------------------

/**
 * Run a subprocess synchronously and throw with captured output on non-zero exit.
 *
 * @param {string} cmd - Executable path or name.
 * @param {string[]} cmdArgs - Command-line arguments.
 * @param {import('node:child_process').SpawnSyncOptions} [options={}] - Extra spawn options.
 * @returns {import('node:child_process').SpawnSyncReturns<string>} Completed process result.
 */
function runChecked(cmd, cmdArgs, options = {}) {
  const res = spawnSync(cmd, cmdArgs, {
    encoding: 'utf8',
    maxBuffer: 64 * 1024 * 1024,
    ...options,
  })
  if (res.error) throw res.error
  if (res.status !== 0) {
    const stderr = (res.stderr || '').trim()
    const stdout = (res.stdout || '').trim()
    throw new Error(
      `Command failed (${res.status}): ${cmd} ${cmdArgs.join(' ')}\n${stderr}\n${stdout}`.trim()
    )
  }
  return res
}

/**
 * Verify the pinned Porffor alpha-19 binary (`0c135ce`), prepare isolated build
 * directories under `build/porffor/`, and ensure `@babel/core` + staging deps
 * exist in `build/porffor/tools/node_modules`.
 */
function ensureToolchainAndDeps(needPorffor) {
  fs.mkdirSync(buildRoot, { recursive: true })
  fs.mkdirSync(libOutDir, { recursive: true })
  fs.mkdirSync(generatedDir, { recursive: true })
  fs.mkdirSync(workOutDir, { recursive: true })

  if (!needPorffor) {
    return
  }

  if (!fs.existsSync(porfBin)) {
    throw new Error(
      `Missing pinned Porffor binary at ${porfBin}. Run 'node Tools/fetch_porffor.mjs' or provide pre-generated C sources in ${generatedDir}.`
    )
  }
  const help = runChecked(porfBin, ['--help'])
  if (!help.stdout.includes('alpha 19 (0c135ce 2026-10-21)')) {
    throw new Error(`Expected Porffor alpha-19 (0c135ce), got:\n${help.stdout}`)
  }

  fs.mkdirSync(toolsDir, { recursive: true })

  if (!fs.existsSync(path.join(toolsNodeModules, '@babel', 'core'))) {
    if (fs.existsSync(prebuiltToolsModules) && fs.existsSync(path.join(prebuiltToolsModules, '@babel', 'core'))) {
      if (fs.existsSync(toolsNodeModules)) {
        fs.rmSync(toolsNodeModules, { recursive: true, force: true })
      }
      fs.symlinkSync(prebuiltToolsModules, toolsNodeModules, 'dir')
    } else {
      fs.copyFileSync(
        path.join(thirdPartyDir, 'carta1', 'package.json'),
        path.join(toolsDir, 'package.json')
      )
      fs.copyFileSync(
        path.join(thirdPartyDir, 'carta1', 'package-lock.json'),
        path.join(toolsDir, 'package-lock.json')
      )
      runChecked('npm', ['ci', '--ignore-scripts', '--prefix', toolsDir], {
        stdio: 'inherit',
      })
    }
  }
  fs.copyFileSync(
    path.join(thirdPartyDir, 'carta1', 'package.json'),
    path.join(toolsDir, 'package.json')
  )
}

// ---------------------------------------------------------------------------
// Embedded Porffor C-bridge sources
// ---------------------------------------------------------------------------

/**
 * Return shared JS/Porffor.c bridge helpers for handle slot allocation,
 * direct host-pointer PCM/byte copies, scratch buffer management, and error text.
 *
 * @returns {string} Embedded JS bridge helper source fragment.
 */
function sharedBridgeHelpers() {
  return `
const encoderHandles = [];
const decoderHandles = [];

function allocSlot(list, item) {
  for (let i = 0; i < list.length; i++) {
    if (list[i] === null) {
      list[i] = item;
      return i;
    }
  }
  list.push(item);
  return list.length - 1;
}

function setLastError(message) {
  const encoded = new TextEncoder().encode(String(message || 'Codec error'));
  const count = encoded.length < 255 ? encoded.length : 255;
  const ptr = Porffor.IR.ptr(encoded.buffer);
  const off = encoded.byteOffset;
  Porffor.c\`
    size_t n = (size_t)PF_F64(count);
    if (n > 255) n = 255;
    if (n > 0) memcpy(g_pf_err, MEM + (u32)PF_F64(ptr) + 8 + (u32)PF_F64(off), n);
    g_pf_err[n] = 0;
  \`;
  return -1;
}

function copyInFloats(srcPtr, samples) {
  const out = new Float32Array(samples);
  const p = Porffor.IR.ptr(out.buffer);
  const off = out.byteOffset;
  Porffor.c\`
    size_t n = (size_t)PF_F64(samples);
    if (n > 0) {
      memcpy(MEM + (u32)PF_F64(p) + 8 + (u32)PF_F64(off), (const void*)(uintptr_t)PF_F64(srcPtr), n * sizeof(float));
    }
  \`;
  return out;
}

function copyInBytes(srcPtr, len) {
  const out = new Uint8Array(len);
  const p = Porffor.IR.ptr(out.buffer);
  const off = out.byteOffset;
  Porffor.c\`
    size_t n = (size_t)PF_F64(len);
    if (n > 0) {
      memcpy(MEM + (u32)PF_F64(p) + 8 + (u32)PF_F64(off), (const void*)(uintptr_t)PF_F64(srcPtr), n);
    }
  \`;
  return out;
}

function copyOutFloats(srcArray, dstPtr, samples) {
  const p = Porffor.IR.ptr(srcArray.buffer);
  const off = srcArray.byteOffset;
  Porffor.c\`
    size_t n = (size_t)PF_F64(samples);
    if (n > 0) {
      memcpy((void*)(uintptr_t)PF_F64(dstPtr), MEM + (u32)PF_F64(p) + 8 + (u32)PF_F64(off), n * sizeof(float));
    }
  \`;
}

function copyOutBytes(srcArray, dstPtr, bytes) {
  const p = Porffor.IR.ptr(srcArray.buffer);
  const off = srcArray.byteOffset;
  Porffor.c\`
    size_t n = (size_t)PF_F64(bytes);
    if (n > 0) {
      memcpy((void*)(uintptr_t)PF_F64(dstPtr), MEM + (u32)PF_F64(p) + 8 + (u32)PF_F64(off), n);
    }
  \`;
}

function reserveScratch(totalBytes) {
  Porffor.c\`
    size_t req = (size_t)PF_F64(totalBytes);
    if (req > g_pf_out_cap) {
      unsigned char* grown = (unsigned char*)realloc(g_pf_out_buf, req ? req : 1);
      if (!grown) abort();
      g_pf_out_buf = grown;
      g_pf_out_cap = req ? req : 1;
    }
    g_pf_out_len = req;
  \`;
}

function writeScratchSlice(srcTypedArray, dstByteOffset, byteLen) {
  const p = Porffor.IR.ptr(srcTypedArray.buffer);
  const off = srcTypedArray.byteOffset;
  Porffor.c\`
    size_t n = (size_t)PF_F64(byteLen);
    if (n > 0) {
      memcpy(g_pf_out_buf + (size_t)PF_F64(dstByteOffset), MEM + (u32)PF_F64(p) + 8 + (u32)PF_F64(off), n);
    }
  \`;
}

function setScratchChannels(channels) {
  Porffor.c\`
    g_pf_out_channels = (int)PF_F64(channels);
  \`;
}

function clearScratch() {
  Porffor.c\`
    g_pf_out_len = 0;
    g_pf_out_channels = 0;
  \`;
}
`
}

/**
 * Generate the Porffor bridge module for `carta1` (ATRAC1 SP Stereo / SP Mono,
 * 512-sample frames, 212-byte sound units, AEA container).
 *
 * @returns {string} Complete `bridge.js` source for `carta1`.
 */
function carta1BridgeSource() {
  return `// Porffor bridge for Carta1 (ATRAC1 SP Stereo / SP Mono)
import { encode } from './codec/pipeline/encoder.js';
import { decode } from './codec/pipeline/decoder.js';
import { serializeFrame, deserializeFrame } from './codec/syntax/frame.js';
import { AeaFile } from './codec/boundary/container.js';
import {
  AEA_HEADER_SIZE,
  SAMPLES_PER_FRAME,
  SOUND_UNIT_SIZE,
  STREAM_FLUSH_TAIL_FRAMES,
  STREAM_PRIMING_SAMPLES,
} from './codec/core/constants.js';
import { EncoderOptions } from './codec/core/options.js';

${sharedBridgeHelpers()}

function createDummyFrame() {
  return {
    nBfu: 0,
    blockModes: [0, 0, 0],
    scaleFactorIndices: new Int32Array(0),
    wordLengthIndices: new Int32Array(0),
    quantizedCoefficients: [],
  };
}

export function bridgeCreate(isEncoder, modeCode, reserved) {
  try {
    const channels = modeCode === 1 ? 1 : 2;
    if (isEncoder) {
      const opts = new EncoderOptions();
      return allocSlot(encoderHandles, {
        channels,
        left: encode(opts),
        right: channels === 2 ? encode(opts) : null,
      });
    }
    return allocSlot(decoderHandles, {
      channels,
      left: decode(),
      right: channels === 2 ? decode() : null,
    });
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta1 create failed');
  }
}

export function bridgeDestroy(handle) {
  if (handle >= 0 && handle < encoderHandles.length && encoderHandles[handle] !== null) {
    encoderHandles[handle] = null;
    return 0;
  }
  if (handle >= 0 && handle < decoderHandles.length && decoderHandles[handle] !== null) {
    decoderHandles[handle] = null;
    return 0;
  }
  return -1;
}

export function bridgeFrameEncode(handle, leftPtr, rightPtr, dstPtr) {
  try {
    const st = encoderHandles[handle];
    if (!st) return setLastError('Invalid carta1 encoder handle');
    const leftIn = copyInFloats(leftPtr, SAMPLES_PER_FRAME);
    const leftBytes = serializeFrame(st.left(leftIn));
    copyOutBytes(leftBytes, dstPtr, SOUND_UNIT_SIZE);
    if (st.channels === 2) {
      const rightIn = copyInFloats(rightPtr, SAMPLES_PER_FRAME);
      const rightBytes = serializeFrame(st.right(rightIn));
      copyOutBytes(rightBytes, dstPtr + SOUND_UNIT_SIZE, SOUND_UNIT_SIZE);
      return SOUND_UNIT_SIZE * 2;
    }
    return SOUND_UNIT_SIZE;
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta1 frame encode failed');
  }
}

export function bridgeFrameDecode(handle, srcPtr, srcLen, leftPtr, rightPtr) {
  try {
    const st = decoderHandles[handle];
    if (!st) return setLastError('Invalid carta1 decoder handle');
    const need = st.channels * SOUND_UNIT_SIZE;
    if (srcLen < need) return setLastError('Truncated carta1 sound unit');
    const inBytes = copyInBytes(srcPtr, need);
    const leftPcm = st.left(deserializeFrame(inBytes.subarray(0, SOUND_UNIT_SIZE)));
    copyOutFloats(leftPcm, leftPtr, SAMPLES_PER_FRAME);
    if (st.channels === 2) {
      const rightPcm = st.right(deserializeFrame(inBytes.subarray(SOUND_UNIT_SIZE, SOUND_UNIT_SIZE * 2)));
      copyOutFloats(rightPcm, rightPtr, SAMPLES_PER_FRAME);
    }
    return SAMPLES_PER_FRAME;
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta1 frame decode failed');
  }
}

export function bridgeContainerEncode(modeCode, channelsArg, leftPtr, rightPtr, sampleCount, reserved) {
  try {
    clearScratch();
    const channels = modeCode === 1 || channelsArg === 1 ? 1 : 2;
    if (sampleCount < 0) return setLastError('Negative sampleCount');
    const leftSrc = copyInFloats(leftPtr, sampleCount);
    const rightSrc = channels === 2 ? copyInFloats(rightPtr, sampleCount) : null;

    const opts = new EncoderOptions();
    const leftEnc = encode(opts);
    const rightEnc = channels === 2 ? encode(opts) : null;
    const totalUnits = [];

    if (sampleCount > 0) {
      const frame = new Float32Array(SAMPLES_PER_FRAME);
      const rFrame = channels === 2 ? new Float32Array(SAMPLES_PER_FRAME) : null;

      for (let pos = 0; pos < sampleCount; pos += SAMPLES_PER_FRAME) {
        frame.fill(0);
        if (rFrame) rFrame.fill(0);
        const count = pos + SAMPLES_PER_FRAME <= sampleCount ? SAMPLES_PER_FRAME : sampleCount - pos;
        for (let i = 0; i < count; i++) {
          frame[i] = leftSrc[pos + i];
          if (rFrame) rFrame[i] = rightSrc[pos + i];
        }
        totalUnits.push(serializeFrame(leftEnc(frame)));
        if (rightEnc) totalUnits.push(serializeFrame(rightEnc(rFrame)));
      }

      const silence = new Float32Array(SAMPLES_PER_FRAME);
      for (let f = 0; f < STREAM_FLUSH_TAIL_FRAMES; f++) {
        totalUnits.push(serializeFrame(leftEnc(silence)));
        if (rightEnc) totalUnits.push(serializeFrame(rightEnc(silence)));
      }
    }

    const header = AeaFile.createHeader(
      'encoded by carta1',
      totalUnits.length,
      channels,
      sampleCount,
      STREAM_PRIMING_SAMPLES
    );
    const totalBytes = AEA_HEADER_SIZE + totalUnits.length * SOUND_UNIT_SIZE;
    reserveScratch(totalBytes);
    setScratchChannels(channels);
    writeScratchSlice(header, 0, AEA_HEADER_SIZE);
    let offset = AEA_HEADER_SIZE;
    for (let i = 0; i < totalUnits.length; i++) {
      writeScratchSlice(totalUnits[i], offset, SOUND_UNIT_SIZE);
      offset += SOUND_UNIT_SIZE;
    }
    return 0;
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta1 container encode failed');
  }
}

export function bridgeContainerDecode(dataPtr, dataLen, reserved) {
  try {
    clearScratch();
    if (dataLen < AEA_HEADER_SIZE) return setLastError('AEA file shorter than 2048-byte header');
    const bytes = copyInBytes(dataPtr, dataLen);
    const info = AeaFile.parseHeader(bytes.subarray(0, AEA_HEADER_SIZE));
    const channels = info.channelCount;
    if (channels !== 1 && channels !== 2) {
      return setLastError('Unsupported AEA channel count');
    }
    const payloadBytes = dataLen - AEA_HEADER_SIZE;
    if (payloadBytes % SOUND_UNIT_SIZE !== 0) {
      return setLastError('Truncated AEA sound unit payload');
    }
    const unitCount = Math.floor(payloadBytes / SOUND_UNIT_SIZE);
    if (unitCount < info.frameCount) {
      return setLastError('Truncated AEA sound unit stream');
    }
    const targetSamples = info.sampleCount;
    const priming = info.primingSampleCount;
    const leftDec = decode();
    const rightDec = channels === 2 ? decode() : null;

    const outLeft = new Float32Array(targetSamples);
    const outRight = channels === 2 ? new Float32Array(targetSamples) : null;
    let timelinePos = 0;
    let emitted = 0;

    function emitBlock(lBlk, rBlk) {
      const start = Math.max(0, priming - timelinePos);
      const remaining = targetSamples - emitted;
      const count = Math.max(0, Math.min(SAMPLES_PER_FRAME - start, remaining));
      timelinePos += SAMPLES_PER_FRAME;
      for (let i = 0; i < count; i++) {
        outLeft[emitted + i] = lBlk[start + i];
        if (outRight) outRight[emitted + i] = rBlk[start + i];
      }
      emitted += count;
    }

    if (channels === 1) {
      for (let off = AEA_HEADER_SIZE; off + SOUND_UNIT_SIZE <= dataLen; off += SOUND_UNIT_SIZE) {
        const unit = deserializeFrame(bytes.subarray(off, off + SOUND_UNIT_SIZE));
        emitBlock(leftDec(unit), null);
      }
    } else {
      let pendingLeft = null;
      for (let off = AEA_HEADER_SIZE; off + SOUND_UNIT_SIZE <= dataLen; off += SOUND_UNIT_SIZE) {
        const unit = deserializeFrame(bytes.subarray(off, off + SOUND_UNIT_SIZE));
        if (pendingLeft === null) {
          pendingLeft = unit;
        } else {
          emitBlock(leftDec(pendingLeft), rightDec(unit));
          pendingLeft = null;
        }
      }
      if (pendingLeft !== null) {
        emitBlock(leftDec(pendingLeft), rightDec(createDummyFrame()));
      }
    }

    if (emitted !== targetSamples) {
      return setLastError('Truncated ATRAC1 timeline');
    }

    const totalFloats = targetSamples * channels;
    reserveScratch(totalFloats * 4);
    setScratchChannels(channels);
    const leftBytes = new Uint8Array(outLeft.buffer, outLeft.byteOffset, outLeft.byteLength);
    writeScratchSlice(leftBytes, 0, leftBytes.byteLength);
    if (outRight) {
      const rightBytes = new Uint8Array(outRight.buffer, outRight.byteOffset, outRight.byteLength);
      writeScratchSlice(rightBytes, leftBytes.byteLength, rightBytes.byteLength);
    }
    return targetSamples;
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta1 container decode failed');
  }
}

if (globalThis.__cartaPorfforRetain) {
  bridgeCreate(0, 0, 0);
  bridgeDestroy(0);
  bridgeFrameEncode(0, 0, 0, 0);
  bridgeFrameDecode(0, 0, 0, 0, 0);
  bridgeContainerEncode(0, 0, 0, 0, 0, 0);
  bridgeContainerDecode(0, 0, 0);
}
`
}

/**
 * Generate the Porffor bridge module for `carta3` (ATRAC3 LP2 132 kbps /
 * LP4 66 kbps, 1024-sample stereo frames, RIFF/AT3 WAV container).
 *
 * @returns {string} Complete `bridge.js` source for `carta3`.
 */
function carta3BridgeSource() {
  return `// Porffor bridge for Carta3 (ATRAC3 LP2 / LP4)
import { encode, decode, encodeWavePcm, decodeWavePcm } from './codec/index.js';

${sharedBridgeHelpers()}

const FRAME_SAMPLES = 1024;

export function bridgeCreate(isEncoder, bitrateKbps, reserved) {
  try {
    const kbps = bitrateKbps === 66 ? 66 : 132;
    const opts = { bitrateKbps: kbps, channels: 2, sampleRate: 44100 };
    if (isEncoder) {
      return allocSlot(encoderHandles, {
        kbps,
        bytesPerFrame: kbps === 66 ? 192 : 384,
        fn: encode(opts),
      });
    }
    return allocSlot(decoderHandles, {
      kbps,
      bytesPerFrame: kbps === 66 ? 192 : 384,
      fn: decode(opts),
    });
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta3 create failed');
  }
}

export function bridgeDestroy(handle) {
  if (handle >= 0 && handle < encoderHandles.length && encoderHandles[handle] !== null) {
    encoderHandles[handle] = null;
    return 0;
  }
  if (handle >= 0 && handle < decoderHandles.length && decoderHandles[handle] !== null) {
    decoderHandles[handle] = null;
    return 0;
  }
  return -1;
}

export function bridgeFrameEncode(handle, leftPtr, rightPtr, dstPtr) {
  try {
    const st = encoderHandles[handle];
    if (!st) return setLastError('Invalid carta3 encoder handle');
    const leftIn = copyInFloats(leftPtr, FRAME_SAMPLES);
    const rightIn = copyInFloats(rightPtr, FRAME_SAMPLES);
    const frame = st.fn([leftIn, rightIn]);
    copyOutBytes(frame, dstPtr, frame.length);
    return frame.length;
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta3 frame encode failed');
  }
}

export function bridgeFrameDecode(handle, srcPtr, srcLen, leftPtr, rightPtr) {
  try {
    const st = decoderHandles[handle];
    if (!st) return setLastError('Invalid carta3 decoder handle');
    const inBytes = copyInBytes(srcPtr, srcLen);
    const pcm = st.fn(inBytes);
    copyOutFloats(pcm[0], leftPtr, FRAME_SAMPLES);
    copyOutFloats(pcm[1], rightPtr, FRAME_SAMPLES);
    return FRAME_SAMPLES;
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta3 frame decode failed');
  }
}

export function bridgeContainerEncode(bitrateKbps, channelsArg, leftPtr, rightPtr, sampleCount, reserved) {
  try {
    clearScratch();
    const kbps = bitrateKbps === 66 ? 66 : 132;
    const left = copyInFloats(leftPtr, sampleCount);
    const right = channelsArg === 2 ? copyInFloats(rightPtr, sampleCount) : left;
    const wav = encodeWavePcm([left, right], { bitrateKbps: kbps });
    reserveScratch(wav.byteLength);
    setScratchChannels(2);
    writeScratchSlice(wav, 0, wav.byteLength);
    return 0;
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta3 container encode failed');
  }
}

export function bridgeContainerDecode(dataPtr, dataLen, reserved) {
  try {
    clearScratch();
    const wavBytes = copyInBytes(dataPtr, dataLen);
    const channels = decodeWavePcm(wavBytes);
    const chCount = channels.length;
    const sampleCount = chCount > 0 ? channels[0].length : 0;
    reserveScratch(sampleCount * chCount * 4);
    setScratchChannels(chCount);
    let offset = 0;
    for (let c = 0; c < chCount; c++) {
      const ch = channels[c];
      const u8 = new Uint8Array(ch.buffer, ch.byteOffset, ch.byteLength);
      writeScratchSlice(u8, offset, u8.byteLength);
      offset += u8.byteLength;
    }
    return sampleCount;
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta3 container decode failed');
  }
}

if (globalThis.__cartaPorfforRetain) {
  bridgeCreate(0, 0, 0);
  bridgeDestroy(0);
  bridgeFrameEncode(0, 0, 0, 0);
  bridgeFrameDecode(0, 0, 0, 0, 0);
  bridgeContainerEncode(0, 0, 0, 0, 0, 0);
  bridgeContainerDecode(0, 0, 0);
}
`
}

/**
 * Generate the Porffor bridge module for `carta5` (ATRAC3plus Hi-SP 256 kbps /
 * Hi-LP 64 kbps, 2048-sample stereo frames, RIFF/AT3+ WAV container).
 *
 * @returns {string} Complete `bridge.js` source for `carta5`.
 */
function carta5BridgeSource() {
  return `// Porffor bridge for Carta5 (ATRAC3plus Hi-SP / Hi-LP)
import { encode, decode, resolveProfile, encodeWavePcm, decodeWavePcm } from './codec/index.js';

${sharedBridgeHelpers()}

const FRAME_SAMPLES = 2048;

export function bridgeCreate(isEncoder, bitrateKbps, reserved) {
  try {
    const kbps = bitrateKbps === 64 ? 64 : 256;
    const profile = resolveProfile({ bitrateKbps: kbps, channels: 2, sampleRate: 44100 });
    if (isEncoder) {
      return allocSlot(encoderHandles, {
        kbps,
        fn: encode(profile),
      });
    }
    return allocSlot(decoderHandles, {
      kbps,
      fn: decode(profile),
    });
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta5 create failed');
  }
}

export function bridgeDestroy(handle) {
  if (handle >= 0 && handle < encoderHandles.length && encoderHandles[handle] !== null) {
    encoderHandles[handle] = null;
    return 0;
  }
  if (handle >= 0 && handle < decoderHandles.length && decoderHandles[handle] !== null) {
    decoderHandles[handle] = null;
    return 0;
  }
  return -1;
}

export function bridgeFrameEncode(handle, leftPtr, rightPtr, dstPtr) {
  try {
    const st = encoderHandles[handle];
    if (!st) return setLastError('Invalid carta5 encoder handle');
    const leftIn = copyInFloats(leftPtr, FRAME_SAMPLES);
    const rightIn = copyInFloats(rightPtr, FRAME_SAMPLES);
    const frame = st.fn({ channels: [leftIn, rightIn], sampleCount: FRAME_SAMPLES });
    if (frame === null) return 0;
    copyOutBytes(frame, dstPtr, frame.length);
    return frame.length;
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta5 frame encode failed');
  }
}

export function bridgeFrameDecode(handle, srcPtr, srcLen, leftPtr, rightPtr) {
  try {
    const st = decoderHandles[handle];
    if (!st) return setLastError('Invalid carta5 decoder handle');
    const inBytes = copyInBytes(srcPtr, srcLen);
    const out = st.fn(inBytes);
    const channels = Array.isArray(out) ? out : out.channels;
    copyOutFloats(channels[0], leftPtr, FRAME_SAMPLES);
    copyOutFloats(channels[1], rightPtr, FRAME_SAMPLES);
    return FRAME_SAMPLES;
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta5 frame decode failed');
  }
}

export function bridgeContainerEncode(bitrateKbps, channelsArg, leftPtr, rightPtr, sampleCount, reserved) {
  try {
    clearScratch();
    const kbps = bitrateKbps === 64 ? 64 : 256;
    const left = copyInFloats(leftPtr, sampleCount);
    const right = channelsArg === 2 ? copyInFloats(rightPtr, sampleCount) : left;
    const wav = encodeWavePcm([left, right], { bitrateKbps: kbps, channels: 2, sampleRate: 44100 });
    reserveScratch(wav.byteLength);
    setScratchChannels(2);
    writeScratchSlice(wav, 0, wav.byteLength);
    return 0;
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta5 container encode failed');
  }
}

export function bridgeContainerDecode(dataPtr, dataLen, reserved) {
  try {
    clearScratch();
    const wavBytes = copyInBytes(dataPtr, dataLen);
    const decoded = decodeWavePcm(wavBytes);
    const channels = Array.isArray(decoded) ? decoded : decoded.channels;
    const chCount = channels.length;
    const sampleCount = chCount > 0 ? channels[0].length : 0;
    reserveScratch(sampleCount * chCount * 4);
    setScratchChannels(chCount);
    let offset = 0;
    for (let c = 0; c < chCount; c++) {
      const ch = channels[c];
      const u8 = new Uint8Array(ch.buffer, ch.byteOffset, ch.byteLength);
      writeScratchSlice(u8, offset, u8.byteLength);
      offset += u8.byteLength;
    }
    return sampleCount;
  } catch (err) {
    return setLastError(err && err.message ? err.message : 'carta5 container decode failed');
  }
}

if (globalThis.__cartaPorfforRetain) {
  bridgeCreate(0, 0, 0);
  bridgeDestroy(0);
  bridgeFrameEncode(0, 0, 0, 0);
  bridgeFrameDecode(0, 0, 0, 0, 0);
  bridgeContainerEncode(0, 0, 0, 0, 0, 0);
  bridgeContainerDecode(0, 0, 0);
}
`
}

const CODECS = [
  {
    name: 'carta1',
    fastLength: true,
    bridgeSource: carta1BridgeSource,
  },
  {
    name: 'carta3',
    fastLength: true,
    bridgeSource: carta3BridgeSource,
  },
  {
    name: 'carta5',
    fastLength: false, // carta5 state objects have `.length` fields; never pass --fast-length
    bridgeSource: carta5BridgeSource,
  },
]

// ---------------------------------------------------------------------------
// Generated-C patching and per-codec build driver
// ---------------------------------------------------------------------------

/**
 * Recursively find the newest modification timestamp (in ms) within `dirPath`.
 *
 * @param {string} dirPath - Directory to scan.
 * @returns {number} Maximum mtime in milliseconds.
 */
function latestMtimeMs(dirPath) {
  let latest = 0
  for (const entry of fs.readdirSync(dirPath, { withFileTypes: true })) {
    const full = path.join(dirPath, entry.name)
    if (entry.isDirectory()) {
      const sub = latestMtimeMs(full)
      if (sub > latest) latest = sub
    } else {
      const st = fs.statSync(full)
      if (st.mtimeMs > latest) latest = st.mtimeMs
    }
  }
  return latest
}

/**
 * Inject C scratch globals/error state and replace the Porffor executable
 * `main()` entrypoint with exported `<codecName>_pf_*` C ABI wrappers.
 *
 * @param {string} cSource - Raw C emitted by `porf c`.
 * @param {string} codecName - Codec identifier (`carta1`, `carta3`, or `carta5`).
 * @returns {string} Patched C source ready for shared-library compilation.
 */
function patchGeneratedC(cSource, codecName) {
  const findFn = (jsName, f64Count) => {
    const f64Args = Array(f64Count).fill('f64').join(', ')
    const re = new RegExp(
      `jsval (p_\\w+_${jsName}_23m\\w+)\\(jsval, jsval${f64Count > 0 ? ', ' + f64Args : ''}\\);`,
      'g'
    )
    const matches = [...cSource.matchAll(re)].map((m) => m[1])
    if (matches.length !== 1) {
      throw new Error(
        `[${codecName}] Expected 1 generated declaration for ${jsName}(f64 x ${f64Count}), found ${matches.length}`
      )
    }
    return matches[0]
  }

  const fnCreate = findFn('bridgeCreate', 3)
  const fnDestroy = findFn('bridgeDestroy', 1)
  const fnFrameEnc = findFn('bridgeFrameEncode', 4)
  const fnFrameDec = findFn('bridgeFrameDecode', 5)
  const fnContEnc = findFn('bridgeContainerEncode', 6)
  const fnContDec = findFn('bridgeContainerDecode', 3)

  const mainAnchor = `int main(int argc, char** argv) {
  porf_init(argc, argv);
  porf_data_init();
  volatile int porf_stack_anchor = 0;
  porf_c_stack_top = (void*)&porf_stack_anchor;
  p__main__23main();
  return 0;
}`
  if (!cSource.includes(mainAnchor)) {
    throw new Error(`[${codecName}] Expected Porffor main(...) anchor not found`)
  }

  // Declare C scratch globals immediately after Porffor includes/prelude, before any JS Porffor.c blocks.
  const preludeMarker = 'static void* porf_c_stack_top = NULL;'
  const preludeIdx = cSource.indexOf(preludeMarker)
  if (preludeIdx === -1) {
    throw new Error(`[${codecName}] Expected '${preludeMarker}' in generated C`)
  }
  const insertAt = preludeIdx + preludeMarker.length
  const scratchDecls = `
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static inline double pf_num_jsval(jsval x) { return x.val; }
static inline double pf_num_f64(f64 x) { return x; }
static inline double pf_num_f32(f32 x) { return (double)x; }
static inline double pf_num_i32(i32 x) { return (double)x; }
static inline double pf_num_u32(u32 x) { return (double)x; }
static inline double pf_num_i64(i64 x) { return (double)x; }
static inline double pf_num_u64(u64 x) { return (double)x; }
#define PF_F64(x) _Generic((x), \\
  jsval: pf_num_jsval, \\
  f64: pf_num_f64, \\
  f32: pf_num_f32, \\
  i32: pf_num_i32, \\
  u32: pf_num_u32, \\
  i64: pf_num_i64, \\
  u64: pf_num_u64)(x)
static unsigned char* g_pf_out_buf = NULL;
static size_t g_pf_out_cap = 0;
static size_t g_pf_out_len = 0;
static int g_pf_out_channels = 0;
static char g_pf_err[256] = {0};
`
  let patched = cSource.slice(0, insertAt) + scratchDecls + cSource.slice(insertAt)

  const exportsBlock = `
#if defined(_WIN32)
#define CARTA_PF_EXPORT __declspec(dllexport)
#else
#define CARTA_PF_EXPORT __attribute__((visibility("default")))
#endif

static int ${codecName}_pf_initialized = 0;
static void ${codecName}_ensure_init(void) {
  if (!${codecName}_pf_initialized) {
    static char arg0[] = "${codecName}-porffor";
    static char* argv[] = { arg0, NULL };
    porf_init(1, argv);
    porf_data_init();
    p__main__23main();
    ${codecName}_pf_initialized = 1;
  }
}

CARTA_PF_EXPORT
int ${codecName}_pf_create_stream(int is_encoder, int mode_code) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  ${codecName}_ensure_init();
  g_pf_err[0] = 0;
  return (int)${fnCreate}(porf_box(0, 0), porf_box(0, 0), (f64)is_encoder, (f64)mode_code, 0.0).val;
}

CARTA_PF_EXPORT
int ${codecName}_pf_destroy_stream(int handle) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  ${codecName}_ensure_init();
  return (int)${fnDestroy}(porf_box(0, 0), porf_box(0, 0), (f64)handle).val;
}

CARTA_PF_EXPORT
int ${codecName}_pf_encode_frame(int handle, const float* left_ptr, const float* right_ptr, unsigned char* dst_ptr) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  ${codecName}_ensure_init();
  if (!dst_ptr) return -1;
  g_pf_err[0] = 0;
  return (int)${fnFrameEnc}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)handle, (f64)(uintptr_t)left_ptr, (f64)(uintptr_t)right_ptr, (f64)(uintptr_t)dst_ptr
  ).val;
}

CARTA_PF_EXPORT
int ${codecName}_pf_decode_frame(int handle, const unsigned char* src_ptr, int src_len, float* left_ptr, float* right_ptr) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  ${codecName}_ensure_init();
  if (!src_ptr || !left_ptr) return -1;
  g_pf_err[0] = 0;
  return (int)${fnFrameDec}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)handle, (f64)(uintptr_t)src_ptr, (f64)src_len, (f64)(uintptr_t)left_ptr, (f64)(uintptr_t)right_ptr
  ).val;
}

CARTA_PF_EXPORT
int ${codecName}_pf_encode_container(int mode_code, int channels, const float* left_ptr, const float* right_ptr, int sample_count) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  ${codecName}_ensure_init();
  if (sample_count < 0 || (sample_count > 0 && !left_ptr)) return -1;
  g_pf_err[0] = 0;
  return (int)${fnContEnc}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)mode_code, (f64)channels, (f64)(uintptr_t)left_ptr, (f64)(uintptr_t)right_ptr, (f64)sample_count, 0.0
  ).val;
}

CARTA_PF_EXPORT
int ${codecName}_pf_decode_container(const unsigned char* data_ptr, int data_len) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  ${codecName}_ensure_init();
  if (!data_ptr || data_len < 0) return -1;
  g_pf_err[0] = 0;
  return (int)${fnContDec}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)data_ptr, (f64)data_len, 0.0
  ).val;
}

CARTA_PF_EXPORT
const unsigned char* ${codecName}_pf_output_ptr(void) {
  return g_pf_out_buf;
}

CARTA_PF_EXPORT
size_t ${codecName}_pf_output_len(void) {
  return g_pf_out_len;
}

CARTA_PF_EXPORT
int ${codecName}_pf_output_channels(void) {
  return g_pf_out_channels;
}

CARTA_PF_EXPORT
const char* ${codecName}_pf_last_error(void) {
  return g_pf_err;
}

CARTA_PF_EXPORT
void ${codecName}_pf_release_scratch(void) {
  free(g_pf_out_buf);
  g_pf_out_buf = NULL;
  g_pf_out_cap = 0;
  g_pf_out_len = 0;
  g_pf_out_channels = 0;
}
`
  patched = patched.replace(mainAnchor, exportsBlock)
  return patched
}

/**
 * Stage, transpile with Porffor, patch C exports, compile with Clang, and
 * verify strict `<codec>_pf_*` symbol isolation for one codec specification.
 *
 * @param {{name: string, fastLength: boolean, bridgeSource: () => string}} spec - Codec build spec.
 */
function buildOneCodec(spec) {
  const codecRoot = path.join(thirdPartyDir, spec.name)
  const outSpec = codecOutputSpec(spec.name)
  const outLibPath = path.join(libOutDir, outSpec.libFile)
  const outImpLibPath = outSpec.impLibFile ? path.join(libOutDir, outSpec.impLibFile) : null
  const generatedCPath = path.join(generatedDir, `${spec.name}_patched.c`)
  const codecWork = path.join(workOutDir, spec.name)
  fs.mkdirSync(codecWork, { recursive: true })

  const compileOnly = shouldCompileOnly(spec.name)
  const scriptMtime = fs.statSync(__filename).mtimeMs

  if (emitCOnly) {
    const srcMtime = latestMtimeMs(path.join(codecRoot, 'codec'))
    const nativeJsMtime = fs.statSync(path.join(codecRoot, 'build', 'native.js')).mtimeMs
    const latestInput = Math.max(scriptMtime, srcMtime, nativeJsMtime)
    if (!forceRebuild && fs.existsSync(generatedCPath) && fs.statSync(generatedCPath).mtimeMs >= latestInput) {
      console.log(`[porffor] Up to date (C): ${generatedCPath}`)
      return
    }
  } else if (compileOnly) {
    if (!fs.existsSync(generatedCPath)) {
      throw new Error(
        `[${spec.name}] Missing pre-generated C source at ${generatedCPath}. Run --emit-c-only on macOS/Linux first.`
      )
    }
    const latestInput = Math.max(scriptMtime, fs.statSync(generatedCPath).mtimeMs)
    if (
      !forceRebuild &&
      fs.existsSync(outLibPath) &&
      (!outImpLibPath || fs.existsSync(outImpLibPath)) &&
      fs.statSync(outLibPath).mtimeMs >= latestInput
    ) {
      console.log(`[porffor] Up to date: ${outLibPath}`)
      return
    }
  } else {
    const srcMtime = latestMtimeMs(path.join(codecRoot, 'codec'))
    const nativeJsMtime = fs.statSync(path.join(codecRoot, 'build', 'native.js')).mtimeMs
    const latestInput = Math.max(scriptMtime, srcMtime, nativeJsMtime)
    if (!forceRebuild && fs.existsSync(outLibPath) && fs.statSync(outLibPath).mtimeMs >= latestInput) {
      console.log(`[porffor] Up to date: ${outLibPath}`)
      return
    }
  }

  let patchedC = generatedCPath
  if (!compileOnly) {
    console.log(`[porffor] Staging & transpiling ${spec.name} (fastLength=${spec.fastLength})...`)
    const bridgePath = path.join(codecWork, 'bridge.js')
    fs.writeFileSync(bridgePath, spec.bridgeSource(), 'utf8')

    let nativeScript = fs.readFileSync(
      path.join(codecRoot, 'build', 'native.js'),
      'utf8'
    )
    const stageNeedle = 'for (const directory of codec.sources) stage(directory)\n'
    if (nativeScript.indexOf(stageNeedle) === -1 || nativeScript.split(stageNeedle).length !== 2) {
      throw new Error(`[${spec.name}] Unexpected staging loop in build/native.js`)
    }
    nativeScript = nativeScript.replace(
      stageNeedle,
      stageNeedle +
        "fs.copyFileSync(process.env.CODEC_BRIDGE, path.join(work, 'bridge.js'))\n"
    )

    const compilerIdx = nativeScript.indexOf('const nativeCompiler = process.env.CC')
    if (compilerIdx === -1) {
      throw new Error(`[${spec.name}] Missing 'const nativeCompiler = process.env.CC' in build/native.js`)
    }
    const fastLengthFlag = spec.fastLength ? "'--fast-length'," : ''
    nativeScript =
      nativeScript.slice(0, compilerIdx) +
      `
fs.writeFileSync(path.join(work, 'entry.js'), "import './host.js'; import './bridge.js'\\n");
try {
  runBuildCommand(porffor, [
    'c',
    '-t',
    path.join(work, 'entry.js'),
    '-o',
    output,
    ${fastLengthFlag}
  ]);
} finally {
  fs.rmSync(work, { recursive: true, force: true });
}
`
    const stageScript = path.join(toolsDir, `stage_${spec.name}.mjs`)
    fs.writeFileSync(stageScript, nativeScript, 'utf8')

    const rawC = path.join(codecWork, `${spec.name}_raw.c`)
    const nativeConfig = JSON.stringify({
      name: spec.name,
      root: codecRoot,
      entry: 'bridge.js',
      argv: [spec.name, 'cli.js'],
      sources: ['codec'],
      constants: 'codec/core/constants.js',
    })

    const nodeDir = path.dirname(process.execPath)
    const pathEnv = process.env.PATH
      ? `${nodeDir}${path.delimiter}${process.env.PATH}`
      : `${nodeDir}:/usr/bin:/bin:/opt/local/bin:/opt/homebrew/bin`
    runChecked(process.execPath, [stageScript, rawC], {
      env: {
        ...process.env,
        PATH: pathEnv,
        PORFFOR: porfBin,
        CODEC_BRIDGE: bridgePath,
        NATIVE_BUILD: nativeConfig,
      },
      stdio: 'inherit',
    })

    const rawSource = fs.readFileSync(rawC, 'utf8')
    const patchedSource = patchGeneratedC(rawSource, spec.name)
    fs.writeFileSync(generatedCPath, patchedSource, 'utf8')
    if (emitCOnly) {
      console.log(`[porffor] Emitted ${generatedCPath}`)
      return
    }
  }

  console.log(`[porffor] Compiling ${outSpec.libFile} from ${patchedC}...`)
  const tmpLib = path.join(codecWork, outSpec.libFile)
  const optFlag = spec.fastLength ? '-O2' : '-O1'

  if (isMac) {
    runChecked(
      clangBin,
      [
        '-dynamiclib',
        optFlag,
        '-fno-fast-math',
        '-ffp-contract=off',
        '-fno-math-errno',
        '-fPIC',
        '-fvisibility=hidden',
        '-mllvm',
        '-enable-machine-outliner=never',
        '-Wl,-mllvm,-enable-machine-outliner=never',
        '-install_name',
        `@rpath/${outSpec.libFile}`,
        '-mmacosx-version-min=11.0',
        patchedC,
        '-o',
        tmpLib,
      ],
      { stdio: 'inherit' }
    )
    const nm = runChecked(nmBin, ['-gU', tmpLib])
    const exported = nm.stdout
      .split('\n')
      .map((line) => line.trim())
      .filter(Boolean)
      .map((line) => line.split(/\s+/).pop())
    const expectedPrefix = `_${spec.name}_pf_`
    const unexpected = exported.filter((sym) => !sym.startsWith(expectedPrefix))
    if (unexpected.length > 0 || exported.length === 0) {
      throw new Error(
        `[${spec.name}] Exported symbol check failed.\nExpected only ${expectedPrefix}*\nUnexpected: ${unexpected.join(', ')}\nAll: ${exported.join(', ')}`
      )
    }
    fs.renameSync(tmpLib, outLibPath)
    console.log(
      `[porffor] Built ${outLibPath} (${exported.length} exported symbols: ${exported.join(', ')})`
    )
  } else if (isWin) {
    const tmpImpLib = path.join(codecWork, outSpec.impLibFile)
    runChecked(
      clangBin,
      [
        '-shared',
        optFlag,
        '-fno-fast-math',
        '-ffp-contract=off',
        '-fno-math-errno',
        '-fvisibility=hidden',
        `-Wl,/IMPLIB:${tmpImpLib}`,
        patchedC,
        '-o',
        tmpLib,
      ],
      { stdio: 'inherit' }
    )
    fs.copyFileSync(tmpLib, outLibPath)
    fs.copyFileSync(tmpImpLib, outImpLibPath)
    console.log(`[porffor] Built ${outLibPath} + ${outImpLibPath}`)
  } else {
    runChecked(
      clangBin,
      [
        '-shared',
        optFlag,
        '-fno-fast-math',
        '-ffp-contract=off',
        '-fno-math-errno',
        '-fPIC',
        '-fvisibility=hidden',
        '-mllvm',
        '-enable-machine-outliner=never',
        `-Wl,-soname,${outSpec.libFile}`,
        patchedC,
        '-lm',
        '-o',
        tmpLib,
      ],
      { stdio: 'inherit' }
    )
    const nm = runChecked(nmBin, ['-D', '--defined-only', '-g', tmpLib])
    const exported = nm.stdout
      .split('\n')
      .map((line) => line.trim())
      .filter(Boolean)
      .map((line) => line.split(/\s+/).pop())
      .filter((sym) => sym && !sym.startsWith('_') && sym !== 'etext' && sym !== 'edata' && sym !== 'end')
    const expectedPrefix = `${spec.name}_pf_`
    const unexpected = exported.filter((sym) => !sym.startsWith(expectedPrefix))
    if (unexpected.length > 0 || exported.length === 0) {
      throw new Error(
        `[${spec.name}] Exported symbol check failed.\nExpected only ${expectedPrefix}*\nUnexpected: ${unexpected.join(', ')}\nAll: ${exported.join(', ')}`
      )
    }
    fs.renameSync(tmpLib, outLibPath)
    console.log(
      `[porffor] Built ${outLibPath} (${exported.length} exported symbols: ${exported.join(', ')})`
    )
  }
}

// ---------------------------------------------------------------------------
// Entrypoint
// ---------------------------------------------------------------------------

const needPorffor = CODECS.some(
  (spec) => selectedCodecs.has(spec.name) && !shouldCompileOnly(spec.name)
)
ensureToolchainAndDeps(needPorffor)
for (const spec of CODECS) {
  if (selectedCodecs.has(spec.name)) {
    buildOneCodec(spec)
  }
}
