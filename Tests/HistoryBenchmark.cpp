#include "HistoryBenchmark.h"
#include "Editing/Editing.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <vector>

int runHistoryBenchmark() {
    using Clock = std::chrono::steady_clock;
    constexpr int tracks = 8, clips = 600, snapshots = 40, passes = 200;
    carta::EditorState state;
    std::vector<std::shared_ptr<carta::Audio>> sources;
    for (int t = 0; t < tracks; ++t) {
        auto audio = std::make_shared<carta::Audio>();
        audio->sampleRate = carta::sessionRate;
        audio->samples.setSize(2, 1024);
        audio->samples.clear();
        sources.push_back(audio);
        carta::Track track;
        track.name = "Track " + juce::String(t + 1);
        track.clips.reserve(clips);
        for (int i = 0; i < clips; ++i)
            track.clips.push_back({audio, static_cast<double>(i), 0, 1024 / carta::sessionRate});
        state.session.tracks.push_back(std::move(track));
    }
    carta::EditHistory history;
    for (int i = 0; i < snapshots; ++i)
        history.record(state);
    carta::Session empty;
    carta::Clipboard clipboard;
    std::cout << "40 history snapshots, 4,800 clips each, 8 distinct sources\n";
    for (bool hasCurrent : {false, true}) {
        const auto& current = hasCurrent ? state.session : empty;
        std::vector<double> timings;
        timings.reserve(passes);
        size_t total = 0;
        for (int i = 0; i < passes; ++i) {
            const auto start = Clock::now();
            const auto memory = history.estimatedRetainedMediaBytes(current, clipboard);
            const auto mapped = history.estimatedRetainedMappedBytes(current, clipboard);
            timings.push_back(
                std::chrono::duration<double, std::milli>(Clock::now() - start).count());
            total = memory + static_cast<size_t>(mapped);
        }
        const size_t expected =
            hasCurrent ? 0 : static_cast<size_t>(tracks) * 2 * 1024 * sizeof(float);
        if (total != expected) {
            std::cerr << "History media estimate mismatch.\n";
            return 1;
        }
        std::sort(timings.begin(), timings.end());
        std::cout << (hasCurrent ? "All sources current" : "Sources only in history")
                  << ": retained payload " << total << " bytes; mean "
                  << std::accumulate(timings.begin(), timings.end(), 0.0) / passes << " ms, p99 "
                  << timings[passes * 99 / 100] << " ms (" << passes << " passes).\n";
    }
    std::cout << "Synthetic test excludes snapshot capture, playback, device scheduling, GUI and "
                 "paging.\n";
    return 0;
}
