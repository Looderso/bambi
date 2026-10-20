# bambi Echo — the engine, its cost and its buses

The engine is `core/include/bambi/echo/engine.hpp` (`EchoEngine`), with one pass in `pass.hpp` and the skew in
`warp.hpp`. The model is [ECHO.md](ECHO.md); this page is how it runs.

`turn the axis to the pole → skew → spin → turn back → order gains → the band`

## A sample, for one tap

```
 in ── send region ──► shared input line
                              │ read `offset` ago
                              ▼
              ┌──────────►  ring[i]  ◄── input + feedback × y
              │               │ x = what was written one period ago
              │               ▼
              │           one pass ──► y ──► out += level × y
              └───────────────┘
```

For every awake tap, in chunks of at most 64 frames:

1. `x` ← the ring, one period behind the write.
2. `y` ← one pass of `x`.
3. the output gains `level × y`.
4. the ring ← the input line read `offset` ago, plus `feedback × y`, through the ring-write guard.

A chunk is no longer than the shortest period in play, so it never reads what it writes, and it never straddles
the end of a control hop, so what the engine does does not depend on how the host cuts blocks. The send region is
applied once, before the line, for all four taps. After the loops comes the output stage: `wet` scales the sum,
then `dry` adds the input back (read from the sanitised input, so a NaN cannot reach the output through it).

## What one pass is made of

| | stage | what it is | per tap it holds |
|---|---|---|---|
| 1 | axis to the pole | one (2n+1)² rotation square per order. Skipped when the axis is the pole (elevation exactly 90°, stated by the control layer, never found by comparing floats) | 84 numbers at order 3, 680 at order 7 |
| 2 | skew | the sparse same-m couplings, built for this tap's skew; skipped at skew 0 | 44 at order 3, 344 at order 7 |
| 3 | spin | for each order n and m ≥ 1, the pair (n, +m), (n, −m) turned by m × spin: `a′ = cos·a − sin·b`, `b′ = cos·b + sin·a`. Positive is counter-clockwise seen from the axis's tip. Applied together with the skew's matrix | cos and sin of m × spin |
| 4 | back from the pole | the inverse of 1 | as 1 |
| 5 | order gains | one gain per order: the blur's cap weight at that order | N+1 gains |
| 6 | the band | per channel, a one-pole low-pass at the high cut, then that minus a one-pole low-pass of it at the low cut. Coefficient `1 − exp(−2π f / fs)`, f capped at 0.45 fs. 6 dB an octave each side, and it compounds every pass | two doubles a channel |

- **Blur is the cap weights the encoder uses for source width** (`capWeights`, half-angle `blur`); under 0.25° it
  is no blur. It is the whole of stage 5. There is no separate order cut: blur at 180° leaves exactly
  `[1, 0, 0, 0]` at order 3, a repeat with no direction left.
- **Blur compounds as the root of the passes.** The cap weights at 6° applied j times equal the cap weights at
  6° × √j to within 0.01 (order 3, j = 4, 9, 16; asserted in `core/tests/echo/test_pass.cpp`). A loop applies one
  fixed gain per order, so pass j is gₙʲ, a heat kernel whose width grows as √j whatever the engine does.
- **Blur has no hard edge.** The deepest inverted lobe of the kernel anywhere in the range is −0.37 of its peak,
  at 125°; a hard order clip at ⅓ would put −0.5 straight opposite the repeat.

| blur | cap weights at order 3 | deepest inverted lobe |
|---|---|---|
| 6°, the default | 1, 0.997, 0.992, 0.984 | −0.25 |
| 30° | 1, 0.933, 0.808, 0.641 | −0.18 |
| 60° | 1, 0.750, 0.375, 0.047 | −0.04 |
| 120° | 1, 0.250, −0.125, 0.016 | −0.34 |
| 180° | 1, 0, 0, 0 | none, the kernel is flat |

- **The band's state** is one per tap, in double, cleared when the tap sleeps and at every reset. Low cut reaches
  2 kHz and high cut reaches down to 500 Hz, so the corners can cross; the band then passes very little but is
  never silent.
- **Float in the loop, double in the band.** A float loop against a double reference is −126 dB at pass 48.

## How the operators are built

Everything an automatable or modulated parameter can move is built **inside the control step, from what `prepare`
sized, without allocating**: the two frame rotations, the warp, the order gains, the band's coefficients
(`buildTap`). A hand-over from another thread would land at a different sample in a bounce than it did live.

- **The definition**, SN3D in and out: `M[i][j] = (2nⱼ+1)/4π ∫ Yᵢ(T d) · Yⱼ(d) · dΩ`, with T the rotation or the
  latitude map. **The (2n+1) belongs to the column**; the two placements agree for a rotation, which never mixes
  orders, and differ for a warp by up to 7× in a cross-order entry at order 3. A region is the same integral with
  a gain in place of T ([REGIONS](../suite/REGIONS.md)), whose written form is the transpose.
- **A rotation is not integrated.** Its blocks come from the 3×3 matrix by Ivanic and Ruedenberg's recurrence
  (`math/shrotation.hpp`), exact against the definition to 1e-11 at order 10. It is applied **by R, not R⁻¹**: the
  blocks applied to the encoding of d are the encoding of R·d, and the rotation's tests assert a 90° turn about up
  carries a source at 35° to 125°.
