import { spawn } from 'node:child_process'
import { encode, decode, serializeFrame, deserializeFrame } from '../../third_party/carta1/codec/index.js'

const count = Number(process.argv[3] || 16)
if (!Number.isInteger(count) || count < 1 || count > 500000) throw new Error('Invalid frame count')
const encoders = [encode(), encode()]
const decoders = [decode(), decode()]
const native = spawn(process.argv[2], [String(count)], { stdio: ['ignore', 'pipe', 'pipe'] })
let stderr = ''
native.stderr.setEncoding('utf8')
native.stderr.on('data', chunk => { if (stderr.length < 16384) stderr += chunk.slice(0, 16384 - stderr.length) })
const exit = new Promise((resolve, reject) => {
  native.on('error', reject)
  native.on('close', (code, signal) => resolve({ code, signal }))
})
const recordSize = 2260
const record = Buffer.allocUnsafe(recordSize)
let buffered = 0, records = 0
try {
  for await (const chunk of native.stdout) {
    let offset = 0
    while (offset < chunk.length) {
      const amount = Math.min(recordSize - buffered, chunk.length - offset)
      chunk.copy(record, buffered, offset, offset + amount)
      offset += amount; buffered += amount
      if (buffered !== recordSize) continue
      if (records >= count * 2) throw new Error('Native probe returned extra frames')
      const frame = Math.floor(records / 2), stream = records % 2
      const pcm = new Float32Array(512)
      for (let i = 0; i < pcm.length; i++) {
        const n = (frame * 113 + stream * 379 + i * 37) % 2049 - 1024
        pcm[i] = frame === 0 ? 0 : frame % 23 === 1 ? (i === 127 + stream ? 0.875 : 0) : n / 2048
      }
      const bytes = serializeFrame(encoders[stream](pcm))
      const reconstruction = decoders[stream](deserializeFrame(bytes))
      const parts = [Buffer.from(bytes), Buffer.from(reconstruction.buffer, reconstruction.byteOffset, reconstruction.byteLength)]
      let base = 0
      for (const part of parts) {
        for (let i = 0; i < part.length; i++) if (record[base + i] !== part[i]) {
          const within = base + i
          throw new Error(`Mismatch at frame ${frame}, stream ${stream}, ${within < 212 ? 'encoded byte ' + within : 'decoded byte ' + (within - 212)}: native ${record[within]}, JS ${part[i]}`)
        }
        base += part.length
      }
      if (base !== recordSize) throw new Error(`Unexpected JavaScript frame size: ${base}`)
      records++; buffered = 0
      if (count >= 100000 && records % 51680 === 0) console.error(`Compared ${records / 2} / ${count} frames per stream`)
    }
  }
  const status = await exit
  if (status.code !== 0) throw new Error(`Native probe failed (${status.code}, ${status.signal}): ${stderr}`)
  if (buffered !== 0 || records !== count * 2) throw new Error(`Incomplete native output: ${records} frames and ${buffered} bytes`)
  console.log(`Matched ${records} interleaved encoded frames and decoded sample bit patterns`)
} catch (error) {
  native.kill()
  await exit.catch(() => {})
  throw error
}
