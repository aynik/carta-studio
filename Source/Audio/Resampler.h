/**
 * Band-limited sinc interpolation and prepared kernel cache for audio playback
 * and offline rendering.
 *
 * Coordinates: `SincResampler::sample()` evaluates at a continuous source-frame
 * position within `[0, sampleCount)` in the source's native sample rate, with
 * endpoint-sample extension outside `[0, sampleCount)`.
 *
 * Threading & real-time contract:
 * - `SincResampler` builds its immutable Blackman-windowed coefficient table at
 *   construction; `sample()` and `sampleStereo()` perform no locks or heap
 *   allocations.
 * - `ResamplingContext` is populated on non-rendering threads and queried via
 *   `find()` on the real-time audio callback (read-only).
 */
#pragma once
#include <vector>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace carta {

// =========================================================================
// Band-Limited Sinc Interpolator
// =========================================================================

/**
 * Stateless, random-access band-limited interpolation.
 *
 * Construction builds an immutable coefficient table; `sample()` does not
 * allocate or take locks. Signal beyond either source boundary is extended
 * with its endpoint sample.
 */
class SincResampler {
  public:
    static constexpr int baseTaps = 256;
    static constexpr int phases = 2048;

    /**
     * Above this downsampling ratio the bounded kernel is shorter than the
     * reference design, so its transition band and passband become wider.
     */
    static constexpr double maxFullSupportRatio = 16.0 * 96000.0 / 44100.0;

    /** Return true if `sourceRate / outputRate` stays within full kernel support. */
    static bool hasFullSupport(double sourceRate, double outputRate) noexcept;

    /** Return even tap count for converting `sourceRate` to `outputRate` (0 if invalid). */
    static int supportTaps(double sourceRate, double outputRate) noexcept;

    SincResampler(double sourceRate, double outputRate);

    bool valid() const noexcept { return !coefficients.empty(); }
    bool matches(double source, double output) const noexcept;

    /** Evaluate mono source PCM at continuous frame `sourcePosition` (0 outside bounds). */
    float sample(const float* data, int sampleCount, double sourcePosition) const noexcept;

    /** Same calculation for two planar channels, sharing phase and coefficient lookup. */
    std::pair<float, float> sampleStereo(const float* left, const float* right, int sampleCount,
                                         double sourcePosition) const noexcept;

  private:
    int taps = 0;
    double sourceRate = 0;
    double outputRate = 0;
    std::vector<float> coefficients;
};

// =========================================================================
// Prepared Resampler Registry
// =========================================================================

/**
 * Registry of precomputed `SincResampler` kernels keyed by `(sourceRate, outputRate)`.
 *
 * Prepare on a non-rendering thread, then use `find()` from the audio callback.
 * Rates remain registered so undo and clipboard can reintroduce old sources.
 */
class ResamplingContext {
  public:
    /** Register `sourceRate` and ensure a kernel exists for `currentOutputRate`. */
    void registerSource(double sourceRate, double currentOutputRate);

    /** Install a table built off-thread; a missing or stale table is rebuilt. */
    void registerPrepared(double sourceRate, double outputRate,
                          std::shared_ptr<const SincResampler>);

    /** Build kernels for all registered source rates at a new `outputRate`. */
    void prepareOutput(double outputRate);

    /** Look up a prepared kernel without allocating or locking (`nullptr` if absent). */
    const SincResampler* find(double sourceRate, double outputRate) const noexcept;

  private:
    void prepare(double sourceRate, double outputRate);
    std::set<double> sourceRates;
    std::map<std::pair<double, double>, std::shared_ptr<const SincResampler>> kernels;
};

} // namespace carta
