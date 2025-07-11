<template>
  <div
    id="app"
    :class="isDarkMode ? 'dark' : 'light'"
    class="min-h-screen bg-surface transition-colors duration-200"
  >
    <!-- Top Toolbar -->
    <header class="bg-surface border-b border-border/50 h-14">
      <div class="flex items-center justify-between px-6 h-full">
        <div class="flex items-center space-x-3">
          <h1 class="text-sm font-medium text-text uppercase tracking-wider">
            Carta Studio
          </h1>
        </div>

        <div class="flex items-center space-x-3">
          <!-- Auto Process Toggle -->
          <label class="flex items-center space-x-2">
            <input
              type="checkbox"
              v-model="autoProcess"
              class="rounded border-border text-accent focus:ring-accent"
            />
            <span class="text-xs text-text-muted">Auto encode/decode</span>
          </label>

          <!-- Theme Toggle -->
          <button
            @click="toggleDarkMode"
            class="p-1 rounded bg-surface-muted hover:bg-surface-alt transition-colors"
          >
            <MoonIcon v-if="isDarkMode" class="w-4 h-4 text-accent" />
            <SunIcon v-else class="w-4 h-4 text-text-muted" />
          </button>
        </div>
      </div>
    </header>

    <div class="flex h-[calc(100vh-3.5rem)]">
      <!-- Sidebar Control Panel -->
      <aside
        class="w-80 bg-surface-alt/50 border-r border-border/30 backdrop-blur-sm"
      >
        <div class="p-4 space-y-6 h-full overflow-y-auto">
          <!-- File Input Section -->
          <div class="space-y-4">
            <h3
              class="text-xs font-semibold text-text-muted uppercase tracking-wider mb-4"
            >
              Audio Files
            </h3>

            <!-- Compact Upload Areas -->
            <div class="grid grid-cols-2 gap-2">
              <!-- WAV Input -->
              <div>
                <input
                  ref="wavInput"
                  type="file"
                  accept=".wav"
                  @change="handleWavFile"
                  :disabled="isProcessing"
                  class="sr-only"
                  id="wav-input"
                />
                <label
                  for="wav-input"
                  class="flex flex-col items-center justify-center h-20 border border-dashed border-border/50 rounded-md cursor-pointer hover:border-text-muted transition-colors text-center"
                  :class="{ 'border-accent bg-surface-muted/30': wavFile }"
                >
                  <DocumentArrowUpIcon class="w-4 h-4 text-text-muted mb-1" />
                  <span class="text-xs text-text-muted">
                    {{ wavFile ? 'WAV ✓' : 'WAV' }}
                  </span>
                </label>
              </div>

              <!-- AEA Input -->
              <div>
                <input
                  ref="aeaInput"
                  type="file"
                  accept=".aea"
                  @change="handleAeaFile"
                  :disabled="isProcessing"
                  class="sr-only"
                  id="aea-input"
                />
                <label
                  for="aea-input"
                  class="flex flex-col items-center justify-center h-20 border border-dashed border-border/50 rounded-md cursor-pointer hover:border-text-muted transition-colors text-center"
                  :class="{ 'border-accent bg-surface-muted/30': aeaFile }"
                >
                  <DocumentArrowUpIcon class="w-4 h-4 text-text-muted mb-1" />
                  <span class="text-xs text-text-muted">
                    {{ aeaFile ? 'AEA ✓' : 'AEA' }}
                  </span>
                </label>
              </div>
            </div>
          </div>

          <!-- Processing Controls -->
          <div class="space-y-4">
            <button
              @click="clearAll"
              :disabled="isProcessing"
              class="w-full btn-secondary text-sm"
            >
              Clear All
            </button>
          </div>

          <!-- Download Controls -->
          <div v-if="downloadUrls.aea || downloadUrls.wav" class="space-y-4">
            <h3
              class="text-xs font-semibold text-text-muted uppercase tracking-wider mb-4"
            >
              Downloads
            </h3>

            <div class="space-y-2">
              <button
                v-if="downloadUrls.aea"
                @click="downloadFile(downloadUrls.aea, 'encoded.aea')"
                class="w-full btn-primary text-sm"
              >
                Download AEA
              </button>

              <button
                v-if="downloadUrls.wav"
                @click="downloadFile(downloadUrls.wav, 'decoded.wav')"
                class="w-full btn-primary text-sm"
              >
                Download WAV
              </button>
            </div>
          </div>

          <!-- Audio Info -->
          <div v-if="audioInfo" class="space-y-4">
            <h3
              class="text-xs font-semibold text-text-muted uppercase tracking-wider mb-4"
            >
              Audio Info
            </h3>

            <div
              class="bg-surface-alt/30 border border-border/30 rounded-lg p-3 space-y-2"
            >
              <div class="flex justify-between">
                <span class="text-sm text-text-muted">Sample Rate:</span>
                <span class="text-sm font-medium text-text"
                  >{{ audioInfo.sampleRate }} Hz</span
                >
              </div>
              <div class="flex justify-between">
                <span class="text-sm text-text-muted">Channels:</span>
                <span class="text-sm font-medium text-text">{{
                  audioInfo.channels
                }}</span>
              </div>
              <div class="flex justify-between">
                <span class="text-sm text-text-muted">Duration:</span>
                <span class="text-sm font-medium text-text">{{
                  formatDuration(audioInfo.duration)
                }}</span>
              </div>
            </div>
          </div>

          <!-- Encoder Controls -->
          <div v-if="encoderOptions" class="space-y-4">
            <h3
              class="text-xs font-semibold text-text-muted uppercase tracking-wider mb-4"
            >
              Encoder Settings
            </h3>

            <div class="space-y-4">
              <div
                v-for="(metadata, key) in encoderOptions.metadata"
                :key="key"
              >
                <label
                  class="block text-sm text-text-muted mb-2"
                  :title="metadata.description"
                >
                  {{ metadata.name }}:
                  {{
                    metadata.step === 1
                      ? encoderOptions.values[key]
                      : encoderOptions.values[key].toFixed(1)
                  }}{{ key.includes('Db') ? ' dB' : '' }}
                </label>
                <input
                  type="range"
                  :min="metadata.range[0]"
                  :max="metadata.range[1]"
                  :step="metadata.step"
                  v-model.number="encoderOptions.values[key]"
                  @input="handleSliderChange($event, encoderOptions)"
                  class="w-full"
                />
              </div>

              <button
                @click="resetEncoderOptions"
                class="w-full btn-secondary text-sm"
              >
                Reset to Defaults
              </button>
            </div>
          </div>

          <!-- Errors -->
          <div v-if="errors.length > 0" class="space-y-2">
            <h3 class="text-sm font-medium text-red-600">Errors</h3>
            <div
              v-for="error in errors"
              :key="error"
              class="bg-surface-muted border border-border-muted rounded p-2"
            >
              <p class="text-xs text-red-600">{{ error }}</p>
            </div>
          </div>
        </div>
      </aside>

      <!-- Main Content Area -->
      <main class="flex-1 flex flex-col min-h-0 bg-surface">
        <!-- Synchronized Playback Controls -->
        <div
          v-if="waveforms.source || waveforms.decoded"
          class="bg-surface border-b border-border/30 p-3"
        >
          <div class="flex items-center justify-between">
            <h3 class="text-sm font-medium text-text">Playback</h3>
            <div class="flex items-center space-x-4">
              <button
                @click="togglePlayback"
                class="flex items-center space-x-2 px-4 py-2 bg-text text-surface rounded-md text-sm font-medium hover:opacity-90 transition-opacity"
              >
                <PlayIcon v-if="!isPlaying" class="w-3 h-3 text-surface" />
                <PauseIcon v-else class="w-3 h-3 text-surface" />
                <span>{{ isPlaying ? 'Pause' : 'Play' }}</span>
              </button>

              <div class="flex items-center space-x-3">
                <span class="text-sm text-text-muted font-mono">{{
                  formatTime(currentTime)
                }}</span>
                <input
                  type="range"
                  min="0"
                  :max="maxDuration"
                  :value="currentTime"
                  @input="handleSeekChange($event)"
                  class="w-32"
                />
                <span class="text-sm text-text-muted font-mono">{{
                  formatTime(maxDuration)
                }}</span>
              </div>
            </div>
          </div>
        </div>

        <!-- Spectrograms -->
        <div class="flex-1 grid grid-rows-2 gap-2 p-2 min-h-0">
          <!-- Input Spectrogram -->
          <div class="relative min-h-0">
            <SpectrogramViewer
              ref="inputSpectrogram"
              title="Input Audio Spectrum"
              :audio-src="waveforms.source"
              :spectrogram-data="spectrograms.input"
              placeholder="Load a WAV file to view input spectrum"
              :width="1200"
              :height="300"
              :zoom-range="globalZoomRange"
              @zoom-changed="handleZoomChange"
              class="min-h-0"
            />
            
            <!-- Loading overlay -->
            <div
              v-if="isGeneratingSpectrogram"
              class="absolute inset-0 bg-surface/80 backdrop-blur-sm flex items-center justify-center"
            >
              <div class="flex items-center space-x-2 text-text-muted">
                <ArrowPathIcon class="w-4 h-4 animate-spin" />
                <span class="text-sm">Generating spectrogram...</span>
              </div>
            </div>
          </div>

          <!-- Output Spectrogram -->
          <div class="relative min-h-0">
            <SpectrogramViewer
              ref="outputSpectrogram"
              title="Output Audio Spectrum (After ATRAC1 Encode/Decode)"
              :audio-src="waveforms.decoded"
              :spectrogram-data="spectrograms.output"
              :short-block-data="shortBlockData"
              placeholder="Encode and decode audio to view output spectrum"
              :width="1200"
              :height="300"
              :zoom-range="globalZoomRange"
              @zoom-changed="handleZoomChange"
              class="min-h-0"
            />
            
            <!-- Loading overlay -->
            <div
              v-if="isEncodingDecoding"
              class="absolute inset-0 bg-surface/80 backdrop-blur-sm flex items-center justify-center"
            >
              <div class="flex items-center space-x-2 text-text-muted">
                <ArrowPathIcon class="w-4 h-4 animate-spin" />
                <span class="text-sm">Encoding/decoding audio...</span>
              </div>
            </div>
            
            <!-- Loading overlay for spectrogram generation -->
            <div
              v-if="isGeneratingOutputSpectrogram"
              class="absolute inset-0 bg-surface/80 backdrop-blur-sm flex items-center justify-center"
            >
              <div class="flex items-center space-x-2 text-text-muted">
                <ArrowPathIcon class="w-4 h-4 animate-spin" />
                <span class="text-sm">Generating output spectrogram...</span>
              </div>
            </div>
          </div>
        </div>
      </main>
    </div>
  </div>
