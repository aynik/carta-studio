#include "RenderBenchmark.h"
#include "Core/Session.h"
#include "Audio/Transport.h"
#include "Editing/Editing.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>
#include <vector>

int runRenderBenchmark() {
    using Clock = std::chrono::steady_clock;
    constexpr int rate = 44100, trackCount = 8, clipsPerTrack = 600, clipFrames = 4 * rate,
                  period = 8 * rate;
    constexpr int duration = 80 * 60 * rate, block = 512, iterations = 6000;
    auto audio = std::make_shared<carta::Audio>();
    audio->sampleRate = rate;
    audio->samples.setSize(2, clipFrames);
    for (int i = 0; i < clipFrames; ++i) {
        audio->samples.setSample(0, i, 0.05f);
        audio->samples.setSample(1, i, -0.05f);
    }
    carta::Session session;
    for (int t = 0; t < trackCount; ++t) {
        carta::Track track;
        track.name = "Track " + juce::String(t + 1);
        track.clips.reserve(clipsPerTrack);
        for (int i = 0; i < clipsPerTrack; ++i)
            track.clips.push_back({audio, static_cast<double>(i * period + t * (rate / 4)) / rate,
                                   0, static_cast<double>(clipFrames) / rate});
        session.tracks.push_back(std::move(track));
    }
    carta::ResamplingContext prepared;
    session.prepareResampling(prepared, rate);
    const carta::RenderClipIndex clipIndex(session);
    juce::AudioBuffer<float> output(2, block), indexed(2, block);
    std::vector<double> timings, indexedTimings;
    timings.reserve(iterations);
    indexedTimings.reserve(iterations);
    for (int iteration = 0; iteration < iterations; ++iteration) {
        const auto frame =
            static_cast<int>((static_cast<juce::int64>(iteration) * 15485863) % (duration - block));
        output.clear();
        const auto begin = Clock::now();
        session.render(output, 0, block, static_cast<double>(frame) / rate, rate, 2, &prepared);
        timings.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
        indexed.clear();
        const auto indexBegin = Clock::now();
        session.render(indexed, 0, block, static_cast<double>(frame) / rate, rate, 2, &prepared, {},
                       -1, &clipIndex);
        indexedTimings.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - indexBegin).count());
        for (int channel = 0; channel < 2; ++channel)
            if (std::memcmp(output.getReadPointer(channel), indexed.getReadPointer(channel),
                            block * sizeof(float)) != 0) {
                std::cerr << "Indexed render mismatch at frame " << frame << '\n';
                return 1;
            }
        for (int i = 0; i < block; ++i) {
            float left = 0, right = 0;
            for (int t = 0; t < trackCount; ++t) {
                const int relative = frame + i - t * (rate / 4);
                if (relative >= 0 && relative / period < clipsPerTrack &&
                    relative % period < clipFrames) {
                    left += 0.05f;
                    right -= 0.05f;
                }
            }
            const float actualLeft = output.getSample(0, i), actualRight = output.getSample(1, i);
            if (std::memcmp(&actualLeft, &left, sizeof(float)) != 0 ||
                std::memcmp(&actualRight, &right, sizeof(float)) != 0) {
                std::cerr << "Render benchmark mismatch at frame " << frame + i << '\n';
                return 1;
            }
        }
    }
    const auto sum = std::accumulate(timings.begin(), timings.end(), 0.0);
    std::sort(timings.begin(), timings.end());
    const auto indexedSum = std::accumulate(indexedTimings.begin(), indexedTimings.end(), 0.0);
    std::sort(indexedTimings.begin(), indexedTimings.end());
    const double deadline = 1000.0 * block / rate;
    const auto late = std::count_if(timings.begin(), timings.end(),
                                    [deadline](double value) { return value > deadline; });
    std::cout << "80-minute synthetic stereo arrangement: " << trackCount << " tracks, "
              << trackCount * clipsPerTrack << " clips, " << block << "-frame blocks\n"
              << "Render mean " << sum / iterations << " ms; p95 " << timings[iterations * 95 / 100]
              << " ms; p99 " << timings[iterations * 99 / 100] << " ms; maximum " << timings.back()
              << " ms\n"
              << "Indexed render mean " << indexedSum / iterations << " ms; p95 "
              << indexedTimings[iterations * 95 / 100] << " ms; p99 "
              << indexedTimings[iterations * 99 / 100] << " ms; maximum " << indexedTimings.back()
              << " ms\n"
              << "Nominal block duration " << deadline << " ms; " << late << " of " << iterations
              << " measured renders exceeded it.\n"
              << "Indexed and linear renders matched bit-for-bit and every sample matched the "
                 "independent constant-signal mix. This excludes index construction, device "
                 "scheduling, locks, paging, GUI work and resampling.\n";
    return 0;
}

