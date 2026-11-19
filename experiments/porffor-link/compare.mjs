import { spawnSync } from 'node:child_process'
import { quantize } from '../../third_party/carta1/codec/quantization/spectrum.js'

const cases = []
const expected = []
for (const scale of [0, 1, 8, 16, 32, 48, 63]) {
  for (const bits of [0, 2, 3, 5, 8, 12, 16]) {
    for (let i = -64; i <= 64; i++) {
      const value = i / 64
      cases.push(`${value} ${scale} ${bits}`)
      expected.push(String(quantize(new Float32Array([value]), scale, bits)[0]))
    }
  }
}
const result = spawnSync(process.argv[2], { input: cases.join('\n') + '\n', encoding: 'utf8', maxBuffer: 4 * 1024 * 1024 })
if (result.error) throw result.error
if (result.status !== 0) throw new Error(`C++ probe failed (${result.status}): ${result.stderr}`)
const actual = result.stdout.trimEnd().split('\n')
if (actual.length !== expected.length) throw new Error(`Expected ${expected.length} results, got ${actual.length}`)
for (let i = 0; i < expected.length; i++) {
  if (actual[i] !== expected[i]) throw new Error(`Mismatch for ${cases[i]}: native ${actual[i]}, JS ${expected[i]}`)
}
console.log(`Matched ${expected.length} Carta1 quantization results against JavaScript`)
