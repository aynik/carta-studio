/**
 * @file HiMdBridge.cpp
 * Out-of-process `carta-himd` helper launcher, NDJSON event parser, persistent mock session
 * manager, and high-level Hi-MD recording/dumping/editing workflows.
 */

#include "HiMdBridge.h"
#include "NativeCodecs.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace carta {
namespace {

constexpr std::int64_t kHiMdMetadataReserveBytes = 65536LL;
constexpr std::int64_t kHiMdPerTrackMetaBytes = 32768LL;

struct HiMdProgressState {
    std::mutex mutex;
    HiMdProgressSnapshot snapshot;
};

HiMdProgressState& progressState() {
    static HiMdProgressState state;
    return state;
}

struct HelperProgressWindow {
    bool active = false;
    std::string operation;
    std::string fallbackPhase;
    double baseProgress = 0.0;
    double spanProgress = 1.0;
};

class ScopedHiMdProgress final {
  public:
    ~ScopedHiMdProgress() { clearHiMdProgress(); }
};

class TempFileRemover final {
  public:
    explicit TempFileRemover(juce::File f) : file(std::move(f)) {}
    ~TempFileRemover() {
        if (file != juce::File{} && file.existsAsFile()) {
            file.deleteFile();
        }
    }
    juce::File file;
};

std::mutex& hiMdBridgeMutex() {
    static std::mutex m;
    return m;
}

std::string& helperOverrideStorage() {
    static std::string s;
    return s;
}

bool& mockGuardStorage() {
    static bool armed = false;
    return armed;
}

std::atomic<bool>& elevationPromptEnabledStorage() {
    static std::atomic<bool> enabled{false};
    return enabled;
}

bool containsCaseInsensitive(const std::string& hay, const char* needle) noexcept {
    if (!needle || needle[0] == '\0') {
        return false;
    }
    const std::size_t nLen = std::strlen(needle);
    if (hay.size() < nLen) {
        return false;
    }
    for (std::size_t i = 0; i + nLen <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < nLen; ++j) {
            if (std::tolower(static_cast<unsigned char>(hay[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

enum class HiMdElevationMode { Auto, Off, Direct, Sudo };

HiMdElevationMode readElevationMode() {
    const juce::String rawStr =
        juce::SystemStats::getEnvironmentVariable("CARTA_HIMD_ELEVATION", {});
    if (rawStr.isEmpty()) {
        return HiMdElevationMode::Auto;
    }
    std::string v = rawStr.toStdString();
    for (char& ch : v) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (v == "off" || v == "0" || v == "never" || v == "none") {
        return HiMdElevationMode::Off;
    }
    if (v == "direct") {
        return HiMdElevationMode::Direct;
    }
    if (v == "sudo") {
        return HiMdElevationMode::Sudo;
    }
    return HiMdElevationMode::Auto;
}

std::string generateSessionTokenHex() {
    auto& rng = juce::Random::getSystemRandom();
    std::string out;
    out.reserve(32);
    static constexpr char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 4; ++i) {
        const auto v = static_cast<std::uint32_t>(rng.nextInt());
        for (int shift = 28; shift >= 0; shift -= 4) {
            out.push_back(kHex[(v >> shift) & 0x0Fu]);
        }
    }
    return out;
}

#if !defined(_WIN32)
std::string quotePosixShellArg(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}
#endif

#if defined(__APPLE__)
std::string escapeAppleScriptString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}
#endif

#if defined(_WIN32)
std::string quoteWinCmdArg(const std::string& arg) {
    std::string quoted = "\"";
    int bsCount = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++bsCount;
        } else if (c == '"') {
            quoted.append(static_cast<std::size_t>(bsCount * 2 + 1), '\\');
            quoted.push_back('"');
            bsCount = 0;
        } else {
            quoted.append(static_cast<std::size_t>(bsCount), '\\');
            quoted.push_back(c);
            bsCount = 0;
        }
    }
    quoted.append(static_cast<std::size_t>(bsCount * 2), '\\');
    quoted.push_back('"');
    return quoted;
}
#endif

std::string percentEncodeField(const std::string& input) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() + 8);
    for (char ch : input) {
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

bool locateHiMdHelperUnlocked(std::string& outPath, std::string& error) {
    outPath.clear();
#if defined(_WIN32)
    static constexpr const char* kHelperName = "carta-himd.exe";
#else
    static constexpr const char* kHelperName = "carta-himd";
#endif

    const juce::String envPath =
        juce::SystemStats::getEnvironmentVariable("CARTA_HIMD_HELPER", {});
    if (envPath.isNotEmpty()) {
        const juce::File f(envPath);
        if (f.existsAsFile()) {
            outPath = f.getFullPathName().toStdString();
            return true;
        }
        error = "CARTA_HIMD_HELPER points to a missing file: " + envPath.toStdString();
        return false;
    }

    if (!helperOverrideStorage().empty()) {
        const juce::File f(juce::String::fromUTF8(helperOverrideStorage().c_str()));
        if (f.existsAsFile()) {
            outPath = f.getFullPathName().toStdString();
            return true;
        }
        error = "Hi-MD helper override path does not exist: " + helperOverrideStorage();
        return false;
    }

    const auto exeFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto exeDir = exeFile.getParentDirectory();
    std::vector<juce::File> candidates;
#if defined(__APPLE__)
    candidates.push_back(exeDir.getParentDirectory().getChildFile("Helpers").getChildFile(kHelperName));
#endif
    candidates.push_back(exeDir.getChildFile(kHelperName));
    candidates.push_back(exeDir.getParentDirectory().getChildFile(kHelperName));

    juce::File probeDir = exeDir;
    for (int depth = 0; depth < 4; ++depth) {
        candidates.push_back(probeDir.getChildFile(kHelperName));
        candidates.push_back(probeDir.getChildFile("build").getChildFile(kHelperName));
        const auto parent = probeDir.getParentDirectory();
        if (parent == probeDir) {
            break;
        }
        probeDir = parent;
    }

    for (const auto& candidate : candidates) {
        if (candidate.existsAsFile()) {
            outPath = candidate.getFullPathName().toStdString();
            return true;
        }
    }

    error = "Could not locate carta-himd helper executable (checked Contents/Helpers, sibling "
            "directories, and CARTA_HIMD_HELPER).";
    return false;
}

void publishHelperProgressLine(const juce::DynamicObject* obj, const HelperProgressWindow* window) {
    if (!obj) {
        return;
    }
    const auto op = obj->getProperty("operation").toString().toStdString();
    const auto phase = obj->getProperty("phase").toString().toStdString();
    const double rawProg = static_cast<double>(obj->getProperty("progress"));
    const auto done =
        static_cast<std::int64_t>(static_cast<juce::int64>(obj->getProperty("bytes_done")));
    const auto total =
        static_cast<std::int64_t>(static_cast<juce::int64>(obj->getProperty("bytes_total")));

    if (window && window->active) {
        const double scaled =
            window->baseProgress + std::max(0.0, std::min(1.0, rawProg)) * window->spanProgress;
        setHiMdProgress(window->operation.empty() ? op : window->operation,
                        phase.empty() ? window->fallbackPhase : phase, scaled, done, total);
    } else {
        setHiMdProgress(op, phase, rawProg, done, total);
    }
}

class PersistentMockHiMdSession final {
  public:
    ~PersistentMockHiMdSession() { stop(); }

    bool ensureStarted(const std::string& helperPath, std::string& error) {
        if (stdinFile_ != nullptr && stdoutFile_ != nullptr && loadedHelperPath_ == helperPath) {
            return true;
        }
        stop();

#if defined(_WIN32)
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE inRead = nullptr;
        HANDLE inWrite = nullptr;
        HANDLE outRead = nullptr;
        HANDLE outWrite = nullptr;
        if (!CreatePipe(&inRead, &inWrite, &sa, 0) || !CreatePipe(&outRead, &outWrite, &sa, 0)) {
            if (inRead) CloseHandle(inRead);
            if (inWrite) CloseHandle(inWrite);
            if (outRead) CloseHandle(outRead);
            if (outWrite) CloseHandle(outWrite);
            error = "CreatePipe failed starting carta-himd mock session.";
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
        const juce::String cmdStr =
            "\"" + juce::String::fromUTF8(helperPath.c_str()) + "\" session --mock";
        std::vector<wchar_t> cmdBuf(cmdStr.length() + 1u, L'\0');
        cmdStr.copyToUTF16(cmdBuf.data(), static_cast<std::size_t>(cmdBuf.size() * sizeof(wchar_t)));

        const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(inRead);
        CloseHandle(outWrite);
        if (!ok) {
            CloseHandle(inWrite);
            CloseHandle(outRead);
            error = "Failed launching carta-himd mock session process.";
            return false;
        }
        CloseHandle(pi.hThread);
        hProc_ = pi.hProcess;
        const int inFd = _open_osfhandle(reinterpret_cast<intptr_t>(inWrite), _O_WRONLY | _O_TEXT);
        const int outFd = _open_osfhandle(reinterpret_cast<intptr_t>(outRead), _O_RDONLY | _O_TEXT);
        stdinFile_ = inFd >= 0 ? _fdopen(inFd, "w") : nullptr;
        stdoutFile_ = outFd >= 0 ? _fdopen(outFd, "r") : nullptr;
        if (!stdinFile_ || !stdoutFile_) {
            if (!stdinFile_ && inFd < 0) CloseHandle(inWrite);
            if (!stdoutFile_ && outFd < 0) CloseHandle(outRead);
            stop();
            error = "Failed opening stdio handles for carta-himd mock session.";
            return false;
        }
#else
        std::signal(SIGPIPE, SIG_IGN);
        int inPipe[2]{-1, -1};
        int outPipe[2]{-1, -1};
        if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0) {
            if (inPipe[0] >= 0) ::close(inPipe[0]);
            if (inPipe[1] >= 0) ::close(inPipe[1]);
            if (outPipe[0] >= 0) ::close(outPipe[0]);
            if (outPipe[1] >= 0) ::close(outPipe[1]);
            error = "pipe() failed starting carta-himd mock session.";
            return false;
        }

        const pid_t child = ::fork();
        if (child < 0) {
            ::close(inPipe[0]);
            ::close(inPipe[1]);
            ::close(outPipe[0]);
            ::close(outPipe[1]);
            error = "fork() failed starting carta-himd mock session.";
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
        pid_ = child;
        stdinFile_ = ::fdopen(inPipe[1], "w");
        stdoutFile_ = ::fdopen(outPipe[0], "r");
        if (!stdinFile_ || !stdoutFile_) {
            stop();
            error = "Failed wrapping pipes for carta-himd mock session.";
            return false;
        }
#endif
        loadedHelperPath_ = helperPath;
        return true;
    }

    void stop() {
        if (stdinFile_ != nullptr) {
            std::fputs("quit\n", stdinFile_);
            std::fflush(stdinFile_);
            std::fclose(stdinFile_);
            stdinFile_ = nullptr;
        }
        if (stdoutFile_ != nullptr) {
            std::fclose(stdoutFile_);
            stdoutFile_ = nullptr;
        }
#if defined(_WIN32)
        if (hProc_ != nullptr) {
            WaitForSingleObject(hProc_, 5000);
            CloseHandle(hProc_);
            hProc_ = nullptr;
        }
#else
        if (pid_ > 0) {
            int st = 0;
            ::waitpid(pid_, &st, 0);
            pid_ = -1;
        }
#endif
        loadedHelperPath_.clear();
    }

    bool isRunning() const noexcept { return stdinFile_ != nullptr && stdoutFile_ != nullptr; }

    bool send(const std::vector<std::string>& fields, const HelperProgressWindow* window,
              juce::var& outResult, std::string& error) {
        outResult = juce::var();
        if (!stdinFile_ || !stdoutFile_) {
            error = "Hi-MD mock session is not running.";
            return false;
        }

        std::string line;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) {
                line.push_back('\t');
            }
            line += (i == 0 ? fields[i] : percentEncodeField(fields[i]));
        }
        line.push_back('\n');

        if (std::fputs(line.c_str(), stdinFile_) == EOF || std::fflush(stdinFile_) != 0) {
            stop();
            error = "Failed writing command to carta-himd mock session.";
            return false;
        }

        std::string buf;
        char chunk[4096];
        while (std::fgets(chunk, sizeof(chunk), stdoutFile_) != nullptr) {
            buf += chunk;
            if (buf.empty() || buf.back() != '\n') {
                continue;
            }
            while (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r')) {
                buf.pop_back();
            }
            if (buf.empty()) {
                continue;
            }
            juce::var parsed;
            if (juce::JSON::parse(juce::String::fromUTF8(buf.c_str()), parsed).failed() ||
                !parsed.isObject()) {
                error = "Malformed NDJSON line from carta-himd session: " + buf;
                return false;
            }
            const auto* obj = parsed.getDynamicObject();
            const auto ev = obj->getProperty("event").toString();
            if (ev == "progress") {
                publishHelperProgressLine(obj, window);
                buf.clear();
                continue;
            }
            if (ev == "result") {
                outResult = parsed;
                if (!static_cast<bool>(obj->getProperty("ok"))) {
                    error = obj->getProperty("error").toString().toStdString();
                    if (error.empty()) {
                        error = "carta-himd mock session command failed.";
                    }
                    return false;
                }
                error.clear();
                return true;
            }
            buf.clear();
        }

        stop();
        error = "carta-himd mock session closed unexpectedly.";
        return false;
    }

  private:
#if defined(_WIN32)
    HANDLE hProc_ = nullptr;
#else
    pid_t pid_ = -1;
#endif
    std::FILE* stdinFile_ = nullptr;
    std::FILE* stdoutFile_ = nullptr;
    std::string loadedHelperPath_;
};

PersistentMockHiMdSession& persistentMockSession() {
    static PersistentMockHiMdSession sess;
    return sess;
}

bool& mockSessionEnabledFlag() {
    static bool enabled = false;
    return enabled;
}

bool runOneShotHelper(const std::string& helperPath, const std::vector<std::string>& args,
                      const HelperProgressWindow* window, juce::var& outResult,
                      std::string& error) {
    outResult = juce::var();
    std::FILE* stdoutFile = nullptr;

#if defined(_WIN32)
    HANDLE hProc = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        error = "CreatePipe failed starting carta-himd.";
        return false;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    auto quoteArg = [](const std::string& arg) -> std::string {
        std::string quoted = "\"";
        int bsCount = 0;
        for (char c : arg) {
            if (c == '\\') {
                ++bsCount;
            } else if (c == '"') {
                quoted.append(static_cast<std::size_t>(bsCount * 2 + 1), '\\');
                quoted.push_back('"');
                bsCount = 0;
            } else {
                quoted.append(static_cast<std::size_t>(bsCount), '\\');
                quoted.push_back(c);
                bsCount = 0;
            }
        }
        quoted.append(static_cast<std::size_t>(bsCount * 2), '\\');
        quoted.push_back('"');
        return quoted;
    };

    std::string cmdLine = quoteArg(helperPath);
    for (const auto& a : args) {
        cmdLine.push_back(' ');
        cmdLine += quoteArg(a);
    }

    const juce::String wideCmd = juce::String::fromUTF8(cmdLine.c_str());
    std::vector<wchar_t> cmdBuf(wideCmd.length() + 1u, L'\0');
    wideCmd.copyToUTF16(cmdBuf.data(), static_cast<std::size_t>(cmdBuf.size() * sizeof(wchar_t)));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = outWrite;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &si, &pi);
    CloseHandle(outWrite);
    if (!ok) {
        CloseHandle(outRead);
        error = "Failed launching carta-himd helper process.";
        return false;
    }
    CloseHandle(pi.hThread);
    hProc = pi.hProcess;
    const int outFd = _open_osfhandle(reinterpret_cast<intptr_t>(outRead), _O_RDONLY | _O_TEXT);
    stdoutFile = outFd >= 0 ? _fdopen(outFd, "r") : nullptr;
    if (!stdoutFile) {
        if (outFd < 0) CloseHandle(outRead);
        WaitForSingleObject(hProc, INFINITE);
        CloseHandle(hProc);
        error = "Failed reading stdout from carta-himd.";
        return false;
    }
#else
    int outPipe[2]{-1, -1};
    if (::pipe(outPipe) != 0) {
        error = "pipe() failed starting carta-himd.";
        return false;
    }
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        error = "fork() failed starting carta-himd.";
        return false;
    }
    if (child == 0) {
        ::dup2(outPipe[1], STDOUT_FILENO);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(helperPath.c_str()));
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(helperPath.c_str(), argv.data());
        ::_exit(127);
    }
    ::close(outPipe[1]);
    stdoutFile = ::fdopen(outPipe[0], "r");
    if (!stdoutFile) {
        ::close(outPipe[0]);
        int st = 0;
        ::waitpid(child, &st, 0);
        error = "fdopen() failed reading carta-himd stdout.";
        return false;
    }