</template>

<script>
import { ref, onMounted, onUnmounted, watch, nextTick } from 'vue'
import SpectrogramViewer from './components/SpectrogramViewer.vue'
import { 
  PlayIcon, 
  PauseIcon, 
  DocumentArrowUpIcon,
  ArrowPathIcon,
  SunIcon,
  MoonIcon
} from '@heroicons/vue/24/solid'
// Load Carta1Worker interface from CDN
let Carta1Worker = null

const moduleCode = `
import * as module from 'https://unpkg.com/carta1@latest/dist/carta1-worker-interface.min.js?v=${Date.now()}';
window.Carta1Worker = module.Carta1Worker || module.default || module;
window.dispatchEvent(new CustomEvent('carta1WorkerReady'));
`
const script = document.createElement('script')
script.type = 'module'
script.textContent = moduleCode

window.addEventListener('carta1WorkerReady', () => {
  Carta1Worker = window.Carta1Worker
}, { once: true })

script.onerror = (error) => {
  console.error('Failed to load Carta1Worker:', error)
}
document.head.appendChild(script)
import { SpectrogramWorker } from './worker/spectrogram-worker-interface.js'

export default {
  name: 'App',
  components: {
    SpectrogramViewer,
    PlayIcon,
    PauseIcon,
    DocumentArrowUpIcon,
    ArrowPathIcon,
    SunIcon,
    MoonIcon,
  },
  setup() {
    // Theme
    const isDarkMode = ref(localStorage.getItem('darkMode') === 'true')

    // File management
    const wavFile = ref(null)
    const aeaFile = ref(null)
    const autoProcess = ref(true)
    const isProcessing = ref(false)
    const errors = ref([])

    // Audio data
    const waveforms = ref({ source: null, decoded: null })
    const spectrograms = ref({ input: null, output: null })
    const shortBlockData = ref(null)
    const audioInfo = ref(null)
    const downloadUrls = ref({ aea: null, wav: null })

    // Playback control
    const isPlaying = ref(false)
    const currentTime = ref(0)
    const maxDuration = ref(0)
    const sourceAudio = ref(null)
    const decodedAudio = ref(null)

    // Zoom synchronization
    const globalZoomRange = ref({ start: 0, end: 1 })

    // Encoder options - will be populated from worker
    const encoderOptions = ref(null)

    // Workers
    let codecWorker = null
    let spectrogramWorker = null

    // Component refs
    const inputSpectrogram = ref(null)
    const outputSpectrogram = ref(null)
    const isGeneratingSpectrogram = ref(false)
    const isGeneratingOutputSpectrogram = ref(false)
    const isEncodingDecoding = ref(false)

    // Initialize workers
    const initWorkers = async () => {
      try {
        // Wait for Carta1Worker to load
        while (!Carta1Worker) {
          await new Promise(resolve => setTimeout(resolve, 10))
        }
        
        if (typeof Carta1Worker === 'function') {
          // Create blob URL for worker script to avoid CORS
          const baseUrl = 'https://unpkg.com/carta1@latest/dist/'
          const workerFileName = 'carta1-worker.min.js'
          const response = await fetch(baseUrl + workerFileName)
          const workerCode = await response.text()
          const blob = new Blob([workerCode], { type: 'application/javascript' })
          const workerUrl = URL.createObjectURL(blob)
          
          codecWorker = new Carta1Worker(workerUrl)
          spectrogramWorker = new SpectrogramWorker()

          // Initialize encoder options
          encoderOptions.value = await codecWorker.getEncoderOptions()

          // Update slider appearances
          nextTick(() => {
            const sliders = document.querySelectorAll('input[type="range"]')
            sliders.forEach((slider) => updateSliderAppearance(slider))
          })

          spectrogramWorker.setProgressCallback(() => {})
        } else {
          throw new Error('Carta1Worker is not available')
        }
      } catch (error) {
        console.error('Failed to initialize workers:', error)
        addError('Failed to initialize audio processing workers')
      }
    }

    const toggleDarkMode = () => {
      isDarkMode.value = !isDarkMode.value
      localStorage.setItem('darkMode', isDarkMode.value.toString())
    }

    const addError = (message) => {
      errors.value.push(message)
      setTimeout(() => {
        const index = errors.value.indexOf(message)
        if (index > -1) errors.value.splice(index, 1)
      }, 5000)
    }

    const clearErrors = () => {
      errors.value = []
    }

    const formatDuration = (seconds) => {
      if (!seconds) return '0:00'
      const mins = Math.floor(seconds / 60)
      const secs = Math.floor(seconds % 60)
      return `${mins}:${secs.toString().padStart(2, '0')}`
    }

    const formatTime = (seconds) => {
      return formatDuration(seconds)
    }

    const handleWavFile = async (event) => {
      const file = event.target.files[0]
      wavFile.value = file
      clearErrors()

      if (!file) {
        waveforms.value.source = null
        spectrograms.value.input = null
        audioInfo.value = null
        return
      }

      try {
        // Create audio URL for playback
        waveforms.value.source = URL.createObjectURL(file)

        // Get audio info and generate spectrogram
        const arrayBuffer = await file.arrayBuffer()
        const audioContext = new AudioContext({ sampleRate: 44100 })
        const audioBuffer = await audioContext.decodeAudioData(arrayBuffer)

        // Update audio info
        audioInfo.value = {
          sampleRate: audioBuffer.sampleRate,
          channels: audioBuffer.numberOfChannels,
          duration: audioBuffer.duration,
          bitrate: Math.round((file.size * 8) / audioBuffer.duration / 1000),
        }

        maxDuration.value = audioBuffer.duration

        // Generate input spectrogram
        isProcessing.value = true
        isGeneratingSpectrogram.value = true
        
        try {
          const pcmData = []
          for (let i = 0; i < audioBuffer.numberOfChannels; i++) {
            pcmData.push(audioBuffer.getChannelData(i))
          }

          const result = await spectrogramWorker.generateSpectrogram(pcmData, {
            fftSize: 2048,
            hopSize: 256,
            windowFunction: 'hann',
            sampleRate: audioBuffer.sampleRate,
          })

          spectrograms.value.input = result.spectrogramData
        } finally {
          isGeneratingSpectrogram.value = false
        }

        // Auto-process if enabled
        if (autoProcess.value) {
          await processAudio()
        }
      } catch (error) {
        console.error('Error processing WAV file:', error)
        addError(`Failed to process WAV file: ${error.message}`)
      } finally {
        isProcessing.value = false
      }
    }

    const handleAeaFile = (event) => {
      const file = event.target.files[0]
      aeaFile.value = file
      clearErrors()
    }

    const processAudio = async () => {
      if (!wavFile.value || !codecWorker || !spectrogramWorker) return

      try {
        isEncodingDecoding.value = true
        
        // Encode
        const arrayBuffer = await wavFile.value.arrayBuffer()
        const audioContext = new AudioContext({ sampleRate: 44100 })
        const audioBuffer = await audioContext.decodeAudioData(arrayBuffer)

        const channelCount = audioBuffer.numberOfChannels
        const pcmData = []
        for (let i = 0; i < channelCount; i++) {
          pcmData.push(audioBuffer.getChannelData(i))
        }

        const encodeResult = await codecWorker.encode(
          pcmData,
          encoderOptions.value?.values
        )
        downloadUrls.value.aea = URL.createObjectURL(encodeResult.aeaBlob)
        shortBlockData.value = encodeResult.shortBlockData

        // Parse and decode
        const { info, frameData } = await codecWorker.parseAeaBlob(
          encodeResult.aeaBlob
        )
        const decodeResult = await codecWorker.decode({
          aeaData: frameData,
          info,
        })

        // Create decoded audio URL
        waveforms.value.decoded = URL.createObjectURL(decodeResult.wavBlob)
        downloadUrls.value.wav = URL.createObjectURL(decodeResult.wavBlob)
        
        isEncodingDecoding.value = false

        // Generate output spectrogram
        isGeneratingOutputSpectrogram.value = true
        try {
          const decodedArrayBuffer = await decodeResult.wavBlob.arrayBuffer()
          const decodedAudioContext = new AudioContext({ sampleRate: 44100 })
          const decodedAudioBuffer =
            await decodedAudioContext.decodeAudioData(decodedArrayBuffer)

          const decodedPcmData = []
          for (let i = 0; i < decodedAudioBuffer.numberOfChannels; i++) {
            decodedPcmData.push(decodedAudioBuffer.getChannelData(i))
          }

          const spectrogramResult = await spectrogramWorker.generateSpectrogram(
            decodedPcmData,
            {
              fftSize: 2048,
              hopSize: 256,
              windowFunction: 'hann',
              sampleRate: decodedAudioBuffer.sampleRate,
            }
          )

          spectrograms.value.output = spectrogramResult.spectrogramData
        } finally {
          isGeneratingOutputSpectrogram.value = false
        }
      } catch (error) {
        console.error('Processing error:', error)
        addError(`Processing failed: ${error.message}`)
      } finally {
        isEncodingDecoding.value = false
      }
    }

    const encodeFile = async () => {
      if (!wavFile.value || !codecWorker) return

      isProcessing.value = true
      clearErrors()

      try {
        await processAudio()
      } finally {
        isProcessing.value = false
      }
    }

    const decodeFile = async () => {
      if (!aeaFile.value || !codecWorker || !spectrogramWorker) return

      isProcessing.value = true
      clearErrors()

      try {
        isEncodingDecoding.value = true
        
        const aeaBlob = new Blob([await aeaFile.value.arrayBuffer()])
        const { info, frameData } = await codecWorker.parseAeaBlob(aeaBlob)

        const result = await codecWorker.decode({ aeaData: frameData, info })

        waveforms.value.decoded = URL.createObjectURL(result.wavBlob)
        downloadUrls.value.wav = URL.createObjectURL(result.wavBlob)
        
        isEncodingDecoding.value = false

        // Generate output spectrogram
        isGeneratingOutputSpectrogram.value = true
        try {
          const arrayBuffer = await result.wavBlob.arrayBuffer()
          const audioContext = new AudioContext({ sampleRate: 44100 })
          const audioBuffer = await audioContext.decodeAudioData(arrayBuffer)

          const pcmData = []
          for (let i = 0; i < audioBuffer.numberOfChannels; i++) {
            pcmData.push(audioBuffer.getChannelData(i))
          }

          const spectrogramResult = await spectrogramWorker.generateSpectrogram(
            pcmData,
            {
              fftSize: 2048,
              hopSize: 256,
              windowFunction: 'hann',
              sampleRate: audioBuffer.sampleRate,
            }
          )

          spectrograms.value.output = spectrogramResult.spectrogramData
        } finally {
          isGeneratingOutputSpectrogram.value = false
        }
      } catch (error) {
        console.error('Decoding error:', error)
        addError(`Decoding failed: ${error.message}`)
      } finally {
        isEncodingDecoding.value = false
        isProcessing.value = false
      }
    }

    const clearAll = () => {
      wavFile.value = null
      aeaFile.value = null
      waveforms.value = { source: null, decoded: null }
      spectrograms.value = { input: null, output: null }
      shortBlockData.value = null
      audioInfo.value = null
      downloadUrls.value = { aea: null, wav: null }
      clearErrors()

      // Clear file inputs
      const inputs = document.querySelectorAll('input[type="file"]')
      inputs.forEach((input) => (input.value = ''))
    }

    const downloadFile = (url, filename) => {
      const a = document.createElement('a')
      a.href = url
      a.download = filename
      a.click()
    }

    const handleZoomChange = (zoomRange) => {
      globalZoomRange.value = zoomRange
      // Sync both spectrograms
      nextTick(() => {
        if (inputSpectrogram.value) inputSpectrogram.value.applyZoom(zoomRange)
        if (outputSpectrogram.value)
          outputSpectrogram.value.applyZoom(zoomRange)
      })
    }

    const togglePlayback = () => {
      isPlaying.value = !isPlaying.value

      if (isPlaying.value) {
        if (sourceAudio.value) sourceAudio.value.play()
        if (decodedAudio.value) decodedAudio.value.play()
      } else {
        if (sourceAudio.value) sourceAudio.value.pause()
        if (decodedAudio.value) decodedAudio.value.pause()
      }
    }

    const seekTo = (event) => {
      const time = parseFloat(event.target.value)
      currentTime.value = time

      if (sourceAudio.value) sourceAudio.value.currentTime = time
      if (decodedAudio.value) decodedAudio.value.currentTime = time
    }

    const handleEncoderOptionsChange = (options) => {
      console.log(options)
      // Auto-encode if enabled and we have a WAV file
      if (autoProcess.value && wavFile.value) {
        encodeFile()
      }
    }

    const resetEncoderOptions = () => {
      if (encoderOptions.value) {
        for (const [key, metadata] of Object.entries(
          encoderOptions.value.metadata
        )) {
          encoderOptions.value.values[key] = metadata.default
        }
        handleEncoderOptionsChange(encoderOptions.value)
      }
    }

    const updateSliderProgress = (event) => {
      updateSliderAppearance(event.target)
    }

    const updateSliderAppearance = (slider) => {
      const value = slider.value
      const min = slider.min || 0
      const max = slider.max || 100
      const percentage = ((value - min) / (max - min)) * 100
      slider.style.setProperty('--value', `${percentage}%`)
    }

    const handleSliderChange = (event, options) => {
      updateSliderProgress(event)
      if (options) {
        handleEncoderOptionsChange(options)
      }
    }

    const handleSeekChange = (event) => {
      seekTo(event)
      updateSliderProgress(event)
    }

    // Setup audio elements for playback sync
    watch(
      () => waveforms.value.source,
      (newUrl) => {
        if (newUrl) {
          nextTick(() => {
            sourceAudio.value = document.querySelector(
              'audio[src="' + newUrl + '"]'
            )
            if (sourceAudio.value) {
              sourceAudio.value.addEventListener('timeupdate', () => {
                currentTime.value = sourceAudio.value.currentTime
              })
            }
          })
        }
      }
    )

    watch(
      () => waveforms.value.decoded,
      (newUrl) => {
        if (newUrl) {
          nextTick(() => {
            decodedAudio.value = document.querySelector(
              'audio[src="' + newUrl + '"]'
            )
          })
        }
      }
    )

    onMounted(() => {
      initWorkers()
      // Initialize all sliders
      nextTick(() => {
        document
          .querySelectorAll('input[type="range"]')
          .forEach(updateSliderAppearance)
      })
    })

    onUnmounted(() => {
      if (codecWorker) codecWorker.terminate()
      if (spectrogramWorker) spectrogramWorker.terminate()

      // Clean up blob URLs
      Object.values(waveforms.value).forEach((url) => {
        if (url) URL.revokeObjectURL(url)
      })
      Object.values(downloadUrls.value).forEach((url) => {
        if (url) URL.revokeObjectURL(url)
      })
    })

    return {
      isDarkMode,
      wavFile,
      aeaFile,
      autoProcess,
      isProcessing,
      isGeneratingSpectrogram,
      isGeneratingOutputSpectrogram,
      isEncodingDecoding,
      errors,
      waveforms,
      spectrograms,
      shortBlockData,
      audioInfo,
      downloadUrls,
      isPlaying,
      currentTime,
      maxDuration,
      globalZoomRange,
      encoderOptions,
      inputSpectrogram,
      outputSpectrogram,
      toggleDarkMode,
      handleWavFile,
      handleAeaFile,
      encodeFile,
      decodeFile,
      clearAll,
      downloadFile,
      handleZoomChange,
      togglePlayback,
      seekTo,
      handleEncoderOptionsChange,
      resetEncoderOptions,
      updateSliderProgress,
      handleSliderChange,
      handleSeekChange,
      formatDuration,
      formatTime,
    }
  },
}
</script>

<style>
.btn-primary {
  @apply px-5 py-2.5 bg-text text-surface rounded-md hover:opacity-90 disabled:opacity-50 disabled:cursor-not-allowed transition-opacity font-medium text-sm;
}

.btn-secondary {
  @apply px-5 py-2.5 bg-transparent border border-border text-text-muted rounded-md hover:border-text-muted hover:text-text disabled:opacity-50 disabled:cursor-not-allowed transition-all font-medium text-sm;
}

/* Custom scrollbar */
::-webkit-scrollbar {
  width: 8px;
  height: 8px;
}

::-webkit-scrollbar-track {
  background: rgb(var(--color-surface-muted));
}

::-webkit-scrollbar-thumb {
  background: rgb(var(--color-border));
  border-radius: 4px;
}

::-webkit-scrollbar-thumb:hover {
  background: rgb(var(--color-text-muted));
}
</style>
