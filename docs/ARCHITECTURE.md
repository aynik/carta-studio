# Carta Studio Architecture

Carta Studio is a C++17 / JUCE 8 multitrack mastering editor and Rainbow Book
MiniDisc authoring studio. It pairs a 44.1 kHz non-destructive timeline with a
UTOC-native MiniDisc workspace, isolated Porffor-compiled ATRAC codecs
(`carta1`, `carta3`, `carta5`), and USB NetMD hardware transfer (`netmd-js` +
`netmd-exploits`).

## Source Layout

| Directory / File | Responsibility |
| --- | --- |
| `Source/Main.cpp` | `CartaStudioApplication`, window creation, `--headless` mode, CLI IPC client handoff (`runIpcClient`). |
| `Source/Core/` | `Session`, `Track`, `Clip`, `Audio`, `Marker`, `MappedPcm`, `RenderClipIndex`, `WaveformPeakCache` (`16 MiB`/source, shared `WaveformPeakBudget`), 44.1 kHz project-sample grid, fade curves. |
| `Source/Audio/` | Real-time `Transport` (`renderPlayback`, `OutputPeakHold`, `PlaybackClock`), `SincResampler` / `ResamplingContext`, background `Spectrogram` tiles, Linux `StableSpectrumRaster` (`16 MiB` chunk cache), `DeviceSettings`. |
| `Source/Editing/` | Non-destructive timeline, clip, group, marker, and gap operations; `EditHistory` (40 snapshots, `64 MiB` metadata, `256 MiB` RAM media, `4 GiB` mapped PCM); zero-crossing and quiet-gap analysis. |
| `Source/IO/` | `SessionIO` (`.carta` `CARTAP01` single-file packages, legacy JSON, recovery snapshots, WAV export with `pcm16`/`pcm24`/`float32`, deterministic TPDF dither, RIFF cue embedding, Audacity/JSON markers), atomic `CheckedOutput`/`CheckedText`, `FileObservation`, `Mp3Gapless`, optional `NativeMp3Format` (`minimp3`). |
| `Source/MiniDisc/` | `MdAuthoring` (`MdDisc`, `MdTrack`, `MdPart`, `MdGroup`, Sound Group coalescing), `Utoc` (2352-byte Sectors 0/1/2/4 serializer & DAG tail-sharing compiler), `MdWorkspace` (two-tab docked UI), `WorkspacePreferences`, `NativeCodecs`, `NetMdBridge` (out-of-process `carta-netmd` subprocess client). |
| `carta-netmd/` | Standalone **GPL-2.0** NetMD USB helper CLI (`main.cpp`, `NetMdHostBridge.h/.cpp`, `LICENSE`, `README.md`) linking `libnetmd_porffor.dylib` and `libusb-1.0` with zero JUCE dependencies; bundled at `Contents/Helpers/carta-netmd` on macOS. |
| `Source/Platform/` | `Ipc` (Unix domain socket JSON-line client/server, `IpcRequestGate`, `daemon` management), `NativeAlertPolicy`, native macOS AppKit / Linux GTK3 prompts (`NativeSavePrompt.h`, `NativeSavePrompt_mac.mm`, `NativeTextPrompt_linux.cpp`). |
| `Source/Studio/` | `Studio` editor component (`Studio.h`, `StudioLookAndFeel.h`, `StudioDialogs.h`) partitioned across lifecycle, timeline rendering, mouse/drag, menus/keys, editing actions, MiniDisc actions, project I/O, and IPC dispatch. |
| `Tools/` | Node.js Porffor alpha-19 build scripts producing isolated shared libraries under `build/porffor/lib/`. |

## Coordinate Systems and Formats

| Domain | Unit / Layout | Notes |
| --- | --- | --- |
| Project timeline | Half-open `[start, end)` at 44.1 kHz | Clip bounds, time selections, `MdPart`, and WAV ranges use exclusive end samples. |
| Source audio | `MappedPcm` or `AudioBuffer<float>` | Sources >= 64 MiB decode to unlinked temporary mapped PCM files. |
| MiniDisc physical grid | `cluster:sector:group` | 32 sectors/cluster, 11 sound groups/sector (`352` sound groups/cluster); first audio cluster is `1`. |
| Codec frame quanta | 512 (`sp-*`), 1024 (`lp2`/`lp4`) on MD authoring; 2048 (`hi-*`) in `NativeCodecs` | `planMdPhysicalAudio` aligns standard MD parts to 512/1024 quanta and coalesces abutting same-codec ranges; `NativeCodecs` additionally encodes/decodes `hi-sp`/`hi-lp`. |
| UTOC limits | 255 tracks, 255 fragments, 255 HW/FW title cells | Sectors 0/1/2/4 are `2352` bytes each; DAG suffix sharing avoids duplicating physical clusters or fragment chains. |
| Project persistence | `CARTAP01` package / JSON v1–25 | Version 24 stores 16x linear gain & fade curves; version 25 stores `mdDisc` authoring state. |

## Threading and Locking Model