#endif

    std::string rawOutput;
    bool gotResult = false;
    bool resultOk = false;
    std::string resultError;
    std::string buf;
    char chunk[4096];
    while (std::fgets(chunk, sizeof(chunk), stdoutFile) != nullptr) {
        buf += chunk;
        if (buf.empty() || buf.back() != '\n') {
            continue;
        }
        while (!buf.empty() && (buf.back() == '\n' || buf.back() == '\r')) {
            buf.pop_back();
        }
        if (!buf.empty()) {
            if (!rawOutput.empty()) {
                rawOutput.push_back('\n');
            }
            rawOutput += buf;
            juce::var parsed;
            if (!juce::JSON::parse(juce::String::fromUTF8(buf.c_str()), parsed).failed() &&
                parsed.isObject()) {
                const auto* obj = parsed.getDynamicObject();
                const auto ev = obj->getProperty("event").toString();
                if (ev == "progress") {
                    publishHelperProgressLine(obj, window);
                } else if (ev == "result") {
                    outResult = parsed;
                    gotResult = true;
                    resultOk = static_cast<bool>(obj->getProperty("ok"));
                    resultError = obj->getProperty("error").toString().toStdString();
                }
            }
        }
        buf.clear();
    }
    std::fclose(stdoutFile);

    int exitCode = -1;
#if defined(_WIN32)
    WaitForSingleObject(hProc, INFINITE);
    DWORD winExit = 1;
    if (GetExitCodeProcess(hProc, &winExit)) {
        exitCode = static_cast<int>(winExit);
    }
    CloseHandle(hProc);
#else
    int st = 0;
    if (::waitpid(child, &st, 0) > 0 && WIFEXITED(st)) {
        exitCode = WEXITSTATUS(st);
    }
#endif

    if (gotResult) {
        if (!resultOk) {
            error = resultError.empty() ? "carta-himd helper returned an error." : resultError;
            return false;
        }
        error.clear();
        return true;
    }

    error = rawOutput.empty()
                ? ("carta-himd exited with code " + std::to_string(exitCode) +
                   " without JSON result.")
                : rawOutput;
    return false;
}

bool invokeHiMdUnlocked(const std::vector<std::string>& sessionFields,
                        const std::vector<std::string>& cliArgs, bool preferMockSession,
                        bool allowElevationPrompt, const HelperProgressWindow* window,
                        juce::var& outResult, std::string& error);

class PersistentElevatedHiMdSession final {
  public:
    ~PersistentElevatedHiMdSession() { stop(); }

    bool isRunning() const noexcept { return activeFlag_.load(std::memory_order_acquire); }
    bool isMockMode() const noexcept { return mockMode_.load(std::memory_order_acquire); }

