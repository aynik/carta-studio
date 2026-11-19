import { quantize } from '../../third_party/carta1/codec/quantization/spectrum.js'

// A deliberately small boundary around the unmodified Carta1 implementation.
export function cartaQuantizeSample(value, scale, bits) {
  const input = new Float32Array(1)
  input[0] = value
  return quantize(input, scale, bits)[0]
}
// Keep the function reachable during ahead-of-time compilation.
cartaQuantizeSample(0, 0, 2)
