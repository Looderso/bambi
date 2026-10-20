# Portability — macOS, Windows, Linux

`core/`, `tools/` and the plugins build and pass their suites on **macOS** (arm64; the plugins as a universal
arm64 + x86-64 binary), **Windows** (x64, MSVC) and **Linux**, on every push and pull request in CI
(`.github/workflows/build.yml`). Formats are VST3 and CLAP.

## The position

Portability of `core/` is held by convention: no OS headers in the shared code, no hand SIMD, no assumption about
struct layout, no filesystem location written inline. A convention is only real if something checks it, so the
build runs on all three systems and `tools/check-portable.sh` holds the rest locally (below).

| what CI runs | where |
|---|---|
| style, layers, format and notices checks | one Linux job |
| `core/` and `tools/` built, `bambi-tests` run | macOS, Windows (x64, MSVC, Ninja, Release), Linux |
| `tools/golden.sh` (exact hashes) | macOS only: the manifest is one toolchain's |
| the plugins built, every check suite run (xvfb on Linux) | all three |
| release zips (VST3 and CLAP per system) | all three, published on a `v*` tag |

## The seams

| what | where it lives |
|---|---|
| OS calls in `core/` | one file per OS behind `core/src/link/os.hpp`: `os_posix.cpp` (`shm_open`, `CLOCK_MONOTONIC`) and `os_windows.cpp`; nothing else in `core/` includes an OS header |
| public headers | no POSIX in any; every one compiles standalone (`--headers`), the class of mistake libc++ hides and MSVC's STL reports |
| liveness on the link bus | a heartbeat, never a pid, which is what usually pins a shared-memory bus to one OS |
| scope of the bus | machine-local; builds from different systems meet only through a project file, and state is JSON keyed by name |
| the segment's layout | `sizeof`/`alignof` of `Slot`, `Header` and `Segment` are `static_assert`ed in `core/src/link/link.cpp`, so an accidental gap fails the build instead of making a bus silently never join |
| SIMD | none by hand: the encode is L1-store-bandwidth bound and the default build already vectorises it (measured: turning vectorisation off costs 0.4 %), so there is no intrinsics layer to port |
| paths | a temp location comes from the platform (`temp_directory_path()`, JUCE's `tempDirectory`), never an inline `/tmp`; tests that write files use a per-process name |

## The guard: `tools/check-portable.sh`

Apple clang and Apple libm throughout. It is not a Windows build and says nothing about MSVC, JUCE or speed.

| guard | catches |
|---|---|
| 1 · every public header compiled standalone (`--headers`, under a second) | a missing `<algorithm>` beside a `std::max`: invisible under libc++, an error under MSVC |
| 2 · the x86-64 build plus the suite under Rosetta | ISA and ABI mistakes: an unaligned SSE load, `long double`, a struct laid out differently, an `#ifdef` with only an Apple-arm branch |
| 3 · every golden scenario rendered by both builds and nulled at −120 dBFS, control traces compared | a divergence that is not rounding: a thresholded decision flipped by one ULP, a different code path per ISA |

Both scripts read the scenario list from `tests/golden/scenarios.txt`. `tools/golden.sh` keeps exact hashes as the
within-toolchain check; the cross-architecture check is a null, because an exact hash cannot be made
architecture-independent at any word length (a quantised hash was measured: a one-ULP difference near 0.1 full
scale is ~7.5e-9 against a 24-bit LSB of 1.2e-7, so about 6 % of samples sit close enough to a rounding boundary to
cross it). Rosetta is a correctness harness only; no cost figure comes from it, and benchmarks stay native and
Release.

Measured by guard 3: every scenario nulls across the two architectures, worst −144.5 dBFS against the −120 gate,
every control trace identical. A control trace is printed at `%.6f` / `%.4f`, so it is already a tolerance golden.
The x86-64 build runs the whole suite to the same case and assertion count as native.

## The promise

**A bounce matches what was heard within one build:** offline equals realtime in every sample, and a render is
bit-identical at block sizes 1 to 1024. Both are asserted. A project rendered by a different system or
architecture is not required to null against the first render.

The cross-architecture divergence is about −150 dBFS, one float32 ULP, and is rounding, not a different algorithm.
Three causes, isolated by experiment:

| cause | what it does |
|---|---|
| FMA contraction | arm64 clang fuses by default; the x86-64 SSE2 baseline cannot. Pinning contraction alone does not close the gap |
| libm | `cos(0.1)` differs by one ULP between the two slices |
| pffft | the NEON and SSE paths differ in one bin of 1025, relative 8.6e-8 |

Closing it would mean owning pffft's twiddle tables, our own `sin`, `cos`, `acos`, `exp` and `log`, and pinned
contraction, permanently. That is not done. What the promise obliges:

- `-ffast-math`, `/fp:fast` and `/Ofast` stay refused.
- The render tools share the plugin's denormal environment, `bambi::ScopedFlushDenormals`.
- No thresholded decision in the audio path may turn one ULP into a block-sized difference; a gate opens by a
  continuous weight, not a step (a hysteresis produces the identical step).

## Windows specifics

| what | how it is handled |
|---|---|
| the shared segment and the clock | `os_windows.cpp`: a named file mapping in the `Local\` namespace, sized when created, its size checked against what this build expects; the performance counter as the machine-wide clock |
| a segment's lifetime | a segment exists while something holds it open, and `unlinkShared` does nothing; the directory and the bus list only what is live, so a user sees no difference |
| includes libc++ supplies indirectly | listed explicitly (`<algorithm>`, `<initializer_list>`); doctest's printing of a `string_view` needs `<ostream>`, force-included into the test binary under MSVC |
| sleeping | a Windows `sleep_for(2ms)` is a whole 15.6 ms timer tick; a test that needs a short wait waits on the clock |
| CMake | Release set before `project()` (MSVC picks Debug); `-Wall -Wextra` only for Clang and GCC, `/W3` for MSVC; `/utf-8 /Zc:__cplusplus /permissive-`; the C runtime's `fopen` notices off |
| printing a path | a `std::filesystem::path` through `%s` is a wide string on Windows; convert it first |

The bash tools (`verify.sh`, `golden.sh`, the checks) are not run on Windows.

Background: decisions 0007, 0010, 0038, 0043, 0044, 0129.
