# core/ — everything musical, no JUCE

Builds and tests in seconds: `cmake --build build && ./build/bambi-tests` (one test: `-tc="*name*"`).

## Working here

- **No JUCE, no platform call, no filesystem path.** A platform API enters only behind an existing seam (the link
  bus's is `src/link/os.hpp`).
- **Layers**, lowest first: `math io < dsp path region < patch < mod < link scene encode echo reverb decode < live`.
  Nothing includes upward — `tools/check-layers.sh` fails on it; move the type instead. A new engine is its own folder
  and library in the engines' layer, added to the table in that script.
- **An engine's parameter list is the one place its keys, ranges and defaults live**
  (`include/bambi/<engine>/params.hpp`); the engine reads them from its manifest, never from a second copy. Changing
  the list: the `add-parameter` skill.
- **Control runs on a fixed 256-sample grid**, so nothing depends on the host's block size. Work that changes per
  block (filters, matrices, gains) is computed at control rate, not per sample.
- **Real-time code allocates, frees and locks nothing** once running; buffers are sized when the engine is prepared.
- **Denormals** are flushed by a `ScopedFlushDenormals` around every rendered block (`include/bambi/math/denormals.hpp`);
  no result depends on flush-to-zero.
- **Unused API is removed**, not kept for later; a helper used by one file is private to it.
- Hand-aligned tables (parameter lists and the like) sit inside `// clang-format off` / `on`.

## Tests

One `tests/<layer>/test_*.cpp` per module, doctest. Every test carries a `Catches: …` comment naming the mistake it
catches, and fails when the code is broken that way. Prefer round trips, invariants under transformation, agreement
with a closed form, and bit-identical output at block sizes 1…1024.

- **For a value below 1, assert a ratio or explicit bounds, never `doctest::Approx(...).epsilon()`**: its tolerance is
  `epsilon * (1 + max(|a|,|b|))`, so on radians or normalised values it is absolute and far too loose.
- **Concurrency tests stop on a sample count, not a clock**, with a deadline only against a hang.
- **Two lists that must agree are held by a test**, not by extra machinery.
- Test names say what they check.
