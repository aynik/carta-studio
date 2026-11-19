# Carta1 frame linking experiment

This experiment compiles Carta1's actual frame encoder, serializer, deserializer and decoder to C using the pinned Porffor alpha-19 compiler and Carta1's existing native-build transformations. It links the generated code as a static archive into a C++ probe. The [C interface](frame.h) processes 512 mono float samples or a 212-byte sound unit per call. It has two separately allocated encoder/decoder pairs and interleaves their frames.

## Run

Initialize the submodules and install Python 3, Node.js, npm, Clang/Clang++, `ar`, and Porffor alpha-19 (`0c135ce`). For the same compiler archives and SHA-256 hashes used by Carta1, see `third_party/carta1/.github/workflows/native.yml`.

```sh
PORFFOR=/path/to/porf python3 experiments/carta1-frame/build.py
# 25,840 frames per stream: approximately five minutes at 44.1 kHz
PORFFOR=/path/to/porf python3 experiments/carta1-frame/build.py 25840
# 413,438 frames per stream: slightly more than 80 minutes at 44.1 kHz
PORFFOR=/path/to/porf python3 experiments/carta1-frame/build.py 413438
# Independently exercise mismatch, truncation and process-failure handling.
python3 experiments/carta1-frame/test_compare.py
```

The first run installs Carta1's locked build dependencies in the experiment's ignored `build/tools` directory, without changing the codec checkout. `CARTA1_NODE_MODULES=/path/to/node_modules` can instead supply an already installed, compatible copy for local development. The test compares every encoded byte and decoded float bit against the JavaScript implementation for silence, impulses and deterministic varying samples. It reads one record at a time, avoiding an output buffer proportional to duration, and rejects incomplete output and native process failures. A mismatch reports the stream, frame and byte. It checks each decoder against its corresponding JavaScript encoder's output; because encoded bytes are checked too, a passing run uses identical inputs to both decoders. Tests have run on macOS arm64; Linux is unverified.

## Scope and limitations

This is not a production library. It patches Porffor-generated C and depends on internal symbol names, value layout, memory representation, GC stack handling and runtime initialization. It supports exactly two permanent streams within a single runtime, has no creation, reset or shutdown interface, serializes all calls on one thread, and may terminate the process on an unhandled JavaScript error. It allocates and must not run on an audio callback. The test accepts valid inputs only; buffer length and pointers are the caller's responsibility. Passing tests do not prove GC safety for arbitrary usage or full audio, container, channel, platform, and error coverage. The bridge passes native addresses through floating-point values and assumes they are exactly representable. It has not been linked alongside Carta3 or Carta5; generated symbols collide if multiple static runtimes are linked without isolation.

On 9 November 2026, the pinned Carta1 submodule `eb5e646ce033629e551296824bbe277e75fddfa5` passed 413,438 frames per stream (slightly over 80 minutes each) on macOS arm64, with Porffor alpha-19, Apple Clang 16 and Node 24.11.1. All 826,876 interleaved encoded frames and decoded float bit patterns matched JavaScript. During the run, 83 samples taken at approximately two-second intervals after startup observed maximum RSS of 38,624 KiB for the native process and 118,880 KiB for the JavaScript comparator; these are sampled values, not guaranteed process peaks. The inputs are synthetic and the bridge supports only the two fixed streams described above.

Separately, the public JavaScript and native CLI pipelines produced identical AEA and decoded WAV files for a three-second stereo sine fixture on this host. `python3 experiments/carta1-frame/compare_cli.py /absolute/path/to/native-carta1` reproduces that comparison with a native CLI built from the same codec revision and pipeline. The output basenames must match because the encoder stores the basename in the AEA header. This is a short file-level check, not a full-duration CLI or device qualification.

A production interface needs explicit stream lifecycle and bounded error reporting, robust runtime ownership and isolation, compatible integration for all codecs, and more independent known-vector, long-duration, GC, malformed-input and cross-platform tests. File-level priming, padding, container handling, and stereo coordination also remain outside this frame probe.
