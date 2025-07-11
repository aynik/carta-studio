<template>
  <div
    class="bg-surface-alt/50 rounded-lg border border-border/50 shadow-sm h-full flex flex-col"
  >
    <div
      class="flex items-center justify-between px-4 py-3 border-b border-border/50 bg-surface"
    >
      <div>
        <h3 class="text-sm font-medium text-text">{{ title }}</h3>
        <div v-if="hasData" class="text-xs text-text-muted mt-1">
          Drag to zoom • Double-click to reset
        </div>
      </div>
      <div class="flex items-center space-x-3">
        <button
          v-if="hasData && (currentZoom.start !== 0 || currentZoom.end !== 1)"
          @click="resetZoom"
          class="px-3 py-1.5 text-xs bg-transparent border border-border text-text-muted hover:border-text-muted hover:text-text rounded-md transition-all"
        >
          Reset
        </button>
        <audio
          v-if="audioSrc"
          ref="audio"
          controls
          :src="audioSrc"
          class="h-8"
        ></audio>
      </div>
    </div>

    <div class="flex-1 p-3 relative min-h-0 bg-surface">
      <div
        ref="canvasContainer"
        class="w-full h-full relative bg-black/80 border border-border/50 rounded-md overflow-hidden"
      >
        <canvas
          ref="canvas"
          class="absolute inset-0 w-full h-full cursor-crosshair"
          @mousedown="startZoom"
          @mousemove="updateZoom"
          @mouseup="endZoom"
          @wheel="handleWheel"
          @dblclick="handleDoubleClick"
        ></canvas>

        <!-- Horizontal scrollbar -->
        <div
          v-if="hasData"
          class="absolute bottom-0 left-0 right-0 h-3 bg-surface-muted border-t border-border"
        >
          <div
            class="h-full bg-accent cursor-pointer relative"
            :style="{
              width: scrollbarThumbWidth + '%',
              left: scrollbarThumbLeft + '%',
            }"
            @mousedown="startScrollDrag"
          ></div>
        </div>

        <div
          v-if="!hasData"
          class="absolute inset-0 flex items-center justify-center"
        >
          <div class="text-center text-text-muted">
            <div
              class="mx-auto w-8 h-8 mb-2 bg-text-muted rounded opacity-50"
            ></div>
            <p class="text-xs">{{ placeholder }}</p>
          </div>
        </div>

        <div
          v-if="isLoading"
          class="absolute inset-0 flex items-center justify-center bg-surface/90"
        >
          <div class="text-center">
            <div
              class="animate-spin rounded-full h-6 w-6 border-b-2 border-accent mx-auto mb-2"
            ></div>
            <p class="text-xs text-text-muted">Generating...</p>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
import { ref, onMounted, watch, nextTick, computed } from 'vue'

