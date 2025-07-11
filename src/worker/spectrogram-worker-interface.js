/**
 * Interface for the Spectrogram Worker
 */

export class SpectrogramWorker {
  constructor() {
    try {
      this.worker = new Worker(
        new URL('./spectrogram-worker.js', import.meta.url)
      )
      this.nextJobId = 1
      this.jobs = new Map()
    } catch (error) {
      console.error('Failed to create SpectrogramWorker:', error)
      this.worker = null
      return null
    }

    this.worker.onmessage = (event) => {
      const { jobId, result, error, progress } = event.data

      if (progress) {
        // Handle progress updates
        this.onProgress?.(progress)
        return
      }

      if (!jobId) return

      const job = this.jobs.get(jobId)
      if (!job) return

      this.jobs.delete(jobId)

      if (error) {
        job.reject(new Error(error))
      } else {
        job.resolve(result)
      }
    }

    this.worker.onerror = (error) => {
      console.error('Spectrogram worker error:', error)
      // Reject all pending jobs
      for (const [jobId, job] of this.jobs) {
        job.reject(new Error('Worker error'))
        this.jobs.delete(jobId)
      }
    }
  }

  async generateSpectrogram(audioData, options = {}) {
    return this._postMessage('generateSpectrogram', {
      audioData,
      options,
    })
  }

  _postMessage(type, data) {
    const jobId = this.nextJobId++

    return new Promise((resolve, reject) => {
      this.jobs.set(jobId, { resolve, reject })

      try {
        this.worker.postMessage({
          jobId,
          type,
          ...data,
        })
      } catch (error) {
        this.jobs.delete(jobId)
        reject(error)
      }
    })
  }

  setProgressCallback(callback) {
    this.onProgress = callback
  }

  terminate() {
    this.worker.terminate()

    // Reject all pending jobs
    for (const [jobId, job] of this.jobs) {
      job.reject(new Error('Worker terminated'))
    }
    this.jobs.clear()
  }
}