1. **Message thread**: Owns `Session`, `EditHistory`, `TimeSelection`, `ClipSelection`, `MdWorkspace`, `PendingImport` queues, and background task handles. All IPC commands dispatch onto the message thread via `IpcRequestGate`.
2. **Audio callback (`getNextAudioBlock`)**: Real-time lock acquisition (`lock.tryEnter` / `ScopedLock` on `Studio::lock`) reads only the committed `Session`, `RenderClipIndex`, and `ResamplingContext`. Contention never blocks the audio thread; instead, the callback outputs silence and increments `editLockSilentBlocks`.
3. **Prepared off-lock mutations (`prepareRangeSessionEdit`)**: Clones `Session` outside `lock`, validates clip/marker/routing invariants and renders any preflight off-thread, then briefly locks to swap state and push undo history.
4. **Background worker pools**: Audio import decoding + SHA-256 verification, waveform/RMS column caches, spectrogram FFT tiles, level/quiet-gap analysis, project save/open, and WAV export run asynchronously with atomic cancellation flags and generation guards.
5. **NetMD worker (`NetMdWorkerThread`)**: Executes USB NetMD/exploit operations on a dedicated thread with an **8 MiB stack** (`8 * 1024 * 1024` bytes) to safely accommodate Porffor-compiled `netmd-js` + `netmd-exploits` call chains.

## Porffor Native Runtime Architecture

Both codecs and NetMD run ahead-of-time transpiled JavaScript via pinned
Porffor alpha-19 (`0c135ce`), compiled into isolated shared libraries under
`build/porffor/lib/`:

| Dylib | Source | Exported C ABI Prefix | Notes |
| --- | --- | --- | --- |
| `libcarta1_porffor.dylib` | `third_party/carta1` | `carta1_pf_*` | ATRAC1 SP Stereo / SP Mono (`--fast-length`, `-O2`). |
| `libcarta3_porffor.dylib` | `third_party/carta3` | `carta3_pf_*` | ATRAC3 LP2 / LP4 (`--fast-length`, `-O2`). |
| `libcarta5_porffor.dylib` | `third_party/carta5` | `carta5_pf_*` | ATRAC3plus Hi-SP / Hi-LP (`-O1`, no `--fast-length`). |
| `libnetmd_porffor.dylib` | `netmd-js` + `netmd-exploits` | `netmd_pf_*` | Lowered async-to-`__CartaSyncPromise`, host C callbacks (`carta_netmd_host_*`), `-O1`. |

Key invariants enforced by `Tools/build_porffor_codecs.mjs` and
`Tools/build_porffor_netmd.mjs`:

- **Symbol isolation**: Compiled with `-fvisibility=hidden` and verified with `nm -gU` so only `<module>_pf_*` symbols are exported; multiple Porffor runtimes coexist in one process without symbol collisions.
- **Bit-exact floating-point parity**: Built with `-fno-fast-math -ffp-contract=off -fno-math-errno` and `-enable-machine-outliner=never`.
- **Per-runtime locking**: `NativeCodecs.cpp` guards each codec runtime with its own `std::mutex`, and `NetMdBridge.cpp` serializes NetMD runtime calls via `netMdBridgeMutex()`.
- **Process & license isolation (`carta-netmd/`)**: `libnetmd_porffor.dylib` (`netmd-js` + `netmd-exploits`, GPL-2.0) and `libusb-1.0` (LGPL-2.1) are linked **only** by the standalone `carta-netmd` CLI executable (`carta-netmd/main.cpp` + `carta-netmd/NetMdHostBridge.cpp`), never by `Carta Studio`. `NetMdBridge.cpp` invokes `carta-netmd` over standard POSIX pipes with temporary binary files for ATRAC wire/UTOC payloads and streams NDJSON `progress`/`result` events from `stdout`.
- **Host primitives (`carta-netmd/NetMdHostBridge.cpp`)**: Implements CommonCrypto DES/3DES & CSPRNG, `iconv` UTF-8 <-> CP932/Shift-JIS, `libusb-1.0` control/bulk transfers, NDJSON progress callbacks, and an in-process mock `Sony MZ-N920 (Mock Type-S)` (`S1.600`) device (exposed via `carta-netmd session --mock`) for deterministic testing.

## MiniDisc Workspace and Hardware Safety

The docked `MdWorkspace` exposes two tabs sharing one undoable `MdDisc` state
and synchronized with the pinned **MD Tracks** timeline lane:

1. **Tracks & UTOC**: Author tracks, parts, groups, half-width (JIS X 0201) & full-width (Shift-JIS) titles, write-protect/SCMS/emphasis flags, BCD timestamps, per-track codec overrides, and clip/marker sync; audition tracks or export a 32-bit float review WAV with cues.
2. **Device & Record**: Inspect UTOC sector budgets (`2352`-byte Sectors 0/1/2/4), import/export `.utoc` bundles, probe USB NetMD recorders, inspect on-disc tracks, review the coalesced physical recording plan (highlighting aliased spans saved by DAG sharing), read disc UTOC, erase discs, write UTOC (`ForceTOCEdit`), and record coalesced ATRAC spans (`KillEepromWrite` + `SPUpload` + `ForceTOCEdit`).

**Hardware safety guard**: Any physical hardware write (`eraseNetMdDisc`,
`writeNetMdUtoc`, `recordNetMdDisc`, and IPC `netmd erase|write-utoc|record`)
requires explicit caller confirmation (`confirmHardware == true` /
`confirm-hardware` token or interactive UI modal confirmation) unless targeting
an unguarded mock device.
