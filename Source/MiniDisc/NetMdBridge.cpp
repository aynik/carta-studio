/**
 * @file NetMdBridge.cpp
 * High-level NetMD orchestration over the out-of-process `carta-netmd` CLI helper.
 *
 * Hardware, License, & Protocol Invariants:
 * - Process isolation: All USB / NetMD / Type-S protocol operations execute in
 *   the standalone GPL-2.0 `carta-netmd` binary via POSIX pipes and NDJSON
 *   stdout events, keeping `Carta Studio` completely free of GPL-2.0 or libusb
 *   code.
 * - Serialization: Helper invocations serialize on `netMdBridgeMutex()`, while
 *   CPU-heavy ATRAC encoding runs beforehand so encode errors never erase a disc.
 * - SP alignment: `carta1` appends a 1-frame flush tail (512 samples), so SP
 *   inputs pad to `(pcmFrames + 1) % 11 == 0`, producing exact 2,332-byte MD
 *   sectors (11 sound groups * 212 bytes).
 * - Cluster `50.0.0` preservation: Sony hardware locates uploaded audio near
 *   cluster 50 instead of logical cluster 1. `writeNetMdUtoc` preserves
 *   physical Sector 0 cluster ranges and `P-FRA` via cached span mappings or
 *   matching on-disc topology, and passes sector mask `0x17` (sectors 0, 1, 2,
 *   4) so device Sector 3 remains untouched.
 */

#include "MiniDisc/NetMdBridge.h"

#include "IO/DiagnosticLog.h"
#include "MiniDisc/NativeCodecs.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace carta {

// ============================================================================
// Thread-Safe Progress State
// ============================================================================

namespace {

std::mutex g_progressMutex;
NetMdProgressSnapshot g_progress{};

std::mutex g_helperConfigMutex;
std::string g_helperPathOverride;
std::atomic<bool> g_mockGuardEnabled{false};

} // namespace

void setNetMdProgress(const std::string& operation, const std::string& phase, double progress,
                      std::int64_t bytesDone, std::int64_t bytesTotal) {
    const std::lock_guard<std::mutex> lk(g_progressMutex);
    g_progress.active = true;
    g_progress.operation = operation;
    g_progress.phase = phase;
    g_progress.progress = std::clamp(progress, 0.0, 1.0);
    g_progress.bytesTotal = std::max<std::int64_t>(0, bytesTotal);
    g_progress.bytesDone = g_progress.bytesTotal > 0
                               ? std::clamp<std::int64_t>(bytesDone, 0, g_progress.bytesTotal)
                               : std::max<std::int64_t>(0, bytesDone);
}

void clearNetMdProgress() {
    const std::lock_guard<std::mutex> lk(g_progressMutex);
    g_progress = {};
}

NetMdProgressSnapshot getNetMdProgress() {
    const std::lock_guard<std::mutex> lk(g_progressMutex);
    return g_progress;
}

bool isNetMdMockDeviceId(const std::string& deviceId) noexcept {
    return deviceId.rfind("mock:", 0) == 0;
}

bool isNetMdTestMockGuardEnabled() noexcept {
    return g_mockGuardEnabled.load(std::memory_order_acquire);
}

void setNetMdHelperPathOverride(const std::string& path) {
    const std::lock_guard<std::mutex> lk(g_helperConfigMutex);
    g_helperPathOverride = path;
}

namespace {

// ============================================================================
// Internal Span Cache, Locking, & Helper Process Transport
// ============================================================================

struct CachedSpanEntry {
    MdCodecMode codec = MdCodecMode::spStereo;
    std::int64_t startSample = 0;
    std::int64_t endSample = 0;
    int unitCount = 0;
    std::vector<std::pair<UtocAddress, UtocAddress>> physRanges;
};

struct CachedPhysicalSpanMap {
    std::string deviceId;
    std::vector<CachedSpanEntry> spans;
};

std::mutex& cachedSpanMutex() {
    static std::mutex m;
    return m;
}

CachedPhysicalSpanMap& cachedSpanMap() {
    static CachedPhysicalSpanMap map;
    return map;
}

void clearCachedPhysicalSpanMap(const std::string& deviceId) {
    std::lock_guard<std::mutex> lk(cachedSpanMutex());
    if (deviceId.empty() || cachedSpanMap().deviceId == deviceId) {
        cachedSpanMap().deviceId.clear();
        cachedSpanMap().spans.clear();
    }
}

int physicalTotalClustersFromUtoc(const UtocDiscState& utoc) {
    if (!utoc.hasSector0) {
        return 0;
    }
    std::uint16_t minCluster = 0;
    std::uint16_t maxCluster = 0;
    auto noteAddr = [&](const UtocAddress& a, const UtocAddress& b) {
        if (a.cluster > 0) {
            minCluster = (minCluster == 0) ? a.cluster : std::min(minCluster, a.cluster);
        }
        maxCluster = std::max(maxCluster, b.cluster);
    };
    auto scanChain = [&](std::uint8_t head) {
        std::uint8_t slot = head;
        std::set<std::uint8_t> visited;
        while (slot != 0 && visited.insert(slot).second && visited.size() <= 255) {
            const auto& f = utoc.fragments[slot];
            if (f.isValid() && f.end.toLinearGroup() >= f.start.toLinearGroup()) {
                noteAddr(f.start, f.end);
            }
            slot = f.link;
        }
    };
    scanChain(utoc.trackMap[0]);       // P-FRA
    scanChain(utoc.sec0aPadding[14]);  // P-DFA
    const int nTracks = std::min<int>(utoc.trackCount, kUtocMaxTracks);
    for (int t = 1; t <= nTracks; ++t) {
        scanChain(utoc.trackMap[static_cast<std::size_t>(t)]);
    }
    if (minCluster == 0 || maxCluster < minCluster) {
        return 0;
    }
    return static_cast<int>(maxCluster - minCluster + 1);
}

std::int64_t expectedUploadedUnitsForSpan(const MdPhysicalSpan& span) noexcept {
    const auto unpadded = std::max<std::int64_t>(0, span.endSample - span.startSample);
    if (unpadded == 0) {
        return 0;
    }
    if (span.codec == MdCodecMode::spStereo || span.codec == MdCodecMode::spMono) {
        const std::int64_t pcmFrames = (unpadded + 511) / 512;
        const std::int64_t totalOutFrames = pcmFrames + 1; // +1 carta1 flush frame
        const std::int64_t rem = totalOutFrames % 11;
        const std::int64_t uploadedFrames = totalOutFrames + (rem == 0 ? 0 : (11 - rem));
        const int upq = (span.codec == MdCodecMode::spStereo) ? 2 : 1;
        return uploadedFrames * upq;
    }
    const std::int64_t alignQuantum = (span.codec == MdCodecMode::lp2) ? 2048 : 4096;
    const std::int64_t rem = unpadded % alignQuantum;
    const std::int64_t target = rem != 0 ? (unpadded + (alignQuantum - rem)) : unpadded;
    const std::int64_t lead = std::min<std::int64_t>(target, 69);
    const std::int64_t mainCalls = (target > lead) ? ((target - lead + 1023) / 1024) : 0;
    const std::int64_t emittedAtracFrames = 1 + std::max<std::int64_t>(0, mainCalls - 1) + 3;
    const int upq = (span.codec == MdCodecMode::lp2) ? 2 : 1;
    return emittedAtracFrames * upq;
}

std::string formatUtocAddressShort(const UtocAddress& a) {
    return std::to_string(a.cluster) + ":" + std::to_string(a.sector) + "." +
           std::to_string(a.group);
}

std::string formatUtocRangeSummary(const std::vector<std::pair<UtocAddress, UtocAddress>>& ranges) {
    if (ranges.empty()) {
        return "empty";
    }
    std::int64_t units = 0;
    for (const auto& r : ranges) {
        const auto sLin = r.first.toLinearGroup();
        const auto eLin = r.second.toLinearGroup();
        if (eLin >= sLin) {
            units += (eLin - sLin + 1);
        }
    }
    std::string out = formatUtocAddressShort(ranges.front().first);
    out += (ranges.size() == 1 ? "-" : "..");
    out += formatUtocAddressShort(ranges.back().second);
    out += " (units=" + std::to_string(units) + ", frags=" + std::to_string(ranges.size()) + ")";
    return out;
}

std::string describeDeviceUtocSector0(const UtocDiscState& utoc) {
    if (!utoc.hasSector0) {
        return "missing-sector0";
    }
    std::ostringstream oss;
    oss << "tracks=" << static_cast<int>(utoc.trackCount)
        << " physClusters=" << physicalTotalClustersFromUtoc(utoc)
        << " pfraSlot=" << static_cast<int>(utoc.trackMap[0])
        << " pdfaSlot=" << static_cast<int>(utoc.sec0aPadding[14]);
    const int limit = std::min<int>(utoc.trackCount, 4);
    for (int t = 1; t <= limit; ++t) {
        std::vector<std::pair<UtocAddress, UtocAddress>> ranges;
        std::uint8_t modeByte = 0;
        bool hasMode = false;
        std::uint8_t slot = utoc.trackMap[static_cast<std::size_t>(t)];
        std::set<std::uint8_t> visited;
        while (slot != 0 && visited.insert(slot).second && visited.size() <= 255) {
            const auto& f = utoc.fragments[slot];
            if (f.isValid()) {
                if (!hasMode) {
                    modeByte = f.mode;
                    hasMode = true;
                }
                ranges.emplace_back(f.start, f.end);
            }
            slot = f.link;
        }
        char modeHex[8];
        std::snprintf(modeHex, sizeof(modeHex), "0x%02x", static_cast<unsigned>(modeByte));
        oss << " t" << t << "[" << (hasMode ? audioModeFromUtocMode(modeByte) : "none") << "("
            << modeHex << ") " << formatUtocRangeSummary(ranges) << "]";
    }
    if (utoc.trackCount > 4) {
        oss << " ...";
    }
    return oss.str();
}

std::string formatBudgetOverflow(const char* prefix, const UtocBudget& b) {
    return std::string(prefix) + " (clusters=" + std::to_string(b.usedClusters) + "/" +
           std::to_string(b.totalClusters) + ", tracks=" + std::to_string(b.usedTracks) + "/" +
           std::to_string(b.maxTracks) + ", fragments=" + std::to_string(b.usedFragments) + "/" +
           std::to_string(b.maxFragments) + ", hwCells=" + std::to_string(b.usedHalfWidthCells) +
           "/" + std::to_string(b.maxHalfWidthCells) + ", fwCells=" +
           std::to_string(b.usedFullWidthCells) + "/" + std::to_string(b.maxFullWidthCells) + ").";
}

std::mutex& netMdBridgeMutex() {
    static std::mutex m;
    return m;
}

struct ScopedNetMdProgress {
    bool enabled = false;
    explicit ScopedNetMdProgress(bool enable = true) noexcept : enabled(enable) {}
    ~ScopedNetMdProgress() {
        if (enabled) {
            clearNetMdProgress();
        }
    }
    ScopedNetMdProgress(const ScopedNetMdProgress&) = delete;
    ScopedNetMdProgress& operator=(const ScopedNetMdProgress&) = delete;
};

struct HelperProgressWindow {
    bool active = false;
    std::string operation;
    std::string fallbackPhase;
    bool preferFallbackPhase = false;
    double baseProgress = 0.0;
    double spanProgress = 1.0;
    std::int64_t bytesOffset = 0;
    std::int64_t bytesSpan = 0;
    std::int64_t bytesTotal = 0;
};

bool isExecutableFile(const juce::File& file) {
    if (!file.existsAsFile()) {
        return false;
    }
#if !defined(_WIN32)
    return ::access(file.getFullPathName().toRawUTF8(), X_OK) == 0;
#else
    return true;
#endif
}

bool resolveNetMdHelperPath(std::string& outPath, std::string& error) {
    outPath.clear();
    {
        const std::lock_guard<std::mutex> lk(g_helperConfigMutex);
        if (!g_helperPathOverride.empty()) {
            const juce::File f(juce::String::fromUTF8(g_helperPathOverride.c_str()));
            if (isExecutableFile(f)) {
                outPath = f.getFullPathName().toStdString();
                return true;
            }
            error = "Configured carta-netmd helper is not executable: " + g_helperPathOverride;
            return false;
        }
    }

    if (const char* envPath = std::getenv("CARTA_NETMD_HELPER"); envPath && envPath[0] != '\0') {
        const juce::File f(juce::String::fromUTF8(envPath));
        if (isExecutableFile(f)) {
            outPath = f.getFullPathName().toStdString();
            return true;
        }
    }

    const auto exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto exeDir = exeFile.getParentDirectory();
#if defined(_WIN32)
    constexpr const char* kHelperName = "carta-netmd.exe";
    constexpr char kPathSep = ';';
#else
    constexpr const char* kHelperName = "carta-netmd";
    constexpr char kPathSep = ':';
#endif
    const std::array<juce::File, 6> candidates = {
        exeDir.getParentDirectory().getChildFile("Helpers").getChildFile(kHelperName),
        exeDir.getChildFile(kHelperName),
        exeDir.getParentDirectory().getChildFile(kHelperName),
        exeDir.getParentDirectory().getParentDirectory().getChildFile(kHelperName),
        exeDir.getParentDirectory().getParentDirectory().getChildFile("Release").getChildFile(
            kHelperName),
        exeDir.getParentDirectory().getParentDirectory().getParentDirectory().getChildFile(
            kHelperName),
    };
    for (const auto& cand : candidates) {
        if (isExecutableFile(cand)) {
            outPath = cand.getFullPathName().toStdString();
            return true;
        }
    }

    if (const char* pathEnv = std::getenv("PATH"); pathEnv && pathEnv[0] != '\0') {
        std::istringstream iss(pathEnv);
        std::string dir;
        while (std::getline(iss, dir, kPathSep)) {
            if (dir.empty()) {
                continue;
            }
            const juce::File cand =
                juce::File(juce::String::fromUTF8(dir.c_str())).getChildFile(kHelperName);
            if (isExecutableFile(cand)) {
                outPath = cand.getFullPathName().toStdString();
                return true;
            }
        }
    }

    error = "Could not locate carta-netmd helper executable (checked Contents/Helpers, sibling "
            "directories, and PATH).";
    return false;
}

std::string percentEncode(const std::string& s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '/' || c == ':') {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0Fu]);
            out.push_back(kHex[c & 0x0Fu]);
        }
    }
    return out;
}

void applyHelperProgressEvent(const juce::DynamicObject& obj, const HelperProgressWindow& win) {
    if (!win.active) {
        return;
    }
    const double evProg = std::clamp(static_cast<double>(obj.getProperty("progress")), 0.0, 1.0);
    const std::string evPhase = obj.getProperty("phase").toString().toStdString();
    const auto evDone =
        static_cast<std::int64_t>(static_cast<juce::int64>(obj.getProperty("bytes_done")));
    const auto evTotal =
        static_cast<std::int64_t>(static_cast<juce::int64>(obj.getProperty("bytes_total")));

    const std::string phase =
        (win.preferFallbackPhase || evPhase.empty()) ? win.fallbackPhase : evPhase;
    const double mappedProg = std::clamp(win.baseProgress + win.spanProgress * evProg, 0.0, 1.0);

    std::int64_t doneBytes = 0;
    const std::int64_t totalBytes = win.bytesTotal;
    if (win.bytesTotal > 0 && win.bytesSpan > 0) {
        double frac = evProg;
        if (evTotal > 0) {
            frac = std::clamp(static_cast<double>(evDone) / static_cast<double>(evTotal), 0.0, 1.0);
        }
        doneBytes = std::clamp<std::int64_t>(
            win.bytesOffset +
                static_cast<std::int64_t>(std::llround(frac * static_cast<double>(win.bytesSpan))),
            0, win.bytesTotal);
    }
    setNetMdProgress(win.operation, phase, mappedProg, doneBytes, totalBytes);
}

bool readNdjsonUntilResult(std::FILE* stream, const HelperProgressWindow& win,
                           juce::var& outResultObj, std::string& outRawResultLine,
                           std::string& error) {
    outResultObj = juce::var();
    outRawResultLine.clear();
    if (!stream) {
        error = "Internal error: null helper stdout stream";
        return false;
    }

    std::string lineBuf;
    char chunk[4096];
    while (std::fgets(chunk, sizeof(chunk), stream) != nullptr) {
        lineBuf += chunk;
        if (lineBuf.empty() || lineBuf.back() != '\n') {
            continue;
        }
        while (!lineBuf.empty() && (lineBuf.back() == '\n' || lineBuf.back() == '\r')) {
            lineBuf.pop_back();
        }
        if (lineBuf.empty()) {
            continue;
        }
        juce::var parsed;
        const auto res = juce::JSON::parse(juce::String::fromUTF8(lineBuf.c_str()), parsed);
        if (res.failed() || !parsed.isObject()) {
            lineBuf.clear();
            continue;
        }
        const auto* obj = parsed.getDynamicObject();
        if (!obj) {
            lineBuf.clear();
            continue;
        }
        const auto ev = obj->getProperty("event").toString();
        if (ev == "progress") {
            applyHelperProgressEvent(*obj, win);
            lineBuf.clear();
            continue;
        }
        if (ev == "result") {
            outRawResultLine = lineBuf;
            outResultObj = parsed;
            return true;
        }
        lineBuf.clear();
    }

    error = "carta-netmd helper exited before sending a result";
    return false;
}

#if !defined(_WIN32)

struct PersistentMockSession {
    pid_t pid = -1;
    std::FILE* stdinFile = nullptr;
    std::FILE* stdoutFile = nullptr;

    bool isRunning() const noexcept {
        return pid > 0 && stdinFile != nullptr && stdoutFile != nullptr;
    }

    void stop() {
        if (stdinFile != nullptr) {
            std::fputs("quit\n", stdinFile);
            std::fflush(stdinFile);
            std::fclose(stdinFile);
            stdinFile = nullptr;
        }
        if (stdoutFile != nullptr) {
            std::fclose(stdoutFile);
            stdoutFile = nullptr;
        }
        if (pid > 0) {
            int st = 0;
            ::waitpid(pid, &st, 0);
            pid = -1;
        }
        g_mockGuardEnabled.store(false, std::memory_order_release);
    }

