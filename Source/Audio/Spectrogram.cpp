/**
 * Off-thread spectrogram tile synthesis and frequency axis formatting.
 *
 * Uses 75%-overlapping STFT frames (`hop = fftSize / 4`), coherent-gain window
 * compensation across Hann/Hamming/Blackman windows, and peak bin max-hold
 * across each column and logarithmic pixel row.
 */
#include "Audio/Spectrogram.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace carta {

double spectrogramTrackRate(const Track& track) noexcept {
    double rate = 0;
    for (const auto& clip : track.clips) {
        if (clip.audio && std::isfinite(clip.audio->sampleRate) && clip.audio->sampleRate > rate) {
            rate = clip.audio->sampleRate;
        }
    }
    return rate;
}

bool spectrogramFrequencyRange(double sampleRate, SpectrumBand band, double& low, double& high,
                               int customLow, int customHigh) noexcept {
    if (!std::isfinite(sampleRate) || sampleRate <= 0) {
        return false;
    }

    const double nyquist = sampleRate * 0.5;
    double minimum = 20;
    double maximum = nyquist;

    switch (band) {
    case SpectrumBand::full:
        break;
    case SpectrumBand::bass:
        maximum = std::min(1000.0, nyquist);
        break;
    case SpectrumBand::voice:
        minimum = 100;
        maximum = std::min(8000.0, nyquist);
        break;
    case SpectrumBand::treble:
        minimum = 1000;
        break;
    case SpectrumBand::custom:
        if (customLow < 1 || customHigh <= customLow || customHigh > 192000) {
            return false;
        }
        low = customLow;
        high = customHigh;
        return true;
    default:
        return false;
    }

    const double lower = std::min(minimum, maximum * 0.5);
    if (!(lower > 0) || !(maximum > lower)) {
        return false;
    }

    low = lower;
    high = maximum;
    return true;
}

