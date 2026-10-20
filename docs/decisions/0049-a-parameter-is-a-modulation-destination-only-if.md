# 0049 · A parameter is a modulation destination only if its effect recomputes on the audio thread, in bounded work, from what `prepare` sized

**Status:** accepted

**Date:** 2026-09-18  **Status when written:** derived from two findings of 2026-09-18 — `dsp-performance` measuring in
Release on arm64 against the built `bambi-core`, `realtime-safety` reading the mod path. It is the rule Echo's
and Reverb's classifications were made with ([ECHO §8](../echo/ECHO-CONTROLS.md), [REVERB §3](../reverb/REVERB.md)); **it was
not put to the user as a question of its own**  **Scope:** suite
**Revised by **, 2026-09-18: *"0015 stays exactly as it is for everything a host parameter
changes"* does not hold for Echo's operators — an automated axis or skew in a bounce is the same hand-over as a
modulated one. The rule below stands as written.

> **A parameter can be a modulation destination if its effect recomputes synchronously, on the audio thread, in
> bounded work, with everything sized at `prepare` — and is then interpolated per sample across the hop if it is
> a gain.**

### Why this is not [0015](0015-state-reaches-the-audio-thread-by-snapshot.md)

**A modulated value only exists on the audio thread.** It is a function of features, LFOs and transport at that
control step, so nothing the matrix drives can be built by 0015's message-thread-compiles / audio-thread-adopts
pattern. If a modulated parameter posts a rebuild and adopts the result when it arrives, **when it arrives
depends on message-thread scheduling** — so the offline pass adopts at a different sample from the realtime
pass, and bounce identity is gone silently. That is a
[0026](0026-the-link-across-plugins-definitions-travel.md) /
[0038](0038-a-bounce-matches-what-was-heard-on-the-machine.md) argument, not a 0015 one: 0015 is
about *allocation and locks*, and a rebuild that satisfies 0015 can still break the bounce.

0015 stays exactly as it is for everything a *host parameter* changes. The rule above is the narrower one that
applies once the matrix drives the same parameter.

### The pattern that already does it, and had no name

`Encoder::setTarget` (`core/src/encode/encoder.cpp:10-36`). Every control step it recomputes `capWeights` and
the whole SH coefficient vector on the audio thread, allocation-free, because `prepare(order)` sized every
vector to the worst case — then `process` ramps from the previous vector to the new one, per sample, indexed
from an absolute position so the move takes the same wall-clock time however the host chops the buffer. Measured:
`capWeights` is **7 ns at order 3, 15 ns at order 7**.

### For a delay time or a room size, all of it

A modulated length is the hard case, and it bounces identically only if **all** of these hold:

- the length in samples is a pure function of (the control-step value, the sample rate), with a fixed rounding
  rule — never of the block size or of how the host partitioned the buffer;
- the buffer is sized at `prepare`, for the worst case;
- the crossfade or repitch runs on the control grid with a fixed length in **samples, not blocks**;
- transport-start reset clears the same state in both passes.

's scope
still applies on top: a reverb tail agrees **from the same start point**, never from anywhere on the timeline.

### What this does not decide

- **Which parameters pass it.** Echo's and Reverb's classifications are [ECHO §8](../echo/ECHO-CONTROLS.md) and
  [REVERB §3](../reverb/REVERB.md); nothing in either has been heard, and no prototype has modulated anything.
- **What happens to a parameter that is refused a row.** Today `isModulationTarget` is literally
  `destinationKind(id) != NotModulatable` (`core/src/mod/matrix.cpp:25-27`), so one list answers two questions —
  *does this get a matrix row* and *is this gridded and smoothed at all* — and **a parameter denied a row
  silently loses its smoothing too**. For the effects those answers diverge: a tap's band corner wants smoothing
  whether or not it earns a row. Splitting them is [STATUS, *Needs a decision*](../STATUS.md).
- **Where the per-sample gain ramp comes from.** There is no shared smoother in `core/` or `plugins/`
  ([MODULATION §4](../suite/MODULATION.md)); the only per-sample ramp is the encoder's own.

**Contingent on** the control grid staying fixed at 256 samples, so "a hop" is one length; on the bounce
promise of 0038 and 0026; and on the measurements listed in [ECHO §10](../echo/ECHO-ENGINE.md) and
[REGIONS §7](../suite/REGIONS.md), all Release arm64, 2026-09-18.

**Would overturn:** a way to schedule a message-thread rebuild at a **sample the timeline determines** rather
than at whichever sample it happens to arrive — then a rebuilt destination could bounce identically and the
rule would loosen to "bounded work *or* a deterministically scheduled rebuild". Or a destination whose rebuild
is measured cheap enough on the audio thread that the distinction stops mattering, which is what the skew ladder
and the SH rotation recursion are proposed as ([ECHO §8](../echo/ECHO-CONTROLS.md)).
