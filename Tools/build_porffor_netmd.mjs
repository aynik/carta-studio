#!/usr/bin/env node
/**
 * @fileoverview Build the isolated Porffor alpha-19 shared library for
 * `netmd-js` + `netmd-exploits` inside
 * `carta-studio/build/porffor/lib/libnetmd_porffor.dylib`.
 *
 * Heavy OS, USB, crypto, and charset primitives (CommonCrypto DES/3DES and RNG,
 * iconv CP932, sleep, progress reporting, and libusb-1.0) live in C++
 * (`Source/MiniDisc/NetMdHostBridge.cpp`) and are resolved at load time via
 * `-Wl,-undefined,dynamic_lookup` / `-Wl,-export_dynamic`.
 *
 * Build pipeline stages:
 *   1. Pre-assemble `ForceTOCEdit` ARM/THUMB payloads across supported firmwares.
 *   2. Stage `netmd-js` and `netmd-exploits` with pure-JS node_modules shims.
 *   3. Bundle via esbuild and lower `async`/`await` to `__CartaSyncPromise`
 *      via Babel so Porffor runs O(1) stack depth synchronously.
 *   4. Run a Node.js `retailmac` + 5,000-step stack-depth sanity test.
 *   5. Transpile to C via Porffor alpha-19, patch C host/scratch ABI symbols,
 *      compile with Clang (`-fvisibility=hidden`), and verify only `_netmd_pf_*`
 *      symbols are exported.
 *
 * Usage:
 *   node Tools/build_porffor_netmd.mjs [--force]
 */
import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import { createRequire } from 'node:module'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

// ---------------------------------------------------------------------------
// Paths, CLI flags, and supported ForceTOCEdit firmware targets
// ---------------------------------------------------------------------------

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)
const studioDir = path.resolve(__dirname, '..')
const buildRoot = path.join(studioDir, 'build', 'porffor')
const libOutDir = path.join(buildRoot, 'lib')
const generatedDir = path.join(buildRoot, 'generated')
const workDir = path.join(buildRoot, 'netmd-work')
const stagedDir = path.join(workDir, 'staged')
const shimsDir = path.join(workDir, 'shims')
const toolsDir = path.join(buildRoot, 'tools')
const toolsNodeModules = path.join(toolsDir, 'node_modules')
const netmdDepsDir = path.join(buildRoot, 'netmd-deps')
const netmdDepsModules = path.join(netmdDepsDir, 'node_modules')
const committedNetmdDepsDir = path.join(studioDir, 'Tools', 'netmd-deps')
const generatedCPath = path.join(generatedDir, 'netmd_patched.c')

const hostPlatform = process.platform
const isMac = hostPlatform === 'darwin'
const isWin = hostPlatform === 'win32'

const outLibName = isMac
  ? 'libnetmd_porffor.dylib'
  : isWin
    ? 'netmd_porffor.lib'
    : 'libnetmd_porffor.so'
const dylibPath = path.join(libOutDir, outLibName)

const porfBin = process.env.PORFFOR || path.join(buildRoot, 'toolchain', 'porf')
const clangBin =
  process.env.CC ||
  (isMac && fs.existsSync('/usr/bin/clang') ? '/usr/bin/clang' : 'clang')
const nmBin = fs.existsSync('/usr/bin/nm') ? '/usr/bin/nm' : 'nm'

const cliArgs = new Set(process.argv.slice(2))
const forceRebuild = cliArgs.has('--force')
const emitCOnly = cliArgs.has('--emit-c-only')
const compileOnlyArg = cliArgs.has('--compile-only')

function shouldCompileOnly() {
  if (emitCOnly) return false
  if (compileOnlyArg || isWin) return true
  return !fs.existsSync(porfBin) && fs.existsSync(generatedCPath)
}

const FORCE_TOC_VERSIONS = [
  'S1.600',
  'S1.500',
  'S1.400',
  'S1.300',
  'S1.200',
  'S1.100',
  'S1.000',
  'R1.000',
  'R1.100',
  'R1.200',
  'R1.300',
  'R1.400',
  'Hn1.000',
  'Hn1.100',
  'Hn1.10A',
  'Hn1.200',
  'Hn1.20A',
  'Hr1.000',
  'Hp1.000',
  'Hx1.040',
  'Hx1.060',
  'Hx1.070',
  'Hx1.080',
  'Hx1.090',
  'Hx1.0A0',
]

// ---------------------------------------------------------------------------
// Process execution, prerequisites, and mtime utilities
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
    maxBuffer: 128 * 1024 * 1024,
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
 * Verify the pinned Porffor alpha-19 compiler (`0c135ce`), staged `@babel/core`
 * + `esbuild` tools, and `netmd-js` / `netmd-exploits` / `diacritics` packages.
 */
function ensurePrereqs(needPorffor) {
  fs.mkdirSync(buildRoot, { recursive: true })
  fs.mkdirSync(libOutDir, { recursive: true })
  fs.mkdirSync(generatedDir, { recursive: true })
  fs.mkdirSync(workDir, { recursive: true })

  if (!needPorffor) {
    return
  }

  if (!fs.existsSync(porfBin)) {
    throw new Error(
      `Missing pinned Porffor binary at ${porfBin}. Run 'node Tools/fetch_porffor.mjs' or provide ${generatedCPath}.`
    )
  }
  const help = runChecked(porfBin, ['--help'])
  if (!help.stdout.includes('alpha 19 (0c135ce 2026-10-21)')) {
    throw new Error(`Expected Porffor alpha-19 (0c135ce), got:\n${help.stdout}`)
  }

  if (
    !fs.existsSync(path.join(toolsNodeModules, '@babel', 'core')) ||
    !fs.existsSync(path.join(toolsNodeModules, 'esbuild'))
  ) {
    fs.mkdirSync(toolsDir, { recursive: true })
    fs.copyFileSync(
      path.join(studioDir, 'third_party', 'carta1', 'package.json'),
      path.join(toolsDir, 'package.json')
    )
    fs.copyFileSync(
      path.join(studioDir, 'third_party', 'carta1', 'package-lock.json'),
      path.join(toolsDir, 'package-lock.json')
    )
    runChecked('npm', ['ci', '--ignore-scripts', '--prefix', toolsDir], {
      stdio: 'inherit',
    })
  }

  const committedLockPath = path.join(committedNetmdDepsDir, 'package-lock.json')
  const buildLockPath = path.join(netmdDepsDir, 'package-lock.json')
  const lockChanged =
    !fs.existsSync(buildLockPath) ||
    !fs.existsSync(committedLockPath) ||
    fs.readFileSync(buildLockPath, 'utf8') !== fs.readFileSync(committedLockPath, 'utf8')

  if (!fs.existsSync(path.join(netmdDepsModules, 'netmd-js')) || lockChanged) {
    if (!fs.existsSync(path.join(committedNetmdDepsDir, 'package.json'))) {
      throw new Error(`Missing committed NetMD manifests in ${committedNetmdDepsDir}`)
    }
    fs.mkdirSync(netmdDepsDir, { recursive: true })
    fs.copyFileSync(
      path.join(committedNetmdDepsDir, 'package.json'),
      path.join(netmdDepsDir, 'package.json')
    )
    fs.copyFileSync(committedLockPath, buildLockPath)
    runChecked('npm', ['ci', '--ignore-scripts', '--prefix', netmdDepsDir], {
      stdio: 'inherit',
    })
  }

  // If netmd-exploits was installed from a git fork (without prebuilt dist/),
  // compile its TypeScript sources via the pinned typescript devDependency.
  const exploitsPkgDir = path.join(netmdDepsModules, 'netmd-exploits')
  const exploitsDistSpUpload = path.join(exploitsPkgDir, 'dist', 'exploits', 'sp-upload.js')
  if (!fs.existsSync(exploitsDistSpUpload)) {
    const tscJs = path.join(netmdDepsModules, 'typescript', 'bin', 'tsc')
    if (!fs.existsSync(tscJs)) {
      throw new Error(`Missing typescript compiler at ${tscJs} to build netmd-exploits`)
    }
    runChecked(process.execPath, [tscJs, '-p', path.join(exploitsPkgDir, 'tsconfig.json')], {
      stdio: 'inherit',
    })
  }

  for (const mod of ['@babel/core', 'esbuild']) {
    if (!fs.existsSync(path.join(toolsNodeModules, mod))) {
      throw new Error(
        `Missing build tool dependency ${mod} in ${toolsNodeModules}`
      )
    }
  }
  for (const pkg of ['netmd-js', 'netmd-exploits', 'diacritics']) {
    if (!fs.existsSync(path.join(netmdDepsModules, pkg))) {
      throw new Error(`Missing NetMD package ${pkg} in ${netmdDepsModules}`)
    }
  }
}

/**
 * Recursively find the newest modification timestamp (in ms) for `targetPath`.
 *
 * @param {string} targetPath - File or directory path.
 * @returns {number} Maximum mtime in milliseconds, or 0 if missing.
 */
function latestMtimeMs(targetPath) {
  if (!fs.existsSync(targetPath)) return 0
  const st = fs.statSync(targetPath)
  if (!st.isDirectory()) return st.mtimeMs
  let latest = st.mtimeMs
  for (const entry of fs.readdirSync(targetPath, { withFileTypes: true })) {
    const sub = latestMtimeMs(path.join(targetPath, entry.name))
    if (sub > latest) latest = sub
  }
  return latest
}

// ---------------------------------------------------------------------------
// Firmware payload pre-assembly and NetMD package staging
// ---------------------------------------------------------------------------

/**
 * Pre-assemble `ForceTOCEdit` ARM/THUMB machine-code payloads offline for each
 * supported Sony Type-S/R/Hi-MD firmware version so the runtime requires no assembler.
 *
 * @param {NodeRequire} requireFromDeps - CommonJS loader anchored in `netmd-deps`.
 */
function writeU32LE(buf, offset, value) {
  buf[offset] = value & 0xff
  buf[offset + 1] = (value >>> 8) & 0xff
  buf[offset + 2] = (value >>> 16) & 0xff
  buf[offset + 3] = (value >>> 24) & 0xff
}

function buildStatusReadCode(pendingFlagAddress, activeStateAddress, activeStateSize) {
  if (activeStateSize !== 1 && activeStateSize !== 4) {
    throw new Error(`Unsupported tocActiveStateSize: ${activeStateSize}`)
  }
  const stub = new Uint8Array(36)
  writeU32LE(stub, 0x00, 0xe59f0014)
  writeU32LE(stub, 0x04, 0xe5900000)
  writeU32LE(stub, 0x08, 0xe58f000c)
  writeU32LE(stub, 0x0c, 0xe59f000c)
  writeU32LE(stub, 0x10, activeStateSize === 1 ? 0xe5d00000 : 0xe5900000)
  writeU32LE(stub, 0x14, 0xe58f0004)
  writeU32LE(stub, 0x18, 0xe12fff1e)
  writeU32LE(stub, 0x1c, pendingFlagAddress)
  writeU32LE(stub, 0x20, activeStateAddress)
  return Array.from(stub)
}

async function preassembleForceTocPayloads(requireFromDeps) {
  const { Assembler } = requireFromDeps(
    'netmd-exploits/dist/assembler/assembler.js'
  )
  const { CORE_MACROS } = requireFromDeps(
    'netmd-exploits/dist/assembler/core-macros.js'
  )
  const { ForceTOCEdit } = requireFromDeps(
    'netmd-exploits/dist/exploits/force-toc-edit.js'
  )

  const armAssembler = await Assembler.create(CORE_MACROS, 'ARM')
  const thumbAssembler = await Assembler.create(CORE_MACROS, 'THUMB')

  const lowerMap = {}
  const triggerMap = {}
  const raiseMap = {}
  const skipFlushAddrMap = {}
  const skipFlushValMap = {}
  const pollStubMap = {}
  const pollMaskMap = {}
  const pollActiveSizeMap = {}

  for (const ver of FORCE_TOC_VERSIONS) {
    const fakeManager = {
      iface: null,
      factoryIface: null,
      device: { versionCode: ver },
      armAssembler,
      thumbAssembler,
      allocate() {
        throw new Error('Unexpected allocate in ForceTOCEdit pre-assembly')
      },
      log() {},
    }
    const inst = new ForceTOCEdit(fakeManager)
    lowerMap[ver] = Array.from(
      inst.assembleProperty('raiseLowerCode', { lowerOrRaise: 0x02 })
    )
    triggerMap[ver] = Array.from(inst.assembleProperty('triggerCode'))
    raiseMap[ver] = Array.from(
      inst.assembleProperty('raiseLowerCode', { lowerOrRaise: 0x01 })
    )
    const skipAddr = inst.getPropertyOrNull('skipFlushAddress')
    if (skipAddr !== null) {
      skipFlushAddrMap[ver] = skipAddr
      skipFlushValMap[ver] = Array.from(inst.getProperty('skipFlushValue'))
    }
    const pendingFlagAddr = inst.getPropertyOrNull('tocPendingFlagAddress')
    const pendingFlagMask = inst.getPropertyOrNull('tocPendingFlagMask')
    const activeStateAddr = inst.getPropertyOrNull('tocActiveStateAddress')
    const activeStateSize = inst.getPropertyOrNull('tocActiveStateSize')
    if (
      pendingFlagAddr === null ||
      pendingFlagMask === null ||
      activeStateAddr === null ||
      activeStateSize === null
    ) {
      throw new Error(
        `Missing TOC completion poll properties in ForceTOCEdit for firmware ${ver}`
      )
    }
    pollStubMap[ver] = buildStatusReadCode(
      pendingFlagAddr,
      activeStateAddr,
      activeStateSize
    )
    pollMaskMap[ver] = pendingFlagMask
    pollActiveSizeMap[ver] = activeStateSize
  }

  return {
    lowerMap,
    triggerMap,
    raiseMap,
    skipFlushAddrMap,
    skipFlushValMap,
    pollStubMap,
    pollMaskMap,
    pollActiveSizeMap,
  }
}

/**
 * Replace an exact snippet in `source` or throw with a descriptive label.
 *
 * @param {string} source - Input JavaScript source string.
 * @param {string} needle - Exact substring expected in `source`.
 * @param {string} replacement - Replacement text.
 * @param {string} label - Diagnostic identifier for missing snippets.
 * @returns {string} Patched source string.
 */
