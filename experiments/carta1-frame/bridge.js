// Experimental Porffor entry point. One encoder and decoder per test stream.
import { encode } from './codec/pipeline/encoder.js'
import { decode } from './codec/pipeline/decoder.js'
import { serializeFrame, deserializeFrame } from './codec/syntax/frame.js'

const enc0 = encode()
const enc1 = encode()
const dec0 = decode()
const dec1 = decode()

export function bridgeEncode(stream, source, destination) {
  const pcm = new Float32Array(512)
  const address = Porffor.IR.ptr(pcm.buffer)
  Porffor.c`memcpy(MEM + (u32)address.val + 8, (const void*)(uintptr_t)source, 512 * sizeof(float));`
  const frame = stream === 0 ? enc0(pcm) : enc1(pcm)
  const bytes = serializeFrame(frame)
  const result = Porffor.IR.ptr(bytes.buffer)
  Porffor.c`memcpy((void*)(uintptr_t)destination, MEM + (u32)result.val + 8, 212);`
  return 0
}

export function bridgeDecode(stream, source, destination) {
  const bytes = new Uint8Array(212)
  const address = Porffor.IR.ptr(bytes.buffer)
  Porffor.c`memcpy(MEM + (u32)address.val + 8, (const void*)(uintptr_t)source, 212);`
  const frame = deserializeFrame(bytes)
  const pcm = stream === 0 ? dec0(frame) : dec1(frame)
  const result = Porffor.IR.ptr(pcm.buffer)
  Porffor.c`memcpy((void*)(uintptr_t)destination, MEM + (u32)result.val + 8, 512 * sizeof(float));`
  return 0
}
// Retain exports during compilation. Never executed.
if (globalThis.__cartaFrameProbe) {
  bridgeEncode(0, 0, 0)
  bridgeDecode(0, 0, 0)
}
