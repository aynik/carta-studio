self.onmessage = async (e) => {
  const { jobId, type, audioData, options } = e.data

  try {
    let result

    if (type === 'generateSpectrogram') {
      // Use the local generateSpectrogram function
      result = await generateSpectrogram(audioData, options)
    } else {
      throw new Error(`Unknown worker message type: ${type}`)
    }

    self.postMessage({ jobId, result })
  } catch (error) {
    console.error('Spectrogram worker error:', error)
    self.postMessage({ jobId, error: error.message })
  }
}

async function generateSpectrogram(audioData, options = {}) {
  const {
    fftSize = 2048,
    hopSize = 512,
    windowFunction = 'hann',
    sampleRate = 44100,
  } = options

  if (!audioData || audioData.length === 0) {
    throw new Error('No audio data provided')
  }

  // Handle stereo by using only left channel or averaging
  let pcmData
  if (Array.isArray(audioData) && audioData.length === 2) {
    // Stereo - average channels
    const [left, right] = audioData
    pcmData = new Float32Array(left.length)
    for (let i = 0; i < left.length; i++) {
      pcmData[i] = (left[i] + right[i]) / 2
    }
  } else if (Array.isArray(audioData) && audioData.length === 1) {
    // Mono
    pcmData = audioData[0]
  } else {
    // Assume single channel
    pcmData = audioData
  }

  const spectrogramData = []
  const numFrames = Math.floor((pcmData.length - fftSize) / hopSize) + 1

  // Window function
  const window = createWindow(fftSize, windowFunction)

  for (let frameIndex = 0; frameIndex < numFrames; frameIndex++) {
    const frameStart = frameIndex * hopSize
    const frameEnd = frameStart + fftSize

    if (frameEnd > pcmData.length) break

    // Extract frame and apply window
    const frame = new Float32Array(fftSize)
    for (let i = 0; i < fftSize; i++) {
      frame[i] = pcmData[frameStart + i] * window[i]
    }

    // Compute FFT
    const spectrum = computeFFTSpectrum(frame)
    spectrogramData.push(spectrum)

    // Report progress occasionally
    if (frameIndex % 100 === 0) {
      self.postMessage({
        jobId: null,
        progress: {
          type: 'spectrogram',
          current: frameIndex,
          total: numFrames,
          percentage: Math.round((frameIndex / numFrames) * 100),
        },
      })
    }
  }

  return {
    spectrogramData,
    metadata: {
      fftSize,
      hopSize,
      sampleRate,
      numFrames: spectrogramData.length,
      freqBins: spectrogramData[0]?.length || 0,
      duration: pcmData.length / sampleRate,
      nyquistFreq: sampleRate / 2,
    },
  }
}

function computeFFTSpectrum(frame) {
  const fftSize = frame.length

  // Prepare separate real and imaginary arrays for FFT
  const real = new Float32Array(fftSize)
  const imag = new Float32Array(fftSize)

  // Copy frame data to real array, zero imaginary
  for (let i = 0; i < fftSize; i++) {
    real[i] = frame[i]
    imag[i] = 0
  }

  // Use local FFT implementation
  FFT.fft(real, imag)

  // Compute magnitude spectrum (only positive frequencies)
  const spectrum = new Float32Array(fftSize / 2)
  for (let i = 0; i < fftSize / 2; i++) {
    const magnitude = Math.sqrt(real[i] * real[i] + imag[i] * imag[i])

    // Convert to dB scale and normalize
    spectrum[i] = Math.log10(Math.max(magnitude, 1e-10)) / 4 + 1
  }

  return spectrum
}

function createWindow(size, type) {
  const window = new Float32Array(size)

  switch (type) {
    case 'hann':
      for (let i = 0; i < size; i++) {
        window[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (size - 1))
      }
      break

    case 'hamming':
      for (let i = 0; i < size; i++) {
        window[i] = 0.54 - 0.46 * Math.cos((2 * Math.PI * i) / (size - 1))
      }
      break

    case 'blackman':
      for (let i = 0; i < size; i++) {
        const n = (2 * Math.PI * i) / (size - 1)
        window[i] = 0.42 - 0.5 * Math.cos(n) + 0.08 * Math.cos(2 * n)
      }
      break

    default: // rectangular
      window.fill(1)
      break
  }

  return window
}

// FFT Implementation
class FFT {
  static fft(real, imag) {
    const size = real.length
    if (size === 1) return

    const bits = Math.log2(size)

    // Bit reversal
    for (let i = 0; i < size; i++) {
      let reversed = 0
      let temp = i
      for (let b = 0; b < bits; b++) {
        reversed = (reversed << 1) | (temp & 1)
        temp >>= 1
      }
      if (reversed > i) {
        ;[real[i], real[reversed]] = [real[reversed], real[i]]
        ;[imag[i], imag[reversed]] = [imag[reversed], imag[i]]
      }
    }

    // Cooley-Tukey decimation-in-time
    for (let stride = 2; stride <= size; stride <<= 1) {
      const halfStride = stride >> 1
      const angle = (-2 * Math.PI) / stride
      const wReal = Math.cos(angle)
      const wImag = Math.sin(angle)

      for (let start = 0; start < size; start += stride) {
        let twiddleReal = 1
        let twiddleImag = 0

        for (let k = 0; k < halfStride; k++) {
          const evenIndex = start + k
          const oddIndex = evenIndex + halfStride

          const evenReal = real[evenIndex]
          const evenImag = imag[evenIndex]
          const oddReal = real[oddIndex]
          const oddImag = imag[oddIndex]

          const tReal = oddReal * twiddleReal - oddImag * twiddleImag
          const tImag = oddReal * twiddleImag + oddImag * twiddleReal

          real[evenIndex] = evenReal + tReal
          imag[evenIndex] = evenImag + tImag
          real[oddIndex] = evenReal - tReal
          imag[oddIndex] = evenImag - tImag

          const nextReal = twiddleReal * wReal - twiddleImag * wImag
          twiddleImag = twiddleReal * wImag + twiddleImag * wReal
          twiddleReal = nextReal
        }
      }
    }
  }
}