    bool ensureStarted(const std::string& helperPath, bool enableMock, bool allowPrompt,
                       std::string& error) {
        if (isRunning() && loadedHelperPath_ == helperPath && mockMode_.load() == enableMock) {
            return true;
        }
        stop();

        const auto mode = readElevationMode();
        if (mode == HiMdElevationMode::Off) {
            error = "Hi-MD administrator elevation is disabled (CARTA_HIMD_ELEVATION=off).";
            return false;
        }
        if (mode == HiMdElevationMode::Auto && (!allowPrompt || !isHiMdElevationPromptEnabled())) {
            error = "Administrator privileges required for Hi-MD USB access.";
            return false;
        }

        const std::string tokenHex = generateSessionTokenHex();
        static std::atomic<std::uint64_t> sessSeq{0};
        const auto seq = ++sessSeq;
        const auto stamp = static_cast<unsigned long long>(juce::Time::getHighResolutionTicks());

        const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory);
        const juce::File tokenFile = tempDir.getChildFile(
            "carta_himd_tok_" + juce::String(static_cast<juce::int64>(stamp)) + "_" +
            juce::String(static_cast<juce::int64>(seq)) + ".txt");
        if (!tokenFile.replaceWithText(juce::String::fromUTF8(tokenHex.c_str()), false, false,
                                       "\n")) {
            error = "Failed creating temporary Hi-MD authentication token file.";
            return false;
        }
        TempFileRemover tokenCleanup(tokenFile);
        const std::string tokenPath = tokenFile.getFullPathName().toStdString();

#if defined(_WIN32)
        const std::string pipeName =
            "\\\\.\\pipe\\carta-himd-" +
            std::to_string(static_cast<unsigned long>(GetCurrentProcessId())) + "-" +
            std::to_string(static_cast<unsigned long long>(seq));
        const std::string parentPidStr =
            std::to_string(static_cast<unsigned long>(GetCurrentProcessId()));

        std::string params = "session";
        if (enableMock) {
            params += " --mock";
        }
        params += " --pipe " + quoteWinCmdArg(pipeName) + " --token-file " +
                  quoteWinCmdArg(tokenPath) + " --parent-pid " + parentPidStr;

        if (mode == HiMdElevationMode::Direct) {
            const juce::String cmdStr =
                juce::String::fromUTF8((quoteWinCmdArg(helperPath) + " " + params).c_str());
            std::vector<wchar_t> cmdBuf(static_cast<std::size_t>(cmdStr.length()) + 1u, L'\0');
            cmdStr.copyToUTF16(cmdBuf.data(), cmdBuf.size() * sizeof(wchar_t));
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                nullptr, nullptr, &si, &pi)) {
                error = "Failed launching direct carta-himd pipe session.";
                return false;
            }
            CloseHandle(pi.hThread);
            hDirectProc_ = pi.hProcess;
        } else {
            const juce::String wideFile = juce::String::fromUTF8(helperPath.c_str());
            const juce::String wideParams = juce::String::fromUTF8(params.c_str());
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
            sei.lpVerb = L"runas";
            sei.lpFile = wideFile.toWideCharPointer();
            sei.lpParameters = wideParams.toWideCharPointer();
            sei.nShow = SW_HIDE;
            if (!ShellExecuteExW(&sei)) {
                const DWORD errCode = GetLastError();
                error = (errCode == ERROR_CANCELLED)
                            ? "Hi-MD administrator authorization was canceled."
                            : ("Failed launching elevated carta-himd session (error " +
                               std::to_string(errCode) + ").");
                return false;
            }
            if (sei.hProcess != nullptr) {
                CloseHandle(sei.hProcess);
            }
        }

        const juce::String widePipe = juce::String::fromUTF8(pipeName.c_str());
        const auto connectDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        HANDLE hPipe = INVALID_HANDLE_VALUE;
        while (std::chrono::steady_clock::now() < connectDeadline) {
            if (hDirectProc_ != nullptr && WaitForSingleObject(hDirectProc_, 0) != WAIT_TIMEOUT) {
                error = "Direct carta-himd pipe session exited before becoming ready.";
                stop();
                return false;
            }
            hPipe = CreateFileW(widePipe.toWideCharPointer(), GENERIC_READ | GENERIC_WRITE, 0,
                                nullptr, OPEN_EXISTING, 0, nullptr);
            if (hPipe != INVALID_HANDLE_VALUE) {
                break;
            }
            const DWORD errCode = GetLastError();
            if (errCode == ERROR_PIPE_BUSY) {
                (void)WaitNamedPipeW(widePipe.toWideCharPointer(), 100);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if (hPipe == INVALID_HANDLE_VALUE) {
            error = "Timed out waiting for elevated Hi-MD session pipe.";
            stop();
            return false;
        }
        hPipe_ = hPipe;
        pipeName_ = pipeName;
        carryBuf_.clear();
        loadedHelperPath_ = helperPath;
        mockMode_.store(enableMock, std::memory_order_release);
        activeFlag_.store(true, std::memory_order_release);

        juce::var authRes;
        if (!send({"auth", tokenHex}, nullptr, authRes, error)) {
            stop();
            if (error.empty()) {
                error = "Elevated Hi-MD session failed token authentication.";
            }
            return false;
        }
        return true;
#else
        std::signal(SIGPIPE, SIG_IGN);
        (void)::chmod(tokenPath.c_str(), 0600);

        const uid_t myUid = ::getuid();
        const gid_t myGid = ::getgid();
        const pid_t myPid = ::getpid();
        const std::string sockPath =
            "/tmp/carta-himd-" + std::to_string(static_cast<unsigned long>(myUid)) + "-" +
            std::to_string(static_cast<long long>(myPid)) + "-" +
            std::to_string(static_cast<unsigned long long>(seq)) + ".sock";
        (void)::unlink(sockPath.c_str());

        const std::string uidStr = std::to_string(static_cast<unsigned long>(myUid));
        const std::string gidStr = std::to_string(static_cast<unsigned long>(myGid));
        const std::string pidStr = std::to_string(static_cast<long long>(myPid));

        std::vector<std::string> baseArgs{helperPath,
                                          "session",
                                          "--socket",
                                          sockPath,
                                          "--client-uid",
                                          uidStr,
                                          "--client-gid",
                                          gidStr,
                                          "--token-file",
                                          tokenPath,
                                          "--parent-pid",
                                          pidStr};
        if (enableMock) {
            baseArgs.push_back("--mock");
        }

        if (mode == HiMdElevationMode::Direct || mode == HiMdElevationMode::Sudo) {
            std::vector<std::string> execArgs;
            if (mode == HiMdElevationMode::Sudo) {
                execArgs.push_back("/usr/bin/sudo");
                execArgs.push_back("-n");
            }
            execArgs.insert(execArgs.end(), baseArgs.begin(), baseArgs.end());

            const pid_t child = ::fork();
            if (child < 0) {
                error = "fork() failed launching Hi-MD session.";
                return false;
            }
            if (child == 0) {
                const int devNull = ::open("/dev/null", O_RDWR);
                if (devNull >= 0) {
                    ::dup2(devNull, STDIN_FILENO);
                    ::dup2(devNull, STDOUT_FILENO);
                    ::dup2(devNull, STDERR_FILENO);
                    if (devNull > STDERR_FILENO) {
                        ::close(devNull);
                    }
                }
                std::vector<char*> cArgs;
                cArgs.reserve(execArgs.size() + 1u);
                for (auto& s : execArgs) {
                    cArgs.push_back(const_cast<char*>(s.c_str()));
                }
                cArgs.push_back(nullptr);
                ::execv(cArgs[0], cArgs.data());
                ::_exit(127);
            }
            directPid_ = child;
        } else {
            std::string shellCmd;
            for (std::size_t i = 0; i < baseArgs.size(); ++i) {
                if (i > 0) {
                    shellCmd.push_back(' ');
                }
                shellCmd += quotePosixShellArg(baseArgs[i]);
            }
            shellCmd += " </dev/null >/dev/null 2>&1 &";

            int errPipe[2]{-1, -1};
            if (::pipe(errPipe) != 0) {
                error = "pipe() failed launching Hi-MD authorization prompt.";
                return false;
            }
            const pid_t child = ::fork();
            if (child < 0) {
                ::close(errPipe[0]);
                ::close(errPipe[1]);
                error = "fork() failed launching Hi-MD authorization prompt.";
                return false;
            }
            if (child == 0) {
                ::dup2(errPipe[1], STDERR_FILENO);
                ::dup2(errPipe[1], STDOUT_FILENO);
                ::close(errPipe[0]);
                ::close(errPipe[1]);
#if defined(__APPLE__)
                const std::string script =
                    "do shell script \"" + escapeAppleScriptString(shellCmd) +
                    "\" with administrator privileges with prompt "
                    "\"Carta Studio needs administrator access to communicate with your Sony Hi-MD "
                    "recorder over USB.\"";
                char* argv[] = {const_cast<char*>("/usr/bin/osascript"), const_cast<char*>("-e"),
                                const_cast<char*>(script.c_str()), nullptr};
                ::execv("/usr/bin/osascript", argv);
#elif defined(__linux__)
                char* argv[] = {const_cast<char*>("/usr/bin/pkexec"), const_cast<char*>("/bin/sh"),
                                const_cast<char*>("-c"), const_cast<char*>(shellCmd.c_str()),
                                nullptr};
                ::execv("/usr/bin/pkexec", argv);
#endif
                ::_exit(127);
            }
            ::close(errPipe[1]);
            std::string promptOut;
            char buf[512];
            while (true) {
                const ssize_t n = ::read(errPipe[0], buf, sizeof(buf));
                if (n < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    break;
                }
                if (n == 0) {
                    break;
                }
                promptOut.append(buf, static_cast<std::size_t>(n));
            }
            ::close(errPipe[0]);
            while (!promptOut.empty() &&
                   (promptOut.back() == '\n' || promptOut.back() == '\r')) {
                promptOut.pop_back();
            }

            int st = 0;
            int exitCode = -1;
            if (::waitpid(child, &st, 0) > 0 && WIFEXITED(st)) {
                exitCode = WEXITSTATUS(st);
            }
            if (exitCode != 0) {
                (void)::unlink(sockPath.c_str());
#if defined(__APPLE__)
                if (containsCaseInsensitive(promptOut, "user canceled") ||
                    containsCaseInsensitive(promptOut, "-128")) {
                    error = "Hi-MD administrator authorization was canceled.";
                } else {
                    error = promptOut.empty()
                                ? "Hi-MD administrator authorization failed."
                                : ("Hi-MD administrator authorization failed: " + promptOut);
                }
#elif defined(__linux__)
                if (exitCode == 126 || exitCode == 127 ||
                    containsCaseInsensitive(promptOut, "dismissed")) {
                    error = "Hi-MD administrator authorization was canceled or pkexec is "
                            "unavailable.";
                } else {
                    error = promptOut.empty()
                                ? "Hi-MD administrator authorization failed."
                                : ("Hi-MD administrator authorization failed: " + promptOut);
                }
#else
                error = "Hi-MD administrator authorization is not supported on this platform.";
#endif
                return false;
            }
        }

        const auto connectDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        int connectedFd = -1;
        while (std::chrono::steady_clock::now() < connectDeadline) {
            if (directPid_ > 0) {
                int st = 0;
                const pid_t wrc = ::waitpid(directPid_, &st, WNOHANG);
                if (wrc == directPid_) {
                    directPid_ = -1;
                    error = (mode == HiMdElevationMode::Sudo)
                                ? "sudo -n failed starting elevated Hi-MD session."
                                : "Hi-MD socket session exited before becoming ready.";
                    (void)::unlink(sockPath.c_str());
                    return false;
                }
            }

            const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) {
                error = "socket() failed connecting to Hi-MD session.";
                stop();
                return false;
            }
            sockaddr_un addr{};
            if (sockPath.size() >= sizeof(addr.sun_path)) {
                ::close(fd);
                error = "Hi-MD socket path exceeds sockaddr_un length.";
                stop();
                return false;
            }
            addr.sun_family = AF_UNIX;
            std::memcpy(addr.sun_path, sockPath.c_str(), sockPath.size() + 1u);
            if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
                connectedFd = fd;
                break;
            }
            ::close(fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (connectedFd < 0) {
            error = "Timed out waiting for elevated Hi-MD session socket.";
            stop();
            return false;
        }

        sockFd_ = connectedFd;
        socketPath_ = sockPath;
        carryBuf_.clear();
        loadedHelperPath_ = helperPath;
        mockMode_.store(enableMock, std::memory_order_release);
        activeFlag_.store(true, std::memory_order_release);

        juce::var authRes;
        if (!send({"auth", tokenHex}, nullptr, authRes, error)) {
            stop();
            if (error.empty()) {
                error = "Elevated Hi-MD session failed token authentication.";
            }
            return false;
        }
        return true;
#endif
    }

    void stop() {
        const bool wasOpen = isRunning();
        activeFlag_.store(false, std::memory_order_release);
        mockMode_.store(false, std::memory_order_release);
        loadedHelperPath_.clear();
        carryBuf_.clear();

#if defined(_WIN32)
        if (hPipe_ != INVALID_HANDLE_VALUE) {
            if (wasOpen) {
                const char quitCmd[] = "quit\n";
                DWORD written = 0;
                (void)WriteFile(hPipe_, quitCmd, static_cast<DWORD>(sizeof(quitCmd) - 1u),
                                &written, nullptr);
            }
            CloseHandle(hPipe_);
            hPipe_ = INVALID_HANDLE_VALUE;
        }
        if (hDirectProc_ != nullptr) {
            WaitForSingleObject(hDirectProc_, 5000);
            CloseHandle(hDirectProc_);
            hDirectProc_ = nullptr;
        }
        pipeName_.clear();
#else
        if (sockFd_ >= 0) {
            if (wasOpen) {
                const char quitCmd[] = "quit\n";
                const ssize_t ignored = ::write(sockFd_, quitCmd, sizeof(quitCmd) - 1u);
                (void)ignored;
            }
            ::close(sockFd_);
            sockFd_ = -1;
        }
        if (directPid_ > 0) {
            int st = 0;
            for (int i = 0; i < 40; ++i) {
                const pid_t w = ::waitpid(directPid_, &st, WNOHANG);
                if (w == directPid_ || w < 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            directPid_ = -1;
        }
        if (!socketPath_.empty()) {
            (void)::unlink(socketPath_.c_str());
            socketPath_.clear();
        }
#endif
    }

    bool send(const std::vector<std::string>& fields, const HelperProgressWindow* window,
              juce::var& outResult, std::string& error) {
        outResult = juce::var();
        if (!isRunning()) {
            error = "Elevated Hi-MD session is not running.";
            return false;
        }

        std::string line;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) {
                line.push_back('\t');
            }
            line += (i == 0 ? fields[i] : percentEncodeField(fields[i]));
        }
        line.push_back('\n');

        if (!writeAllToSession(line.data(), line.size())) {
            stop();
            error = "Failed writing command to elevated Hi-MD session.";
            return false;
        }

        std::string rawLine;
        while (readLineFromSession(rawLine)) {
            if (!rawLine.empty() && rawLine.back() == '\r') {
                rawLine.pop_back();
            }
            if (rawLine.empty()) {
                continue;
            }
            juce::var parsed;
            if (juce::JSON::parse(juce::String::fromUTF8(rawLine.c_str()), parsed).failed() ||
                !parsed.isObject()) {
                stop();
                error = "Malformed NDJSON line from elevated Hi-MD session: " + rawLine;
                return false;
            }
            const auto* obj = parsed.getDynamicObject();
            const auto ev = obj->getProperty("event").toString();
            if (ev == "progress") {
                publishHelperProgressLine(obj, window);
                continue;
            }
            if (ev == "result") {
                outResult = parsed;
                if (!static_cast<bool>(obj->getProperty("ok"))) {
                    error = obj->getProperty("error").toString().toStdString();
                    if (error.empty()) {
                        error = "Elevated Hi-MD session command failed.";
                    }
                    return false;
                }
                error.clear();
                return true;
            }
        }

        stop();
        error = "Elevated Hi-MD session closed unexpectedly.";
        return false;
    }

  private:
    bool writeAllToSession(const char* data, std::size_t len) {
#if defined(_WIN32)
        if (hPipe_ == INVALID_HANDLE_VALUE) {
            return false;
        }
        std::size_t off = 0;
        while (off < len) {
            DWORD chunk = 0;
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(len - off, 65536u));
            if (!WriteFile(hPipe_, data + off, want, &chunk, nullptr) || chunk == 0) {
                return false;
            }
            off += static_cast<std::size_t>(chunk);
        }
        return true;
#else
        if (sockFd_ < 0) {
            return false;
        }
        std::size_t off = 0;
        while (off < len) {
            const ssize_t n = ::write(sockFd_, data + off, len - off);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                return false;
            }
            off += static_cast<std::size_t>(n);
        }
        return true;
