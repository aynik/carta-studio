#include "SoftwarePaintProbe.h"
#include "Audio/StableSpectrumRaster.h"
#include <juce_graphics/juce_graphics.h>
#include <algorithm>
#include <iostream>
#include <vector>

bool verifySoftwareRasterPrototype(bool report) {
    juce::Image source(juce::Image::ARGB, 128, 192, true, juce::SoftwareImageType());
    for (int y = 0; y < source.getHeight(); ++y)
        for (int x = 0; x < source.getWidth(); ++x)
            source.setPixelAt(
                x, y,
                juce::Colour::fromRGB(static_cast<juce::uint8>((x * 11 + y * 7) % 256),
                                      static_cast<juce::uint8>((x * 3 + y * 17) % 256),
                                      static_cast<juce::uint8>((x * 19 + y * 5) % 256)));
    constexpr size_t budget = 2u * 1024u * 1024u;
    carta::StableSpectrumRaster cache(budget);
    int differences = 0, cases = 0, appearanceDifferences = 0, maxChannelDifference = 0;
    const auto started = juce::Time::getMillisecondCounterHiRes();
    for (int width : {50, 257, 1024, 4097, 65536})
        for (int height : {23, 123})
            for (int sourceY : {0, 96})
                for (int origin : {37, -300}) {
                    const auto paint = [&](juce::Rectangle<int> dirty) {
                        juce::Image output(juce::Image::ARGB, 800, 180, true,
                                           juce::SoftwareImageType());
                        juce::Graphics graphics(output);
                        graphics.fillAll(juce::Colour(0xff333333));
                        graphics.reduceClipRegion(dirty);
                        const auto clip = graphics.getClipBounds();
                        for (int offset = 0; offset < width;) {
                            const int chunkWidth =
                                std::min(carta::StableSpectrumRaster::chunkSize, width - offset);
                            const auto x = origin + offset;
                            if (juce::Rectangle<int>(x, 20, chunkWidth, height).intersects(clip)) {
                                const auto image =
                                    cache.chunk(source, sourceY, 96, width, height, offset);
                                if (!image.isValid())
                                    return juce::Image{};
                                graphics.drawImageAt(image, x, 20);
                            }
                            offset += chunkWidth;
                        }
                        return output;
                    };
                    const auto full = paint({0, 0, 800, 180});
                    if (!full.isValid())
                        return false;
                    for (int x0 : {0, 211, 212, 217, 292, 293, 500}) {
                        const juce::Rectangle<int> dirty(x0, 0, 18, 180);
                        const auto partial = paint(dirty);
                        if (!partial.isValid() || cache.bytes() > budget)
                            return false;
                        for (int y = 0; y < 180; ++y)
                            for (int x = dirty.getX(); x < dirty.getRight(); ++x)
                                if (full.getPixelAt(x, y) != partial.getPixelAt(x, y))
                                    ++differences;
                        ++cases;
                    }
                    if (report && width <= 4097) {
                        juce::Image canonical(juce::Image::ARGB, width, height, true,
                                              juce::SoftwareImageType());
                        {
                            juce::Graphics graphics(canonical);
                            graphics.drawImage(source, 0, 0, width, height, 0, sourceY,
                                               source.getWidth(), 96);
                        }
                        canonical.multiplyAllAlphas(0.75f);
                        juce::Image reference(juce::Image::ARGB, 800, 180, true,
                                              juce::SoftwareImageType());
                        {
                            juce::Graphics graphics(reference);
                            graphics.fillAll(juce::Colour(0xff333333));
                            graphics.drawImageAt(canonical, origin, 20);
                        }
                        for (int y = 0; y < 180; ++y)
                            for (int x = 0; x < 800; ++x) {
                                const auto a = full.getPixelAt(x, y),
                                           b = reference.getPixelAt(x, y);
                                if (a != b) {
                                    ++appearanceDifferences;
                                    maxChannelDifference =
                                        std::max({maxChannelDifference,
                                                  std::abs(static_cast<int>(a.getRed()) -
                                                           static_cast<int>(b.getRed())),
                                                  std::abs(static_cast<int>(a.getGreen()) -
                                                           static_cast<int>(b.getGreen())),
                                                  std::abs(static_cast<int>(a.getBlue()) -
                                                           static_cast<int>(b.getBlue()))});
                                }
                            }
                    }
                }
    carta::StableSpectrumRaster tiny(1);
    const auto uncached = tiny.chunk(source, 0, 96, 257, 123, 0);
    if (!uncached.isValid() || tiny.bytes() != 0 || tiny.size() != 0 ||
        cache.chunk(source, 0, 96, 257, 123, 1).isValid())
        return false;
    if (report)
        std::cout
            << "Chunked software raster: " << cases << " cases, " << differences
            << " partial-paint differences, " << appearanceDifferences
            << " appearance differences versus unchunked baked-alpha scaling (max channel delta "
            << maxChannelDifference << "), " << cache.bytes() << " accounted cache bytes in "
            << cache.size() << " entries, " << (juce::Time::getMillisecondCounterHiRes() - started)
            << " ms.\n";
    return differences == 0;
}