int runMixedRateRenderBenchmark() {
    using Clock = std::chrono::steady_clock;
    constexpr int rate = 44100, trackCount = 8, clipsPerTrack = 600, clipSeconds = 8;
    constexpr int duration = clipsPerTrack * clipSeconds * rate, block = 512, iterations = 600;
    constexpr int sourceRates[] = {44100, 48000, 96000, 192000};
    std::vector<std::shared_ptr<carta::Audio>> sources;
    for (const int sourceRate : sourceRates) {
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = sourceRate;
        audio->samples.setSize(2, sourceRate * clipSeconds);
        for (int i = 0; i < audio->numSamples(); ++i) {
            audio->samples.setSample(0, i, 0.05f);
            audio->samples.setSample(1, i, -0.05f);
        }
        sources.push_back(std::move(audio));
    }
    carta::Session session;
    for (int t = 0; t < trackCount; ++t) {
        carta::Track track;
        track.name = "Track " + juce::String(t + 1);
        track.clips.reserve(clipsPerTrack);
        for (int i = 0; i < clipsPerTrack; ++i)
            track.clips.push_back({sources[static_cast<size_t>(t % 4)],
                                   static_cast<double>(i * clipSeconds), 0, clipSeconds});
        session.tracks.push_back(std::move(track));
    }
    carta::ResamplingContext prepared;
    session.prepareResampling(prepared, rate);
    juce::AudioBuffer<float> output(2, block);
    std::vector<double> timings;
    timings.reserve(iterations);
    float maximumError = 0;
    for (int iteration = 0; iteration < iterations; ++iteration) {
        // Some blocks cross clip boundaries; others sample the full arrangement.
        const auto frame =
            iteration % 10 == 0
                ? (iteration / 10) * clipSeconds * rate + clipSeconds * rate - block / 2
                : static_cast<int>((static_cast<juce::int64>(iteration) * 15485863) %
                                   (duration - block));
        output.clear();
        double position = static_cast<double>(frame) / rate;
        const auto begin = Clock::now();
        const bool playing = carta::renderPlayback(session, output, 0, block, position, rate, 2,
                                                   {0, session.length(), false}, &prepared);
        timings.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
        if (!playing) {
            std::cerr << "Unexpected end in mixed-rate render benchmark.\n";
            return 1;
        }
        for (int i = 0; i < block; ++i) {
            const auto error = std::max(std::abs(output.getSample(0, i) - 0.4f),
                                        std::abs(output.getSample(1, i) + 0.4f));
            maximumError = std::max(maximumError, error);
            if (error > 2.0e-5f) {
                std::cerr << "Mixed-rate render mismatch at frame " << frame + i << ": error "
                          << error << '\n';
                return 1;
            }
        }
    }
    const auto sum = std::accumulate(timings.begin(), timings.end(), 0.0);
    std::sort(timings.begin(), timings.end());
    const double deadline = 1000.0 * block / rate;
    const auto late = std::count_if(timings.begin(), timings.end(),
                                    [deadline](double value) { return value > deadline; });
    std::cout << "80-minute stereo mix: 8 continuously active tracks, 4800 clips, 44.1/48/96/192 "
                 "kHz sources, 44.1 kHz output\n"
              << "Playback render mean " << sum / iterations << " ms; p95 "
              << timings[iterations * 95 / 100] << " ms; p99 " << timings[iterations * 99 / 100]
              << " ms; maximum " << timings.back() << " ms\n"
              << "Nominal " << block << "-frame block duration " << deadline << " ms; " << late
              << " of " << iterations << " renders exceeded it.\n"
              << "Maximum constant-mix error " << maximumError
              << ". Includes resampling and project-end scanning; excludes device scheduling, "
                 "locks, paging and GUI activity.\n";
    return 0;
}

