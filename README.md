# Carta Studio

Carta Studio is a native C++17/JUCE multitrack audio editor and MiniDisc
mastering studio. It combines a 44.1 kHz non-destructive timeline with a
UTOC-native MiniDisc authoring workspace, natively compiled Porffor ATRAC
codecs (`carta1`, `carta3`, `carta5`), and USB NetMD hardware transfer.

## Supported MiniDisc & Codec Modes

| Mode | Codec | Channels | Frame Samples | Sound Unit | Nominal Bitrate | Container |
| --- | --- | --- | --- | --- | --- | --- |
| `sp-stereo` | `carta1` (ATRAC1) | 2 | 512 | 212 B (424 B/frame) | ~292 kbps | `.aea` |
| `sp-mono` | `carta1` (ATRAC1) | 1 | 512 | 212 B | ~146 kbps | `.aea` |
| `lp2` | `carta3` (ATRAC3) | 2 | 1024 | 384 B | 132 kbps | `.wav` |
| `lp4` | `carta3` (ATRAC3) | 2 | 1024 | 192 B | 66 kbps | `.wav` |
| `hi-sp` | `carta5` (ATRAC3plus) | 2 | 2048 | 1488 B | 256 kbps | `.wav` |
| `hi-lp` | `carta5` (ATRAC3plus) | 2 | 2048 | 376 B | 64 kbps | `.wav` |

## Features

### Multitrack Editing & Mastering

- **44.1 kHz half-open timeline**: Sample-accurate clip positioning, trimming,
  slipping, splitting, joining, persistent clip groups, and multi-layer mixing
  with linear track gain (up to 16x), stereo balance, and per-channel routing
  matrices.
- **Envelopes & fades**: Four fade curves (`linear`, `equal-power`, `s-curve`,
  `exponential`) anchored to source time, plus same-track and cross-track
  crossfades.
- **Visualizations & analysis**: Background peak/RMS waveform overlays (linear
  or dB scale, full or half-wave), configurable FFT spectrograms, quiet-gap and
  source-silence detection, zero-crossing snapping, and 44.1 kHz mix-level
  scanning.
- **Portable projects & WAV export**: Self-contained `.carta` (`CARTAP01`)
  project packages with SHA-256 verified media, automatic recovery snapshots,
  and 16-bit PCM, 24-bit PCM, or 32-bit float WAV export (with optional
  deterministic TPDF dither, clipping preflight, and embedded RIFF cue labels).

### MiniDisc Authoring & NetMD Transfer

- **Two-tab docked workspace** (**View > Show MiniDisc workspace**):
  - **Tracks & UTOC**: Edit disc/track half-width (JIS X 0201) and full-width
    (Shift-JIS) titles, BCD timestamps, write-protect/SCMS/emphasis flags,
    per-track codec overrides, track groups, multi-part references, and
    track/disc audition or cue-embedded review WAV export.
  - **Device & Record**: View UTOC slot budgets (255 tracks, 255 fragments, 255
    title cells, MD60/MD74/MD80 cluster capacity), inspect/import/export
    2352-byte UTOC sectors, scan USB NetMD recorders, read physical disc TOCs,
    write UTOC updates (`ForceTOCEdit`), erase discs, and record coalesced
    SP/LP2/LP4 spans (`KillEepromWrite` + `SPUpload` + `ForceTOCEdit`).
- **Pinned MD Tracks lane**: Interactive timeline ribbon synchronized with
  `Tracks & UTOC` for magnetic rolling splits, Alt-trimming, and Alt-duplication
  (sharing physical audio clusters via UTOC DAG suffix sharing).
- **Hardware safety**: Destructive NetMD operations (`erase`, `write-utoc`,
  `record`) require explicit UI confirmation or the `confirm-hardware` IPC
  argument.

## Build

Prerequisites:

- CMake 3.22+ and a C++17 toolchain
- Node.js (with pinned Porffor alpha-19 toolchain under `build/porffor/toolchain/porf`
  and staged packages under `build/porffor/tools/` and `build/porffor/netmd-deps/`)
- `libusb-1.0` headers and library
- Platform dependencies: AppKit + `iconv` on macOS; ALSA, X11, GTK3, `xvfb`,
  `openbox`, `xdotool`, and `ffmpeg` on Linux

Configure, build, and run tests:

```sh
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCARTA_STUDIO_COMPONENT_TESTS=ON
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

Rebuild isolated Porffor dylibs manually when modifying codec or NetMD bridges:

```sh
node Tools/build_porffor_codecs.mjs --force
node Tools/build_porffor_netmd.mjs --force
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the `Source/` domain map,
locking/threading invariants, and Porffor C ABI details.

## Testing

