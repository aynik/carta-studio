/**
 * @file HiMdBridge.cpp
 * Out-of-process `carta-himd` helper launcher, NDJSON event parser, persistent mock session
 * manager, and high-level Hi-MD recording/dumping/editing workflows.
 */

#include "HiMdBridge.h"
#include "NativeCodecs.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <sstream>
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
#include <csignal>
#include <fcntl.h>
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

    if (const char* envPath = std::getenv("CARTA_HIMD_HELPER"); envPath && envPath[0] != '\0') {
        const juce::File f(juce::String::fromUTF8(envPath));
        if (f.existsAsFile()) {
            outPath = f.getFullPathName().toStdString();
            return true;
        }
        error = "CARTA_HIMD_HELPER points to a missing file: " + std::string(envPath);
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
    const auto done = static_cast<std::int64_t>(obj->getProperty("bytes_done"));
    const auto total = static_cast<std::int64_t>(obj->getProperty("bytes_total"));

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
                        const HelperProgressWindow* window, juce::var& outResult,
                        std::string& error) {
    std::string helperPath;
    if (!locateHiMdHelperUnlocked(helperPath, error)) {
        return false;
    }
    if (preferMockSession) {
        if (!persistentMockSession().ensureStarted(helperPath, error)) {
            return false;
        }
        return persistentMockSession().send(sessionFields, window, outResult, error);
    }
    return runOneShotHelper(helperPath, cliArgs, window, outResult, error);
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
    if (!invokeHiMdUnlocked({"list"}, {"list"}, mockSessionEnabledFlag(), nullptr, res, error)) {
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
                             std::string& error) {
    out = {};
    juce::var res;
    const bool useMockSess = isHiMdMockDeviceId(resolvedId);
    if (!invokeHiMdUnlocked({"probe", resolvedId}, {"probe", resolvedId}, useMockSess, nullptr, res,
                            error)) {
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
        out.maxLba = static_cast<std::int64_t>(cap->getProperty("maxLba"));
        out.blockSize = static_cast<int>(cap->getProperty("blockSize"));
        out.deviceSize = static_cast<std::int64_t>(cap->getProperty("deviceSize"));
    }
    if (const auto* fs = st->getProperty("filesystem").getDynamicObject()) {
        out.fsTotalBytes = static_cast<std::int64_t>(fs->getProperty("total"));
        out.fsUsedBytes = static_cast<std::int64_t>(fs->getProperty("used"));
        out.fsFreeBytes = static_cast<std::int64_t>(fs->getProperty("left"));
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
                              nullptr, res, error);
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
                              useMockSess, nullptr, res, error);
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

bool isHiMdTestMockGuardEnabled() noexcept {
    return mockGuardStorage();
}

void setHiMdHelperPathOverride(const std::string& path) {
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
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

bool probeHiMdDevice(const std::string& deviceId, HiMdDeviceStatus& out, std::string& error) {
    const std::lock_guard<std::mutex> guard(hiMdBridgeMutex());
    std::string resolvedId;
    if (!resolveTargetDeviceIdUnlocked(deviceId, resolvedId, error)) {
        return false;
    }
    return probeHiMdDeviceUnlocked(resolvedId, out, error);
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

    std::int64_t totalAudio = 0;
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
        totalAudio += estimateHiMdTrackBytes(codec, tr.totalSamples());
    }

    const std::int64_t trackOverhead =
        static_cast<std::int64_t>(disc.tracks.size()) * kHiMdPerTrackMetaBytes;
    const std::int64_t defaultCap =
        isHiMdMedium(disc.medium) ? hiMdMediumCapacityBytes(disc.medium)
                                  : hiMdMediumCapacityBytes(MdMedium::himd300);

    report.tracksToUpload = static_cast<int>(disc.tracks.size());
    report.estimatedAudioBytes = totalAudio;
    report.requiredBytes = totalAudio + trackOverhead + kHiMdMetadataReserveBytes;
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
                                cliArgs, useMockSess, &win, res, error)) {
            return false;
        }
        if (const auto* uObj =
                res.getDynamicObject()
                    ? res.getDynamicObject()->getProperty("upload").getDynamicObject()
                    : nullptr) {
            totalBlocksWritten += static_cast<std::int64_t>(uObj->getProperty("blocksWritten"));
            const auto bw = static_cast<std::int64_t>(uObj->getProperty("bytesWritten"));
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

    ScopedHiMdProgress progressScope;
    HelperProgressWindow win;
    win.active = true;
    win.operation = "dump";
    win.fallbackPhase = "Dumping track from Hi-MD disc...";
    win.baseProgress = 0.0;
    win.spanProgress = 1.0;

    const bool useMockSess = isHiMdMockDeviceId(resolvedId);
    std::vector<std::string> cliArgs{"dump",    resolvedId, "--track", std::to_string(trackIndex),
                                     "--out",   outPath};
    if (rawAudio) {
        cliArgs.emplace_back("--raw");
    }
    juce::var res;
    return invokeHiMdUnlocked(
        {"dump", resolvedId, std::to_string(trackIndex), outPath, rawAudio ? "1" : "0"}, cliArgs,
        useMockSess, &win, res, error);
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
                              cliArgs, useMockSess, nullptr, res, error);
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
                              cliArgs, useMockSess, nullptr, res, error);
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
                              useMockSess, nullptr, res, error);
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
                              useMockSess, &win, res, error);
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

    std::int64_t timelineCursor = 0;
    outDisc.tracks.reserve(status.tracks.size());
    outDisc.loadedDisc.regions.reserve(status.tracks.size());
    for (const auto& tr : status.tracks) {
        const auto codec = codecFromHiMdTrack(tr.encoding, tr.bitrate);
        const std::int64_t samples = std::max<std::int64_t>(
            1, std::llround(tr.durationSeconds * static_cast<double>(kMdSampleRate)));

        MdTrack mt;
        mt.id = makeMdTrackId();
        mt.halfWidthTitle = tr.title;
        mt.artist = tr.artist;
        mt.album = tr.album;
        mt.codecOverride = codec;
        mt.parts = {{timelineCursor, timelineCursor + samples}};
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