    bool start(std::string& error) {
        stop();
        std::string helperPath;
        if (!resolveNetMdHelperPath(helperPath, error)) {
            return false;
        }
        int inPipe[2] = {-1, -1};
        int outPipe[2] = {-1, -1};
        if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0) {
            if (inPipe[0] >= 0)
                ::close(inPipe[0]);
            if (inPipe[1] >= 0)
                ::close(inPipe[1]);
            if (outPipe[0] >= 0)
                ::close(outPipe[0]);
            if (outPipe[1] >= 0)
                ::close(outPipe[1]);
            error = "Failed creating pipes for carta-netmd mock session";
            return false;
        }
        const pid_t child = ::fork();
        if (child < 0) {
            ::close(inPipe[0]);
            ::close(inPipe[1]);
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            error = "Failed forking carta-netmd mock session";
            return false;
        }
        if (child == 0) {
            ::dup2(inPipe[0], STDIN_FILENO);
            ::dup2(outPipe[1], STDOUT_FILENO);
            ::close(inPipe[0]);
            ::close(inPipe[1]);
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            char* argv[] = {const_cast<char*>(helperPath.c_str()), const_cast<char*>("session"),
                            const_cast<char*>("--mock"), nullptr};
            ::execv(helperPath.c_str(), argv);
            ::_exit(127);
        }
        ::close(inPipe[0]);
        ::close(outPipe[1]);
        pid = child;
        stdinFile = ::fdopen(inPipe[1], "w");
        stdoutFile = ::fdopen(outPipe[0], "r");
        if (!stdinFile || !stdoutFile) {
            stop();
            error = "Failed opening stdio streams for carta-netmd mock session";
            return false;
        }
        return true;
    }

    ~PersistentMockSession() { stop(); }
};

PersistentMockSession& mockSession() {
    static PersistentMockSession s;
    return s;
}

bool runOneShotHelperCommand(const std::vector<std::string>& args, const HelperProgressWindow& win,
                             juce::var& outResultObj, std::string& outRawResultLine,
                             std::string& error) {
    outResultObj = juce::var();
    outRawResultLine.clear();

    std::string helperPath;
    if (!resolveNetMdHelperPath(helperPath, error)) {
        return false;
    }

    int outPipe[2] = {-1, -1};
    if (::pipe(outPipe) != 0) {
        error = "Failed creating stdout pipe for carta-netmd";
        return false;
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(helperPath.c_str()));
    for (const auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        error = "Failed spawning carta-netmd helper process";
        return false;
    }
    if (child == 0) {
        ::dup2(outPipe[1], STDOUT_FILENO);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        ::execv(helperPath.c_str(), argv.data());
        ::_exit(127);
    }

    ::close(outPipe[1]);
    std::FILE* outStream = ::fdopen(outPipe[0], "r");
    if (!outStream) {
        ::close(outPipe[0]);
        int st = 0;
        ::waitpid(child, &st, 0);
        error = "Failed reading stdout from carta-netmd helper";
        return false;
    }

    const bool gotResult =
        readNdjsonUntilResult(outStream, win, outResultObj, outRawResultLine, error);
    std::fclose(outStream);

    int status = 0;
    ::waitpid(child, &status, 0);

    if (!gotResult) {
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            error = "Failed executing carta-netmd helper at: " + helperPath;
        }
        return false;
    }

    const auto* obj = outResultObj.getDynamicObject();
    if (!obj || !static_cast<bool>(obj->getProperty("ok"))) {
        const auto errProp =
            obj ? obj->getProperty("error").toString().toStdString() : std::string();
        error = errProp.empty() ? "carta-netmd operation failed" : errProp;
        return false;
    }
    return true;
}

bool runSessionHelperCommand(const std::string& sessionLine, const HelperProgressWindow& win,
                             juce::var& outResultObj, std::string& outRawResultLine,
                             std::string& error) {
    auto& sess = mockSession();
    if (!sess.isRunning()) {
        error = "carta-netmd mock session is not running";
        return false;
    }
    if (std::fprintf(sess.stdinFile, "%s\n", sessionLine.c_str()) < 0 ||
        std::fflush(sess.stdinFile) != 0) {
        error = "Failed writing command to carta-netmd mock session";
        return false;
    }
    if (!readNdjsonUntilResult(sess.stdoutFile, win, outResultObj, outRawResultLine, error)) {
        return false;
    }
    const auto* obj = outResultObj.getDynamicObject();
    if (!obj || !static_cast<bool>(obj->getProperty("ok"))) {
        const auto errProp =
            obj ? obj->getProperty("error").toString().toStdString() : std::string();
        error = errProp.empty() ? "carta-netmd mock session command failed" : errProp;
        return false;
    }
    return true;
}

bool invokeNetMdHelperUnlocked(const std::vector<std::string>& cliArgs,
                               const std::string& sessionLine, const HelperProgressWindow& win,
                               juce::var& outResultObj, std::string& error) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::string op = cliArgs.empty() ? "unknown" : cliArgs[0];
    const std::string dev = cliArgs.size() > 1 ? cliArgs[1] : "";
    std::string rawLine;
    const bool ok = mockSession().isRunning()
                        ? runSessionHelperCommand(sessionLine, win, outResultObj, rawLine, error)
                        : runOneShotHelperCommand(cliArgs, win, outResultObj, rawLine, error);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    if (ok) {
        logDiagnostic("netmd", op + (dev.empty() ? "" : " " + dev) + " -> ok (" +
                                   std::to_string(ms) + " ms)");
    } else {
        logDiagnostic("netmd", op + (dev.empty() ? "" : " " + dev) + " -> FAIL (" +
                                   std::to_string(ms) + " ms): " + error);
    }
    return ok;
}

#else

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring quoteWindowsArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring buildWindowsCommandLine(const std::string& exePath,
                                     const std::vector<std::string>& args) {
    std::wstring cmd = quoteWindowsArg(utf8ToWide(exePath));
    for (const auto& a : args) {
        cmd.push_back(L' ');
        cmd += quoteWindowsArg(utf8ToWide(a));
    }
    return cmd;
}

struct PersistentMockSession {
    HANDLE processHandle = nullptr;
    std::FILE* stdinFile = nullptr;
    std::FILE* stdoutFile = nullptr;

    bool isRunning() const noexcept {
        return processHandle != nullptr && stdinFile != nullptr && stdoutFile != nullptr;
    }

    void stop() {
        if (stdinFile != nullptr) {
            std::fputs("quit\n", stdinFile);
            std::fflush(stdinFile);
            std::fclose(stdinFile);
            stdinFile = nullptr;
        }
        if (stdoutFile != nullptr) {
            std::fclose(stdoutFile);
            stdoutFile = nullptr;
        }
        if (processHandle != nullptr) {
            WaitForSingleObject(processHandle, 5000);
            CloseHandle(processHandle);
            processHandle = nullptr;
        }
        g_mockGuardEnabled.store(false, std::memory_order_release);
    }

    bool start(std::string& error) {
        stop();
        std::string helperPath;
        if (!resolveNetMdHelperPath(helperPath, error)) {
            return false;
        }
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE inRead = nullptr;
        HANDLE inWrite = nullptr;
        HANDLE outRead = nullptr;
        HANDLE outWrite = nullptr;
        if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0)) {
            if (inRead)
                CloseHandle(inRead);
            if (inWrite)
                CloseHandle(inWrite);
            if (outRead)
                CloseHandle(outRead);
            if (outWrite)
                CloseHandle(outWrite);
            error = "Failed creating pipes for carta-netmd mock session";
            return false;
        }
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inRead;
        si.hStdOutput = outWrite;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        PROCESS_INFORMATION pi{};
        std::wstring cmdLine = buildWindowsCommandLine(helperPath, {"session", "--mock"});
        const BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(inRead);
        CloseHandle(outWrite);
        if (!ok) {
            CloseHandle(inWrite);
            CloseHandle(outRead);
            error = "Failed launching carta-netmd mock session";
            return false;
        }
        CloseHandle(pi.hThread);
        processHandle = pi.hProcess;
        const int inFd = _open_osfhandle(reinterpret_cast<intptr_t>(inWrite), _O_WRONLY | _O_TEXT);
        const int outFd = _open_osfhandle(reinterpret_cast<intptr_t>(outRead), _O_RDONLY | _O_TEXT);
        if (inFd < 0 || outFd < 0) {
            if (inFd >= 0)
                _close(inFd);
            else
                CloseHandle(inWrite);
            if (outFd >= 0)
                _close(outFd);
            else
                CloseHandle(outRead);
            stop();
            error = "Failed wrapping pipes for carta-netmd mock session";
            return false;
        }
        stdinFile = _fdopen(inFd, "w");
        stdoutFile = _fdopen(outFd, "r");
        if (!stdinFile || !stdoutFile) {
            stop();
            error = "Failed opening stdio streams for carta-netmd mock session";
            return false;
        }
        return true;
    }

    ~PersistentMockSession() { stop(); }
};

PersistentMockSession& mockSession() {
    static PersistentMockSession s;
    return s;
}

bool runOneShotHelperCommand(const std::vector<std::string>& args, const HelperProgressWindow& win,
                             juce::var& outResultObj, std::string& outRawResultLine,
                             std::string& error) {
    outResultObj = juce::var();
    outRawResultLine.clear();

    std::string helperPath;
    if (!resolveNetMdHelperPath(helperPath, error)) {
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        error = "Failed creating stdout pipe for carta-netmd";
        return false;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = outWrite;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring cmdLine = buildWindowsCommandLine(helperPath, args);
    const BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(outWrite);
    if (!ok) {
        CloseHandle(outRead);
        error = "Failed spawning carta-netmd helper at: " + helperPath;
        return false;
    }
    CloseHandle(pi.hThread);

    const int outFd = _open_osfhandle(reinterpret_cast<intptr_t>(outRead), _O_RDONLY | _O_TEXT);
    std::FILE* outStream = (outFd >= 0) ? _fdopen(outFd, "r") : nullptr;
    if (!outStream) {
        if (outFd >= 0)
            _close(outFd);
        else
            CloseHandle(outRead);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        error = "Failed reading stdout from carta-netmd helper";
        return false;
    }

    const bool gotResult =
        readNdjsonUntilResult(outStream, win, outResultObj, outRawResultLine, error);
    std::fclose(outStream);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);

    if (!gotResult) {
        return false;
    }
    const auto* obj = outResultObj.getDynamicObject();
    if (!obj || !static_cast<bool>(obj->getProperty("ok"))) {
        const auto errProp =
            obj ? obj->getProperty("error").toString().toStdString() : std::string();
        error = errProp.empty() ? "carta-netmd operation failed" : errProp;
        return false;
    }
    return true;
}

bool runSessionHelperCommand(const std::string& sessionLine, const HelperProgressWindow& win,
                             juce::var& outResultObj, std::string& outRawResultLine,
                             std::string& error) {
    auto& sess = mockSession();
    if (!sess.isRunning()) {
        error = "carta-netmd mock session is not running";
        return false;
    }
    if (std::fprintf(sess.stdinFile, "%s\n", sessionLine.c_str()) < 0 ||
        std::fflush(sess.stdinFile) != 0) {
        error = "Failed writing command to carta-netmd mock session";
        return false;
    }
    if (!readNdjsonUntilResult(sess.stdoutFile, win, outResultObj, outRawResultLine, error)) {
        return false;
    }
    const auto* obj = outResultObj.getDynamicObject();
    if (!obj || !static_cast<bool>(obj->getProperty("ok"))) {
        const auto errProp =
            obj ? obj->getProperty("error").toString().toStdString() : std::string();
        error = errProp.empty() ? "carta-netmd mock session command failed" : errProp;
        return false;
    }
    return true;
}

bool invokeNetMdHelperUnlocked(const std::vector<std::string>& cliArgs,
                               const std::string& sessionLine, const HelperProgressWindow& win,
                               juce::var& outResultObj, std::string& error) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::string op = cliArgs.empty() ? "unknown" : cliArgs[0];
    const std::string dev = cliArgs.size() > 1 ? cliArgs[1] : "";
    std::string rawLine;
    const bool ok = mockSession().isRunning()
                        ? runSessionHelperCommand(sessionLine, win, outResultObj, rawLine, error)
                        : runOneShotHelperCommand(cliArgs, win, outResultObj, rawLine, error);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    if (ok) {
        logDiagnostic("netmd", op + (dev.empty() ? "" : " " + dev) + " -> ok (" +
                                   std::to_string(ms) + " ms)");
    } else {
        logDiagnostic("netmd", op + (dev.empty() ? "" : " " + dev) + " -> FAIL (" +
                                   std::to_string(ms) + " ms): " + error);
    }
    return ok;
}

#endif

int uploadModeCodeForCodec(MdCodecMode codec) noexcept {
    switch (codec) {
    case MdCodecMode::spStereo:
        return 0;
    case MdCodecMode::spMono:
        return 1;
    case MdCodecMode::lp2:
        return 2;
    case MdCodecMode::lp4:
        return 3;
    case MdCodecMode::hiSp:
    case MdCodecMode::hiLp:
    case MdCodecMode::pcm:
        return -1;
    }
    return -1;
}

// ============================================================================
// Unlocked Helper Device I/O Wrappers (Caller Holds netMdBridgeMutex)
// ============================================================================

bool listNetMdDevicesUnlocked(std::vector<NetMdDeviceEntry>& out, std::string& error) {
    out.clear();
    error.clear();

    juce::var resObj;
    if (!invokeNetMdHelperUnlocked({"list"}, "list", HelperProgressWindow{}, resObj, error)) {
        return false;
    }
    const auto* root = resObj.getDynamicObject();
    const auto devsVar = root ? root->getProperty("devices") : juce::var();
    const auto* arr = devsVar.getArray();
    if (!arr) {
        return true;
    }
    for (const auto& item : *arr) {
        const auto* obj = item.getDynamicObject();
        if (!obj) {
            continue;
        }
        NetMdDeviceEntry entry;
        entry.id = obj->getProperty("id").toString().toStdString();
        entry.vendorId = static_cast<int>(obj->getProperty("vendorId"));
        entry.productId = static_cast<int>(obj->getProperty("productId"));
        entry.bus = static_cast<int>(obj->getProperty("bus"));
        entry.address = static_cast<int>(obj->getProperty("address"));
        entry.name = obj->getProperty("name").toString().toStdString();
        entry.isMock =
            static_cast<bool>(obj->getProperty("isMock")) || isNetMdMockDeviceId(entry.id);
        if (!entry.id.empty()) {
            out.push_back(std::move(entry));
        }
    }
    return true;
}

bool probeNetMdDeviceUnlocked(const std::string& deviceId, NetMdDeviceStatus& out,
                              std::string& error, bool includeTracks = true) {
    out = {};
    error.clear();
    if (deviceId.empty()) {
        error = "NetMD device ID is empty";
        return false;
    }

    std::vector<std::string> cli = {"probe", deviceId};
    if (!includeTracks) {
        cli.push_back("--no-tracks");
    }
    const std::string sess =
        "probe\t" + percentEncode(deviceId) + (includeTracks ? "\t1" : "\t0");

    juce::var resObj;
    if (!invokeNetMdHelperUnlocked(cli, sess, HelperProgressWindow{}, resObj, error)) {
        return false;
    }
    const auto* root = resObj.getDynamicObject();
    const auto statusVar = root ? root->getProperty("status") : juce::var();
    const auto* obj = statusVar.getDynamicObject();
    if (!obj) {
        error = "Empty NetMD probe status returned by carta-netmd";
        return false;
    }

    out.deviceId = obj->getProperty("deviceId").toString().toStdString();
    out.deviceName = obj->getProperty("deviceName").toString().toStdString();
    out.versionCode = obj->getProperty("versionCode").toString().toStdString();
    out.supportsSpUpload = static_cast<bool>(obj->getProperty("supportsSpUpload"));
    out.supportsForceTocEdit = static_cast<bool>(obj->getProperty("supportsForceTocEdit"));
    out.discPresent = static_cast<bool>(obj->getProperty("discPresent"));
    out.writable = static_cast<bool>(obj->getProperty("writable"));
    out.writeProtected = static_cast<bool>(obj->getProperty("writeProtected"));
    out.trackCount = static_cast<int>(obj->getProperty("trackCount"));
    out.usedSeconds = static_cast<int>(obj->getProperty("usedSeconds"));
    out.totalSeconds = static_cast<int>(obj->getProperty("totalSeconds"));
    out.leftSeconds = static_cast<int>(obj->getProperty("leftSeconds"));
    out.discTitle = obj->getProperty("discTitle").toString().toStdString();
    out.fullWidthTitle = obj->getProperty("fullWidthTitle").toString().toStdString();
    out.hiMdFormatted = (out.discTitle == "Hi-MD Disc");
    out.tracks.clear();
    const auto tracksVar = obj->getProperty("tracks");
    if (const auto* tracksArr = tracksVar.getArray()) {
        out.tracks.reserve(static_cast<std::size_t>(tracksArr->size()));
        for (const auto& item : *tracksArr) {
            const auto* trObj = item.getDynamicObject();
            if (!trObj) {
                continue;
            }
            NetMdDeviceTrack tr;
            tr.index = static_cast<int>(trObj->getProperty("index"));
            tr.title = trObj->getProperty("title").toString().toStdString();
            tr.fullWidthTitle = trObj->getProperty("fullWidthTitle").toString().toStdString();
            tr.codec = trObj->getProperty("codec").toString().toStdString();
            tr.durationSeconds = static_cast<int>(trObj->getProperty("durationSeconds"));
            out.tracks.push_back(std::move(tr));
        }
    }
    logDiagnostic("probe", out.deviceName + " [" + out.deviceId + "] fw=" + out.versionCode +
                               " spUpload=" + (out.supportsSpUpload ? "1" : "0") +
                               " forceToc=" + (out.supportsForceTocEdit ? "1" : "0") +
                               " disc=" + (out.discPresent ? "1" : "0") +
                               " writable=" + (out.writable ? "1" : "0") +
                               " tracks=" + std::to_string(out.trackCount) +
                               " used=" + std::to_string(out.usedSeconds) + "s" +
                               " left=" + std::to_string(out.leftSeconds) + "s");
    return true;
}

bool resolveTargetDeviceIdUnlocked(const std::string& preferredId, std::string& outId,
                                   std::string& error) {
    if (!preferredId.empty()) {
        outId = preferredId;
        return true;
    }
    std::vector<NetMdDeviceEntry> devs;
    if (!listNetMdDevicesUnlocked(devs, error)) {
        return false;
    }
    if (devs.empty()) {
        error = "No USB NetMD / Hi-MD device detected.";
        return false;
    }
    outId = devs.front().id;
    return true;
}

bool eraseDeviceUnlocked(const std::string& deviceId, bool confirmHardware,
                         const HelperProgressWindow& win, std::string& error) {
    std::vector<std::string> cli = {"erase", deviceId};
    if (confirmHardware) {
        cli.push_back("--confirm-hardware");
    }
    const std::string sess =
        "erase\t" + percentEncode(deviceId) + "\t" + (confirmHardware ? "1" : "0");
    juce::var resObj;
    return invokeNetMdHelperUnlocked(cli, sess, win, resObj, error);
}

