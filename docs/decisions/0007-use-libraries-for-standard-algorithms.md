# 0007 · Use libraries for standard algorithms

**Status:** accepted

**Date:** 2026-09-11  **Status when written:** decided

**Use an existing library for anything with a canonical implementation.** Hand-roll only
what is domain-specific to this project.

### What prompted it

A hand-rolled radix-2 FFT measured **20x slower than numpy's pocketfft and was numerically
wrong**. A "fixed" second attempt was 10x slower and *still* wrong. Replacing both with
**pffft** — one C file, BSD-style FFTPACK licence, SIMD — gave exact results on the first
run and 6x the speed.

The false economy was thinking "no dependencies" was the goal. The goal is a fast, correct,
maintainable core; a single vendored C file costs 0.3 s of build time and buys decades of
other people's debugging.

### The line

| hand-roll | use a library |
|-----------|---------------|
| spherical harmonics, trajectories, features, the matrix — *nothing else does this* | FFT, resampling, filters, file formats, compression |

### Consequences — all three now closed

Every "open" item recorded when this decision was written has since been acted on.

- **WAV** was hand-rolled and had a real bug: the chunk walk abandoned its search after any
  exactly-consumed chunk, which is every non-extensible `fmt` chunk, so **mono and stereo
  files were unreadable**. Now `dr_wav` (public domain / MIT-0). **Closed.**
- **JSON** was hand-rolled, justified by compile time — "on the wrong side of this line if a
  fast alternative appears". `yyjson` (MIT) is that alternative. **Closed.** One thing did
  not survive the swap unchanged: yyjson parses iteratively and so accepts unlimited nesting,
  while our conversion from its tree recurses. The depth cap moved into the conversion. The
  test that caught this was written for the old parser and had no business still passing —
  it is the reason a stack-smash on a hostile preset did not ship.
- **`shSN3D` was O(order³)**, recomputing the Legendre recurrence for every (n, m). Rewritten
  degree-outer/order-inner so the recurrence carries upward, with a Chebyshev recurrence for
  the azimuth trig (2 trig calls instead of 64 at order 7). **8.3x faster. Closed.**

### The correction this decision needed

The original budget below was **measured in a Debug build**, as was every other number this
project quoted at the time. That single mistake inverted its central claim: it recorded
"pffft is ~3x pocketfft here", concluding our wrapper was the problem and should be moved to
float end to end.

In Release, the same measurement is **2.348 us against numpy's 9.444 us** — pffft is **4.0x
faster**, not 3x slower. There was no wrapper problem: the double<->float conversion and the
per-bin sqrt this decision blamed are already inside that 2.348 us.
The optimisation that decision implied would have been work spent on a cost that did not
exist. `CMAKE_BUILD_TYPE` now defaults to Release for exactly this reason.

**The rule that follows: a performance number without a stated build type is not a number.**

### Budget (Release, from `bambi-bench`)

```
fft 2048                1.730 us
shSN3D order 3          0.048 us
shSN3D order 7          0.097 us
encode block, 16ch      0.238 us    0.009% of a core
encode block, 64ch      0.958 us    0.036% of a core
features block          2.113 us    0.079% of a core   (spectral on)
features block          0.827 us    0.031% of a core   (spectral off)
```

A full order-3 instance with every feature live is **under 0.09% of a core**, so **32
instances land near 2.8%**, and nearer 1.3% for the instances that route nothing to the
spectral features. The budget is not the constraint it was assumed to be.

The features figure was itself measured wrong first. `FeatureBank::prepare`'s second
argument is `fftSize`, and the benchmark passed the block size into it — so it timed a
128-point FFT and reported 0.307 us, a third of the truth, for a configuration that does not
ship. **A benchmark can be wrong in the same direction twice**: first the build type, then
the parameter. Both times the number looked plausible, which is precisely why it survived.
`bambi-bench` now pins the shipping configuration explicitly and reports both spectral
states, and every figure quoted anywhere in these documents comes from running it.
