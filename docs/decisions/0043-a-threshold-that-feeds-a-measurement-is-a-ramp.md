# 0043 · A threshold that feeds a measurement is a ramp; a latch belongs where the output is an event

**Status:** accepted

**Date:** 2026-09-18  **Status when written:** decided, built in `core/`  **Scope:** suite

**Revises** [0038](0038-a-bounce-matches-what-was-heard-on-the-machine.md)'s *"hysteresis there is
the fix"*. The mechanism 0038 named as the one thing that could overturn it — a thresholded decision in the
audio path turning one ULP into a block-sized difference — was the Attack feature's `slow_ > attackGateFloor`.
It is fixed, and **the fix is not the one 0038 proposed**.

**Attack's opening out of silence is a weight, not a decision.** `normRange(slow_, attackGateFloor, openHi)`
multiplies the Attack value, so the feature is continuous in the envelope and one ULP in gives one ULP out
(`core/src/dsp/features.cpp`).

### The rule this generalises to

> **Hysteresis belongs where the output is an event; a ramp belongs where the output is a measurement.**

`EnvTrigger` is an event and is right to latch — it already does, and it saves its latch in state. The six
features are measurements, and a measurement that latches is a measurement that depends on history.

### Why hysteresis was specified, and then rejected

| | |
|---|---|
| **a latch is still a step** | it stops chatter but does not remove the discontinuity: a one-ULP difference still changes *which* hop opens, and the value either side differs by the full swing. Verified by mutation — a latch with 6 dB of separation produces **the identical 0.556 worst step** the bare branch does |
| **a latch makes Attack history-dependent** | a bounce from bar 5 would read differently from realtime through bar 5 — [0026](0026-the-link-across-plugins-definitions-travel.md)'s spirit broken in a way a ramp cannot break it |
| **a latch adds state** | so `reset()` would have to change. A missing reset is exactly the omission that makes a bounce diverge from realtime on the **first block** and stays invisible until someone renders twice. The ramp adds no state, so there is nothing to reset |

### The ramp introduces no new number

The band is `attackHi / attackLo` wide — the same relative interval the ratio itself spans, so the open weight
is never the steepest term in the product. No new `FeatureConfig` field and no new constant.
`FeatureConfig` is populated from nothing anywhere in the tree — no `ParamId`, not in the state JSON — so its
"deliberately NOT a user control" comment still holds.

### Measured, 2026-09-18, Release, this machine

| | |
|---|---|
| worst adjacent step, 0.1 dB amplitude sweep 1e-3…1e-1, read at the **first** analysed frame | branch **0.556** → ramp **< 0.05** (asserted) |
| mutations caught by the new case | **4 of 4**, each built broken and watched to fail: the bare branch, a hysteresis latch at any separation, a zero-width ramp, the open weight deleted |
| the 11 golden scenarios | **unchanged — 11 of 11 audio and trace hashes, 0 of 564 trace rows differ** |
| `features block` | 1.964 µs spectral on / 0.788 µs off — unchanged within run-to-run spread |

**The goldens did not move because of what they contain, not by luck.** Every scenario starts at full level, so
`slow_` clears the whole ramp band inside its first hop; the ramp only changes an envelope *rising through*
−70 to −64 dBFS. This is a pure robustness change with no behavioural diff on anything currently rendered.

Why it was worth doing at all: the crossing falls **on** the transient with `gate_` already ~0.58, at a frame
RMS of −41.3 dBFS — ordinary material — and `modulation.hpp` does not smooth a rate destination, because the
motion clock integrates it. So one hop of 0.58 became a **permanent position offset**, not a transient.

### What this does not decide

- **The gate's own threshold** — `db > cfg_.gateDb` — has the same shape and is not fixed. Its width cannot be
  derived the way Attack's was, and it will move the gate column of every control trace and therefore the
  goldens ([STATUS, *Found, not fixed*](../STATUS.md)).
- **`features.cpp`'s `flatDb = −60.0` fallback**, which through an inverted `normRange` reads as *fully tonal*.
  Unreachable today; dead code with a wrong dead branch. **Corrected on 2026-09-18** to `cfg_.tonalLoDb`, the
  not-tonal end of the range — a fix, not a decision: the branch never ran, and the goldens did not move
  ([STATUS, *Found, not fixed*](../STATUS.md)).

**Contingent on** Attack staying a measurement rather than becoming a trigger; on `attackHi / attackLo`
remaining the ratio's own span, so the ramp keeps costing no new number; and on rate destinations staying
unsmoothed, which is what turns a step
into a permanent offset.

**Would overturn:** a measurement whose ramp band cannot be derived from a range it already has **and** whose
chosen width is audible — there the honest answer is a latch plus a stated history dependence, not an invented
constant; or a destination that smooths its input, which would make the step transient and the whole argument
smaller.