bool readDeviceUtocSectorsUnlocked(
    const std::string& deviceId,
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5>& rawSectors,
    UtocDiscState& parsedState, const HelperProgressWindow& win, std::string& error) {
    juce::TemporaryFile tmp(".utoc.bin");
    const std::string outPath = tmp.getFile().getFullPathName().toStdString();

    juce::var resObj;
    if (!invokeNetMdHelperUnlocked({"read-utoc", deviceId, "--out", outPath},
                                   "read-utoc\t" + percentEncode(deviceId) + "\t" +
                                       percentEncode(outPath),
                                   win, resObj, error)) {
        return false;
    }

    juce::MemoryBlock data;
    if (!tmp.getFile().loadFileAsData(data) || data.getSize() != 5 * kUtocSectorBytes) {
        error = "Unexpected UTOC sector payload length returned by carta-netmd";
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data.getData());
    for (std::size_t s = 0; s < 5; ++s) {
        std::memcpy(rawSectors[s].data(), bytes + s * kUtocSectorBytes, kUtocSectorBytes);
    }
    return parseUtocSectors(rawSectors[0].data(), rawSectors[1].data(), rawSectors[2].data(),
                            rawSectors[4].data(), parsedState, error);
}

bool validatePhysicalUtocClusters(const UtocDiscState& utoc, std::string& error) {
    // Rainbow Book: clusters 0..49 (0x00..0x31) are reserved for lead-in / UTOC.
    // Writing virtual clusters (e.g. kFirstAudioCluster = 1 from Export UTOC)
    // to a physical MiniDisc causes 'TOC Error' on every player.
    constexpr std::uint16_t kMinPhysicalAudioCluster = 50;
    for (int t = 1; t <= static_cast<int>(utoc.trackCount); ++t) {
        std::uint8_t slot = utoc.trackMap[static_cast<std::size_t>(t)];
        std::set<std::uint8_t> visited;
        while (slot != 0 && visited.insert(slot).second) {
            const auto& frag = utoc.fragments[slot];
            if (!frag.isValid() || frag.start.cluster < kMinPhysicalAudioCluster ||
                frag.end.cluster < frag.start.cluster) {
                error = "Refusing to write virtual/invalid UTOC cluster address (cluster " +
                        std::to_string(frag.start.cluster) +
                        " < 50) to physical MiniDisc. Use Record MiniDisc or import UTOC "
                        "directly from the disc.";
                return false;
            }
            slot = frag.link;
        }
    }
    return true;
}

bool prepareWriteSectorBundle(
    const UtocDiscState& utocState,
    const std::array<std::uint8_t, kUtocSectorBytes>* preservedSector3,
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5>& outRawSectors, int& outSectorMask,
    std::string& outSector0Hash, std::string& error) {
    for (auto& s : outRawSectors) {
        s.fill(0);
    }
    if (!compileUtocSectors(utocState, outRawSectors[0], outRawSectors[1], outRawSectors[2],
                            outRawSectors[4], error)) {
        return false;
    }
    const bool hasFullWidth =
        std::any_of(utocState.fullTitleMap.begin(), utocState.fullTitleMap.end(),
                    [](std::uint8_t s) { return s != 0; });
    outRawSectors[0][0x23] = static_cast<std::uint8_t>(outRawSectors[0][0x23] | 0x07u |
                                                       (hasFullWidth ? 0x10u : 0x00u));
    outSectorMask = (hasFullWidth || (outRawSectors[0][0x23] & 0x10u) != 0) ? 23 : 7;
    if (preservedSector3 != nullptr) {
        outRawSectors[3] = *preservedSector3;
    }
    outSector0Hash = computeUtocSectorSha256Hex(outRawSectors[0].data());
    return true;
}

bool writeDeviceUtocSectorsUnlocked(
    const std::string& deviceId, const UtocDiscState& utocState,
    const std::array<std::uint8_t, kUtocSectorBytes>* preservedSector3, bool confirmHardware,
    const HelperProgressWindow& win, std::string& error,
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5>* outCommittedRaw = nullptr,
    std::string* outCommittedSec0Hash = nullptr) {
    if (!isNetMdMockDeviceId(deviceId) && !validatePhysicalUtocClusters(utocState, error)) {
        return false;
    }

    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> rawBundle{};
    int sectorMask = 7;
    std::string sec0Hash;
    if (!prepareWriteSectorBundle(utocState, preservedSector3, rawBundle, sectorMask, sec0Hash,
                                  error)) {
        return false;
    }
    const std::string maskStr = std::to_string(sectorMask);

    std::vector<std::uint8_t> packed(5 * kUtocSectorBytes, 0);
    for (std::size_t s = 0; s < 5; ++s) {
        std::memcpy(packed.data() + s * kUtocSectorBytes, rawBundle[s].data(), kUtocSectorBytes);
    }

    juce::TemporaryFile tmp(".utoc.bin");
    if (!tmp.getFile().replaceWithData(packed.data(), packed.size())) {
        error = "Failed writing temporary UTOC payload file";
        return false;
    }
    const std::string inPath = tmp.getFile().getFullPathName().toStdString();

    std::vector<std::string> cli = {"write-utoc", deviceId, "--in", inPath, "--mask", maskStr};
    if (confirmHardware) {
        cli.push_back("--confirm-hardware");
    }
    const std::string sess = "write-utoc\t" + percentEncode(deviceId) + "\t" +
                             percentEncode(inPath) + "\t" + maskStr + "\t" +
                             (confirmHardware ? "1" : "0");
    juce::var resObj;
    if (!invokeNetMdHelperUnlocked(cli, sess, win, resObj, error)) {
        return false;
    }
    logDiagnostic("utoc", "write-utoc ok: device=" + deviceId +
                              " tracks=" + std::to_string(utocState.trackCount) +
                              " mask=" + maskStr +
                              " usedSectors=0x" +
                              juce::String::toHexString(rawBundle[0][0x23]).toStdString());

    if (!isNetMdMockDeviceId(deviceId)) {
        std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> verifyRaw{};
        UtocDiscState verifyUtoc{};
        std::string verifyErr;
        if (readDeviceUtocSectorsUnlocked(deviceId, verifyRaw, verifyUtoc, HelperProgressWindow{},
                                          verifyErr) &&
            verifyUtoc.trackCount != utocState.trackCount) {
            error = "Device DRAM UTOC verification mismatch after TOCWRT (expected " +
                    std::to_string(utocState.trackCount) + " tracks, read back " +
                    std::to_string(verifyUtoc.trackCount) + ").";
            logDiagnostic("utoc", error);
            return false;
        }
        logDiagnostic("utoc", "DRAM UTOC readback verified: trackCount=" +
                                  std::to_string(verifyUtoc.trackCount));
    }
    if (outCommittedRaw != nullptr) {
        *outCommittedRaw = rawBundle;
    }
    if (outCommittedSec0Hash != nullptr) {
        *outCommittedSec0Hash = sec0Hash;
    }
    return true;
}

bool uploadSpanUnlocked(const std::string& deviceId, int modeCode,
                        const std::vector<std::uint8_t>& wireBytes, const std::string& hwTitle,
                        const std::string& fwTitle, bool confirmHardware,
                        const HelperProgressWindow& win, int& outTrackIndex, std::string& error) {
    outTrackIndex = -1;
    juce::TemporaryFile tmp(".wire.bin");
    if (!tmp.getFile().replaceWithData(wireBytes.data(), wireBytes.size())) {
        error = "Failed writing temporary ATRAC wire payload file";
        return false;
    }
    const std::string inPath = tmp.getFile().getFullPathName().toStdString();

    std::vector<std::string> cli = {
        "upload", deviceId,     "--mode", std::to_string(modeCode), "--in", inPath, "--hw-title",
        hwTitle,  "--fw-title", fwTitle,
    };
    if (confirmHardware) {
        cli.push_back("--confirm-hardware");
    }
    const std::string sess = "upload\t" + percentEncode(deviceId) + "\t" +
                             std::to_string(modeCode) + "\t" + percentEncode(inPath) + "\t" +
                             percentEncode(hwTitle) + "\t" + percentEncode(fwTitle) + "\t" +
                             (confirmHardware ? "1" : "0");
    juce::var resObj;
    if (!invokeNetMdHelperUnlocked(cli, sess, win, resObj, error)) {
        return false;
    }
    const auto* root = resObj.getDynamicObject();
    if (!root || !root->hasProperty("track_index")) {
        error = "carta-netmd upload result missing track_index";
        return false;
    }
    outTrackIndex = static_cast<int>(root->getProperty("track_index"));
    return outTrackIndex >= 0;
}

void copyDeviceHeadersToUtoc(const UtocDiscState& deviceUtoc, UtocDiscState& dst) {
    dst.sec0Padding = deviceUtoc.sec0Padding;
    dst.deviceSignature =
        deviceUtoc.deviceSignature != 0 ? deviceUtoc.deviceSignature : kCartaUtocSignature;
    dst.sec0aPadding = deviceUtoc.sec0aPadding;
    dst.sec1Padding = deviceUtoc.sec1Padding;
    dst.sec2Padding = deviceUtoc.sec2Padding;
    dst.sec4Padding = deviceUtoc.sec4Padding;
}

// ============================================================================
// Encoding Plan & Physical Cluster Remapping Helpers
// ============================================================================

struct SpanUploadPlan {
    int spanIndex = 0;
    MdCodecMode codec = MdCodecMode::spStereo;
    std::string modeName;
    int modeCode = 0;
    std::int64_t startSample = 0;
    std::int64_t endSample = 0;
    int expectedUnits = 0;
    std::string hwTitle;
    std::string fwTitle;
    std::vector<std::uint8_t> wireBytes;
    int uploadedTrackIndex = -1;
    std::vector<std::pair<UtocAddress, UtocAddress>> physRanges;
};

bool validateTracksAgainstTimeline(const MdDisc& disc, const Session& session, std::string& error) {
    const std::int64_t masterSamples = static_cast<std::int64_t>(projectEndSample(session));
    for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
        const auto& trk = disc.tracks[t];
        for (const auto& part : trk.parts) {
            if (disc.loadedDisc.active &&
                findMdOnDiscRegionForPart(disc.loadedDisc, part) >= 0) {
                continue;
            }
            if (part.startSample < 0 || part.endSample <= part.startSample ||
                part.endSample > masterSamples) {
                error = "Track " + std::to_string(t + 1) + " part [" +
                        std::to_string(part.startSample) + ", " + std::to_string(part.endSample) +
                        ") exceeds timeline master (" + std::to_string(masterSamples) +
                        " samples).";
                return false;
            }
        }
    }
    return true;
}

bool buildSpanUploadPlans(const MdDisc& disc, const MdPhysicalPlan& physPlan,
                          const Session* sessionOrNull, std::vector<SpanUploadPlan>& plans,
                          std::string& error) {
    plans.clear();
    if (sessionOrNull != nullptr && !validateTracksAgainstTimeline(disc, *sessionOrNull, error)) {
        return false;
    }

    auto spanTargetSampleCount = [](const MdPhysicalSpan& span) noexcept -> std::size_t {
        const auto unpaddedSamples =
            static_cast<std::size_t>(std::max<std::int64_t>(0, span.endSample - span.startSample));
        if (span.codec == MdCodecMode::spStereo || span.codec == MdCodecMode::spMono) {
            const std::size_t pcmFrames = (unpaddedSamples + 511u) / 512u;
            const std::size_t totalOutFrames = pcmFrames + 1u;
            const std::size_t remFrames = totalOutFrames % 11u;
            const std::size_t targetPcmFrames =
                pcmFrames + (remFrames == 0 ? 0 : (11u - remFrames));
            return targetPcmFrames * 512u;
        }
        const std::size_t alignQuantum = (span.codec == MdCodecMode::lp2) ? 2048u : 4096u;
        const std::size_t rem = unpaddedSamples % alignQuantum;
        return rem != 0 ? (unpaddedSamples + (alignQuantum - rem)) : unpaddedSamples;
    };

    auto estimateEncodeFramesForTargetSamples =
        [](MdCodecMode codec, std::size_t targetSamples) noexcept -> std::size_t {
        if (targetSamples == 0) {
            return 0;
        }
        if (codec == MdCodecMode::spStereo || codec == MdCodecMode::spMono) {
            return ((targetSamples + 511u) / 512u) + 1u;
        }
        const std::size_t lead = std::min<std::size_t>(targetSamples, 69u);
        const std::size_t mainCalls =
            (targetSamples > lead) ? ((targetSamples - lead + 1023u) / 1024u) : 0u;
        return 1u + mainCalls + 3u;
    };

    std::size_t totalEncodeFrames = 0;
    if (sessionOrNull != nullptr) {
        for (const auto& span : physPlan.spans) {
            totalEncodeFrames +=
                estimateEncodeFramesForTargetSamples(span.codec, spanTargetSampleCount(span));
        }
    }
    std::size_t completedFramesBeforeSpan = 0;

    plans.reserve(physPlan.spans.size());
    for (std::size_t i = 0; i < physPlan.spans.size(); ++i) {
        const auto& span = physPlan.spans[i];
        const int modeCode = uploadModeCodeForCodec(span.codec);
        const auto modeName = mdCodecModeToString(span.codec);
        const auto* codecSpec = findAtracModeSpec(modeName);
        if (!codecSpec) {
            error = "Missing Porffor ATRAC codec spec for mode '" + modeName + "'.";
            return false;
        }

        SpanUploadPlan plan;
        plan.spanIndex = static_cast<int>(i);
        plan.codec = span.codec;
        plan.modeName = modeName;
        plan.modeCode = modeCode;
        plan.startSample = span.startSample;
        plan.endSample = span.endSample;
        plan.expectedUnits = span.unitCount;
        plan.hwTitle = "Span " + std::to_string(i + 1);
        for (const auto& pl : physPlan.placements) {
            if (pl.spanIndex == static_cast<int>(i) && pl.trackIndex >= 0 &&
                pl.trackIndex < static_cast<int>(disc.tracks.size())) {
                const auto& trk = disc.tracks[static_cast<std::size_t>(pl.trackIndex)];
                if (!trk.halfWidthTitle.empty()) {
                    plan.hwTitle = trk.halfWidthTitle;
                }
                plan.fwTitle = trk.fullWidthTitle;
                break;
            }
        }

        if (sessionOrNull != nullptr) {
            const double startFrac = totalEncodeFrames > 0
                                         ? static_cast<double>(completedFramesBeforeSpan) /
                                               static_cast<double>(totalEncodeFrames)
                                         : 0.0;
            setNetMdProgress("record",
                             "Encoding " + modeName + " span " + std::to_string(i + 1) + "/" +
                                 std::to_string(physPlan.spans.size()) + " (0%)...",
                             0.02 + 0.28 * startFrac);
            std::vector<float> left;
            std::vector<float> right;
            if (!renderSessionMixRange(*sessionOrNull, span.startSample, span.endSample,
                                       codecSpec->channels, left, right, error)) {
                return false;
            }
            const std::size_t unpaddedSamples = left.size();
            const std::size_t targetSamples = spanTargetSampleCount(span);
            if (targetSamples > unpaddedSamples) {
                left.resize(targetSamples, 0.0f);
                if (!right.empty()) {
                    right.resize(targetSamples, 0.0f);
                }
            }
            const int sampleCount = static_cast<int>(left.size());
            const std::size_t spanFrames =
                estimateEncodeFramesForTargetSamples(span.codec, left.size());
            auto progressCb = [&](int doneFrames, int totalSpanFrames) {
                const std::size_t globalDone =
                    completedFramesBeforeSpan + static_cast<std::size_t>(std::max(0, doneFrames));
                const double overallFrac =
                    totalEncodeFrames > 0 ? std::clamp(static_cast<double>(globalDone) /
                                                           static_cast<double>(totalEncodeFrames),
                                                       0.0, 1.0)
                                          : 1.0;
                const int spanPct = totalSpanFrames > 0
                                        ? std::clamp(static_cast<int>(std::llround(
                                                         (static_cast<double>(doneFrames) * 100.0) /
                                                         static_cast<double>(totalSpanFrames))),
                                                     0, 100)
                                        : 100;
                setNetMdProgress("record",
                                 "Encoding " + modeName + " span " + std::to_string(i + 1) + "/" +
                                     std::to_string(physPlan.spans.size()) + " (" +
                                     std::to_string(spanPct) + "%)...",
                                 0.02 + 0.28 * overallFrac);
            };
            if (!encodeAtracNetMdWire(modeName, left.empty() ? nullptr : left.data(),
                                      codecSpec->channels == 2 && !right.empty()
                                          ? right.data()
                                          : (left.empty() ? nullptr : left.data()),
                                      codecSpec->channels, sampleCount, plan.wireBytes, progressCb,
                                      error)) {
                return false;
            }
            completedFramesBeforeSpan += spanFrames;
            if (plan.wireBytes.empty()) {
                error = "ATRAC encoder produced an empty NetMD wire payload for physical span " +
                        std::to_string(i + 1) + ".";
                return false;
            }
            if ((span.codec == MdCodecMode::spStereo || span.codec == MdCodecMode::spMono) &&
                (plan.wireBytes.size() % 2332) != 0) {
                error = "SP wire payload is not aligned to 2332-byte physical MD sectors (got " +
                        std::to_string(plan.wireBytes.size()) + " bytes).";
                return false;
            }
        }

        plans.push_back(std::move(plan));
    }

    return true;
}

bool extractTrackFragmentRanges(const UtocDiscState& deviceUtoc, int zeroBasedTrackIdx,
                                std::vector<std::pair<UtocAddress, UtocAddress>>& outRanges,
                                std::string& error) {
    outRanges.clear();
    if (zeroBasedTrackIdx < 0 || zeroBasedTrackIdx >= kUtocMaxTracks) {
        error = "Uploaded NetMD track index out of UTOC range.";
        return false;
    }
    std::uint8_t slot = deviceUtoc.trackMap[static_cast<std::size_t>(zeroBasedTrackIdx + 1)];
    std::set<std::uint8_t> visited;
    while (slot != 0) {
        if (!visited.insert(slot).second || visited.size() > 255) {
            error = "Cycle detected in NetMD device UTOC Sector 0 fragment chain.";
            return false;
        }
        const auto& frag = deviceUtoc.fragments[slot];
        if (!frag.isValid()) {
            error = "Device UTOC fragment slot " + std::to_string(slot) + " contains empty range.";
            return false;
        }
        outRanges.emplace_back(frag.start, frag.end);
        slot = frag.link;
    }
    if (outRanges.empty()) {
        error = "Device UTOC Sector 0 missing fragment chain for uploaded track " +
                std::to_string(zeroBasedTrackIdx) + ".";
        return false;
    }
    return true;
}

