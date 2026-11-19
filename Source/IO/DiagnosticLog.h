#pragma once

/**
 * @file DiagnosticLog.h
 * @brief Lightweight, thread-safe diagnostic logger and user-exportable log bundle.
 *
 * Writes timestamped operational events (NetMD helper commands, timings, firmware
 * capabilities, UTOC sector/cluster remapping, errors) to an auto-rotating log file
 * in the OS user logs directory and maintains an in-memory ring buffer so users can
 * export a self-contained diagnostic report from Help -> Export Diagnostic Log.
 */

#include <juce_core/juce_core.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <sstream>
#include <string>

namespace carta {

namespace detail {

constexpr std::size_t kDiagnosticRingCapacity = 2000;
constexpr std::int64_t kDiagnosticMaxFileBytes = 2 * 1024 * 1024; // 2 MiB

struct DiagnosticLogState {
    std::mutex mutex;
    std::deque<std::string> ring;
    bool initialized = false;
    juce::File logFile;
};

inline DiagnosticLogState& diagnosticLogState() {
    static DiagnosticLogState state;
    return state;
}

inline juce::File resolveDiagnosticLogFileLocked() {
#if JUCE_MAC
    const juce::File base =
        juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Library/Logs");
    return base.getChildFile("Carta Studio").getChildFile("carta-studio.log");
#else
    const juce::File base =
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
    return base.getChildFile("Carta Studio").getChildFile("Logs").getChildFile("carta-studio.log");
#endif
}

inline std::string currentIsoTimestamp() {
    const auto now = juce::Time::getCurrentTime();
    return now.formatted("%Y-%m-%d %H:%M:%S").toStdString() + "." +
           juce::String(now.getMilliseconds()).paddedLeft('0', 3).toStdString();
}

inline void appendLineLocked(DiagnosticLogState& state, const std::string& line) {
    state.ring.push_back(line);
    while (state.ring.size() > kDiagnosticRingCapacity) {
        state.ring.pop_front();
    }
    if (state.logFile == juce::File()) {
        return;
    }
    if (state.logFile.existsAsFile() && state.logFile.getSize() > kDiagnosticMaxFileBytes) {
        const juce::File prev = state.logFile.getSiblingFile("carta-studio.prev.log");
        prev.deleteFile();
        state.logFile.moveFileTo(prev);
    }
    state.logFile.appendText(juce::String::fromUTF8(line.c_str()) + "\n", false, false, "\n");
}

inline void ensureDiagnosticLogInitLocked(DiagnosticLogState& state) {
    if (state.initialized) {
        return;
    }
    state.initialized = true;
    state.logFile = resolveDiagnosticLogFileLocked();
    state.logFile.getParentDirectory().createDirectory();
    std::ostringstream banner;
    banner << "[" << currentIsoTimestamp() << "] [system] Carta Studio session started | OS="
           << juce::SystemStats::getOperatingSystemName().toStdString()
           << " | CPU=" << juce::SystemStats::getCpuModel().toStdString()
           << " (" << juce::SystemStats::getNumCpus() << " cores)";
    appendLineLocked(state, banner.str());
}

} // namespace detail

inline juce::File diagnosticLogFile() {
    auto& state = detail::diagnosticLogState();
    std::lock_guard<std::mutex> lock(state.mutex);
    detail::ensureDiagnosticLogInitLocked(state);
    return state.logFile;
}

inline void logDiagnostic(const std::string& category, const std::string& message) {
    auto& state = detail::diagnosticLogState();
    std::lock_guard<std::mutex> lock(state.mutex);
    detail::ensureDiagnosticLogInitLocked(state);
    std::string clean = message;
    for (char& c : clean) {
        if (c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    const std::string line =
        "[" + detail::currentIsoTimestamp() + "] [" + category + "] " + clean;
    detail::appendLineLocked(state, line);
}

inline std::string formatDiagnosticBundle(const std::string& extraContext) {
    auto& state = detail::diagnosticLogState();
    std::lock_guard<std::mutex> lock(state.mutex);
    detail::ensureDiagnosticLogInitLocked(state);

    std::ostringstream out;
    out << "=== Carta Studio Diagnostic Log ===\n";
    out << "Generated: " << detail::currentIsoTimestamp() << "\n";
    out << "OS: " << juce::SystemStats::getOperatingSystemName().toStdString() << "\n";
    out << "CPU: " << juce::SystemStats::getCpuModel().toStdString() << " ("
        << juce::SystemStats::getNumCpus() << " cores, "
        << juce::SystemStats::getMemorySizeInMegabytes() << " MB RAM)\n";
    out << "Log File: " << state.logFile.getFullPathName().toStdString() << "\n";
    if (!extraContext.empty()) {
        out << "\n=== Session & Hardware Context ===\n";
        out << extraContext;
        if (extraContext.back() != '\n') {
            out << "\n";
        }
    }
    out << "\n=== Recent Events (" << state.ring.size() << " lines) ===\n";
    for (const auto& line : state.ring) {
        out << line << "\n";
    }
    return out.str();
}

inline bool exportDiagnosticLogToFile(const juce::File& target, const std::string& extraContext,
                                      std::string& error) {
    error.clear();
    const std::string text = formatDiagnosticBundle(extraContext);
    if (!target.replaceWithText(juce::String::fromUTF8(text.c_str()), false, false, "\n")) {
        error = "Could not write diagnostic log to '" + target.getFullPathName().toStdString() +
                "'.";
        return false;
    }
    logDiagnostic("system", "Exported diagnostic log to " + target.getFileName().toStdString());
    return true;
}

} // namespace carta
