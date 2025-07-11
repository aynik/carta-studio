<template>
  <div class="control-panel">
    <div class="panel-section">
      <h3>Audio Input</h3>
      <FileInput
        id="wav-input"
        accept=".wav"
        :disabled="isProcessing"
        :current-file="wavFile"
        placeholder="Choose WAV file"
        @file-selected="$emit('wav-file-selected', $event)"
      />

      <FileInput
        id="aea-input"
        accept=".aea"
        :disabled="isProcessing"
        :current-file="aeaFile"
        placeholder="Choose AEA file"
        @file-selected="$emit('aea-file-selected', $event)"
      />
    </div>

    <div class="panel-section">
      <h3>Operations</h3>
      <button
        @click="$emit('encode')"
        :disabled="!wavFile || isProcessing"
        class="operation-btn primary"
      >
        {{ isProcessing ? 'Encoding...' : 'Encode to ATRAC1' }}
      </button>

      <button
        @click="$emit('decode')"
        :disabled="!aeaFile || isProcessing"
        class="operation-btn primary"
      >
        {{ isProcessing ? 'Decoding...' : 'Decode to WAV' }}
      </button>

      <button
        @click="$emit('clear')"
        :disabled="isProcessing"
        class="operation-btn secondary"
      >
        Clear All
      </button>
    </div>

    <div class="panel-section">
      <h3>Encoder Settings</h3>
      <div class="control-group">
        <label>Transient Sensitivity</label>
        <input
          type="range"
          min="0.1"
          max="3.0"
          step="0.1"
          v-model.number="transientSensitivity"
          @input="updateEncoderOptions"
          class="slider"
        />
        <span class="value">{{ transientSensitivity.toFixed(1) }}</span>
      </div>

      <div class="control-group">
        <label>Normalization Target (dB)</label>
        <input
          type="range"
          min="60"
          max="80"
          step="1"
          v-model.number="normalizationDb"
          @input="updateEncoderOptions"
          class="slider"
        />
        <span class="value">{{ normalizationDb }} dB</span>
      </div>

      <button
        @click="resetEncoderOptions"
        class="operation-btn secondary small"
      >
        Reset to Defaults
      </button>
    </div>

    <div v-if="errors.length > 0" class="panel-section">
      <h3>Errors</h3>
      <div v-for="error in errors" :key="error" class="error">
        {{ error }}
      </div>
    </div>

    <div class="panel-section">
      <h3>Info</h3>
      <div v-if="audioInfo" class="info-grid">
        <div class="info-item">
          <span class="label">Sample Rate:</span>
          <span class="value">{{ audioInfo.sampleRate }} Hz</span>
        </div>
        <div class="info-item">
          <span class="label">Channels:</span>
          <span class="value">{{ audioInfo.channels }}</span>
        </div>
        <div class="info-item">
          <span class="label">Duration:</span>
          <span class="value">{{ formatDuration(audioInfo.duration) }}</span>
        </div>
        <div v-if="audioInfo.bitrate" class="info-item">
          <span class="label">Bitrate:</span>
          <span class="value">{{ audioInfo.bitrate }} kbps</span>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
import { ref } from 'vue'
import FileInput from './FileInput.vue'

export default {
  name: 'ControlPanel',
  components: {
    FileInput,
  },
  props: {
    wavFile: {
      type: File,
      default: null,
    },
    aeaFile: {
      type: File,
      default: null,
    },
    isProcessing: {
      type: Boolean,
      default: false,
    },
    errors: {
      type: Array,
      default: () => [],
    },
    audioInfo: {
      type: Object,
      default: null,
    },
  },
  emits: [
    'wav-file-selected',
    'aea-file-selected',
    'encode',
    'decode',
    'clear',
    'encoder-options-changed',
  ],
  setup(props, { emit }) {
    // Encoder options
    const transientSensitivity = ref(1.0)
    const normalizationDb = ref(68)

    const updateEncoderOptions = () => {
      emit('encoder-options-changed', {
        transientSensitivity: transientSensitivity.value,
        normalizationDb: normalizationDb.value,
      })
    }

    const resetEncoderOptions = () => {
      transientSensitivity.value = 1.0
      normalizationDb.value = 68
      updateEncoderOptions()
    }

    const formatDuration = (seconds) => {
      if (!seconds) return '0:00'
      const mins = Math.floor(seconds / 60)
      const secs = Math.floor(seconds % 60)
      return `${mins}:${secs.toString().padStart(2, '0')}`
    }

    return {
      transientSensitivity,
      normalizationDb,
      updateEncoderOptions,
      resetEncoderOptions,
      formatDuration,
    }
  },
}
</script>

<style scoped>
.control-panel {
  background: var(--panel-bg);
  border-right: 1px solid var(--border-color);
  padding: 1rem;
  overflow-y: auto;
  min-width: 280px;
  max-width: 320px;
}

.panel-section {
  margin-bottom: 2rem;
}

.panel-section h3 {
  margin: 0 0 1rem 0;
  color: var(--text-color);
  font-size: 1rem;
  font-weight: 600;
  padding-bottom: 0.5rem;
  border-bottom: 1px solid var(--border-color);
}

.operation-btn {
  width: 100%;
  padding: 0.75rem 1rem;
  margin-bottom: 0.5rem;
  border: none;
  border-radius: 6px;
  font-size: 0.9rem;
  font-weight: 500;
  cursor: pointer;
  transition: all 0.2s ease;
}

.operation-btn.primary {
  background: var(--primary-color);
  color: white;
}

.operation-btn.primary:hover:not(:disabled) {
  background: var(--primary-hover);
}

.operation-btn.secondary {
  background: var(--secondary-bg);
  color: var(--text-color);
  border: 1px solid var(--border-color);
}

.operation-btn.secondary:hover:not(:disabled) {
  background: var(--secondary-hover);
}

.operation-btn.small {
  padding: 0.5rem 0.75rem;
  font-size: 0.8rem;
}

.operation-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.control-group {
  margin-bottom: 1rem;
}

.control-group label {
  display: block;
  margin-bottom: 0.5rem;
  color: var(--text-color);
  font-size: 0.9rem;
  font-weight: 500;
}

.slider {
  width: 100%;
  margin-bottom: 0.25rem;
}

.control-group .value {
  font-size: 0.8rem;
  color: var(--text-muted);
  float: right;
}

.error {
  background: var(--error-bg);
  color: var(--error-color);
  padding: 0.75rem;
  border-radius: 6px;
  font-size: 0.9rem;
  margin-bottom: 0.5rem;
}

.info-grid {
  display: grid;
  gap: 0.5rem;
}

.info-item {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 0.5rem;
  background: var(--secondary-bg);
  border-radius: 4px;
  font-size: 0.85rem;
}

.info-item .label {
  color: var(--text-muted);
  font-weight: 500;
}

.info-item .value {
  color: var(--text-color);
  font-weight: 600;
}
</style>