bool sliceUnitSubrange(const std::vector<std::pair<UtocAddress, UtocAddress>>& spanPhysRanges,
                       std::int64_t unitOffset, std::int64_t unitCount,
                       std::vector<std::pair<UtocAddress, UtocAddress>>& outRanges,
                       std::string& error) {
    outRanges.clear();
    if (unitOffset < 0 || unitCount <= 0) {
        error = "Invalid sound-group slice inside uploaded NetMD span.";
        return false;
    }

    const std::int64_t wantStart = unitOffset;
    const std::int64_t wantEnd = unitOffset + unitCount;
    std::int64_t cursor = 0;

    for (const auto& r : spanPhysRanges) {
        const std::int64_t sLin = r.first.toLinearGroup();
        const std::int64_t eLin = r.second.toLinearGroup();
        if (eLin < sLin) {
            error = "Device UTOC returned inverted fragment bounds.";
            return false;
        }
        const std::int64_t fragUnits = eLin - sLin + 1;
        const std::int64_t fragStart = cursor;
        const std::int64_t fragEnd = cursor + fragUnits;
        const std::int64_t overlapStart = std::max(wantStart, fragStart);
        const std::int64_t overlapEnd = std::min(wantEnd, fragEnd);
        if (overlapEnd > overlapStart) {
            const std::int64_t localStart = overlapStart - fragStart;
            const std::int64_t localLast = overlapEnd - fragStart - 1;
            outRanges.emplace_back(UtocAddress::fromLinearGroup(sLin + localStart),
                                   UtocAddress::fromLinearGroup(sLin + localLast));
        }
        cursor = fragEnd;
    }

    if (outRanges.empty() || cursor < wantEnd) {
        error = "Uploaded physical span on NetMD device has fewer sound groups (" +
                std::to_string(cursor) + ") than required (" + std::to_string(wantEnd) + ").";
        logDiagnostic("utoc", "sliceUnitSubrange FAIL: " + error +
                                  " (unitOffset=" + std::to_string(unitOffset) +
                                  ", unitCount=" + std::to_string(unitCount) +
                                  ", span=" + formatUtocRangeSummary(spanPhysRanges) + ")");
        return false;
    }
    return true;
}

bool remapSector0WithPhysicalUploads(const MdDisc& disc, const MdPhysicalPlan& physPlan,
                                     const UtocDiscState& deviceUtoc,
                                     const std::vector<SpanUploadPlan>& plans,
                                     UtocDiscState& finalUtoc, std::string& error) {
    copyDeviceHeadersToUtoc(deviceUtoc, finalUtoc);
    finalUtoc.sec0aPadding[14] = 0; // Reset P-DFA pointer; re-allocate valid chain below.
    finalUtoc.discNonEmpty = disc.tracks.empty() ? 0 : 1;
    finalUtoc.trackCount = static_cast<std::uint8_t>(disc.tracks.size());

    finalUtoc.trackMap.fill(0);
    for (std::size_t i = 0; i < kUtocSlots; ++i) {
        finalUtoc.fragments[i] = {};
    }
    for (std::uint8_t i = 1; i < 255; ++i) {
        finalUtoc.fragments[i].link = static_cast<std::uint8_t>(i + 1);
    }
    finalUtoc.fragments[255].link = 0;
    finalUtoc.nextFreeTrackSlot = 1;

    struct FragKey {
        std::uint16_t sc;
        std::uint8_t ss;
        std::uint8_t sg;
        std::uint8_t mode;
        std::uint16_t ec;
        std::uint8_t es;
        std::uint8_t eg;
        std::uint8_t link;
        bool operator<(const FragKey& o) const noexcept {
            return std::tie(sc, ss, sg, mode, ec, es, eg, link) <
                   std::tie(o.sc, o.ss, o.sg, o.mode, o.ec, o.es, o.eg, o.link);
        }
    };
    std::map<FragKey, std::uint8_t> fragPool;

    auto allocSlot = [&](const UtocFragment& frag, bool deduplicate,
                         std::uint8_t& outSlot) -> bool {
        if (deduplicate) {
            const FragKey k{frag.start.cluster, frag.start.sector, frag.start.group, frag.mode,
                            frag.end.cluster,   frag.end.sector,   frag.end.group,   frag.link};
            if (const auto it = fragPool.find(k); it != fragPool.end()) {
                outSlot = it->second;
                return true;
            }
        }
        if (finalUtoc.nextFreeTrackSlot == 0) {
            error = "UTOC Sector 0 fragment table exceeded 255 slots after physical remapping.";
            return false;
        }
        const std::uint8_t slot = finalUtoc.nextFreeTrackSlot;
        finalUtoc.nextFreeTrackSlot = finalUtoc.fragments[slot].link;
        finalUtoc.fragments[slot] = frag;
        if (deduplicate) {
            const FragKey k{frag.start.cluster, frag.start.sector, frag.start.group, frag.mode,
                            frag.end.cluster,   frag.end.sector,   frag.end.group,   frag.link};
            fragPool.emplace(k, slot);
        }
        outSlot = slot;
        return true;
    };

    // Preserve device free-area chain (P-FRA at trackMap[0]) in physical coordinates.
    if (deviceUtoc.trackMap[0] != 0) {
        std::vector<UtocFragment> pfraChain;
        std::uint8_t slot = deviceUtoc.trackMap[0];
        std::set<std::uint8_t> visited;
        while (slot != 0 && visited.insert(slot).second && visited.size() <= 255) {
            pfraChain.push_back(deviceUtoc.fragments[slot]);
            slot = deviceUtoc.fragments[slot].link;
        }
        std::uint8_t nextLink = 0;
        for (auto it = pfraChain.rbegin(); it != pfraChain.rend(); ++it) {
            UtocFragment f = *it;
            f.link = nextLink;
            std::uint8_t assigned = 0;
            if (!allocSlot(f, false, assigned)) {
                return false;
            }
            nextLink = assigned;
        }
        finalUtoc.trackMap[0] = nextLink;
    }

    // Preserve device defective-area chain (P-DFA at sec0aPadding[14]) in physical coordinates.
    if (deviceUtoc.sec0aPadding[14] != 0) {
        std::vector<UtocFragment> pdfaChain;
        std::uint8_t slot = deviceUtoc.sec0aPadding[14];
        std::set<std::uint8_t> visited;
        while (slot != 0 && visited.insert(slot).second && visited.size() <= 255) {
            pdfaChain.push_back(deviceUtoc.fragments[slot]);
            slot = deviceUtoc.fragments[slot].link;
        }
        std::uint8_t nextLink = 0;
        for (auto it = pdfaChain.rbegin(); it != pdfaChain.rend(); ++it) {
            UtocFragment f = *it;
            f.link = nextLink;
            std::uint8_t assigned = 0;
            if (!allocSlot(f, false, assigned)) {
                return false;
            }
            nextLink = assigned;
        }
        finalUtoc.sec0aPadding[14] = nextLink;
    }

    std::map<std::pair<int, int>, const MdPartPlacement*> placeByTrackPart;
    for (const auto& pl : physPlan.placements) {
        placeByTrackPart.emplace(std::make_pair(pl.trackIndex, pl.partIndex), &pl);
    }

    for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
        const auto& trk = disc.tracks[t];
        const auto codec = effectiveTrackCodec(disc, trk);
        const std::uint8_t modeByte = utocModeFromTrack(
            mdCodecModeToString(codec), trk.writeProtected, trk.scmsCopyProtected, trk.emphasis);

        std::vector<std::pair<UtocAddress, UtocAddress>> trackRanges;
        for (std::size_t pIdx = 0; pIdx < trk.parts.size(); ++pIdx) {
            const auto plIt = placeByTrackPart.find({static_cast<int>(t), static_cast<int>(pIdx)});
            if (plIt == placeByTrackPart.end()) {
                error = "Missing physical placement for track " + std::to_string(t + 1) + " part " +
                        std::to_string(pIdx + 1) + ".";
                return false;
            }
            const auto* pl = plIt->second;
            if (pl->spanIndex < 0 || pl->spanIndex >= static_cast<int>(plans.size())) {
                error = "Physical span index out of range during Sector 0 remap.";
                return false;
            }
            std::vector<std::pair<UtocAddress, UtocAddress>> partRanges;
            if (!sliceUnitSubrange(plans[static_cast<std::size_t>(pl->spanIndex)].physRanges,
                                   pl->unitOffsetInSpan, pl->unitCount, partRanges, error)) {
                return false;
            }
            for (const auto& pr : partRanges) {
                if (!trackRanges.empty() &&
                    trackRanges.back().second.toLinearGroup() + 1 == pr.first.toLinearGroup()) {
                    trackRanges.back().second = pr.second;
                } else {
                    trackRanges.push_back(pr);
                }
            }
        }

        std::uint8_t nextLink = 0;
        for (auto rIt = trackRanges.rbegin(); rIt != trackRanges.rend(); ++rIt) {
            UtocFragment frag;
            frag.start = rIt->first;
            frag.end = rIt->second;
            frag.mode = modeByte;
            frag.link = nextLink;
            std::uint8_t assigned = 0;
            if (!allocSlot(frag, true, assigned)) {
                return false;
            }
            nextLink = assigned;
        }
        finalUtoc.trackMap[t + 1] = nextLink;
    }

    return true;
}

void storeCachedPhysicalSpanMap(const std::string& resolvedId, const MdPhysicalPlan& physPlan,
                                const std::vector<SpanUploadPlan>& plans) {
    std::lock_guard<std::mutex> lk(cachedSpanMutex());
    auto& cache = cachedSpanMap();
    cache.deviceId = resolvedId;
    cache.spans.clear();
    cache.spans.reserve(plans.size());
    for (std::size_t i = 0; i < physPlan.spans.size() && i < plans.size(); ++i) {
        CachedSpanEntry entry;
        entry.codec = physPlan.spans[i].codec;
        entry.startSample = physPlan.spans[i].startSample;
        entry.endSample = physPlan.spans[i].endSample;
        entry.unitCount = physPlan.spans[i].unitCount;
        entry.physRanges = plans[i].physRanges;
        cache.spans.push_back(std::move(entry));
    }
}

bool tryRemapUtocFromCachedSpans(const MdDisc& disc, const std::string& resolvedId,
                                 const UtocDiscState& deviceUtoc, UtocDiscState& logicalUtoc,
                                 bool& outRemapped, std::string& error) {
    outRemapped = false;
    MdPhysicalPlan physPlan{};
    std::string planErr;
    if (!planMdPhysicalAudio(disc, physPlan, planErr)) {
        return true;
    }

    std::vector<SpanUploadPlan> cachedPlans;
    {
        std::lock_guard<std::mutex> lk(cachedSpanMutex());
        const auto& cache = cachedSpanMap();
        if (cache.deviceId == resolvedId && cache.spans.size() == physPlan.spans.size()) {
            bool allMatch = true;
            for (std::size_t i = 0; i < physPlan.spans.size(); ++i) {
                const auto& a = cache.spans[i];
                const auto& b = physPlan.spans[i];
                if (a.codec != b.codec || a.startSample != b.startSample ||
                    a.endSample != b.endSample || a.unitCount != b.unitCount ||
                    a.physRanges.empty()) {
                    allMatch = false;
                    break;
                }
            }
            if (allMatch) {
                cachedPlans.reserve(cache.spans.size());
                for (std::size_t i = 0; i < cache.spans.size(); ++i) {
                    SpanUploadPlan up;
                    up.spanIndex = static_cast<int>(i);
                    up.codec = cache.spans[i].codec;
                    up.startSample = cache.spans[i].startSample;
                    up.endSample = cache.spans[i].endSample;
                    up.expectedUnits = cache.spans[i].unitCount;
                    up.physRanges = cache.spans[i].physRanges;
                    cachedPlans.push_back(std::move(up));
                }
            }
        }
    }

    if (cachedPlans.empty()) {
        return true;
    }

    UtocDiscState remapped = logicalUtoc;
    if (!remapSector0WithPhysicalUploads(disc, physPlan, deviceUtoc, cachedPlans, remapped,
                                         error)) {
        return false;
    }
    logicalUtoc = std::move(remapped);
    outRemapped = true;
    return true;
}

bool tryPreserveDeviceSector0Topology(const MdDisc& disc, const UtocDiscState& deviceUtoc,
                                      UtocDiscState& logicalUtoc) {
    if (!deviceUtoc.hasSector0 ||
        static_cast<std::size_t>(deviceUtoc.trackCount) != disc.tracks.size() ||
        disc.tracks.empty()) {
        return false;
    }

    bool topologyMatches = true;
    std::vector<std::vector<std::uint8_t>> devSlotsByTrack(disc.tracks.size());
    for (std::size_t t = 0; t < disc.tracks.size() && topologyMatches; ++t) {
        std::vector<std::uint8_t> devChain;
        std::set<std::uint8_t> devGuard;
        std::uint8_t dSlot = deviceUtoc.trackMap[t + 1];
        while (dSlot != 0 && devGuard.insert(dSlot).second) {
            if (!deviceUtoc.fragments[dSlot].isValid()) {
                topologyMatches = false;
                break;
            }
            devChain.push_back(dSlot);
            dSlot = deviceUtoc.fragments[dSlot].link;
        }

        struct CoalescedLogicalFrag {
            std::int64_t startLin = 0;
            std::int64_t endLin = 0;
            std::string codec;
        };
        std::vector<CoalescedLogicalFrag> logRanges;
        std::set<std::uint8_t> logGuard;
        std::uint8_t lSlot = logicalUtoc.trackMap[t + 1];
        while (lSlot != 0 && logGuard.insert(lSlot).second) {
            const auto& lf = logicalUtoc.fragments[lSlot];
            if (!lf.isValid()) {
                topologyMatches = false;
                break;
            }
            const std::int64_t sLin = lf.start.toLinearGroup();
            const std::int64_t eLin = lf.end.toLinearGroup();
            const std::string codec = audioModeFromUtocMode(lf.mode);
            if (!logRanges.empty() && logRanges.back().endLin + 1 == sLin &&
                logRanges.back().codec == codec) {
                logRanges.back().endLin = eLin;
            } else {
                logRanges.push_back({sLin, eLin, codec});
            }
            lSlot = lf.link;
        }

        if (devChain.empty() || devChain.size() != logRanges.size()) {
            topologyMatches = false;
            break;
        }
        for (std::size_t k = 0; k < devChain.size(); ++k) {
            const auto& df = deviceUtoc.fragments[devChain[k]];
            const auto& lr = logRanges[k];
            const std::int64_t dUnits = df.end.toLinearGroup() - df.start.toLinearGroup();
            const std::int64_t lUnits = lr.endLin - lr.startLin;
            if (dUnits != lUnits || audioModeFromUtocMode(df.mode) != lr.codec) {
                topologyMatches = false;
                break;
            }
        }
        devSlotsByTrack[t] = std::move(devChain);
    }

    if (!topologyMatches) {
        return false;
    }

    logicalUtoc.sec0Padding = deviceUtoc.sec0Padding;
    logicalUtoc.deviceSignature =
        deviceUtoc.deviceSignature != 0 ? deviceUtoc.deviceSignature : kCartaUtocSignature;
    logicalUtoc.discNonEmpty = deviceUtoc.discNonEmpty;
    logicalUtoc.trackCount = deviceUtoc.trackCount;
    logicalUtoc.sec0aPadding = deviceUtoc.sec0aPadding;
    logicalUtoc.nextFreeTrackSlot = deviceUtoc.nextFreeTrackSlot;
    logicalUtoc.trackMap = deviceUtoc.trackMap;
    logicalUtoc.fragments = deviceUtoc.fragments;
    for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
        const auto& trk = disc.tracks[t];
        const auto modeStr = mdCodecModeToString(effectiveTrackCodec(disc, trk));
        const std::uint8_t updatedMode =
            utocModeFromTrack(modeStr, trk.writeProtected, trk.scmsCopyProtected, trk.emphasis);
        for (std::uint8_t slot : devSlotsByTrack[t]) {
            logicalUtoc.fragments[slot].mode = updatedMode;
        }
    }
    return true;
}

bool tryRemapUtocFromRawDeviceSpans(const MdDisc& disc, const UtocDiscState& deviceUtoc,
                                    UtocDiscState& logicalUtoc, bool& outRemapped,
                                    std::vector<SpanUploadPlan>& outRecoveredPlans,
                                    std::string& outMismatchDetail, std::string& error) {
    outRemapped = false;
    outRecoveredPlans.clear();
    outMismatchDetail.clear();
    if (!deviceUtoc.hasSector0 || disc.tracks.empty()) {
        return true;
    }

    MdPhysicalPlan physPlan{};
    std::string planErr;
    if (!planMdPhysicalAudio(disc, physPlan, planErr) || physPlan.spans.empty()) {
        return true;
    }

    if (static_cast<std::size_t>(deviceUtoc.trackCount) != physPlan.spans.size()) {
        outMismatchDetail = "disc has " + std::to_string(deviceUtoc.trackCount) +
                            " track(s), project has " + std::to_string(physPlan.spans.size()) +
                            " physical span(s) / " + std::to_string(disc.tracks.size()) +
                            " logical track(s)";
        return true;
    }

    constexpr std::int64_t kMaxExtraPaddingUnits = 352;
    std::vector<SpanUploadPlan> rawPlans;
    rawPlans.reserve(physPlan.spans.size());

    for (std::size_t i = 0; i < physPlan.spans.size(); ++i) {
        const auto& span = physPlan.spans[i];
        const auto expectedModeStr = mdCodecModeToString(span.codec);
        std::vector<std::pair<UtocAddress, UtocAddress>> physRanges;
        std::uint8_t slot = deviceUtoc.trackMap[i + 1];
        std::set<std::uint8_t> visited;
        std::int64_t deviceUnits = 0;

        while (slot != 0 && visited.insert(slot).second && visited.size() <= 255) {
            const auto& frag = deviceUtoc.fragments[slot];
            if (!frag.isValid()) {
                outMismatchDetail = "span " + std::to_string(i + 1) +
                                    ": device fragment slot " + std::to_string(slot) +
                                    " is empty";
                return true;
            }
            if (frag.start.cluster < 50) {
                outMismatchDetail = "span " + std::to_string(i + 1) +
                                    ": device cluster " + std::to_string(frag.start.cluster) +
                                    " < 50";
                return true;
            }
            const std::int64_t sLin = frag.start.toLinearGroup();
            const std::int64_t eLin = frag.end.toLinearGroup();
            if (eLin < sLin) {
                outMismatchDetail = "span " + std::to_string(i + 1) +
                                    ": inverted device fragment range";
                return true;
            }
            const auto fragMode = audioModeFromUtocMode(frag.mode);
            if (fragMode != expectedModeStr) {
                outMismatchDetail = "span " + std::to_string(i + 1) + ": disc has " + fragMode +
                                    ", project expects " + expectedModeStr;
                return true;
            }
            deviceUnits += (eLin - sLin + 1);
            physRanges.emplace_back(frag.start, frag.end);
            slot = frag.link;
        }
        if (slot != 0) {
            outMismatchDetail = "span " + std::to_string(i + 1) +
                                ": cycle detected in device UTOC fragment chain";
            return true;
        }
        if (physRanges.empty()) {
            outMismatchDetail = "span " + std::to_string(i + 1) +
                                ": device track has no audio fragments";
            return true;
        }

        const std::int64_t expectedUnits = static_cast<std::int64_t>(span.unitCount);
        const std::int64_t expectedUploaded = expectedUploadedUnitsForSpan(span);
        const std::int64_t maxAllowedUnits =
            std::max(expectedUnits, expectedUploaded) + kMaxExtraPaddingUnits;
        if (deviceUnits < expectedUnits || deviceUnits > maxAllowedUnits) {
            outMismatchDetail = "span " + std::to_string(i + 1) + " (" + expectedModeStr +
                                "): disc has " + std::to_string(deviceUnits) +
                                " sound groups, expected " + std::to_string(expectedUnits) + ".." +
                                std::to_string(maxAllowedUnits);
            return true;
        }

        SpanUploadPlan up;
        up.spanIndex = static_cast<int>(i);
        up.codec = span.codec;
        up.modeName = expectedModeStr;
        up.modeCode = uploadModeCodeForCodec(span.codec);
        up.startSample = span.startSample;
        up.endSample = span.endSample;
        up.expectedUnits = span.unitCount;
        up.uploadedTrackIndex = static_cast<int>(i);
        up.physRanges = std::move(physRanges);
        rawPlans.push_back(std::move(up));
    }

    UtocDiscState remapped = logicalUtoc;
    if (!remapSector0WithPhysicalUploads(disc, physPlan, deviceUtoc, rawPlans, remapped, error)) {
        return false;
    }

    const int devicePhysTotal = physicalTotalClustersFromUtoc(deviceUtoc);
    const auto remappedBudget =
        computeUtocBudget(remapped, mdMediumToString(disc.medium), devicePhysTotal);
    if (!remappedBudget.fits()) {
        error = formatBudgetOverflow("Remapped physical UTOC exceeds disc limits", remappedBudget);
        return false;
    }

    logicalUtoc = std::move(remapped);
    outRecoveredPlans = std::move(rawPlans);
    outRemapped = true;
    return true;
}

