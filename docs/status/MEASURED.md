# Measured, not asserted

Every number the project stands on, with the run it came from. Part of [STATUS](../STATUS.md).

**Cost** — Release build, `bambi-bench`, 128-sample blocks at 48 kHz (2.667 ms):

| | cost | share of one core |
|---|---|---|
| encode, order 3 (16 ch) | 0.226 µs | 0.008 % |
| encode, order 7 (64 ch) | 0.761 µs | 0.029 % |
| encode, order 10 (121 ch) | 1.398 µs | 0.052 % |
| features, spectral on | 1.964 µs | 0.074 % |
| features, spectral off | 0.788 µs | 0.030 % |
| modulation step, 8 cells | 0.086 µs per control hop | — |
| link submit (audio thread) | 0.001 µs | — |
| link scene update, 9 instances (UI thread) | 0.096 µs | — |

The two feature rows are `bambi-bench` on 2026-09-18, after [0043](../DECISIONS.md); the encode rows are
still the 2026-09-11 run. **The 1.848 µs this table published until then was stale** — five runs of the
same default configuration on 2026-09-18 read **2.05–2.10 µs**, so the old figure was about 11 % low.
That is a stale baseline and machine variance between runs, not a regression: 0043's ramp is a multiply
by a value already computed. Quote the spread, not the single number.

An order-3 instance with every feature live costs about **0.08 % of a core** (0.226 + 1.964 µs of
2.667 ms); 32 of them about **2.6 %**, and **2.8 %** at the top of the spread above. The budget is not the constraint it was assumed to be. **Hand SIMD would not help:**
disabling auto-vectorisation costs the encode **0.4 %**, because it is L1-store-bandwidth bound and
the default build already emits 4-wide `fmla` ([PORTABILITY](../suite/PORTABILITY.md)).

**The Decoder's fill of a layout's gaps** — `bambi-bench`, Release, 2026-10-22: on top of the layout's own decode,
**0.37 %** of a core for a pair and **0.56 %** for 7.1.4 at order 3, **1.70 %** for 64 loudspeakers at order 7; the
detail per stage is in [DECODER-FILL](../decoder/DECODER-FILL.md).

**Portability** — one x86-64 cross-build of this source, 2026-09-17, detail and causes in
[PORTABILITY](../suite/PORTABILITY.md): **68 of 68 targets with no source change and no new warning**,
the suite passing under Rosetta, and block-size independence 1…1024 holding — against **9 of 11
audio golden hashes differing by at most −150.5 dBFS**, one float32 ULP. POSIX is confined to **3 of
`core/`'s 22 translation units** and none of its 26 public headers, each of which compiles
standalone. Rosetta is **not** a speed proxy (1.46–3.1× slow), so no cost figure comes from that
build. Since 2026-09-18 the check also **renders every golden scenario with both builds and nulls them**
([0044](../DECISIONS.md)): **11 of 11 null, worst −144.5 dBFS against a −120 dBFS gate, 2 bit-identical,
every control trace identical.** That is how the goldens became architecture-independent; a quantised
hash cannot be, at any word length.

**Orders** — `bambi-orders`, every order 1–35: SN3D exact to **2.9e-14** under exact
quadrature, and a 35th-order instance (1,296 channels) costs **0.63 %** of a core. Neither is
the ceiling; the format is.

**Trajectories and width**

| result | number |
|--------|--------|
| Curvature node placement vs equal arc length (orbit wave, 8 nodes) | **10.04° → 0.20°** |
| Every generator converted to custom at 16 nodes | under **1.0°** |
| Coherent widening at full width | **0.25** of point-source SH energy, exactly `1/(order+1)` |
| Decorrelated widening | **1.000** — preserves it |

**Features** — decided on the Ableton factory library's WAV subset (150 files):

| result | AUC |
|--------|-----|
| Attack: envelope ratio vs spectral flux | **0.848** vs 0.823 |
| Tonal: flatness vs causal HPSS | **0.914** vs 0.843 |
| Level as a content discriminator | **0.576** — chance, i.e. genuinely orthogonal |

**Cross-validation**, C++ against Python on all 246 files (`--seconds 8 --ext .wav`):

| feature | Python | C++ | |
|---|---|---|---|
| Attack | 0.830 | **0.830** | exact |
| Tonal | 0.902 | **0.891** | −0.011 |

The Tonal gap is causal framing: the C++ bank analyses the trailing window because it runs in
real time, where scipy centres each frame on audio it has not heard yet. Tonal-category means
agree to three decimals; only Drums reads hotter. A difference in *when*, not in *what*.

Unplanned: the **Mid band scores 0.898**, matching Tonal for free. Drums vs Tonal in this
library is substantially a midrange-energy distinction — something the matrix can exploit.

Calibration from the library, not theory: flatness spans **−15.4 .. −1.5 dB** on real
material (drums −3.9, tonal −10.6); normalise **−2 .. −13**.

**What a window costs to DRAW** — Release, `bambi-plugin-bench`, CoreGraphics (a native image, which is what
a plugin window is), at the design's own 1:1 scale, 2026-09-20. A frame is thirty a second, so 1 ms is 3 % of a
core. The machine was busy for some runs; read the ratios, not the last digit.

| | before | after |
|---|---|---|
| an effect's two spheres, one repaint | 11.6 ms | **0.62 ms** (1.34 with the energy picture on) |
| a region turning every frame, both views, the layer alone | 2.17 ms | **0.53 ms** (sectors, the dearest kind: 1.19) |
| a region standing still | sampled every paint | drawn from what was kept |
| the encoder's whole editor, one full repaint | — | about 1.4 ms |

**Not measured: a scaled window.** Sampling is in the design's pixels and does not grow with the window; the
draws do. And nothing in a plugin's own window reports any of this: the footer's figure is `dsp`, the audio
thread's alone.

**Live, as the device reads it** — measured once with a stand-in host since removed, a 2026-10 set on this machine: the song position and a
playing clip's `playing_position` each arrive about every 11.5–12 ms, 95% of them within 21 ms. Enough to project
both between reports.
