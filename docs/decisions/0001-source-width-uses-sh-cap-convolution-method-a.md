# 0001 · Source width uses SH cap convolution (method A) — provisional

**Date:** 2026-09-10
**Status:** accepted — provisional, pending evaluation in a real ambisonic array

### Decision

Ship **method A** — exact spherical convolution with a cap, applied as per-order gains
`w_n(α)` — as the default width implementation. Do not build a decorrelator bank yet.

### Why

- Reasonable on headphones; A preferred over B over C in listening.
- Cheapest possible implementation: one gain per order, no extra signal paths, no
  convolution, no CPU beyond the encode that already exists.
- Timbrally transparent by construction — it cannot colour the source, because it only
  scales SH components of one coherent signal.

### Why this is provisional

**Binaural HRTF rendering is the wrong instrument for this question.** Perceived source
width depends on how a real sound field decorrelates at the two ears, in a real array, in a
real room. A generic non-individualised HRTF set with no room cannot settle it. The
headphone result says A is not *broken*; it does not say A is *right*.

Two things are known and remain true regardless of the listening rig:

- Coherent widening loses SH energy — measured at **0.25 of point-source energy at full
  width, exactly `1/(order+1)`**, meaning only the omni component survives. Decorrelated
  widening holds at 1.000. Whether that reads as "duller" or "wider" is what the dome
  answers.
- The method C implementation tested was **defective** and its result should carry no
  weight: a decaying-noise-burst IR is a random FIR with a badly non-flat magnitude
  response, it added a direct impulse (direct + diffuse = a comb filter), it decorrelated
  broadband including the low end, and it never collapsed to a coherent point at width 0.
  A fair test needs an all-pass decorrelator (velvet noise or random-phase), a crossover
  around ~700 Hz keeping the low band coherent, and decorrelation depth scaling with width.

### The seam that keeps this swappable

The widening method's entire contract is:

```
encode(direction, width, order) -> SH coefficient vector[]
```

**One** vector means a coherent method; **N** vectors means N decorrelated signal paths.
Nothing downstream — decoder, transform, playback — may know or care which. In the
prototype this is `coefSets()` feeding `decodeToSpeakers()`.

Rules that keep it swappable:

- Width is always expressed as an **angle**, never as an implementation parameter
  (not "order reduction", not "number of spread points").
- Normalisation is defined on **SH energy relative to a point source**
  (`Σ_ch Y_ch² = order+1` under SN3D), so any method is level-matched against any other
  without touching the decoder.
- A method returning N vectors implies N decorrelated feeds; the graph must be able to
  carry them even while method A uses one. Do not collapse that plumbing away.

### Revisit when

A dome or a real ambisonic array is available. If decorrelated widening wins there, the
change is confined to the encode strategy — plus the decorrelator bank and its CPU cost,
which is the part worth budgeting for in advance.
