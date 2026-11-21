#!/usr/bin/env node
/**
 * @fileoverview Build the isolated Porffor alpha-19 shared library for
 * `himd-js` + `nufatfs` + `node-mass-storage` inside
 * `carta-studio/build/porffor/lib/libhimd_porffor.dylib`.
 *
 * Heavy OS, SCSI, crypto, charset, and streaming file-I/O primitives
 * (DES/3DES and RNG, iconv CP932, sleep, progress reporting, file chunk I/O,
 * and SCSI transport / Mock MZ-RH1 emulation) live in C++
 * (`carta-himd/HiMdHostBridge.cpp`) and are resolved at load time via
 * `-Wl,-undefined,dynamic_lookup` / `-Wl,-export_dynamic`.
 *
 * Usage:
 *   node Tools/build_porffor_himd.mjs [--force] [--emit-c-only] [--compile-only]
 */
import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import { createRequire } from 'node:module'
import path from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'

// ---------------------------------------------------------------------------
// Paths and CLI configuration
// ---------------------------------------------------------------------------

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)
const studioDir = path.resolve(__dirname, '..')
const buildRoot = path.join(studioDir, 'build', 'porffor')
const libOutDir = path.join(buildRoot, 'lib')
const generatedDir = path.join(buildRoot, 'generated')
const workDir = path.join(buildRoot, 'himd-work')
const stagedDir = path.join(workDir, 'staged')
const shimsDir = path.join(workDir, 'shims')
const toolsDir = path.join(buildRoot, 'tools')
const toolsNodeModules = path.join(toolsDir, 'node_modules')
const himdDepsDir = path.join(buildRoot, 'himd-deps')
const himdDepsModules = path.join(himdDepsDir, 'node_modules')
const committedHimdDepsDir = path.join(studioDir, 'Tools', 'himd-deps')
const generatedCPath = path.join(generatedDir, 'himd_patched.c')

const hostPlatform = process.platform
const isMac = hostPlatform === 'darwin'
const isWin = hostPlatform === 'win32'

const outLibName = isMac
  ? 'libhimd_porffor.dylib'
  : isWin
    ? 'himd_porffor.lib'
    : 'libhimd_porffor.so'
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

// ---------------------------------------------------------------------------
// Process execution, prerequisites, and mtime utilities
// ---------------------------------------------------------------------------

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

  const committedLockPath = path.join(committedHimdDepsDir, 'package-lock.json')
  const buildLockPath = path.join(himdDepsDir, 'package-lock.json')
  const lockChanged =
    !fs.existsSync(buildLockPath) ||
    !fs.existsSync(committedLockPath) ||
    fs.readFileSync(buildLockPath, 'utf8') !== fs.readFileSync(committedLockPath, 'utf8')

  if (!fs.existsSync(path.join(himdDepsModules, 'himd-js')) || lockChanged) {
    if (!fs.existsSync(path.join(committedHimdDepsDir, 'package.json'))) {
      throw new Error(`Missing committed Hi-MD manifests in ${committedHimdDepsDir}`)
    }
    fs.mkdirSync(himdDepsDir, { recursive: true })
    fs.copyFileSync(
      path.join(committedHimdDepsDir, 'package.json'),
      path.join(himdDepsDir, 'package.json')
    )
    fs.copyFileSync(committedLockPath, buildLockPath)
    runChecked('npm', ['ci', '--ignore-scripts', '--prefix', himdDepsDir], {
      stdio: 'inherit',
    })
  }

  for (const mod of ['@babel/core', 'esbuild']) {
    if (!fs.existsSync(path.join(toolsNodeModules, mod))) {
      throw new Error(`Missing build tool dependency ${mod} in ${toolsNodeModules}`)
    }
  }
  for (const pkg of ['himd-js', 'nufatfs', 'node-mass-storage']) {
    if (!fs.existsSync(path.join(himdDepsModules, pkg))) {
      throw new Error(`Missing Hi-MD package ${pkg} in ${himdDepsModules}`)
    }
  }
}

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

function replaceOrThrow(source, needle, replacement, label) {
  if (!source.includes(needle)) {
    throw new Error(`[himd-stage] Missing expected snippet for ${label}`)
  }
  return source.replace(needle, replacement)
}

function replaceAllOrThrow(source, needle, replacement, label) {
  if (!source.includes(needle)) {
    throw new Error(`[himd-stage] Missing expected snippet for ${label}`)
  }
  return source.replaceAll(needle, replacement)
}

// ---------------------------------------------------------------------------
// Lower TypeScript ES2015 __awaiter(..., function* () { yield ... }) via Babel
// ---------------------------------------------------------------------------

function lowerTsAwaiterGenerators(babel, sourceCode) {
  const t = babel.types
  return babel.transformSync(sourceCode, {
    babelrc: false,
    configFile: false,
    plugins: [
      () => ({
        visitor: {
          FunctionDeclaration(p) {
            if (p.node.id && p.node.id.name === '__awaiter') {
              p.remove()
            }
          },
          VariableDeclarator(p) {
            if (t.isIdentifier(p.node.id) && p.node.id.name === '__awaiter') {
              p.remove()
            }
          },
          CallExpression(p) {
            if (!t.isIdentifier(p.node.callee, { name: '__awaiter' })) return
            if (p.node.arguments.length < 4) return
            const gen = p.node.arguments[3]
            if (!t.isFunctionExpression(gen) || !gen.generator) return

            // Convert function* body yields into awaits inside an async arrow/function.
            gen.async = true
            gen.generator = false
            p.get('arguments.3.body').traverse({
              Function(inner) {
                inner.skip()
              },
              YieldExpression: {
                exit(yp) {
                  yp.replaceWith(t.awaitExpression(yp.node.argument || t.identifier('undefined')))
                },
              },
            })

            const parentFn = p.getFunctionParent()
            const enclosingStmt = p.getStatementParent()
            if (
              parentFn &&
              enclosingStmt &&
              t.isReturnStatement(enclosingStmt.node) &&
              enclosingStmt.node.argument === p.node &&
              enclosingStmt.getFunctionParent() === parentFn
            ) {
              // Replace outer `return __awaiter(this, ..., function* () { BODY });` by making parent async!
              parentFn.node.async = true
              parentFn.node.body = gen.body
            } else {
              const thisExpr = p.node.arguments[0]
              p.replaceWith(
                t.callExpression(t.memberExpression(gen, t.identifier('call')), [thisExpr])
              )
            }
          },
        },
      }),
    ],
  }).code
}

// ---------------------------------------------------------------------------
// Stage patched himd-js, nufatfs, node-mass-storage, and pure-JS shims
// ---------------------------------------------------------------------------

