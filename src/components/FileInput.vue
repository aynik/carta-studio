<template>
  <div class="file-input-wrapper">
    <input
      :id="id"
      type="file"
      :accept="accept"
      :disabled="disabled"
      @change="handleFileChange"
      class="file-input"
    />
    <label
      :for="id"
      class="file-label"
      :class="{ disabled, 'has-file': currentFile }"
    >
      <span class="file-icon">📁</span>
      <span class="file-text">
        {{ currentFile ? currentFile.name : placeholder }}
      </span>
      <span v-if="currentFile" class="file-size">
        ({{ formatFileSize(currentFile.size) }})
      </span>
    </label>
    <button
      v-if="currentFile"
      @click="clearFile"
      :disabled="disabled"
      class="clear-btn"
      type="button"
    >
      ✕
    </button>
  </div>
</template>

<script>
export default {
  name: 'FileInput',
  props: {
    id: {
      type: String,
      required: true,
    },
    accept: {
      type: String,
      default: '*',
    },
    disabled: {
      type: Boolean,
      default: false,
    },
    currentFile: {
      type: File,
      default: null,
    },
    placeholder: {
      type: String,
      default: 'Choose file',
    },
  },
  emits: ['file-selected'],
  setup(props, { emit }) {
    const handleFileChange = (event) => {
      const file = event.target.files[0]
      emit('file-selected', file)
    }

    const clearFile = () => {
      emit('file-selected', null)
      // Clear the input
      const input = document.getElementById(props.id)
      if (input) input.value = ''
    }

    const formatFileSize = (bytes) => {
      if (bytes === 0) return '0 B'
      const k = 1024
      const sizes = ['B', 'KB', 'MB', 'GB']
      const i = Math.floor(Math.log(bytes) / Math.log(k))
      return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i]
    }

    return {
      handleFileChange,
      clearFile,
      formatFileSize,
    }
  },
}
</script>

<style scoped>
.file-input-wrapper {
  position: relative;
  margin-bottom: 1rem;
}

.file-input {
  display: none;
}

.file-label {
  display: flex;
  align-items: center;
  padding: 0.75rem 1rem;
  background: var(--input-bg);
  border: 2px dashed var(--border-color);
  border-radius: 6px;
  cursor: pointer;
  transition: all 0.2s ease;
  min-height: 50px;
}

.file-label:hover:not(.disabled) {
  border-color: var(--primary-color);
  background: var(--input-hover-bg);
}

.file-label.has-file {
  border-style: solid;
  border-color: var(--primary-color);
  background: var(--primary-bg);
}

.file-label.disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.file-icon {
  margin-right: 0.5rem;
  font-size: 1.2rem;
}

.file-text {
  flex: 1;
  color: var(--text-color);
  font-size: 0.9rem;
  font-weight: 500;
  word-break: break-all;
}

.file-size {
  color: var(--text-muted);
  font-size: 0.8rem;
  margin-left: 0.5rem;
}

.clear-btn {
  position: absolute;
  top: 0.5rem;
  right: 0.5rem;
  width: 24px;
  height: 24px;
  border: none;
  background: var(--error-bg);
  color: var(--error-color);
  border-radius: 50%;
  cursor: pointer;
  font-size: 0.8rem;
  display: flex;
  align-items: center;
  justify-content: center;
  transition: all 0.2s ease;
}

.clear-btn:hover:not(:disabled) {
  background: var(--error-color);
  color: white;
}

.clear-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}
</style>