#endif
    }

    bool readLineFromSession(std::string& outLine) {
        outLine.clear();
        while (true) {
            const std::size_t nl = carryBuf_.find('\n');
            if (nl != std::string::npos) {
                outLine = carryBuf_.substr(0, nl);
                carryBuf_.erase(0, nl + 1);
                return true;
            }
#if defined(_WIN32)
            if (hPipe_ == INVALID_HANDLE_VALUE) {
                return false;
            }
            char chunk[4096];
            DWORD readLen = 0;
            if (!ReadFile(hPipe_, chunk, static_cast<DWORD>(sizeof(chunk)), &readLen, nullptr) ||
                readLen == 0) {
                return false;
            }
            carryBuf_.append(chunk, static_cast<std::size_t>(readLen));
#else
            if (sockFd_ < 0) {
                return false;
            }
            char chunk[4096];
            const ssize_t n = ::read(sockFd_, chunk, sizeof(chunk));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            if (n == 0) {
                return false;
            }
            carryBuf_.append(chunk, static_cast<std::size_t>(n));
#endif
        }
    }

#if defined(_WIN32)
    HANDLE hPipe_ = INVALID_HANDLE_VALUE;
    HANDLE hDirectProc_ = nullptr;
    std::string pipeName_;
#else
    int sockFd_ = -1;
    pid_t directPid_ = -1;
    std::string socketPath_;
#endif
    std::string carryBuf_;
    std::string loadedHelperPath_;
    std::atomic<bool> activeFlag_{false};
    std::atomic<bool> mockMode_{false};
};

PersistentElevatedHiMdSession& persistentElevatedSession() {
    static PersistentElevatedHiMdSession sess;
    return sess;
}

bool invokeHiMdUnlocked(const std::vector<std::string>& sessionFields,
                        const std::vector<std::string>& cliArgs, bool preferMockSession,
                        bool allowElevationPrompt, const HelperProgressWindow* window,
                        juce::var& outResult, std::string& error) {
    std::string helperPath;
    if (!locateHiMdHelperUnlocked(helperPath, error)) {
        return false;
    }

    const bool isListOp = !sessionFields.empty() && sessionFields[0] == "list";
    if (preferMockSession) {
        if (persistentElevatedSession().isRunning() && persistentElevatedSession().isMockMode()) {
            return persistentElevatedSession().send(sessionFields, window, outResult, error);
        }
        if (!persistentMockSession().ensureStarted(helperPath, error)) {
            return false;
        }
        return persistentMockSession().send(sessionFields, window, outResult, error);
    }

    if (isListOp) {
        return runOneShotHelper(helperPath, cliArgs, window, outResult, error);
    }

    if (persistentElevatedSession().isRunning()) {
        if (persistentElevatedSession().send(sessionFields, window, outResult, error)) {
            return true;
        }
        // Only fall through to re-establish if the transport itself disconnected.
        if (persistentElevatedSession().isRunning()) {
            return false;
        }
    }

    if (runOneShotHelper(helperPath, cliArgs, window, outResult, error)) {
        return true;
    }

    if (!isHiMdPermissionError(error)) {
        return false;
    }

    const auto mode = readElevationMode();
    if (mode == HiMdElevationMode::Off ||
        (mode == HiMdElevationMode::Auto &&
         (!allowElevationPrompt || !isHiMdElevationPromptEnabled()))) {
        return false;
    }

    if (window != nullptr) {
        setHiMdProgress(window->operation, "Waiting for administrator authorization...",
                        window->baseProgress);
    }
    std::string elevErr;
    if (!persistentElevatedSession().ensureStarted(helperPath, false, allowElevationPrompt,
                                                   elevErr)) {
        if (!elevErr.empty()) {
            error = elevErr;
        }
        return false;
    }
    if (window != nullptr) {
        setHiMdProgress(window->operation, window->fallbackPhase, window->baseProgress);
    }
    return persistentElevatedSession().send(sessionFields, window, outResult, error);
}

bool requireWriteConfirmationUnlocked(const std::string& resolvedId, bool confirmHardware,
                                      std::string& error) {
    const bool needsConfirm = !isHiMdMockDeviceId(resolvedId) || isHiMdTestMockGuardEnabled();
    if (needsConfirm && !confirmHardware) {
        error = "Refusing to modify Hi-MD hardware without explicit hardware confirmation.";
        return false;
    }
    return true;
}