bool tryApplyImportedPhysicalUtoc(const MdDisc& disc, const UtocDiscState& deviceUtoc,
                                  UtocDiscState& logicalUtoc) {
    const bool hasImportedHeaders =
        std::all_of(disc.importedUtocSectors.begin(), disc.importedUtocSectors.end(),
                    [](const auto& s) { return s.size() == kUtocSectorBytes; });
    if (!hasImportedHeaders || !deviceUtoc.hasSector0) {
        return false;
    }

    UtocDiscState importedUtoc{};
    std::string ignoredErr;
    if (!parseUtocSectors(disc.importedUtocSectors[0].data(), disc.importedUtocSectors[1].data(),
                          disc.importedUtocSectors[2].data(), disc.importedUtocSectors[3].data(),
                          importedUtoc, ignoredErr) ||
        !importedUtoc.hasSector0 || !validatePhysicalUtocClusters(importedUtoc, ignoredErr)) {
        return false;
    }

    UtocDiscState candidate = logicalUtoc;
    if (!tryPreserveDeviceSector0Topology(disc, importedUtoc, candidate)) {
        return false;
    }

    struct PhysInterval {
        std::int64_t startLin = 0;
        std::int64_t endLin = 0;
        std::string mode;
    };
    std::vector<PhysInterval> deviceIntervals;
    const int devTracks = std::min<int>(deviceUtoc.trackCount, kUtocMaxTracks);
    for (int t = 1; t <= devTracks; ++t) {
        std::uint8_t slot = deviceUtoc.trackMap[static_cast<std::size_t>(t)];
        std::set<std::uint8_t> visited;
        while (slot != 0 && visited.insert(slot).second && visited.size() <= 255) {
            const auto& f = deviceUtoc.fragments[slot];
            if (f.isValid() && f.end.toLinearGroup() >= f.start.toLinearGroup()) {
                deviceIntervals.push_back(
                    {f.start.toLinearGroup(), f.end.toLinearGroup(), audioModeFromUtocMode(f.mode)});
            }
            slot = f.link;
        }
    }
    if (deviceIntervals.empty()) {
        return false;
    }
    std::sort(deviceIntervals.begin(), deviceIntervals.end(),
              [](const PhysInterval& a, const PhysInterval& b) {
                  return std::tie(a.mode, a.startLin, a.endLin) <
                         std::tie(b.mode, b.startLin, b.endLin);
              });
    std::vector<PhysInterval> merged;
    for (const auto& iv : deviceIntervals) {
        if (!merged.empty() && merged.back().mode == iv.mode &&
            iv.startLin <= merged.back().endLin + 1) {
            merged.back().endLin = std::max(merged.back().endLin, iv.endLin);
        } else {
            merged.push_back(iv);
        }
    }

    const int candTracks = std::min<int>(candidate.trackCount, kUtocMaxTracks);
    if (candTracks <= 0) {
        return false;
    }
    for (int t = 1; t <= candTracks; ++t) {
        std::uint8_t slot = candidate.trackMap[static_cast<std::size_t>(t)];
        std::set<std::uint8_t> visited;
        int fragCount = 0;
        while (slot != 0 && visited.insert(slot).second && visited.size() <= 255) {
            const auto& f = candidate.fragments[slot];
            if (!f.isValid() || f.end.toLinearGroup() < f.start.toLinearGroup()) {
                return false;
            }
            const std::int64_t sLin = f.start.toLinearGroup();
            const std::int64_t eLin = f.end.toLinearGroup();
            const std::string mode = audioModeFromUtocMode(f.mode);
            const bool covered = std::any_of(merged.begin(), merged.end(), [&](const PhysInterval& m) {
                return m.mode == mode && sLin >= m.startLin && eLin <= m.endLin;
            });
            if (!covered) {
                return false;
            }
            ++fragCount;
            slot = f.link;
        }
        if (slot != 0 || fragCount == 0) {
            return false;
        }
    }

    logicalUtoc = std::move(candidate);
    return true;
}

std::vector<MdPhysRange> readUtocFreeRangesUnlocked(const UtocDiscState& utoc) {
    std::vector<MdPhysRange> ranges;
    std::uint8_t slot = utoc.trackMap[0];
    std::set<std::uint8_t> visited;
    while (slot != 0 && visited.insert(slot).second && visited.size() <= 255) {
        const auto& f = utoc.fragments[slot];
        if (f.isValid() && f.end.toLinearGroup() >= f.start.toLinearGroup()) {
            ranges.push_back({{f.start.cluster, f.start.sector, f.start.group},
                              {f.end.cluster, f.end.sector, f.end.group}});
        }
        slot = f.link;
    }
    return ranges;
}

int countUnusedUtocFragSlots(const UtocDiscState& utoc) {
    std::array<bool, kUtocSlots> used{};
    used[0] = true;
    auto markChain = [&](std::uint8_t head) {
        std::uint8_t slot = head;
        std::set<std::uint8_t> visited;
        while (slot != 0 && visited.insert(slot).second) {
            used[slot] = true;
            slot = utoc.fragments[slot].link;
        }
    };
    markChain(utoc.trackMap[0]);
    markChain(utoc.sec0aPadding[14]);
    const int n = std::min<int>(utoc.trackCount, kUtocMaxTracks);
    for (int t = 1; t <= n; ++t) {
        markChain(utoc.trackMap[static_cast<std::size_t>(t)]);
    }
    int unused = 0;
    for (std::size_t i = 1; i < kUtocSlots; ++i) {
        if (!used[i]) {
            ++unused;
        }
    }
    return unused;
}

MdDisc buildReclaimOnlyDisc(const MdDisc& disc) {
    MdDisc rd = disc;
    rd.tracks.clear();
    rd.groups.clear();

    std::vector<int> oldToNewTrack1(disc.tracks.size() + 1, 0);
    for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
        MdTrack tr = disc.tracks[t];
        tr.parts.clear();
        for (const auto& p : disc.tracks[t].parts) {
            if (findMdOnDiscRegionForPart(disc.loadedDisc, p) >= 0) {
                tr.parts.push_back(p);
            }
        }
        if (!tr.parts.empty()) {
            rd.tracks.push_back(std::move(tr));
            oldToNewTrack1[t + 1] = static_cast<int>(rd.tracks.size());
        }
    }

    for (const auto& g : disc.groups) {
        int newStart = 0;
        int newEnd = 0;
        for (int oldT = g.startTrack; oldT <= g.endTrack && oldT <= static_cast<int>(disc.tracks.size());
             ++oldT) {
            const int mapped = oldToNewTrack1[static_cast<std::size_t>(oldT)];
            if (mapped > 0) {
                if (newStart == 0) {
                    newStart = mapped;
                }
                newEnd = mapped;
            }
        }
        if (newStart > 0 && newEnd >= newStart) {
            MdGroup ng = g;
            ng.startTrack = newStart;
            ng.endTrack = newEnd;
            rd.groups.push_back(std::move(ng));
        }
    }
    return rd;
}

bool buildCombinedLoadedAndUploadedUtoc(const MdDisc& disc, const MdPhysicalPlan& physPlan,
                                        const UtocDiscState& logicalUtoc,
                                        const UtocDiscState& postUploadDeviceUtoc,
                                        const std::vector<SpanUploadPlan>& plans,
                                        int physTotalClusters, UtocDiscState& finalUtoc,
                                        std::string& error) {
    finalUtoc = logicalUtoc;
    finalUtoc.trackMap.fill(0);
    finalUtoc.sec0aPadding[14] = 0;
    for (std::size_t i = 0; i < kUtocSlots; ++i) {
        finalUtoc.fragments[i] = {};
    }
    for (std::uint8_t i = 1; i < 255; ++i) {
        finalUtoc.fragments[i].link = static_cast<std::uint8_t>(i + 1);
    }
    finalUtoc.fragments[255].link = 0;
    finalUtoc.nextFreeTrackSlot = 1;

    struct FragKey {
        std::uint16_t sc;
        std::uint8_t ss;
        std::uint8_t sg;
        std::uint8_t mode;
        std::uint16_t ec;
        std::uint8_t es;
        std::uint8_t eg;
        std::uint8_t link;
        bool operator<(const FragKey& o) const noexcept {
            return std::tie(sc, ss, sg, mode, ec, es, eg, link) <
                   std::tie(o.sc, o.ss, o.sg, o.mode, o.ec, o.es, o.eg, o.link);
        }
    };
    std::map<FragKey, std::uint8_t> fragPool;

    auto allocSlot = [&](const UtocFragment& frag, bool deduplicate,
                         std::uint8_t& outSlot) -> bool {
        if (deduplicate) {
            const FragKey k{frag.start.cluster, frag.start.sector, frag.start.group, frag.mode,
                            frag.end.cluster,   frag.end.sector,   frag.end.group,   frag.link};
            if (const auto it = fragPool.find(k); it != fragPool.end()) {
                outSlot = it->second;
                return true;
            }
        }
        if (finalUtoc.nextFreeTrackSlot == 0) {
            error = "UTOC Sector 0 fragment table exceeded 255 slots combining on-disc and new "
                    "tracks.";
            return false;
        }
        const std::uint8_t slot = finalUtoc.nextFreeTrackSlot;
        finalUtoc.nextFreeTrackSlot = finalUtoc.fragments[slot].link;
        finalUtoc.fragments[slot] = frag;
        if (deduplicate) {
            const FragKey k{frag.start.cluster, frag.start.sector, frag.start.group, frag.mode,
                            frag.end.cluster,   frag.end.sector,   frag.end.group,   frag.link};
            fragPool.emplace(k, slot);
        }
        outSlot = slot;
        return true;
    };

    // Preserve exact on-disc part mode bytes compiled into `logicalUtoc`.
    std::map<std::pair<int, int>, std::uint8_t> logicalModeByTrackPart;
    for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
        std::uint8_t lSlot = logicalUtoc.trackMap[t + 1];
        std::set<std::uint8_t> guard;
        int pIdx = 0;
        while (lSlot != 0 && guard.insert(lSlot).second) {
            logicalModeByTrackPart.emplace(std::make_pair(static_cast<int>(t), pIdx),
                                           logicalUtoc.fragments[lSlot].mode);
            lSlot = logicalUtoc.fragments[lSlot].link;
            ++pIdx;
        }
    }

    std::map<std::pair<int, int>, const MdPartPlacement*> placeByTrackPart;
    for (const auto& pl : physPlan.placements) {
        placeByTrackPart.emplace(std::make_pair(pl.trackIndex, pl.partIndex), &pl);
    }

    std::set<std::uint16_t> occupiedNowClusters;
    for (std::size_t t = 0; t < disc.tracks.size(); ++t) {
        const auto& trk = disc.tracks[t];
        const auto codec = effectiveTrackCodec(disc, trk);
        const std::uint8_t defaultMode = utocModeFromTrack(
            mdCodecModeToString(codec), trk.writeProtected, trk.scmsCopyProtected, trk.emphasis);

        struct TrackFragSpec {
            UtocAddress start{};
            UtocAddress end{};
            std::uint8_t mode = 0;
        };
        std::vector<TrackFragSpec> specs;

        for (std::size_t pIdx = 0; pIdx < trk.parts.size(); ++pIdx) {
            const auto plIt = placeByTrackPart.find({static_cast<int>(t), static_cast<int>(pIdx)});
            if (plIt == placeByTrackPart.end()) {
                error = "Missing physical placement for track " + std::to_string(t + 1) +
                        " part " + std::to_string(pIdx + 1) + ".";
                return false;
            }
            const auto* pl = plIt->second;
            if (pl->isOnDisc) {
                std::uint8_t modeByte = defaultMode;
                if (const auto mIt = logicalModeByTrackPart.find(
                        {static_cast<int>(t), static_cast<int>(pIdx)});
                    mIt != logicalModeByTrackPart.end()) {
                    modeByte = mIt->second;
                }
                const UtocAddress s{pl->startAddr.cluster, pl->startAddr.sector,
                                    pl->startAddr.group};
                const UtocAddress e{pl->endAddr.cluster, pl->endAddr.sector, pl->endAddr.group};
                specs.push_back({s, e, modeByte});
                for (std::uint16_t c = s.cluster; c <= e.cluster; ++c) {
                    occupiedNowClusters.insert(c);
                }
            } else {
                if (pl->spanIndex < 0 || pl->spanIndex >= static_cast<int>(plans.size())) {
                    error = "Physical span index out of range during UTOC combination.";
                    return false;
                }
                std::vector<std::pair<UtocAddress, UtocAddress>> partRanges;
                if (!sliceUnitSubrange(plans[static_cast<std::size_t>(pl->spanIndex)].physRanges,
                                       pl->unitOffsetInSpan, pl->unitCount, partRanges, error)) {
                    return false;
                }
                for (const auto& pr : partRanges) {
                    for (std::uint16_t c = pr.first.cluster; c <= pr.second.cluster; ++c) {
                        occupiedNowClusters.insert(c);
                    }
                    if (!specs.empty() && specs.back().mode == defaultMode &&
                        specs.back().end.toLinearGroup() + 1 == pr.first.toLinearGroup()) {
                        specs.back().end = pr.second;
                    } else {
                        specs.push_back({pr.first, pr.second, defaultMode});
                    }
                }
            }
        }

        std::uint8_t nextLink = 0;
        for (auto it = specs.rbegin(); it != specs.rend(); ++it) {
            UtocFragment frag;
            frag.start = it->start;
            frag.end = it->end;
            frag.mode = it->mode;
            frag.link = nextLink;
            std::uint8_t assigned = 0;
            if (!allocSlot(frag, true, assigned)) {
                return false;
            }
            nextLink = assigned;
        }
        finalUtoc.trackMap[t + 1] = nextLink;
    }

    auto countFreeSlots = [&]() {
        int cnt = 0;
        std::set<std::uint8_t> seen;
        for (std::uint8_t s = finalUtoc.nextFreeTrackSlot; s != 0 && seen.insert(s).second;
             s = finalUtoc.fragments[s].link) {
            ++cnt;
        }
        return cnt;
    };

    // Restore P-DFA defective cluster ranges verbatim.
    std::set<std::uint16_t> defectiveClusters;
    for (const auto& d : disc.loadedDisc.defectiveRanges) {
        for (std::uint16_t c = d.start.cluster; c <= d.end.cluster; ++c) {
            defectiveClusters.insert(c);
            occupiedNowClusters.insert(c);
        }
    }
    if (!disc.loadedDisc.defectiveRanges.empty()) {
        if (countFreeSlots() < static_cast<int>(disc.loadedDisc.defectiveRanges.size())) {
            error = "UTOC Sector 0 fragment table exceeded 255 slots (insufficient space for "
                    "defective area P-DFA).";
            return false;
        }
        std::uint8_t prev = 0;
        for (const auto& d : disc.loadedDisc.defectiveRanges) {
            UtocFragment f;
            f.start = {d.start.cluster, d.start.sector, d.start.group};
            f.end = {d.end.cluster, d.end.sector, d.end.group};
            f.mode = 0;
            f.link = 0;
            std::uint8_t assigned = 0;
            if (!allocSlot(f, false, assigned)) {
                return false;
            }
            if (prev == 0) {
                finalUtoc.sec0aPadding[14] = assigned;
            } else {
                finalUtoc.fragments[prev].link = assigned;
            }
            prev = assigned;
        }
    }

    // Build final P-FRA (trackMap[0]): post-upload device P-FRA + whole reclaimed clusters not
    // occupied by kept on-disc tracks, newly uploaded tracks, or P-DFA.
    std::set<std::uint16_t> initiallyUsedClusters;
    for (const auto& reg : disc.loadedDisc.regions) {
        for (std::uint16_t c = reg.physStart.cluster; c <= reg.physEnd.cluster; ++c) {
            initiallyUsedClusters.insert(c);
        }
    }
    std::vector<std::pair<std::int64_t, std::int64_t>> freeIntervals;
    for (const auto& fr : readUtocFreeRangesUnlocked(postUploadDeviceUtoc)) {
        const UtocAddress s{fr.start.cluster, fr.start.sector, fr.start.group};
        const UtocAddress e{fr.end.cluster, fr.end.sector, fr.end.group};
        freeIntervals.emplace_back(s.toLinearGroup(), e.toLinearGroup());
    }
    std::set<std::uint16_t> reclaimedOnlyClusters;
    for (std::uint16_t c : initiallyUsedClusters) {
        if (!occupiedNowClusters.count(c) && !defectiveClusters.count(c)) {
            reclaimedOnlyClusters.insert(c);
        }
    }
    if (!reclaimedOnlyClusters.empty()) {
        auto it = reclaimedOnlyClusters.begin();
        std::uint16_t runStart = *it;
        std::uint16_t runEnd = *it;
        auto pushWholeRun = [&](std::uint16_t c0, std::uint16_t c1) {
            const UtocAddress a{c0, 0, 0};
            const UtocAddress b{c1, static_cast<std::uint8_t>(kMdSectorsPerCluster - 1),
                                static_cast<std::uint8_t>(kMdUnitsPerSector - 1)};
            freeIntervals.emplace_back(a.toLinearGroup(), b.toLinearGroup());
        };
        for (++it; it != reclaimedOnlyClusters.end(); ++it) {
            if (*it == runEnd + 1) {
                runEnd = *it;
            } else {
                pushWholeRun(runStart, runEnd);
                runStart = *it;
                runEnd = *it;
            }
        }
        pushWholeRun(runStart, runEnd);
    }

    std::sort(freeIntervals.begin(), freeIntervals.end());
    std::vector<MdPhysRange> mergedFree;
    for (const auto& iv : freeIntervals) {
        if (mergedFree.empty() || iv.first > mergedFree.back().end.toLinearUnits() + 1) {
            mergedFree.push_back(
                {MdPhysAddr::fromLinearUnits(iv.first), MdPhysAddr::fromLinearUnits(iv.second)});
        } else if (iv.second > mergedFree.back().end.toLinearUnits()) {
            mergedFree.back().end = MdPhysAddr::fromLinearUnits(iv.second);
        }
    }

    const int freeSlots = countFreeSlots();
    if (freeSlots > 0 && !mergedFree.empty()) {
        if (static_cast<int>(mergedFree.size()) > freeSlots) {
            std::sort(mergedFree.begin(), mergedFree.end(),
                      [](const MdPhysRange& a, const MdPhysRange& b) {
                          return (a.end.toLinearUnits() - a.start.toLinearUnits()) >
                                 (b.end.toLinearUnits() - b.start.toLinearUnits());
                      });
            mergedFree.resize(static_cast<std::size_t>(freeSlots));
            std::sort(mergedFree.begin(), mergedFree.end(),
                      [](const MdPhysRange& a, const MdPhysRange& b) { return a.start < b.start; });
        }
        std::uint8_t prev = 0;
        for (const auto& r : mergedFree) {
            UtocFragment f;
            f.start = {r.start.cluster, r.start.sector, r.start.group};
            f.end = {r.end.cluster, r.end.sector, r.end.group};
            f.mode = 0;
            f.link = 0;
            std::uint8_t assigned = 0;
            if (!allocSlot(f, false, assigned)) {
                return false;
            }
            if (prev == 0) {
                finalUtoc.trackMap[0] = assigned;
            } else {
                finalUtoc.fragments[prev].link = assigned;
            }
            prev = assigned;
        }
    }

    const auto budget =
        computeUtocBudget(finalUtoc, mdMediumToString(disc.medium), physTotalClusters);
    if (!budget.fits()) {
        error = formatBudgetOverflow("Combined physical UTOC exceeds disc limits", budget);
        return false;
    }
    return true;
}