std::vector<SpectrumFrequencyTick> spectrogramFrequencyTicks(double low, double high, int height) {
    std::vector<SpectrumFrequencyTick> result;
    if (!std::isfinite(low) || !std::isfinite(high) || low <= 0 || high <= low || height <= 0) {
        return result;
    }

    const double logLow = std::log(low);
    const double logSpan = std::log(high) - logLow;
    if (!std::isfinite(logSpan) || logSpan <= 0) {
        return result;
    }

    if (height <= 12) {
        return {{high, 0}};
    }

    std::vector<double> candidates;

    // Logarithmic marks cover wide ranges, while linear marks add useful
    // detail when the user chooses a narrow band between round decades.
    for (int exponent = -9; exponent <= 9; ++exponent) {
        const double decade = std::pow(10.0, exponent);
        for (int multiple = 1; multiple <= 9; ++multiple) {
            const double value = decade * multiple;
            if (value > low && value < high) {
                candidates.push_back(value);
            }
        }
    }

    const double desired = (high - low) / std::max(1, height / 22);
    if (std::isfinite(desired) && desired > 0) {
        const double magnitude = std::pow(10.0, std::floor(std::log10(desired)));
        if (std::isfinite(magnitude) && magnitude > 0) {
            const double ratio = desired / magnitude;
            const double step = (ratio <= 1 ? 1 : ratio <= 2 ? 2 : ratio <= 5 ? 5 : 10) * magnitude;
            const double first = std::ceil(low / step) * step;
            if (std::isfinite(first) && step > 0) {
                for (int i = 0; i < 1024; ++i) {
                    const double value = first + i * step;
                    if (!std::isfinite(value) || value >= high) {
                        break;
                    }
                    if (value > low) {
                        candidates.push_back(value);
                    }
                }
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(), std::greater<double>());
    result.push_back({high, 0});

    const double pixels = static_cast<double>(std::max(0, height - 1));
    const double spacing = 12.0;
    const auto labelTop = [&](double position) {
        return std::clamp(static_cast<int>(std::lround(position * pixels)) - 6, 0, height - 12);
    };

    int previousTop = 0;
    for (double frequency : candidates) {
        const double position = (std::log(high) - std::log(frequency)) / logSpan;
        const int top = labelTop(position);
        if (top - previousTop >= spacing && height - 12 - top >= spacing) {
            result.push_back({frequency, position});
            previousTop = top;
        }
    }

    if (height - 12 - previousTop >= spacing) {
        result.push_back({low, 1});
    }

    return result;
}

juce::String formatSpectrumFrequency(double hz) {
    if (!std::isfinite(hz) || hz <= 0) {
        return {};
    }

    const bool kilo = hz >= 1000;
    const double value = kilo ? hz / 1000.0 : hz;
    if (value < 0.0001) {
        return juce::String::formatted("%.2g", value);
    }

    const int decimals = value >= 100 ? 2 : value >= 1 ? 3 : 5;
    auto text = juce::String(value, decimals).trimCharactersAtEnd("0").trimCharactersAtEnd(".");
    return text + (kilo ? "k" : "");
}

namespace {

juce::Image makeTile(const Audio& audio, const Clip* clip, juce::int64 startFrame,
                     juce::int64 framesPerColumn, int columns,
                     const std::function<bool()>& cancelled, int fftOrder, SpectrumBand band,
                     double displayRate, int customLow, int customHigh, SpectrumWindow windowType,
                     int floorDb) {
    constexpr int height = 96;
    if (fftOrder < 10 || fftOrder > 13 || floorDb < -120 || floorDb > -40) {
        return {};
    }

    using Window = juce::dsp::WindowingFunction<float>;
    Window::WindowingMethod method;
    switch (windowType) {
    case SpectrumWindow::hann:
        method = Window::hann;
        break;
    case SpectrumWindow::hamming:
        method = Window::hamming;
        break;
    case SpectrumWindow::blackman:
        method = Window::blackman;
        break;
    default:
        return {};
    }

    const int fftSize = 1 << fftOrder;
    const int bins = fftSize / 2;
    const int hop = fftSize / 4;
    const int samples = audio.numSamples();
    const int channels = audio.numChannels();

    if (samples <= 0 || channels <= 0 || !std::isfinite(audio.sampleRate) ||
        audio.sampleRate <= 0 || startFrame < 0 || startFrame >= samples || framesPerColumn <= 0 ||
        columns <= 0 || columns > 128 ||
        static_cast<juce::int64>(columns) * height * channels > 2000000 ||
        framesPerColumn > (std::numeric_limits<juce::int64>::max() - startFrame) / columns) {
        return {};
    }

    if (std::equal_to<double>{}(displayRate, 0)) {
        displayRate = audio.sampleRate;
    }

    double low = 0;
    double high = 0;
    if (!spectrogramFrequencyRange(displayRate, band, low, high, customLow, customHigh)) {
        return {};
    }

    juce::Image image(juce::Image::RGB, columns, height * channels, true,
                      juce::SoftwareImageType{});
    juce::Image::BitmapData pixels(image, juce::Image::BitmapData::writeOnly);
    juce::dsp::FFT fft(fftOrder);

    std::vector<float> data(static_cast<size_t>(fftSize) * 2);
    std::vector<float> window(static_cast<size_t>(fftSize));
    std::vector<float> spectrum(static_cast<size_t>(bins) + 1);
    std::array<int, height> firstBin{};
    std::array<int, height> lastBin{};

    Window::fillWindowingTables(window.data(), static_cast<size_t>(fftSize), method, false);

    // Match the existing Hann calibration while compensating for each
    // window's coherent gain, so a steady tone keeps the same brightness.
    double windowSum = 0;
    for (float value : window) {
        windowSum += value;
    }
    const float scale = windowType == SpectrumWindow::hann
                            ? 4.0f / fftSize
                            : static_cast<float>(2.0 * (fftSize - 1) / (fftSize * windowSum));

    const auto frequency = [&](double row) {
        return low *
               std::pow(high / low, juce::jlimit(0.0, 1.0, (height - 1 - row) / (height - 1)));
    };
    for (int y = 0; y < height; ++y) {
        const auto index = static_cast<size_t>(y);
        if (frequency(y + 0.5) > audio.sampleRate * 0.5) {
            firstBin[index] = -1;
            lastBin[index] = -1;
            continue;
        }
        firstBin[index] = juce::jlimit(
            0, bins, static_cast<int>(std::floor(frequency(y + 0.5) / audio.sampleRate * fftSize)));
        lastBin[index] = juce::jlimit(
            firstBin[index], bins,
            static_cast<int>(std::ceil(std::min(frequency(y - 0.5), audio.sampleRate * 0.5) /
                                       audio.sampleRate * fftSize)));
    }

    for (int ch = 0; ch < channels; ++ch) {
        const auto* input = audio.channelData(ch);

        for (int x = 0; x < columns; ++x) {
            if (cancelled && cancelled()) {
                return {};
            }

            std::fill(spectrum.begin(), spectrum.end(), 0);
            const auto begin = startFrame + static_cast<juce::int64>(x) * framesPerColumn;
            const auto end = begin + framesPerColumn;

            // Neighbouring 75%-overlapped windows cover column edges, so a
            // short transient cannot fall between the sampled FFT centres.
            juce::int64 centre = framesPerColumn <= hop
                                     ? begin + framesPerColumn / 2
                                     : std::max<juce::int64>(0, (begin / hop) * hop - hop);
            const auto last =
                framesPerColumn <= hop ? centre : std::min<juce::int64>(samples, end) + hop;

            for (; centre <= last; centre += hop) {
                if (cancelled && cancelled()) {
                    return {};
                }

                std::fill(data.begin(), data.end(), 0);
                for (int i = 0; i < fftSize; ++i) {
                    const auto sample = centre + i - fftSize / 2;
                    if (sample < 0 || sample >= samples) {
                        continue;
                    }

                    if (!clip) {
                        data[static_cast<size_t>(i)] =
                            input[sample] * window[static_cast<size_t>(i)];
                        continue;
                    }

                    const double sourceTime = static_cast<double>(sample) / audio.sampleRate;
                    if (sourceTime < clip->offset || sourceTime >= clip->offset + clip->duration) {
                        continue;
                    }

                    const float level = clipLevel(*clip, sourceTime);
                    data[static_cast<size_t>(i)] =
                        input[sample] * level * window[static_cast<size_t>(i)];
                }

                fft.performFrequencyOnlyForwardTransform(data.data(), true);
                for (int bin = 0; bin <= bins; ++bin) {
                    spectrum[static_cast<size_t>(bin)] = std::max(
                        spectrum[static_cast<size_t>(bin)], data[static_cast<size_t>(bin)]);
                }
            }

            for (int y = 0; y < height; ++y) {
                if (firstBin[static_cast<size_t>(y)] < 0) {
                    continue;
                }

                float peak = 0;
                for (int bin = firstBin[static_cast<size_t>(y)];
                     bin <= lastBin[static_cast<size_t>(y)]; ++bin) {
                    peak = std::max(peak, spectrum[static_cast<size_t>(bin)]);
                }

                const float db =
                    juce::Decibels::gainToDecibels(peak * scale, static_cast<float>(floorDb));
                const auto level = static_cast<juce::uint8>(juce::roundToInt(
                    juce::jlimit(0.0f, 1.0f, (db - floorDb) / -static_cast<float>(floorDb)) * 255));
                pixels.setPixelColour(x, ch * height + y, juce::Colour(level, level, level));
            }
        }
    }

    return image;
}

} // namespace

juce::Image makeSpectrogramTile(const Audio& audio, juce::int64 startFrame,
                                juce::int64 framesPerColumn, int columns,
                                const std::function<bool()>& cancelled, int fftOrder,
                                SpectrumBand band, int customLow, int customHigh,
                                SpectrumWindow window, int floorDb) {
    return makeTile(audio, nullptr, startFrame, framesPerColumn, columns, cancelled, fftOrder, band,
                    0, customLow, customHigh, window, floorDb);
}

juce::Image makeClipSpectrogramTile(const Clip& clip, juce::int64 startFrame,
                                    juce::int64 framesPerColumn, int columns,
                                    const std::function<bool()>& cancelled, int fftOrder,
                                    SpectrumBand band, double displayRate, int customLow,
                                    int customHigh, SpectrumWindow window, int floorDb) {
    if (!clip.audio || !std::isfinite(clip.offset) || clip.offset < 0 ||
        !std::isfinite(clip.duration) || clip.duration <= 0 ||
        !std::isfinite(clip.offset + clip.duration) || !std::isfinite(clip.gain) || clip.gain < 0 ||
        !std::isfinite(clip.fadeIn) || clip.fadeIn < 0 || !std::isfinite(clip.fadeOut) ||
        clip.fadeOut < 0 || !std::isfinite(clip.fadeInStart) || !std::isfinite(clip.fadeOutEnd)) {
        return {};
    }

    return makeTile(*clip.audio, &clip, startFrame, framesPerColumn, columns, cancelled, fftOrder,
                    band, displayRate, customLow, customHigh, window, floorDb);
}

} // namespace carta
