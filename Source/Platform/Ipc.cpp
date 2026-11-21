/**
 * Unix domain socket IPC server, CLI client, and headless daemon control.
 *
 * Implements the single-instance per-user automation endpoint (`control.sock`
 * guarded by a `0700` directory and non-blocking `flock` lockfile), newline-
 * delimited UTF-8 JSON framing with 3 s socket timeouts, 1 MiB server / 64 MiB
 * client line limits, 2 s message-thread dispatch timeout via `IpcRequestGate`,
 * client working-directory path resolution, and `--auto-start` / `daemon`
 * process management on macOS and Linux.
 */

#include "Platform/Ipc.h"
#include <juce_events/juce_events.h>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <thread>
#include <atomic>
#include <future>
#include <chrono>
#if JUCE_MAC || JUCE_LINUX
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>
#endif

namespace carta {
namespace {

const char* helpPart1 = R"(Carta Studio
Usage:
  CartaStudio [audio-file ...]       Launch the editor and queue audio imports
  CartaStudio --help                 Show this command tree
  CartaStudio --headless             Start without a visible window or Dock icon (agent mode)
  CartaStudio daemon start|stop|status
                                    Manage the background headless Carta Studio daemon
  CartaStudio ipc --auto-start COMMAND [ARGS...]
                                    Start the headless daemon if needed, then run IPC command
  CartaStudio ipc status             JSON session, transport, audition and activity state
  CartaStudio ipc formats            Registered import readers and file extensions
  CartaStudio ipc snap status | grid INTERVAL | mode off|grid|boundaries
  CartaStudio ipc snap target clip-edges|markers|selection|playhead|timeline-start on|off
                                    Grid interval: samples or quoted time
  CartaStudio ipc layers|tracks      JSON layers and clips (zero-based indices)
                                    Clip sample bounds are half-open, or null if empty
  CartaStudio ipc markers            JSON marker list with sample positions and repair flags
  CartaStudio ipc disc status        JSON Audio Pool, Play Order, coalesced spans, tracks, and groups
  CartaStudio ipc disc view tracks|map|status
  CartaStudio ipc disc medium md60|md74|md80|himd300|himd1gb
  CartaStudio ipc disc codec sp-stereo|sp-mono|lp2|lp4|hi-sp|hi-lp|pcm
  CartaStudio ipc disc auto-track on|off
  CartaStudio ipc disc title HALF_WIDTH [FULL_WIDTH]
  CartaStudio ipc disc title-forms HALF_WIDTH FULL_WIDTH
  CartaStudio ipc disc timestamp [ISO|clear]
  CartaStudio ipc disc sync clips|layer1|markers
  CartaStudio ipc disc track add START END [TITLE] [INDEX] [CODEC]
  CartaStudio ipc disc track add-selection [INDEX]
  CartaStudio ipc disc track remove INDEX
  CartaStudio ipc disc track move INDEX DEST_INDEX
  CartaStudio ipc disc track move-block FROM_START FROM_END DEST_START
  CartaStudio ipc disc track duplicate INDEX [COUNT]
  CartaStudio ipc disc track clone INDEX DEST_INDEX
  CartaStudio ipc disc track alias INDEX DEST_INDEX
  CartaStudio ipc disc track alias-audio AUDIO_ID DEST_INDEX
  CartaStudio ipc disc track detach INDEX
  CartaStudio ipc disc track range INDEX START END
  CartaStudio ipc disc track split INDEX SAMPLE
  CartaStudio ipc disc track merge INDEX
  CartaStudio ipc disc track rename INDEX TITLE
  CartaStudio ipc disc track title-forms INDEX HALF_WIDTH FULL_WIDTH
  CartaStudio ipc disc track metadata INDEX ARTIST ALBUM
  CartaStudio ipc disc track flags INDEX MODE|clear|keep WP|keep SCMS|keep EMP|keep [TS|clear|keep]
  CartaStudio ipc disc group set START_TRACK END_TRACK TITLE [FULL_WIDTH]
  CartaStudio ipc disc group clear START_TRACK END_TRACK
  CartaStudio ipc disc focus TRACK_INDEX
  CartaStudio ipc disc preview [TRACK_INDEX | from TRACK_INDEX]
  CartaStudio ipc meter status|reset  Inspect or reset held output sample peaks
  CartaStudio ipc history list       List undo/redo targets and generation
  CartaStudio ipc history undo|redo STEPS GENERATION REVISION
                                    Restore a target from the history list
  CartaStudio ipc master-tracks      JSON marker-defined tracks and sample ranges
  CartaStudio ipc master-track rename INDEX NAME
                                    Rename a marker-defined track
  CartaStudio ipc master-track select INDEX all|tracks
                                    Select its exact time on all or selected tracks
  CartaStudio ipc import [--layer INDEX | --new-layer] [--at SAMPLE] [--marker | --no-marker] [--] FILE [...]
                                    Queue audio imports (default: append on Layer 1 + auto MD Track)
  CartaStudio ipc imports status|cancel|clear-issues
                                    Inspect or cancel pending imports and review issues
  CartaStudio ipc render selected FILE
                                    Render selected time and tracks to a new muted track
  CartaStudio ipc render tracks FILE
                                    Render selected tracks for the full master duration
  CartaStudio ipc render stereo-pair LEFT_TRACK RIGHT_TRACK FILE
                                    Render the mono clips' span as a new muted stereo track
  CartaStudio ipc render clips FILE
                                    Render selected clips over their complete span
  CartaStudio ipc render consolidate FILE [allow-routing-change]
                                    Render and replace selected clips on one track
                                    Optional flag accepts changed later routing of mixed sources
  CartaStudio ipc sources verify all | verify track INDEX
  CartaStudio ipc sources relink TRACK CLIP FILE
  CartaStudio ipc sources status | cancel
                                    Verify or relink sources in the background
  CartaStudio ipc analysis quiet THRESHOLD_DB MIN_SAMPLES
  CartaStudio ipc analysis source-silence TRACK CLIP MIN_SAMPLES
                                    Find conservative all-channel source silence
  CartaStudio ipc analysis levels master|selection|tracks | track INDEX
                                    Measure sample peak and RMS in the background
  CartaStudio ipc analysis locate-peak mono|left|right
  CartaStudio ipc analysis locate-over mono|left|right
                                    Stop and seek to a nonzero measured sample peak
  CartaStudio ipc analysis zero-crossings any|all
  CartaStudio ipc analysis status|cancel
  CartaStudio ipc analysis apply-markers [GAP_INDEX ...]
                                    Apply all unmarked gaps or selected zero-based indices
  CartaStudio ipc analysis select INDEX
                                    Select a detected gap (source silence selects its track)
  CartaStudio ipc analysis detach INDEX
                                    Detach a source-silence interior from its scanned clip
  CartaStudio ipc analysis detach-all
                                    Detach all available interiors as one undoable edit
  CartaStudio ipc analysis audition INDEX [start|middle|end]
                                    Audition a detected gap boundary or midpoint in the full mix
  CartaStudio ipc export wav FILE   Export full master (32-bit float by default)
  CartaStudio ipc export disc-review FILE
                                    Export MiniDisc playback order and track cues as 32-bit float WAV
  CartaStudio ipc export clip|clips FILE
                                    Export the focused clip or selected clip group
  CartaStudio ipc export close-gap focused|tracks|all FILE
                                    Export a proposed gap closure with context
  CartaStudio ipc export crossfade selected|same|across FILE
                                    Export a proposed crossfade with audition context
  CartaStudio ipc export track INDEX FILE
                                    Export one marker-defined track as float WAV
  CartaStudio ipc export tracks PARENT
                                    Export all marker-defined tracks to a new folder
  CartaStudio ipc export selection full|tracks FILE
                                    Export selected range, full mix or selected tracks
                                    Other WAV export commands accept pcm16|pcm24|float32,
                                    dither, cues, and allow-clip after FILE.
                                    PCM clipping fails unless allow-clip is present.
  CartaStudio ipc export cancel     Request cancellation of an IPC export
  CartaStudio ipc project save-copy FILE
  CartaStudio ipc project save       Save the current named project
  CartaStudio ipc project save-as FILE
                                    Save to a new path and make it current
  CartaStudio ipc project package-as FILE
                                    Save an editable self-contained package
  CartaStudio ipc project status|cancel
                                    Inspect or cancel an IPC project operation
  CartaStudio ipc project open FILE [ORIGINAL REPLACEMENT ...]
                                    Open a clean project with optional verified source relinks
  CartaStudio ipc seek SAMPLE        Move playhead to a project sample
  CartaStudio ipc view status|fit
  CartaStudio ipc view capture FILE | capture status
                                    Capture the editor component to a new PNG
  CartaStudio ipc view master-track INDEX
  CartaStudio ipc view range START_SAMPLE END_SAMPLE
  CartaStudio ipc view default waveform|spectrogram
  CartaStudio ipc view waveform-rms on|off
  CartaStudio ipc view waveform-zoom 1|2|4|8
  CartaStudio ipc view waveform-scale linear|decibels
  CartaStudio ipc view waveform-shape full|half
  CartaStudio ipc view follow off|page|pinned|resume
  CartaStudio ipc view spectrum-range LOW_HZ HIGH_HZ
  CartaStudio ipc view spectrum-preset full|bass|voice|treble
  CartaStudio ipc view spectrum-window hann|hamming|blackman
  CartaStudio ipc view spectrum-floor -40|-60|-80|-100|-120
  CartaStudio ipc view spectrum-fft 1024|2048|4096|8192
                                    Inspect or change the timeline viewport
  CartaStudio ipc loop set START END | from-selection | play | clear
                                    Manage the independent full-mix loop region
  CartaStudio ipc select memory status|remember|restore|clear
  CartaStudio ipc select clip TRACK CLIP
  CartaStudio ipc select focus-clip TRACK CLIP
                                    Focus without changing selections or playback
  CartaStudio ipc select focus-track TRACK
                                    Focus without changing selections or playback
  CartaStudio ipc select clips TRACK CLIP [TRACK CLIP ...]
                                    Select independent whole clips
  CartaStudio ipc select source-clips [all|tracks|range]
                                    Select clips using the focused source
  CartaStudio ipc select clip-scope all|tracks|range
                                    Select whole clips on all or selected tracks,
                                    or those intersecting the time selection
  CartaStudio ipc select clip-span  Select their combined time on their tracks
  CartaStudio ipc select clip-time all|visible|before|after
                                    Select focused clip time on its track
  CartaStudio ipc select point TRACK SAMPLE
                                    Place an edit point on a track
  CartaStudio ipc select point-gap [selected|all|tracks INDEX ...]
                                    Select a common clip gap around the edit point
  CartaStudio ipc select point-boundary start|end|previous-marker|next-marker|previous-clip|next-clip|previous-clip-all|next-clip-all
                                    Move the point to a project boundary
  CartaStudio ipc select point-range start|end|previous-marker|next-marker|previous-clip|next-clip|previous-clip-all|next-clip-all
                                    Select time from the point to a boundary
  CartaStudio ipc select range FIRST_TRACK LAST_TRACK START_SAMPLE END_SAMPLE
                                    Select half-open sample range
)";

const char* helpPart2 = R"(  CartaStudio ipc select tracks START_SAMPLE END_SAMPLE TRACK [TRACK ...]
                                    Select a range on independent tracks
  CartaStudio ipc edit move-range TARGET_TRACK START_SAMPLE
                                    Move selected time, mixing destination overlaps
  CartaStudio ipc edit move-range-impact
                                    Count groups linked to audio outside selected time or tracks
  CartaStudio ipc edit close-gap|cut-close-gap|insert-gap focused|tracks|all
                                    Delete, cut or insert selected-length time
  CartaStudio ipc edit split|delete|undo|redo
  CartaStudio ipc edit copy|cut       Copy or cut selected time range
  CartaStudio ipc edit duplicate-time|trim-time
                                    Duplicate selected time or trim tracks outside it
  CartaStudio ipc edit group|ungroup  Group selected clips or dissolve touched groups
  CartaStudio ipc edit crossfade-selected
                                    Crossfade exactly two selected clips
  CartaStudio ipc edit sequence|distribute
                                    Arrange selected clips, treating groups as units
  CartaStudio ipc edit slip-clip TRACK CLIP SIGNED_SAMPLES
                                    Shift one clip or its persistent group
  CartaStudio ipc edit slip-clips SIGNED_SAMPLES
                                    Shift selected source audio within fixed clip boundaries
  CartaStudio ipc edit trim-group start|end SIGNED_SAMPLES
                                    Move selected clip edges together
  CartaStudio ipc edit trim-group-range TRACK CLIP START|keep END|keep
                                    Set an anchor range; shift selected edges together
  CartaStudio ipc edit align-group start|end
                                    Trim clip/group anchors to the playhead
  CartaStudio ipc edit join-next
  CartaStudio ipc edit split-to-new-tracks
  CartaStudio ipc edit split-tracks selected|all
  CartaStudio ipc edit move-clip TRACK CLIP START_SAMPLE
  CartaStudio ipc edit trim-clip TRACK CLIP START|keep END|keep
  CartaStudio ipc edit rename-clip TRACK CLIP NAME
  CartaStudio ipc edit align-clips TRACK CLIP start|end SAMPLE
                                    Align an anchor by moving all selected clips
  CartaStudio ipc edit clips copy|cut|delete|duplicate|split|join
                                    Operate on selected whole clips
  CartaStudio ipc edit paste mix|replace|insert|insert-all
                                    Paste at playhead using the chosen policy
  CartaStudio ipc edit paste-point mix|replace|insert|insert-all
                                    Paste at the selected edit point
  CartaStudio ipc marker add SAMPLE NAME
  CartaStudio ipc marker add-point NAME
                                    Add a marker at the selected edit point
  CartaStudio ipc marker find ID     Resolve a persistent marker identity to its current index
  CartaStudio ipc marker focus INDEX|id:ID
                                    Focus a marker without seeking or changing audio
  CartaStudio ipc marker focus-adjacent previous|next
                                    Focus a neighboring marker without seeking
  CartaStudio ipc marker audition-focused
                                    Audition the full mix around the focused marker
  CartaStudio ipc marker set INDEX|id:ID sample SAMPLE | name NAME
  CartaStudio ipc marker nudge INDEX|id:ID -1|1
                                    Move or rename an existing marker
  CartaStudio ipc marker from-clips start|end
                                    Add markers at selected clip boundaries
  CartaStudio ipc marker import|export json|labels FILE
                                    Replace markers from a file, or export to a new file
  CartaStudio ipc marker exchange status|cancel
                                    Inspect or cancel an IPC marker exchange
  CartaStudio ipc master mono|stereo
  CartaStudio ipc master duration [MINIMUM_SAMPLES]
  CartaStudio ipc layer|track add [NAME]
  CartaStudio ipc layer|track rename INDEX NAME
  CartaStudio ipc layer|track duplicate|remove INDEX
  CartaStudio ipc layer|track move INDEX DESTINATION_INDEX
                                    Edit mixing layers using zero-based indices (track is an alias)
  CartaStudio ipc track set INDEX mute|solo on|off
  CartaStudio ipc track set-selected mute|solo on|off
                                    Set selected rows together; otherwise use focused track
  CartaStudio ipc track set-selected gain VALUE
                                    Set selected track gains to the same linear value (0..16)
  CartaStudio ipc track set-selected gain-db DB|-inf
                                    Set selected gains in dB; -inf is silence
  CartaStudio ipc track set-selected balance VALUE
                                    Set selected track balances (-1 left to 1 right)
  CartaStudio ipc track scale-gain FACTOR
  CartaStudio ipc track scale-gain-db DB|-inf
                                    Scale selected gains; each result must be at most 16
  CartaStudio ipc track set INDEX gain|balance VALUE
                                    Linear gain 0..16; balance -1..1
  CartaStudio ipc track set INDEX view default|waveform|spectrogram|combined
  CartaStudio ipc track set INDEX height PIXELS
  CartaStudio ipc track set INDEX waveform-zoom default|1|2|4|8
  CartaStudio ipc track set INDEX channel-heights equal|WEIGHTS
                                    Comma-separated relative source-channel heights
                                    Track height 72..800; expands collapsed tracks
  CartaStudio ipc track routing INDEX LEFT RIGHT [LEFT RIGHT ...]
  CartaStudio ipc track routing INDEX clear
                                    Stereo matrix in source-channel order
  CartaStudio ipc track stereo INDEX swap|fold
                                    Swap stereo source channels or fold routing to mono
  CartaStudio ipc track channels INDEX split|combine
                                    Split stereo source channels or recombine matching adjacent tracks
  CartaStudio ipc clip set TRACK CLIP gain VALUE
  CartaStudio ipc clip set TRACK CLIP fade-in|fade-out SAMPLES
                                    Linear gain 0..16; fades in project samples
  CartaStudio ipc clip set TRACK CLIP fade-in-shape|fade-out-shape SHAPE
  CartaStudio ipc clip fade-shape fade-in|fade-out|both SHAPE
                                    SHAPE: linear|equal-power|s-curve|exponential
  CartaStudio ipc clip envelope-selected GAIN|keep FADE_IN_SAMPLES|keep FADE_OUT_SAMPLES|keep
                                    Atomically set selected clip gain and edge-anchored fades
  CartaStudio ipc edit amplify DB    Atomically amplify selected clips or range in dB (<= +24.08 dB)
  CartaStudio ipc edit normalize [TARGET_DB] [linked|independent]
                                    Atomically normalize selected clips or range (default -1 dBFS, linked)
  CartaStudio ipc marker set-positions INDEX|id:ID SAMPLE [INDEX|id:ID SAMPLE ...]
                                    Atomically reposition selected markers
  CartaStudio ipc marker rename-many INDEX|id:ID NAME [INDEX|id:ID NAME ...]
                                    Atomically rename selected markers
  CartaStudio ipc marker shift-many SIGNED_SAMPLES INDEX|id:ID [INDEX|id:ID ...]
                                    Shift selected markers without crossings or collisions
  CartaStudio ipc marker delete INDEX|id:ID
  CartaStudio ipc marker delete-many INDEX|id:ID [INDEX|id:ID ...]
  CartaStudio ipc marker delete-selection
                                    Delete markers in the selected half-open time range
  CartaStudio ipc transport play     Start or resume playback
  CartaStudio ipc transport pause    Pause playback
  CartaStudio ipc transport stop     Stop and return to the start
  CartaStudio ipc transport stop-here  Stop audition; edit previews restore the original playhead
  CartaStudio ipc transport reset-interruptions
                                    Reset edit-lock silent-block counter
  CartaStudio ipc transport context [BEFORE_SAMPLES AFTER_SAMPLES]
                                    Inspect or set audition context
  CartaStudio ipc transport selection-context full|tracks
                                    Play selection with audition context
  CartaStudio ipc transport audition-selection full|tracks once|loop
                                    Play or loop the selected time
  CartaStudio ipc transport audition-point
                                    Audition around the edit point
  CartaStudio ipc transport audition-close-gap focused|tracks|all
                                    Play a proposed gap closure with context
  CartaStudio ipc transport audition-crossfade selected|same|across
                                    Play a proposed crossfade with context
  CartaStudio ipc transport audition-clips once|loop
                                    Play or loop selected clips
  CartaStudio ipc view resize WIDTH HEIGHT
                                    Resize workspace window (WIDTH 600..4000, HEIGHT 350..2500)
  CartaStudio ipc schema [DOMAIN]    Machine-readable JSON command schema
  CartaStudio ipc utoc budget|inspect
                                    Inspect physical 2352-byte UTOC sectors and slot budgets
  CartaStudio ipc utoc export|import FILE
                                    Export 9408-byte or import 2352/9408-byte UTOC sector bundle
  CartaStudio ipc encode sp-stereo|sp-mono|lp2|lp4|hi-sp|hi-lp INPUT_WAV OUTPUT_FILE
                                    Encode PCM WAV with Porffor native ATRAC codecs
  CartaStudio ipc netmd devices|status|preflight [DEVICE_ID]
  CartaStudio ipc netmd read-utoc [DEVICE_ID] [--out FILE]
  CartaStudio ipc netmd erase|write-utoc|record [DEVICE_ID] [confirm-hardware]
  CartaStudio ipc app quit [force]   Quit the running GUI or headless daemon

Track, clip and marker indices are zero-based. Samples use the 44100 Hz project
rate. Editing commands use the same undo history as the graphical editor.
IPC commands require a running Carta Studio instance. File paths are resolved
relative to the client working directory. JSON results contain ok and result or
error; a nonzero exit status indicates failure. Imports run asynchronously;
inspect status for pending imports, import issues, and export completion. Exports
never replace an existing file. Only one instance per user
can own the default automation endpoint. Set CARTA_STUDIO_IPC_DIR to the same
absolute private directory for an isolated editor and its clients. macOS and
Linux are supported.
)";