NetMdPreflightReport evaluateNetMdPreflightReport(const Session& session, const MdDisc& disc,
                                                   bool deviceConnected,
                                                   const NetMdDeviceStatus& deviceStatus,
                                                   const std::string& devErr) {
    NetMdPreflightReport report;
    report.requiresForceTocEdit = true;
    report.logicalTracks = static_cast<int>(disc.tracks.size());
    report.hasLoadedDisc = disc.loadedDisc.active;
    report.deviceConnected = deviceConnected;
    report.device = deviceStatus;
    const int physTotal = disc.loadedDisc.active ? disc.loadedDisc.physicalTotalClusters() : 0;

    const auto medium = mdMediumToString(disc.medium);
    UtocDiscState utoc{};
    std::string compileErr;
    report.utocCompiled = compileMdDiscToUtoc(disc, utoc, compileErr);
    if (report.utocCompiled) {
        report.budget = computeUtocBudget(utoc, medium, physTotal);
    }

    MdPhysicalPlan physPlan{};
    std::string planErr;
    if (planMdPhysicalAudio(disc, physPlan, planErr)) {
        report.uniqueUploadSegments = static_cast<int>(physPlan.spans.size());
        report.onDiscClusters = physPlan.onDiscClusters;
        report.newUploadClusters = physPlan.newUploadClusters;
        report.reclaimedClusters = physPlan.reclaimedClusters;
        if (disc.loadedDisc.active) {
            for (const auto& tr : disc.tracks) {
                if (mdTrackUsesOnDiscAudio(disc, tr)) {
                    ++report.preservedOnDiscTracks;
                }
            }
            if (!physPlan.spans.empty() &&
                physPlan.initialFreeClusters < physPlan.newUploadClusters &&
                physPlan.freeClustersAfterReclaim >= physPlan.newUploadClusters) {
                report.needsPreUploadReclaim = true;
            }
        }
        for (const auto& s : physPlan.spans) {
            if (s.codec == MdCodecMode::spStereo || s.codec == MdCodecMode::spMono) {
                report.requiresSpUpload = true;
            }
        }
        validateTracksAgainstTimeline(disc, session, planErr);
    }

    std::vector<std::string> blockers;
    if (!report.utocCompiled) {
        blockers.push_back(compileErr.empty() ? "UTOC compilation failed" : compileErr);
    } else if (!report.budget.fits()) {
        blockers.push_back("Disc exceeds UTOC cluster or 255-slot table limits");
    }
    if (disc.loadedDisc.active && !physPlan.spans.empty() &&
        physPlan.freeClustersAfterReclaim < physPlan.newUploadClusters) {
        blockers.push_back("Not enough whole free clusters on disc after reclaiming deleted audio (" +
                           std::to_string(physPlan.freeClustersAfterReclaim) + " available, " +
                           std::to_string(physPlan.newUploadClusters) + " needed)");
    }
    if (!planErr.empty()) {
        blockers.push_back(planErr);
    }

    std::vector<std::string> recBlockers = blockers;
    if (!disc.loadedDisc.active && disc.tracks.empty()) {
        recBlockers.push_back("MD disc contains 0 tracks");
    }

    std::vector<std::string> utocBlockers;
    if (!report.utocCompiled) {
        utocBlockers.push_back(compileErr.empty() ? "UTOC compilation failed" : compileErr);
    } else if (!report.budget.fits()) {
        utocBlockers.push_back("UTOC exceeds 255-slot table limits");
    }
    if (disc.loadedDisc.active && !physPlan.spans.empty()) {
        utocBlockers.push_back("Added local audio spans require Record MiniDisc to upload audio");
    }

    if (!report.deviceConnected) {
        const auto msg = devErr.empty() ? "No USB NetMD / Hi-MD device connected" : devErr;
        recBlockers.push_back(msg);
        utocBlockers.push_back(msg);
        report.exploitBadge = "NetMD / Type-S homebrew bridge: standby";
    } else {
        if (!report.device.discPresent) {
            recBlockers.push_back("No MiniDisc inserted");
            utocBlockers.push_back("No MiniDisc inserted");
        } else {
            if (!report.device.writable) {
                recBlockers.push_back(
                    "Inserted MiniDisc is not writable (pre-mastered or read-only)");
                utocBlockers.push_back("Inserted MiniDisc is not writable");
            }
            if (report.device.writeProtected) {
                recBlockers.push_back("Inserted MiniDisc write-protect tab is locked");
                utocBlockers.push_back("Inserted MiniDisc write-protect tab is locked");
            }
            if (report.device.hiMdFormatted) {
                recBlockers.push_back(
                    "Inserted disc is Hi-MD formatted; click Erase Disc... first to reformat for "
                    "standard MiniDisc");
                utocBlockers.push_back(
                    "Inserted disc is Hi-MD formatted; erase or load after reformat");
            } else if (!disc.loadedDisc.active && report.device.trackCount > 0) {
                recBlockers.push_back(
                    "Inserted MiniDisc is not empty (" +
                    std::to_string(report.device.trackCount) +
                    " track(s)). Click Load Disc to edit non-destructively, or Erase Disc... first");
            }
        }
        if (!report.device.supportsForceTocEdit) {
            recBlockers.push_back("Firmware '" + report.device.versionCode +
                                  "' does not support ForceTOCEdit");
            utocBlockers.push_back("Firmware '" + report.device.versionCode +
                                   "' does not support ForceTOCEdit");
        }
        if (report.requiresSpUpload && !report.device.supportsSpUpload) {
            recBlockers.push_back(
                "ATRAC1 SP upload requires Type-S, Type-R, or supported Hi-MD firmware (got '" +
                report.device.versionCode + "')");
        }

        const bool isHiMd = !report.device.versionCode.empty() &&
                            report.device.versionCode.front() == 'H';
        const bool isTypeR = !report.device.versionCode.empty() &&
                             report.device.versionCode.front() == 'R';
        std::ostringstream badge;
        badge << "Firmware "
              << (report.device.versionCode.empty() ? "unknown" : report.device.versionCode)
              << "  |  ForceTOC: " << (report.device.supportsForceTocEdit ? "OK" : "NO")
              << "  |  SP-Upload: "
              << (report.device.supportsSpUpload
                      ? (isHiMd ? "Hi-MD OK" : (isTypeR ? "Type-R OK" : "Type-S OK"))
                      : (report.requiresSpUpload ? "REQUIRED (NO)" : "N/A"));
        report.exploitBadge = badge.str();
    }

    report.canWriteUtoc = utocBlockers.empty();
    report.canRecord = recBlockers.empty();
    report.ok = report.canRecord;

    if (!recBlockers.empty()) {
        report.blocker = recBlockers.front();
        report.statusText = "Blocked: " + report.blocker;
    } else if (disc.loadedDisc.active) {
        report.statusText =
            "Ready (preserve " + std::to_string(report.preservedOnDiscTracks) +
            " on-disc track(s), upload " + std::to_string(report.uniqueUploadSegments) +
            " new span(s), reclaim " + std::to_string(report.reclaimedClusters) + " cluster(s))";
    } else {
        report.statusText =
            "Ready (" + std::to_string(report.uniqueUploadSegments) + " physical upload" +
            (report.uniqueUploadSegments == 1 ? "" : "s") + " -> " +
            std::to_string(report.logicalTracks) + " UTOC track" +
            (report.logicalTracks == 1 ? "" : "s") + ")";
    }

    std::ostringstream sum;
    sum << "Target: " << medium << " / classic / " << mdCodecModeToString(disc.defaultCodec)
        << "  |  Tracks: " << report.logicalTracks
        << "  |  Physical uploads: " << report.uniqueUploadSegments << " (saved "
        << physPlan.savedClusters << " cl)"
        << "  |  Groups: " << disc.groups.size() << "\n";
    if (report.utocCompiled) {
        sum << "UTOC: " << report.budget.usedClusters << "/" << report.budget.totalClusters
            << " clusters, " << report.budget.usedTracks << "/" << report.budget.maxTracks
            << " tracks, " << report.budget.usedFragments << "/" << report.budget.maxFragments
            << " fragments (shared " << report.budget.sharedFragmentCount << "), "
            << report.budget.usedHalfWidthCells << "/" << report.budget.maxHalfWidthCells
            << " HW cells, " << report.budget.usedFullWidthCells << "/"
            << report.budget.maxFullWidthCells << " FW cells"
            << (report.budget.fits() ? " (FITS)" : " (OVER BUDGET)") << "\n";
    } else {
        sum << "UTOC error: " << compileErr << "\n";
    }
    if (report.deviceConnected) {
        sum << "Device: " << report.device.deviceName << " [" << report.device.deviceId
            << "] fw=" << (report.device.versionCode.empty() ? "?" : report.device.versionCode)
            << (report.device.discPresent
                    ? (std::string(report.device.writeProtected
                                       ? " (LOCKED)"
                                       : (report.device.hiMdFormatted
                                              ? " (Hi-MD formatted)"
                                              : (report.device.writable ? " (disc ready)"
                                                                        : " (read-only)"))) +
                       ", on-disc tracks=" + std::to_string(report.device.trackCount))
                    : " (no disc)")
            << ", ForceTOC=" << (report.device.supportsForceTocEdit ? "OK" : "NO") << ", SP="
            << (report.device.supportsSpUpload
                    ? ((!report.device.versionCode.empty() &&
                        report.device.versionCode.front() == 'H')
                           ? "Hi-MD"
                           : ((!report.device.versionCode.empty() &&
                               report.device.versionCode.front() == 'R')
                                  ? "Type-R"
                                  : "Type-S"))
                    : (report.requiresSpUpload ? "NO" : "N/A"));
    } else {
        sum << "Device: "
            << (devErr.empty() ? "connect a NetMD recorder via USB and click Refresh." : devErr);
    }
    if (!report.blocker.empty()) {
        sum << "\nPreflight check: " << report.blocker;
    }
    report.summary = sum.str();
    return report;
}

} // namespace

// ============================================================================
// Public NetMD Bridge Operations
// ============================================================================

bool listNetMdDevices(std::vector<NetMdDeviceEntry>& out, std::string& error) {
    const std::lock_guard<std::mutex> guard(netMdBridgeMutex());
    return listNetMdDevicesUnlocked(out, error);
}

bool probeNetMdDevice(const std::string& deviceId, NetMdDeviceStatus& out, std::string& error) {
    const std::lock_guard<std::mutex> guard(netMdBridgeMutex());
    return probeNetMdDeviceUnlocked(deviceId, out, error);
}

NetMdPreflightReport preflightNetMdRecording(const Session& session, const MdDisc& disc,
                                             const std::string& preferredDeviceId) {
    std::string targetId;
    std::string devErr;
    NetMdDeviceStatus deviceStatus{};
    bool deviceConnected = false;
    {
        const std::lock_guard<std::mutex> guard(netMdBridgeMutex());
        if (resolveTargetDeviceIdUnlocked(preferredDeviceId, targetId, devErr)) {
            if (probeNetMdDeviceUnlocked(targetId, deviceStatus, devErr)) {
                deviceConnected = true;
            }
        }
    }
    return evaluateNetMdPreflightReport(session, disc, deviceConnected, deviceStatus, devErr);
}

NetMdPreflightReport preflightNetMdRecordingWithStatus(const Session& session, const MdDisc& disc,
                                                       const NetMdDeviceStatus& deviceStatus) {
    return evaluateNetMdPreflightReport(session, disc, !deviceStatus.deviceId.empty(), deviceStatus,
                                        {});
}

bool eraseNetMdDisc(const std::string& deviceId, bool confirmHardware,
                    NetMdDeviceStatus& outAfterStatus, std::string& error) {
    outAfterStatus = {};
    error.clear();

    const std::lock_guard<std::mutex> guard(netMdBridgeMutex());

    std::string resolvedId;
    if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
        return false;
    }

    const bool needsConfirm = !isNetMdMockDeviceId(resolvedId) || isNetMdTestMockGuardEnabled();
    if (needsConfirm && !confirmHardware) {
        error = "Refusing to erase real NetMD hardware without explicit confirmation (pass "
                "confirm-hardware or confirm in the dialog).";
        return false;
    }
    ScopedNetMdProgress progressScope;
    setNetMdProgress("erase", "Probing NetMD device...", 0.08);

    NetMdDeviceStatus status;
    if (!probeNetMdDeviceUnlocked(resolvedId, status, error, /*includeTracks=*/false)) {
        return false;
    }
    if (!status.discPresent) {
        error = "No MiniDisc inserted in device '" + resolvedId + "'.";
        return false;
    }
    if (!status.writable || status.writeProtected) {
        error = "MiniDisc in device '" + resolvedId + "' is read-only or write-protected.";
        return false;
    }

    setNetMdProgress("erase", "Erasing MiniDisc (this may take 10-20s)...", 0.30);
    HelperProgressWindow eraseWin;
    eraseWin.active = true;
    eraseWin.operation = "erase";
    eraseWin.fallbackPhase = "Erasing MiniDisc (this may take 10-20s)...";
    eraseWin.baseProgress = 0.25;
    eraseWin.spanProgress = 0.55;
    if (!eraseDeviceUnlocked(resolvedId, confirmHardware, eraseWin, error)) {
        return false;
    }
    clearCachedPhysicalSpanMap(resolvedId);

    setNetMdProgress("erase", "Refreshing physical disc status...", 0.85);
    if (!probeNetMdDeviceUnlocked(resolvedId, outAfterStatus, error)) {
        outAfterStatus = status;
        outAfterStatus.trackCount = 0;
        outAfterStatus.hiMdFormatted = false;
        error.clear();
    }
    return true;
}

bool readNetMdDiscUtoc(const std::string& deviceId,
                       std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5>& rawSectors,
                       UtocDiscState& outState, std::string& error) {
    for (auto& s : rawSectors) {
        s.fill(0);
    }
    outState = {};
    error.clear();

    const std::lock_guard<std::mutex> guard(netMdBridgeMutex());

    std::string resolvedId;
    if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
        return false;
    }

    ScopedNetMdProgress progressScope;
    setNetMdProgress("read-utoc", "Probing NetMD device...", 0.04);

    NetMdDeviceStatus status;
    if (!probeNetMdDeviceUnlocked(resolvedId, status, error, /*includeTracks=*/false)) {
        return false;
    }
    if (!status.discPresent) {
        error = "No MiniDisc inserted in device '" + resolvedId + "'.";
        return false;
    }

    setNetMdProgress("read-utoc", "Reading UTOC Sectors 0/1/2/4 from MiniDisc...", 0.12);
    HelperProgressWindow readWin;
    readWin.active = true;
    readWin.operation = "read-utoc";
    readWin.fallbackPhase = "Reading UTOC Sectors 0/1/2/4 from MiniDisc...";
    readWin.baseProgress = 0.12;
    readWin.spanProgress = 0.85;
    return readDeviceUtocSectorsUnlocked(resolvedId, rawSectors, outState, readWin, error);
}