function replaceOrThrow(source, needle, replacement, label) {
  if (!source.includes(needle)) {
    throw new Error(`[netmd-stage] Missing expected snippet for ${label}`)
  }
  return source.replace(needle, replacement)
}

/**
 * Stage patched `netmd-js`, `netmd-exploits`, and pure-JS compatibility shims
 * (`diacritics`, `util`, `buffer`, `jconv`, `jsbi`, `crypto-js-wasm`) inside
 * `build/porffor/netmd-work/`.
 *
 * @param {NodeRequire} requireFromDeps - CommonJS loader anchored in `netmd-deps`.
 * @param {{lowerMap: Record<string, number[]>, triggerMap: Record<string, number[]>, raiseMap: Record<string, number[]>}} forceTocTables
 */
function stageNetmdPackages(requireFromDeps, forceTocTables) {
  fs.rmSync(workDir, { recursive: true, force: true })
  fs.mkdirSync(path.join(stagedDir, 'netmd-js', 'factory'), { recursive: true })
  fs.mkdirSync(path.join(stagedDir, 'netmd-exploits', 'exploits'), {
    recursive: true,
  })
  fs.mkdirSync(path.join(stagedDir, 'netmd-exploits', 'assembler'), {
    recursive: true,
  })
  fs.mkdirSync(shimsDir, { recursive: true })
  fs.mkdirSync(libOutDir, { recursive: true })

  // 1. Copy & transform netmd-js/dist (*.js excluding *.test.js)
  const netmdDist = path.join(netmdDepsModules, 'netmd-js', 'dist')
  const tsAwaiterSnippet = `var __awaiter = (this && this.__awaiter) || function (thisArg, _arguments, P, generator) {
    function adopt(value) { return value instanceof P ? value : new P(function (resolve) { resolve(value); }); }
    return new (P || (P = Promise))(function (resolve, reject) {
        function fulfilled(value) { try { step(generator.next(value)); } catch (e) { reject(e); } }
        function rejected(value) { try { step(generator["throw"](value)); } catch (e) { reject(e); } }
        function step(result) { result.done ? resolve(result.value) : adopt(result.value).then(fulfilled, rejected); }
        step((generator = generator.apply(thisArg, _arguments || [])).next());
    });
};`
  let patchedAwaiterCount = 0
  const copyNetmdDir = (srcSub, dstSub) => {
    const srcDir = path.join(netmdDist, srcSub)
    const dstDir = path.join(stagedDir, 'netmd-js', dstSub)
    fs.mkdirSync(dstDir, { recursive: true })
    for (const entry of fs.readdirSync(srcDir, { withFileTypes: true })) {
      if (entry.isDirectory()) {
        copyNetmdDir(
          path.join(srcSub, entry.name),
          path.join(dstSub, entry.name)
        )
        continue
      }
      if (!entry.name.endsWith('.js') || entry.name.endsWith('.test.js'))
        continue
      let code = fs
        .readFileSync(path.join(srcDir, entry.name), 'utf8')
        .replaceAll('\r\n', '\n')
      const rel = path.join(srcSub, entry.name).replaceAll('\\', '/')

      if (code.includes(tsAwaiterSnippet)) {
        code = code.replace(
          tsAwaiterSnippet,
          'var __awaiter = function (thisArg, _arguments, P, generator) { return globalThis.__cartaSyncAwaiter(thisArg, _arguments, P, generator); };'
        )
        patchedAwaiterCount++
      }

      if (rel === 'utils.js') {
        code = replaceOrThrow(
          code,
          `function sleep(msec) {
    return __awaiter(this, void 0, void 0, function () {
        return __generator(this, function (_a) {
            switch (_a.label) {
                case 0: return [4 /*yield*/, new Promise(function (resolve) { return setTimeout(resolve, msec); })];
                case 1:
                    _a.sent();
                    return [2 /*return*/];
            }
        });
    });
}`,
          `function sleep(msec) {
    if (globalThis.__cartaNetMdHost && globalThis.__cartaNetMdHost.sleepMs) {
        globalThis.__cartaNetMdHost.sleepMs(msec | 0);
    }
    return Promise.resolve();
}`,
          'netmd-js/utils.js sleep'
        )
        code = replaceOrThrow(
          code,
          `function aggressiveSanitizeTitle(title) {
    return (0, diacritics_1.remove)(title).normalize('NFD').replace(/[^\\x00-\\x7F]/g, '');
}`,
          `function aggressiveSanitizeTitle(title) {
    var clean = (0, diacritics_1.remove)(String(title));
    var out = '';
    for (var i = 0; i < clean.length; i++) {
        var c = clean.charCodeAt(i);
        if (c >= 0 && c <= 0x7f) out += clean.charAt(i);
    }
    return out;
}`,
          'netmd-js/utils.js aggressiveSanitizeTitle'
        )
      }

      if (rel === 'netmd.js') {
        code = replaceOrThrow(
          code,
          `                        if (!(len === 0)) return [3 /*break*/, 5];
                        return [4 /*yield*/, (0, utils_1.sleep)(NetMD.readReplyRetryIntervalInMsec * Math.pow(2, i))];`,
          `                        if (!(len === 0)) return [3 /*break*/, 5];
                        if (i > 180) throw new Error('Timed out waiting for NetMD reply');
                        return [4 /*yield*/, (0, utils_1.sleep)(Math.min(1000, NetMD.readReplyRetryIntervalInMsec * Math.pow(2, i)))];`,
          'netmd-js/netmd.js bounded _readReply sleep'
        )
      }

      if (rel === 'netmd-commands.js') {
        code = replaceOrThrow(
          code,
          'newFullWidthNameWithGroups = oldRawFullWidthName.replace(/^０；.*?／／/, newFullWidthName !== \'\' ? "\\uFF10\\uFF1B".concat(newFullWidthName, "\\uFF0F\\uFF0F") : "");',
          `var __fwSep = oldRawFullWidthName.indexOf('\\uFF0F\\uFF0F');
                            var __fwRest = __fwSep >= 0 ? oldRawFullWidthName.slice(__fwSep + 2) : oldRawFullWidthName;
                            newFullWidthNameWithGroups = (newFullWidthName !== '' ? "\\uFF10\\uFF1B".concat(newFullWidthName, "\\uFF0F\\uFF0F") : "") + __fwRest;`,
          'netmd-commands.js full-width regex replace'
        )
        code = replaceOrThrow(
          code,
          'newNameWithGroups = oldRawName.replace(/^0;.*?\\/\\//, newName !== \'\' ? "0;".concat(newName, "//") : ""); // Replace or delete the old title',
          `var __hwSep = oldRawName.indexOf('//');
                            var __hwRest = __hwSep >= 0 ? oldRawName.slice(__hwSep + 2) : oldRawName;
                            newNameWithGroups = (newName !== '' ? "0;".concat(newName, "//") : "") + __hwRest;`,
          'netmd-commands.js half-width regex replace'
        )
      }

      fs.writeFileSync(path.join(dstDir, entry.name), code, 'utf8')
    }
  }
  copyNetmdDir('', '')
  if (patchedAwaiterCount < 8) {
    throw new Error(
      `[netmd-stage] Expected >= 8 __awaiter helpers patched across netmd-js, got ${patchedAwaiterCount}`
    )
  }

  // 2. Stage runtime subset of netmd-exploits/dist
  const exploitsDist = path.join(netmdDepsModules, 'netmd-exploits', 'dist')
  const stageExploitFile = (rel, transform = null) => {
    let code = fs.readFileSync(path.join(exploitsDist, rel), 'utf8')
    if (transform) code = transform(code)
    const dst = path.join(stagedDir, 'netmd-exploits', rel)
    fs.mkdirSync(path.dirname(dst), { recursive: true })
    fs.writeFileSync(dst, code, 'utf8')
  }

  fs.writeFileSync(
    path.join(stagedDir, 'netmd-exploits', 'assembler', 'core-macros.js'),
    `"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.CORE_MACROS = { fw_compat_bridge: { properties: {} } };
`,
    'utf8'
  )

  stageExploitFile('utils.js', (code) =>
    replaceOrThrow(
      code,
      `function isVersionMatchingFormat(format, version) {
    return new RegExp('^' + format.replace(/\\*/g, '.*') + '$').test(version);
}`,
      `function isVersionMatchingFormat(format, version) {
    if (format === '*') return true;
    var parts = String(format).split('*');
    if (parts.length === 1) return format === version;
    var pos = 0;
    if (parts[0].length > 0) {
        if (!version.startsWith(parts[0])) return false;
        pos = parts[0].length;
    }
    var last = parts[parts.length - 1];
    for (var i = 1; i < parts.length - 1; i++) {
        if (parts[i].length === 0) continue;
        var found = version.indexOf(parts[i], pos);
        if (found === -1) return false;
        pos = found + parts[i].length;
    }
    if (last.length > 0) {
        var endPos = version.length - last.length;
        if (endPos < pos) return false;
        return version.endsWith(last);
    }
    return true;
}`,
      'netmd-exploits/utils.js isVersionMatchingFormat'
    )
  )

  stageExploitFile('exploit.js')
  stageExploitFile('safety.js')
  stageExploitFile('exploits/usb-code-execution.js')
  stageExploitFile('exploits/sp-upload.js')
  stageExploitFile('exploits/mono-sp-upload.js')
  stageExploitFile('exploits/kill-eeprom-writes.js')

  stageExploitFile('exploit-state.js', (code) => {
    let out = replaceOrThrow(
      code,
      'const assembler_1 = require("./assembler/assembler");',
      '// assembler disabled (pre-assembled at build time)',
      'exploit-state assembler require'
    )
    out = replaceOrThrow(
      out,
      `            const fake = new Proxy(this, {
                get: (target, prop, receiver) => { var _a; return ((_a = overwrittenMethods[prop]) !== null && _a !== void 0 ? _a : this[prop]); }
            });`,
      `            const fake = {
                iface: this.iface,
                factoryIface: this.factoryIface,
                device: this.device,
                armAssembler: this.armAssembler,
                thumbAssembler: this.thumbAssembler,
                logger: this.logger,
                getMaxPatchesAmount: () => this.getMaxPatchesAmount(),
                markPatchAsPermanent: (p) => this.markPatchAsPermanent(p),
                patch: (address, value, patchNumber) => {
                    const slot = patchNumber !== undefined ? patchNumber : overwrittenMethods.getIncrementalPatchNumber();
                    return this.patch(address, value, slot);
                },
                envelop: (...a) => this.envelop(...a),
                getCurrentlyLoaded: () => this.getCurrentlyLoaded(),
                getFreeMemoryMap: () => this.getFreeMemoryMap(),
                _log: (tag, e) => this._log(tag, e),
                getIncrementalPatchNumber: overwrittenMethods.getIncrementalPatchNumber,
                freePatch: overwrittenMethods.freePatch,
                allocate: overwrittenMethods.allocate,
                release: overwrittenMethods.release,
                log: overwrittenMethods.log,
                require: overwrittenMethods.require,
                unload: overwrittenMethods.unload,
            };`,
      'exploit-state Proxy replacement'
    )
    out = replaceOrThrow(
      out,
      `}, await assembler_1.Assembler.create(core_macros_1.CORE_MACROS, 'ARM'), await assembler_1.Assembler.create(core_macros_1.CORE_MACROS, 'THUMB'), logger);`,
      `}, null, null, logger);`,
      'exploit-state Assembler.create replacement'
    )
    return out
  })

  // Emit pre-assembled ForceTOCEdit
  const preassembledForceTocSource = `"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
exports.ForceTOCEdit = void 0;
const netmd_js_1 = require("netmd-js");
const utils_1 = require("netmd-js/dist/utils");
const exploit_1 = require("../exploit");
const usb_code_execution_1 = require("./usb-code-execution");

function toByteMap(rawMap) {
  const out = {};
  const keys = Object.keys(rawMap);
  for (let i = 0; i < keys.length; i++) {
    out[keys[i]] = new Uint8Array(rawMap[keys[i]]);
  }
  return out;
}

const PREASSEMBLED_LOWER = toByteMap(${JSON.stringify(forceTocTables.lowerMap)});
const PREASSEMBLED_TRIGGER = toByteMap(${JSON.stringify(forceTocTables.triggerMap)});
const PREASSEMBLED_RAISE = toByteMap(${JSON.stringify(forceTocTables.raiseMap)});
const PREASSEMBLED_SKIP_ADDR = ${JSON.stringify(forceTocTables.skipFlushAddrMap)};
const PREASSEMBLED_SKIP_VAL = toByteMap(${JSON.stringify(forceTocTables.skipFlushValMap)});
const PREASSEMBLED_POLL_STUB = toByteMap(${JSON.stringify(forceTocTables.pollStubMap)});
const PREASSEMBLED_POLL_MASK = ${JSON.stringify(forceTocTables.pollMaskMap)};
const PREASSEMBLED_POLL_ACTIVE_SIZE = ${JSON.stringify(forceTocTables.pollActiveSizeMap)};

const INITIAL_POLL_DELAY_MS = 1000;
const POLL_INTERVAL_MS = 500;
const CONSECUTIVE_IDLE_REQUIRED = 2;
const SETTLE_BUFFER_MS = 1000;
const MAX_WAIT_MS = 50000;
const MAX_CONSECUTIVE_POLL_ERRORS = 3;

function readU32LE(buf, off) {
  return ((buf[off] | (buf[off + 1] << 8) | (buf[off + 2] << 16) | (buf[off + 3] << 24)) >>> 0);
}

function nowMs() {
  const h = globalThis.__cartaNetMdHost;
  if (h && h.nowMs) return h.nowMs();
  return Date.now();
}

function reportTocWait(elapsedMs, maxMs, done) {
  const h = globalThis.__cartaNetMdHost;
  if (h && h.reportTocWait) h.reportTocWait(elapsedMs | 0, maxMs | 0, done ? 1 : 0);
}

class ForceTOCEdit extends exploit_1.Exploit {
    constructor() {
        super(...arguments);
        this._policies = {
            stateManagerUnpatchLegal: true,
        };
    }
    getPropertyStore() {
        return {};
    }
    async init() {
        await super.init();
        const ver = this.stateManager.device.versionCode;
        const skipAddr = PREASSEMBLED_SKIP_ADDR[ver];
        const skipVal = PREASSEMBLED_SKIP_VAL[ver];
        if (skipAddr !== undefined && skipVal) {
            await this.stateManager.patch(skipAddr, skipVal);
        }
    }
    async waitForTOCWriteCompletion(codeExecution) {
        const ver = this.stateManager.device.versionCode;
        const readStub = PREASSEMBLED_POLL_STUB[ver];
        const pendingFlagMask = PREASSEMBLED_POLL_MASK[ver];
        const activeStateSize = PREASSEMBLED_POLL_ACTIVE_SIZE[ver];
        if (!readStub || pendingFlagMask === undefined || activeStateSize === undefined) {
            await (0, utils_1.sleep)(MAX_WAIT_MS);
            return;
        }
        const startTime = nowMs();
        reportTocWait(0, MAX_WAIT_MS, 0);
        await (0, utils_1.sleep)(INITIAL_POLL_DELAY_MS);
        let idleCount = 0;
        let consecutiveErrors = 0;
        while (nowMs() - startTime < MAX_WAIT_MS) {
            try {
                const reply = await codeExecution.execute(readStub);
                if (!reply || reply.length < readStub.length) {
                    throw new Error('Truncated TOC status reply');
                }
                const pendingFlag = readU32LE(reply, 0x1c);
                const rawActiveState = readU32LE(reply, 0x20);
                const activeState = activeStateSize === 1 ? (rawActiveState & 0xff) : rawActiveState;
                consecutiveErrors = 0;
                const isBusy = ((pendingFlag & pendingFlagMask) !== 0) || (activeState !== 0);
                const elapsed = nowMs() - startTime;
                reportTocWait(elapsed, MAX_WAIT_MS, 0);
                if (isBusy) {
                    idleCount = 0;
                } else {
                    idleCount++;
                    if (idleCount >= CONSECUTIVE_IDLE_REQUIRED) {
                        await (0, utils_1.sleep)(SETTLE_BUFFER_MS);
                        reportTocWait(nowMs() - startTime, MAX_WAIT_MS, 1);
                        return;
                    }
                }
            } catch (err) {
                idleCount = 0;
                consecutiveErrors++;
                if (consecutiveErrors >= MAX_CONSECUTIVE_POLL_ERRORS) {
                    const remaining = MAX_WAIT_MS - (nowMs() - startTime);
                    if (remaining > 0) {
                        await (0, utils_1.sleep)(remaining);
                    }
                    reportTocWait(MAX_WAIT_MS, MAX_WAIT_MS, 1);
                    return;
                }
            }
            const remaining = MAX_WAIT_MS - (nowMs() - startTime);
            if (remaining <= 0) {
                break;
            }
            await (0, utils_1.sleep)(Math.min(POLL_INTERVAL_MS, remaining));
        }
        reportTocWait(MAX_WAIT_MS, MAX_WAIT_MS, 1);
    }
    async forceTOCEdit() {
        const ver = this.stateManager.device.versionCode;
        const lowerCode = PREASSEMBLED_LOWER[ver];
        const triggerCode = PREASSEMBLED_TRIGGER[ver];
        const raiseCode = PREASSEMBLED_RAISE[ver];
        if (!lowerCode || !triggerCode || !raiseCode) {
            throw new exploit_1.CompatibilityError('ForceTOCEdit is not preassembled for firmware version ' + ver);
        }
        const isTypeS = ver.startsWith('S');
        if (isTypeS)
            await (0, netmd_js_1.display)(this.factoryIface, 'TOCWRT', true);
        await this.stateManager.envelop(usb_code_execution_1.USBCodeExecution, async (codeExecution) => {
            await codeExecution.execute(lowerCode);
            await (0, utils_1.sleep)(1000);
            await codeExecution.execute(triggerCode);
            await this.waitForTOCWriteCompletion(codeExecution);
            await codeExecution.execute(raiseCode);
        });
        if (isTypeS)
            await this.factoryIface.setDisplayMode(netmd_js_1.DisplayMode.DEFAULT);
    }
}
exports.ForceTOCEdit = ForceTOCEdit;
ForceTOCEdit._name = 'ForceTOCEdit';
ForceTOCEdit.SUPPORTED_VERSIONS = Object.keys(PREASSEMBLED_TRIGGER);
`
  fs.writeFileSync(
    path.join(stagedDir, 'netmd-exploits', 'exploits', 'force-toc-edit.js'),
    preassembledForceTocSource,
    'utf8'
  )

  fs.writeFileSync(
    path.join(stagedDir, 'netmd-exploits', 'exploits', 'index.js'),
    `"use strict";
const { ForceTOCEdit } = require('./force-toc-edit');
const { USBCodeExecution } = require('./usb-code-execution');
const { KillEepromWrite } = require('./kill-eeprom-writes');
const { SPUpload } = require('./sp-upload');
const { MonoSPUpload } = require('./mono-sp-upload');
module.exports = { ForceTOCEdit, USBCodeExecution, KillEepromWrite, SPUpload, MonoSPUpload };
`,
    'utf8'
  )

  fs.writeFileSync(
    path.join(stagedDir, 'netmd-exploits', 'index.js'),
    `"use strict";
const state = require('./exploit-state');
const exploits = require('./exploits/index');
const safety = require('./safety');
const utils = require('./utils');
module.exports = {
  ...state,
  ...exploits,
  ...safety,
  ...utils,
};
`,
    'utf8'
  )

  // 3. Emit pure-JS shims for node_modules dependencies
  const { diacriticsMap } = requireFromDeps('diacritics')
  fs.writeFileSync(
    path.join(shimsDir, 'diacritics.js'),
    `const CHAR_MAP = ${JSON.stringify(diacriticsMap)};
export function remove(str) {
  const s = String(str == null ? '' : str);
  let out = '';
  for (let i = 0; i < s.length; i++) {
    const ch = s.charAt(i);
    out += CHAR_MAP[ch] || ch;
  }
  return out;
}
export default { remove };
`,
    'utf8'
  )

  fs.writeFileSync(
    path.join(shimsDir, 'util.js'),
    `export function inspect(v) { return String(v); }
export default { inspect };
`,
    'utf8'
  )

  fs.writeFileSync(
    path.join(shimsDir, 'buffer.js'),
    `function utf8EncodeSimple(str) {
  const s = String(str);
  const out = [];
  for (let i = 0; i < s.length; i++) {
    let cp = s.charCodeAt(i);
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < s.length) {
      const lo = s.charCodeAt(i + 1);
      if (lo >= 0xdc00 && lo <= 0xdfff) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
        i++;
      }
    }
    if (cp < 0x80) out.push(cp);
    else if (cp < 0x800) { out.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f)); }
    else if (cp < 0x10000) { out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f)); }
    else { out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f)); }
  }
  return new Uint8Array(out);
}
export const Buffer = {
  from(value) {
    if (typeof value === 'string') return utf8EncodeSimple(value);
    if (value instanceof Uint8Array) return new Uint8Array(value);
    if (value instanceof ArrayBuffer) return new Uint8Array(value);
    if (value && value.buffer instanceof ArrayBuffer && typeof value.byteLength === 'number') {
      return new Uint8Array(value.buffer, value.byteOffset || 0, value.byteLength);
    }
    return new Uint8Array(value || 0);
  },
  alloc(len) {
    return new Uint8Array(len | 0);
  },
  concat(list) {
    let total = 0;
    for (let i = 0; i < list.length; i++) total += list[i].length;
    const out = new Uint8Array(total);
    let off = 0;
    for (let i = 0; i < list.length; i++) {
      out.set(list[i], off);
      off += list[i].length;
    }
    return out;
  },
  isBuffer(x) {
    return x instanceof Uint8Array;
  }
};
export default { Buffer };
`,
    'utf8'
  )

  fs.writeFileSync(
    path.join(shimsDir, 'jconv.js'),
    `function utf8Encode(str) {
  const s = String(str == null ? '' : str);
  const out = [];
  for (let i = 0; i < s.length; i++) {
    let cp = s.charCodeAt(i);
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < s.length) {
      const lo = s.charCodeAt(i + 1);
      if (lo >= 0xdc00 && lo <= 0xdfff) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
        i++;
      } else {
        cp = 0xfffd;
      }
    } else if (cp >= 0xdc00 && cp <= 0xdfff) {
      cp = 0xfffd;
    }
    if (cp < 0x80) out.push(cp);
    else if (cp < 0x800) out.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f));
    else if (cp < 0x10000) out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
    else out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
  }
  return new Uint8Array(out);
}
function utf8Decode(bytes) {
  let out = '';
  let i = 0;
  while (i < bytes.length) {
    const b0 = bytes[i++];
    let cp = b0;
    let need = 0;
    if ((b0 & 0xe0) === 0xc0) { cp = b0 & 0x1f; need = 1; }
    else if ((b0 & 0xf0) === 0xe0) { cp = b0 & 0x0f; need = 2; }
    else if ((b0 & 0xf8) === 0xf0) { cp = b0 & 0x07; need = 3; }
    else if (b0 >= 0x80) { cp = 0xfffd; need = 0; }
    for (let k = 0; k < need; k++) {
      if (i >= bytes.length || (bytes[i] & 0xc0) !== 0x80) { cp = 0xfffd; break; }
      cp = (cp << 6) | (bytes[i++] & 0x3f);
    }
    if (cp <= 0xffff) out += String.fromCharCode(cp);
    else {
      cp -= 0x10000;
      out += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
    }
  }
  return out;
}
function encode(str, encoding) {
  const utf8 = utf8Encode(str);
  if (utf8.length === 0) return new Uint8Array(0);
  const host = globalThis.__cartaNetMdHost;
  if (!host || !host.cp932) throw new Error('Host CP932 encoder unavailable');
  return host.cp932(0, utf8);
}
function decode(buf, encoding) {
  const bytes = buf instanceof Uint8Array ? buf : new Uint8Array(buf || 0);
  if (bytes.length === 0) return '';
  const host = globalThis.__cartaNetMdHost;
  if (!host || !host.cp932) throw new Error('Host CP932 decoder unavailable');
  const utf8 = host.cp932(1, bytes);
  return utf8Decode(utf8);
}
export { encode, decode };
export default { encode, decode };
`,
    'utf8'
  )

  // Software 64-bit integer JSBI shim (no native BigInt needed in Porffor)
  fs.writeFileSync(
    path.join(shimsDir, 'jsbi.js'),
    `function JSBIValue(lo, hi) {
  this.lo = lo >>> 0;
  this.hi = hi | 0;
}
JSBIValue.prototype.toString = function (radix) {
  return JSBI.toNumber(this).toString(radix || 10);
};
function fromNum(n) {
  if (n instanceof JSBIValue) return n;
  const v = Number(n);
  if (!Number.isFinite(v) || v === 0) return new JSBIValue(0, 0);
  const neg = v < 0;
  const abs = Math.floor(Math.abs(v));
  let lo = (abs % 4294967296) >>> 0;
  let hi = Math.floor(abs / 4294967296) | 0;
  if (neg) {
    lo = ((~lo) + 1) >>> 0;
    hi = (~hi + (lo === 0 ? 1 : 0)) | 0;
  }
  return new JSBIValue(lo, hi);
}
function parseStr(s) {
  const t = String(s).trim();
  const neg = t.startsWith('-');
  const body = neg ? t.slice(1) : t;
  const base = body.startsWith('0x') || body.startsWith('0X') ? 16 : 10;
  const digits = base === 16 ? body.slice(2) : body;
  let lo = 0;
  let hi = 0;
  for (let i = 0; i < digits.length; i++) {
    const ch = digits.charCodeAt(i);
    let d = 0;
    if (ch >= 48 && ch <= 57) d = ch - 48;
    else if (ch >= 97 && ch <= 102) d = 10 + ch - 97;
    else if (ch >= 65 && ch <= 70) d = 10 + ch - 65;
    else break;
    // (hi:lo) * base + d
    const l0 = (lo & 0xffff) * base + d;
    const l1 = ((lo >>> 16) * base) + (l0 >>> 16);
    lo = ((l1 & 0xffff) << 16) | (l0 & 0xffff);
    hi = (Math.imul(hi, base) + (l1 >>> 16)) | 0;
  }
  if (neg) {
    lo = ((~lo) + 1) >>> 0;
    hi = (~hi + (lo === 0 ? 1 : 0)) | 0;
  }
  return new JSBIValue(lo, hi);
}
const JSBI = {
  BigInt(v) {
    if (v instanceof JSBIValue) return v;
    if (typeof v === 'string') return parseStr(v);
    return fromNum(v);
  },
  toNumber(x) {
    const j = fromNum(x);
    return j.hi * 4294967296 + j.lo;
  },
  leftShift(a, b) {
    const x = fromNum(a);
    const s = JSBI.toNumber(b) | 0;
    if (s <= 0) return x;
    if (s >= 64) return new JSBIValue(0, 0);
    if (s >= 32) return new JSBIValue(0, (x.lo << (s - 32)) | 0);
    return new JSBIValue((x.lo << s) >>> 0, ((x.hi << s) | (x.lo >>> (32 - s))) | 0);
  },
  signedRightShift(a, b) {
    const x = fromNum(a);
    const s = JSBI.toNumber(b) | 0;
    if (s <= 0) return x;
    if (s >= 64) return x.hi < 0 ? new JSBIValue(0xffffffff, -1) : new JSBIValue(0, 0);
    if (s >= 32) return new JSBIValue((x.hi >> (s - 32)) >>> 0, x.hi < 0 ? -1 : 0);
    return new JSBIValue(((x.lo >>> s) | (x.hi << (32 - s))) >>> 0, x.hi >> s);
  },
  bitwiseAnd(a, b) {
    const x = fromNum(a);
    const y = fromNum(b);
    return new JSBIValue((x.lo & y.lo) >>> 0, x.hi & y.hi);
  },
  bitwiseOr(a, b) {
    const x = fromNum(a);
    const y = fromNum(b);
    return new JSBIValue((x.lo | y.lo) >>> 0, x.hi | y.hi);
  },
};
export default JSBI;
export const BigInt = JSBI.BigInt;
export const toNumber = JSBI.toNumber;
export const leftShift = JSBI.leftShift;
export const signedRightShift = JSBI.signedRightShift;
export const bitwiseAnd = JSBI.bitwiseAnd;
export const bitwiseOr = JSBI.bitwiseOr;
`,
    'utf8'
  )

  // @originjs/crypto-js-wasm shim backed by host CommonCrypto 3DES/DES + RNG
  fs.writeFileSync(
    path.join(shimsDir, 'crypto-js-wasm.js'),
    `function bytesToWordArray(u8) {
  const bytes = u8 instanceof Uint8Array ? u8 : new Uint8Array(u8 || 0);
  const words = [];
  for (let i = 0; i < bytes.length; i += 4) {
    const w =
      ((bytes[i] || 0) << 24) |
      ((i + 1 < bytes.length ? bytes[i + 1] : 0) << 16) |
      ((i + 2 < bytes.length ? bytes[i + 2] : 0) << 8) |
      (i + 3 < bytes.length ? bytes[i + 3] : 0);
    words.push(w | 0);
  }
  return new WordArray(words, bytes.length);
}
function wordArrayToBytes(wa, forcedLen) {
  const len = forcedLen !== undefined ? forcedLen : wa.sigBytes;
  const out = new Uint8Array(len);
  const words = wa.words || [];
  let idx = 0;
  for (let i = 0; i < words.length && idx < len; i++) {
    const w = words[i];
    if (idx < len) out[idx++] = (w >>> 24) & 0xff;
    if (idx < len) out[idx++] = (w >>> 16) & 0xff;
    if (idx < len) out[idx++] = (w >>> 8) & 0xff;
    if (idx < len) out[idx++] = w & 0xff;
  }
  return out;
}
function bytesToHex(bytes) {
  let out = '';
  const hex = '0123456789abcdef';
  for (let i = 0; i < bytes.length; i++) {
    const b = bytes[i] & 0xff;
    out += hex.charAt(b >> 4) + hex.charAt(b & 15);
  }
  return out;
}
function hexToWordArray(str) {
  const clean = String(str || '').trim();
  const len = Math.floor(clean.length / 2);
  const bytes = new Uint8Array(len);
  for (let i = 0; i < len; i++) {
    bytes[i] = Number.parseInt(clean.slice(i * 2, i * 2 + 2), 16) & 0xff;
  }
  return bytesToWordArray(bytes);
}
function utf8BytesFromString(s) {
  const str = String(s || '');
  const out = [];
  for (let i = 0; i < str.length; i++) {
    let cp = str.charCodeAt(i);
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < str.length) {
      const lo = str.charCodeAt(i + 1);
      if (lo >= 0xdc00 && lo <= 0xdfff) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
        i++;
      }
    }
    if (cp < 0x80) out.push(cp);
    else if (cp < 0x800) out.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f));
    else if (cp < 0x10000) out.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
    else out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
  }
  return new Uint8Array(out);
}
function utf8StringFromBytes(bytes) {
  let out = '';
  let i = 0;
  while (i < bytes.length) {
    const b0 = bytes[i++];
    let cp = b0;
    let need = 0;
    if ((b0 & 0xe0) === 0xc0) { cp = b0 & 0x1f; need = 1; }
    else if ((b0 & 0xf0) === 0xe0) { cp = b0 & 0x0f; need = 2; }
    else if ((b0 & 0xf8) === 0xf0) { cp = b0 & 0x07; need = 3; }
    else if (b0 >= 0x80) { cp = 0xfffd; need = 0; }
    for (let k = 0; k < need; k++) {
      if (i >= bytes.length || (bytes[i] & 0xc0) !== 0x80) { cp = 0xfffd; break; }
      cp = (cp << 6) | (bytes[i++] & 0x3f);
    }
    if (cp <= 0xffff) out += String.fromCharCode(cp);
    else {
      cp -= 0x10000;
      out += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
    }
  }
  return out;
}
class WordArray {
  constructor(words, sigBytes) {
    this.words = words || [];
    this.sigBytes = sigBytes !== undefined ? sigBytes : this.words.length * 4;
  }
  toString(encoder) {
    const bytes = wordArrayToBytes(this);
    if (encoder && encoder._kind === 'Utf8') return utf8StringFromBytes(bytes);
    return bytesToHex(bytes);
  }
}
WordArray.create = function (v) {
  if (v instanceof WordArray) return v;
  if (v instanceof Uint8Array) return bytesToWordArray(v);
  if (v instanceof ArrayBuffer) return bytesToWordArray(new Uint8Array(v));
  if (Array.isArray(v)) {
    const sig = v.length * 4;
    return new WordArray(v.slice(), sig);
  }
  if (v && v.buffer instanceof ArrayBuffer) {
    return bytesToWordArray(new Uint8Array(v.buffer, v.byteOffset || 0, v.byteLength));
  }
  return new WordArray([], 0);
};
WordArray.random = function (nBytes) {
  const host = globalThis.__cartaNetMdHost;
  if (!host || !host.randomBytes) throw new Error('Host randomBytes unavailable');
  return bytesToWordArray(host.randomBytes(nBytes | 0));
};
const enc = {
  Hex: {
    _kind: 'Hex',
    parse: hexToWordArray,
    stringify(wa) { return bytesToHex(wordArrayToBytes(wa)); },
  },
  Utf8: {
    _kind: 'Utf8',
    parse(s) { return bytesToWordArray(utf8BytesFromString(s)); },
    stringify(wa) { return utf8StringFromBytes(wordArrayToBytes(wa)); },
  },
};
const mode = { ECB: 'ECB', CBC: 'CBC' };
const pad = { Pkcs7: 'Pkcs7', NoPadding: 'NoPadding', ZeroPadding: 'ZeroPadding' };
function extractWordArray(input, isCipherObj) {
  if (isCipherObj && input && input.ciphertext) return WordArray.create(input.ciphertext);
  if (input && input.words && typeof input.sigBytes === 'number') return input;
  return WordArray.create(input);
}
function runCipher(isTriple, isEncrypt, dataOrParams, keyArg, cfg) {
  const host = globalThis.__cartaNetMdHost;
  if (!host || !host.desCrypt) throw new Error('Host desCrypt unavailable');
  const wa = extractWordArray(dataOrParams, !isEncrypt);
  const rawIn = wordArrayToBytes(wa);
  const keyWa = extractWordArray(keyArg, false);
  const keyBytes = wordArrayToBytes(keyWa);
  const isCbc = cfg && cfg.mode === 'CBC' ? 1 : 0;
  let ivBytes = null;
  if (isCbc) {
    const ivWa = cfg.iv ? extractWordArray(cfg.iv, false) : new WordArray([0, 0], 8);
    ivBytes = wordArrayToBytes(ivWa, 8);
  }
  const padMode = (cfg && cfg.padding) ? cfg.padding : 'Pkcs7';
  if (isEncrypt) {
    let padded;
    if (padMode === 'Pkcs7') {
      const padLen = 8 - (rawIn.length % 8);
      padded = new Uint8Array(rawIn.length + padLen);
      padded.set(rawIn, 0);
      for (let i = rawIn.length; i < padded.length; i++) padded[i] = padLen;
    } else {
      const aligned = Math.ceil(rawIn.length / 8) * 8;
      padded = new Uint8Array(aligned);
      padded.set(rawIn, 0);
    }
    const out = padded.length > 0 ? host.desCrypt(isTriple ? 1 : 0, 1, isCbc, keyBytes, ivBytes, padded) : new Uint8Array(0);
    return { ciphertext: bytesToWordArray(out) };
  }
  const alignedLen = Math.floor(rawIn.length / 8) * 8;
  const alignedIn = alignedLen === rawIn.length ? rawIn : rawIn.subarray(0, alignedLen);
  const out = alignedIn.length > 0 ? host.desCrypt(isTriple ? 1 : 0, 0, isCbc, keyBytes, ivBytes, alignedIn) : new Uint8Array(0);
  const res = bytesToWordArray(out);
  if (padMode === 'Pkcs7' && out.length > 0 && out.length % 8 === 0) {
    const p = out[out.length - 1];
    if (p >= 1 && p <= 8 && out.length >= p) {
      let ok = true;
      for (let i = out.length - p; i < out.length; i++) {
        if (out[i] !== p) { ok = false; break; }
      }
      if (ok) res.sigBytes = out.length - p;
    }
  }
  return res;
}
const DES = {
  encrypt(plain, key, cfg) { return runCipher(false, true, plain, key, cfg); },
  decrypt(cipher, key, cfg) { return runCipher(false, false, cipher, key, cfg); },
  loadWasm() { return Promise.resolve(); },
};
const TripleDES = {
  encrypt(plain, key, cfg) { return runCipher(true, true, plain, key, cfg); },
  decrypt(cipher, key, cfg) { return runCipher(true, false, cipher, key, cfg); },
  loadWasm() { return Promise.resolve(); },
};
const algo = { DES, TripleDES };
const CryptoJS = { lib: { WordArray }, enc, mode, pad, DES, TripleDES, algo };
export { DES, TripleDES, algo, enc, mode, pad };
export default CryptoJS;
`,
    'utf8'
  )
}