function stageHimdPackages(babel) {
  fs.rmSync(workDir, { recursive: true, force: true })
  fs.mkdirSync(path.join(stagedDir, 'himd-js', 'filesystem'), { recursive: true })
  fs.mkdirSync(path.join(stagedDir, 'nufatfs'), { recursive: true })
  fs.mkdirSync(path.join(stagedDir, 'node-mass-storage'), { recursive: true })
  fs.mkdirSync(shimsDir, { recursive: true })
  fs.mkdirSync(libOutDir, { recursive: true })

  // -------------------------------------------------------------------------
  // 1. Stage himd-js/dist (excluding Node-only / worker / MP3-only files)
  // -------------------------------------------------------------------------
  const himdDist = path.join(himdDepsModules, 'himd-js', 'dist')
  const himdFiles = [
    'codecs.js',
    'encryption.js',
    'himd.js',
    'himd-functions.js',
    'secure-session.js',
    'streams.js',
    'trackinfo.js',
    'utils.js',
    'filesystem/himd-filesystem.js',
    'filesystem/usb-mass-storage-webusb-filesystem.js',
  ]

  for (const rel of himdFiles) {
    let code = fs.readFileSync(path.join(himdDist, rel), 'utf8').replaceAll('\r\n', '\n')

    if (rel === 'himd.js') {
      // Remove TypeScript __decorate helper and decorator calls that rely on Reflect/Object.getOwnPropertyDescriptor.
      code = replaceOrThrow(
        code,
        `var __decorate = (this && this.__decorate) || function (decorators, target, key, desc) {
    var c = arguments.length, r = c < 3 ? target : desc === null ? desc = Object.getOwnPropertyDescriptor(target, key) : desc, d;
    if (typeof Reflect === "object" && typeof Reflect.decorate === "function") r = Reflect.decorate(decorators, target, key, desc);
    else for (var i = decorators.length - 1; i >= 0; i--) if (d = decorators[i]) r = (c < 3 ? d(r) : c > 3 ? d(target, key, r) : d(target, key)) || r;
    return c > 3 && r && Object.defineProperty(target, key, r), r;
};`,
        `var __decorate = function () {};`,
        'himd.js __decorate header'
      )
      code = replaceOrThrow(
        code,
        `function dirty(target, propertyKey, descriptor) {
    const old = descriptor.value;
    descriptor.value = function (...args) {
        this.dirty = true;
        return old.apply(this, args);
    };
    return descriptor;
}`,
        `function markDirtyMethods(proto, names) {
    for (let i = 0; i < names.length; i++) {
        const key = names[i];
        const orig = proto[key];
        proto[key] = function (...args) {
            this.dirty = true;
            return orig.apply(this, args);
        };
    }
}`,
        'himd.js dirty decorator helper'
      )
      const decStart = code.indexOf('__decorate([\n    dirty\n], HiMD.prototype,')
      if (decStart === -1) {
        throw new Error('[himd-stage] Missing HiMD.prototype __decorate block')
      }
      const tail = code.slice(decStart)
      const decoratedMethods = [...tail.matchAll(/HiMD\.prototype,\s*"([^"]+)"/g)].map((m) => m[1])
      if (decoratedMethods.length < 13) {
        throw new Error(`[himd-stage] Expected >= 13 decorated HiMD methods, got ${decoratedMethods.length}`)
      }
      code =
        code.slice(0, decStart) +
        `markDirtyMethods(HiMD.prototype, ${JSON.stringify(decoratedMethods)});\n`

      // Regex-free atdataXX.hma matcher in findDatanum.
      code = replaceOrThrow(
        code,
        `            const match = name.match(/(.*)atdata([0-9a-f]{2})\\.hma$/i);
            if (match === null || type === 'directory')
                continue;
            const datanum = parseInt(match[2], 16);`,
        `            if (type === 'directory')
                continue;
            const slash = name.lastIndexOf('/');
            const base = (slash >= 0 ? name.slice(slash + 1) : name).toLowerCase();
            if (!base.startsWith('atdata') || !base.endsWith('.hma') || base.length !== 12)
                continue;
            const hexStr = base.slice(6, 8);
            const h0 = hexStr.charCodeAt(0);
            const h1 = hexStr.charCodeAt(1);
            const isHex0 = (h0 >= 48 && h0 <= 57) || (h0 >= 97 && h0 <= 102);
            const isHex1 = (h1 >= 48 && h1 <= 57) || (h1 >= 97 && h1 <= 102);
            if (!isHex0 || !isHex1)
                continue;
            const datanum = parseInt(hexStr, 16);`,
        'himd.js regex-free findDatanum'
      )

      // Fix missing await on atdata.seek in openAtdataForWriting.
      code = replaceOrThrow(
        code,
        `    async openAtdataForWriting() {
        const atdata = await this.openDatanumDependent('atdata', 'rw');
        atdata.seek(atdata.length);
        return atdata;
    }`,
        `    async openAtdataForWriting() {
        const atdata = await this.openDatanumDependent('atdata', 'rw');
        await atdata.seek(atdata.length);
        return atdata;
    }`,
        'himd.js await atdata.seek'
      )
    }

    if (rel === 'utils.js') {
      // Regex-free join()
      code = replaceOrThrow(
        code,
        `function join(...paths) {
    return paths.filter(e => e).join("/").replace(/\\/*/, '/');
}`,
        `function join(...paths) {
    const joined = paths.filter(e => e).join("/");
    let i = 0;
    while (i < joined.length && joined.charCodeAt(i) === 47) i++;
    return "/" + joined.slice(i);
}`,
        'himd-js/utils.js regex-free join'
      )
      // Use host CSPRNG when available (falls back to Math.random in pure JS if absent).
      code = replaceOrThrow(
        code,
        `function createRandomBytes(length = 8) {
    return new Uint8Array(Array(length)
        .fill(0)
        .map(() => Math.floor(Math.random() * 256)));
}`,
        `function createRandomBytes(length = 8) {
    const host = globalThis.__cartaHiMdHost;
    if (host && host.randomBytes) return host.randomBytes(length | 0);
    const out = new Uint8Array(length);
    for (let i = 0; i < length; i++) out[i] = Math.floor(Math.random() * 256);
    return out;
}`,
        'himd-js/utils.js createRandomBytes'
      )
    }

    if (rel === 'encryption.js') {
      // Avoid negative indices in subarray(-8).
      code = replaceOrThrow(
        code,
        `    return (0, utils_1.wordArrayToByteArray)(result.ciphertext).subarray(-8);`,
        `    const out = (0, utils_1.wordArrayToByteArray)(result.ciphertext);
    return out.subarray(out.length - 8);`,
        'encryption.js createIcvMac subarray(-8)'
      )
      code = replaceOrThrow(
        code,
        `    return (0, utils_1.wordArrayToByteArray)(mac.ciphertext).subarray(-8);`,
        `    const out = (0, utils_1.wordArrayToByteArray)(mac.ciphertext);
    return out.subarray(out.length - 8);`,
        'encryption.js createTrackMac subarray(-8)'
      )
    }

    if (rel === 'filesystem/usb-mass-storage-webusb-filesystem.js') {
      // Replace setTimeout poll in awaitSystemReady with synchronous host sleepMs.
      code = replaceOrThrow(
        code,
        `            await new Promise(res => setTimeout(res, 200));`,
        `            if (globalThis.__cartaHiMdHost && globalThis.__cartaHiMdHost.sleepMs) globalThis.__cartaHiMdHost.sleepMs(200);`,
        'usb-mass-storage-webusb-filesystem.js awaitSystemReady sleep'
      )
      // Await redefineClusterChain and guard empty clusterChain when deleting the last track.
      code = replaceOrThrow(
        code,
        `        this.fatfs.getUnderlying().redefineClusterChain(oldInitial, clusterChain);
        entry.fileSize -= totalBytesFreed;
        entry._firstCluster = clusterChain[0];`,
        `        await this.fatfs.getUnderlying().redefineClusterChain(oldInitial, clusterChain);
        entry.fileSize -= totalBytesFreed;
        entry._firstCluster = clusterChain.length > 0 ? clusterChain[0] : 0;`,
        'usb-mass-storage-webusb-filesystem.js freeFileRegions empty guard'
      )
    }

    if (rel === 'himd-functions.js') {
      // Strip node-id3 and mp3-parser imports (MP3 & async-generator dump helpers are replaced in entry).
      code = replaceOrThrow(
        code,
        `const node_id3_1 = require("node-id3");`,
        `// node-id3 removed`,
        'himd-functions.js node-id3'
      )
      code = replaceOrThrow(
        code,
        `const mp3_parser_1 = require("mp3-parser");`,
        `// mp3-parser removed`,
        'himd-functions.js mp3-parser'
      )
      // Fix upstream duplicate himd.addFragment(fragment) leak in uploadMacDependent.
      code = replaceOrThrow(
        code,
        `    const fragmentIndex = himd.addFragment(fragment);
    // Create a new track
    trackData.firstFragment = himd.addFragment(fragment);`,
        `    // Create a new track
    trackData.firstFragment = himd.addFragment(fragment);`,
        'himd-functions.js duplicate addFragment fix'
      )
      // Avoid Set.prototype.has with thisArg in rewriteGroups.
      code = replaceOrThrow(
        code,
        `    let alreadyGrouped = new Set();`,
        `    const alreadyGrouped = {};`,
        'himd-functions.js rewriteGroups set init'
      )
      code = replaceOrThrow(
        code,
        `        if (indices.some(alreadyGrouped.has, alreadyGrouped)) {
            throw new himd_1.HiMDError(\`Cannot add a track to group - track already grouped!\`);
        }
        const groupLength = end - start + 1;
        addGroup(himd, group.title, start, groupLength);
        indices.forEach(alreadyGrouped.add, alreadyGrouped);`,
        `        for (let k = 0; k < indices.length; k++) {
            if (alreadyGrouped[indices[k]]) {
                throw new himd_1.HiMDError(\`Cannot add a track to group - track already grouped!\`);
            }
        }
        const groupLength = end - start + 1;
        addGroup(himd, group.title, start, groupLength);
        for (let k = 0; k < indices.length; k++) {
            alreadyGrouped[indices[k]] = true;
        }`,
        'himd-functions.js rewriteGroups plain object map'
      )

      // Replace async-generator dump* and MP3/streaming upload functions with lightweight stubs
      // (dumpTrack & streaming upload live in himd_core_entry.js using host chunked file I/O).
      const dumpStart = code.indexOf('function getTotal({ blockStream })')
      const rewriteGroupsStart = code.indexOf('function rewriteGroups(himd, groups)')
      if (dumpStart === -1 || rewriteGroupsStart === -1 || rewriteGroupsStart <= dumpStart) {
        throw new Error('[himd-stage] Missing dump* section markers in himd-functions.js')
      }
      const uploadMp3Start = code.indexOf('async function uploadMP3Track(')
      const uploadMacStart = code.indexOf('async function uploadMacDependent(')
      const uploadStreamStart = code.indexOf('async function uploadStreamingMacDependent(')
      const deleteTracksStart = code.indexOf('async function deleteTracks(')
      if (
        uploadMp3Start === -1 ||
        uploadMacStart === -1 ||
        uploadStreamStart === -1 ||
        deleteTracksStart === -1
      ) {
        throw new Error('[himd-stage] Missing upload/delete section markers in himd-functions.js')
      }
      code =
        code.slice(0, dumpStart) +
        `function dumpTrack() { throw new himd_1.HiMDError('Use himdDumpTrackToFile'); }\n` +
        code.slice(rewriteGroupsStart, uploadMp3Start) +
        `async function uploadMP3Track() { throw new himd_1.HiMDError('MP3 upload unsupported'); }\n` +
        code.slice(uploadMacStart, uploadStreamStart) +
        `async function uploadStreamingMacDependent() { throw new himd_1.HiMDError('Use himdUploadTrackFromFile'); }\n` +
        code.slice(deleteTracksStart)
    }

    const dst = path.join(stagedDir, 'himd-js', rel)
    fs.mkdirSync(path.dirname(dst), { recursive: true })
    fs.writeFileSync(dst, code, 'utf8')
  }

  fs.writeFileSync(
    path.join(stagedDir, 'himd-js', 'filesystem', 'index.js'),
    `"use strict";
Object.defineProperty(exports, "__esModule", { value: true });
var hf = require("./himd-filesystem");
var usb = require("./usb-mass-storage-webusb-filesystem");
module.exports = { ...hf, ...usb };
`,
    'utf8'
  )

  fs.writeFileSync(
    path.join(stagedDir, 'himd-js', 'index.js'),
    `"use strict";
module.exports = {
  ...require("./filesystem"),
  ...require("./himd"),
  ...require("./himd-functions"),
  ...require("./codecs"),
  ...require("./streams"),
  ...require("./secure-session"),
  ...require("./encryption"),
  ...require("./trackinfo"),
  ...require("./utils"),
};
`,
    'utf8'
  )

  // -------------------------------------------------------------------------
  // 2. Stage nufatfs/dist (excluding node-fs-driver)
  // -------------------------------------------------------------------------
  const nufatDist = path.join(himdDepsModules, 'nufatfs', 'dist')
  const nufatFiles = [
    'chained-structures.js',
    'chunking-fs-driver.js',
    'cluster-allocator.js',
    'cluster-chain.js',
    'constructors.js',
    'high-level.js',
    'low-level.js',
    'mbr-driver.js',
    'overlay-driver.js',
    'types.js',
    'utils.js',
  ]
  for (const rel of nufatFiles) {
    let code = fs.readFileSync(path.join(nufatDist, rel), 'utf8').replaceAll('\r\n', '\n')

    if (rel === 'low-level.js') {
      // Replace Set with plain object map for Porffor compatibility.
      code = replaceOrThrow(
        code,
        `        this.alteredFATSectors = new Set();`,
        `        this.alteredFATSectors = {};`,
        'nufatfs/low-level.js alteredFATSectors init'
      )
      code = replaceAllOrThrow(
        code,
        `this.alteredFATSectors.add(`,
        `this.__markFatSector(`,
        'nufatfs/low-level.js alteredFATSectors.add'
      )
      code = replaceOrThrow(
        code,
        `    writeFATClusterEntry(number, next) {`,
        `    __markFatSector(sec) { this.alteredFATSectors[sec] = true; }
    writeFATClusterEntry(number, next) {`,
        'nufatfs/low-level.js __markFatSector method'
      )
      code = replaceOrThrow(
        code,
        `                for (const usedFATSector of this.alteredFATSectors) {
                    const sectorSlice = fatContents.subarray(usedFATSector * this.bootsectorInfo.bytesPerLogicalSector, (usedFATSector + 1) * this.bootsectorInfo.bytesPerLogicalSector);
                    await this.driver.writeSectors(this.bootsectorInfo.reservedLogicalSectors + fat * this.logicalSectorsPerFat + usedFATSector, sectorSlice);
                }
            }
            this.alteredFATSectors = new Set();`,
        `                const alteredKeys = Object.keys(this.alteredFATSectors);
                for (let k = 0; k < alteredKeys.length; k++) {
                    const usedFATSector = Number(alteredKeys[k]);
                    const sectorSlice = fatContents.subarray(usedFATSector * this.bootsectorInfo.bytesPerLogicalSector, (usedFATSector + 1) * this.bootsectorInfo.bytesPerLogicalSector);
                    await this.driver.writeSectors(this.bootsectorInfo.reservedLogicalSectors + fat * this.logicalSectorsPerFat + usedFATSector, sectorSlice);
                }
            }
            this.alteredFATSectors = {};`,
        'nufatfs/low-level.js alteredFATSectors flush loop'
      )
      // Guard redefineClusterChain when newChain is empty (deleting all clusters of ATDATA).
      code = replaceOrThrow(
        code,
        `        for (let link of previousChain) {
            this.writeFATClusterEntry(link, 0x00);
        }
        let previous = newChain[0];`,
        `        for (let link of previousChain) {
            this.writeFATClusterEntry(link, 0x00);
        }
        if (!newChain || newChain.length === 0) {
            return;
        }
        let previous = newChain[0];`,
        'nufatfs/low-level.js redefineClusterChain empty guard'
      )
      // Ensure fatContents DataView spans the exact slice if ever offset.
      code = replaceOrThrow(
        code,
        `        this.fatContents = new DataView(rawFat.buffer);`,
        `        this.fatContents = new DataView(rawFat.buffer, rawFat.byteOffset, rawFat.byteLength);`,
        'nufatfs/low-level.js fatContents DataView bounds'
      )
      // Bounds guard for consumeAllDirectoryEntries loop.
      code = replaceOrThrow(
        code,
        `        for (;;) {
            let entry = (0, constructors_1.createFatFSDirectoryEntry)(data, offset);`,
        `        for (;;) {
            if (offset + 32 > data.length)
                break;
            let entry = (0, constructors_1.createFatFSDirectoryEntry)(data, offset);`,
        'nufatfs/low-level.js consumeAllDirectoryEntries bounds guard'
      )
    }

    if (rel === 'utils.js') {
      // Rewrite structFormatUnpack cleanly without closures, Array.prototype.splice,
      // or argument side effects (dataIndex += repeat inside slice()).
      code = replaceOrThrow(
        code,
        `function structFormatUnpack(format, data, offset = 0) {
    let output = [];
    let endianness = Endianness.BIG;
    let _format = format.split('');
    const next = () => _format.splice(0, 1)[0];
    const peek = () => _format[0];
    if (['<', '>', '@'].includes(peek())) {
        let modifier = next();
        if (modifier === '<')
            endianness = Endianness.LITTLE;
    }
    let repeat = 0;
    let dataIndex = offset;
    while (_format.length) {
        let char = next();
        if ("1234567890".includes(char)) {
            // Is a repeat value.
            repeat *= 10;
            repeat += parseInt(char);
            continue;
        }
        if (char === 'x') {
            // Padding
            dataIndex += Math.max(1, repeat);
            repeat = 0;
            continue;
        }
        else if (char.toLowerCase() in TypeSizeMap) {
            // A simple integer
            let byteCount = TypeSizeMap[char.toLowerCase()];
            for (let i = 0; i < Math.max(1, repeat); i++) {
                let resultingInteger = 0;
                let bytes = data.slice(dataIndex, dataIndex + byteCount);
                dataIndex += byteCount;
                // Form an integer
                for (let j = 0; j < byteCount; j++) {
                    let index = endianness === Endianness.LITTLE ? (byteCount - 1 - j) : j;
                    let byte = bytes[index];
                    resultingInteger = (resultingInteger << 8) | byte;
                }
                // Apply two's compliment if needed.
                if (char.toLowerCase() !== char) {
                    // Is upper-case, so unsigned
                    resultingInteger >>>= 0;
                }
                output.push(resultingInteger);
            }
            repeat = 0;
        }
        else if (char == 's') {
            // String.
            output.push(data.slice(dataIndex, dataIndex += repeat));
            repeat = 0;
        }
        else {
            console.log(\`Unknown format char: \${char}\`);
        }
    }
    return output;
}
exports.structFormatUnpack = structFormatUnpack;`,
        `function structFormatUnpack(format, data, offset = 0) {
    const output = [];
    let littleEndian = false;
    let fi = 0;
    const flen = format.length;
    if (flen > 0) {
        const c0 = format.charCodeAt(0);
        if (c0 === 60 || c0 === 62 || c0 === 64) { // '<', '>', '@'
            littleEndian = (c0 === 60);
            fi = 1;
        }
    }
    let repeat = 0;
    let dataIndex = offset;
    while (fi < flen) {
        const code = format.charCodeAt(fi);
        fi += 1;
        if (code >= 48 && code <= 57) { // '0'..'9'
            repeat = repeat * 10 + (code - 48);
            continue;
        }
        if (code === 120) { // 'x'
            dataIndex += repeat > 0 ? repeat : 1;
            repeat = 0;
            continue;
        }
        if (code === 115) { // 's'
            const end = dataIndex + repeat;
            output.push(data.slice(dataIndex, end));
            dataIndex = end;
            repeat = 0;
            continue;
        }
        const isUpper = code >= 65 && code <= 90;
        const lower = isUpper ? code + 32 : code;
        let byteCount = 0;
        if (lower === 98 || lower === 99) byteCount = 1; // b, c
        else if (lower === 104) byteCount = 2; // h
        else if (lower === 105 || lower === 108) byteCount = 4; // i, l
        else if (lower === 113) byteCount = 8; // q
        if (byteCount > 0) {
            const count = repeat > 0 ? repeat : 1;
            for (let i = 0; i < count; i++) {
                let resultingInteger = 0;
                for (let j = 0; j < byteCount; j++) {
                    const idx = littleEndian ? (dataIndex + byteCount - 1 - j) : (dataIndex + j);
                    resultingInteger = (resultingInteger << 8) | (data[idx] & 0xff);
                }
                dataIndex += byteCount;
                if (isUpper) {
                    resultingInteger = resultingInteger >>> 0;
                }
                output.push(resultingInteger);
            }
            repeat = 0;
        } else {
            throw new Error('Unknown format char code: ' + code);
        }
    }
    return output;
}
exports.structFormatUnpack = structFormatUnpack;`,
        'nufatfs/utils.js structFormatUnpack rewrite'
      )
    }

    if (rel === 'high-level.js') {
      // Fix upstream typo in rename(): oldParentPath used newPath.lastIndexOf("/") instead of path.lastIndexOf("/").
      code = replaceOrThrow(
        code,
        `        lastSlash = newPath.lastIndexOf("/");
        let oldParentPath = path.slice(0, lastSlash);
        const oldParent = await this.fat.traverse(oldParentPath);`,
        `        const oldLastSlash = path.lastIndexOf("/");
        let oldParentPath = oldLastSlash >= 0 ? path.slice(0, oldLastSlash) : "";
        const oldParent = oldParentPath ? await this.fat.traverse(oldParentPath) : this.fat.root;`,
        'nufatfs/high-level.js rename oldParentPath fix'
      )
    }

    fs.writeFileSync(path.join(stagedDir, 'nufatfs', rel), code, 'utf8')
  }

  fs.writeFileSync(
    path.join(stagedDir, 'nufatfs', 'index.js'),
    `"use strict";
module.exports = {
  ...require("./high-level"),
  ...require("./low-level"),
  ...require("./chunking-fs-driver"),
  ...require("./mbr-driver"),
  ...require("./overlay-driver"),
};
`,
    'utf8'
  )

  // -------------------------------------------------------------------------
  // 3. Stage node-mass-storage/dist with SCSI transport delegation
  // -------------------------------------------------------------------------
  const nmsDist = path.join(himdDepsModules, 'node-mass-storage', 'dist')
  const nmsHelpers = fs.readFileSync(path.join(nmsDist, 'helpers.js'), 'utf8')
  fs.writeFileSync(path.join(stagedDir, 'node-mass-storage', 'helpers.js'), nmsHelpers, 'utf8')
  fs.writeFileSync(
    path.join(stagedDir, 'node-mass-storage', 'index.js'),
    `"use strict";
module.exports = {
  ...require("./helpers"),
  ...require("./usb-mass-storage"),
};
`,
    'utf8'
  )

  let nmsUms = fs
    .readFileSync(path.join(nmsDist, 'usb-mass-storage.js'), 'utf8')
    .replaceAll('\r\n', '\n')

  // Replace low-level USB BOT CBW/CSW transfer methods with clean SCSI pass-through
  // calling this.usbDevice.scsiCommand(cdb, cdbLen, direction, dataOrLen).
  // All high-level SCSI CDB builders (inquiry, getCapacity, readBlocks, writeBlocks,
  // getSense, createArbitraryNUFatFSVolumeDriver, CDB_LEN_TABLE) remain intact!
  const sendMassStart = nmsUms.indexOf('    sendMassStorageOutCommand(')
  const getSenseStart = nmsUms.indexOf('    getSense(')
  const getStatusStart = nmsUms.indexOf('    getMassStorageStatus(')
  const inquiryStart = nmsUms.indexOf('    inquiry(')
  const closeStart = nmsUms.indexOf('    close(')
  const closeEnd = nmsUms.indexOf('}\nexports.USBMassStorageDriver')
  if (
    sendMassStart === -1 ||
    getSenseStart === -1 ||
    getStatusStart === -1 ||
    inquiryStart === -1 ||
    closeStart === -1 ||
    closeEnd === -1
  ) {
    throw new Error('[himd-stage] Could not locate USBMassStorageDriver method boundaries')
  }

  const getSenseSection = nmsUms.slice(getSenseStart, getStatusStart)
  const midSection = nmsUms.slice(inquiryStart, closeStart)

  const lowLevelScsiReplacement = `    __prepareCdb(cdb, cdbLength) {
        if (cdbLength === undefined) {
            cdbLength = exports.CDB_LEN_TABLE[cdb[0]];
        }
        if (!cdbLength || cdbLength > 16) {
            throw new MassStorageError("Unknown / invalid command: " + cdb[0]);
        }
        if (this.usbSubclass !== USBMassStorageSubclass.REDUCED_BLOCK_COMMANDS &&
            this.usbSubclass !== USBMassStorageSubclass.TRANSPARENT) {
            cdbLength = Math.max(12, cdbLength);
        }
        const out = new Uint8Array(cdbLength);
        const copyLen = Math.min(cdb.length, cdbLength);
        for (let i = 0; i < copyLen; i++) out[i] = cdb[i];
        return out;
    }
    async sendMassStorageOutCommand(cdb, inputDataLength, cdbLength) {
        const prepared = this.__prepareCdb(cdb, cdbLength);
        if ((inputDataLength | 0) === 0) {
            this.usbDevice.scsiCommand(prepared, prepared.length, 0, 0);
        }
        return { len: 0, expectedTag: this.tag++ };
    }
    async sendMassStorageInCommand(cdb, outputDataLength, cdbLength) {
        const prepared = this.__prepareCdb(cdb, cdbLength);
        if ((outputDataLength | 0) === 0) {
            this.usbDevice.scsiCommand(prepared, prepared.length, 0, 0);
        }
        return { len: 0, expectedTag: this.tag++ };
    }
    async _getStatus() {
        return 0;
    }
    async sendCommandInGetResult(cdb, length, canStall = false, cdbLength) {
        const prepared = this.__prepareCdb(cdb, cdbLength);
        const wantLen = length | 0;
        const dir = wantLen > 0 ? 1 : 0;
        const result = this.usbDevice.scsiCommand(prepared, prepared.length, dir, wantLen);
        return { result: result || new Uint8Array(0), status: 0 };
    }
    async sendCommandOutGetResult(cdb, data, cdbLength) {
        const prepared = this.__prepareCdb(cdb, cdbLength);
        const payload = data instanceof Uint8Array ? data : new Uint8Array(data || 0);
        const written = this.usbDevice.scsiCommand(prepared, prepared.length, 2, payload);
        return { result: { status: 'ok', bytesWritten: written }, status: 0 };
    }
`

  const lifecycleScsiReplacement = `    async init() {
        await this.usbDevice.open();
    }
    async runBOMSR() {
        this.usbDevice.scsiReset();
    }
    async getMaxLun() {
        return 0;
    }
`

  const closeReplacement = `    async close() {
        await this.usbDevice.close();
    }
`

  nmsUms =
    nmsUms.slice(0, sendMassStart) +
    lowLevelScsiReplacement +
    getSenseSection +
    lifecycleScsiReplacement +
    midSection +
    closeReplacement +
    nmsUms.slice(closeEnd)

  // Lower remaining __awaiter(..., function* () { yield ... }) in getSense/inquiry/readBlocks/writeBlocks.
  nmsUms = lowerTsAwaiterGenerators(babel, nmsUms)

  fs.writeFileSync(path.join(stagedDir, 'node-mass-storage', 'usb-mass-storage.js'), nmsUms, 'utf8')

  // -------------------------------------------------------------------------
  // 4. Emit pure-JS node_modules shims
  // -------------------------------------------------------------------------
  fs.writeFileSync(
    path.join(shimsDir, 'async-mutex.js'),
    `export class Mutex {
  constructor() { this._locked = false; }
  async acquire() {
    this._locked = true;
    return () => { this._locked = false; };
  }
  async runExclusive(fn) {
    const release = await this.acquire();
    try { return await fn(); } finally { release(); }
  }
}
export default { Mutex };
`,
    'utf8'
  )

  fs.writeFileSync(
    path.join(shimsDir, 'empty-stub.js'),
    `export function create() { return new Uint8Array(0); }
export function readTags() { return []; }
export function readFrame() { return null; }
export default {};
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
      return new Uint8Array(value.buffer.slice(value.byteOffset || 0, (value.byteOffset || 0) + value.byteLength));
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
  const host = globalThis.__cartaHiMdHost;
  if (!host || !host.cp932) throw new Error('Host CP932 encoder unavailable');
  return host.cp932(0, utf8);
}
function decode(buf, encoding) {
  const bytes = buf instanceof Uint8Array ? buf : new Uint8Array(buf || 0);
  if (bytes.length === 0) return '';
  const host = globalThis.__cartaHiMdHost;
  if (!host || !host.cp932) throw new Error('Host CP932 decoder unavailable');
  const utf8 = host.cp932(1, bytes);
  return utf8Decode(utf8);
}
export { encode, decode };
export default { encode, decode };
`,
    'utf8'
  )

  fs.writeFileSync(
    path.join(shimsDir, 'iconv-lite.js'),
    `import jconv from './jconv.js';

function normEncoding(name) {
  return String(name || '').toLowerCase().replaceAll('-', '').replaceAll('_', '');
}

function toU8(buf) {
  if (buf instanceof Uint8Array) return buf;
  if (buf instanceof ArrayBuffer) return new Uint8Array(buf);
  if (buf && buf.buffer instanceof ArrayBuffer && typeof buf.byteLength === 'number') {
    return new Uint8Array(buf.buffer, buf.byteOffset || 0, buf.byteLength);
  }
  return new Uint8Array(buf || 0);
}

export function getEncoder(encoding) {
  const enc = normEncoding(encoding);
  return {
    write(content) {
      const s = String(content == null ? '' : content);
      if (enc === 'latin1' || enc === 'iso88591') {
        const out = new Uint8Array(s.length);
        for (let i = 0; i < s.length; i++) {
          const c = s.charCodeAt(i);
          if (c > 0xff) {
            throw new Error('Non-Latin1 code unit');
          }
          out[i] = c & 0xff;
        }
        return out;
      }
      if (enc === 'utf16be' || enc === 'ucs2be') {
        const out = new Uint8Array(s.length * 2);
        for (let i = 0; i < s.length; i++) {
          const c = s.charCodeAt(i);
          out[i * 2] = (c >>> 8) & 0xff;
          out[i * 2 + 1] = c & 0xff;
        }
        return out;
      }
      if (enc === 'sjis' || enc === 'shiftjis' || enc === 'cp932') {
        return jconv.encode(s, 'sjis');
      }
      throw new Error('Unsupported iconv-lite encoder: ' + encoding);
    },
  };
}

export function decode(buf, encoding) {
  const enc = normEncoding(encoding);
  const u8 = toU8(buf);
  if (enc === 'latin1' || enc === 'iso88591') {
    let out = '';
    for (let i = 0; i < u8.length; i++) {
      out += String.fromCharCode(u8[i]);
    }
    return out;
  }
  if (enc === 'utf16be' || enc === 'ucs2be') {
    let out = '';
    for (let i = 0; i + 1 < u8.length; i += 2) {
      out += String.fromCharCode((u8[i] << 8) | u8[i + 1]);
    }
    return out;
  }
  if (enc === 'sjis' || enc === 'shiftjis' || enc === 'cp932') {
    return jconv.decode(u8, 'sjis');
  }
  throw new Error('Unsupported iconv-lite decoder: ' + encoding);
}

export default { getEncoder, decode };
`,
    'utf8'
  )

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
class WordArray {
  constructor(words, sigBytes) {
    this.words = words || [];
    this.sigBytes = sigBytes !== undefined ? sigBytes : this.words.length * 4;
  }
  toString() {
    return bytesToHex(wordArrayToBytes(this));
  }
}
WordArray.create = function (v) {
  if (v instanceof WordArray) return v;
  if (v instanceof Uint8Array) return bytesToWordArray(v);
  if (v instanceof ArrayBuffer) return bytesToWordArray(new Uint8Array(v));
  if (Array.isArray(v)) return new WordArray(v.slice(), v.length * 4);
  if (v && v.buffer instanceof ArrayBuffer) {
    return bytesToWordArray(new Uint8Array(v.buffer, v.byteOffset || 0, v.byteLength));
  }
  return new WordArray([], 0);
};
WordArray.random = function (nBytes) {
  const host = globalThis.__cartaHiMdHost;
  if (!host || !host.randomBytes) throw new Error('Host randomBytes unavailable');
  return bytesToWordArray(host.randomBytes(nBytes | 0));
};
const CipherParams = {
  create(obj) {
    return { ciphertext: obj && obj.ciphertext ? obj.ciphertext : new WordArray([], 0) };
  },
};
const enc = {
  Hex: {
    _kind: 'Hex',
    parse: hexToWordArray,
    stringify(wa) { return bytesToHex(wordArrayToBytes(wa)); },
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
  const host = globalThis.__cartaHiMdHost;
  if (!host || !host.desCrypt) throw new Error('Host desCrypt unavailable');
  const wa = extractWordArray(dataOrParams, !isEncrypt);
  const rawIn = wordArrayToBytes(wa);
  const keyWa = extractWordArray(keyArg, false);
  const fullKeyBytes = wordArrayToBytes(keyWa);
  // CryptoJS single-DES only uses the first 8 key bytes even if given a 16-byte Pkcs7 key.
  const keyBytes = isTriple ? fullKeyBytes : fullKeyBytes.subarray(0, 8);
  const isCbc = cfg && cfg.mode === 'CBC' ? 1 : 0;
  let ivBytes = null;
  if (isCbc) {
    const ivWa = cfg.iv ? extractWordArray(cfg.iv, false) : new WordArray([0, 0], 8);
    ivBytes = wordArrayToBytes(ivWa, 8);
  }
  // Treat custom NO_PADDING objects { pad, unpad } or explicit 'NoPadding' as non-Pkcs7.
  const padCfg = cfg ? cfg.padding : undefined;
  const usePkcs7 = padCfg === undefined || padCfg === 'Pkcs7';
  if (isEncrypt) {
    let padded;
    if (usePkcs7) {
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
  if (usePkcs7 && out.length > 0 && out.length % 8 === 0) {
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
const CryptoJS = { lib: { WordArray, CipherParams }, enc, mode, pad, DES, TripleDES, algo };
export { DES, TripleDES, algo, enc, mode, pad };
export default CryptoJS;
`,
    'utf8'
  )
}

// ---------------------------------------------------------------------------
// Bundle + Babel async-to-SyncPromise lowering + Node sanity self-test
// ---------------------------------------------------------------------------

async function writeCoreEntryAndBundle(babel, esbuild) {
  const preludePath = path.join(workDir, 'himd_prelude.js')
  const coreEntryPath = path.join(workDir, 'himd_core_entry.js')
  const bundledPath = path.join(workDir, 'himd_bundled.mjs')
  const loweredPath = path.join(workDir, 'himd_lowered.mjs')

  const preludeCode = `
// Executed BEFORE any Hi-MD / nufatfs / node-mass-storage modules evaluate.
let __promiseJobs = [];
let __promiseJobHead = 0;
let __drainingPromiseJobs = false;
export function __resetPromiseJobs() {
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

export class __CartaSyncPromise {
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
export function __syncResolve(v) { return __CartaSyncPromise.resolve(v); }
export function __syncReject(e) { return __CartaSyncPromise.reject(e); }
export function __syncAwait(v) {
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
export function __cartaSyncAwaiter(thisArg, _arguments, P, generator) {
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

export function __utf8EncodeImpl(str) {
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
export function __utf8DecodeImpl(buf) {
  let bytes;
  if (buf instanceof Uint8Array) bytes = buf;
  else if (buf instanceof ArrayBuffer) bytes = new Uint8Array(buf);
  else if (buf && buf.buffer instanceof ArrayBuffer) bytes = new Uint8Array(buf.buffer, buf.byteOffset || 0, buf.byteLength);
  else bytes = new Uint8Array(buf || 0);
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
class __CartaTextEncoder {
  encode(str) { return __utf8EncodeImpl(str); }
}
class __CartaTextDecoder {
  decode(buf) { return __utf8DecodeImpl(buf); }
}
globalThis.TextEncoder = __CartaTextEncoder;
globalThis.TextDecoder = __CartaTextDecoder;
const __CartaBuffer = {
  from(value) {
    if (typeof value === 'string') return __utf8EncodeImpl(value);
    if (value instanceof Uint8Array) return new Uint8Array(value);
    if (value instanceof ArrayBuffer) return new Uint8Array(value);
    if (value && value.buffer instanceof ArrayBuffer && typeof value.byteLength === 'number') {
      return new Uint8Array(value.buffer.slice(value.byteOffset || 0, (value.byteOffset || 0) + value.byteLength));
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
  },
};
globalThis.Buffer = __CartaBuffer;
`
  fs.writeFileSync(preludePath, preludeCode, 'utf8')

  const coreEntryCode = `
import {
  __CartaSyncPromise,
  __syncAwait,
  __syncResolve,
  __syncReject,
  __cartaSyncAwaiter,
  __resetPromiseJobs,
  __utf8EncodeImpl,
  __utf8DecodeImpl,
} from ${JSON.stringify(preludePath)};
import * as himdJs from ${JSON.stringify(path.join(stagedDir, 'himd-js', 'index.js'))};

const {
  HiMD,
  HiMDError,
  HiMDCodec,
  HiMDStringType,
  DOSTIME_NULL,
  BLOCK_SIZE,
  HIMD_AUDIO_SIZE,
  UMSCHiMDFilesystem,
  HiMDSecureSession,
  generateCodecInfo,
  getBytesPerFrame,
  getCodecName,
  getKBPS,
  getSeconds,
  getFramesPerBlock,
  getAllTracks,
  getGroups,
  renameDisc,
  renameTrack,
  moveTrack,
  rewriteGroups,
  deleteTracks,
  createEA3Header,
  createRandomBytes,
  getUint32,
  retailMac,
  createIcvMac,
  createTrackMac,
  encryptTrackKey,
  createTrackKey,
  encryptBlock,
  decryptBlock,
  decryptMaclistKey,
} = himdJs;

function hostOrThrow() {
  const h = globalThis.__cartaHiMdHost;
  if (!h) throw new Error('Hi-MD host bridge not initialized');
  return h;
}

function reportHostStep(done, total) {
  const h = globalThis.__cartaHiMdHost;
  if (h && h.reportStep) h.reportStep(done | 0, total | 0);
}

function toHex2(n) {
  const hex = '0123456789abcdef';
  return hex.charAt((n >>> 4) & 15) + hex.charAt(n & 15);
}

function bytesToHexStr(bytes) {
  let out = '';
  for (let i = 0; i < bytes.length; i++) {
    out += toHex2(bytes[i] & 0xff);
  }
  return out;
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

class HostScsiDevice {
  constructor(info) {
    this.deviceId = info.id;
    this.vendorId = info.vendorId | 0;
    this.productId = info.productId | 0;
    this.productName = info.name || '';
    this.opened = false;
    this.handle = -1;
  }
  async open() {
    if (this.opened && this.handle >= 0) return;
    this.handle = hostOrThrow().scsiOpen(this.deviceId);
    this.opened = true;
  }
  async close() {
    if (this.handle >= 0) {
      const h = this.handle;
      this.handle = -1;
      this.opened = false;
      hostOrThrow().scsiClose(h);
    }
  }
  scsiReset() {
    if (this.handle >= 0) {
      hostOrThrow().scsiReset(this.handle);
    }
  }
  scsiCommand(cdb, cdbLen, direction, dataOrLen) {
    return hostOrThrow().scsiCommand(this.handle, cdb, cdbLen | 0, direction | 0, dataOrLen);
  }
}

function findDeviceById(deviceId) {
  const listJson = hostOrThrow().scsiListJson();
  const list = JSON.parse(listJson);
  for (let i = 0; i < list.length; i++) {
    if (list[i].id === deviceId) return list[i];
  }
  if (typeof deviceId === 'string' && deviceId.indexOf('usb:') === 0) {
    return {
      id: deviceId,
      vendorId: 0,
      productId: 0,
      name: 'Hi-MD USB Device',
      isMock: false,
      transport: 'libusb-bot',
    };
  }
  throw new Error('Hi-MD device not found: ' + deviceId);
}

async function openHiMdSession(deviceId) {
  const info = findDeviceById(deviceId);
  const dev = new HostScsiDevice(info);
  const fs = new UMSCHiMDFilesystem(dev);
  await fs.init(false);
  const himd = await HiMD.init(fs);
  return { info, dev, fs, himd };
}

function parseCodecSpec(spec) {
  const key = String(spec || '').trim().toLowerCase();
  switch (key) {
    case 'pcm':
    case 'lpcm':
      return { codecInfo: generateCodecInfo('PCM', 64, 2, 44100), typeString: 'LPCM' };
    case 'hi-sp':
    case 'hisp':
    case 'at3p-256':
      return { codecInfo: generateCodecInfo('A3+', 1488, 2, 44100), typeString: 'ATX ' };
    case 'hi-lp':
    case 'hilp':
    case 'at3p-64':
      return { codecInfo: generateCodecInfo('A3+', 376, 2, 44100), typeString: 'ATX ' };
    case 'at3p-352':
      return { codecInfo: generateCodecInfo('A3+', 2048, 2, 44100), typeString: 'ATX ' };
    case 'at3p-192':
      return { codecInfo: generateCodecInfo('A3+', 1120, 2, 44100), typeString: 'ATX ' };
    case 'at3p-48':
      return { codecInfo: generateCodecInfo('A3+', 280, 2, 44100), typeString: 'ATX ' };
    case 'lp2':
    case 'at3-132':
      return { codecInfo: generateCodecInfo('AT3', 384, 2, 44100), typeString: 'A3D ' };
    case 'at3-105':
      return { codecInfo: generateCodecInfo('AT3', 304, 2, 44100), typeString: 'A3D ' };
    case 'lp4':
    case 'at3-66':
      return { codecInfo: generateCodecInfo('AT3', 192, 2, 44100), typeString: 'A3D ' };
    default:
      throw new Error('Unsupported Hi-MD codec spec: ' + spec);
  }
}

function writeU32LE(buf, off, v) {
  buf[off + 0] = v & 0xff;
  buf[off + 1] = (v >>> 8) & 0xff;
  buf[off + 2] = (v >>> 16) & 0xff;
  buf[off + 3] = (v >>> 24) & 0xff;
}

function createExactWavHeader(payloadBytes) {
  const hdr = new Uint8Array([
    0x52, 0x49, 0x46, 0x46, 0x00, 0x00, 0x00, 0x00,
    0x57, 0x41, 0x56, 0x45, 0x66, 0x6d, 0x74, 0x20,
    0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x44, 0xac, 0x00, 0x00, 0x10, 0xb1, 0x02, 0x00,
    0x04, 0x00, 0x10, 0x00, 0x64, 0x61, 0x74, 0x61,
    0x00, 0x00, 0x00, 0x00,
  ]);
  writeU32LE(hdr, 4, (36 + payloadBytes) >>> 0);
  writeU32LE(hdr, 40, payloadBytes >>> 0);
  return hdr;
}

export async function himdListDevices() {
  return hostOrThrow().scsiListJson();
}

export async function himdProbeDevice(deviceId) {
  const { info, fs, himd } = await openHiMdSession(deviceId);
  try {
    const inquiry = await fs.driver.inquiry();
    const capacity = await fs.driver.getCapacity();
    const fsStats = await fs.statFilesystem();
    const rawTracks = getAllTracks(himd);
    const tracks = [];
    for (let i = 0; i < rawTracks.length; i++) {
      const tr = rawTracks[i];
      tracks.push({
        index: tr.index,
        slot: himd.trackIndexToTrackSlot(tr.index),
        title: tr.title,
        artist: tr.artist,
        album: tr.album,
        encoding: tr.encoding,
        bitrate: tr.bitrate,
        duration: tr.duration,
      });
    }
    const rawGroups = getGroups(himd);
    const groups = [];
    for (let i = 0; i < rawGroups.length; i++) {
      const g = rawGroups[i];
      groups.push({
        groupIndex: g.groupIndex,
        title: g.title,
        startIndex: g.startIndex,
        trackCount: g.tracks.length,
      });
    }
    return asciiJsonStringify({
      deviceId: info.id,
      deviceName: himd.getDeviceName() || info.name || 'Hi-MD',
      inquiry: {
        vid: String(inquiry.vid || '').trim(),
        pid: String(inquiry.pid || '').trim(),
        rev: String(inquiry.rev || '').trim(),
      },
      capacity: {
        maxLba: capacity.maxLba,
        blockSize: capacity.blockSize,
        deviceSize: capacity.deviceSize,
      },
      filesystem: {
        total: fsStats.total,
        used: fsStats.used,
        left: fsStats.left,
      },
      datanum: himd.datanum,
      discId: bytesToHexStr(himd.getDiscId()),
      discTitle: himd.getDiscTitle(),
      trackCount: himd.getTrackCount(),
      tracks,
      groups,
    });
  } finally {
    await fs.driver.close();
  }
}

export async function himdUploadTrackFromFile(deviceId, codecSpec, filePath, title, artist, album) {
  const h = hostOrThrow();
  const { codecInfo, typeString } = parseCodecSpec(codecSpec);
  const bytesPerFrame = getBytesPerFrame(codecInfo);
  const maxBytesInBlock = HIMD_AUDIO_SIZE - (typeString === 'LPCM' ? 0 : 1);
  const framesPerBlockCap = Math.floor(maxBytesInBlock / bytesPerFrame);
  if (framesPerBlockCap <= 0) {
    throw new Error('Invalid Hi-MD frame/block geometry');
  }
  const blockPayloadCap = framesPerBlockCap * bytesPerFrame;
  const type = __utf8EncodeImpl(typeString);
  const mCode = typeString === 'LPCM' ? 0x0124 : 3;

  const fd = h.fileOpen(filePath, 0);
  let closedFile = false;
  try {
    const fileSize = h.fileSize(fd);
    if (fileSize <= 0) {
      throw new Error('Hi-MD upload input file is empty: ' + filePath);
    }

    const { fs, himd } = await openHiMdSession(deviceId);
    try {
      const session = new HiMDSecureSession(himd, fs.driver);
      await session.performAuthentication();

      const slot = himd.getNextFreeTrackSlot();
      const totalFramesEst = Math.ceil(fileSize / bytesPerFrame);
      const trackData = {
        albumIndex: 0,
        artistIndex: 0,
        titleIndex: 0,
        trackNumber: slot,
        firstFragment: 0,
        ekbNumber: 0x00010012,
        cc: 68,
        cn: 0,
        ct: 0,
        dest: 0,
        lt: 1,
        xcc: 1,
        key: new Uint8Array(0),
        mac: new Uint8Array(0),
        contentId: new Uint8Array(0),
        licenseEndTime: DOSTIME_NULL,
        licenseStartTime: DOSTIME_NULL,
        seconds: Math.round(getSeconds(codecInfo, totalFramesEst)),
        recordingTime: DOSTIME_NULL,
        trackInAlbum: 0,
        codecId: codecInfo.codecId,
        codecInfo: codecInfo.codecInfo,
      };
      const cryptoSignedData = await session.createAndSignNewTrack(trackData);
      const keyForFragment = createRandomBytes(8);
      const lo32ContentId = getUint32(cryptoSignedData.contentId, 16) >>> 0;

      const writeStream = await himd.openWriteStream();
      writeStream.setKeys(cryptoSignedData.trackKey, keyForFragment);

      // Single reused read buffer keeps Porffor bump arena O(1) across large tracks.
      const readBuf = new Uint8Array(blockPayloadCap);
      const ivZero = new Uint8Array(8).fill(0);
      const resvZero = new Uint8Array(4).fill(0);

      let blockIdx = 0;
      let bytesReadTotal = 0;
      let lastFrameInFragment = 0;
      const estBlocks = Math.max(1, Math.ceil(fileSize / blockPayloadCap));

      reportHostStep(0, estBlocks);
      while (true) {
        const nRead = h.fileRead(fd, readBuf, blockPayloadCap);
        if (nRead <= 0) break;
        const framesInThisBlock = Math.floor(nRead / bytesPerFrame);
        if (framesInThisBlock === 0) break;
        const bytesInThisBlock = framesInThisBlock * bytesPerFrame;
        bytesReadTotal += bytesInThisBlock;
        lastFrameInFragment = framesInThisBlock - 1;

        const keyForBlock = createRandomBytes(8);
        const audioSlice =
          bytesInThisBlock === blockPayloadCap
            ? readBuf
            : readBuf.subarray(0, bytesInThisBlock);

        const audioBlock = {
          audioData: audioSlice,
          backupKey: keyForBlock,
          backupMCode: mCode,
          backupReserved: 0,
          backupSerialNumber: blockIdx,
          backupType: type,
          iv: ivZero,
          key: keyForBlock,
          lendata: 0,
          lo32ContentId,
          mCode,
          nFrames: 0,
          reserved1: 0,
          reserved2: resvZero,
          serialNumber: blockIdx,
          type,
        };
        await writeStream.writeAndEncryptAudioBlock(audioBlock);
        blockIdx++;
        reportHostStep(Math.min(blockIdx, estBlocks), estBlocks);
      }

      if (blockIdx === 0) {
        await writeStream.close();
        throw new Error('Input file contained fewer than one complete Hi-MD audio frame');
      }

      const { firstBlock, lastBlock } = await writeStream.close();
      const fragment = {
        firstBlock,
        lastBlock,
        key: keyForFragment,
        nextFragment: 0,
        fragmentType: 0,
        firstFrame: 0,
        lastFrame: Math.max(0, lastFrameInFragment),
      };
      trackData.firstFragment = himd.addFragment(fragment);
      if (title && title.length > 0) trackData.titleIndex = himd.addString(title, HiMDStringType.TITLE);
      if (artist && artist.length > 0) trackData.artistIndex = himd.addString(artist, HiMDStringType.ARTIST);
      if (album && album.length > 0) trackData.albumIndex = himd.addString(album, HiMDStringType.ALBUM);

      const newTrackSlot = himd.addTrack(trackData);
      const trackIndex = himd.getTrackCount();
      himd.writeTrackIndexToTrackSlot(trackIndex, newTrackSlot);
      himd.writeTrackCount(trackIndex + 1);
      await himd.flush();
      await session.finalizeSession();
      reportHostStep(estBlocks, estBlocks);

      return asciiJsonStringify({
        trackIndex,
        slot: newTrackSlot,
        blocksWritten: blockIdx,
        bytesWritten: bytesReadTotal,
        datanum: himd.datanum,
      });
    } finally {
      await fs.driver.close();
    }
  } finally {
    if (!closedFile) {
      closedFile = true;
      h.fileClose(fd);
    }
  }
}

export async function himdDumpTrackToFile(deviceId, trackIndex, outFilePath, rawAudioOnly) {
  const h = hostOrThrow();
  const { fs, himd } = await openHiMdSession(deviceId);
  const fd = h.fileOpen(outFilePath, 1);
  try {
    const count = himd.getTrackCount();
    if (trackIndex < 0 || trackIndex >= count) {
      throw new HiMDError('Track index out of range: ' + trackIndex);
    }
    const slot = himd.trackIndexToTrackSlot(trackIndex);
    const track = himd.getTrack(slot);
    if (!track || track.firstFragment === 0) {
      throw new HiMDError('No such track slot: ' + slot);
    }
    const codecName = getCodecName(track);
    if (codecName === 'MP3') {
      throw new HiMDError('MP3 dump is not supported by this helper');
    }
    const bitrate = getKBPS(track);
    const bytesPerFrame = getBytesPerFrame(track);
    const framesPerBlock = getFramesPerBlock(track);

    const nonMP3Stream = await himd.openNonMP3Stream(slot);
    try {
      const frags = nonMP3Stream.blockStream.fragments;
      let totalBlocks = 0;
      let exactPayloadBytes = 0;
      for (let i = 0; i < frags.length; i++) {
        const f = frags[i];
        totalBlocks += f.lastBlock - f.firstBlock + 1;
        const framesInFrag = (f.lastBlock - f.firstBlock) * framesPerBlock + (f.lastFrame - f.firstFrame + 1);
        exactPayloadBytes += framesInFrag * bytesPerFrame;
      }

      let totalWritten = 0;
      if (!rawAudioOnly) {
        if (codecName === 'PCM') {
          const wavHdr = createExactWavHeader(exactPayloadBytes);
          totalWritten += h.fileWrite(fd, wavHdr, wavHdr.length);
        } else {
          const ea3Hdr = createEA3Header(track);
          totalWritten += h.fileWrite(fd, ea3Hdr, ea3Hdr.length);
        }
      }

      let blockDone = 0;
      reportHostStep(0, Math.max(1, totalBlocks));
      let block;
      while ((block = await nonMP3Stream.readBlock()) !== null) {
        const data = block.block;
        if (!rawAudioOnly && codecName === 'PCM') {
          for (let i = 0; i + 1 < data.length; i += 2) {
            const tmp = data[i];
            data[i] = data[i + 1];
            data[i + 1] = tmp;
          }
        }
        if (data.length > 0) {
          totalWritten += h.fileWrite(fd, data, data.length);
        }
        blockDone++;
        reportHostStep(blockDone, Math.max(1, totalBlocks));
      }

      return asciiJsonStringify({
        trackIndex,
        encoding: codecName,
        bitrate,
        bytesWritten: totalWritten,
      });
    } finally {
      await nonMP3Stream.blockStream.close();
    }
  } finally {
    h.fileClose(fd);
    await fs.driver.close();
  }
}

export async function himdRenameDisc(deviceId, title) {
  const { fs, himd } = await openHiMdSession(deviceId);
  try {
    renameDisc(himd, title && title.length > 0 ? title : null);
    await himd.flush();
  } finally {
    await fs.driver.close();
  }
}

export async function himdRenameTrack(
  deviceId,
  trackIndex,
  hasTitle,
  title,
  hasArtist,
  artist,
  hasAlbum,
  album
) {
  const { fs, himd } = await openHiMdSession(deviceId);
  try {
    const count = himd.getTrackCount();
    if (trackIndex < 0 || trackIndex >= count) {
      throw new HiMDError('Track index out of range: ' + trackIndex);
    }
    const patch = {};
    if (hasTitle) patch.title = title;
    if (hasArtist) patch.artist = artist;
    if (hasAlbum) patch.album = album;
    renameTrack(himd, trackIndex, patch);
    await himd.flush();
  } finally {
    await fs.driver.close();
  }
}

export async function himdMoveTrack(deviceId, fromIndex, toIndex) {
  const { fs, himd } = await openHiMdSession(deviceId);
  try {
    const count = himd.getTrackCount();
    if (fromIndex < 0 || fromIndex >= count || toIndex < 0 || toIndex >= count) {
      throw new HiMDError('Track move index out of range');
    }
    moveTrack(himd, fromIndex, toIndex);
    await himd.flush();
  } finally {
    await fs.driver.close();
  }
}

export async function himdSetGroups(deviceId, groupsJson) {
  const parsed = JSON.parse(groupsJson);
  if (!Array.isArray(parsed)) {
    throw new Error('Expected JSON array for Hi-MD groups');
  }
  const { fs, himd } = await openHiMdSession(deviceId);
  try {
    rewriteGroups(himd, parsed);
    await himd.flush();
  } finally {
    await fs.driver.close();
  }
}

export async function himdDeleteTracks(deviceId, indicesJson) {
  const indices = JSON.parse(indicesJson);
  if (!Array.isArray(indices)) {
    throw new Error('Expected JSON array for delete-tracks indices');
  }
  const { fs, himd } = await openHiMdSession(deviceId);
  try {
    const count = himd.getTrackCount();
    const deletedSlots = [];
    for (let i = 0; i < indices.length; i++) {
      const idx = indices[i] | 0;
      if (idx < 0 || idx >= count) {
        throw new HiMDError('Delete track index out of range: ' + idx);
      }
      deletedSlots.push(himd.trackIndexToTrackSlot(idx));
    }
    const session = new HiMDSecureSession(himd, fs.driver);
    await session.performAuthentication();
    await deleteTracks(himd, indices.slice());
    const zeroMac = new Uint8Array(8).fill(0);
    for (let i = 0; i < deletedSlots.length; i++) {
      const slot = deletedSlots[i];
      if (slot >= 1) {
        session.allMacs.set(zeroMac, (slot - 1) * 8);
      }
    }
    await session.finalizeSession();
  } finally {
    await fs.driver.close();
  }
}

export async function himdWipeDisc(deviceId, reformat) {
  if (reformat) {
    const info = findDeviceById(deviceId);
    const dev = new HostScsiDevice(info);
    const fs = new UMSCHiMDFilesystem(dev);
    await fs.driver.init();
    try {
      await fs.driver.runBOMSR();
      await fs.driver.inquiry();
      await fs.driver.testUnitReady();
      await fs.driver.reformatHiMD();
      await fs.initFS(false);
      await HiMD.init(fs);
    } finally {
      await fs.driver.close();
    }
    return;
  }
  const { fs, himd } = await openHiMdSession(deviceId);
  try {
    await himd.wipe(false);
  } finally {
    await fs.driver.close();
  }
}

export {
  himdJs,
  retailMac,
  createIcvMac,
  createTrackMac,
  encryptTrackKey,
  createTrackKey,
  encryptBlock,
  decryptBlock,
  decryptMaclistKey,
};
`
  fs.writeFileSync(coreEntryPath, coreEntryCode, 'utf8')

  const aliasPlugin = {
    name: 'himd-porffor-aliases',
    setup(build) {
      const exactMap = {
        'himd-js': path.join(stagedDir, 'himd-js', 'index.js'),
        nufatfs: path.join(stagedDir, 'nufatfs', 'index.js'),
        'node-mass-storage': path.join(stagedDir, 'node-mass-storage', 'index.js'),
        '@originjs/crypto-js-wasm': path.join(shimsDir, 'crypto-js-wasm.js'),
        jconv: path.join(shimsDir, 'jconv.js'),
        'iconv-lite': path.join(shimsDir, 'iconv-lite.js'),
        buffer: path.join(shimsDir, 'buffer.js'),
        'async-mutex': path.join(shimsDir, 'async-mutex.js'),
        'node-id3': path.join(shimsDir, 'empty-stub.js'),
        'mp3-parser': path.join(shimsDir, 'empty-stub.js'),
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

  // Lower all async/await cleanly into SyncPromise synchronous calls via Babel.
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
    /\bnew\s+Proxy\b|\bnew\s+RegExp\b|\bnew\s+Function\b|\beval\s*\(|\basync\s+function\b|\bfunction\s*\*|\byield\b|\bfor\s+await\b|=\s*class\s+(?!extends\b)[A-Za-z_$]/
  const badMatch = lowered.match(forbidden)
  if (badMatch) {
    throw new Error(`[himd] Forbidden construct remained in lowered bundle: ${badMatch[0]}`)
  }

  fs.writeFileSync(loweredPath, lowered, 'utf8')
  return loweredPath
}

async function runNodeSanitySelfTest(loweredPath) {
  const selfTestPath = path.join(workDir, 'node_sanity_test.mjs')
  const selfTestCode = `
import { createCipheriv, createDecipheriv, randomBytes } from 'node:crypto'
import * as mod from ${JSON.stringify(pathToFileURL(loweredPath).href)}
globalThis.__cartaHiMdHost = {
  randomBytes: (n) => new Uint8Array(randomBytes(n)),
  desCrypt: (isTriple, isEncrypt, isCbc, key, iv, input) => {
    const alg = isTriple
      ? (isCbc ? 'des-ede3-cbc' : 'des-ede3-ecb')
      : (isCbc ? 'des-cbc' : 'des-ecb')
    let k = isTriple ? key : key.subarray(0, 8)
    if (isTriple && k.length === 16) {
      k = Buffer.concat([k, k.subarray(0, 8)])
    }
    const ivArg = isCbc ? iv : null
    const c = isEncrypt
      ? createCipheriv(alg, k, ivArg)
      : createDecipheriv(alg, k, ivArg)
    c.setAutoPadding(false)
    return new Uint8Array(Buffer.concat([c.update(input), c.final()]))
  },
}

// 1. Verify Hi-MD EKB 0x00010012 track key + maclist key wrapping roundtrip
const rawTrackKey = new Uint8Array([0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef])
const wrappedTrackKey = mod.encryptTrackKey(rawTrackKey)
const unwrappedTrackKey = mod.createTrackKey(0x00010012, wrappedTrackKey)
if (wrappedTrackKey.length !== 8 || !unwrappedTrackKey.every((b, i) => b === rawTrackKey[i])) {
  throw new Error('Node self-test encryptTrackKey/createTrackKey mismatch')
}

// 2. Verify block encryption/decryption roundtrip (with 16-byte mainKey single-DES clamp)
const fragKey = new Uint8Array(8).fill(0x3a)
const blkKey = new Uint8Array(8).fill(0x5c)
const blkIv = new Uint8Array(8).fill(0x00)
const plain = new Uint8Array(16320)
for (let i = 0; i < plain.length; i++) plain[i] = (i * 73 + 11) & 0xff
const cipher = mod.encryptBlock(rawTrackKey, fragKey, blkKey, blkIv, plain)
const decrypted = mod.decryptBlock(rawTrackKey, fragKey, blkKey, blkIv, cipher)
if (!decrypted.every((b, i) => b === plain[i])) {
  throw new Error('Node self-test encryptBlock/decryptBlock roundtrip mismatch')
}

// 3. Verify retailMac, createIcvMac, createTrackMac, decryptMaclistKey
const rm = mod.retailMac(new Uint8Array(32).fill(0x22), new Uint8Array(16).fill(0x11))
const icvMac = mod.createIcvMac(new Uint8Array(24).fill(0x44), new Uint8Array(8).fill(0x55))
const trkMac = mod.createTrackMac(rawTrackKey, new Uint8Array(40).fill(0x66))
const decMclistKey = mod.decryptMaclistKey(new Uint8Array(16).fill(0x77))
if (rm.length !== 8 || icvMac.length !== 8 || trkMac.length !== 8 || decMclistKey.length !== 16) {
  throw new Error('Node self-test MAC/key helper length mismatch')
}

// 4. Verify O(1) stack depth over 5,000 SyncPromise iterations
let deepestStackLines = 0
const deepRes = globalThis.__cartaSyncAwaiter(undefined, [], undefined, function* () {
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
  throw new Error('Node self-test stack check failed: stackLines=' + deepestStackLines)
}
console.log('[porffor-himd] Node self-test OK (stackLines@5000=' + deepestStackLines + ')')
`
  fs.writeFileSync(selfTestPath, selfTestCode, 'utf8')
  runChecked(process.execPath, ['--openssl-legacy-provider', selfTestPath], {
    stdio: 'inherit',
  })
}

// ---------------------------------------------------------------------------
// Porffor C-ABI entry script and C patching
// ---------------------------------------------------------------------------

function buildEntryJs(loweredPath) {
  const entryPath = path.join(workDir, 'entry.js')
  const loweredCode = fs
    .readFileSync(loweredPath, 'utf8')
    .replace(/\nexport\s*\{[\s\S]*?\};\s*$/, '\n')

  const entryCode = `
${loweredCode}

function setLastError(msg) {
  const str = String(msg || 'Hi-MD error');
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
    if (len > 1023) len = 1023;
    if (len > 0) memcpy(g_pf_err, MEM + (u32)PF_F64(ptr) + 8 + (u32)PF_F64(off), len);
    g_pf_err[len] = 0;
  \`;
  return -1;
}

function copyLastHostError(fallback) {
  let len = 0;
  Porffor.c\`
    const char* herr = carta_himd_host_last_error();
    if (herr && herr[0]) {
      size_t n = strlen(herr);
      if (n > 1023) n = 1023;
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

function cstrFromStr(s) {
  return __utf8EncodeImpl(String(s == null ? '' : s) + '\\0');
}

// Install C++ host callbacks for Hi-MD core
globalThis.__cartaHiMdHost = {
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
      rc = (f64)carta_himd_host_des_crypt(
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
      rc = (f64)carta_himd_host_random_bytes(op_c, (int)PF_F64(count));
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
      rc = (f64)carta_himd_host_cp932(
        (int)PF_F64(toUtf8), ip_c, (int)PF_F64(inLen), op_c, (int)PF_F64(outCap));
    \`;
    if (rc < 0) throw new Error('CP932 conversion failed');
    return out.slice(0, rc | 0);
  },
  sleepMs(ms) {
    const dur = ms | 0;
    Porffor.c\`
      carta_himd_host_sleep_ms((int)PF_F64(dur));
    \`;
  },
  reportStep(done, total) {
    const d = done | 0;
    const tot = total | 0;
    Porffor.c\`
      carta_himd_host_report_step((int)PF_F64(d), (int)PF_F64(tot));
    \`;
  },
  fileOpen(pathStr, writeMode) {
    const pathBytes = cstrFromStr(pathStr);
    const ip = Porffor.IR.ptr(pathBytes.buffer);
    const ioff = pathBytes.byteOffset;
    const wm = writeMode ? 1 : 0;
    let fd = -1;
    Porffor.c\`
      const char* p_c = (const char*)(MEM + (u32)PF_F64(ip) + 8 + (u32)PF_F64(ioff));
      fd = (f64)carta_himd_host_file_open(p_c, (int)PF_F64(wm));
    \`;
    if (fd < 0) {
      copyLastHostError('Failed opening host file');
      throw new Error('Failed opening host file: ' + pathStr);
    }
    return fd | 0;
  },
  fileSize(fd) {
    const f = fd | 0;
    let sz = -1;
    Porffor.c\`
      sz = (f64)carta_himd_host_file_size((int)PF_F64(f));
    \`;
    if (sz < 0) {
      copyLastHostError('Failed getting host file size');
      throw new Error('Failed getting host file size');
    }
    return sz;
  },
  fileRead(fd, outBuf, maxLen) {
    const f = fd | 0;
    const n = maxLen | 0;
    const op = Porffor.IR.ptr(outBuf.buffer);
    const ooff = outBuf.byteOffset;
    let rc = -1;
    Porffor.c\`
      u8* op_c = (u8*)(MEM + (u32)PF_F64(op) + 8 + (u32)PF_F64(ooff));
      rc = (f64)carta_himd_host_file_read((int)PF_F64(f), op_c, (int)PF_F64(n));
    \`;
    if (rc < 0) {
      copyLastHostError('Host file read error');
      throw new Error('Host file read error');
    }
    return rc | 0;
  },
  fileWrite(fd, dataBuf, len) {
    const f = fd | 0;
    const n = len | 0;
    if (n <= 0) return 0;
    const ip = Porffor.IR.ptr(dataBuf.buffer);
    const ioff = dataBuf.byteOffset;
    let rc = -1;
    Porffor.c\`
      const u8* ip_c = (const u8*)(MEM + (u32)PF_F64(ip) + 8 + (u32)PF_F64(ioff));
      rc = (f64)carta_himd_host_file_write((int)PF_F64(f), ip_c, (int)PF_F64(n));
    \`;
    if (rc < 0) {
      copyLastHostError('Host file write error');
      throw new Error('Host file write error');
    }
    return rc | 0;
  },
  fileClose(fd) {
    const f = fd | 0;
    Porffor.c\`
      carta_himd_host_file_close((int)PF_F64(f));
    \`;
  },
  scsiListJson() {
    const cap = 16384;
    const buf = new Uint8Array(cap);
    const op = Porffor.IR.ptr(buf.buffer);
    const ooff = buf.byteOffset;
    let rc = -1;
    Porffor.c\`
      u8* op_c = (u8*)(MEM + (u32)PF_F64(op) + 8 + (u32)PF_F64(ooff));
      rc = (f64)carta_himd_host_scsi_list(op_c, (int)PF_F64(cap));
    \`;
    if (rc < 0) {
      copyLastHostError('SCSI device list failed');
      throw new Error('SCSI device list failed');
    }
    return __utf8DecodeImpl(buf.subarray(0, rc | 0));
  },
  scsiOpen(deviceId) {
    const bytes = cstrFromStr(deviceId);
    const ip = Porffor.IR.ptr(bytes.buffer);
    const ioff = bytes.byteOffset;
    let h = -1;
    Porffor.c\`
      const char* id_c = (const char*)(MEM + (u32)PF_F64(ip) + 8 + (u32)PF_F64(ioff));
      h = (f64)carta_himd_host_scsi_open(id_c);
    \`;
    if (h < 0) {
      copyLastHostError('SCSI open failed');
      throw new Error('SCSI open failed for ' + deviceId);
    }
    return h | 0;
  },
  scsiClose(handle) {
    const h = handle | 0;
    Porffor.c\`
      carta_himd_host_scsi_close((int)PF_F64(h));
    \`;
  },
  scsiReset(handle) {
    const h = handle | 0;
    let rc = -1;
    Porffor.c\`
      rc = (f64)carta_himd_host_scsi_reset((int)PF_F64(h));
    \`;
    if (rc < 0) {
      copyLastHostError('SCSI reset failed');
      throw new Error('SCSI reset failed');
    }
  },
  scsiCommandNone(handle, cdb, cdbLen) {
    const h = handle | 0;
    const cl = cdbLen | 0;
    const cp = Porffor.IR.ptr(cdb.buffer);
    const coff = cdb.byteOffset;
    let rc = -1;
    Porffor.c\`
      const u8* cdb_c = (const u8*)(MEM + (u32)PF_F64(cp) + 8 + (u32)PF_F64(coff));
      rc = (f64)carta_himd_host_scsi_command(
        (int)PF_F64(h), cdb_c, (int)PF_F64(cl), 0, NULL, 0, NULL, 0);
    \`;
    if (rc < 0) {
      copyLastHostError('SCSI command failed');
      throw new Error('SCSI command failed');
    }
    return new Uint8Array(0);
  },
  scsiCommandIn(handle, cdb, cdbLen, wantLen) {
    const h = handle | 0;
    const cl = cdbLen | 0;
    const wl = wantLen | 0;
    const cap = wl > 0 ? wl : 1;
    const cp = Porffor.IR.ptr(cdb.buffer);
    const coff = cdb.byteOffset;
    const out = new Uint8Array(cap);
    const op = Porffor.IR.ptr(out.buffer);
    const ooff = out.byteOffset;
    let rc = -1;
    Porffor.c\`
      const u8* cdb_c = (const u8*)(MEM + (u32)PF_F64(cp) + 8 + (u32)PF_F64(coff));
      u8* out_c = (u8*)(MEM + (u32)PF_F64(op) + 8 + (u32)PF_F64(ooff));
      rc = (f64)carta_himd_host_scsi_command(
        (int)PF_F64(h), cdb_c, (int)PF_F64(cl), 1, NULL, 0, out_c, (int)PF_F64(wl));
    \`;
    if (rc < 0) {
      copyLastHostError('SCSI IN command failed');
      throw new Error('SCSI IN command failed');
    }
    const n = rc | 0;
    return n === out.length ? out : out.slice(0, n);
  },
  scsiCommandOut(handle, cdb, cdbLen, data) {
    const h = handle | 0;
    const cl = cdbLen | 0;
    const cp = Porffor.IR.ptr(cdb.buffer);
    const coff = cdb.byteOffset;
    const payload = data instanceof Uint8Array ? data : new Uint8Array(0);
    const inLen = payload.length | 0;
    const sentinel = inLen > 0 ? payload : new Uint8Array(1);
    const ip = Porffor.IR.ptr(sentinel.buffer);
    const ioff = sentinel.byteOffset;
    let rc = -1;
    Porffor.c\`
      const u8* cdb_c = (const u8*)(MEM + (u32)PF_F64(cp) + 8 + (u32)PF_F64(coff));
      const u8* in_c = (int)PF_F64(inLen) > 0 ? (const u8*)(MEM + (u32)PF_F64(ip) + 8 + (u32)PF_F64(ioff)) : NULL;
      rc = (f64)carta_himd_host_scsi_command(
        (int)PF_F64(h), cdb_c, (int)PF_F64(cl), 2, in_c, (int)PF_F64(inLen), NULL, 0);
    \`;
    if (rc < 0) {
      copyLastHostError('SCSI OUT command failed');
      throw new Error('SCSI OUT command failed');
    }
    return rc | 0;
  },
  scsiCommand(handle, cdb, cdbLen, direction, dataOrLen) {
    const dir = direction | 0;
    if (dir === 0) return this.scsiCommandNone(handle, cdb, cdbLen);
    if (dir === 1) return this.scsiCommandIn(handle, cdb, cdbLen, dataOrLen | 0);
    return this.scsiCommandOut(handle, cdb, cdbLen, dataOrLen);
  },
};

export function bridgeListDevices() {
  globalThis.__resetPromiseJobs();
  try {
    const json = __syncAwait(himdListDevices());
    return writeScratchBytes(__utf8EncodeImpl(json));
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

export function bridgeProbeDevice(idPtr, idLen) {
  globalThis.__resetPromiseJobs();
  try {
    const devId = __utf8DecodeImpl(copyInBytes(idPtr, idLen));
    const json = __syncAwait(himdProbeDevice(devId));
    return writeScratchBytes(__utf8EncodeImpl(json));
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

export function bridgeUploadTrack(
  idPtr, idLen, codecPtr, codecLen, filePtr, fileLen,
  titlePtr, titleLen, artistPtr, artistLen, albumPtr, albumLen
) {
  globalThis.__resetPromiseJobs();
  try {
    const devId = __utf8DecodeImpl(copyInBytes(idPtr, idLen));
    const codec = __utf8DecodeImpl(copyInBytes(codecPtr, codecLen));
    const filePath = __utf8DecodeImpl(copyInBytes(filePtr, fileLen));
    const title = titleLen > 0 ? __utf8DecodeImpl(copyInBytes(titlePtr, titleLen)) : '';
    const artist = artistLen > 0 ? __utf8DecodeImpl(copyInBytes(artistPtr, artistLen)) : '';
    const album = albumLen > 0 ? __utf8DecodeImpl(copyInBytes(albumPtr, albumLen)) : '';
    const json = __syncAwait(himdUploadTrackFromFile(devId, codec, filePath, title, artist, album));
    return writeScratchBytes(__utf8EncodeImpl(json));
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

export function bridgeDumpTrack(idPtr, idLen, trackIndex, outFilePtr, outFileLen, rawAudioOnly) {
  globalThis.__resetPromiseJobs();
  try {
    const devId = __utf8DecodeImpl(copyInBytes(idPtr, idLen));
    const outFile = __utf8DecodeImpl(copyInBytes(outFilePtr, outFileLen));
    const json = __syncAwait(
      himdDumpTrackToFile(devId, trackIndex | 0, outFile, (rawAudioOnly | 0) !== 0)
    );
    return writeScratchBytes(__utf8EncodeImpl(json));
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

export function bridgeRenameDisc(idPtr, idLen, titlePtr, titleLen) {
  globalThis.__resetPromiseJobs();
  try {
    const devId = __utf8DecodeImpl(copyInBytes(idPtr, idLen));
    const title = titleLen > 0 ? __utf8DecodeImpl(copyInBytes(titlePtr, titleLen)) : '';
    __syncAwait(himdRenameDisc(devId, title));
    return 0;
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

export function bridgeRenameTrack(
  idPtr, idLen, trackIndex,
  hasTitle, titlePtr, titleLen,
  hasArtist, artistPtr, artistLen,
  hasAlbum, albumPtr, albumLen
) {
  globalThis.__resetPromiseJobs();
  try {
    const devId = __utf8DecodeImpl(copyInBytes(idPtr, idLen));
    const title = titleLen > 0 ? __utf8DecodeImpl(copyInBytes(titlePtr, titleLen)) : '';
    const artist = artistLen > 0 ? __utf8DecodeImpl(copyInBytes(artistPtr, artistLen)) : '';
    const album = albumLen > 0 ? __utf8DecodeImpl(copyInBytes(albumPtr, albumLen)) : '';
    __syncAwait(
      himdRenameTrack(
        devId, trackIndex | 0,
        (hasTitle | 0) !== 0, title,
        (hasArtist | 0) !== 0, artist,
        (hasAlbum | 0) !== 0, album
      )
    );
    return 0;
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

export function bridgeMoveTrack(idPtr, idLen, fromIndex, toIndex) {
  globalThis.__resetPromiseJobs();
  try {
    const devId = __utf8DecodeImpl(copyInBytes(idPtr, idLen));
    __syncAwait(himdMoveTrack(devId, fromIndex | 0, toIndex | 0));
    return 0;
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

export function bridgeSetGroups(idPtr, idLen, groupsPtr, groupsLen) {
  globalThis.__resetPromiseJobs();
  try {
    const devId = __utf8DecodeImpl(copyInBytes(idPtr, idLen));
    const groupsJson = __utf8DecodeImpl(copyInBytes(groupsPtr, groupsLen));
    __syncAwait(himdSetGroups(devId, groupsJson));
    return 0;
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

export function bridgeDeleteTracks(idPtr, idLen, indicesPtr, indicesLen) {
  globalThis.__resetPromiseJobs();
  try {
    const devId = __utf8DecodeImpl(copyInBytes(idPtr, idLen));
    const indicesJson = __utf8DecodeImpl(copyInBytes(indicesPtr, indicesLen));
    __syncAwait(himdDeleteTracks(devId, indicesJson));
    return 0;
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

export function bridgeWipeDisc(idPtr, idLen, reformat) {
  globalThis.__resetPromiseJobs();
  try {
    const devId = __utf8DecodeImpl(copyInBytes(idPtr, idLen));
    __syncAwait(himdWipeDisc(devId, (reformat | 0) !== 0));
    return 0;
  } catch (err) {
    copyLastHostError(err && err.message ? err.message : String(err));
    return -1;
  }
}

if (globalThis.__cartaPorfforRetain) {
  bridgeListDevices();
  bridgeProbeDevice(0, 0);
  bridgeUploadTrack(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  bridgeDumpTrack(0, 0, 0, 0, 0, 0);
  bridgeRenameDisc(0, 0, 0, 0);
  bridgeRenameTrack(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  bridgeMoveTrack(0, 0, 0, 0);
  bridgeSetGroups(0, 0, 0, 0);
  bridgeDeleteTracks(0, 0, 0, 0);
  bridgeWipeDisc(0, 0, 0);
}
`
  fs.writeFileSync(entryPath, entryCode, 'utf8')
  return entryPath
}

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
        `[himd] Expected 1 generated declaration for ${jsName}(f64 x ${f64Count}), found ${matches.length}`
      )
    }
    return matches[0]
  }

  const fnList = findFn('bridgeListDevices', 0)
  const fnProbe = findFn('bridgeProbeDevice', 2)
  const fnUpload = findFn('bridgeUploadTrack', 12)
  const fnDump = findFn('bridgeDumpTrack', 6)
  const fnRenameDisc = findFn('bridgeRenameDisc', 4)
  const fnRenameTrack = findFn('bridgeRenameTrack', 12)
  const fnMoveTrack = findFn('bridgeMoveTrack', 4)
  const fnSetGroups = findFn('bridgeSetGroups', 4)
  const fnDeleteTracks = findFn('bridgeDeleteTracks', 4)
  const fnWipeDisc = findFn('bridgeWipeDisc', 3)

  const mainAnchor = `int main(int argc, char** argv) {
  porf_init(argc, argv);
  porf_data_init();
  volatile int porf_stack_anchor = 0;
  porf_c_stack_top = (void*)&porf_stack_anchor;
  p__main__23main();
  return 0;
}`
  if (!cSource.includes(mainAnchor)) {
    throw new Error('[himd] Expected Porffor main(...) anchor not found')
  }

  const preludeMarker = 'static void* porf_c_stack_top = NULL;'
  const preludeIdx = cSource.indexOf(preludeMarker)
  if (preludeIdx === -1) {
    throw new Error(`[himd] Expected '${preludeMarker}' in generated C`)
  }
  const insertAt = preludeIdx + preludeMarker.length

  const hostDeclsAndScratch = `
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern int carta_himd_host_des_crypt(int is_triple_des, int is_encrypt, int is_cbc, const uint8_t* key_ptr, int key_len, const uint8_t* iv_ptr, const uint8_t* in_ptr, int in_len, uint8_t* out_ptr);
extern int carta_himd_host_random_bytes(uint8_t* out_ptr, int len);
extern int carta_himd_host_cp932(int to_utf8, const uint8_t* in_ptr, int in_len, uint8_t* out_ptr, int out_cap);
extern void carta_himd_host_sleep_ms(int ms);
extern void carta_himd_host_report_step(int done, int total);
extern double carta_himd_host_now_ms(void);
extern int carta_himd_host_file_open(const char* path_ptr, int write_mode);
extern double carta_himd_host_file_size(int fd);
extern int carta_himd_host_file_read(int fd, uint8_t* out_ptr, int max_len);
extern int carta_himd_host_file_write(int fd, const uint8_t* data_ptr, int len);
extern int carta_himd_host_file_close(int fd);
extern int carta_himd_host_scsi_list(uint8_t* json_out, int json_cap);
extern int carta_himd_host_scsi_open(const char* device_id);
extern int carta_himd_host_scsi_close(int handle);
extern int carta_himd_host_scsi_reset(int handle);
extern int carta_himd_host_scsi_command(int handle, const uint8_t* cdb_ptr, int cdb_len, int direction, const uint8_t* data_in_ptr, int data_in_len, uint8_t* data_out_ptr, int data_out_max_len);
extern const char* carta_himd_host_last_error(void);

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
static char g_pf_err[1024] = {0};
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

static int himd_pf_initialized = 0;
static void himd_ensure_init(void) {
  if (!himd_pf_initialized) {
    static char arg0[] = "himd-porffor";
    static char* argv[] = { arg0, NULL };
    porf_init(1, argv);
    porf_data_init();
    p__main__23main();
    himd_pf_initialized = 1;
  }
}

CARTA_PF_EXPORT
int himd_pf_list_devices(void) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  himd_ensure_init();
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnList}(porf_box(0, 0), porf_box(0, 0)).val;
}

CARTA_PF_EXPORT
int himd_pf_probe_device(const char* id_ptr, int id_len) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  himd_ensure_init();
  if (!id_ptr || id_len <= 0) return -1;
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnProbe}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)id_ptr, (f64)id_len
  ).val;
}

CARTA_PF_EXPORT
int himd_pf_upload_track(const char* id_ptr,
                         int id_len,
                         const char* codec_ptr,
                         int codec_len,
                         const char* file_ptr,
                         int file_len,
                         const char* title_ptr,
                         int title_len,
                         const char* artist_ptr,
                         int artist_len,
                         const char* album_ptr,
                         int album_len) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  himd_ensure_init();
  if (!id_ptr || id_len <= 0 || !codec_ptr || codec_len <= 0 || !file_ptr || file_len <= 0) return -1;
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnUpload}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)id_ptr, (f64)id_len,
    (f64)(uintptr_t)codec_ptr, (f64)codec_len,
    (f64)(uintptr_t)file_ptr, (f64)file_len,
    (f64)(uintptr_t)title_ptr, (f64)title_len,
    (f64)(uintptr_t)artist_ptr, (f64)artist_len,
    (f64)(uintptr_t)album_ptr, (f64)album_len
  ).val;
}

CARTA_PF_EXPORT
int himd_pf_dump_track(const char* id_ptr,
                       int id_len,
                       int track_index,
                       const char* out_file_ptr,
                       int out_file_len,
                       int raw_audio_only) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  himd_ensure_init();
  if (!id_ptr || id_len <= 0 || !out_file_ptr || out_file_len <= 0) return -1;
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnDump}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)id_ptr, (f64)id_len,
    (f64)track_index,
    (f64)(uintptr_t)out_file_ptr, (f64)out_file_len,
    (f64)raw_audio_only
  ).val;
}

CARTA_PF_EXPORT
int himd_pf_rename_disc(const char* id_ptr, int id_len, const char* title_ptr, int title_len) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  himd_ensure_init();
  if (!id_ptr || id_len <= 0) return -1;
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnRenameDisc}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)id_ptr, (f64)id_len,
    (f64)(uintptr_t)title_ptr, (f64)title_len
  ).val;
}

CARTA_PF_EXPORT
int himd_pf_rename_track(const char* id_ptr,
                         int id_len,
                         int track_index,
                         int has_title,
                         const char* title_ptr,
                         int title_len,
                         int has_artist,
                         const char* artist_ptr,
                         int artist_len,
                         int has_album,
                         const char* album_ptr,
                         int album_len) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  himd_ensure_init();
  if (!id_ptr || id_len <= 0) return -1;
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnRenameTrack}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)id_ptr, (f64)id_len,
    (f64)track_index,
    (f64)has_title, (f64)(uintptr_t)title_ptr, (f64)title_len,
    (f64)has_artist, (f64)(uintptr_t)artist_ptr, (f64)artist_len,
    (f64)has_album, (f64)(uintptr_t)album_ptr, (f64)album_len
  ).val;
}

CARTA_PF_EXPORT
int himd_pf_move_track(const char* id_ptr, int id_len, int from_index, int to_index) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  himd_ensure_init();
  if (!id_ptr || id_len <= 0) return -1;
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnMoveTrack}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)id_ptr, (f64)id_len,
    (f64)from_index, (f64)to_index
  ).val;
}

CARTA_PF_EXPORT
int himd_pf_set_groups(const char* id_ptr,
                       int id_len,
                       const char* groups_json_ptr,
                       int groups_json_len) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  himd_ensure_init();
  if (!id_ptr || id_len <= 0 || !groups_json_ptr || groups_json_len <= 0) return -1;
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnSetGroups}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)id_ptr, (f64)id_len,
    (f64)(uintptr_t)groups_json_ptr, (f64)groups_json_len
  ).val;
}

CARTA_PF_EXPORT
int himd_pf_delete_tracks(const char* id_ptr,
                          int id_len,
                          const char* indices_json_ptr,
                          int indices_json_len) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  himd_ensure_init();
  if (!id_ptr || id_len <= 0 || !indices_json_ptr || indices_json_len <= 0) return -1;
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnDeleteTracks}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)id_ptr, (f64)id_len,
    (f64)(uintptr_t)indices_json_ptr, (f64)indices_json_len
  ).val;
}

CARTA_PF_EXPORT
int himd_pf_wipe_disc(const char* id_ptr, int id_len, int reformat) {
  volatile int anchor = 0;
  porf_c_stack_top = (void*)&anchor;
  himd_ensure_init();
  if (!id_ptr || id_len <= 0) return -1;
  g_pf_err[0] = 0;
  g_pf_out_len = 0;
  return (int)${fnWipeDisc}(
    porf_box(0, 0), porf_box(0, 0),
    (f64)(uintptr_t)id_ptr, (f64)id_len,
    (f64)reformat
  ).val;
}

CARTA_PF_EXPORT
const unsigned char* himd_pf_output_ptr(void) {
  return g_pf_out_buf;
}

CARTA_PF_EXPORT
size_t himd_pf_output_len(void) {
  return g_pf_out_len;
}

CARTA_PF_EXPORT
const char* himd_pf_last_error(void) {
  return g_pf_err;
}

CARTA_PF_EXPORT
void himd_pf_release_scratch(void) {
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

async function main() {
  const compileOnly = shouldCompileOnly()
  ensurePrereqs(!compileOnly)

  const scriptMtime = fs.statSync(__filename).mtimeMs
  if (emitCOnly) {
    const depsMtime = Math.max(
      latestMtimeMs(path.join(himdDepsModules, 'himd-js', 'dist')),
      latestMtimeMs(path.join(himdDepsModules, 'nufatfs', 'dist')),
      latestMtimeMs(path.join(himdDepsModules, 'node-mass-storage', 'dist'))
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
      latestMtimeMs(path.join(himdDepsModules, 'himd-js', 'dist')),
      latestMtimeMs(path.join(himdDepsModules, 'nufatfs', 'dist')),
      latestMtimeMs(path.join(himdDepsModules, 'node-mass-storage', 'dist'))
    )
    const latestInput = Math.max(scriptMtime, depsMtime)
    if (!forceRebuild && fs.existsSync(dylibPath) && fs.statSync(dylibPath).mtimeMs >= latestInput) {
      console.log(`[porffor] Up to date: ${dylibPath}`)
      return
    }
  }

  if (!compileOnly) {
    console.log('[porffor] Staging & transpiling himd-js + nufatfs + node-mass-storage...')
    const requireFromTools = createRequire(path.join(toolsNodeModules, '_anchor.js'))
    const babel = requireFromTools('@babel/core')
    const esbuild = requireFromTools('esbuild')

    stageHimdPackages(babel)
    const loweredPath = await writeCoreEntryAndBundle(babel, esbuild)
    await runNodeSanitySelfTest(loweredPath)
    const entryPath = buildEntryJs(loweredPath)

    const rawC = path.join(workDir, 'himd_raw.c')
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
    const unexpected = exported.filter((sym) => !sym.startsWith('_himd_pf_'))
    if (unexpected.length > 0 || exported.length === 0) {
      throw new Error(
        `[himd] Exported symbol check failed.\nExpected only _himd_pf_*\nUnexpected: ${unexpected.join(', ')}\nAll: ${exported.join(', ')}`
      )
    }
    fs.renameSync(tmpOut, dylibPath)
    console.log(
      `[porffor] Built ${dylibPath} (${exported.length} exported symbols: ${exported.join(', ')})`
    )
  } else if (isWin) {
    const tmpObj = path.join(workDir, 'himd_porffor.obj')
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
    const unexpected = exported.filter((sym) => !sym.startsWith('himd_pf_'))
    if (unexpected.length > 0 || exported.length === 0) {
      throw new Error(
        `[himd] Exported symbol check failed.\nExpected only himd_pf_*\nUnexpected: ${unexpected.join(', ')}\nAll: ${exported.join(', ')}`
      )
    }
    fs.renameSync(tmpOut, dylibPath)
    console.log(
      `[porffor] Built ${dylibPath} (${exported.length} exported symbols: ${exported.join(', ')})`
    )
  }
}

await main()
