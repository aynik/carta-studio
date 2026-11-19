/**
 * Band-limited sinc interpolation and prepared kernel cache implementation.
 *
 * Coefficient tables contain `phases + 1` normalized polyphase rows of `taps`
 * Blackman-windowed sinc weights. Evaluation in `sample()` and `sampleStereo()`
 * runs without locks or allocations, clamping boundary taps to `[0, sampleCount - 1]`.
 */
#include "Audio/Resampler.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace carta {
namespace {
constexpr double pi = 3.1415926535897932384626433832795;
} // namespace

bool SincResampler::hasFullSupport(double source, double output) noexcept {
    return std::isfinite(source) && std::isfinite(output) && source > 0 && output > 0 &&
           source / output <= maxFullSupportRatio;
}

int SincResampler::supportTaps(double source, double output) noexcept {
    if (!std::isfinite(source) || !std::isfinite(output) || source <= 0 || output <= 0) {
        return 0;
    }

    const double scale = std::clamp((source / output) / (96000.0 / 44100.0), 1.0, 16.0);
    return 2 * static_cast<int>(std::ceil(baseTaps * scale / 2));
}

SincResampler::SincResampler(double source, double output)
    : sourceRate(source), outputRate(output) {
    taps = supportTaps(sourceRate, outputRate);
    if (taps == 0) {
        return;
    }

    // The transition band stays below the lower of the two Nyquist limits.
    const double cutoff = 0.95 * std::min(1.0, outputRate / sourceRate);

    // Keep the transition width comparable for high-rate sources. The cap
    // bounds table size and CPU for unusual device/source ratios.
    coefficients.resize(static_cast<size_t>((phases + 1) * taps));
    for (int phase = 0; phase <= phases; ++phase) {
        const double fraction = static_cast<double>(phase) / phases;
        double sum = 0;

        for (int tap = 0; tap < taps; ++tap) {
            const double distance = static_cast<double>(tap - (taps / 2 - 1)) - fraction;
            const double argument = pi * cutoff * distance;
            const double sinc = std::abs(argument) < 1.0e-14 ? 1.0 : std::sin(argument) / argument;

            // Continuous Blackman window centered at the output position.
            const double u = distance / (taps / 2.0);
            const double window = std::abs(u) >= 1.0
                                      ? 0.0
                                      : 0.42 + 0.5 * std::cos(pi * u) + 0.08 * std::cos(2 * pi * u);
            const double value = cutoff * sinc * window;
            coefficients[static_cast<size_t>(phase * taps + tap)] = static_cast<float>(value);
            sum += value;
        }

        for (int tap = 0; tap < taps; ++tap) {
            coefficients[static_cast<size_t>(phase * taps + tap)] /= static_cast<float>(sum);
        }
    }
}

bool SincResampler::matches(double source, double output) const noexcept {
    return valid() && std::abs(sourceRate - source) < 1.0e-9 &&
           std::abs(outputRate - output) < 1.0e-9;
}

float SincResampler::sample(const float* data, int sampleCount,
                            double sourcePosition) const noexcept {
    if (!valid() || !data || sampleCount <= 0 || !std::isfinite(sourcePosition) ||
        sourcePosition < 0 || sourcePosition >= sampleCount) {
        return 0;
    }

    const int index = static_cast<int>(std::floor(sourcePosition));
    const double fraction = sourcePosition - index;
    const int phase = std::min(phases, static_cast<int>(std::floor(fraction * phases + 0.5)));
    const auto* weights = coefficients.data() + static_cast<size_t>(phase * taps);
    const int start = index - (taps / 2 - 1);

    double value = 0;
    if (start >= 0 && start <= sampleCount - taps) {
        for (int tap = 0; tap < taps; ++tap) {
            value += static_cast<double>(data[start + tap]) * weights[tap];
        }
    } else {
        for (int tap = 0; tap < taps; ++tap) {
            const auto at = std::clamp(static_cast<long long>(start) + tap, 0LL,
                                       static_cast<long long>(sampleCount) - 1);
            value += static_cast<double>(data[at]) * weights[tap];
        }
    }

    return static_cast<float>(value);
}

std::pair<float, float> SincResampler::sampleStereo(const float* left, const float* right,
                                                    int sampleCount,
                                                    double sourcePosition) const noexcept {
    if (!valid() || !left || !right || sampleCount <= 0 || !std::isfinite(sourcePosition) ||
        sourcePosition < 0 || sourcePosition >= sampleCount) {
        return {};
    }

    const int index = static_cast<int>(std::floor(sourcePosition));
    const double fraction = sourcePosition - index;
    const int phase = std::min(phases, static_cast<int>(std::floor(fraction * phases + 0.5)));
    const auto* weights = coefficients.data() + static_cast<size_t>(phase * taps);
    const int start = index - (taps / 2 - 1);

    double l = 0;
    double r = 0;
    if (start >= 0 && start <= sampleCount - taps) {
        for (int tap = 0; tap < taps; ++tap) {
            l += static_cast<double>(left[start + tap]) * weights[tap];
            r += static_cast<double>(right[start + tap]) * weights[tap];
        }
    } else {
        for (int tap = 0; tap < taps; ++tap) {
            const auto at = std::clamp(static_cast<long long>(start) + tap, 0LL,
                                       static_cast<long long>(sampleCount) - 1);
            l += static_cast<double>(left[at]) * weights[tap];
            r += static_cast<double>(right[at]) * weights[tap];
        }
    }

    return {static_cast<float>(l), static_cast<float>(r)};
}

void ResamplingContext::prepare(double sourceRate, double outputRate) {
    if (!std::isfinite(sourceRate) || !std::isfinite(outputRate) || sourceRate <= 0 ||
        outputRate <= 0) {
        return;
    }

    const auto key = std::make_pair(sourceRate, outputRate);
    if (kernels.find(key) == kernels.end()) {
        kernels.emplace(key, std::make_shared<SincResampler>(sourceRate, outputRate));
    }
}

void ResamplingContext::registerSource(double sourceRate, double currentOutputRate) {
    if (!std::isfinite(sourceRate) || sourceRate <= 0) {
        return;
    }

    sourceRates.insert(sourceRate);
    prepare(sourceRate, currentOutputRate);
}

void ResamplingContext::registerPrepared(double sourceRate, double outputRate,
                                         std::shared_ptr<const SincResampler> kernel) {
    if (!std::isfinite(sourceRate) || sourceRate <= 0) {
        return;
    }

    sourceRates.insert(sourceRate);
    if (!std::isfinite(outputRate) || outputRate <= 0) {
        return;
    }

    const auto key = std::make_pair(sourceRate, outputRate);
    if (kernels.find(key) == kernels.end()) {
        if (kernel && kernel->matches(sourceRate, outputRate)) {
            kernels.emplace(key, std::move(kernel));
        } else {
            prepare(sourceRate, outputRate);
        }
    }
}

void ResamplingContext::prepareOutput(double outputRate) {
    for (const auto sourceRate : sourceRates) {
        prepare(sourceRate, outputRate);
    }
}

const SincResampler* ResamplingContext::find(double sourceRate, double outputRate) const noexcept {
    const auto it = kernels.find(std::make_pair(sourceRate, outputRate));
    return it == kernels.end() ? nullptr : it->second.get();
}

} // namespace carta
