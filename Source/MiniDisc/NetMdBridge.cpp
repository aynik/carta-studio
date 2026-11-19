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
    std::string rawLine;
    if (mockSession().isRunning()) {
        return runSessionHelperCommand(sessionLine, win, outResultObj, rawLine, error);
    }
    return runOneShotHelperCommand(cliArgs, win, outResultObj, rawLine, error);
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
    std::string rawLine;
    if (mockSession().isRunning()) {
        return runSessionHelperCommand(sessionLine, win, outResultObj, rawLine, error);
    }
    return runOneShotHelperCommand(cliArgs, win, outResultObj, rawLine, error);
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
                              std::string& error) {
    out = {};
    error.clear();
    if (deviceId.empty()) {
        error = "NetMD device ID is empty";
        return false;
    }

    juce::var resObj;
    if (!invokeNetMdHelperUnlocked({"probe", deviceId}, "probe\t" + percentEncode(deviceId),
                                   HelperProgressWindow{}, resObj, error)) {
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

bool writeDeviceUtocSectorsUnlocked(
    const std::string& deviceId, const UtocDiscState& utocState,
    const std::array<std::uint8_t, kUtocSectorBytes>* preservedSector3, bool confirmHardware,
    const HelperProgressWindow& win, std::string& error) {
    std::array<std::uint8_t, kUtocSectorBytes> sec0{};
    std::array<std::uint8_t, kUtocSectorBytes> sec1{};
    std::array<std::uint8_t, kUtocSectorBytes> sec2{};
    std::array<std::uint8_t, kUtocSectorBytes> sec4{};
    if (!compileUtocSectors(utocState, sec0, sec1, sec2, sec4, error)) {
        return false;
    }

    std::vector<std::uint8_t> packed(5 * kUtocSectorBytes, 0);
    std::memcpy(packed.data() + 0 * kUtocSectorBytes, sec0.data(), kUtocSectorBytes);
    std::memcpy(packed.data() + 1 * kUtocSectorBytes, sec1.data(), kUtocSectorBytes);
    std::memcpy(packed.data() + 2 * kUtocSectorBytes, sec2.data(), kUtocSectorBytes);
    if (preservedSector3 != nullptr) {
        std::memcpy(packed.data() + 3 * kUtocSectorBytes, preservedSector3->data(),
                    kUtocSectorBytes);
    }
    std::memcpy(packed.data() + 4 * kUtocSectorBytes, sec4.data(), kUtocSectorBytes);

    juce::TemporaryFile tmp(".utoc.bin");
    if (!tmp.getFile().replaceWithData(packed.data(), packed.size())) {
        error = "Failed writing temporary UTOC payload file";
        return false;
    }
    const std::string inPath = tmp.getFile().getFullPathName().toStdString();

    // Mask 0x17 = 23 = (1<<0)|(1<<1)|(1<<2)|(1<<4): commit Sectors 0, 1, 2, 4; preserve Sector 3.
    std::vector<std::string> cli = {"write-utoc", deviceId, "--in", inPath, "--mask", "23"};
    if (confirmHardware) {
        cli.push_back("--confirm-hardware");
    }
    const std::string sess = "write-utoc\t" + percentEncode(deviceId) + "\t" +
                             percentEncode(inPath) + "\t23\t" + (confirmHardware ? "1" : "0");
    juce::var resObj;
    return invokeNetMdHelperUnlocked(cli, sess, win, resObj, error);
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
        return false;
    }
    return true;
}

bool remapSector0WithPhysicalUploads(const MdDisc& disc, const MdPhysicalPlan& physPlan,
                                     const UtocDiscState& deviceUtoc,
                                     const std::vector<SpanUploadPlan>& plans,
                                     UtocDiscState& finalUtoc, std::string& error) {
    copyDeviceHeadersToUtoc(deviceUtoc, finalUtoc);
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
    NetMdPreflightReport report;
    report.requiresForceTocEdit = true;
    report.logicalTracks = static_cast<int>(disc.tracks.size());

    const auto medium = mdMediumToString(disc.medium);
    UtocDiscState utoc{};
    std::string compileErr;
    report.utocCompiled = compileMdDiscToUtoc(disc, utoc, compileErr);
    if (report.utocCompiled) {
        report.budget = computeUtocBudget(utoc, medium);
    }

    MdPhysicalPlan physPlan{};
    std::string planErr;
    if (planMdPhysicalAudio(disc, physPlan, planErr)) {
        report.uniqueUploadSegments = static_cast<int>(physPlan.spans.size());
        for (const auto& s : physPlan.spans) {
            if (s.codec == MdCodecMode::spStereo || s.codec == MdCodecMode::spMono) {
                report.requiresSpUpload = true;
            }
        }
        validateTracksAgainstTimeline(disc, session, planErr);
    }

    std::string targetId;
    std::string devErr;
    {
        const std::lock_guard<std::mutex> guard(netMdBridgeMutex());
        if (resolveTargetDeviceIdUnlocked(preferredDeviceId, targetId, devErr)) {
            if (probeNetMdDeviceUnlocked(targetId, report.device, devErr)) {
                report.deviceConnected = true;
            }
        }
    }

    std::vector<std::string> blockers;
    if (!report.utocCompiled) {
        blockers.push_back(compileErr.empty() ? "UTOC compilation failed" : compileErr);
    } else if (!report.budget.fits()) {
        blockers.push_back("Disc exceeds UTOC cluster or 255-slot table limits");
    }
    if (!planErr.empty()) {
        blockers.push_back(planErr);
    }

    std::vector<std::string> recBlockers = blockers;
    if (disc.tracks.empty()) {
        recBlockers.push_back("MD disc contains 0 tracks");
    }

    std::vector<std::string> utocBlockers;
    if (!report.utocCompiled) {
        utocBlockers.push_back(compileErr.empty() ? "UTOC compilation failed" : compileErr);
    } else if (!report.budget.fits()) {
        utocBlockers.push_back("UTOC exceeds 255-slot table limits");
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
                utocBlockers.push_back(
                    "Inserted disc is Hi-MD formatted; erase or record audio first");
            }
        }
        if (!report.device.supportsForceTocEdit) {
            recBlockers.push_back("Firmware '" + report.device.versionCode +
                                  "' does not support ForceTOCEdit");
            utocBlockers.push_back("Firmware '" + report.device.versionCode +
                                   "' does not support ForceTOCEdit");
        }
        if (report.requiresSpUpload && !report.device.supportsSpUpload) {
            recBlockers.push_back("ATRAC1 SP upload requires Type-S firmware (got '" +
                                  report.device.versionCode + "')");
        }

        std::ostringstream badge;
        badge << "Firmware "
              << (report.device.versionCode.empty() ? "unknown" : report.device.versionCode)
              << "  |  ForceTOC: " << (report.device.supportsForceTocEdit ? "OK" : "NO")
              << "  |  SP-Upload: "
              << (report.device.supportsSpUpload
                      ? "Type-S OK"
                      : (report.requiresSpUpload ? "REQUIRED (NO)" : "N/A"));
        report.exploitBadge = badge.str();
    }

    report.canWriteUtoc = utocBlockers.empty();
    report.canRecord = recBlockers.empty();
    report.ok = report.canRecord;

    if (!recBlockers.empty()) {
        report.blocker = recBlockers.front();
        report.statusText = "Blocked: " + report.blocker;
    } else {
        report.statusText =
            std::string(report.device.hiMdFormatted ? "Ready (reformats Hi-MD disc; " : "Ready (") +
            std::to_string(report.uniqueUploadSegments) + " physical upload" +
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
            << (report.device.supportsSpUpload ? "Type-S"
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
    if (!probeNetMdDeviceUnlocked(resolvedId, status, error)) {
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
    if (!probeNetMdDeviceUnlocked(resolvedId, status, error)) {
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

bool writeNetMdUtoc(const MdDisc& disc, const std::string& deviceId, bool confirmHardware,
                    UtocBudget& outBudget, std::string& error) {
    outBudget = {};
    error.clear();

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
    UtocDiscState logicalUtoc{};
    if (!compileMdDiscToUtoc(disc, logicalUtoc, error)) {
        return false;
    }
    outBudget = computeUtocBudget(logicalUtoc, medium);
    if (!outBudget.fits()) {
        error = "Compiled UTOC exceeds disc cluster or 255-slot table limits.";
        return false;
    }

    NetMdDeviceStatus status;
    if (!probeNetMdDeviceUnlocked(resolvedId, status, error)) {
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

    const bool hasImportedHeaders =
        std::all_of(disc.importedUtocSectors.begin(), disc.importedUtocSectors.end(),
                    [](const auto& s) { return s.size() == kUtocSectorBytes; });
    if (!hasImportedHeaders) {
        copyDeviceHeadersToUtoc(deviceUtoc, logicalUtoc);
    }

    bool remappedFromHardware = false;
    if (!tryRemapUtocFromCachedSpans(disc, resolvedId, deviceUtoc, logicalUtoc,
                                     remappedFromHardware, error)) {
        return false;
    }
    if (!remappedFromHardware) {
        remappedFromHardware = tryPreserveDeviceSector0Topology(disc, deviceUtoc, logicalUtoc);
    }

    if (!remappedFromHardware && !isNetMdMockDeviceId(resolvedId) && !hasImportedHeaders) {
        error = "Physical audio layout on MiniDisc does not match the current track spans; use "
                "Erase & Record MiniDisc to upload audio first.";
        return false;
    }

    setNetMdProgress("write-utoc", "Writing UTOC Sectors 0/1/2/4 & running TOCWRT...", 0.28);
    HelperProgressWindow writeWin;
    writeWin.active = true;
    writeWin.operation = "write-utoc";
    writeWin.fallbackPhase = "Writing UTOC Sectors 0/1/2/4 & running TOCWRT...";
    writeWin.baseProgress = 0.28;
    writeWin.spanProgress = 0.70;
    if (!writeDeviceUtocSectorsUnlocked(resolvedId, logicalUtoc, &rawSectors[3], confirmHardware,
                                        writeWin, error)) {
        return false;
    }
    outBudget = computeUtocBudget(logicalUtoc, medium);
    return true;
}

bool recordNetMdDisc(const Session& session, const MdDisc& disc, const std::string& deviceId,
                     bool confirmHardware, NetMdRecordResult& outResult, std::string& error) {
    outResult = {};
    error.clear();

    if (disc.tracks.empty()) {
        error = "MD disc has no tracks to record.";
        return false;
    }

    const auto medium = mdMediumToString(disc.medium);
    UtocDiscState logicalUtoc{};
    if (!compileMdDiscToUtoc(disc, logicalUtoc, error)) {
        return false;
    }
    const auto initialBudget = computeUtocBudget(logicalUtoc, medium);
    if (!initialBudget.fits()) {
        error = "MD disc exceeds UTOC cluster or 255-slot limits.";
        return false;
    }

    MdPhysicalPlan physPlan{};
    if (!planMdPhysicalAudio(disc, physPlan, error)) {
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
        error = "Refusing to erase/record real NetMD hardware without explicit confirmation (pass "
                "confirm-hardware or confirm in the dialog).";
        return false;
    }

    ScopedNetMdProgress progressScope;
    setNetMdProgress("record", "Preparing ATRAC encoders...", 0.02);

    std::vector<SpanUploadPlan> plans;
    if (!buildSpanUploadPlans(disc, physPlan, &session, plans, error)) {
        return false;
    }
    if (plans.empty()) {
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

    setNetMdProgress("record", "Probing NetMD device...", 0.30);
    NetMdDeviceStatus status;
    if (!probeNetMdDeviceUnlocked(resolvedId, status, error)) {
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
    if (!status.supportsForceTocEdit) {
        error = "Device firmware '" + status.versionCode + "' does not support ForceTOCEdit.";
        return false;
    }
    if (requiresSpUpload && !status.supportsSpUpload) {
        error =
            "Raw ATRAC1 SP recording requires Type-S firmware (got '" + status.versionCode + "').";
        return false;
    }

    setNetMdProgress("record", "Erasing MiniDisc before recording...", 0.32);
    HelperProgressWindow eraseWin;
    eraseWin.active = true;
    eraseWin.operation = "record";
    eraseWin.fallbackPhase = "Erasing MiniDisc before recording...";
    eraseWin.preferFallbackPhase = true;
    eraseWin.baseProgress = 0.31;
    eraseWin.spanProgress = 0.03;
    if (!eraseDeviceUnlocked(resolvedId, confirmHardware, eraseWin, error)) {
        return false;
    }
    clearCachedPhysicalSpanMap(resolvedId);

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
        return false;
    }

    for (auto& p : plans) {
        if (!extractTrackFragmentRanges(deviceUtoc, p.uploadedTrackIndex, p.physRanges, error)) {
            return false;
        }
    }

    UtocDiscState finalUtoc = logicalUtoc;
    if (!remapSector0WithPhysicalUploads(disc, physPlan, deviceUtoc, plans, finalUtoc, error)) {
        return false;
    }
    const auto finalBudget = computeUtocBudget(finalUtoc, medium);
    if (!finalBudget.fits()) {
        error = "Remapped physical UTOC exceeds disc cluster or 255-slot limits.";
        return false;
    }

    setNetMdProgress("record", "Writing final UTOC Sectors 0/1/2/4 & running TOCWRT...", 0.88);
    HelperProgressWindow writeWin;
    writeWin.active = true;
    writeWin.operation = "record";
    writeWin.fallbackPhase = "Writing final UTOC Sectors 0/1/2/4 & running TOCWRT...";
    writeWin.baseProgress = 0.88;
    writeWin.spanProgress = 0.11;
    if (!writeDeviceUtocSectorsUnlocked(resolvedId, finalUtoc, &rawSectors[3], confirmHardware,
                                        writeWin, error)) {
        return false;
    }

    {
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

    outResult.deviceId = resolvedId;
    outResult.uploadedPhysicalSegments = static_cast<int>(plans.size());
    outResult.logicalTracks = static_cast<int>(disc.tracks.size());
    outResult.groups = static_cast<int>(disc.groups.size());
    outResult.encodedBytes = totalEncodedBytes;
    outResult.committedUtoc = true;
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