| Target / Script | Coverage |
| --- | --- |
| `CartaNativeAlertPolicyTests` (`carta-native-alert-policy`) | Native alert Escape-key and button-index policy |
| `CartaMdAuthoringTests` (`carta-md-authoring`) | `MdDisc` auto-tracking, Layer 1/marker sync, group shifts, physical span coalescing |
| `CartaUtocTests` (`carta-utoc`) | UTOC 2352-byte sector packing, Shift-JIS/half-width titles, DAG fragment sharing, overflow checks |
| `CartaPorfforCodecTests` (`carta-porffor-codecs`) | Bit-exact parity vs Node.js `third_party/carta{1,3,5}`, handle isolation, 16s stream stability |
| `CartaNetMdTests` (`carta-netmd`) | Mock `MZ-N920` (`S1.600`) NetMD safety guards, coalesced SP/LP2 upload, UTOC roundtrip, 544 KB stack test |
| `CartaSessionTests` (`carta-session` + Unix crash/limit tests) | Session rendering, sinc resampling, MP3 gapless/CRC inspection, `EditHistory`, `SessionIO` packages & WAV export |
| `CartaComponentTests` (`carta-component` + workflow flags) | Windowless `Studio` + `MdWorkspace` UI, drag/snap/undo, guarded alerts, IPC authoring |
| `CartaGtkPromptTests` / `Tests/run_linux_ui_tests.py` | Linux Xvfb/GTK3 native prompt and UI integration runner |
| `Tests/ipc_*.py`, `Tests/ffmpeg_*.py`, `Tests/mp3_*.py` | Real-process CLI IPC automation, relinking, MD authoring, and FFmpeg/LAME interoperability |

## Headless Daemon & Local IPC Automation

The application binary doubles as a local Unix-socket JSON-line IPC client and
headless daemon (`Release/` subfolder on Xcode/multi-config generators, or
directly under `build/CartaStudio_artefacts/` on single-config Ninja/Make):

```sh
"build/CartaStudio_artefacts/Release/Carta Studio.app/Contents/MacOS/Carta Studio" --help
"build/CartaStudio_artefacts/Release/Carta Studio.app/Contents/MacOS/Carta Studio" daemon start
"build/CartaStudio_artefacts/Release/Carta Studio.app/Contents/MacOS/Carta Studio" ipc status
"build/CartaStudio_artefacts/Release/Carta Studio.app/Contents/MacOS/Carta Studio" ipc schema
```

Set `CARTA_STUDIO_IPC_DIR` to a private user-owned directory to isolate test or
automation sessions from the default per-user socket (`/tmp/carta-studio-<uid>/control.sock`).

| Domain | Scope |
| --- | --- |
| `status` / `app` / `formats` / `schema` / `meter` | Session, transport, audition, peak meter hold, headless daemon lifecycle, registered readers, IPC schema |
| `layers` / `tracks` / `layer` / `track` | Layer creation, mute/solo, gain/balance, stereo matrices, channel split/combine, display settings |
| `import` / `imports` / `project` / `export` / `render` | Audio import queues, `.carta` package open/save, WAV export (`pcm16`/`pcm24`/`float32`), track batch/consolidation |
| `select` / `edit` / `clip` / `snap` / `history` | Time/track/clip selection, non-destructive edits, fades/amplification/normalization, snapping, undo/redo history |
| `markers` / `marker` / `master` / `master-tracks` / `master-track` | Timeline track markers, Audacity/JSON marker exchange, master channel mode & duration |
| `transport` / `loop` / `seek` / `view` | Play/pause/stop, seek cursor, boundary/crossfade/gap auditions, saved loop, viewport, PNG UI capture |
| `analysis` / `sources` | Quiet-gap, source-silence, zero-crossing, peak/RMS analysis, SHA-256 verification & relink |
| `disc` / `utoc` / `encode` / `netmd` | `MdDisc` tracks/parts/groups/preview, UTOC inspect/import/export, Porffor ATRAC encode, USB NetMD |

## Acknowledgements

- [JUCE](https://juce.com/) for cross-platform GUI, audio device I/O, and formatting primitives.
- `carta1`, `carta3`, and `carta5` (with gratitude to [AtracDEnc](https://github.com/dcherednik/atracdenc)) for ATRAC1, ATRAC3, and ATRAC3plus codecs.
- [netmd-js](https://github.com/cybercase/netmd-js) and [netmd-exploits](https://github.com/asivery/netmd-exploits) for NetMD protocol and Sony Type-S/R firmware support.
- [Porffor](https://porffor.dev/) for ahead-of-time JavaScript-to-C compilation.
- [minimp3](https://github.com/lieff/minimp3) for optional streaming MPEG Layer III decoding on Linux.