export default {
  name: 'SpectrogramViewer',
  props: {
    title: {
      type: String,
      required: true,
    },
    audioSrc: {
      type: String,
      default: null,
    },
    spectrogramData: {
      type: Array,
      default: null,
    },
    shortBlockData: {
      type: Object,
      default: null,
    },
    placeholder: {
      type: String,
      default: 'No data to display',
    },
    width: {
      type: Number,
      default: 1024,
    },
    height: {
      type: Number,
      default: 512,
    },
    zoomRange: {
      type: Object,
      default: () => ({ start: 0, end: 1 }),
    },
  },
  emits: ['zoom-changed'],
  setup(props, { emit }) {
    const canvas = ref(null)
    const canvasContainer = ref(null)
    const audio = ref(null)
    const hasData = ref(false)
    const isLoading = ref(false)

    // Zoom state
    const isZooming = ref(false)
    const zoomStart = ref(0)
    const zoomEnd = ref(0)
    const currentZoom = ref({ start: 0, end: 1 })

    // Scroll state
    const isScrollDragging = ref(false)
    const isScrolling = ref(false)
    const scrollStartX = ref(0)
    let scrollTimeout = null

    // Progressive rendering state
    const renderCache = new Map()
    const isRendering = ref(false)

    // Rendering context
    let ctx = null
    let overlayCtx = null
    let baseImageData = null
    let overlayCanvas = null
    let animationFrameId = null

    // Computed properties for scrollbar
    const scrollbarThumbWidth = computed(() => {
      const range = currentZoom.value.end - currentZoom.value.start
      return Math.max(range * 100, 5) // Minimum 5% width
    })

    const scrollbarThumbLeft = computed(() => {
      return currentZoom.value.start * 100
    })

    const initCanvas = async () => {
      await nextTick()
      if (!canvas.value || !canvasContainer.value) return

      const container = canvasContainer.value
      const rect = container.getBoundingClientRect()

      canvas.value.width = rect.width || props.width
      canvas.value.height = (rect.height || props.height) - 12 // Account for scrollbar
      ctx = canvas.value.getContext('2d')

      // Create overlay canvas for zoom rectangle
      overlayCanvas = document.createElement('canvas')
      overlayCanvas.width = canvas.value.width
      overlayCanvas.height = canvas.value.height
      overlayCanvas.style.position = 'absolute'
      overlayCanvas.style.top = '0'
      overlayCanvas.style.left = '0'
      overlayCanvas.style.pointerEvents = 'none'
      overlayCanvas.style.zIndex = '10'
      canvasContainer.value.appendChild(overlayCanvas)
      overlayCtx = overlayCanvas.getContext('2d')

      // Create base ImageData for pixel manipulation
      baseImageData = ctx.createImageData(
        canvas.value.width,
        canvas.value.height
      )
    }

    const getCacheKey = (zoomRange) => {
      return `${zoomRange.start.toFixed(3)}-${zoomRange.end.toFixed(3)}`
    }

    const renderSpectrogramProgressive = (zoomRange = currentZoom.value) => {
      if (!props.spectrogramData || !ctx || !baseImageData) return

      const cacheKey = getCacheKey(zoomRange)

      // Check cache first
      if (renderCache.has(cacheKey) && !isScrolling.value) {
        const cachedImageData = renderCache.get(cacheKey)
        ctx.putImageData(cachedImageData, 0, 0)
        hasData.value = true
        isLoading.value = false
        return
      }

      isLoading.value = true

      // Cancel previous render
      if (animationFrameId) {
        cancelAnimationFrame(animationFrameId)
      }

      animationFrameId = requestAnimationFrame(() => {
        const timeSlices = props.spectrogramData.length
        const freqBins = props.spectrogramData[0]?.length || 0

        if (timeSlices === 0 || freqBins === 0) {
          isLoading.value = false
          return
        }

        const canvasWidth = canvas.value.width
        const canvasHeight = canvas.value.height - 3

        const imageData = ctx.createImageData(canvasWidth, canvasHeight)
        const data = imageData.data
        data.fill(0) // Black background

        // Calculate zoom range
        const startIdx = Math.floor(zoomRange.start * timeSlices)
        const endIdx = Math.floor(zoomRange.end * timeSlices)
        const zoomedSlices = endIdx - startIdx

        // Find percentiles for adaptive scaling
        const samples = []
        const sampleRate = Math.max(1, Math.floor(zoomedSlices / 1000)) // Sample ~1000 points

        for (let i = startIdx; i < endIdx; i += sampleRate) {
          if (i >= timeSlices) break
          const spectrum = props.spectrogramData[i]
          for (
            let j = 0;
            j < freqBins;
            j += Math.max(1, Math.floor(freqBins / 50))
          ) {
            if (spectrum[j] > 0) samples.push(spectrum[j]) // Only positive values
          }
        }

        samples.sort((a, b) => a - b)
        const p10 = samples[Math.floor(samples.length * 0.1)] || 0
        const p50 = samples[Math.floor(samples.length * 0.5)] || 0.5
        const p90 = samples[Math.floor(samples.length * 0.9)] || 1
        const p99 = samples[Math.floor(samples.length * 0.99)] || 1.5

        for (let x = 0; x < canvasWidth; x++) {
          const timeIdx =
            startIdx + Math.floor((x / canvasWidth) * zoomedSlices)
          if (timeIdx >= timeSlices) continue

          const spectrum = props.spectrogramData[timeIdx]

          for (let y = 0; y < canvasHeight; y++) {
            // Flip Y axis (high frequencies at top)
            const freqIdx = Math.floor(
              ((canvasHeight - y) / canvasHeight) * freqBins
            )
            if (freqIdx >= freqBins) continue

            const magnitude = spectrum[freqIdx]

            // Multi-segment non-linear mapping
            let intensity

            if (magnitude <= 0) {
              // Negative values = black
              intensity = 0
            } else if (magnitude < p10) {
              // Bottom 10% = very dark (0-20)
              intensity = (magnitude / p10) * 20
            } else if (magnitude < p50) {
              // 10-50 percentile = dark gray (20-80)
              intensity = 20 + ((magnitude - p10) / (p50 - p10)) * 60
            } else if (magnitude < p90) {
              // 50-90 percentile = mid to light gray (80-180)
              intensity = 80 + ((magnitude - p50) / (p90 - p50)) * 100
            } else if (magnitude < p99) {
              // 90-99 percentile = light gray to near white (180-240)
              intensity = 180 + ((magnitude - p90) / (p99 - p90)) * 60
            } else {
              // Top 1% = near white to pure white (240-255)
              intensity = 240 + ((magnitude - p99) / (p99 * 0.1)) * 15
            }

            intensity = Math.floor(Math.max(0, Math.min(255, intensity)))

            const pixelIndex = (y * canvasWidth + x) * 4
            data[pixelIndex] = intensity // R
            data[pixelIndex + 1] = intensity // G
            data[pixelIndex + 2] = intensity // B
            data[pixelIndex + 3] = 255 // A
          }
        }

        ctx.putImageData(imageData, 0, 0)

        // Render short block indicators on top of spectrogram
        renderShortBlockIndicators(zoomRange, canvasWidth, canvasHeight)

        // Cache the result
        renderCache.set(
          cacheKey,
          ctx.getImageData(0, 0, canvasWidth, canvasHeight)
        )

        // Limit cache size
        if (renderCache.size > 20) {
          const firstKey = renderCache.keys().next().value
          renderCache.delete(firstKey)
        }

        hasData.value = true
        isLoading.value = false
      })
    }

    const renderShortBlockIndicators = (
      zoomRange,
      canvasWidth,
      canvasHeight
    ) => {
      if (!props.shortBlockData || !ctx) return

      const frameCount = props.spectrogramData?.length || 1
      const startIdx = Math.floor(zoomRange.start * frameCount)
      const endIdx = Math.floor(zoomRange.end * frameCount)
      const zoomedFrames = endIdx - startIdx

      // Calculate frame positions relative to the current zoom
      const getFrameX = (frameIndex) => {
        if (frameIndex < startIdx || frameIndex > endIdx) return -1
        return ((frameIndex - startIdx) / zoomedFrames) * canvasWidth
      }

      // Define band heights (50% high, 25% mid, 25% low from top to bottom)
      const highBandHeight = canvasHeight * 0.5
      const midBandStart = highBandHeight
      const midBandHeight = canvasHeight * 0.25
      const lowBandStart = midBandStart + midBandHeight
      const lowBandHeight = canvasHeight * 0.25

      ctx.lineWidth = 1
      ctx.globalAlpha = 0.5

      // Render high band indicators (blue) - top 50%
      ctx.strokeStyle = '#0080ff'
      if (props.shortBlockData.highBand) {
        for (const frameIndex of props.shortBlockData.highBand) {
          const x = getFrameX(frameIndex)
          if (x >= 0) {
            ctx.beginPath()
            ctx.moveTo(x, 0)
            ctx.lineTo(x, highBandHeight)
            ctx.stroke()
          }
        }
      }

      // Render mid band indicators (green) - middle 25%
      ctx.strokeStyle = '#00ff00'
      if (props.shortBlockData.midBand) {
        for (const frameIndex of props.shortBlockData.midBand) {
          const x = getFrameX(frameIndex)
          if (x >= 0) {
            ctx.beginPath()
            ctx.moveTo(x, midBandStart)
            ctx.lineTo(x, midBandStart + midBandHeight)
            ctx.stroke()
          }
        }
      }

      // Render low band indicators (red) - bottom 25%
      ctx.strokeStyle = '#ff0000'
      if (props.shortBlockData.lowBand) {
        for (const frameIndex of props.shortBlockData.lowBand) {
          const x = getFrameX(frameIndex)
          if (x >= 0) {
            ctx.beginPath()
            ctx.moveTo(x, lowBandStart)
            ctx.lineTo(x, lowBandStart + lowBandHeight)
            ctx.stroke()
          }
        }
      }

      ctx.globalAlpha = 1.0
    }

    const renderSpectrogram = () => {
      renderSpectrogramProgressive()
    }

    const startZoom = (event) => {
      isZooming.value = true
      const rect = canvas.value.getBoundingClientRect()
      zoomStart.value = (event.clientX - rect.left) / rect.width
    }

    const updateZoom = (event) => {
      if (!isZooming.value) return
      const rect = canvas.value.getBoundingClientRect()
      zoomEnd.value = (event.clientX - rect.left) / rect.width

      // Draw zoom selection overlay on overlay canvas
      if (overlayCtx) {
        overlayCtx.clearRect(0, 0, overlayCanvas.width, overlayCanvas.height)

        const startX =
          Math.min(zoomStart.value, zoomEnd.value) * overlayCanvas.width
        const endX =
          Math.max(zoomStart.value, zoomEnd.value) * overlayCanvas.width

        // Draw selection rectangle
        overlayCtx.fillStyle = 'rgba(255, 255, 255, 0.2)'
        overlayCtx.fillRect(startX, 0, endX - startX, overlayCanvas.height)

        // Draw border
        overlayCtx.strokeStyle = 'rgba(255, 255, 255, 0.6)'
        overlayCtx.lineWidth = 1
        overlayCtx.strokeRect(startX, 0, endX - startX, overlayCanvas.height)
      }
    }

    const endZoom = () => {
      if (!isZooming.value) return
      isZooming.value = false

      // Clear overlay
      if (overlayCtx) {
        overlayCtx.clearRect(0, 0, overlayCanvas.width, overlayCanvas.height)
      }

      const start = Math.min(zoomStart.value, zoomEnd.value)
      const end = Math.max(zoomStart.value, zoomEnd.value)

      if (Math.abs(end - start) > 0.01) {
        // Minimum zoom threshold
        // Map to current zoom range
        const range = currentZoom.value.end - currentZoom.value.start
        const newZoom = {
          start: currentZoom.value.start + start * range,
          end: currentZoom.value.start + end * range,
        }

        currentZoom.value = newZoom
        emit('zoom-changed', newZoom)
        renderSpectrogramProgressive(newZoom)
      }
    }

    const handleWheel = (event) => {
      event.preventDefault()

      // Reset zoom on ctrl+wheel
      if (event.ctrlKey) {
        resetZoom()
        return
      }

      // Horizontal scrolling on canvas
      if (!isScrollDragging.value) {
        const deltaX = event.deltaX || event.deltaY * 0.5
        const scrollSpeed = 0.001
        const range = currentZoom.value.end - currentZoom.value.start
        const deltaRatio = deltaX * scrollSpeed

        const newStart = Math.max(
          0,
          Math.min(1 - range, currentZoom.value.start + deltaRatio)
        )
        const newZoom = {
          start: newStart,
          end: newStart + range,
        }

        if (newZoom.start !== currentZoom.value.start) {
          currentZoom.value = newZoom
          emit('zoom-changed', newZoom)

          // Mark as scrolling for performance
          isScrolling.value = true
          clearTimeout(scrollTimeout)
          scrollTimeout = setTimeout(() => {
            isScrolling.value = false
            renderSpectrogramProgressive(currentZoom.value)
          }, 150)

          // Render with caching
          renderSpectrogramProgressive(newZoom)
        }
      }
    }

    const handleDoubleClick = (event) => {
      event.preventDefault()
      resetZoom()
    }

    const resetZoom = () => {
      currentZoom.value = { start: 0, end: 1 }
      emit('zoom-changed', currentZoom.value)
      // Clear cache and re-render
      renderCache.clear()
      renderSpectrogramProgressive(currentZoom.value)
    }

    const startScrollDrag = (event) => {
      isScrollDragging.value = true
      scrollStartX.value = event.clientX
      event.preventDefault()

      const handleMouseMove = (e) => {
        if (!isScrollDragging.value) return

        const deltaX = e.clientX - scrollStartX.value
        const containerWidth =
          canvasContainer.value.getBoundingClientRect().width
        const deltaRatio = deltaX / containerWidth

        const range = currentZoom.value.end - currentZoom.value.start
        const newStart = Math.max(
          0,
          Math.min(1 - range, currentZoom.value.start + deltaRatio)
        )
        const newZoom = {
          start: newStart,
          end: newStart + range,
        }

        currentZoom.value = newZoom
        emit('zoom-changed', newZoom)

        // Mark as scrolling for performance
        isScrolling.value = true
        clearTimeout(scrollTimeout)

        // Render while dragging
        renderSpectrogramProgressive(newZoom)

        scrollStartX.value = e.clientX
      }

      const handleMouseUp = () => {
        isScrollDragging.value = false

        // Re-render with higher quality after scrolling stops
        scrollTimeout = setTimeout(() => {
          isScrolling.value = false
          renderSpectrogramProgressive(currentZoom.value)
        }, 100)

        document.removeEventListener('mousemove', handleMouseMove)
        document.removeEventListener('mouseup', handleMouseUp)
      }

      document.addEventListener('mousemove', handleMouseMove)
      document.addEventListener('mouseup', handleMouseUp)
    }

    // Public method to apply zoom from parent
    const applyZoom = (zoomRange) => {
      currentZoom.value = zoomRange
      renderSpectrogram()
    }

    // Watch for external zoom changes
    watch(
      () => props.zoomRange,
      (newZoom) => {
        if (
          newZoom &&
          (newZoom.start !== currentZoom.value.start ||
            newZoom.end !== currentZoom.value.end)
        ) {
          currentZoom.value = newZoom
          renderSpectrogramProgressive(newZoom)
        }
      },
      { deep: true }
    )

    // Watch for data changes
    watch(
      () => props.spectrogramData,
      () => {
        if (props.spectrogramData) {
          renderCache.clear() // Clear cache when new data arrives
          renderSpectrogramProgressive()
        }
      },
      { deep: true }
    )

    // Watch for short block data changes
    watch(
      () => props.shortBlockData,
      () => {
        if (props.spectrogramData) {
          renderCache.clear() // Clear cache when short block data changes
          renderSpectrogramProgressive()
        }
      },
      { deep: true }
    )

    // Handle window resize
    const handleResize = () => {
      initCanvas()
      if (props.spectrogramData) {
        renderCache.clear() // Clear cache on resize
        renderSpectrogramProgressive()
      }
    }

    onMounted(() => {
      initCanvas()
      window.addEventListener('resize', handleResize)
    })

    return {
      canvas,
      canvasContainer,
      audio,
      hasData,
      isLoading,
      currentZoom,
      scrollbarThumbWidth,
      scrollbarThumbLeft,
      startZoom,
      updateZoom,
      endZoom,
      handleWheel,
      handleDoubleClick,
      resetZoom,
      startScrollDrag,
      applyZoom,
    }
  },
}
</script>

<style scoped>
/* Additional component-specific styles if needed */
</style>
