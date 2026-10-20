# 0038 · A bounce matches what was heard on the machine that heard it

**Status:** accepted, revised in part by [0043](0043-a-threshold-that-feeds-a-measurement-is-a-ramp.md)

**Date:** 2026-09-17  **Status when written:** decided  **Scope:** suite
**Revised by [0043](0043-a-threshold-that-feeds-a-measurement-is-a-ramp.md)**
— the thresholded decision named in *would overturn* below was found and fixed, and hysteresis is not the fix.
**Answered by [0044](0044-cross-architecture-agreement-is-checked-by.md)**
— both items under *what this does not decide* are decided. The reasoning below stands as written.

**Bit-identity is a promise within one build, not across platforms or architectures.** Two things it
obliges are already asserted and stay asserted: **offline equals realtime in every sample**, and **a
render is bit-identical at block sizes 1…1024** — `bambi-render` and the 11 golden scenarios
([STATUS](../STATUS.md)). What it does **not** oblige: a project authored on macOS / arm64 and rendered
later by a Windows / x86-64 build is not required to null against the macOS render. That difference is
an **accepted risk, recorded here**, not a debt with work behind it.

### The measurement that bought it

One x86-64 cross-build of this source, 2026-09-17, Apple clang, Release, suite run under Rosetta —
the full run, its numbers and its method are in [PORTABILITY §3](../suite/PORTABILITY.md) and are not
restated here:

| what it found | |
|---|---|
| audio goldens | **9 of 11 hashes differ**; worst **−150.5 dBFS** on `orbit-const`, one float32 ULP |
| control traces | **every hash matches** — they are printed at `%.6f` / `%.4f` and are already tolerance goldens |
| causes, isolated by experiment | FMA contraction, Apple libm, pffft's NEON-vs-SSE path — all three rounding |
| the obvious cheap fix | **pinning contraction alone does not close it**: arm64 with contraction off still differs from x86-64 with contraction off |

So the divergence is rounding, not a different algorithm, and it is not one knob away from gone.

### Why the risk is accepted rather than paid down

Cross-machine bit-exactness has a known price, and all of it is permanent:

| what would have to be owned | why |
|---|---|
| pffft's twiddle tables | it builds them with libm `cosf` / `sinf`, so no compiler flag or convention of ours reaches them |
| our own `sin`, `cos`, `acos`, `exp`, `log` | every libm call is a per-platform result — `atan2` / `cos` in `sh.cpp`, `exp` in `features.cpp` and `modulation.cpp` |
| pinned `-ffp-contract` on every target | necessary, and shown above to be insufficient alone |

That is a vendored maths kernel maintained forever, in exchange for a −150 dBFS difference, and it is
a guarantee no plugin in this category makes. The promise is therefore scoped to one build rather
than the cost being paid.

### What the promise still obliges, so this is not read as licence

- **`-ffast-math`, `/fp:fast` and `/Ofast` stay refused regardless.** The promise is *within* a build,
  and those break it there too — reassociation makes one build's own output depend on optimisation
  level and inlining.
- **`tests/golden/manifest.txt` is a per-toolchain manifest, and is named as one.** `tools/golden.sh`'s
  header says so as of 2026-09-17; a new golden is a tolerance or quantised golden, or it is labelled.
- **`bambi-render` and the plugin must share one denormal environment.** The tool sets none while the
  plugin has `ScopedNoDenormals`. That is invisible while the encoder is feed-forward and stops being
  invisible with Reverb's tail and Echo's loops — offline would not equal realtime, which this entry
  promises. It is therefore **work this decision requires**, carried as debt payable before either
  engine is written ([STATUS, *Debt*](../STATUS.md)), not a loose defect.

### The residual, stated plainly

Inaudible for one unmodulated instance. It **compounds** through features → matrix → trajectory, so a
modulated render can drift audibly over minutes, and **stems bounced on a different machine from the
mix do not sum bit-exactly to it**. Anyone who needs that nulls their stems on one machine.

### What this does not decide

- **Whether the goldens themselves become architecture-independent** — 24-bit hashes, or an
  `bambi-nulltest` gate at −120 dBFS. That is test hygiene, not a promise to users, and it stays open
  ([STATUS](../STATUS.md)): its value is that `tools/check-portable.sh` could then run the goldens instead
  of deliberately skipping them ([PORTABILITY §5](../suite/PORTABILITY.md)).
- **The `static_assert` on `sizeof(Slot)` and `sizeof(Segment)`.** The link records were measured
  identical on both architectures, which is currently a runtime comparison rather than a declaration;
  making it a compile error is proposed and unanswered.

**Contingent on** the causes staying rounding — three isolated, all three rounding; on the link bus
being machine-local, so a macOS build and a later Windows build meet only through a project file, which
is JSON keyed by name end to end ([0010](0010-the-scene-observes-paths-nothing-real-time.md),
[0015](0015-state-reaches-the-audio-thread-by-snapshot.md)); and on no delivery
obligation requiring a cross-machine null.

**Would overturn:** a thresholded decision in the audio path turning one ULP into a block-sized
difference — the feature gate's bare comparison is exactly that mechanism ([STATUS, *Found, not
fixed*](../STATUS.md)), and hysteresis there is the fix, not bit-exactness; a portable libm and a portable
FFT arriving at no maintenance cost; or a workflow where stems must be bounced on several machines and
summed.