juce::var response(bool ok, const juce::var& value) {
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", ok);
    o->setProperty(ok ? "result" : "error", value);
    return juce::var(o);
}

#if JUCE_MAC || JUCE_LINUX
juce::String socketPath() {
    const auto directory = juce::SystemStats::getEnvironmentVariable("CARTA_STUDIO_IPC_DIR", {});
    const auto base = directory.isEmpty()
                          ? "/tmp/carta-studio-" + juce::String(static_cast<juce::int64>(getuid()))
                          : directory;
    return base + "/control.sock";
}

bool address(sockaddr_un& addr, const juce::String& socket) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    const auto path = socket.toStdString();
    if (path.empty() || path.front() != '/' || path.size() >= sizeof(addr.sun_path)) {
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

void configure(int fd) {
    timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#if defined(SO_NOSIGPIPE)
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

bool sendLine(int fd, const juce::String& text) {
    const auto bytes = (text + "\n").toStdString();
    size_t sent = 0;
    while (sent < bytes.size()) {
#if defined(MSG_NOSIGNAL)
        const auto n = send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#else
        const auto n = send(fd, bytes.data() + sent, bytes.size() - sent, 0);
#endif
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool readLine(int fd, juce::String& line, size_t limit) {
    std::string result;
    char buffer[8192];
    for (;;) {
        const auto n = recv(fd, buffer, sizeof(buffer), 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        const auto count = static_cast<size_t>(n);
        const auto* newline = static_cast<const char*>(std::memchr(buffer, '\n', count));
        const auto length = newline ? static_cast<size_t>(newline - buffer) : count;
        if (length > limit - result.size()) {
            return false;
        }
        result.append(buffer, length);
        if (newline) {
            line = juce::String::fromUTF8(result.data(), static_cast<int>(result.size()));
            return true;
        }
    }
}

bool sendSingleIpc(const juce::StringArray& commandArgs, juce::String& lineOut) {
    juce::Array<juce::var> arr;
    for (const auto& item : commandArgs) {
        arr.add(item);
    }
    sockaddr_un addr{};
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || !address(addr, socketPath()) ||
        connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    configure(fd);
    const bool ok = sendLine(fd, juce::JSON::toString(juce::var(arr), true)) &&
                    readLine(fd, lineOut, 64 * 1024 * 1024);
    close(fd);
    return ok;
}

/**
 * Determine whether a non-`import` IPC argument is a filesystem path that
 * should be resolved relative to the client's working directory.
 */
bool isFixedPathArgument(const juce::String& command, const juce::String& subcommand, int offset,
                         const char* previousArg) {
    const bool exportPath =
        command == "export" &&
        ((offset == 2 && (subcommand == "wav" || subcommand == "clip" || subcommand == "clips" ||
                          subcommand == "tracks" || subcommand == "disc-review")) ||
         (offset == 3 && (subcommand == "track" || subcommand == "selection" ||
                          subcommand == "crossfade" || subcommand == "close-gap")));
    const bool projectPath =
        command == "project" && (offset == 2 || (subcommand == "open" && offset >= 3));
    const bool renderPath =
        command == "render" && (subcommand == "stereo-pair" ? offset == 4 : offset == 2);
    const bool markerPath =
        command == "marker" && (subcommand == "import" || subcommand == "export") && offset == 3;
    const bool sourcesPath = command == "sources" && subcommand == "relink" && offset == 4;
    const bool utocPath =
        command == "utoc" && (subcommand == "export" || subcommand == "import") && offset == 2;
    const bool encodePath = command == "encode" && (offset == 2 || offset == 3);
    const bool netmdPath = command == "netmd" && subcommand == "read-utoc" && offset >= 3 &&
                           juce::String::fromUTF8(previousArg) == "--out";
    return exportPath || projectPath || renderPath || markerPath || sourcesPath || utocPath ||
           encodePath || netmdPath;
}

juce::Array<juce::var> buildIpcArgs(int argc, char* argv[], int cmdIndex) {
    juce::Array<juce::var> args;
    const juce::String command = juce::String::fromUTF8(argv[cmdIndex]);
    const juce::String subcommand =
        cmdIndex + 1 < argc ? juce::String::fromUTF8(argv[cmdIndex + 1]) : juce::String();
    bool importEndOfOptions = false;
    bool importExpectValue = false;

    for (int i = cmdIndex; i < argc; ++i) {
        juce::String a = juce::String::fromUTF8(argv[i]);
        const int offset = i - cmdIndex;
        bool importPath = false;
        if (command == "import" && offset >= 1) {
            if (importExpectValue) {
                importExpectValue = false;
            } else if (!importEndOfOptions && a == "--") {
                importEndOfOptions = true;
            } else if (!importEndOfOptions && (a == "--layer" || a == "--at")) {
                importExpectValue = true;
            } else if (!importEndOfOptions && a.startsWith("--")) {
            } else {
                importPath = true;
            }
        }

        const bool path =
            importPath || (i > 0 && isFixedPathArgument(command, subcommand, offset, argv[i - 1]));
        if (path) {
            a = juce::File::getCurrentWorkingDirectory().getChildFile(a).getFullPathName();
        }
        args.add(a);
    }
    return args;
}

bool spawnHeadlessDaemon(const char* argv0, juce::String& error) {
    const auto selfFile = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto exePath = selfFile.existsAsFile() ? selfFile.getFullPathName().toStdString()
                                                 : std::string(argv0 ? argv0 : "");
    if (exePath.empty()) {
        error = "Cannot determine Carta Studio executable path.";
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        error = "Failed to fork headless daemon process.";
        return false;
    }
    if (pid == 0) {
        setsid();
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        const std::string flag = "--headless";
        std::vector<char> exeBuf(exePath.begin(), exePath.end());
        exeBuf.push_back('\0');
        std::vector<char> flagBuf(flag.begin(), flag.end());
        flagBuf.push_back('\0');
        char* childArgv[]{exeBuf.data(), flagBuf.data(), nullptr};
        execv(exePath.c_str(), childArgv);
        _exit(127);
    }

    for (int attempt = 0; attempt < 100; ++attempt) {
        juce::String probe;
        if (sendSingleIpc({"status"}, probe)) {
            return true;
        }
        int statusCode = 0;
        const pid_t dead = waitpid(pid, &statusCode, WNOHANG);
        if (dead == pid) {
            error = "Headless daemon exited unexpectedly before initializing IPC.";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    error = "Timed out waiting for headless daemon IPC socket.";
    return false;
}

int runDaemonAction(const juce::String& action, const char* argv0) {
    if (action == "status") {
        juce::String line;
        const bool running = sendSingleIpc({"status"}, line);
        auto* o = new juce::DynamicObject();
        o->setProperty("running", running);
        o->setProperty("socket", socketPath());
        if (running) {
            o->setProperty("status", juce::JSON::parse(line)["result"]);
        }
        std::puts(juce::JSON::toString(response(true, juce::var(o)), true).toRawUTF8());
        return 0;
    }

    if (action == "start") {
        juce::String line;
        if (sendSingleIpc({"status"}, line)) {
            auto* o = new juce::DynamicObject();
            o->setProperty("running", true);
            o->setProperty("already_running", true);
            o->setProperty("socket", socketPath());
            std::puts(juce::JSON::toString(response(true, juce::var(o)), true).toRawUTF8());
            return 0;
        }
        juce::String err;
        if (!spawnHeadlessDaemon(argv0, err)) {
            std::puts(juce::JSON::toString(response(false, err), true).toRawUTF8());
            return 1;
        }
        auto* o = new juce::DynamicObject();
        o->setProperty("running", true);
        o->setProperty("already_running", false);
        o->setProperty("socket", socketPath());
        std::puts(juce::JSON::toString(response(true, juce::var(o)), true).toRawUTF8());
        return 0;
    }

    if (action == "stop") {
        juce::String line;
        if (!sendSingleIpc({"app", "quit", "force"}, line)) {
            auto* o = new juce::DynamicObject();
            o->setProperty("running", false);
            o->setProperty("stopped", false);
            o->setProperty("socket", socketPath());
            std::puts(juce::JSON::toString(response(true, juce::var(o)), true).toRawUTF8());
            return 0;
        }
        bool stopped = false;
        for (int i = 0; i < 60; ++i) {
            juce::String probe;
            if (!sendSingleIpc({"status"}, probe)) {
                stopped = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!stopped) {
            std::puts(juce::JSON::toString(
                          response(false, "Timed out waiting for headless daemon to stop."), true)
                          .toRawUTF8());
            return 1;
        }
        auto* o = new juce::DynamicObject();
        o->setProperty("running", false);
        o->setProperty("stopped", true);
        o->setProperty("socket", socketPath());
        std::puts(juce::JSON::toString(response(true, juce::var(o)), true).toRawUTF8());
        return 0;
    }

    std::fputs("Usage: CartaStudio daemon start|stop|status\n", stderr);
    return 2;
}

int runIpcCommand(int argc, char* argv[], int cmdIndex, bool autoStart) {
    const auto args = buildIpcArgs(argc, argv, cmdIndex);
    sockaddr_un addr{};
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    juce::String line;
    const bool validAddr = fd >= 0 && address(addr, socketPath());
    bool connected =
        validAddr && connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;

    if (!connected && autoStart) {
        if (fd >= 0) {
            close(fd);
        }
        juce::String err;
        if (!spawnHeadlessDaemon(argv[0], err)) {
            std::fprintf(stderr, "Cannot auto-start Carta Studio: %s\n", err.toRawUTF8());
            return 1;
        }
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        connected = fd >= 0 && address(addr, socketPath()) &&
                    connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }

    if (!connected) {
        if (fd >= 0) {
            close(fd);
        }
        std::fputs("Cannot connect to Carta Studio; start the editor first.\n", stderr);
        return 1;
    }

    configure(fd);
    const bool ok = sendLine(fd, juce::JSON::toString(juce::var(args), true)) &&
                    readLine(fd, line, 64 * 1024 * 1024);
    close(fd);
    if (!ok) {
        std::fputs("IPC request failed; command outcome may be unknown.\n", stderr);
        return 1;
    }

    std::puts(line.toRawUTF8());
    const auto result = juce::JSON::parse(line);
    return static_cast<bool>(result.getProperty("ok", false)) ? 0 : 1;
}
#endif

} // namespace

struct IpcServer::Impl {
    juce::String failure;

#if JUCE_MAC || JUCE_LINUX
    int listener = -1;
    int lockfd = -1;
    std::atomic<bool> stopping{false};
    std::thread worker;
    juce::String endpoint;
    std::function<juce::var(const juce::StringArray&)> handler;

    explicit Impl(std::function<juce::var(const juce::StringArray&)> fn,
                  const juce::File& socketDirectory)
        : endpoint(socketDirectory == juce::File{}
                       ? socketPath()
                       : socketDirectory.getChildFile("control.sock").getFullPathName()),
          handler(std::move(fn)) {
        if (!openEndpoint()) {
            return;
        }
        worker = std::thread([this] { run(); });
    }

    bool openEndpoint() {
        if (!endpoint.startsWithChar('/')) {
            failure = "IPC directory must be an absolute path";
            return false;
        }
        const auto dir = juce::File(endpoint).getParentDirectory().getFullPathName().toStdString();
        if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
            failure = "Cannot create IPC directory";
            return false;
        }

        struct stat st{};
        if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
            (st.st_mode & 077) != 0) {
            failure = "IPC directory must be private and owned by the current user";
            return false;
        }

        const auto lockpath = dir + "/lock";
        lockfd = open(lockpath.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
        if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
            failure = "Another instance owns the IPC endpoint";
            return false;
        }

        sockaddr_un addr{};
        if (!address(addr, endpoint)) {
            failure = "IPC path is too long";
            return false;
        }
        listener = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listener < 0) {
            failure = "Cannot create IPC socket";
            return false;
        }
        unlink(addr.sun_path);
        if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            chmod(addr.sun_path, 0600) != 0 || listen(listener, 8) != 0) {
            failure = "Cannot bind IPC socket";
            close(listener);
            listener = -1;
            unlink(addr.sun_path);
            return false;
        }
        return true;
    }

    void handleClient(int fd) {
        juce::String input;
        if (!readLine(fd, input, 1024 * 1024)) {
            return;
        }

        auto parsed = juce::JSON::parse(input);
        juce::StringArray args;
        if (auto* a = parsed.getArray()) {
            for (const auto& v : *a) {
                args.add(v.toString());
            }
        }
        if (!parsed.isArray() || args.isEmpty()) {
            sendLine(fd, juce::JSON::toString(response(false, "Invalid request"), true));
            return;
        }

        // The result can contain live message-thread-owned objects (for
        // example export progress). Serialize before releasing the
        // message thread so workers cannot observe a partial update.
        auto promise = std::make_shared<std::promise<juce::String>>();
        auto future = promise->get_future();
        auto callback = handler;
        auto gate = std::make_shared<IpcRequestGate>();

        const bool posted = juce::MessageManager::callAsync([promise, callback, args, gate] {
            if (gate->tryStart()) {
                promise->set_value(juce::JSON::toString(callback(args), true));
            }
        });
        if (!posted) {
            sendLine(fd,
                     juce::JSON::toString(
                         response(false, "Editor is unavailable; command was not executed"), true));
            return;
        }

        if (future.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
            sendLine(fd, future.get());
        } else {
            sendLine(fd,
                     juce::JSON::toString(
                         response(false, gate->cancelIfQueued()
                                             ? "Editor is busy; command was not executed"
                                             : "Editor is busy; command outcome may be unknown"),
                         true));
        }
    }

    void run() {
        while (!stopping.load()) {
            const int fd = accept(listener, nullptr, nullptr);
            if (fd < 0) {
                if (stopping.load()) {
                    break;
                }
                continue;
            }
            configure(fd);
            handleClient(fd);
            close(fd);
        }
    }

    ~Impl() {
        stopping = true;
        if (listener >= 0) {
            shutdown(listener, SHUT_RDWR);
            close(listener);
        }
        if (worker.joinable()) {
            worker.join();
        }
        if (lockfd >= 0) {
            if (failure.isEmpty()) {
                unlink(endpoint.toRawUTF8());
            }
            close(lockfd);
        }
    }
#else
    explicit Impl(std::function<juce::var(const juce::StringArray&)>, const juce::File&) {
        failure = "IPC is not yet supported on this platform";
    }
#endif
};

IpcServer::IpcServer(std::function<juce::var(const juce::StringArray&)> h,
                     juce::File socketDirectory)
    : impl(std::make_unique<Impl>(std::move(h), socketDirectory)) {
}

IpcServer::~IpcServer() = default;

juce::String IpcServer::error() const {
    return impl->failure;
}

int runIpcClient(int argc, char* argv[]) {
    if (argc < 2) {
        return -1;
    }
    const juce::String first = juce::String::fromUTF8(argv[1]);
    if (first == "--help" || first == "-h") {
        std::fputs(helpPart1, stdout);
        std::fputs(helpPart2, stdout);
        return 0;
    }

    if (first == "daemon") {
#if JUCE_MAC || JUCE_LINUX
        if (argc != 3) {
            std::fputs("Usage: CartaStudio daemon start|stop|status\n", stderr);
            return 2;
        }
        return runDaemonAction(juce::String::fromUTF8(argv[2]), argv[0]);
#else
        std::fputs("Daemon mode is not yet supported on this platform.\n", stderr);
        return 1;
#endif
    }

    if (first != "ipc") {
        return -1;
    }

    int cmdIndex = 2;
    bool autoStart = false;
    if (argc >= 3 && juce::String::fromUTF8(argv[2]) == "--auto-start") {
        autoStart = true;
        cmdIndex = 3;
    }
    if (argc <= cmdIndex || juce::String(argv[cmdIndex]) == "--help") {
        std::fputs(helpPart1, stdout);
        std::fputs(helpPart2, stdout);
        return argc <= cmdIndex ? 2 : 0;
    }

#if JUCE_MAC || JUCE_LINUX
    return runIpcCommand(argc, argv, cmdIndex, autoStart);
#else
    std::fputs("IPC is not yet supported on this platform.\n", stderr);
    return 1;
#endif
}

} // namespace carta
