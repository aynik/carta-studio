/**
 * Off-thread spectrogram tile synthesis and logarithmic frequency scale helpers.
 *
 * Coordinates: tiles are indexed in native source frames (`startFrame`,
 * `framesPerColumn`) rather than 44.1 kHz project samples, with 96 vertical
 * logarithmic pixels per source channel stacked top-to-bottom.
 *
 * Threading: tile synthesis is pure read-only over immutable decoded `Audio`
 * buffers and supports cooperative cancellation via `cancelled`; intended for
 * background worker threads rather than real-time audio callbacks.
 */
#pragma once
#include "Core/Session.h"
#include <juce_graphics/juce_graphics.h>
#include <functional>

namespace carta {

// =========================================================================
// Frequency Band & Axis Scale
// =========================================================================

/** Display frequency range presets for spectrogram tiles and rulers. */
enum class SpectrumBand { full, bass, voice, treble, custom };

/** FFT analysis window shapes (coherent-gain normalized across types). */
enum class SpectrumWindow { hann, hamming, blackman };

/**
 * Resolve visible `[low, high]` Hz bounds for a display rate and band preset.
 *
 * Presets clip to the display rate's Nyquist; custom bounds remain fixed and
 * frequencies above each source's Nyquist are displayed as blank.
 */
bool spectrogramFrequencyRange(double sampleRate, SpectrumBand, double& low, double& high,
                               int customLow = 20, int customHigh = 22050) noexcept;

/** Tick position along a spectrogram channel lane (`position`: 0 at top, 1 at bottom). */
struct SpectrumFrequencyTick {
    double frequency = 0;
    double position = 0;
};

/** Log-positioned, nonoverlapping marks for the available lane height. */
std::vector<SpectrumFrequencyTick> spectrogramFrequencyTicks(double low, double high, int height);

/** Format Hz values concisely for axis labels (e.g., `440`, `12.5k`). */
juce::String formatSpectrumFrequency(double hz);

/**
 * Use one frequency scale for all clips in a track, even at different rates.
 *
 * Returns the maximum finite positive sample rate among valid clips, or 0 if none.
 */
double spectrogramTrackRate(const Track&) noexcept;

// =========================================================================
// Source & Clip Spectrogram Tile Synthesis
// =========================================================================

/**
 * Synthesize a source-aligned grayscale RGB tile of 96 px per channel.
 *
 * Source-aligned tile, with each column covering `framesPerColumn` source
 * frames. Wide columns aggregate all overlapping FFT windows. Returns an
 * invalid image on cancellation or invalid input.
 */
juce::Image makeSpectrogramTile(const Audio&, juce::int64 startFrame, juce::int64 framesPerColumn,
                                int columns, const std::function<bool()>& cancelled = {},
                                int fftOrder = 11, SpectrumBand band = SpectrumBand::full,
                                int customLow = 20, int customHigh = 22050,
                                SpectrumWindow window = SpectrumWindow::hann, int floorDb = -80);

/**
 * Analyze the clip's source-aligned samples with clip gain and fades applied;
 * source samples outside its trim bounds are silent. Track mixing is excluded.
 */
juce::Image makeClipSpectrogramTile(const Clip&, juce::int64 startFrame,
                                    juce::int64 framesPerColumn, int columns,
                                    const std::function<bool()>& cancelled = {}, int fftOrder = 11,
                                    SpectrumBand band = SpectrumBand::full, double displayRate = 0,
                                    int customLow = 20, int customHigh = 22050,
                                    SpectrumWindow window = SpectrumWindow::hann,
                                    int floorDb = -80);

} // namespace carta