// ---------------------------------------------------------------------------
// Async-to-SyncPromise bundling, lowering, and Node sanity self-test
// ---------------------------------------------------------------------------

/**
 * Emit the NetMD core entry with synchronous `Promise`/`__cartaSyncAwaiter`
 * polyfills, bundle dependencies via esbuild, and lower all `async`/`await`
 * functions into synchronous `__syncAwait`/`__syncResolve` calls via Babel.
 *
 * @param {typeof import('@babel/core')} babel - Staged Babel compiler.
 * @param {typeof import('esbuild')} esbuild - Staged esbuild bundler.
 * @returns {Promise<string>} Path to the lowered ESM bundle (`netmd_lowered.mjs`).
 */
async function writeCoreEntryAndBundle(babel, esbuild) {
  const coreEntryPath = path.join(workDir, 'netmd_core_entry.js')
  const bundledPath = path.join(workDir, 'netmd_bundled.mjs')
  const loweredPath = path.join(workDir, 'netmd_lowered.mjs')

  const coreEntryCode = `
// Install synchronous Promise & Symbol.asyncIterator BEFORE NetMD modules evaluate.
if (typeof Symbol !== 'undefined' && !Symbol.asyncIterator) {
  Symbol.asyncIterator = Symbol.for('Symbol.asyncIterator');
}
let __promiseJobs = [];
let __promiseJobHead = 0;
let __drainingPromiseJobs = false;
function __resetPromiseJobs() {
  __promiseJobs = [];
  __promiseJobHead = 0;
  __drainingPromiseJobs = false;
}
function __maybeCompactPromiseJobs() {
  if (__promiseJobHead >= __promiseJobs.length) {
    __promiseJobs.length = 0;
    __promiseJobHead = 0;
  } else if (__promiseJobHead > 64 && __promiseJobHead * 2 >= __promiseJobs.length) {
    __promiseJobs = __promiseJobs.slice(__promiseJobHead);
    __promiseJobHead = 0;
  }
}
function __drainPromiseJobs() {
  if (__drainingPromiseJobs) return;
  __drainingPromiseJobs = true;
  try {
    while (__promiseJobHead < __promiseJobs.length) {
      const job = __promiseJobs[__promiseJobHead];
      __promiseJobs[__promiseJobHead] = undefined;
      __promiseJobHead++;
      __maybeCompactPromiseJobs();
      if (job) job();
    }
  } finally {
    __drainingPromiseJobs = false;
  }
}
function __enqueuePromiseJob(fn) {
  __promiseJobs.push(fn);
  if (!__drainingPromiseJobs) __drainPromiseJobs();
}

class __CartaSyncPromise {
  constructor(executor) {
    this.__isSyncPromise = true;
    this._state = 'pending';
    this._value = undefined;
    this._waiters = [];
    const self = this;
    function settle(state, val) {
      if (self._state !== 'pending') return;
      if (state === 'fulfilled' && val && val.__isSyncPromise) {
        if (val._state === 'pending') {
          val.then(
            (inner) => settle('fulfilled', inner),
            (err) => settle('rejected', err)
          );
          return;
        }
        self._state = val._state;
        self._value = val._value;
      } else {
        self._state = state;
        self._value = val;
      }
      if (self._waiters.length > 0) {
        const waiters = self._waiters;
        self._waiters = [];
        for (let i = 0; i < waiters.length; i++) {
          const w = waiters[i];
          const st = self._state;
          const v = self._value;
          __promiseJobs.push(() => w(st, v));
        }
        if (!__drainingPromiseJobs) __drainPromiseJobs();
      }
    }
    try {
      executor(
        (v) => settle('fulfilled', v),
        (e) => settle('rejected', e)
      );
    } catch (e) {
      settle('rejected', e);
    }
  }
  then(onFulfilled, onRejected) {
    if (this._state === 'fulfilled' && typeof onFulfilled !== 'function') return this;
    if (this._state === 'rejected' && typeof onRejected !== 'function') return this;
    const self = this;
    return new __CartaSyncPromise((resolve, reject) => {
      function runReaction(st, val) {
        if (st === 'fulfilled') {
          if (typeof onFulfilled !== 'function') resolve(val);
          else {
            try { resolve(onFulfilled(val)); } catch (e) { reject(e); }
          }
        } else {
          if (typeof onRejected !== 'function') reject(val);
          else {
            try { resolve(onRejected(val)); } catch (e) { reject(e); }
          }
        }
      }
      if (self._state !== 'pending') {
        const st = self._state;
        const val = self._value;
        __enqueuePromiseJob(() => runReaction(st, val));
      } else {
        self._waiters.push(runReaction);
      }
    });
  }
  catch(onRejected) {
    return this.then(undefined, onRejected);
  }
  finally(cb) {
    if (typeof cb !== 'function') return this;
    return this.then(
      (v) => { cb(); return v; },
      (e) => { cb(); throw e; }
    );
  }
  static resolve(v) {
    if (v && v.__isSyncPromise) return v;
    return new __CartaSyncPromise((res) => res(v));
  }
  static reject(e) {
    return new __CartaSyncPromise((_, rej) => rej(e));
  }
  static all(arr) {
    const out = [];
    for (let i = 0; i < arr.length; i++) {
      out.push(__syncAwait(arr[i]));
    }
    return __CartaSyncPromise.resolve(out);
  }
}
globalThis.__CartaSyncPromise = __CartaSyncPromise;
globalThis.Promise = __CartaSyncPromise;
function __syncResolve(v) { return __CartaSyncPromise.resolve(v); }
function __syncReject(e) { return __CartaSyncPromise.reject(e); }
function __syncAwait(v) {
  if (v && v.__isSyncPromise) {
    while (v._state === 'pending' && __promiseJobHead < __promiseJobs.length) {
      const job = __promiseJobs[__promiseJobHead];
      __promiseJobs[__promiseJobHead] = undefined;
      __promiseJobHead++;
      __maybeCompactPromiseJobs();
      if (job) job();
    }
    if (v._state === 'rejected') throw v._value;
    if (v._state === 'pending') throw new Error('Pending SyncPromise encountered');
    return v._value;
  }
  if (v && typeof v.then === 'function') {
    let settled = false;
    let failed = false;
    let out = undefined;
    v.then(
      (res) => { settled = true; out = res; },
      (err) => { settled = true; failed = true; out = err; }
    );
    while (!settled && __promiseJobHead < __promiseJobs.length) {
      const job = __promiseJobs[__promiseJobHead];
      __promiseJobs[__promiseJobHead] = undefined;
      __promiseJobHead++;
      __maybeCompactPromiseJobs();
      if (job) job();
    }
    if (!settled) throw new Error('Pending thenable encountered in synchronous runtime');
    if (failed) throw out;
    return out;
  }
  return v;
}
function __cartaSyncAwaiter(thisArg, _arguments, P, generator) {
  try {
    const gen = generator.apply(thisArg, _arguments || []);
    let nextVal = undefined;
    let errVal = undefined;
    let isErr = false;
    while (true) {
      let res;
      if (isErr) {
        if (typeof gen['throw'] !== 'function') return __CartaSyncPromise.reject(errVal);
        res = gen['throw'](errVal);
      } else {
        res = gen.next(nextVal);
      }
      if (res.done) return __CartaSyncPromise.resolve(res.value);
      try {
        nextVal = __syncAwait(res.value);
        isErr = false;
      } catch (e) {
        errVal = e;
        isErr = true;
      }
    }
  } catch (e) {
    return __CartaSyncPromise.reject(e);
  }
}
globalThis.__syncResolve = __syncResolve;
globalThis.__syncReject = __syncReject;
globalThis.__syncAwait = __syncAwait;
globalThis.__cartaSyncAwaiter = __cartaSyncAwaiter;
globalThis.__resetPromiseJobs = __resetPromiseJobs;

import * as netmdJs from ${JSON.stringify(path.join(stagedDir, 'netmd-js', 'index.js'))};
import * as netmdExploits from ${JSON.stringify(path.join(stagedDir, 'netmd-exploits', 'index.js'))};
import * as jconvMod from ${JSON.stringify(path.join(shimsDir, 'jconv.js'))};

const {
  NetMD,
  NetMDInterface,
  Wireformat,
  DiscFormat,
  DiscFlag,
  MDTrack,
  MDSession,
  getAsyncPacketIterator,
  getDeviceStatus,
  listContent,
  download,
  readUTOCSector,
  writeUTOCSector,
  getDescriptiveDeviceCode,
} = netmdJs;

const {
  ExploitStateManager,
  KillEepromWrite,
  SPUpload,
  ForceTOCEdit,
  getFromVersionStoreOrNull,
} = netmdExploits;

function hostOrThrow() {
  const h = globalThis.__cartaNetMdHost;
  if (!h) throw new Error('NetMD host bridge not initialized');
  return h;
}

function reportHostStep(done, total) {
  const h = globalThis.__cartaNetMdHost;
  if (h && h.reportStep) h.reportStep(done | 0, total | 0);
}

function toHex4(n) {
  const hex = '0123456789abcdef';
  return (
    hex.charAt((n >>> 12) & 15) +
    hex.charAt((n >>> 8) & 15) +
    hex.charAt((n >>> 4) & 15) +
    hex.charAt(n & 15)
  );
}

function asciiJsonString(str) {
  const s = String(str);
  let out = '"';
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c === 34) out += '\\\\"';
    else if (c === 92) out += '\\\\\\\\';
    else if (c === 8) out += '\\\\b';
    else if (c === 12) out += '\\\\f';
    else if (c === 10) out += '\\\\n';
    else if (c === 13) out += '\\\\r';
    else if (c === 9) out += '\\\\t';
    else if (c < 32 || c >= 127) out += '\\\\u' + toHex4(c);
    else out += s.charAt(i);
  }
  return out + '"';
}

function asciiJsonStringify(value) {
  if (value === null) return 'null';
  const t = typeof value;
  if (t === 'boolean') return value ? 'true' : 'false';
  if (t === 'number') return Number.isFinite(value) ? String(value) : 'null';
  if (t === 'string') return asciiJsonString(value);
  if (Array.isArray(value)) {
    let out = '[';
    for (let i = 0; i < value.length; i++) {
      if (i > 0) out += ',';
      const v = value[i];
      out += v === undefined || typeof v === 'function' ? 'null' : asciiJsonStringify(v);
    }
    return out + ']';
  }
  if (t === 'object') {
    const keys = Object.keys(value);
    let out = '{';
    let first = true;
    for (let i = 0; i < keys.length; i++) {
      const k = keys[i];
      const v = value[k];
      if (v === undefined || typeof v === 'function') continue;
      if (!first) out += ',';
      first = false;
      out += asciiJsonString(k) + ':' + asciiJsonStringify(v);
    }
    return out + '}';
  }
  return 'null';
}

class HostUSBDevice {
  constructor(info) {
    this.deviceId = info.id;
    this.vendorId = info.vendorId | 0;
    this.productId = info.productId | 0;
    this.productName = info.name || '';
    this.manufacturerName = '';
    this.opened = false;
    this.handle = -1;
  }
  async open() {
    if (this.opened && this.handle >= 0) return;
    this.handle = hostOrThrow().usbOpen(this.deviceId);
    this.opened = true;
  }
  async close() {
    if (this.handle >= 0) {
      const h = this.handle;
      this.handle = -1;
      this.opened = false;
      hostOrThrow().usbClose(h);
    }
  }
  async reset() {}
  async selectConfiguration() {}
  async claimInterface() {}
  async releaseInterface() {}
  async controlTransferOut(setup, data) {
    const bytes =
      data instanceof Uint8Array
        ? data
        : data instanceof ArrayBuffer
        ? new Uint8Array(data)
        : data && data.buffer instanceof ArrayBuffer
        ? new Uint8Array(data.buffer, data.byteOffset || 0, data.byteLength)
        : new Uint8Array(data || 0);
    const written = hostOrThrow().usbControlOut(
      this.handle,
      setup.request | 0,
      setup.value | 0,
      setup.index | 0,
      bytes
    );
    return { status: 'ok', bytesWritten: written };
  }
  async controlTransferIn(setup, length) {
    const buf = hostOrThrow().usbControlIn(
      this.handle,
      setup.request | 0,
      setup.value | 0,
      setup.index | 0,
      length | 0
    );
    const exact = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
    return { status: 'ok', data: new DataView(exact) };
  }
  async transferOut(endpoint, data) {
    const bytes =
      data instanceof Uint8Array
        ? data
        : data instanceof ArrayBuffer
        ? new Uint8Array(data)
        : data && data.buffer instanceof ArrayBuffer
        ? new Uint8Array(data.buffer, data.byteOffset || 0, data.byteLength)
        : new Uint8Array(data || 0);
    const written = hostOrThrow().usbBulkOut(this.handle, endpoint | 0, bytes);
    return { status: 'ok', bytesWritten: written };
  }
  async transferIn(endpoint, length) {
    const buf = hostOrThrow().usbBulkIn(this.handle, endpoint | 0, length | 0);
    const exact = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
    return { status: 'ok', data: new DataView(exact) };
  }
}

function findDeviceById(deviceId) {
  const listJson = hostOrThrow().usbListJson();
  const list = JSON.parse(listJson);
  for (let i = 0; i < list.length; i++) {
    if (list[i].id === deviceId) return list[i];
  }
  throw new Error('NetMD device not found: ' + deviceId);
}

async function openNetMdSession(deviceId) {
  const info = findDeviceById(deviceId);
  const dev = new HostUSBDevice(info);
  const netMd = new NetMD(dev, 0);
  await netMd.init();
  const iface = new NetMDInterface(netMd);
  return { info, dev, netMd, iface };
}

function bcdTimeArrayToSeconds(arr) {
  if (!arr || arr.length < 3) return 0;
  return (Number(arr[0]) || 0) * 3600 + (Number(arr[1]) || 0) * 60 + (Number(arr[2]) || 0);
}

const SP_UPLOAD_VERSIONS = [
  'S1.000', 'S1.100', 'S1.200', 'S1.300', 'S1.400', 'S1.500', 'S1.600',
  'R1.000', 'R1.100', 'R1.200', 'R1.300', 'R1.400',
  'Hn1.000', 'Hn1.100', 'Hn1.10A', 'Hn1.200', 'Hn1.20A',
  'Hr1.000', 'Hp1.000',
  'Hx1.040', 'Hx1.060', 'Hx1.070', 'Hx1.080', 'Hx1.090', 'Hx1.0A0',
];

const MAX_AVC_PROBE_TRACKS = 40;
function isUsbTransportError(err) {
  const msg = err && err.message ? String(err.message) : String(err || '');
  return msg.indexOf('USB ') === 0 || msg.indexOf('LIBUSB_') !== -1 ||
         msg.indexOf('Timed out waiting for NetMD reply') !== -1;
}

export async function netmdProbeDevice(deviceId, includeTracks) {
  const wantTracks = includeTracks === undefined ? true : Boolean(includeTracks);
  const { info, netMd, iface } = await openNetMdSession(deviceId);
  try {
    let versionCode = '';
    try {
      const factoryIface = await iface.factory();
      versionCode = await getDescriptiveDeviceCode(factoryIface);
    } catch (e) {
      if (isUsbTransportError(e)) throw e;
      versionCode = '';
    }
    const status = await getDeviceStatus(iface);
    const supportsSpUpload = SP_UPLOAD_VERSIONS.includes(versionCode);
    const supportsForceTocEdit = ForceTOCEdit.SUPPORTED_VERSIONS.includes(versionCode);

    let writable = false;
    let writeProtected = false;
    let trackCount = 0;
    let usedSeconds = 0;
    let totalSeconds = 0;
    let leftSeconds = 0;
    let discTitle = '';
    let fullWidthTitle = '';
    let tracks = [];

    if (status.discPresent) {
      const flags = await iface.getDiscFlags();
      writable = (flags & DiscFlag.writable) !== 0;
      writeProtected = (flags & DiscFlag.writeProtected) !== 0;
      trackCount = await iface.getTrackCount();
      try {
        const cap = await iface.getDiscCapacity();
        usedSeconds = bcdTimeArrayToSeconds(cap[0]);
        totalSeconds = bcdTimeArrayToSeconds(cap[1]);
        leftSeconds = bcdTimeArrayToSeconds(cap[2]);
      } catch (e) {
        if (isUsbTransportError(e)) throw e;
      }
      try { discTitle = await iface.getDiscTitle(false); } catch (e) {
        if (isUsbTransportError(e)) throw e;
      }
      try { fullWidthTitle = await iface.getDiscTitle(true); } catch (e) {
        if (isUsbTransportError(e)) throw e;
      }
      if (wantTracks && discTitle !== 'Hi-MD Disc' && trackCount > 0 && trackCount <= MAX_AVC_PROBE_TRACKS) {
        for (let t = 0; t < trackCount; t++) {
          let hw = '';
          let fw = '';
          let dur = 0;
          let codec = 'SP';
          try { hw = await iface.getTrackTitle(t, false); } catch (e) {
            if (isUsbTransportError(e)) throw e;
          }
          try { fw = await iface.getTrackTitle(t, true); } catch (e) {
            if (isUsbTransportError(e)) throw e;
          }
          try {
            const lenArr = await iface.getTrackLength(t);
            dur = bcdTimeArrayToSeconds(lenArr);
          } catch (e) {
            if (isUsbTransportError(e)) throw e;
          }
          try {
            const encInfo = await iface.getTrackEncoding(t);
            const enc = Array.isArray(encInfo) ? encInfo[0] : 0;
            const ch = Array.isArray(encInfo) ? encInfo[1] : 0;
            if (enc === 0x92) codec = 'LP2';
            else if (enc === 0x93) codec = 'LP4';
            else if (ch === 0x01) codec = 'Mono';
            else codec = 'SP';
          } catch (e) {
            if (isUsbTransportError(e)) throw e;
          }
          tracks.push({
            index: t,
            title: hw || '',
            fullWidthTitle: fw || '',
            codec,
            durationSeconds: dur,
          });
        }
      }
    }

    return asciiJsonStringify({
      deviceId: info.id,
      deviceName: netMd.getDeviceName() || info.name || 'NetMD',
      versionCode,
      supportsSpUpload,
      supportsForceTocEdit,
      discPresent: Boolean(status.discPresent),
      writable,
      writeProtected,
      trackCount,
      usedSeconds,
      totalSeconds,
      leftSeconds,
      discTitle,
      fullWidthTitle,
      tracks,
    });
  } finally {
    await netMd.finalize();
  }
}

export async function netmdEraseDisc(deviceId) {
  const { netMd, iface } = await openNetMdSession(deviceId);
  try {
    await iface.acquire();
    try {
      await iface.eraseDisc();
    } finally {
      await iface.release();
    }
  } finally {
    await netMd.finalize();
  }
}

export async function netmdUploadTrack(deviceId, modeCode, wireBytes, hwTitle, fwTitle) {
  const { netMd, iface } = await openNetMdSession(deviceId);
  try {
    const rawBuf = (wireBytes.byteOffset === 0 && wireBytes.byteLength === wireBytes.buffer.byteLength)
      ? wireBytes.buffer
      : wireBytes.buffer.slice(wireBytes.byteOffset, wireBytes.byteOffset + wireBytes.byteLength);
    if (modeCode === 0 || modeCode === 1) {
      const factoryIface = await iface.factory();
      const esm = await ExploitStateManager.create(iface, factoryIface);
      if (!SP_UPLOAD_VERSIONS.includes(esm.device.versionCode)) {
        throw new Error('Raw ATRAC1 SP upload requires Type-S, Type-R, or supported Hi-MD firmware (got ' + esm.device.versionCode + ')');
      }
      let uploadedIdx = -1;
      // KillEepromWrite is only supported on Type-S/R and on Type-R blocks
      // SPI EEPROM access used by sendKeyData, so only load it on Type-S.
      const useKillEeprom = esm.device.versionCode.startsWith('S');
      if (useKillEeprom) {
        await esm.require(KillEepromWrite);
      }
      try {
        const ch = modeCode === 1 ? 1 : 2;
        await esm.envelop(SPUpload, ch, async (spUpload) => {
          const baseTrack = new MDTrack(
            hwTitle || '',
            Wireformat.pcm,
            rawBuf,
            0x10000,
            fwTitle && fwTitle.length > 0 ? fwTitle : undefined,
            getAsyncPacketIterator
          );
          const prepared = spUpload.prepareTrack(baseTrack);
          const [trkIdx] = await download(iface, prepared);
          uploadedIdx = Number(trkIdx);
        });
      } finally {
        if (useKillEeprom) {
          await esm.unload(KillEepromWrite);
        }
      }
      return uploadedIdx;
    }

    if (modeCode === 2 || modeCode === 3) {
      const wf = modeCode === 2 ? Wireformat.lp2 : Wireformat.lp4;
      const track = new MDTrack(
        hwTitle || '',
        wf,
        rawBuf,
        0x10000,
        fwTitle && fwTitle.length > 0 ? fwTitle : undefined,
        getAsyncPacketIterator
      );
      const [trkIdx] = await download(iface, track);
      return Number(trkIdx);
    }

    throw new Error('Unsupported NetMD upload mode code: ' + modeCode);
  } finally {
    await netMd.finalize();
  }
}

export async function netmdReadUtocSectors(deviceId) {
  const { netMd, iface } = await openNetMdSession(deviceId);
  try {
    const factoryIface = await iface.factory();
    const combined = new Uint8Array(5 * 2352);
    for (let s = 0; s < 5; s++) {
      const sec = await readUTOCSector(factoryIface, s);
      combined.set(sec, s * 2352);
      reportHostStep(s + 1, 5);
    }
    return combined;
  } finally {
    await netMd.finalize();
  }
}

export async function netmdWriteUtocSectors(deviceId, sectors5, sectorsMask) {
  if (!sectors5 || sectors5.length !== 5 * 2352) {
    throw new Error('Expected 11760 bytes (5 UTOC sectors)');
  }
  const { netMd, iface } = await openNetMdSession(deviceId);
  try {
    const factoryIface = await iface.factory();
    const esm = await ExploitStateManager.create(iface, factoryIface);
    if (!ForceTOCEdit.SUPPORTED_VERSIONS.includes(esm.device.versionCode)) {
      throw new Error('ForceTOCEdit exploit unsupported on firmware ' + esm.device.versionCode);
    }
    const useKillEeprom = esm.device.versionCode.startsWith('S') || esm.device.versionCode.startsWith('R');
    if (useKillEeprom) {
      await esm.require(KillEepromWrite);
    }
    try {
      for (let s = 0; s < 5; s++) {
        if ((sectorsMask & (1 << s)) !== 0) {
          const slice = sectors5.slice(s * 2352, (s + 1) * 2352);
          await writeUTOCSector(factoryIface, s, slice);
        }
        reportHostStep(s + 1, 10);
      }
      await esm.envelop(ForceTOCEdit, async (fte) => {
        await fte.forceTOCEdit();
      });
    } finally {
      if (useKillEeprom) {
        await esm.unload(KillEepromWrite);
      }
    }
  } finally {
    await netMd.finalize();
  }
}

export { jconvMod, netmdJs };
`
  fs.writeFileSync(coreEntryPath, coreEntryCode, 'utf8')

  const aliasPlugin = {
    name: 'netmd-porffor-aliases',
    setup(build) {
      const exactMap = {
        'netmd-js': path.join(stagedDir, 'netmd-js', 'index.js'),
        'netmd-js/dist/utils': path.join(stagedDir, 'netmd-js', 'utils.js'),
        '@originjs/crypto-js-wasm': path.join(shimsDir, 'crypto-js-wasm.js'),
        jconv: path.join(shimsDir, 'jconv.js'),
        buffer: path.join(shimsDir, 'buffer.js'),
        diacritics: path.join(shimsDir, 'diacritics.js'),
        util: path.join(shimsDir, 'util.js'),
        jsbi: path.join(shimsDir, 'jsbi.js'),
      }
      build.onResolve({ filter: /.*/ }, (args) => {
        if (exactMap[args.path]) {
          return { path: exactMap[args.path] }
        }
        return undefined
      })
    },
  }

  await esbuild.build({
    entryPoints: [coreEntryPath],
    outfile: bundledPath,
    bundle: true,
    format: 'esm',
    platform: 'neutral',
    target: ['es2020'],
    mainFields: ['module', 'main'],
    plugins: [aliasPlugin],
    write: true,
    logLevel: 'silent',
  })

  // Lower all remaining `async`/`await` cleanly into SyncPromise synchronous returns + calls.
  const t = babel.types
  const bundledSource = fs.readFileSync(bundledPath, 'utf8')
  const lowered = babel.transformSync(bundledSource, {
    babelrc: false,
    configFile: false,
    plugins: [
      () => ({
        visitor: {
          VariableDeclarator(vp) {
            const { id, init } = vp.node
            if (
              t.isIdentifier(id) &&
              t.isClassExpression(init) &&
              t.isIdentifier(init.id)
            ) {
              const innerName = init.id.name
              const outerName = id.name
              const binding = vp.get('init').scope.getBinding(innerName)
              if (binding) {
                for (const refPath of binding.referencePaths) {
                  if (t.isIdentifier(refPath.node)) {
                    refPath.node.name = outerName
                  }
                }
              }
              init.id = null
            }
          },
          Identifier(ip) {
            if (ip.node.name === 'Promise' && ip.isReferencedIdentifier()) {
              ip.node.name = '__CartaSyncPromise'
            }
          },
          Function: {
            exit(p) {
              if (!p.node.async || p.node.generator) return
              if (!t.isBlockStatement(p.node.body)) {
                p.node.body = t.blockStatement([t.returnStatement(p.node.body)])
                p.node.expression = false
              }
              p.get('body').traverse({
                Function(inner) {
                  inner.skip()
                },
                AwaitExpression: {
                  exit(ap) {
                    ap.replaceWith(
                      t.callExpression(t.identifier('__syncAwait'), [
                        ap.node.argument,
                      ])
                    )
                  },
                },
                ReturnStatement(rp) {
                  rp.node.argument = t.callExpression(
                    t.identifier('__syncResolve'),
                    [rp.node.argument || t.identifier('undefined')]
                  )
                },
              })
              p.node.async = false
              const stmts = p.node.body.body
              p.node.body = t.blockStatement([
                t.tryStatement(
                  t.blockStatement([
                    ...stmts,
                    t.returnStatement(
                      t.callExpression(t.identifier('__syncResolve'), [
                        t.identifier('undefined'),
                      ])
                    ),
                  ]),
                  t.catchClause(
                    t.identifier('__syncErr'),
                    t.blockStatement([
                      t.returnStatement(
                        t.callExpression(t.identifier('__syncReject'), [
                          t.identifier('__syncErr'),
                        ])
                      ),
                    ])
                  )
                ),
              ])
            },
          },
        },
      }),
    ],
  }).code

  const forbidden =
    /\bnew\s+Proxy\b|\bnew\s+RegExp\b|\bnew\s+Function\b|\beval\s*\(|\basync\s+function\b|=\s*class\s+(?!extends\b)[A-Za-z_$]/
  const badMatch = lowered.match(forbidden)
  if (badMatch) {
    throw new Error(
      `[netmd] Forbidden construct remained in lowered bundle: ${badMatch[0]}`
    )
  }

  fs.writeFileSync(loweredPath, lowered, 'utf8')
  return loweredPath
}

/**
 * Verify the lowered NetMD bundle under Node.js by testing `retailmac` crypto
 * parity and O(1) stack depth across 5,000 chained `__CartaSyncPromise` steps.
 *
 * @param {string} loweredPath - Path to `netmd_lowered.mjs`.
 */
async function runNodeSanitySelfTest(loweredPath) {
  const selfTestPath = path.join(workDir, 'node_sanity_test.mjs')
  const selfTestCode = `
import { createCipheriv, createDecipheriv, randomBytes } from 'node:crypto'
import * as mod from ${JSON.stringify(pathToFileURL(loweredPath).href)}
globalThis.__cartaNetMdHost = {
  randomBytes: (n) => new Uint8Array(randomBytes(n)),
  desCrypt: (isTriple, isEncrypt, isCbc, key, iv, input) => {
    const alg = isTriple
      ? (isCbc ? 'des-ede3-cbc' : 'des-ede3-ecb')
      : (isCbc ? 'des-cbc' : 'des-ecb')
    let k = key
    if (isTriple && key.length === 16) {
      k = Buffer.concat([key, key.subarray(0, 8)])
    }
    const ivArg = isCbc ? iv : null
    const c = isEncrypt
      ? createCipheriv(alg, k, ivArg)
      : createDecipheriv(alg, k, ivArg)
    c.setAutoPadding(false)
    return new Uint8Array(Buffer.concat([c.update(input), c.final()]))
  },
}
const key = new Uint8Array(16).fill(0x11)
const val = new Uint8Array(16).fill(0x22)
const mac = mod.netmdJs.retailmac(key, val)
if (typeof mac !== 'string' || mac.length !== 16) {
  throw new Error('Node self-test retailmac unexpected result: ' + mac)
}
// Verify O(1) stack depth across 5000 iterative awaiter + SyncPromise steps.
let deepestStackLines = 0
const deepRes = mod.globalThisCheckAwaiter
  ? mod.globalThisCheckAwaiter()
  : globalThis.__cartaSyncAwaiter(undefined, [], undefined, function* () {
      let acc = 0
      for (let i = 0; i < 5000; i++) {
        const v = yield globalThis.__CartaSyncPromise.resolve(1).then((x) => {
          if (i === 4999) {
            deepestStackLines = (new Error().stack || '').split('\\n').length
          }
          return x
        })
        acc += v
      }
      return acc
    })
if (deepRes._state !== 'fulfilled' || deepRes._value !== 5000 || deepestStackLines >= 25) {
  throw new Error('Node self-test stack check failed: state=' + deepRes._state + ' val=' + deepRes._value + ' stackLines=' + deepestStackLines)
}
console.log('[porffor-netmd] Node self-test OK (retailmac=' + mac + ', stackLines@5000=' + deepestStackLines + ')')
`
  fs.writeFileSync(selfTestPath, selfTestCode, 'utf8')
  runChecked(process.execPath, ['--openssl-legacy-provider', selfTestPath], {
    stdio: 'inherit',
  })
}

// ---------------------------------------------------------------------------
// Porffor C-ABI entry script, generated-C patching, and build entrypoint
// ---------------------------------------------------------------------------

/**
 * Wrap the lowered NetMD bundle with `Porffor.c` host callbacks
 * (`globalThis.__cartaNetMdHost`) and exported bridge functions for Porffor.
 *
 * @param {string} loweredPath - Path to `netmd_lowered.mjs`.
 * @returns {string} Path to the generated `entry.js` Porffor entrypoint.
 */
function buildEntryJs(loweredPath) {
  const entryPath = path.join(workDir, 'entry.js')
  const loweredCode = fs
    .readFileSync(loweredPath, 'utf8')
    .replace(/\nexport\s*\{[\s\S]*?\};\s*$/, '\n')
  const entryCode = `
${loweredCode}

function setLastError(msg) {
  const str = String(msg || 'NetMD error');
  const n = str.length < 255 ? str.length : 255;
  const arr = new Uint8Array(n);
  for (let i = 0; i < n; i++) {
    const ch = str.charCodeAt(i);
    arr[i] = ch < 128 ? ch : 63;
  }
  const ptr = Porffor.IR.ptr(arr.buffer);
  const off = arr.byteOffset;
  Porffor.c\`
    size_t len = (size_t)PF_F64(n);
    if (len > 255) len = 255;
    if (len > 0) memcpy(g_pf_err, MEM + (u32)PF_F64(ptr) + 8 + (u32)PF_F64(off), len);
    g_pf_err[len] = 0;
  \`;
  return -1;
}

function copyLastHostError(fallback) {
  let len = 0;
  Porffor.c\`
    const char* herr = carta_netmd_host_last_error();
    if (herr && herr[0]) {
      size_t n = strlen(herr);
      if (n > 255) n = 255;
      memcpy(g_pf_err, herr, n);
      g_pf_err[n] = 0;
      len = (f64)n;
    }
  \`;
  if (len === 0) setLastError(fallback);
}

function copyInBytes(srcPtr, len) {
  const n = len | 0;
  const out = new Uint8Array(n);
  if (n <= 0) return out;
  const p = Porffor.IR.ptr(out.buffer);
  const off = out.byteOffset;
  Porffor.c\`
    size_t sz = (size_t)PF_F64(n);
    if (sz > 0) {
      memcpy(MEM + (u32)PF_F64(p) + 8 + (u32)PF_F64(off), (const void*)(uintptr_t)PF_F64(srcPtr), sz);
    }
  \`;
  return out;
}

function asciiToString(bytes) {
  let out = '';
  for (let i = 0; i < bytes.length; i++) {
    out += String.fromCharCode(bytes[i]);
  }
  return out;
}

function utf8DecodeBytes(bytes) {
  let out = '';
  let i = 0;
  while (i < bytes.length) {
    const b0 = bytes[i++];
    let cp = b0;
    let need = 0;
    if ((b0 & 0xe0) === 0xc0) { cp = b0 & 0x1f; need = 1; }
    else if ((b0 & 0xf0) === 0xe0) { cp = b0 & 0x0f; need = 2; }
    else if ((b0 & 0xf8) === 0xf0) { cp = b0 & 0x07; need = 3; }
    else if (b0 >= 0x80) { cp = 0xfffd; need = 0; }
    for (let k = 0; k < need; k++) {
      if (i >= bytes.length || (bytes[i] & 0xc0) !== 0x80) { cp = 0xfffd; break; }
      cp = (cp << 6) | (bytes[i++] & 0x3f);
    }
    if (cp <= 0xffff) out += String.fromCharCode(cp);
    else {
      cp -= 0x10000;
      out += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
    }
  }
  return out;
}

function utf8EncodeBytes(str) {
  const s = String(str == null ? '' : str);
  const arr = [];
  for (let i = 0; i < s.length; i++) {
    let cp = s.charCodeAt(i);
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < s.length) {
      const lo = s.charCodeAt(i + 1);
      if (lo >= 0xdc00 && lo <= 0xdfff) {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
        i++;
      } else {
        cp = 0xfffd;
      }
    } else if (cp >= 0xdc00 && cp <= 0xdfff) {
      cp = 0xfffd;
    }
    if (cp < 0x80) arr.push(cp);
    else if (cp < 0x800) arr.push(0xc0 | (cp >> 6), 0x80 | (cp & 0x3f));
    else if (cp < 0x10000) arr.push(0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
    else arr.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
  }
  return new Uint8Array(arr);
}

function writeScratchBytes(u8) {
  const len = u8.length | 0;
  const p = len > 0 ? Porffor.IR.ptr(u8.buffer) : 0;
  const off = len > 0 ? u8.byteOffset : 0;
  Porffor.c\`
    size_t req = (size_t)PF_F64(len);
    if (req > g_pf_out_cap) {
      unsigned char* grown = (unsigned char*)realloc(g_pf_out_buf, req ? req : 1);
      if (!grown) abort();
      g_pf_out_buf = grown;
      g_pf_out_cap = req ? req : 1;
    }
    g_pf_out_len = req;
    if (req > 0) {
      memcpy(g_pf_out_buf, MEM + (u32)PF_F64(p) + 8 + (u32)PF_F64(off), req);
    }
  \`;
  return len;
}

// Install C++ host callbacks for NetMD core
globalThis.__cartaNetMdHost = {
  desCrypt(isTriple, isEncrypt, isCbc, keyBytes, ivBytes, inBytes) {
    const len = inBytes.length | 0;
    if (len === 0) return new Uint8Array(0);
    const kLen = keyBytes.length | 0;
    const kp = Porffor.IR.ptr(keyBytes.buffer);
    const koff = keyBytes.byteOffset;
    const hasIv = ivBytes && ivBytes.length >= 8 ? 1 : 0;
    const ivp = hasIv ? Porffor.IR.ptr(ivBytes.buffer) : 0;
    const ivoff = hasIv ? ivBytes.byteOffset : 0;
    const ip = Porffor.IR.ptr(inBytes.buffer);
    const ioff = inBytes.byteOffset;
    const out = new Uint8Array(len);
    const op = Porffor.IR.ptr(out.buffer);
    const ooff = out.byteOffset;
    let rc = -1;
    Porffor.c\`
      const u8* kp_c = (const u8*)(MEM + (u32)PF_F64(kp) + 8 + (u32)PF_F64(koff));
      const u8* ivp_c = (int)PF_F64(hasIv) ? (const u8*)(MEM + (u32)PF_F64(ivp) + 8 + (u32)PF_F64(ivoff)) : NULL;
      const u8* ip_c = (const u8*)(MEM + (u32)PF_F64(ip) + 8 + (u32)PF_F64(ioff));
      u8* op_c = (u8*)(MEM + (u32)PF_F64(op) + 8 + (u32)PF_F64(ooff));
      rc = (f64)carta_netmd_host_des_crypt(
        (int)PF_F64(isTriple), (int)PF_F64(isEncrypt), (int)PF_F64(isCbc),
        kp_c, (int)PF_F64(kLen), ivp_c, ip_c, (int)PF_F64(len), op_c);
    \`;
    if (rc < 0) {
      copyLastHostError('DES/3DES host error');
      throw new Error('DES/3DES host error');
    }
    return out;
  },
  randomBytes(n) {
    const count = n | 0;
    if (count <= 0) return new Uint8Array(0);
    const out = new Uint8Array(count);
    const op = Porffor.IR.ptr(out.buffer);
    const ooff = out.byteOffset;
    let rc = -1;
    Porffor.c\`
      u8* op_c = (u8*)(MEM + (u32)PF_F64(op) + 8 + (u32)PF_F64(ooff));
      rc = (f64)carta_netmd_host_random_bytes(op_c, (int)PF_F64(count));
    \`;
    if (rc < 0) throw new Error('randomBytes host error');
    return out;
  },
  cp932(toUtf8, inBytes) {
    const inLen = inBytes.length | 0;
    if (inLen <= 0) return new Uint8Array(0);
    const outCap = inLen * 4 + 16;
    const out = new Uint8Array(outCap);
    const ip = Porffor.IR.ptr(inBytes.buffer);
    const ioff = inBytes.byteOffset;
    const op = Porffor.IR.ptr(out.buffer);
    const ooff = out.byteOffset;
    let rc = -1;
    Porffor.c\`
      const u8* ip_c = (const u8*)(MEM + (u32)PF_F64(ip) + 8 + (u32)PF_F64(ioff));
      u8* op_c = (u8*)(MEM + (u32)PF_F64(op) + 8 + (u32)PF_F64(ooff));
      rc = (f64)carta_netmd_host_cp932(
        (int)PF_F64(toUtf8), ip_c, (int)PF_F64(inLen), op_c, (int)PF_F64(outCap));
    \`;
    if (rc < 0) throw new Error('CP932 conversion failed');
    return out.slice(0, rc | 0);
  },
  sleepMs(ms) {
    const dur = ms | 0;
    Porffor.c\`
      carta_netmd_host_sleep_ms((int)PF_F64(dur));
    \`;
  },
  reportStep(done, total) {
    const d = done | 0;
    const tot = total | 0;
    Porffor.c\`
      carta_netmd_host_report_step((int)PF_F64(d), (int)PF_F64(tot));
    \`;
  },
  nowMs() {
    let ms = 0;
    Porffor.c\`
      ms = (f64)carta_netmd_host_now_ms();
    \`;
    return ms;
  },
  reportTocWait(elapsedMs, maxMs, done) {
    const e = elapsedMs | 0;
    const m = maxMs | 0;
    const d = done | 0;
    Porffor.c\`
      carta_netmd_host_report_toc_wait((int)PF_F64(e), (int)PF_F64(m), (int)PF_F64(d));
    \`;
  },
  usbListJson() {
    const cap = 16384;
    const buf = new Uint8Array(cap);
    const op = Porffor.IR.ptr(buf.buffer);
    const ooff = buf.byteOffset;
    let rc = -1;
    Porffor.c\`
      u8* op_c = (u8*)(MEM + (u32)PF_F64(op) + 8 + (u32)PF_F64(ooff));
      rc = (f64)carta_netmd_host_usb_list(op_c, (int)PF_F64(cap));
    \`;
    if (rc < 0) {
      copyLastHostError('USB list failed');
      throw new Error('USB list failed');
    }
    return utf8DecodeBytes(buf.subarray(0, rc | 0));
  },
  usbOpen(deviceId) {
    const bytes = utf8EncodeBytes(deviceId + '\\0');
    const ip = Porffor.IR.ptr(bytes.buffer);
    const ioff = bytes.byteOffset;
    let h = -1;
    Porffor.c\`
      const char* id_c = (const char*)(MEM + (u32)PF_F64(ip) + 8 + (u32)PF_F64(ioff));
      h = (f64)carta_netmd_host_usb_open(id_c);
    \`;
    if (h < 0) {
      copyLastHostError('USB open failed');
      throw new Error('USB open failed for ' + deviceId);
    }
    return h | 0;
  },
  usbClose(handle) {
    const h = handle | 0;
    Porffor.c\`
      carta_netmd_host_usb_close((int)PF_F64(h));
    \`;
  },
  usbReset(handle) {
    const h = handle | 0;
    let rc = 0;
    Porffor.c\`
      rc = (f64)carta_netmd_host_usb_reset((int)PF_F64(h));
    \`;
    if (rc < 0) {
      copyLastHostError('USB reset failed');
      throw new Error('USB reset failed');
    }
  },
  usbControlOut(handle, req, val, idx, bytes) {
    const h = handle | 0;
    const len = bytes.length | 0;
    const sentinel = len > 0 ? bytes : new Uint8Array(1);
    const ip = Porffor.IR.ptr(sentinel.buffer);
    const ioff = sentinel.byteOffset;
    let rc = -1;
    Porffor.c\`
      const u8* d_c = (int)PF_F64(len) > 0 ? (const u8*)(MEM + (u32)PF_F64(ip) + 8 + (u32)PF_F64(ioff)) : NULL;
      rc = (f64)carta_netmd_host_usb_control_out(
        (int)PF_F64(h), (int)PF_F64(req), (int)PF_F64(val), (int)PF_F64(idx), d_c, (int)PF_F64(len));
    \`;
    if (rc < 0) {
      copyLastHostError('USB control OUT failed');
      throw new Error('USB control OUT failed');
    }
    return rc | 0;
  },
  usbControlIn(handle, req, val, idx, maxLen) {
    const h = handle | 0;
    const cap = (maxLen | 0) > 0 ? (maxLen | 0) : 1;
    const out = new Uint8Array(cap);
    const op = Porffor.IR.ptr(out.buffer);
    const ooff = out.byteOffset;
    let rc = -1;
    Porffor.c\`
      u8* o_c = (u8*)(MEM + (u32)PF_F64(op) + 8 + (u32)PF_F64(ooff));
      rc = (f64)carta_netmd_host_usb_control_in(
        (int)PF_F64(h), (int)PF_F64(req), (int)PF_F64(val), (int)PF_F64(idx), o_c, (int)PF_F64(maxLen));
    \`;
    if (rc < 0) {
      copyLastHostError('USB control IN failed');
      throw new Error('USB control IN failed');
    }
    return out.slice(0, rc | 0);
  },
  usbBulkOut(handle, endpoint, bytes) {
    const h = handle | 0;
    const ep = endpoint | 0;
    const len = bytes.length | 0;
    const sentinel = len > 0 ? bytes : new Uint8Array(1);
    const ip = Porffor.IR.ptr(sentinel.buffer);
    const ioff = sentinel.byteOffset;
    let rc = -1;
    Porffor.c\`
      const u8* d_c = (int)PF_F64(len) > 0 ? (const u8*)(MEM + (u32)PF_F64(ip) + 8 + (u32)PF_F64(ioff)) : NULL;
      rc = (f64)carta_netmd_host_usb_bulk_out(
        (int)PF_F64(h), (int)PF_F64(ep), d_c, (int)PF_F64(len));
    \`;
    if (rc < 0) {
      copyLastHostError('USB bulk OUT failed');
      throw new Error('USB bulk OUT failed');
    }
    return rc | 0;
  },
  usbBulkIn(handle, endpoint, maxLen) {
    const h = handle | 0;
    const ep = endpoint | 0;
    const cap = (maxLen | 0) > 0 ? (maxLen | 0) : 1;
    const out = new Uint8Array(cap);
    const op = Porffor.IR.ptr(out.buffer);
    const ooff = out.byteOffset;
    let rc = -1;
    Porffor.c\`
      u8* o_c = (u8*)(MEM + (u32)PF_F64(op) + 8 + (u32)PF_F64(ooff));
      rc = (f64)carta_netmd_host_usb_bulk_in(
        (int)PF_F64(h), (int)PF_F64(ep), o_c, (int)PF_F64(maxLen));
    \`;
    if (rc < 0) {
      copyLastHostError('USB bulk IN failed');
      throw new Error('USB bulk IN failed');
    }
    return out.slice(0, rc | 0);
  },
};

export function bridgeListDevices() {
  globalThis.__resetPromiseJobs();
  try {
    const json = globalThis.__cartaNetMdHost.usbListJson();
    return writeScratchBytes(utf8EncodeBytes(json));
  } catch (err) {
    return setLastError(err && err.message ? err.message : String(err));
  }
}

export function bridgeProbeDevice(idPtr, idLen, includeTracks) {
  globalThis.__resetPromiseJobs();
  try {
    const devId = utf8DecodeBytes(copyInBytes(idPtr, idLen));
    const json = __syncAwait(netmdProbeDevice(devId, (includeTracks | 0) !== 0));
    return writeScratchBytes(utf8EncodeBytes(json));
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

export function bridgeEraseDisc(idPtr, idLen) {
  globalThis.__resetPromiseJobs();
  try {
    const devId = utf8DecodeBytes(copyInBytes(idPtr, idLen));
    __syncAwait(netmdEraseDisc(devId));
    return 0;
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

export function bridgeUploadTrack(idPtr, idLen, modeCode, wirePtr, wireLen, hwPtr, hwLen, fwPtr, fwLen) {
  globalThis.__resetPromiseJobs();
  try {
    const devId = utf8DecodeBytes(copyInBytes(idPtr, idLen));
    const wire = copyInBytes(wirePtr, wireLen);
    const hwTitle = hwLen > 0 ? utf8DecodeBytes(copyInBytes(hwPtr, hwLen)) : '';
    const fwTitle = fwLen > 0 ? utf8DecodeBytes(copyInBytes(fwPtr, fwLen)) : '';
    const trkIdx = __syncAwait(netmdUploadTrack(devId, modeCode | 0, wire, hwTitle, fwTitle));
    return trkIdx | 0;
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

export function bridgeReadUtocSectors(idPtr, idLen) {
  globalThis.__resetPromiseJobs();
  try {
    const devId = utf8DecodeBytes(copyInBytes(idPtr, idLen));
    const data = __syncAwait(netmdReadUtocSectors(devId));
    return writeScratchBytes(data);
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

export function bridgeWriteUtocSectors(idPtr, idLen, secPtr, secLen, sectorsMask) {
  globalThis.__resetPromiseJobs();
  try {
    const devId = utf8DecodeBytes(copyInBytes(idPtr, idLen));
    const secBytes = copyInBytes(secPtr, secLen);
    __syncAwait(netmdWriteUtocSectors(devId, secBytes, sectorsMask | 0));
    return 0;
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

if (globalThis.__cartaPorfforRetain) {
  bridgeListDevices();
  bridgeProbeDevice(0, 0, 1);
  bridgeEraseDisc(0, 0);
  bridgeUploadTrack(0, 0, 0, 0, 0, 0, 0, 0, 0);
  bridgeReadUtocSectors(0, 0);
  bridgeWriteUtocSectors(0, 0, 0, 0, 0);
}
`
  fs.writeFileSync(entryPath, entryCode, 'utf8')
  return entryPath
}

/**
 * Inject `carta_netmd_host_*` extern declarations and C scratch globals, and
 * replace Porffor's executable `main()` with exported `netmd_pf_*` C wrappers.
 *
 * @param {string} cSource - Raw C emitted by `porf c`.
 * @returns {string} Patched C source ready for shared-library compilation.
 */
function patchGeneratedC(cSource) {
  const findFn = (jsName, f64Count) => {
    const f64Args = Array(f64Count).fill('f64').join(', ')
    const re = new RegExp(
      `jsval (p_\\w+_${jsName}_23m\\w+)\\(jsval, jsval${f64Count > 0 ? ', ' + f64Args : ''}\\);`,
      'g'
    )
    const matches = [...cSource.matchAll(re)].map((m) => m[1])
    if (matches.length !== 1) {
      throw new Error(
        `[netmd] Expected 1 generated declaration for ${jsName}(f64 x ${f64Count}), found ${matches.length}`
      )
    }
    return matches[0]
  }

  const fnList = findFn('bridgeListDevices', 0)
  const fnProbe = findFn('bridgeProbeDevice', 3)
  const fnErase = findFn('bridgeEraseDisc', 2)
  const fnUpload = findFn('bridgeUploadTrack', 9)
  const fnReadUtoc = findFn('bridgeReadUtocSectors', 2)
  const fnWriteUtoc = findFn('bridgeWriteUtocSectors', 5)

  const mainAnchor = `int main(int argc, char** argv) {
  porf_init(argc, argv);
  porf_data_init();
  volatile int porf_stack_anchor = 0;
  porf_c_stack_top = (void*)&porf_stack_anchor;
  p__main__23main();
  return 0;
}`
  if (!cSource.includes(mainAnchor)) {
    throw new Error('[netmd] Expected Porffor main(...) anchor not found')
  }

  const preludeMarker = 'static void* porf_c_stack_top = NULL;'
  const preludeIdx = cSource.indexOf(preludeMarker)
  if (preludeIdx === -1) {
    throw new Error(`[netmd] Expected '${preludeMarker}' in generated C`)
  }
  const insertAt = preludeIdx + preludeMarker.length

  const hostDeclsAndScratch = `
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern int carta_netmd_host_des_crypt(int is_triple_des, int is_encrypt, int is_cbc, const uint8_t* key_ptr, int key_len, const uint8_t* iv_ptr, const uint8_t* in_ptr, int in_len, uint8_t* out_ptr);
extern int carta_netmd_host_random_bytes(uint8_t* out_ptr, int len);
extern int carta_netmd_host_cp932(int to_utf8, const uint8_t* in_ptr, int in_len, uint8_t* out_ptr, int out_cap);
extern void carta_netmd_host_sleep_ms(int ms);
extern void carta_netmd_host_report_step(int done, int total);
extern double carta_netmd_host_now_ms(void);
extern void carta_netmd_host_report_toc_wait(int elapsed_ms, int max_ms, int done);
extern int carta_netmd_host_usb_list(uint8_t* json_out, int json_cap);
extern int carta_netmd_host_usb_open(const char* device_id);
extern int carta_netmd_host_usb_close(int handle);
extern int carta_netmd_host_usb_reset(int handle);
extern int carta_netmd_host_usb_control_out(int handle, int request, int value, int index, const uint8_t* data, int len);
extern int carta_netmd_host_usb_control_in(int handle, int request, int value, int index, uint8_t* out, int max_len);
extern int carta_netmd_host_usb_bulk_out(int handle, int endpoint, const uint8_t* data, int len);
extern int carta_netmd_host_usb_bulk_in(int handle, int endpoint, uint8_t* out, int max_len);
extern const char* carta_netmd_host_last_error(void);

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
static char g_pf_err[256] = {0};
`

  const exportsBlock = `
#if defined(_WIN32)
#if defined(CARTA_PF_STATIC)
#define CARTA_PF_EXPORT
#else
#define CARTA_PF_EXPORT __declspec(dllexport)
#endif
#else
#define CARTA_PF_EXPORT __attribute__((visibility("default")))
#endif

static int netmd_pf_initialized = 0;
static void netmd_ensure_init(void) {
  if (!netmd_pf_initialized) {
    static char arg0[] = "netmd-porffor";
    static char* argv[] = { arg0, NULL };
    porf_init(1, argv);
    porf_data_init();
    p__main__23main();
    netmd_pf_initialized = 1;
  }
}

CARTA_PF_EXPORT
int netmd_pf_list_devices(void) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  netmd_ensure_init();
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnList}(porf_box(0, 0), porf_box(0, 0)).val;
}

CARTA_PF_EXPORT
int netmd_pf_probe_device(const char* device_id_ptr, int device_id_len, int include_tracks) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  netmd_ensure_init();
  if (!device_id_ptr || device_id_len <= 0) return -1;
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnProbe}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)device_id_ptr, (f64)device_id_len, (f64)include_tracks
  ).val;
}

CARTA_PF_EXPORT
int netmd_pf_erase_disc(const char* device_id_ptr, int device_id_len) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  netmd_ensure_init();
  if (!device_id_ptr || device_id_len <= 0) return -1;
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnErase}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)device_id_ptr, (f64)device_id_len
  ).val;
}

CARTA_PF_EXPORT
int netmd_pf_upload_track(const char* device_id_ptr,
                          int device_id_len,
                          int mode_code,
                          const unsigned char* wire_ptr,
                          int wire_len,
                          const char* hw_title_ptr,
                          int hw_title_len,
                          const char* fw_title_ptr,
                          int fw_title_len) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  netmd_ensure_init();
  if (!device_id_ptr || device_id_len <= 0 || !wire_ptr || wire_len <= 0) return -1;
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnUpload}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)device_id_ptr, (f64)device_id_len,
    (f64)mode_code,
    (f64)(uintptr_t)wire_ptr, (f64)wire_len,
    (f64)(uintptr_t)hw_title_ptr, (f64)hw_title_len,
    (f64)(uintptr_t)fw_title_ptr, (f64)fw_title_len
  ).val;
}

CARTA_PF_EXPORT
int netmd_pf_read_utoc_sectors(const char* device_id_ptr, int device_id_len) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  netmd_ensure_init();
  if (!device_id_ptr || device_id_len <= 0) return -1;
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnReadUtoc}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)device_id_ptr, (f64)device_id_len
  ).val;
}

CARTA_PF_EXPORT
int netmd_pf_write_utoc_sectors(const char* device_id_ptr,
                                 int device_id_len,
                                 const unsigned char* sectors5_ptr,
                                 int sectors5_len,
                                 int sectors_mask) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  netmd_ensure_init();
  if (!device_id_ptr || device_id_len <= 0 || !sectors5_ptr || sectors5_len <= 0) return -1;
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnWriteUtoc}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)device_id_ptr, (f64)device_id_len,
    (f64)(uintptr_t)sectors5_ptr, (f64)sectors5_len,
    (f64)sectors_mask
  ).val;
}

CARTA_PF_EXPORT
const unsigned char* netmd_pf_output_ptr(void) {
  return g_pf_out_buf;
}

CARTA_PF_EXPORT
size_t netmd_pf_output_len(void) {
  return g_pf_out_len;
}

CARTA_PF_EXPORT
const char* netmd_pf_last_error(void) {
  return g_pf_err;
}

CARTA_PF_EXPORT
void netmd_pf_release_scratch(void) {
  free(g_pf_out_buf);
  g_pf_out_buf = NULL;
  g_pf_out_cap = 0;
  g_pf_out_len = 0;
}
`

  let patched =
    cSource.slice(0, insertAt) + hostDeclsAndScratch + cSource.slice(insertAt)
  patched = patched.replace(mainAnchor, exportsBlock)
  return patched
}

/**
 * Orchestrate incremental up-to-date check, staging, async lowering, Node
 * sanity test, Porffor C emission, Clang dylib compilation, and `nm -gU` export
 * verification for `libnetmd_porffor.dylib`.
 */
async function main() {
  const compileOnly = shouldCompileOnly()
  ensurePrereqs(!compileOnly)

  const scriptMtime = fs.statSync(__filename).mtimeMs
  if (emitCOnly) {
    const depsMtime = Math.max(
      latestMtimeMs(path.join(netmdDepsModules, 'netmd-js', 'dist')),
      latestMtimeMs(path.join(netmdDepsModules, 'netmd-exploits', 'dist'))
    )
    const latestInput = Math.max(scriptMtime, depsMtime)
    if (!forceRebuild && fs.existsSync(generatedCPath) && fs.statSync(generatedCPath).mtimeMs >= latestInput) {
      console.log(`[porffor] Up to date (C): ${generatedCPath}`)
      return
    }
  } else if (compileOnly) {
    if (!fs.existsSync(generatedCPath)) {
      throw new Error(
        `Missing pre-generated C source at ${generatedCPath}. Run --emit-c-only on macOS/Linux first.`
      )
    }
    const latestInput = Math.max(scriptMtime, fs.statSync(generatedCPath).mtimeMs)
    if (!forceRebuild && fs.existsSync(dylibPath) && fs.statSync(dylibPath).mtimeMs >= latestInput) {
      console.log(`[porffor] Up to date: ${dylibPath}`)
      return
    }
  } else {
    const depsMtime = Math.max(
      latestMtimeMs(path.join(netmdDepsModules, 'netmd-js', 'dist')),
      latestMtimeMs(path.join(netmdDepsModules, 'netmd-exploits', 'dist'))
    )
    const latestInput = Math.max(scriptMtime, depsMtime)
    if (!forceRebuild && fs.existsSync(dylibPath) && fs.statSync(dylibPath).mtimeMs >= latestInput) {
      console.log(`[porffor] Up to date: ${dylibPath}`)
      return
    }
  }

  if (!compileOnly) {
    console.log('[porffor] Staging & transpiling NetMD + exploits...')
    const requireFromDeps = createRequire(
      path.join(netmdDepsModules, '_anchor.js')
    )
    const requireFromTools = createRequire(
      path.join(toolsNodeModules, '_anchor.js')
    )
    const babel = requireFromTools('@babel/core')
    const esbuild = requireFromTools('esbuild')

    const forceTocTables = await preassembleForceTocPayloads(requireFromDeps)
    stageNetmdPackages(requireFromDeps, forceTocTables)
    const loweredPath = await writeCoreEntryAndBundle(babel, esbuild)
    await runNodeSanitySelfTest(loweredPath)
    const entryPath = buildEntryJs(loweredPath)

    const rawC = path.join(workDir, 'netmd_raw.c')
    runChecked(porfBin, ['c', '-t', entryPath, '-o', rawC], { stdio: 'inherit' })

    const rawSource = fs.readFileSync(rawC, 'utf8')
    const patchedSource = patchGeneratedC(rawSource)
    fs.writeFileSync(generatedCPath, patchedSource, 'utf8')
    if (emitCOnly) {
      console.log(`[porffor] Emitted ${generatedCPath}`)
      return
    }
  }

  console.log(`[porffor] Compiling ${outLibName} from ${generatedCPath}...`)
  const tmpOut = path.join(workDir, outLibName)

  if (isMac) {
    runChecked(
      clangBin,
      [
        '-dynamiclib',
        '-O1',
        '-fno-fast-math',
        '-ffp-contract=off',
        '-fno-math-errno',
        '-fPIC',
        '-fvisibility=hidden',
        '-mllvm',
        '-enable-machine-outliner=never',
        '-Wl,-mllvm,-enable-machine-outliner=never',
        '-Wl,-undefined,dynamic_lookup',
        '-install_name',
        `@rpath/${outLibName}`,
        '-mmacosx-version-min=11.0',
        generatedCPath,
        '-o',
        tmpOut,
      ],
      { stdio: 'inherit' }
    )

    const nm = runChecked(nmBin, ['-gU', tmpOut])
    const exported = nm.stdout
      .split('\n')
      .map((line) => line.trim())
      .filter(Boolean)
      .map((line) => line.split(/\s+/).pop())
    const unexpected = exported.filter((sym) => !sym.startsWith('_netmd_pf_'))
    if (unexpected.length > 0 || exported.length === 0) {
      throw new Error(
        `[netmd] Exported symbol check failed.\nExpected only _netmd_pf_*\nUnexpected: ${unexpected.join(', ')}\nAll: ${exported.join(', ')}`
      )
    }
    fs.renameSync(tmpOut, dylibPath)
    console.log(
      `[porffor] Built ${dylibPath} (${exported.length} exported symbols: ${exported.join(', ')})`
    )
  } else if (isWin) {
    const tmpObj = path.join(workDir, 'netmd_porffor.obj')
    const winCompatInc = path.join(studioDir, 'Tools', 'porffor-win32-compat')
    runChecked(
      clangBin,
      [
        '-c',
        '-DCARTA_PF_STATIC=1',
        '-O1',
        '-fno-fast-math',
        '-ffp-contract=off',
        '-fno-math-errno',
        '-I',
        winCompatInc,
        generatedCPath,
        '-o',
        tmpObj,
      ],
      { stdio: 'inherit' }
    )
    const llvmLib = process.env.LLVM_LIB || 'llvm-lib'
    runChecked(llvmLib, [`/OUT:${tmpOut}`, tmpObj], { stdio: 'inherit' })
    fs.copyFileSync(tmpOut, dylibPath)
    console.log(`[porffor] Built static library ${dylibPath}`)
  } else {
    runChecked(
      clangBin,
      [
        '-shared',
        '-O1',
        '-fno-fast-math',
        '-ffp-contract=off',
        '-fno-math-errno',
        '-fPIC',
        '-fvisibility=hidden',
        '-mllvm',
        '-enable-machine-outliner=never',
        `-Wl,-soname,${outLibName}`,
        '-Wl,--allow-shlib-undefined',
        generatedCPath,
        '-lm',
        '-o',
        tmpOut,
      ],
      { stdio: 'inherit' }
    )

    const nm = runChecked(nmBin, ['-D', '--defined-only', '-g', tmpOut])
    const exported = nm.stdout
      .split('\n')
      .map((line) => line.trim())
      .filter(Boolean)
      .map((line) => line.split(/\s+/).pop())
      .filter((sym) => sym && !sym.startsWith('_') && sym !== 'etext' && sym !== 'edata' && sym !== 'end')
    const unexpected = exported.filter((sym) => !sym.startsWith('netmd_pf_'))
    if (unexpected.length > 0 || exported.length === 0) {
      throw new Error(
        `[netmd] Exported symbol check failed.\nExpected only netmd_pf_*\nUnexpected: ${unexpected.join(', ')}\nAll: ${exported.join(', ')}`
      )
    }
    fs.renameSync(tmpOut, dylibPath)
    console.log(
      `[porffor] Built ${dylibPath} (${exported.length} exported symbols: ${exported.join(', ')})`
    )
  }
}

await main()