bool listHiMdDevicesUnlocked(std::vector<HiMdDeviceEntry>& out, std::string& error) {
    out.clear();
    juce::var res;
    if (!invokeHiMdUnlocked({"list"}, {"list"}, mockSessionEnabledFlag(),
                            /*allowElevationPrompt=*/false, nullptr, res, error)) {
        return false;
    }
    const auto* rootObj = res.getDynamicObject();
    const auto* arr = rootObj ? rootObj->getProperty("devices").getArray() : nullptr;
    if (!arr) {
        error = "carta-himd list response did not include a devices array.";
        return false;
    }
    out.reserve(static_cast<std::size_t>(arr->size()));
    for (const auto& item : *arr) {
        const auto* d = item.getDynamicObject();
        if (!d) {
            continue;
        }
        HiMdDeviceEntry entry;
        entry.id = d->getProperty("id").toString().toStdString();
        entry.vendorId = static_cast<std::uint16_t>(static_cast<int>(d->getProperty("vendorId")));
        entry.productId =
            static_cast<std::uint16_t>(static_cast<int>(d->getProperty("productId")));
        entry.name = d->getProperty("name").toString().toStdString();
        entry.isMock = static_cast<bool>(d->getProperty("isMock"));
        entry.transport = d->getProperty("transport").toString().toStdString();
        entry.systemPath = d->getProperty("systemPath").toString().toStdString();
        out.push_back(std::move(entry));
    }
    error.clear();
    return true;
}

bool resolveTargetDeviceIdUnlocked(const std::string& preferredId, std::string& outId,
                                   std::string& error) {
    if (!preferredId.empty()) {
        outId = preferredId;
        return true;
    }
    std::vector<HiMdDeviceEntry> devs;
    if (!listHiMdDevicesUnlocked(devs, error)) {
        return false;
    }
    if (devs.empty()) {
        error = "No USB Hi-MD device detected.";
        return false;
    }
    outId = devs.front().id;
    return true;
}

bool probeHiMdDeviceUnlocked(const std::string& resolvedId, HiMdDeviceStatus& out,
                             std::string& error, bool allowElevationPrompt = true) {
    out = {};
    juce::var res;
    const bool useMockSess = isHiMdMockDeviceId(resolvedId);
    if (!invokeHiMdUnlocked({"probe", resolvedId}, {"probe", resolvedId}, useMockSess,
                            allowElevationPrompt, nullptr, res, error)) {
        return false;
    }

    const auto* rootObj = res.getDynamicObject();
    const auto* st = rootObj ? rootObj->getProperty("status").getDynamicObject() : nullptr;
    if (!st) {
        error = "carta-himd probe response did not include status.";
        return false;
    }

    out.deviceId = st->getProperty("deviceId").toString().toStdString();
    out.deviceName = st->getProperty("deviceName").toString().toStdString();
    if (const auto* inq = st->getProperty("inquiry").getDynamicObject()) {
        out.inquiryVid = inq->getProperty("vid").toString().toStdString();
        out.inquiryPid = inq->getProperty("pid").toString().toStdString();
        out.inquiryRev = inq->getProperty("rev").toString().toStdString();
    }
    if (const auto* cap = st->getProperty("capacity").getDynamicObject()) {
        out.maxLba =
            static_cast<std::int64_t>(static_cast<juce::int64>(cap->getProperty("maxLba")));
        out.blockSize = static_cast<int>(cap->getProperty("blockSize"));
        out.deviceSize =
            static_cast<std::int64_t>(static_cast<juce::int64>(cap->getProperty("deviceSize")));
    }
    if (const auto* fs = st->getProperty("filesystem").getDynamicObject()) {
        out.fsTotalBytes =
            static_cast<std::int64_t>(static_cast<juce::int64>(fs->getProperty("total")));
        out.fsUsedBytes =
            static_cast<std::int64_t>(static_cast<juce::int64>(fs->getProperty("used")));
        out.fsFreeBytes =
            static_cast<std::int64_t>(static_cast<juce::int64>(fs->getProperty("left")));
    }
    out.datanum = static_cast<int>(st->getProperty("datanum"));
    out.discId = st->getProperty("discId").toString().toStdString();
    out.discTitle = st->getProperty("discTitle").toString().toStdString();
    out.trackCount = static_cast<int>(st->getProperty("trackCount"));

    if (const auto* trArr = st->getProperty("tracks").getArray()) {
        out.tracks.reserve(static_cast<std::size_t>(trArr->size()));
        for (const auto& item : *trArr) {
            const auto* t = item.getDynamicObject();
            if (!t) {
                continue;
            }
            HiMdDeviceTrack tr;
            tr.index = static_cast<int>(t->getProperty("index"));
            tr.slot = static_cast<int>(t->getProperty("slot"));
            tr.title = t->getProperty("title").toString().toStdString();
            tr.artist = t->getProperty("artist").toString().toStdString();
            tr.album = t->getProperty("album").toString().toStdString();
            tr.encoding = t->getProperty("encoding").toString().toStdString();
            tr.bitrate = static_cast<int>(t->getProperty("bitrate"));
            tr.durationSeconds = static_cast<double>(t->getProperty("duration"));
            out.tracks.push_back(std::move(tr));
        }
    }

    if (const auto* gArr = st->getProperty("groups").getArray()) {
        out.groups.reserve(static_cast<std::size_t>(gArr->size()));
        for (const auto& item : *gArr) {
            const auto* g = item.getDynamicObject();
            if (!g) {
                continue;
            }
            HiMdDeviceGroup gr;
            gr.groupIndex = static_cast<int>(g->getProperty("groupIndex"));
            if (gr.groupIndex < 0) {
                continue;
            }
            gr.title = g->getProperty("title").toString().toStdString();
            gr.startIndex = static_cast<int>(g->getProperty("startIndex"));
            gr.trackCount = static_cast<int>(g->getProperty("trackCount"));
            out.groups.push_back(std::move(gr));
        }
        std::sort(out.groups.begin(), out.groups.end(),
                  [](const HiMdDeviceGroup& a, const HiMdDeviceGroup& b) {
                      if (a.startIndex != b.startIndex) {
                          return a.startIndex < b.startIndex;
                      }
                      return a.groupIndex < b.groupIndex;
                  });
    }

    error.clear();
    return true;
}

bool renameHiMdDiscUnlocked(const std::string& resolvedId, const std::string& title,
                            bool confirmHardware, std::string& error) {
    if (!requireWriteConfirmationUnlocked(resolvedId, confirmHardware, error)) {
        return false;
    }
    const bool useMockSess = isHiMdMockDeviceId(resolvedId);
    const std::string confirmArg = confirmHardware ? "1" : "0";
    std::vector<std::string> cliArgs{"rename-disc", resolvedId, "--title", title};
    if (confirmHardware) {
        cliArgs.emplace_back("--confirm-hardware");
    }
    juce::var res;
    return invokeHiMdUnlocked({"rename-disc", resolvedId, title, confirmArg}, cliArgs, useMockSess,
                              /*allowElevationPrompt=*/true, nullptr, res, error);
}

bool setHiMdGroupsUnlocked(const std::string& resolvedId, const std::vector<MdGroup>& groups,
                           int trackCount, bool confirmHardware, std::string& error) {
    if (!requireWriteConfirmationUnlocked(resolvedId, confirmHardware, error)) {
        return false;
    }

    juce::Array<juce::var> groupArray;
    for (std::size_t gIdx = 0; gIdx < groups.size(); ++gIdx) {
        const auto& grp = groups[gIdx];
        if (grp.startTrack < 1 || grp.endTrack < grp.startTrack ||
            (trackCount > 0 && grp.endTrack > trackCount)) {
            error = "Hi-MD group " + std::to_string(gIdx + 1) + " track range " +
                    std::to_string(grp.startTrack) + "-" + std::to_string(grp.endTrack) +
                    " is out of range (track count " + std::to_string(trackCount) + ").";
            return false;
        }
        auto* obj = new juce::DynamicObject();
        const std::string title =
            !grp.halfWidthTitle.empty() ? grp.halfWidthTitle : grp.fullWidthTitle;
        obj->setProperty("title", juce::String::fromUTF8(title.c_str()));
        juce::Array<juce::var> indices;
        for (int trk = grp.startTrack; trk <= grp.endTrack; ++trk) {
            indices.add(trk - 1); // Convert 1-based MdGroup bounds to 0-based Hi-MD indices.
        }
        obj->setProperty("indices", indices);
        groupArray.add(juce::var(obj));
    }

    const std::string groupsJson = juce::JSON::toString(juce::var(groupArray), true).toStdString();
    const bool useMockSess = isHiMdMockDeviceId(resolvedId);
    const std::string confirmArg = confirmHardware ? "1" : "0";
    std::vector<std::string> cliArgs{"set-groups", resolvedId, "--groups-json", groupsJson};
    if (confirmHardware) {
        cliArgs.emplace_back("--confirm-hardware");
    }
    juce::var res;
    return invokeHiMdUnlocked({"set-groups", resolvedId, groupsJson, confirmArg}, cliArgs,
                              useMockSess, /*allowElevationPrompt=*/true, nullptr, res, error);
}

MdCodecMode codecFromHiMdTrack(const std::string& encoding, int bitrate) noexcept {
    if (encoding == "PCM" || encoding == "LPCM") {
        return MdCodecMode::pcm;
    }
    if (encoding == "AT3" || encoding == "ATRAC3") {
        return bitrate >= 100 ? MdCodecMode::lp2 : MdCodecMode::lp4;
    }
    return bitrate >= 192 ? MdCodecMode::hiSp : MdCodecMode::hiLp;
}

std::string preferredDiscTitle(const MdDisc& disc) {
    return !disc.halfWidthTitle.empty() ? disc.halfWidthTitle : disc.fullWidthTitle;
}

std::string preferredTrackTitle(const MdTrack& track) {
    return !track.halfWidthTitle.empty() ? track.halfWidthTitle : track.fullWidthTitle;
}

} // namespace

HiMdProgressSnapshot getHiMdProgress() {
    const std::lock_guard<std::mutex> lock(progressState().mutex);
    return progressState().snapshot;
}

