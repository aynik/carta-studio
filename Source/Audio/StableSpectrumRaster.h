/**
 * Bounded LRU cache of pre-scaled, chunked spectrogram rasters for UI painting.
 *
 * Coordinates: `chunk()` slices a scaled `(width x height)` destination strip
 * into aligned `chunkSize`-pixel ARGB chunks starting at multiples of `chunkSize`.
 * Each chunk is scaled independently from a fixed origin and has opacity baked
 * in, so painting a clipped subset uses the same pixels as painting the whole tile.
 *
 * Ownership & threading: source `juce::Image`s must remain immutable while
 * cached (the entry retains a reference to `source`). Intended for UI message
 * thread painting only; not synchronized for concurrent access.
 */
#pragma once
#include <juce_graphics/juce_graphics.h>
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <tuple>

// Source images must remain immutable while cached. Each
// chunk is scaled independently from a fixed origin and has opacity baked in,
// so painting a clipped subset uses the same pixels as painting the whole tile.
namespace carta {

/** Bounded LRU cache of horizontally chunked, 75%-opacity spectrogram rasters. */
class StableSpectrumRaster {
  public:
    explicit StableSpectrumRaster(size_t limitBytes) : limit(limitBytes) {}

    static constexpr int chunkSize = 256;

    /**
     * Return a cached (or freshly rasterized) ARGB strip at horizontal pixel
     * `offset` (must be a non-negative multiple of `chunkSize` below `width`).
     * Returns an invalid image on invalid geometry or allocation failure.
     */
    juce::Image chunk(const juce::Image& source, int sourceY, int sourceHeight, int width,
                      int height, int offset) {
        if (!source.isValid() || sourceY < 0 || sourceHeight <= 0 ||
            sourceY > source.getHeight() - sourceHeight || width <= 0 || height <= 0 ||
            height > 8192 || offset < 0 || offset >= width || offset % chunkSize != 0) {
            return {};
        }

        try {
            const Key key{reinterpret_cast<std::uintptr_t>(source.getPixelData().get()),
                          sourceY,
                          sourceHeight,
                          width,
                          height,
                          offset};
            auto found = entries.find(key);
            if (found != entries.end()) {
                found->second.used = ++clock;
                return found->second.raster;
            }

            const int chunkWidth = std::min(chunkSize, width - offset);
            juce::Image raster(juce::Image::ARGB, chunkWidth, height, true,
                               juce::SoftwareImageType());
            {
                juce::Graphics graphics(raster);
                graphics.drawImage(source, -offset, 0, width, height, 0, sourceY, source.getWidth(),
                                   sourceHeight);
            }
            raster.multiplyAllAlphas(0.75f);

            // Accounting intentionally overcounts a shared source once per entry;
            // allocator overhead, stride padding and temporary rasters are excluded.
            const auto cost = static_cast<size_t>(chunkWidth) * static_cast<size_t>(height) * 4u +
                              static_cast<size_t>(source.getWidth()) *
                                  static_cast<size_t>(source.getHeight()) * 4u;
            if (cost > limit) {
                return raster;
            }

            while (usedBytes > limit - cost && !entries.empty()) {
                auto oldest = entries.begin();
                for (auto it = std::next(entries.begin()); it != entries.end(); ++it) {
                    if (it->second.used < oldest->second.used) {
                        oldest = it;
                    }
                }
                usedBytes -= oldest->second.bytes;
                entries.erase(oldest);
            }

            entries.emplace(key, Entry{source, raster, cost, ++clock});
            usedBytes += cost;
            return raster;
        } catch (...) {
            return {};
        }
    }

    size_t bytes() const noexcept { return usedBytes; }
    size_t size() const noexcept { return entries.size(); }

  private:
    using Key = std::tuple<std::uintptr_t, int, int, int, int, int>;

    struct Entry {
        juce::Image source;
        juce::Image raster;
        size_t bytes = 0;
        std::uint64_t used = 0;
    };

    size_t limit = 0;
    size_t usedBytes = 0;
    std::uint64_t clock = 0;
    std::map<Key, Entry> entries;
};

} // namespace carta