- **A warp is integrated per |m| in one dimension at 64 Gauss–Legendre nodes**: exact to 1e-12 for |a| ≤ 0.9 up
  to order 7. The grid must be that fine because the integrand is rational with a pole at z = −1/a; a grid exact
  for a product of two order-N harmonics (2N + 4 nodes) is wrong by 0.59 at order 3 and 0.92 at order 7 at a = 0.9.
- **A region's projection** onto the bus is the shared one ([REGIONS](../suite/REGIONS.md)).

## What is ramped, and what is stepped

The control step runs every **256 samples** (`kEchoHop`), on an absolute sample grid, and `setFrame` hands the
engine the target for the next hop.

| | behaviour |
|---|---|
| level, feedback, send amount, wet, dry | ramped over the hop, per sample |
| a tap's period | the read is crossfaded from the old distance to the new over the hop; the tail survives, nothing clicks |
| turns, skew, order gains, band, offset | rebuilt each hop and swapped whole at the hop boundary |

Modulated parameters are smoothed upstream by the modulation engine: 720 °/s for an axis, 40 ms for a scalar,
20 ms for the spin's rate ([ECHO-CONTROLS.md](ECHO-CONTROLS.md#modulation-and-ducking)). The offset is an integer
read of the input line.

## Sleeping, waking, restarting

- **Off** sets the level target to zero. Once the tap is off and its level is exactly zero it **sleeps**: ring and
  band cleared, skipped entirely. **On** wakes it with the level rising from nothing over the hop.
- **The ring never changes size, and a clear is virtual**: a count of frames written since the clear, with a read
  past it returning zero. A real memset of one order-7 ring measured 224 µs, 8 % of a 128-sample block; four on
  a tempo change, 34 % (Release, M3 Pro).
- **Play start, locate and loop wrap clear the engine** (rings, band states, input line); smoothers snap, periods
  are latched again. **Stop does not clear**: tails ring out, and live input through a stopped transport works.
  A bounce from the same start point therefore equals playing through to it.
- A held period moves only when the tap's time really moves, so a tempo jittering in its last bit does not
  re-latch it.

### The ring-write guard

What may go round a loop is finite, within ±1e4 (+80 dBFS) and not denormal: NaN becomes 0, an excess is clamped
and counted (`clamped()`, which the processor exposes as the sign a feedback setting ran away), and magnitudes under 1e-15
are written as exact zero, which makes silence exact. The guard is part of the engine because a skewed loop can
swell, and can grow under modulation ([ECHO.md](ECHO.md#the-slide-is-a-latitude-warp)). The same flush is applied
in the band's state, by compare rather than a CPU flag, so every machine agrees.

## Cost

Measured, Release, M3 Pro, as a share of one core at 128 samples / 48 kHz. Figures marked *bench* come from
`bambi-bench`; the others from a throwaway benchmark written to find the shape of the cost.

| | order 3 | order 7 |
|---|---|---|
| the whole engine, four taps awake | **1.59 %** | **5.83 %** (*bench*, as reported in [STATUS](../STATUS.md)) |
| four awake taps, axes off the pole, interleaved rings, fixed-size kernels | 1.25 % | 4.13 %, p99 4.94 % |
| the same with runtime-size kernels | | 18.7 % |
| the same with planar per-channel rings | 2.8× slower | 2.8× slower |
| the warp as a pair list, against fixed-size form | | 3.99 % against 0.38 % |
| rotation blocks built, by recurrence | 0.243 µs | 1.967 µs (*bench*) |
| rotation blocks built, by quadrature | 25 µs | 219 µs |
| rotation blocks applied, in double, a frame | 0.037 µs | 0.160 µs (*bench*) |
| a warp built, 64 nodes | 3.10 µs | 10.72 µs |
| four taps' rotations and warps rebuilt every hop | | about 0.8 % |
| a send region applied, dense, as a column axpy | 0.08 % | 0.48 % |
| the energy picture's two covariances, every frame / every 8th | | 2.7 % / 0.34 % |

- **The layout decides the cost, not the arithmetic.** Rings are interleaved (a frame's channels together) and the
  kernels take the order as a compile-time size. The pair list stays the description of what couples, not the
  loop.
- **CPU is not Echo's constraint at order 7; memory is** ([ECHO.md](ECHO.md#four-loops-each-with-its-own-feedback)).
  Rebuilding axis and skew every hop is cheap enough that cost refuses neither as a modulation target.
- **Denormals.** The M3 shows no penalty for them, so an arm64 run cannot detect the x86 stall; the flush by
  compare costs 0.07 %. Tool and plugin share one scope, `bambi::ScopedFlushDenormals`.
- **Delay memory** is the table in [ECHO.md](ECHO.md#four-loops-each-with-its-own-feedback), held from `prepare`.
- **Tail length** reported to the host is the longest stored time × 8, rounded up rather than derived from live
  parameters: a host reads it once, outside any block.

## Buses and sidechain

`FieldEffectProcessor` (`host/`) owns the layout, shared by every field effect. The **main input and output are
(N+1)² channels, the same order in and out**, for N 1…7; any other pair is refused (`fieldEffectLayoutSupported`).
A **stereo sidechain** is an optional aux input after the main pins. It is never mixed into the field: it feeds
the detectors only, which the modulation matrix reads (ducking, [ECHO-CONTROLS.md](ECHO-CONTROLS.md)). Formats are
VST3 and CLAP; one layout rule serves both, asserted by the conformance suite (`bambi-echo-check`, `tools/conformance.sh`).

Background: decisions 0015, 0029, 0053, 0064, 0129.