void setHiMdProgress(const std::string& operation, const std::string& phase, double progress,
                     std::int64_t bytesDone, std::int64_t bytesTotal) {
    const std::lock_guard<std::mutex> lock(progressState().mutex);
    progressState().snapshot.operation = operation;
    progressState().snapshot.phase = phase;
    progressState().snapshot.progress = std::max(0.0, std::min(1.0, progress));
    progressState().snapshot.bytesDone = std::max<std::int64_t>(0, bytesDone);
    progressState().snapshot.bytesTotal = std::max<std::int64_t>(0, bytesTotal);
}

void clearHiMdProgress() {
    const std::lock_guard<std::mutex> lock(progressState().mutex);
    progressState().snapshot = {};
}

bool isHiMdMockDeviceId(const std::string& deviceId) noexcept {
    return deviceId.rfind("mock:", 0) == 0;
}

bool isHiMdPermissionError(const std::string& error) noexcept {
    return containsCaseInsensitive(error, "access denied") ||
           containsCaseInsensitive(error, "insufficient permissions") ||
           containsCaseInsensitive(error, "permission denied") ||
           containsCaseInsensitive(error, "eacces") ||
           containsCaseInsensitive(error, "error_access_denied") ||
           containsCaseInsensitive(error, "operation not permitted") ||
           containsCaseInsensitive(error, "eperm") ||
           containsCaseInsensitive(error, "run with sudo") ||
           containsCaseInsensitive(error, "run as administrator") ||
           containsCaseInsensitive(error, "iousbmassstoragedriver");
}

void setHiMdElevationPromptEnabled(bool enabled) noexcept {
    elevationPromptEnabledStorage().store(enabled, std::memory_order_release);
}

bool isHiMdElevationPromptEnabled() noexcept {
    return elevationPromptEnabledStorage().load(std::memory_order_acquire);
}

bool isHiMdElevatedSessionActive() noexcept {
    return persistentElevatedSession().isRunning();
}

bool ensureHiMdElevatedSession(std::string& error) {
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    std::string helperPath;
    if (!locateHiMdHelperUnlocked(helperPath, error)) {
        return false;
    }
    const bool useMock = mockSessionEnabledFlag();
    return persistentElevatedSession().ensureStarted(helperPath, useMock, true, error);
}

void stopHiMdElevatedSession() {
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    persistentElevatedSession().stop();
}

bool isHiMdTestMockGuardEnabled() noexcept {
    return mockGuardStorage();
}

void setHiMdHelperPathOverride(const std::string& path) {
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    persistentElevatedSession().stop();
    persistentMockSession().stop();
    helperOverrideStorage() = path;
}

bool setHiMdTestMockSessionEnabled(bool enabled, std::string& error) {
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    error.clear();
    if (!enabled) {
        mockSessionEnabledFlag() = false;
        mockGuardStorage() = false;
        persistentMockSession().stop();
        return true;
    }
    std::string helperPath;
    if (!locateHiMdHelperUnlocked(helperPath, error)) {
        return false;
    }
    if (!persistentMockSession().ensureStarted(helperPath, error)) {
        return false;
    }
    mockSessionEnabledFlag() = true;
    return true;
}

bool resetHiMdTestMockSession(std::string& error) {
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    std::string helperPath;
    if (!locateHiMdHelperUnlocked(helperPath, error)) {
        return false;
    }
    if (!persistentMockSession().ensureStarted(helperPath, error)) {
        return false;
    }
    mockSessionEnabledFlag() = true;
    mockGuardStorage() = false;
    juce::var res;
    return persistentMockSession().send({"mock-reset"}, nullptr, res, error);
}

bool setHiMdTestMockGuard(bool enabled, std::string& error) {
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    std::string helperPath;
    if (!locateHiMdHelperUnlocked(helperPath, error)) {
        return false;
    }
    if (!persistentMockSession().ensureStarted(helperPath, error)) {
        return false;
    }
    mockSessionEnabledFlag() = true;
    juce::var res;
    if (!persistentMockSession().send({"mock-guard", enabled ? "1" : "0"}, nullptr, res, error)) {
        return false;
    }
    mockGuardStorage() = enabled;
    return true;
}

bool fetchHiMdTestMockSnapshot(std::string& outJson, std::string& error) {
    outJson.clear();
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    std::string helperPath;
    if (!locateHiMdHelperUnlocked(helperPath, error)) {
        return false;
    }
    if (!persistentMockSession().ensureStarted(helperPath, error)) {
        return false;
    }
    mockSessionEnabledFlag() = true;
    juce::var res;
    if (!persistentMockSession().send({"mock-snapshot"}, nullptr, res, error)) {
        return false;
    }
    outJson = juce::JSON::toString(res, true).toStdString();
    return true;
}

bool listHiMdDevices(std::vector<HiMdDeviceEntry>& out, std::string& error) {
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    return listHiMdDevicesUnlocked(out, error);
}

bool probeHiMdDevice(const std::string& deviceId, HiMdDeviceStatus& out, std::string& error,
                     bool allowElevationPrompt) {
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    std::string resolvedId;
    if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
        return false;
    }
    return probeHiMdDeviceUnlocked(resolvedId, out, error, allowElevationPrompt);
}

HiMdPreflightReport preflightHiMdRecording(const MdDisc& disc,
                                           const HiMdDeviceStatus* deviceStatus) {
    HiMdPreflightReport report;
    if (!validateMdDisc(disc, report.error)) {
        return report;
    }
    if (disc.tracks.empty()) {
        report.error = "Hi-MD disc has no tracks to record.";
        return report;
    }

    for (std::size_t i = 0; i < disc.tracks.size(); ++i) {
        const auto& tr = disc.tracks[i];
        const auto codec = effectiveTrackCodec(disc, tr);
        if (isStandardMdOnlyCodec(codec)) {
            report.error = "Track " + std::to_string(i + 1) + " uses Standard MD codec '" +
                           mdCodecModeToString(codec) +
                           "', which is not supported on Hi-MD (use Hi-SP, Hi-LP, LP2, LP4, or "
                           "PCM).";
            return report;
        }
    }

    MdPhysicalPlan plan;
    MdDisc hiMdView = disc;
    if (!isHiMdMedium(hiMdView.medium)) {
        hiMdView.medium = MdMedium::himd300;
    }
    if (!planMdPhysicalAudio(hiMdView, plan, report.error)) {
        return report;
    }

    const std::int64_t trackOverhead =
        static_cast<std::int64_t>(plan.uniqueAudioTrackCount) * kHiMdPerTrackMetaBytes;
    const std::int64_t defaultCap =
        isHiMdMedium(disc.medium) ? hiMdMediumCapacityBytes(disc.medium)
                                  : hiMdMediumCapacityBytes(MdMedium::himd300);

    report.tracksToUpload = plan.playOrderTrackCount;
    report.uniqueAudioTracksToUpload = plan.uniqueAudioTrackCount;
    report.estimatedAudioBytes = plan.hiMdUniqueBytes;
    report.unaliasedAudioBytes = plan.hiMdUnaliasedBytes;
    report.savedBytes = plan.hiMdSavedBytes;
    report.requiredBytes = plan.hiMdUniqueBytes + trackOverhead + kHiMdMetadataReserveBytes;
    report.discTotalBytes =
        (deviceStatus && deviceStatus->fsTotalBytes > 0) ? deviceStatus->fsTotalBytes : defaultCap;
    report.discFreeBytes = (deviceStatus && deviceStatus->fsTotalBytes > 0)
                               ? deviceStatus->fsFreeBytes
                               : report.discTotalBytes;

    if (report.requiredBytes > report.discFreeBytes) {
        report.error = "Hi-MD disc does not have enough free space (need " +
                       std::to_string(report.requiredBytes) + " bytes, available " +
                       std::to_string(report.discFreeBytes) + " bytes).";
        return report;
    }

    report.valid = true;
    report.error.clear();
    return report;
}

