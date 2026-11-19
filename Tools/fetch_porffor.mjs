#!/usr/bin/env node
/**
 * @fileoverview Download and verify the pinned Porffor alpha-19 (0c135ce) binary
 * for macOS or Linux into `build/porffor/toolchain/porf`.
 */
import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)
const studioDir = path.resolve(__dirname, '..')
const toolchainDir = path.join(studioDir, 'build', 'porffor', 'toolchain')
const porfBin = path.join(toolchainDir, 'porf')

const TAG = 'alpha-19'
const EXPECTED_BANNER = 'alpha 19 (0c135ce 2026-10-21)'

function assetNameForHost() {
  const plat = process.platform
  const arch = process.arch
  if (plat === 'darwin' && arch === 'arm64') return 'porffor-darwin-arm64.tar.gz'
  if (plat === 'darwin' && arch === 'x64') return 'porffor-darwin-x64.tar.gz'
  if (plat === 'linux' && arch === 'x64') return 'porffor-linux-x64.tar.gz'
  if (plat === 'linux' && arch === 'arm64') return 'porffor-linux-arm64.tar.gz'
  throw new Error(
    `No prebuilt Porffor ${TAG} binary for ${plat}/${arch}. Use pre-generated C sources with --compile-only.`
  )
}

function isPinnedBinaryReady(binPath) {
  if (!fs.existsSync(binPath)) return false
  const res = spawnSync(binPath, ['--help'], { encoding: 'utf8' })
  return res.status === 0 && (res.stdout || '').includes(EXPECTED_BANNER)
}

async function main() {
  if (isPinnedBinaryReady(porfBin)) {
    console.log(`[porffor] Pinned binary ready: ${porfBin}`)
    return
  }

  const asset = assetNameForHost()
  const url = `https://github.com/CanadaHonk/porffor/releases/download/${TAG}/${asset}`
  fs.mkdirSync(toolchainDir, { recursive: true })
  const tarball = path.join(toolchainDir, 'porffor.tar.gz')

  console.log(`[porffor] Downloading ${url}...`)
  const res = spawnSync('curl', ['-fsSL', url, '-o', tarball], { stdio: 'inherit' })
  if (res.status !== 0) {
    throw new Error(`Failed downloading ${url}`)
  }

  const tar = spawnSync('tar', ['-xzf', tarball, '-C', toolchainDir], { stdio: 'inherit' })
  if (tar.status !== 0) {
    throw new Error(`Failed extracting ${tarball}`)
  }

  // Some archives place `porf` at top-level or inside a nested directory.
  if (!fs.existsSync(porfBin)) {
    for (const entry of fs.readdirSync(toolchainDir, { withFileTypes: true })) {
      if (entry.isDirectory()) {
        const nested = path.join(toolchainDir, entry.name, 'porf')
        if (fs.existsSync(nested)) {
          fs.copyFileSync(nested, porfBin)
          break
        }
      }
    }
  }
  fs.chmodSync(porfBin, 0o755)

  if (!isPinnedBinaryReady(porfBin)) {
    throw new Error(`Extracted Porffor binary at ${porfBin} failed version check (${EXPECTED_BANNER})`)
  }
  console.log(`[porffor] Installed ${EXPECTED_BANNER} at ${porfBin}`)
}

await main()