int runGroupDragBenchmark() {
    using Clock = std::chrono::steady_clock;
    constexpr int tracks = 8, clips = 600, iterations = 1000;
    auto audio = std::make_shared<carta::Audio>();
    audio->sampleRate = carta::sessionRate;
    audio->samples.setSize(1, 1024);
    audio->samples.clear();
    carta::Session session;
    std::vector<carta::ClipReference> selected;
    for (int t = 0; t < tracks; ++t) {
        carta::Track track;
        track.name = "Track " + juce::String(t + 1);
        track.clips.reserve(clips);
        for (int c = 0; c < clips; ++c) {
            track.clips.push_back(
                {audio, static_cast<double>(c + 1), 0, 1024 / carta::sessionRate});
            if ((c % 2) == 0)
                selected.push_back({t, c});
        }
        session.tracks.push_back(std::move(track));
    }
    carta::ClipGroupMovePreview preview;
    juce::String error;
    if (!preview.prepare(session, selected, error)) {
        std::cerr << "Could not prepare drag benchmark: " << error << '\n';
        return 1;
    }
    const auto targets = carta::clipGroupSnapTargets(session, {}, 0, selected);
    std::vector<double> fullTimes, preparedTimes;
    fullTimes.reserve(iterations);
    preparedTimes.reserve(iterations);
    for (int i = 0; i < iterations; ++i) {
        const auto delta = static_cast<juce::int64>((i % 201) - 100);
        juce::String fullError, preparedError;
        auto start = Clock::now();
        const bool full = carta::validateClipGroupMove(session, selected, delta, 0, fullError);
        fullTimes.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        start = Clock::now();
        const bool fast = preview.validate(delta, 0, preparedError);
        preparedTimes.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        if (full != fast || fullError != preparedError) {
            std::cerr << "Group preview validation mismatch.\n";
            return 1;
        }
    }
    const auto report = [&](const char* label, std::vector<double>& values) {
        const auto mean = std::accumulate(values.begin(), values.end(), 0.0) / iterations;
        std::sort(values.begin(), values.end());
        std::cout << label << ": mean " << mean << " ms, p99 " << values[iterations * 99 / 100]
                  << " ms\n";
    };
    std::cout << "Group drag preflight: " << selected.size() << " selected clips, "
              << targets.size() << " sorted snap targets, " << iterations << " offsets\n";
    report("Rebuilding validation", fullTimes);
    report("Prepared preview", preparedTimes);
    std::cout << "Validation results matched. Excludes snap search, device scheduling, locks, GUI "
                 "drawing and paging.\n";
    carta::ClipGroupEdgePreview edgePlan;
    if (!edgePlan.prepare(session, selected, error)) {
        std::cerr << "Could not prepare edge benchmark: " << error << '\n';
        return 1;
    }
    std::vector<double> fullEdges, preparedEdges;
    std::vector<carta::Clip> fullResult, preparedResult;
    for (int i = 0; i < iterations; ++i) {
        const auto delta = static_cast<juce::int64>((i % 100) + 1);
        juce::String fullError, preparedError;
        auto start = Clock::now();
        const bool full =
            carta::previewClipGroupEdges(session, selected, false, delta, fullResult, fullError);
        fullEdges.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        start = Clock::now();
        const bool fast = edgePlan.calculate(false, delta, preparedResult, preparedError);
        preparedEdges.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        if (!full || !fast || fullResult.size() != preparedResult.size()) {
            std::cerr << "Group edge preview failed.\n";
            return 1;
        }
        for (size_t n = 0; n < fullResult.size(); ++n)
            if (std::memcmp(&fullResult[n].start, &preparedResult[n].start, sizeof(double)) ||
                std::memcmp(&fullResult[n].offset, &preparedResult[n].offset, sizeof(double)) ||
                std::memcmp(&fullResult[n].duration, &preparedResult[n].duration, sizeof(double))) {
                std::cerr << "Group edge preview geometry mismatch.\n";
                return 1;
            }
    }
    std::cout << "Group edge preview: " << selected.size() << " selected clips, " << iterations
              << " offsets\n";
    report("Rebuilding edge preview", fullEdges);
    report("Prepared edge preview", preparedEdges);
    std::cout << "Geometry matched. Excludes marker checks, snapping, device scheduling, locks, "
                 "drawing and paging.\n";
    auto& moved = session.tracks.back().clips.back();
    const double originalStart = moved.start;
    double fixedEnd = 0;
    for (size_t t = 0; t < session.tracks.size(); ++t)
        for (size_t c = 0; c < session.tracks[t].clips.size(); ++c)
            if (t != session.tracks.size() - 1 || c != session.tracks[t].clips.size() - 1)
                fixedEnd = std::max(fixedEnd, session.tracks[t].clips[c].start +
                                                  session.tracks[t].clips[c].duration);
    std::vector<double> fullEnds, preparedEnds;
    for (int i = 0; i < iterations; ++i) {
        moved.start = originalStart + static_cast<double>((i % 101) - 50) / carta::sessionRate;
        auto start = Clock::now();
        const auto full = carta::projectEndSample(session);
        fullEnds.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        start = Clock::now();
        const auto fast = carta::projectEndSampleForAudioEnd(
            std::max(fixedEnd, moved.start + moved.duration), session.minimumEndSample);
        preparedEnds.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        if (full != fast) {
            std::cerr << "Clip-drag master end mismatch.\n";
            return 1;
        }
    }
    std::cout << "Single-clip master-end preview: " << tracks * clips << " total clips, "
              << iterations << " offsets\n";
    report("Full arrangement scan", fullEnds);
    report("Prepared fixed end", preparedEnds);
    std::cout << "Master ends matched. Excludes one-time preparation, snapping, locks, drawing, "
                 "device scheduling and paging.\n";
    std::vector<juce::Range<int>> titleClips;
    titleClips.reserve(4800);
    for (int i = 0; i < 4800; ++i) {
        const int start = i % 2000;
        titleClips.emplace_back(start, start + 2000);
    }
    const auto oldTitleLayout = [](const std::vector<juce::Range<int>>& input) {
        std::vector<int> edges;
        for (const auto& clip : input) {
            edges.push_back(clip.getStart());
            edges.push_back(clip.getEnd());
        }
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        std::vector<carta::ClipTitleRegion> output;
        for (size_t e = 1; e < edges.size(); ++e) {
            const int start = edges[e - 1], end = edges[e];
            std::vector<int> active;
            for (size_t i = 0; i < input.size(); ++i)
                if (input[i].getStart() < end && input[i].getEnd() > start)
                    active.push_back(static_cast<int>(i));
            for (size_t i = 0; i < active.size(); ++i) {
                const int left = start + static_cast<int>((static_cast<juce::int64>(end - start) *
                                                           static_cast<juce::int64>(i)) /
                                                          static_cast<juce::int64>(active.size()));
                const int right = start + static_cast<int>((static_cast<juce::int64>(end - start) *
                                                            static_cast<juce::int64>(i + 1)) /
                                                           static_cast<juce::int64>(active.size()));
                if (left == right)
                    continue;
                if (!output.empty() && output.back().clip == active[i] &&
                    output.back().bounds.getEnd() == left)
                    output.back().bounds = output.back().bounds.withEnd(right);
                else
                    output.push_back({active[i], {left, right}});
            }
        }
        return output;
    };
    const auto oldStart = Clock::now();
    const auto originalTitles = oldTitleLayout(titleClips);
    const double oldTime =
        std::chrono::duration<double, std::milli>(Clock::now() - oldStart).count();
    std::vector<double> titleTimes;
    for (int i = 0; i < 30; ++i) {
        const auto start = Clock::now();
        const auto current = carta::layoutClipTitles(titleClips, {0, 4000});
        titleTimes.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        if (current.size() != originalTitles.size()) {
            std::cerr << "Title layout count mismatch.\n";
            return 1;
        }
        for (size_t j = 0; j < current.size(); ++j)
            if (current[j].clip != originalTitles[j].clip ||
                current[j].bounds != originalTitles[j].bounds) {
                std::cerr << "Title layout mismatch.\n";
                return 1;
            }
    }
    std::sort(titleTimes.begin(), titleTimes.end());
    std::cout
        << "Dense title layout: 4800 overlapping clips, 4000-pixel viewport; previous scan "
        << oldTime << " ms, sweep median " << titleTimes[titleTimes.size() / 2] << " ms.\n"
        << "Every title region matched; excludes drawing, hit testing and device scheduling.\n";
    carta::Track overlapTrack;
    overlapTrack.clips.reserve(4800);
    for (int i = 0; i < 4800; ++i)
        overlapTrack.clips.push_back({audio, static_cast<double>(i % 2000), 0, 2000});
    std::vector<int> pairwise;
    pairwise.reserve(overlapTrack.clips.size());
    const auto pairwiseStart = Clock::now();
    for (int i = 0; i < 4800; ++i)
        pairwise.push_back(carta::overlappingClipCount(overlapTrack, i));
    const double pairwiseTime =
        std::chrono::duration<double, std::milli>(Clock::now() - pairwiseStart).count();
    std::vector<double> batchTimes;
    for (int i = 0; i < 30; ++i) {
        const auto begin = Clock::now();
        const auto counts = carta::overlappingClipCounts(overlapTrack);
        batchTimes.push_back(
            std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
        if (counts != pairwise) {
            std::cerr << "Batch overlap-count mismatch.\n";
            return 1;
        }
    }
    std::sort(batchTimes.begin(), batchTimes.end());
    std::cout
        << "Clip overlap counts: 4800 clips, pairwise " << pairwiseTime << " ms, batch median "
        << batchTimes[batchTimes.size() / 2] << " ms.\n"
        << "All counts matched; excludes browser construction, drawing and device scheduling.\n";
    return 0;
}