bool recordHiMdDisc(const Session& session, const MdDisc& disc, const std::string& deviceId,
                    bool confirmHardware, HiMdRecordResult& outResult, std::string& error) {
    outResult = {};
    error.clear();

    if (disc.tracks.empty()) {
        error = "Hi-MD disc has no tracks to record.";
        return false;
    }

    std::string resolvedId;
    {
        const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
        if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
            return false;
        }
        // Check hardware write confirmation BEFORE any mix rendering or ATRAC3plus/ATRAC3/PCM
        // encoding takes place.
        if (!requireWriteConfirmationUnlocked(resolvedId, confirmHardware, error)) {
            return false;
        }
    }

    if (!validateMdDisc(disc, error)) {
        return false;
    }
    for (std::size_t i = 0; i < disc.tracks.size(); ++i) {
        const auto codec = effectiveTrackCodec(disc, disc.tracks[i]);
        if (isStandardMdOnlyCodec(codec)) {
            error = "Track " + std::to_string(i + 1) + " uses Standard MD codec '" +
                    mdCodecModeToString(codec) +
                    "', which is not supported on Hi-MD (use Hi-SP, Hi-LP, LP2, LP4, or PCM).";
            return false;
        }
    }

    ScopedHiMdProgress progressScope;
    setHiMdProgress("record", "Probing Hi-MD device...", 0.02);

    HiMdDeviceStatus status;
    {
        const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
        if (!probeHiMdDeviceUnlocked(resolvedId, status, error)) {
            return false;
        }
    }
    outResult.deviceId = status.deviceId.empty() ? resolvedId : status.deviceId;
    outResult.deviceName = status.deviceName;
    outResult.hardwareConfirmed = confirmHardware;

    if (!disc.loadedDisc.active && status.trackCount > 0) {
        error = "Inserted Hi-MD disc is not empty (" + std::to_string(status.trackCount) +
                " track(s)). Wipe the disc or load it before recording.";
        return false;
    }

    const auto preflight = preflightHiMdRecording(disc, &status);
    if (!preflight.valid) {
        error = preflight.error;
        return false;
    }

    // Step 1: Render and encode all tracks to raw Hi-MD upload payloads (temp files cleaned on exit).
    struct PreparedUpload {
        std::string codecSpec;
        std::string title;
        std::string artist;
        std::string album;
        std::int64_t payloadBytes = 0;
        juce::File payloadFile;
    };
    std::vector<PreparedUpload> prepared;
    prepared.reserve(disc.tracks.size());
    std::vector<std::unique_ptr<TempFileRemover>> tempCleanups;
    tempCleanups.reserve(disc.tracks.size());

    const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto stamp = juce::Time::getHighResolutionTicks();

    for (std::size_t idx = 0; idx < disc.tracks.size(); ++idx) {
        const auto& trk = disc.tracks[idx];
        const auto codec = effectiveTrackCodec(disc, trk);
        const std::string codecSpec = mdCodecModeToString(codec);
        const std::int64_t totalTrackSamples = trk.totalSamples();
        if (totalTrackSamples <= 0) {
            error = "Track " + std::to_string(idx + 1) + " has zero duration.";
            return false;
        }

        const double trackBase =
            0.05 + 0.45 * (static_cast<double>(idx) / static_cast<double>(disc.tracks.size()));
        const double trackSpan = 0.45 / static_cast<double>(disc.tracks.size());
        setHiMdProgress("record",
                        "Rendering & encoding track " + std::to_string(idx + 1) + "/" +
                            std::to_string(disc.tracks.size()) + " (" + codecSpec + ")...",
                        trackBase);

        std::vector<float> trackL;
        std::vector<float> trackR;
        trackL.reserve(static_cast<std::size_t>(totalTrackSamples));
        trackR.reserve(static_cast<std::size_t>(totalTrackSamples));

        for (const auto& part : trk.parts) {
            std::vector<float> partL;
            std::vector<float> partR;
            if (!renderSessionMixRange(session, part.startSample, part.endSample, 2, partL, partR,
                                       error)) {
                return false;
            }
            trackL.insert(trackL.end(), partL.begin(), partL.end());
            trackR.insert(trackR.end(), partR.begin(), partR.end());
        }

        std::vector<std::uint8_t> payload;
        auto encodeCb = [&](int done, int total) {
            const double frac =
                total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 1.0;
            setHiMdProgress("record",
                            "Encoding track " + std::to_string(idx + 1) + "/" +
                                std::to_string(disc.tracks.size()) + " (" + codecSpec + ")...",
                            trackBase + trackSpan * std::max(0.0, std::min(1.0, frac)));
        };
        if (!encodeHiMdTrackPayload(codecSpec, trackL.data(), trackR.data(), 2,
                                    static_cast<int>(trackL.size()), payload, encodeCb, error)) {
            return false;
        }
        if (payload.empty()) {
            error = "Encoded Hi-MD payload is empty for track " + std::to_string(idx + 1) + ".";
            return false;
        }

        const juce::File tempFile = tempDir.getChildFile(
            "carta_himd_upload_" + juce::String(stamp) + "_" + juce::String(idx) + ".bin");
        if (!tempFile.replaceWithData(payload.data(), payload.size())) {
            error = "Failed writing temporary Hi-MD upload payload: " +
                    tempFile.getFullPathName().toStdString();
            return false;
        }
        tempCleanups.push_back(std::make_unique<TempFileRemover>(tempFile));

        PreparedUpload item;
        item.codecSpec = codecSpec;
        item.title = preferredTrackTitle(trk);
        item.artist = trk.artist;
        item.album = trk.album;
        item.payloadBytes = static_cast<std::int64_t>(payload.size());
        item.payloadFile = tempFile;
        prepared.push_back(std::move(item));
    }

    // Step 2: Upload all tracks sequentially and apply disc title + groups.
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    const bool useMockSess = isHiMdMockDeviceId(resolvedId);
    const std::string confirmArg = confirmHardware ? "1" : "0";

    std::int64_t totalBytesUploaded = 0;
    std::int64_t totalBlocksWritten = 0;
    int latestDatanum = status.datanum;

    for (std::size_t idx = 0; idx < prepared.size(); ++idx) {
        const auto& up = prepared[idx];
        HelperProgressWindow win;
        win.active = true;
        win.operation = "record";
        win.fallbackPhase = "Uploading track " + std::to_string(idx + 1) + "/" +
                            std::to_string(prepared.size()) + " to Hi-MD disc...";
        win.baseProgress =
            0.50 + 0.42 * (static_cast<double>(idx) / static_cast<double>(prepared.size()));
        win.spanProgress = 0.42 / static_cast<double>(prepared.size());

        const std::string inPath = up.payloadFile.getFullPathName().toStdString();
        std::vector<std::string> cliArgs{"upload", resolvedId, "--codec", up.codecSpec,
                                         "--in",   inPath,     "--title", up.title,
                                         "--artist", up.artist, "--album", up.album};
        if (confirmHardware) {
            cliArgs.emplace_back("--confirm-hardware");
        }

        juce::var res;
        if (!invokeHiMdUnlocked({"upload", resolvedId, up.codecSpec, inPath, up.title, up.artist,
                                 up.album, confirmArg},
                                cliArgs, useMockSess, /*allowElevationPrompt=*/true, &win, res,
                                error)) {
            return false;
        }
        if (const auto* uObj =
                res.getDynamicObject()
                    ? res.getDynamicObject()->getProperty("upload").getDynamicObject()
                    : nullptr) {
            totalBlocksWritten += static_cast<std::int64_t>(
                static_cast<juce::int64>(uObj->getProperty("blocksWritten")));
            const auto bw =
                static_cast<std::int64_t>(static_cast<juce::int64>(uObj->getProperty("bytesWritten")));
            totalBytesUploaded += bw > 0 ? bw : up.payloadBytes;
            latestDatanum = static_cast<int>(uObj->getProperty("datanum"));
        } else {
            totalBytesUploaded += up.payloadBytes;
        }
    }

    const std::string discTitle = preferredDiscTitle(disc);
    if (!discTitle.empty()) {
        setHiMdProgress("record", "Writing Hi-MD disc title...", 0.94, totalBytesUploaded,
                        totalBytesUploaded);
        if (!renameHiMdDiscUnlocked(resolvedId, discTitle, confirmHardware, error)) {
            return false;
        }
    }

    if (!disc.groups.empty()) {
        setHiMdProgress("record", "Writing Hi-MD group index...", 0.97, totalBytesUploaded,
                        totalBytesUploaded);
        if (!setHiMdGroupsUnlocked(resolvedId, disc.groups, static_cast<int>(disc.tracks.size()),
                                   confirmHardware, error)) {
            return false;
        }
    }

    setHiMdProgress("record", "Hi-MD recording complete.", 1.0, totalBytesUploaded,
                    totalBytesUploaded);
    outResult.ok = true;
    outResult.discTitle = discTitle;
    outResult.uploadedTracks = static_cast<int>(prepared.size());
    outResult.uploadedBytes = totalBytesUploaded;
    outResult.blocksWritten = totalBlocksWritten;
    outResult.datanum = latestDatanum;
    error.clear();
    return true;
}

bool dumpHiMdTrack(const std::string& deviceId, int trackIndex, const std::string& outPath,
                   bool rawAudio, std::string& error) {
    if (trackIndex < 0) {
        error = "Hi-MD track index must be >= 0.";
        return false;
    }
    if (outPath.empty()) {
        error = "Output path for Hi-MD track dump must not be empty.";
        return false;
    }

    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    std::string resolvedId;
    if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
        return false;
    }

    const juce::File destFile(juce::String::fromUTF8(outPath.c_str()));
    if (!destFile.getParentDirectory().isDirectory()) {
        error = "Output parent directory does not exist for Hi-MD track dump.";
        return false;
    }

    ScopedHiMdProgress progressScope;
    HelperProgressWindow win;
    win.active = true;
    win.operation = "dump";
    win.fallbackPhase = "Dumping track from Hi-MD disc...";
    win.baseProgress = 0.0;
    win.spanProgress = 1.0;

    const bool useMockSess = isHiMdMockDeviceId(resolvedId);
    const bool useTempCopy = !useMockSess || persistentElevatedSession().isRunning();
    juce::File helperOutFile = destFile;
    std::unique_ptr<TempFileRemover> tempDumpCleanup;
    if (useTempCopy) {
        const auto ext = destFile.getFileExtension();
        const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory);
        const auto stamp = static_cast<juce::int64>(juce::Time::getHighResolutionTicks());
        helperOutFile = tempDir.getChildFile("carta_himd_dump_" + juce::String(stamp) + ext);
        tempDumpCleanup = std::make_unique<TempFileRemover>(helperOutFile);
    }

    const std::string helperOutPath = helperOutFile.getFullPathName().toStdString();
    std::vector<std::string> cliArgs{"dump",    resolvedId, "--track", std::to_string(trackIndex),
                                     "--out",   helperOutPath};
    if (rawAudio) {
        cliArgs.emplace_back("--raw");
    }
    juce::var res;
    if (!invokeHiMdUnlocked(
            {"dump", resolvedId, std::to_string(trackIndex), helperOutPath, rawAudio ? "1" : "0"},
            cliArgs, useMockSess, /*allowElevationPrompt=*/true, &win, res, error)) {
        return false;
    }

    if (useTempCopy) {
        juce::FileInputStream inStream(helperOutFile);
        if (!inStream.openedOk()) {
            error = "Failed opening dumped Hi-MD temporary file: " + helperOutPath;
            return false;
        }
        juce::TemporaryFile target(destFile);
        {
            juce::FileOutputStream outStream(target.getFile());
            if (!outStream.openedOk()) {
                error = "Failed creating destination Hi-MD dump file: " + outPath;
                return false;
            }
            std::array<char, 65536> buf{};
            while (!inStream.isExhausted()) {
                const int n = inStream.read(buf.data(), static_cast<int>(buf.size()));
                if (n < 0) {
                    error = "Read error copying dumped Hi-MD audio.";
                    return false;
                }
                if (n == 0) {
                    break;
                }
                if (!outStream.write(buf.data(), static_cast<std::size_t>(n))) {
                    error = "Write error saving Hi-MD dump file: " + outPath;
                    return false;
                }
            }
            outStream.flush();
        }
        if (!target.overwriteTargetFileWithTemporary()) {
            error = "Failed saving Hi-MD dump file to: " + outPath;
            return false;
        }
    }
    error.clear();
    return true;
}

bool renameHiMdDisc(const std::string& deviceId, const std::string& title, bool confirmHardware,
                    std::string& error) {
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    std::string resolvedId;
    if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
        return false;
    }
    return renameHiMdDiscUnlocked(resolvedId, title, confirmHardware, error);
}