int runSoftwareRasterBenchmark() {
    std::vector<juce::Image> sources;
    for (int tile = 0; tile < 7; ++tile) {
        juce::Image source(juce::Image::ARGB, 128, 192, true, juce::SoftwareImageType());
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 128; ++x)
                source.setPixelAt(x, y,
                                  juce::Colour::fromRGB(
                                      static_cast<juce::uint8>((x * 11 + y * 7 + tile * 13) % 256),
                                      static_cast<juce::uint8>((x * 3 + y * 17 + tile * 5) % 256),
                                      static_cast<juce::uint8>((x * 19 + y * 5 + tile * 7) % 256)));
        sources.push_back(source);
    }
    carta::StableSpectrumRaster cache(4u * 1024u * 1024u);
    const auto render = [&](bool stable, int dirtyWidth) {
        juce::Image canvas(juce::Image::ARGB, 1200, 700, true, juce::SoftwareImageType());
        juce::Graphics graphics(canvas);
        graphics.fillAll(juce::Colour(0xff333333));
        if (dirtyWidth > 0)
            graphics.reduceClipRegion(juce::Rectangle<int>(600, 0, dirtyWidth, 700));
        for (int row = 0; row < 4; ++row)
            for (int clip = 0; clip < 8; ++clip)
                for (int tile = 0; tile < 7; ++tile)
                    for (int channel = 0; channel < 2; ++channel) {
                        const int x = 150 + tile * 150, y = 40 + row * 150 + channel * 65;
                        if (stable) {
                            if (!graphics.clipRegionIntersects({x, y, 150, 60}))
                                continue;
                            const auto raster = cache.chunk(sources[static_cast<size_t>(tile)],
                                                            channel * 96, 96, 150, 60, 0);
                            if (!raster.isValid())
                                return juce::Image{};
                            graphics.setOpacity(1.0f);
                            graphics.drawImageAt(raster, x, y);
                        } else {
                            graphics.setOpacity(0.75f);
                            graphics.drawImage(sources[static_cast<size_t>(tile)], x, y, 150, 60, 0,
                                               channel * 96, 128, 96);
                        }
                    }
        return canvas;
    };
    const auto direct = render(false, 0), stable = render(true, 0);
    if (!direct.isValid() || !stable.isValid())
        return 1;
    int changed = 0, maxDelta = 0;
    for (int y = 0; y < 700; ++y)
        for (int x = 0; x < 1200; ++x) {
            const auto a = direct.getPixelAt(x, y), b = stable.getPixelAt(x, y);
            if (a != b) {
                ++changed;
                maxDelta = std::max(
                    {maxDelta,
                     std::abs(static_cast<int>(a.getRed()) - static_cast<int>(b.getRed())),
                     std::abs(static_cast<int>(a.getGreen()) - static_cast<int>(b.getGreen())),
                     std::abs(static_cast<int>(a.getBlue()) - static_cast<int>(b.getBlue()))});
            }
        }
    const auto stablePartial = render(true, 12);
    if (!stablePartial.isValid())
        return 1;
    int clippedDifferences = 0;
    for (int y = 0; y < 700; ++y)
        for (int x = 600; x < 612; ++x)
            if (stable.getPixelAt(x, y) != stablePartial.getPixelAt(x, y))
                ++clippedDifferences;
    for (int dirty : {0, 12})
        for (bool candidate : {false, true}) {
            std::vector<double> timings;
            timings.reserve(30);
            for (int i = 0; i < 30; ++i) {
                const auto start = juce::Time::getMillisecondCounterHiRes();
                const auto image = render(candidate, dirty);
                if (!image.isValid())
                    return 1;
                timings.push_back(juce::Time::getMillisecondCounterHiRes() - start);
            }
            std::sort(timings.begin(), timings.end());
            std::cout << (candidate ? "chunked" : "direct") << ", dirty width " << dirty
                      << ": median " << timings[15] << " ms, max " << timings.back() << " ms.\n";
        }
    std::cout << "Overlap fixture: " << clippedDifferences << " clipped-paint differences; "
              << changed << " pixels differ from direct rendering (max channel delta " << maxDelta
              << "); " << cache.bytes() << " accounted cache bytes in " << cache.size()
              << " entries.\n";
    return clippedDifferences == 0 ? 0 : 1;
}