bool loadNetMdDisc(const std::string& deviceId, MdDisc& outDisc, NetMdDeviceStatus* outStatus,
                   std::string& error) {
    outDisc = {};
    if (outStatus != nullptr) {
        *outStatus = {};
    }
    error.clear();

    const std::lock_guard<std::mutex> guard(netMdBridgeMutex());

    std::string resolvedId;
    if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
        return false;
    }

    ScopedNetMdProgress progressScope;
    setNetMdProgress("load-disc", "Probing NetMD device...", 0.04);

    NetMdDeviceStatus status;
    if (!probeNetMdDeviceUnlocked(resolvedId, status, error, /*includeTracks=*/false)) {
        return false;
    }
    if (!status.discPresent) {
        error = "No MiniDisc inserted in device '" + resolvedId + "'.";
        return false;
    }
    if (status.hiMdFormatted) {
        error = "Inserted disc is Hi-MD formatted; click Erase Disc... first to reformat for "
                "standard MiniDisc.";
        return false;
    }

    setNetMdProgress("load-disc", "Reading UTOC Sectors 0/1/2/3/4 from MiniDisc...", 0.12);
    HelperProgressWindow readWin;
    readWin.active = true;
    readWin.operation = "load-disc";
    readWin.fallbackPhase = "Reading UTOC Sectors 0/1/2/3/4 from MiniDisc...";
    readWin.baseProgress = 0.12;
    readWin.spanProgress = 0.80;
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> rawSectors{};
    UtocDiscState deviceUtoc{};
    if (!readDeviceUtocSectorsUnlocked(resolvedId, rawSectors, deviceUtoc, readWin, error)) {
        return false;
    }

    setNetMdProgress("load-disc", "Decoding MiniDisc UTOC & timeline layout...", 0.94);
    if (!loadMdDiscFromDeviceUtoc(rawSectors, resolvedId, outDisc, error)) {
        return false;
    }
    if (outStatus != nullptr) {
        status.trackCount = static_cast<int>(outDisc.tracks.size());
        status.discTitle = outDisc.halfWidthTitle;
        status.fullWidthTitle = outDisc.fullWidthTitle;
        status.tracks.clear();
        status.tracks.reserve(outDisc.tracks.size());
        for (std::size_t i = 0; i < outDisc.tracks.size(); ++i) {
            const auto& trk = outDisc.tracks[i];
            NetMdDeviceTrack row;
            row.index = static_cast<int>(i);
            row.title = trk.halfWidthTitle;
            row.fullWidthTitle = trk.fullWidthTitle;
            switch (effectiveTrackCodec(outDisc, trk)) {
            case MdCodecMode::spStereo:
                row.codec = "SP";
                break;
            case MdCodecMode::spMono:
                row.codec = "Mono";
                break;
            case MdCodecMode::lp2:
                row.codec = "LP2";
                break;
            case MdCodecMode::lp4:
                row.codec = "LP4";
                break;
            case MdCodecMode::hiSp:
                row.codec = "Hi-SP";
                break;
            case MdCodecMode::hiLp:
                row.codec = "Hi-LP";
                break;
            case MdCodecMode::pcm:
                row.codec = "PCM";
                break;
            }
            row.durationSeconds =
                static_cast<int>(std::llround(static_cast<double>(trk.totalSamples()) /
                                              static_cast<double>(kMdSampleRate)));
            status.tracks.push_back(std::move(row));
        }
        *outStatus = status;
    }
    return true;
}

bool writeNetMdUtoc(
    const MdDisc& disc, const std::string& deviceId, bool confirmHardware, UtocBudget& outBudget,
    std::string& error, std::string* outCommittedSector0Hash,
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5>* outCommittedRawSectors) {
    outBudget = {};
    error.clear();
    if (outCommittedSector0Hash != nullptr) {
        outCommittedSector0Hash->clear();
    }
    if (outCommittedRawSectors != nullptr) {
        for (auto& s : *outCommittedRawSectors) {
            s.fill(0);
        }
    }

    const std::lock_guard<std::mutex> guard(netMdBridgeMutex());

    std::string resolvedId;
    if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
        return false;
    }

    const bool needsConfirm = !isNetMdMockDeviceId(resolvedId) || isNetMdTestMockGuardEnabled();
    if (needsConfirm && !confirmHardware) {
        error = "Refusing to modify real NetMD hardware without explicit confirmation (pass "
                "confirm-hardware or confirm in the dialog).";
        return false;
    }
    ScopedNetMdProgress progressScope;
    setNetMdProgress("write-utoc", "Validating UTOC & probing NetMD device...", 0.08);

    const auto medium = mdMediumToString(disc.medium);
    const int physTotal = disc.loadedDisc.active ? disc.loadedDisc.physicalTotalClusters() : 0;
    if (disc.loadedDisc.active) {
        MdPhysicalPlan physPlan{};
        if (!planMdPhysicalAudio(disc, physPlan, error)) {
            return false;
        }
        if (!physPlan.spans.empty()) {
            error = "Disc contains newly added local audio spans; use Record MiniDisc... to "
                    "upload audio before updating UTOC.";
            return false;
        }
    }

    UtocDiscState logicalUtoc{};
    if (!compileMdDiscToUtoc(disc, logicalUtoc, error)) {
        return false;
    }
    outBudget = computeUtocBudget(logicalUtoc, medium, physTotal);
    if (!outBudget.fits()) {
        error = "Compiled UTOC exceeds disc cluster or 255-slot table limits.";
        return false;
    }

    NetMdDeviceStatus status;
    if (!probeNetMdDeviceUnlocked(resolvedId, status, error, /*includeTracks=*/false)) {
        return false;
    }
    if (!status.discPresent) {
        error = "No MiniDisc inserted in device '" + resolvedId + "'.";
        return false;
    }
    if (!status.writable || status.writeProtected) {
        error = "MiniDisc in device '" + resolvedId + "' is read-only or write-protected.";
        return false;
    }
    if (status.hiMdFormatted) {
        error = "Inserted disc is Hi-MD formatted; click Erase Disc... first to reformat for "
                "standard MiniDisc.";
        return false;
    }
    if (!status.supportsForceTocEdit) {
        error = "Device firmware '" + status.versionCode + "' does not support ForceTOCEdit.";
        return false;
    }

    setNetMdProgress("write-utoc", "Reading device UTOC Sector 3 / headers...", 0.12);
    HelperProgressWindow readWin;
    readWin.active = true;
    readWin.operation = "write-utoc";
    readWin.fallbackPhase = "Reading device UTOC Sector 3 / headers...";
    readWin.baseProgress = 0.12;
    readWin.spanProgress = 0.14;
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> rawSectors{};
    UtocDiscState deviceUtoc{};
    if (!readDeviceUtocSectorsUnlocked(resolvedId, rawSectors, deviceUtoc, readWin, error)) {
        return false;
    }
    logDiagnostic("utoc", "writeNetMdUtoc device Sector 0: " + describeDeviceUtocSector0(deviceUtoc));

    if (disc.loadedDisc.active && !disc.loadedDisc.baselineSector0Hash.empty()) {
        const auto liveHash = computeUtocSectorSha256Hex(rawSectors[0].data());
        if (liveHash != disc.loadedDisc.baselineSector0Hash) {
            error = "Inserted MiniDisc UTOC changed on hardware since Load Disc; click Load Disc "
                    "again before writing UTOC.";
            logDiagnostic("utoc", "writeNetMdUtoc FAIL: " + error);
            return false;
        }
    }

    bool remappedFromHardware = false;
    bool importedHasPhysicalLayout = false;
    std::string resolutionMethod = "none";
    if (disc.loadedDisc.active) {
        std::string physCheckErr;
        importedHasPhysicalLayout = validatePhysicalUtocClusters(logicalUtoc, physCheckErr);
        if (!importedHasPhysicalLayout) {
            error = physCheckErr.empty()
                        ? "Compiled UTOC physical cluster layout failed verification."
                        : physCheckErr;
            logDiagnostic("utoc", "writeNetMdUtoc FAIL: " + error);
            return false;
        }
        resolutionMethod = "loadedDisc";
    } else {
        const bool hasImportedHeaders =
            std::all_of(disc.importedUtocSectors.begin(), disc.importedUtocSectors.end(),
                        [](const auto& s) { return s.size() == kUtocSectorBytes; });
        if (!hasImportedHeaders) {
            copyDeviceHeadersToUtoc(deviceUtoc, logicalUtoc);
        }

        if (!tryRemapUtocFromCachedSpans(disc, resolvedId, deviceUtoc, logicalUtoc,
                                         remappedFromHardware, error)) {
            logDiagnostic("utoc", "writeNetMdUtoc cached-span remap FAIL: " + error);
            return false;
        }
        if (remappedFromHardware) {
            resolutionMethod = "cachedSpans";
        }
        if (!remappedFromHardware) {
            remappedFromHardware = tryPreserveDeviceSector0Topology(disc, deviceUtoc, logicalUtoc);
            if (remappedFromHardware) {
                resolutionMethod = "preserveTopology";
            }
        }
        std::string rawMismatchDetail;
        if (!remappedFromHardware) {
            std::vector<SpanUploadPlan> recoveredPlans;
            if (!tryRemapUtocFromRawDeviceSpans(disc, deviceUtoc, logicalUtoc,
                                                remappedFromHardware, recoveredPlans,
                                                rawMismatchDetail, error)) {
                logDiagnostic("utoc", "writeNetMdUtoc raw-span remap FAIL: " + error);
                return false;
            }
            if (remappedFromHardware) {
                resolutionMethod = "rawDeviceSpans";
                MdPhysicalPlan cachePlan{};
                std::string cachePlanErr;
                if (planMdPhysicalAudio(disc, cachePlan, cachePlanErr)) {
                    storeCachedPhysicalSpanMap(resolvedId, cachePlan, recoveredPlans);
                }
            }
        }
        if (!remappedFromHardware && hasImportedHeaders) {
            importedHasPhysicalLayout =
                tryApplyImportedPhysicalUtoc(disc, deviceUtoc, logicalUtoc);
            if (importedHasPhysicalLayout) {
                resolutionMethod = "importedPhysicalUtoc";
            }
        }
        if (!remappedFromHardware &&
            (!isNetMdMockDeviceId(resolvedId) || isNetMdTestMockGuardEnabled()) &&
            !importedHasPhysicalLayout) {
            error = "Physical audio layout on MiniDisc does not match the current track spans; "
                    "use Record MiniDisc to upload audio first.";
            if (!rawMismatchDetail.empty()) {
                error += " (" + rawMismatchDetail + ")";
            }
            logDiagnostic("utoc", "writeNetMdUtoc FAIL: " + error);
            return false;
        }
        if (resolutionMethod == "none") {
            resolutionMethod = "mockBypass";
        }
    }
    logDiagnostic("utoc", "writeNetMdUtoc resolution=" + resolutionMethod +
                              " tracks=" + std::to_string(logicalUtoc.trackCount));

    setNetMdProgress("write-utoc", "Writing UTOC Sectors 0/1/2/4 & running TOCWRT...", 0.28);
    HelperProgressWindow writeWin;
    writeWin.active = true;
    writeWin.operation = "write-utoc";
    writeWin.fallbackPhase = "Writing UTOC Sectors 0/1/2/4 & running TOCWRT...";
    writeWin.baseProgress = 0.28;
    writeWin.spanProgress = 0.70;
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> committedRaw{};
    std::string committedHash;
    if (!writeDeviceUtocSectorsUnlocked(resolvedId, logicalUtoc, &rawSectors[3], confirmHardware,
                                        writeWin, error, &committedRaw, &committedHash)) {
        logDiagnostic("utoc", "writeNetMdUtoc commit FAIL: " + error);
        return false;
    }
    if (outCommittedSector0Hash != nullptr) {
        *outCommittedSector0Hash = committedHash;
    }
    if (outCommittedRawSectors != nullptr) {
        *outCommittedRawSectors = committedRaw;
    }
    const int effectivePhysTotal =
        physTotal > 0 ? physTotal : physicalTotalClustersFromUtoc(deviceUtoc);
    outBudget = computeUtocBudget(logicalUtoc, medium, effectivePhysTotal);
    return true;
}

