---
name: benchmark
description: Measure what bambi's code costs — an engine in core, a whole plugin block, a before/after of an optimisation — and report it the project's way. Use before claiming anything is fast, slow or cheaper, and when updating docs/status/MEASURED.md.
---

# Benchmarking

A Debug build once made this project believe pffft was 3× slower than numpy; in Release it was 4× faster. Every cost
claim comes from a Release run, here.

## Which tool

| what | tool |
|---|---|
| an engine in `core/` (encode, features, modulation, regions, delay, reverb) | `./build/bambi-bench` (`tools/bench/main.cpp`) |
| a whole `processBlock` as a host drives it, no window | `bambi-plugin-bench` (encoder) and `host/BlockBench.h`, which the effects' check suites use |
| the plugin in a host | not REAPER's CPU meter — it cannot separate the audio thread from a window redrawing |

## How

1. **Release** — the default build type; confirm with `grep CMAKE_BUILD_TYPE build/CMakeCache.txt`.
2. A quiet machine: nothing else compiling, no load generators left running (`pgrep yes`).
3. Run each measurement more than once; report the median, and the spread when it is large.
4. A before/after is two runs of the same harness on the same build type, one change apart.
5. New engines join `bambi-bench`, so the number stays reproducible rather than remembered.

## How to report

- **A share of one core at 128 samples / 48 kHz** (one block = 2.667 ms), beside the absolute time per block.
- Never a speedup without the absolute cost beside it.
- Say what, on what machine, which build, which order and settings.
- An estimate says "estimated" and from what (multiplies per sample, a scaling law).
- To check vectorisation, read the asm or compare against `-fno-vectorize` — that is the lower bound a weaker ISA
  cannot fall below. Rosetta or a cross build proves correctness only, never speed.

The standing numbers live in `docs/status/MEASURED.md`; update the line when a measured number changes.
