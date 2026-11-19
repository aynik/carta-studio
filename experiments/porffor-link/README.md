# Porffor linking experiment

This proof of concept calls the unmodified Carta1 `quantize` implementation from C++. It wraps one sample at a time in a JavaScript `Float32Array`, emits C with Porffor alpha-19, patches the generated program's entry point into a small C bridge, and links the result into a C++ executable. It compares 6,321 results with Node.js. It also builds two copies as shared libraries with hidden internal symbols and compares both with Node.js in the same process. Directly linking two unmodified generated objects on macOS produced duplicate Porffor runtime symbols; hiding the internals in separate shared libraries avoided those linker errors in this test.

Requirements: Node.js, Python 3, Clang/Clang++, `ar`, and the pinned Porffor alpha-19 binary. Initialize the Carta submodules, then run:

```sh
PORFFOR=/path/to/porf python3 experiments/porffor-link/build.py
```

The experiment has been run on macOS arm64. The shared-library script also contains Linux flags, but Linux has not been tested. Generated files live in the ignored `build/` directory here.

This is an investigation, not a supported codec API. The generated symbol names, value representation, stack handling, and runtime entry point are compiler internals. The script checks the compiler version and expected generated patterns, but those checks cannot guarantee runtime correctness. Each library has a singleton runtime, and the bridge is single-threaded, allocates, has no error containment or shutdown, and must never be used on a realtime audio callback. Results do not establish thread safety, independent encoder state, GC safety over long sessions, full codec equivalence, or safe behavior for invalid inputs. The two libraries in the isolation test are copies of the same quantizer, not the three full Carta codecs.

Next, adapt the existing codec native build transformations for a frame-level encoder and decoder boundary, preserving one codec state per stream. Test bytes and decoded floating-point bit patterns against JavaScript across multiple interleaved streams and realistic durations. Investigate error handling, runtime lifetime and thread confinement before connecting this to the application.