int runSoftwarePaintProbe() {
    juce::Image source(juce::Image::ARGB, 128, 96, true, juce::SoftwareImageType());
    for (int y = 0; y < 96; ++y)
        for (int x = 0; x < 128; ++x)
            source.setPixelAt(
                x, y,
                juce::Colour::fromRGB(static_cast<juce::uint8>((x * 11 + y * 7) % 256),
                                      static_cast<juce::uint8>((x * 3 + y * 17) % 256),
                                      static_cast<juce::uint8>((x * 19 + y * 5) % 256)));
    const auto paint = [&](bool stable, bool partial, bool rect = false, bool bakedAlpha = false) {
        juce::Image image(juce::Image::ARGB, 800, 300, true, juce::SoftwareImageType());
        juce::Graphics graphics(image);
        graphics.fillAll(juce::Colour(0xff333333));
        if (partial)
            graphics.reduceClipRegion(juce::Rectangle<int>(217, 0, 18, 300));
        graphics.setOpacity(bakedAlpha ? 1.0f : 0.75f);
        if (rect) {
            graphics.setColour(juce::Colour(0xffabcdef).withAlpha(0.75f));
            graphics.fillRect(150, 100, 213, 123);
        } else if (stable) {
            juce::Image scaled(juce::Image::ARGB, 213, 123, true, juce::SoftwareImageType());
            {
                juce::Graphics g(scaled);
                g.drawImage(source, 0, 0, 213, 123, 0, 0, 128, 96);
            }
            if (bakedAlpha)
                scaled.multiplyAllAlphas(0.75f);
            graphics.drawImageAt(scaled, 150, 100);
        } else {
            auto input = source.createCopy();
            if (bakedAlpha)
                input.multiplyAllAlphas(0.75f);
            graphics.drawImage(input, 150, 100, 213, 123, 0, 0, 128, 96);
        }
        return image;
    };
    const auto compare = [&](juce::Image a, juce::Image b) {
        int n = 0;
        for (int y = 0; y < 300; ++y)
            for (int x = 217; x < 235; ++x)
                if (a.getPixelAt(x, y) != b.getPixelAt(x, y))
                    ++n;
        return n;
    };
    const auto full = paint(false, false), partial = paint(false, true), fixed = paint(true, false),
               fixedPartial = paint(true, true);
    std::cout << "direct partial differences " << compare(full, partial) << "\n";
    std::cout << "prescaled partial differences " << compare(fixed, fixedPartial) << "\n";
    std::cout << "full-image appearance differences in region " << compare(full, fixed) << "\n";
    std::cout << "solid partial differences "
              << compare(paint(false, false, true), paint(false, true, true)) << "\n";
    std::cout << "baked-alpha partial differences "
              << compare(paint(true, false, false, true), paint(true, true, false, true)) << "\n";
    std::cout << "scaled baked-alpha partial differences "
              << compare(paint(false, false, false, true), paint(false, true, false, true)) << "\n";
    return verifySoftwareRasterPrototype(true) ? 0 : 1;
}