bool renameHiMdTrack(const std::string& deviceId, int trackIndex, bool hasTitle,
                     const std::string& title, bool hasArtist, const std::string& artist,
                     bool hasAlbum, const std::string& album, bool confirmHardware,
                     std::string& error) {
    if (trackIndex < 0) {
        error = "Hi-MD track index must be >= 0.";
        return false;
    }
    if (!hasTitle && !hasArtist && !hasAlbum) {
        error = "renameHiMdTrack requires at least one of title, artist, or album.";
        return false;
    }

    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    std::string resolvedId;
    if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
        return false;
    }
    if (!requireWriteConfirmationUnlocked(resolvedId, confirmHardware, error)) {
        return false;
    }

    const bool useMockSess = isHiMdMockDeviceId(resolvedId);
    const std::string confirmArg = confirmHardware ? "1" : "0";
    std::vector<std::string> cliArgs{"rename-track", resolvedId, "--track",
                                     std::to_string(trackIndex)};
    if (hasTitle) {
        cliArgs.emplace_back("--title");
        cliArgs.push_back(title);
    }
    if (hasArtist) {
        cliArgs.emplace_back("--artist");
        cliArgs.push_back(artist);
    }
    if (hasAlbum) {
        cliArgs.emplace_back("--album");
        cliArgs.push_back(album);
    }
    if (confirmHardware) {
        cliArgs.emplace_back("--confirm-hardware");
    }

    juce::var res;
    return invokeHiMdUnlocked({"rename-track", resolvedId, std::to_string(trackIndex),
                               hasTitle ? "1" : "0", title, hasArtist ? "1" : "0", artist,
                               hasAlbum ? "1" : "0", album, confirmArg},
                              cliArgs, useMockSess, /*allowElevationPrompt=*/true, nullptr, res,
                              error);
}

bool moveHiMdTrack(const std::string& deviceId, int fromIndex, int toIndex, bool confirmHardware,
                   std::string& error) {
    if (fromIndex < 0 || toIndex < 0) {
        error = "Hi-MD track indices must be >= 0.";
        return false;
    }

    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    std::string resolvedId;
    if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
        return false;
    }
    if (!requireWriteConfirmationUnlocked(resolvedId, confirmHardware, error)) {
        return false;
    }

    const bool useMockSess = isHiMdMockDeviceId(resolvedId);
    const std::string confirmArg = confirmHardware ? "1" : "0";
    std::vector<std::string> cliArgs{"move-track", resolvedId, "--from", std::to_string(fromIndex),
                                     "--to", std::to_string(toIndex)};
    if (confirmHardware) {
        cliArgs.emplace_back("--confirm-hardware");
    }

    juce::var res;
    return invokeHiMdUnlocked({"move-track", resolvedId, std::to_string(fromIndex),
                               std::to_string(toIndex), confirmArg},
                              cliArgs, useMockSess, /*allowElevationPrompt=*/true, nullptr, res,
                              error);
}

bool setHiMdGroups(const std::string& deviceId, const std::vector<MdGroup>& groups, int trackCount,
                   bool confirmHardware, std::string& error) {
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    std::string resolvedId;
    if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
        return false;
    }
    return setHiMdGroupsUnlocked(resolvedId, groups, trackCount, confirmHardware, error);
}

bool deleteHiMdTracks(const std::string& deviceId, const std::vector<int>& trackIndices,
                      bool confirmHardware, std::string& error) {
    if (trackIndices.empty()) {
        error = "deleteHiMdTracks requires at least one track index.";
        return false;
    }

    std::set<int> uniqueSorted;
    for (int idx : trackIndices) {
        if (idx < 0) {
            error = "Hi-MD track index must be >= 0.";
            return false;
        }
        uniqueSorted.insert(idx);
    }

    std::ostringstream csv;
    bool first = true;
    for (int idx : uniqueSorted) {
        if (!first) {
            csv << ',';
        }
        first = false;
        csv << idx;
    }
    const std::string csvStr = csv.str();

    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    std::string resolvedId;
    if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
        return false;
    }
    if (!requireWriteConfirmationUnlocked(resolvedId, confirmHardware, error)) {
        return false;
    }

    const bool useMockSess = isHiMdMockDeviceId(resolvedId);
    const std::string confirmArg = confirmHardware ? "1" : "0";
    std::vector<std::string> cliArgs{"delete-tracks", resolvedId, "--tracks", csvStr};
    if (confirmHardware) {
        cliArgs.emplace_back("--confirm-hardware");
    }

    juce::var res;
    return invokeHiMdUnlocked({"delete-tracks", resolvedId, csvStr, confirmArg}, cliArgs,
                              useMockSess, /*allowElevationPrompt=*/true, nullptr, res, error);
}

bool wipeHiMdDisc(const std::string& deviceId, bool reformat, bool confirmHardware,
                  std::string& error) {
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    std::string resolvedId;
    if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
        return false;
    }
    if (!requireWriteConfirmationUnlocked(resolvedId, confirmHardware, error)) {
        return false;
    }

    ScopedHiMdProgress progressScope;
    HelperProgressWindow win;
    win.active = true;
    win.operation = "wipe";
    win.fallbackPhase = reformat ? "Reformatting Hi-MD filesystem..." : "Wiping Hi-MD tracks...";
    win.baseProgress = 0.0;
    win.spanProgress = 1.0;

    const bool useMockSess = isHiMdMockDeviceId(resolvedId);
    const std::string confirmArg = confirmHardware ? "1" : "0";
    std::vector<std::string> cliArgs{"wipe", resolvedId};
    if (reformat) {
        cliArgs.emplace_back("--reformat");
    }
    if (confirmHardware) {
        cliArgs.emplace_back("--confirm-hardware");
    }

    juce::var res;
    return invokeHiMdUnlocked({"wipe", resolvedId, reformat ? "1" : "0", confirmArg}, cliArgs,
                              useMockSess, /*allowElevationPrompt=*/true, &win, res, error);
}

bool loadHiMdDiscIntoSession(const std::string& deviceId, MdDisc& outDisc,
                             HiMdDeviceStatus* outStatus, std::string& error) {
    outDisc = MdDisc{};
    if (outStatus != nullptr) {
        *outStatus = {};
    }

    HiMdDeviceStatus status;
    {
        const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
        std::string resolvedId;
        if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
            return false;
        }
        if (!probeHiMdDeviceUnlocked(resolvedId, status, error)) {
            return false;
        }
    }

    const std::int64_t totalBytes =
        status.fsTotalBytes > 0 ? status.fsTotalBytes : status.deviceSize;
    outDisc.medium =
        (totalBytes > 400LL * 1024LL * 1024LL) ? MdMedium::himd1gb : MdMedium::himd300;
    outDisc.defaultCodec =
        !status.tracks.empty()
            ? codecFromHiMdTrack(status.tracks.front().encoding, status.tracks.front().bitrate)
            : MdCodecMode::hiSp;
    outDisc.halfWidthTitle = status.discTitle;
    outDisc.loadedDisc.active = true;
    outDisc.loadedDisc.sourceDeviceId = status.deviceId;
    outDisc.loadedDisc.baselineSector0Hash =
        status.discId + ":" + std::to_string(status.datanum);

    std::map<int, int> slotRefCounts;
    for (const auto& tr : status.tracks) {
        if (tr.slot > 0) {
            ++slotRefCounts[tr.slot];
        }
    }
    struct LoadedSlotEntry {
        MdTrack canonicalTrack{};
    };
    std::map<int, LoadedSlotEntry> poolBySlot;

    std::int64_t timelineCursor = 0;
    outDisc.tracks.reserve(status.tracks.size());
    outDisc.loadedDisc.regions.reserve(status.tracks.size());
    for (const auto& tr : status.tracks) {
        if (tr.slot > 0) {
            if (const auto it = poolBySlot.find(tr.slot); it != poolBySlot.end()) {
                MdTrack mt = it->second.canonicalTrack;
                mt.id = makeMdTrackId();
                outDisc.tracks.push_back(std::move(mt));
                continue;
            }
        }

        const auto codec = codecFromHiMdTrack(tr.encoding, tr.bitrate);
        const std::int64_t samples = std::max<std::int64_t>(
            1, std::llround(tr.durationSeconds * static_cast<double>(kMdSampleRate)));

        MdTrack mt;
        mt.id = makeMdTrackId();
        if (tr.slot > 0 && slotRefCounts[tr.slot] > 1) {
            mt.audioId = "himd-slot-" + std::to_string(tr.slot);
        }
        mt.halfWidthTitle = tr.title;
        mt.artist = tr.artist;
        mt.album = tr.album;
        mt.codecOverride = codec;
        mt.parts = {{timelineCursor, timelineCursor + samples}};
        if (tr.slot > 0) {
            poolBySlot.emplace(tr.slot, LoadedSlotEntry{mt});
        }
        outDisc.tracks.push_back(std::move(mt));

        MdOnDiscRegion reg;
        reg.id = makeMdTrackId();
        reg.codec = codec;
        reg.timelineStartSample = timelineCursor;
        reg.timelineEndSample = timelineCursor + samples;
        outDisc.loadedDisc.regions.push_back(std::move(reg));

        timelineCursor += samples;
    }

    for (const auto& grp : status.groups) {
        if (grp.groupIndex < 0 || grp.trackCount <= 0 || grp.startIndex < 0 ||
            grp.startIndex + grp.trackCount > static_cast<int>(outDisc.tracks.size())) {
            continue;
        }
        MdGroup mg;
        mg.startTrack = grp.startIndex + 1;
        mg.endTrack = grp.startIndex + grp.trackCount;
        mg.halfWidthTitle = grp.title;
        outDisc.groups.push_back(std::move(mg));
    }

    if (!validateMdDisc(outDisc, error)) {
        outDisc = {};
        return false;
    }

    if (outStatus != nullptr) {
        *outStatus = std::move(status);
    }
    error.clear();
    return true;
}

bool runHiMdHelperStackSelftest(std::string& error) {
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    std::string helperPath;
    if (!locateHiMdHelperUnlocked(helperPath, error)) {
        return false;
    }
    juce::var res;
    return runOneShotHelper(helperPath, {"selftest-stack"}, nullptr, res, error);
}

} // namespace carta