bool recordNetMdDisc(const Session& session, const MdDisc& disc, const std::string& deviceId,
                     bool confirmHardware, NetMdRecordResult& outResult, std::string& error) {
    outResult = {};
    error.clear();

    if (!disc.loadedDisc.active && disc.tracks.empty()) {
        error = "MD disc has no tracks to record.";
        return false;
    }

    const auto medium = mdMediumToString(disc.medium);
    const int physTotal = disc.loadedDisc.active ? disc.loadedDisc.physicalTotalClusters() : 0;
    UtocDiscState logicalUtoc{};
    if (!compileMdDiscToUtoc(disc, logicalUtoc, error)) {
        return false;
    }
    const auto initialBudget = computeUtocBudget(logicalUtoc, medium, physTotal);
    if (!initialBudget.fits()) {
        error = "MD disc exceeds UTOC cluster or 255-slot limits.";
        return false;
    }

    MdPhysicalPlan physPlan{};
    if (!planMdPhysicalAudio(disc, physPlan, error)) {
        return false;
    }
    if (disc.loadedDisc.active && !physPlan.spans.empty() &&
        physPlan.freeClustersAfterReclaim < physPlan.newUploadClusters) {
        error = "Not enough whole free clusters on disc after reclaiming deleted tracks (need " +
                std::to_string(physPlan.newUploadClusters) + ", available " +
                std::to_string(physPlan.freeClustersAfterReclaim) + ").";
        return false;
    }

    std::string resolvedId;
    {
        const std::lock_guard<std::mutex> guard(netMdBridgeMutex());
        if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
            return false;
        }
    }

    const bool needsConfirm = !isNetMdMockDeviceId(resolvedId) || isNetMdTestMockGuardEnabled();
    if (needsConfirm && !confirmHardware) {
        error = "Refusing to modify real NetMD hardware without explicit confirmation (pass "
                "confirm-hardware or confirm in the dialog).";
        return false;
    }

    ScopedNetMdProgress progressScope;
    setNetMdProgress("record", "Preparing ATRAC encoders...", 0.02);

    std::vector<SpanUploadPlan> plans;
    if (!physPlan.spans.empty()) {
        if (!buildSpanUploadPlans(disc, physPlan, &session, plans, error)) {
            return false;
        }
    }
    if (!disc.loadedDisc.active && plans.empty()) {
        error = "MD disc contains no audio spans to upload.";
        return false;
    }

    bool requiresSpUpload = false;
    std::int64_t totalEncodedBytes = 0;
    for (const auto& p : plans) {
        if (p.modeCode == 0 || p.modeCode == 1) {
            requiresSpUpload = true;
        }
        totalEncodedBytes += static_cast<std::int64_t>(p.wireBytes.size());
    }

    const std::lock_guard<std::mutex> guard(netMdBridgeMutex());

    setNetMdProgress("record", "Probing NetMD device...", 0.25);
    NetMdDeviceStatus status;
    if (!probeNetMdDeviceUnlocked(resolvedId, status, error, /*includeTracks=*/false)) {
        return false;
    }
    if (!status.discPresent) {
        error = "No MiniDisc inserted in device '" + resolvedId + "'.";
        return false;
    }
    if (!status.writable || status.writeProtected) {
        error = "MiniDisc in device '" + resolvedId + "' is read-only or write-protected.";
        return false;
    }
    if (status.hiMdFormatted) {
        error = "Inserted disc is Hi-MD formatted; click Erase Disc... first to reformat for "
                "standard MiniDisc.";
        return false;
    }
    if (!status.supportsForceTocEdit) {
        error = "Device firmware '" + status.versionCode + "' does not support ForceTOCEdit.";
        return false;
    }
    if (requiresSpUpload && !status.supportsSpUpload) {
        error =
            "Raw ATRAC1 SP recording requires Type-S, Type-R, or supported Hi-MD firmware (got '" +
            status.versionCode + "').";
        return false;
    }

    // Path 1: Session was NOT loaded from an existing hardware disc.
    // Never silently erase non-empty hardware discs; require Load Disc or explicit Erase Disc.
    if (!disc.loadedDisc.active) {
        if (status.trackCount > 0) {
            error = "Inserted MiniDisc is not empty (" + std::to_string(status.trackCount) +
                    " track(s)). Click Load Disc to edit non-destructively, or click Erase "
                    "Disc... first to wipe the disc.";
            return false;
        }
        clearCachedPhysicalSpanMap(resolvedId);

        logDiagnostic("utoc", "record Path1 start: device=" + resolvedId + " medium=" + medium +
                                  " spans=" + std::to_string(plans.size()) +
                                  " logicalTracks=" + std::to_string(disc.tracks.size()) +
                                  " totalWireBytes=" + std::to_string(totalEncodedBytes));
        for (std::size_t idx = 0; idx < plans.size(); ++idx) {
            const auto& p = plans[idx];
            logDiagnostic("utoc", "record Path1 plan span=" + std::to_string(idx) +
                                      " mode=" + p.modeName +
                                      " samples=[" + std::to_string(p.startSample) + "," +
                                      std::to_string(p.endSample) + ")" +
                                      " expectedUnits=" + std::to_string(p.expectedUnits) +
                                      " wireBytes=" + std::to_string(p.wireBytes.size()));
        }

        const std::int64_t totalBytesDenom = std::max<std::int64_t>(1, totalEncodedBytes);
        std::int64_t sentSoFar = 0;
        for (std::size_t idx = 0; idx < plans.size(); ++idx) {
            auto& p = plans[idx];
            const auto spanWireBytes = static_cast<std::int64_t>(p.wireBytes.size());
            const double spanStartFrac =
                static_cast<double>(sentSoFar) / static_cast<double>(totalBytesDenom);
            const double spanWidthFrac =
                static_cast<double>(spanWireBytes) / static_cast<double>(totalBytesDenom);

            const std::string phaseLabel = "Uploading " + p.modeName + " span " +
                                           std::to_string(idx + 1) + "/" +
                                           std::to_string(plans.size()) + " (" + p.hwTitle + ")...";
            setNetMdProgress("record", phaseLabel, 0.30 + 0.50 * spanStartFrac, sentSoFar,
                             totalBytesDenom);

            HelperProgressWindow uploadWin;
            uploadWin.active = true;
            uploadWin.operation = "record";
            uploadWin.fallbackPhase = phaseLabel;
            uploadWin.preferFallbackPhase = true;
            uploadWin.baseProgress = 0.30 + 0.50 * spanStartFrac;
            uploadWin.spanProgress = 0.50 * spanWidthFrac;
            uploadWin.bytesOffset = sentSoFar;
            uploadWin.bytesSpan = spanWireBytes;
            uploadWin.bytesTotal = totalBytesDenom;

            int trkIdx = -1;
            if (!uploadSpanUnlocked(resolvedId, p.modeCode, p.wireBytes, p.hwTitle, p.fwTitle,
                                    confirmHardware, uploadWin, trkIdx, error)) {
                logDiagnostic("utoc", "record Path1 upload FAIL span=" + std::to_string(idx) +
                                          ": " + error);
                return false;
            }
            p.uploadedTrackIndex = trkIdx;
            sentSoFar += spanWireBytes;
            logDiagnostic("utoc", "record Path1 uploaded span=" + std::to_string(idx) +
                                      " -> hwTrack=" + std::to_string(trkIdx));
        }

        setNetMdProgress("record", "Reading physical UTOC allocation from disc...", 0.82);
        HelperProgressWindow readWin;
        readWin.active = true;
        readWin.operation = "record";
        readWin.fallbackPhase = "Reading physical UTOC allocation from disc...";
        readWin.baseProgress = 0.82;
        readWin.spanProgress = 0.06;
        std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> rawSectors{};
        UtocDiscState deviceUtoc{};
        if (!readDeviceUtocSectorsUnlocked(resolvedId, rawSectors, deviceUtoc, readWin, error)) {
            logDiagnostic("utoc", "record Path1 post-upload read-utoc FAIL: " + error);
            return false;
        }
        logDiagnostic("utoc", "record Path1 post-upload device Sector 0: " +
                                  describeDeviceUtocSector0(deviceUtoc));

        for (std::size_t idx = 0; idx < plans.size(); ++idx) {
            auto& p = plans[idx];
            if (!extractTrackFragmentRanges(deviceUtoc, p.uploadedTrackIndex, p.physRanges,
                                            error)) {
                logDiagnostic("utoc", "record Path1 extractTrackFragmentRanges FAIL span=" +
                                          std::to_string(idx) + " hwTrack=" +
                                          std::to_string(p.uploadedTrackIndex) + ": " + error);
                return false;
            }
            logDiagnostic("utoc", "record Path1 span=" + std::to_string(idx) +
                                      " hwTrack=" + std::to_string(p.uploadedTrackIndex) +
                                      " phys=" + formatUtocRangeSummary(p.physRanges) +
                                      " expectedUnits=" + std::to_string(p.expectedUnits));
        }

        // Cache extracted hardware span coordinates immediately so Write UTOC Only can recover in
        // session even if remapping or TOCWRT fails transiently.
        storeCachedPhysicalSpanMap(resolvedId, physPlan, plans);

        UtocDiscState finalUtoc = logicalUtoc;
        if (!remapSector0WithPhysicalUploads(disc, physPlan, deviceUtoc, plans, finalUtoc, error)) {
            logDiagnostic("utoc", "record Path1 remapSector0 FAIL: " + error);
            return false;
        }
        const int devicePhysTotal = physicalTotalClustersFromUtoc(deviceUtoc);
        const auto finalBudget = computeUtocBudget(finalUtoc, medium, devicePhysTotal);
        if (!finalBudget.fits()) {
            error = formatBudgetOverflow("Remapped physical UTOC exceeds disc limits", finalBudget);
            logDiagnostic("utoc", "record Path1 finalBudget FAIL: " + error);
            return false;
        }

        setNetMdProgress("record", "Writing final UTOC Sectors 0/1/2/4 & running TOCWRT...", 0.88);
        HelperProgressWindow writeWin;
        writeWin.active = true;
        writeWin.operation = "record";
        writeWin.fallbackPhase = "Writing final UTOC Sectors 0/1/2/4 & running TOCWRT...";
        writeWin.baseProgress = 0.88;
        writeWin.spanProgress = 0.11;
        std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> committedRaw{};
        std::string committedHash;
        if (!writeDeviceUtocSectorsUnlocked(resolvedId, finalUtoc, &rawSectors[3], confirmHardware,
                                            writeWin, error, &committedRaw, &committedHash)) {
            logDiagnostic("utoc", "record Path1 writeDeviceUtoc FAIL: " + error);
            return false;
        }

        outResult.deviceId = resolvedId;
        outResult.uploadedPhysicalSegments = static_cast<int>(plans.size());
        outResult.logicalTracks = static_cast<int>(disc.tracks.size());
        outResult.groups = static_cast<int>(disc.groups.size());
        outResult.encodedBytes = totalEncodedBytes;
        outResult.committedUtoc = true;
        outResult.committedRawSectors = committedRaw;
        outResult.hasCommittedRawSectors = true;
        outResult.committedSector0Hash = committedHash;
        outResult.budget = finalBudget;
        return true;
    }

    // Path 2: Non-destructive synchronization of a loaded hardware MiniDisc.
    setNetMdProgress("record", "Reading current device UTOC & verifying disc identity...", 0.27);
    HelperProgressWindow readInitialWin;
    readInitialWin.active = true;
    readInitialWin.operation = "record";
    readInitialWin.fallbackPhase = "Reading current device UTOC & verifying disc identity...";
    readInitialWin.baseProgress = 0.27;
    readInitialWin.spanProgress = 0.06;
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> rawSectors{};
    UtocDiscState deviceUtoc{};
    if (!readDeviceUtocSectorsUnlocked(resolvedId, rawSectors, deviceUtoc, readInitialWin, error)) {
        return false;
    }

    if (!disc.loadedDisc.baselineSector0Hash.empty()) {
        const auto liveHash = computeUtocSectorSha256Hex(rawSectors[0].data());
        if (liveHash != disc.loadedDisc.baselineSector0Hash) {
            error = "Inserted MiniDisc UTOC changed on hardware since Load Disc; click Load Disc "
                    "again to reload before recording.";
            return false;
        }
    }

    int preservedTracks = 0;
    for (const auto& tr : disc.tracks) {
        if (mdTrackUsesOnDiscAudio(disc, tr)) {
            ++preservedTracks;
        }
    }

    // Subpath 2A: UTOC-only edits (no newly added local audio spans).
    if (plans.empty()) {
        if (!validatePhysicalUtocClusters(logicalUtoc, error)) {
            return false;
        }
        setNetMdProgress("record", "Writing updated UTOC Sectors 0/1/2/4 & running TOCWRT...",
                         0.45);
        HelperProgressWindow writeWin;
        writeWin.active = true;
        writeWin.operation = "record";
        writeWin.fallbackPhase = "Writing updated UTOC Sectors 0/1/2/4 & running TOCWRT...";
        writeWin.baseProgress = 0.45;
        writeWin.spanProgress = 0.52;
        std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> committedRaw{};
        std::string committedHash;
        if (!writeDeviceUtocSectorsUnlocked(resolvedId, logicalUtoc, &rawSectors[3],
                                            confirmHardware, writeWin, error, &committedRaw,
                                            &committedHash)) {
            return false;
        }
        outResult.deviceId = resolvedId;
        outResult.uploadedPhysicalSegments = 0;
        outResult.logicalTracks = static_cast<int>(disc.tracks.size());
        outResult.groups = static_cast<int>(disc.groups.size());
        outResult.encodedBytes = 0;
        outResult.committedUtoc = true;
        outResult.preservedOnDiscTracks = preservedTracks;
        outResult.reclaimedClusters = physPlan.reclaimedClusters;
        outResult.needsReloadToFinishUpload = false;
        outResult.committedRawSectors = committedRaw;
        outResult.hasCommittedRawSectors = true;
        outResult.committedSector0Hash = committedHash;
        outResult.budget = initialBudget;
        return true;
    }

    // Subpath 2B: Mixed preserved on-disc audio + new local audio uploads.
    const auto liveFreeRanges = readUtocFreeRangesUnlocked(deviceUtoc);
    const int liveWholeFreeClusters = countWholeFreeClusters(liveFreeRanges);
    const int liveFreeTrackSlots = std::max(0, 255 - static_cast<int>(deviceUtoc.trackCount));
    const int liveFreeFragSlots = countUnusedUtocFragSlots(deviceUtoc);
    const int spansCount = static_cast<int>(plans.size());

    const bool needPreUploadReclaim = (liveWholeFreeClusters < physPlan.newUploadClusters) ||
                                      (liveFreeTrackSlots < spansCount) ||
                                      (liveFreeFragSlots < spansCount);
    if (needPreUploadReclaim) {
        const MdDisc reclaimDisc = buildReclaimOnlyDisc(disc);
        UtocDiscState reclaimUtoc{};
        if (!compileMdDiscToUtoc(reclaimDisc, reclaimUtoc, error)) {
            return false;
        }
        const int reclaimWholeFreeClusters =
            countWholeFreeClusters(readUtocFreeRangesUnlocked(reclaimUtoc));
        const int reclaimFreeTrackSlots =
            std::max(0, 255 - static_cast<int>(reclaimUtoc.trackCount));
        const int reclaimFreeFragSlots = countUnusedUtocFragSlots(reclaimUtoc);
        if (reclaimWholeFreeClusters < physPlan.newUploadClusters) {
            error = "Not enough whole free clusters after reclaiming deleted tracks (need " +
                    std::to_string(physPlan.newUploadClusters) + ", available " +
                    std::to_string(reclaimWholeFreeClusters) + ").";
            return false;
        }
        if (reclaimFreeTrackSlots < spansCount || reclaimFreeFragSlots < spansCount) {
            error = "Not enough UTOC track or fragment slots after reclaiming deleted tracks to "
                    "upload " +
                    std::to_string(spansCount) + " new span(s).";
            return false;
        }

        setNetMdProgress("record",
                         "Reclaiming deleted audio into P-FRA via UTOC write (Step 1 of 2)...",
                         0.40);
        HelperProgressWindow reclaimWin;
        reclaimWin.active = true;
        reclaimWin.operation = "record";
        reclaimWin.fallbackPhase =
            "Reclaiming deleted audio into P-FRA via UTOC write (Step 1 of 2)...";
        reclaimWin.baseProgress = 0.40;
        reclaimWin.spanProgress = 0.56;
        std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> committedRaw{};
        std::string committedHash;
        if (!writeDeviceUtocSectorsUnlocked(resolvedId, reclaimUtoc, &rawSectors[3],
                                            confirmHardware, reclaimWin, error, &committedRaw,
                                            &committedHash)) {
            return false;
        }

        outResult.deviceId = resolvedId;
        outResult.uploadedPhysicalSegments = 0;
        outResult.logicalTracks = static_cast<int>(reclaimDisc.tracks.size());
        outResult.groups = static_cast<int>(reclaimDisc.groups.size());
        outResult.encodedBytes = 0;
        outResult.committedUtoc = true;
        outResult.preservedOnDiscTracks = preservedTracks;
        outResult.reclaimedClusters = physPlan.reclaimedClusters;
        outResult.needsReloadToFinishUpload = true;
        outResult.nextActionHint =
            "Reclaimed " + std::to_string(physPlan.reclaimedClusters) +
            " deleted cluster(s) into disc free space (P-FRA). Please eject and reinsert the "
            "MiniDisc so the recorder refreshes its allocation table, then click Record "
            "MiniDisc... again to upload your new audio.";
        outResult.committedRawSectors = committedRaw;
        outResult.hasCommittedRawSectors = true;
        outResult.committedSector0Hash = committedHash;
        outResult.budget = computeUtocBudget(reclaimUtoc, medium, physTotal);
        return true;
    }

    if (status.trackCount != static_cast<int>(deviceUtoc.trackCount)) {
        error = "Device SRAM track count (" + std::to_string(status.trackCount) +
                ") differs from UTOC (" + std::to_string(deviceUtoc.trackCount) +
                "). Eject and reinsert the MiniDisc so the recorder refreshes its allocation table "
                "before uploading new audio.";
        return false;
    }

    const std::int64_t totalBytesDenom = std::max<std::int64_t>(1, totalEncodedBytes);
    std::int64_t sentSoFar = 0;
    for (std::size_t idx = 0; idx < plans.size(); ++idx) {
        auto& p = plans[idx];
        const auto spanWireBytes = static_cast<std::int64_t>(p.wireBytes.size());
        const double spanStartFrac =
            static_cast<double>(sentSoFar) / static_cast<double>(totalBytesDenom);
        const double spanWidthFrac =
            static_cast<double>(spanWireBytes) / static_cast<double>(totalBytesDenom);

        const std::string phaseLabel = "Uploading new " + p.modeName + " span " +
                                       std::to_string(idx + 1) + "/" +
                                       std::to_string(plans.size()) + " (" + p.hwTitle + ")...";
        setNetMdProgress("record", phaseLabel, 0.34 + 0.48 * spanStartFrac, sentSoFar,
                         totalBytesDenom);

        HelperProgressWindow uploadWin;
        uploadWin.active = true;
        uploadWin.operation = "record";
        uploadWin.fallbackPhase = phaseLabel;
        uploadWin.preferFallbackPhase = true;
        uploadWin.baseProgress = 0.34 + 0.48 * spanStartFrac;
        uploadWin.spanProgress = 0.48 * spanWidthFrac;
        uploadWin.bytesOffset = sentSoFar;
        uploadWin.bytesSpan = spanWireBytes;
        uploadWin.bytesTotal = totalBytesDenom;

        int trkIdx = -1;
        if (!uploadSpanUnlocked(resolvedId, p.modeCode, p.wireBytes, p.hwTitle, p.fwTitle,
                                confirmHardware, uploadWin, trkIdx, error)) {
            return false;
        }
        p.uploadedTrackIndex = trkIdx;
        sentSoFar += spanWireBytes;
    }

    setNetMdProgress("record", "Reading post-upload UTOC allocation from disc...", 0.82);
    HelperProgressWindow readPostWin;
    readPostWin.active = true;
    readPostWin.operation = "record";
    readPostWin.fallbackPhase = "Reading post-upload UTOC allocation from disc...";
    readPostWin.baseProgress = 0.82;
    readPostWin.spanProgress = 0.06;
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> postSectors{};
    UtocDiscState postDeviceUtoc{};
    if (!readDeviceUtocSectorsUnlocked(resolvedId, postSectors, postDeviceUtoc, readPostWin,
                                       error)) {
        logDiagnostic("utoc", "record Path2 post-upload read-utoc FAIL: " + error);
        return false;
    }
    logDiagnostic("utoc", "record Path2 post-upload device Sector 0: " +
                              describeDeviceUtocSector0(postDeviceUtoc));

    for (std::size_t idx = 0; idx < plans.size(); ++idx) {
        auto& p = plans[idx];
        if (!extractTrackFragmentRanges(postDeviceUtoc, p.uploadedTrackIndex, p.physRanges,
                                        error)) {
            logDiagnostic("utoc", "record Path2 extractTrackFragmentRanges FAIL span=" +
                                      std::to_string(idx) + " hwTrack=" +
                                      std::to_string(p.uploadedTrackIndex) + ": " + error);
            return false;
        }
        logDiagnostic("utoc", "record Path2 span=" + std::to_string(idx) +
                                  " hwTrack=" + std::to_string(p.uploadedTrackIndex) +
                                  " phys=" + formatUtocRangeSummary(p.physRanges) +
                                  " expectedUnits=" + std::to_string(p.expectedUnits));
    }

    const int postPhysTotal =
        physTotal > 0 ? physTotal : physicalTotalClustersFromUtoc(postDeviceUtoc);
    UtocDiscState finalUtoc = logicalUtoc;
    if (!buildCombinedLoadedAndUploadedUtoc(disc, physPlan, logicalUtoc, postDeviceUtoc, plans,
                                            postPhysTotal, finalUtoc, error)) {
        logDiagnostic("utoc", "record Path2 buildCombinedUtoc FAIL: " + error);
        return false;
    }
    const auto finalBudget = computeUtocBudget(finalUtoc, medium, postPhysTotal);
    if (!finalBudget.fits()) {
        error = formatBudgetOverflow("Combined loaded/uploaded UTOC exceeds disc limits",
                                     finalBudget);
        logDiagnostic("utoc", "record Path2 finalBudget FAIL: " + error);
        return false;
    }

    setNetMdProgress("record", "Writing combined UTOC Sectors 0/1/2/4 & running TOCWRT...", 0.88);
    HelperProgressWindow writeWin;
    writeWin.active = true;
    writeWin.operation = "record";
    writeWin.fallbackPhase = "Writing combined UTOC Sectors 0/1/2/4 & running TOCWRT...";
    writeWin.baseProgress = 0.88;
    writeWin.spanProgress = 0.11;
    std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5> committedRaw{};
    std::string committedHash;
    if (!writeDeviceUtocSectorsUnlocked(resolvedId, finalUtoc, &postSectors[3], confirmHardware,
                                        writeWin, error, &committedRaw, &committedHash)) {
        logDiagnostic("utoc", "record Path2 writeDeviceUtoc FAIL: " + error);
        return false;
    }

    outResult.deviceId = resolvedId;
    outResult.uploadedPhysicalSegments = static_cast<int>(plans.size());
    outResult.logicalTracks = static_cast<int>(disc.tracks.size());
    outResult.groups = static_cast<int>(disc.groups.size());
    outResult.encodedBytes = totalEncodedBytes;
    outResult.committedUtoc = true;
    outResult.preservedOnDiscTracks = preservedTracks;
    outResult.reclaimedClusters = physPlan.reclaimedClusters;
    outResult.needsReloadToFinishUpload = false;
    outResult.committedRawSectors = committedRaw;
    outResult.hasCommittedRawSectors = true;
    outResult.committedSector0Hash = committedHash;
    outResult.budget = finalBudget;
    return true;
}

// ============================================================================
// Out-of-Process Mock Test Session & Stack Selftest Controls
// ============================================================================

bool setNetMdTestMockSessionEnabled(bool enabled, std::string& error) {
    error.clear();
    const std::lock_guard<std::mutex> guard(netMdBridgeMutex());
    if (!enabled) {
        mockSession().stop();
        clearCachedPhysicalSpanMap({});
        return true;
    }
    return mockSession().start(error);
}

bool resetNetMdTestMockSession(std::string& error) {
    error.clear();
    const std::lock_guard<std::mutex> guard(netMdBridgeMutex());
    juce::var resObj;
    std::string raw;
    if (!runSessionHelperCommand("mock-reset", HelperProgressWindow{}, resObj, raw, error)) {
        return false;
    }
    clearCachedPhysicalSpanMap({});
    return true;
}

bool seedNetMdTestMockUtoc(
    const std::array<std::array<std::uint8_t, kUtocSectorBytes>, 5>& rawSectors,
    std::string& error) {
    error.clear();
    const std::lock_guard<std::mutex> guard(netMdBridgeMutex());

    std::vector<std::uint8_t> packed(5 * kUtocSectorBytes, 0);
    for (std::size_t s = 0; s < 5; ++s) {
        std::memcpy(packed.data() + s * kUtocSectorBytes, rawSectors[s].data(), kUtocSectorBytes);
    }
    juce::TemporaryFile tmp(".seed.utoc.bin");
    if (!tmp.getFile().replaceWithData(packed.data(), packed.size())) {
        error = "Failed writing temporary mock UTOC seed file";
        return false;
    }
    const std::string inPath = tmp.getFile().getFullPathName().toStdString();
    juce::var resObj;
    std::string raw;
    if (!runSessionHelperCommand("mock-seed-utoc\t" + percentEncode(inPath),
                                 HelperProgressWindow{}, resObj, raw, error)) {
        return false;
    }
    clearCachedPhysicalSpanMap({});
    return true;
}

bool setNetMdTestMockGuard(bool enabled, std::string& error) {
    error.clear();
    const std::lock_guard<std::mutex> guard(netMdBridgeMutex());
    juce::var resObj;
    std::string raw;
    if (!runSessionHelperCommand(std::string("mock-guard\t") + (enabled ? "1" : "0"),
                                 HelperProgressWindow{}, resObj, raw, error)) {
        return false;
    }
    g_mockGuardEnabled.store(enabled, std::memory_order_release);
    return true;
}

bool fetchNetMdTestMockSnapshot(const std::string& outDir, std::string& outJson,
                                std::string& error) {
    outJson.clear();
    error.clear();
    const std::lock_guard<std::mutex> guard(netMdBridgeMutex());
    juce::var resObj;
    return runSessionHelperCommand("mock-snapshot\t" + percentEncode(outDir),
                                   HelperProgressWindow{}, resObj, outJson, error);
}

bool runNetMdHelperStackSelftest(std::string& error) {
    error.clear();
    const std::lock_guard<std::mutex> guard(netMdBridgeMutex());
    juce::var resObj;
    std::string raw;
    return runOneShotHelperCommand({"selftest-stack"}, HelperProgressWindow{}, resObj, raw, error);
}

} // namespace carta
