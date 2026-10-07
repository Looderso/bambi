# HRTF data — making the databases comparable

The decoder's personalisation (DECODER §5) is to modify the KU100 with a model learned from many measured heads.
This page is the data side: which databases, how their grids are unified, and what was measured on the way. The
model itself comes after, and is not PCA — PCA is not shift-invariant, and pinna notches move in frequency from
head to head; PCA may still serve to remove database-specific patterns while keeping the binaural cues.

**What the model is for ([0165](../DECISIONS.md)):** a quick gain over the KU100 from one or two sliders, not the
reconstruction of measured heads. Heads are taken as symmetric — one ear over the sphere, mirrored — so every measured
head gives two. Sections 2–6 predate this and worked on both ears of the heads as measured.

The scripts are in `prototypes/decoder/hrtf-data/`; their plots (plotly) go to `out/` there, served by the prototype
server.

## 1. The databases

A model that ships with the plugin may only be trained on data whose licence allows it. **The licence written
inside a database's files is the one that counts**; it disagrees with the databases' web pages more often than not.

| database | heads | directions | elevation | distance | rate | licence in the files | ships? |
|---|---|---|---|---|---|---|---|
| **HUTUBS** | 96 | 440 | −90…90°, whole sphere | 1.47 m | 44.1 kHz | CC BY 4.0 | **yes** |
| **3D3A** | 38 | 648 | −57…75° | 0.76 m | 96 kHz | CC BY 4.0 | **yes** |
| **SADIE II KU100** (D1) | 1 | 8802 | whole sphere | 1.2 m | 48 kHz | Apache 2.0 | **yes** — the base |
| SONICOM (= AXD) | ~300 | 828 | −45…90° | 1.5 m | 48 kHz | CC BY-SA 3.0 in the mirror's files; MIT in the 2025 paper; "all rights reserved" on its own site | ask |
| ARI | >250 | 1550 | −30…80° | 1.2 m | 48 kHz | CC BY-SA 3.0 — not compatible with GPLv3 (4.0 would be) | ask |
| SADIE II humans | 18 | 2818 | whole sphere | 1.2 m | 48 kHz | University of York non-commercial licence | ask |
| LISTEN | 50 | 187 | −45…90° | 2.06 m | 44.1 kHz | IRCAM, all rights reserved | ask |
| ITA Aachen | 48 | 2304 | 5° × 5° | 1.5 m | — | CC BY or CC BY-NC-SA — sources disagree | ask |
| RIEC | 105 | 865 | −30…90° | — | — | non-profit academic only | no |
| BiLi | 56 | 1680 | −51…86° | — | — | IRCAM, all rights reserved | no |

Analysing a database to make the data comparable is a different question from shipping a model trained on it.
SONICOM matters most: it is the largest by far.

## 2. Unifying the grids with spherical harmonics

**The harmonics are smooth at the poles; the explosion comes from the fit.** Where a grid has no points — the zenith
for ARI and 3D3A, everything below −30…−57° for most — nothing constrains the high orders, and plain least squares
fills the gap with oscillation.

Measured (`grid-experiment.py`): the KU100 sampled at each database's real grid, fitted at order 20, compared with
the whole KU100. CLL error, RMS over 2–16 kHz:

| fit | ARI — inside | zenith gap | below | 3D3A — inside | zenith gap | below |
|---|---|---|---|---|---|---|
| plain least squares | 2.7 dB | 23.8 dB | 1.9·10⁸ dB | 1387 dB | 7.8 dB | 20.4 dB |
| roughness penalty λ·n(n+1) | 0.61 | 1.6 | 4.1 | 1.1 | 1.2 | 2.3 |

**A penalty on roughness stops the explosion**; its best strength sat between 10⁻⁴ and 3·10⁻³ for every grid
(relative to the fit's own scale).

**Filling a gap from a prior head works only if the prior is an average.** Measured (`prior-experiment.py`): each of
20 HUTUBS heads in turn is the truth, the average of the other 95 is the prior, a database's coverage is cut from
the truth's whole sphere. Mean over the 20 heads, CLL / ILD in dB:

| coverage | fill | inside (held back) | above | below |
|---|---|---|---|---|
| ARI −30…80° | smooth only | 1.57 / 4.76 | 0.73 / 1.39 | 10.85 / 9.00 |
| | **deviation from the average** | **1.51 / 4.13** | **0.62 / 1.25** | **4.01 / 5.99** |
| | deviation from the KU100 | 2.48 / 5.44 | 1.01 / 1.52 | 9.41 / 8.39 |
| 3D3A −57…75° | smooth only | 2.55 / 8.28 | 1.07 / 1.78 | 12.24 / 8.58 |
| | **deviation from the average** | **1.87 / 6.03** | **0.94 / 1.69** | **3.65 / 6.23** |
| SONICOM, LISTEN −45…90° | smooth only | 2.48 / 8.16 | — | 11.29 / 8.35 |
| | **deviation from the average** | **1.53 / 4.68** | — | **3.80 / 5.93** |

The average of the others is 4.5 dB (CLL) and 6.4 dB (ILD) from a held-out head over the whole sphere; the KU100
is 10.6 and 8.8 dB from a HUTUBS head. One other head as the prior — tried first, with SADIE's H3 filling the
KU100's gaps — did worse than smoothing alone: the wrong head is worse than no head.

**So the unification is: the deviation from a population average, fitted with a roughness penalty.** Two caveats:

- **The average and the truth came from the same database** here, with the same setup. Across databases the prior
  carries another database's fingerprint; how much that costs is measured after the equalisation onto a common CTF
  (step 3).
- **The ILD is harder than the CLL.** Even from a dense whole-sphere grid, order 20 leaves 1.9 dB of ILD error, from
  the far ear's sharp notches (held at ±40 dB). A gentler representation of the far-ear ILD may be needed.

## 3. Equalising across databases

Measured (`equalise.py`) on HUTUBS (96 heads), 3D3A (38) and SONICOM (140, the mirror's AXD files): each head
resampled to 48 kHz, windowed to 256 samples from its onset, equalised onto the KU100's CTF as the thesis did — the
CTF a head's magnitude averaged over directions, here weighted by area since the grids are not uniform — and unified
by the deviation-from-average fit (HUTUBS first, over the whole sphere; the others' gaps filled from its average).
Compared on the directions all three measured (−45…75°, 1673 directions), 2–16 kHz:

| | averages apart, CLL (pairs) | averages apart, ILD | a head from its own database's average (CLL / ILD) | a classifier names the database |
|---|---|---|---|---|
| raw | 6.1 – 17.6 dB | 2.7 – 2.9 dB | 4.2–4.4 / 5.4–5.8 dB | 99 % |
| equalised | **2.1 – 2.5 dB** | 2.5 – 2.6 dB | 3.4 / 4.8–5.0 dB | **100 %** |

- **Equalisation removes the level and colour a database carries**: the averages' CLL comes from up to 17.6 dB apart
  to 2.1–2.5 dB — less than heads differ within one database.
- **The fingerprint survives it**: a classifier names the database every time — as the study the thesis cites found
  (> 95 %).
- **The CTF is taken per ear**, so a fixed left/right difference of the measurement chain leaves the ILD too (the
  thesis held the ILD unchanged). It averages about 2 dB in every database, and passes 6 dB at some frequency for
  56 of the 96 HUTUBS heads and 14 of the 38 3D3A heads.
- **Part of the ILD gap is distance.** To the side, below 1.5 kHz, 3D3A's ILD is 7.5 dB against HUTUBS's 5.7: 3D3A
  measured at 0.76 m, HUTUBS at 1.47 m, and a nearer source makes a larger ILD. At 2–16 kHz the two agree within
  1 dB.

## 4. Removing the database fingerprint

Measured (`normalise.py`): each direction normalised — a head minus its database's average, plus the average of the
databases, each database weighted equally. In the cross-validation a test head is normalised with the average of
its database's training heads only. Three databases, chance is 33 %:

| | linear classifier | kernel SVM | heads around their own database's average (CLL / ILD) | all heads around the common average |
|---|---|---|---|---|
| equalised | 100 % | 99 % | 3.35–3.45 / 4.79–5.01 dB | 3.68 / 5.14 dB |
| normalised | **33 %** | **54 %** | unchanged | 3.40 / 4.93 dB |

- **The gap between databases is measurement, not people.** Two random groups of heads from within one database,
  of the databases' sizes, differ at most by 0.69–0.80 dB (CLL) and 0.96–1.10 dB (ILD), 95th percentile of 300 draws;
  the databases' averages differ by 2.1–2.5 and 2.5–2.6 dB — three times what the choice of people explains. So
  normalising the averages removes the measurement, not a population.
- **The linear fingerprint is gone**: a linear classifier names the database at chance, and the differences between
  heads are untouched.
- **A non-linear remainder is left**: a kernel SVM still names it at 54 %, nearly all of it telling HUTUBS from
  SONICOM (3D3A is almost never named). It is **not a difference of resolution** from the fit — cutting every head to
  order 15, 10, 8 or 5 leaves it at 54–56 % — **nor of spread** — scaling each database's spread to the common one
  leaves it at 59 %. SONICOM's heads do vary a little more widely than HUTUBS's, 1.06× (CLL) and 1.09× (ILD).

## 5. A two-input model of the vertical cues

The CLL's DTF split at 3 ERB (`extract.py`) into a baseline and a notch layer. The baseline is fixed — the population's
average: a head's baseline sits 1.4–2.4 dB from it (5th–95th percentile, no outliers), the KU100's 3.3 dB, further
than any human head, almost all of it below the ear (5.8 dB there; a head without a torso). The notch layer comes from
two inputs: a **shift** along the ERB axis, a fixed operation, and a **pattern**, a free axis learned from the heads'
notch layers with their own shift taken out (`pattern-models.py`). Held out, 68 heads, auditory measure (dB per ERB,
median):

| | distance from the head | against the KU100 |
|---|---|---|
| the KU100 as it is | 2.16 | — |
| the KU100's notch layer on the population baseline, at its best shift | 1.79 | 17 % closer |
| spectral ordering of the heads (best pattern, shift) | 1.44 | 33 % closer |
| **one-dimensional VAE (best pattern, shift)** | **1.31** | **39 % closer** |
| the head's own pattern at its best shift — the limit of this representation | 0.52 | 76 % closer |

Every one of the 68 heads comes closer than with the KU100. The VAE's axis is smooth (even steps) and its two inputs
are independent (r = −0.04); the ordering's axis zigzags. **The VAE blurs**: its notches are 5.0 dB deep against
7.3 dB for real heads — the first thing to listen for, and to improve. Trained on the Apple GPU in about 1.5 minutes.

**Too weak to hear, and fixed.** Listened to in the match prototype, the one-input VAE's notches were too weak to
move elevation. Two more rounds (`pattern-models2.py`, `pattern-models3.py`), held out as before:

| variant | distance | notch depth at the best place, of the head's own | pad: largest step ÷ median |
|---|---|---|---|
| one free input + shift, as before | 1.35 | 78 % | 1.2× |
| one free input + shift, dips weighted, mirrored, β 0.005 | 1.35 | 144 % | 2.6× |
| two free inputs, the same | 1.33 | 140 % | 3.4× |
| two free, mirrored, depth term symmetric, β 0.02 | 1.33 | 108 % | 2.4× |
| — the same, β 0.05 | 1.33 | 102 % | 2.3× |
| **— the same, β 0.02, randomly shifted ±1.5 ERB while training** | **1.34** | **98 %** | **1.6×** |

- **A loss that weights the dips deepens the notches**; only penalising a shallower notch overshoots (140 %), a
  symmetric depth term lands on the heads' own.
- **Two free inputs do as well as one input and a shift**, so the fixed shift is not needed.
- **Shifting the training heads at random makes the pad even**: the latent space comes out smoother.
- **Every variant overfits early**: the validation loss is best after 200–1300 steps and worse after; with 206
  heads (412 mirrored) more steps do not help, and the best step is kept.

The last is on the pad: two free inputs, notches as deep as the heads', 38 % closer than the KU100.

`export-model.py` turns it into `prototypes/decoder/match/model.js` (outside version control: SONICOM data), which the
match prototype plays as "Model" cues on a pad — both inputs free, interpolated between a 13 × 13 grid.

### 5.1 Where the notches widened, and the fix

Listened to, the notches were still too wide. Measured stage by stage (`stages.py`) on the 68 held-out heads at their
best place, 20 directions of the frontal median plane, the deepest notch of 4–14 kHz in the 3-ERB notch layer
(width at half depth, in quarter-ERB steps):

| stage | depth | width |
|---|---|---|
| real head | 4.7 dB (100 %) | 1.00 ERB (100 %) |
| two-input model (V2b) | 71 % | 100 % |
| exported with order-8 SH, as it played | 61 % | 150 % |
| … through the renderer, as it was heard | 53 % | 175 % |
| a real head through that renderer | 92 % | 125 % |
| a real head through the renderer built on the fine grid | 98 % | 100 % |
| **V4, the decoder giving order-20 SH itself** | **75 %** | **100 %** |
| **V4 through the fine-grid renderer, as it now plays** | **71 %** | **100 %** |

- **The model kept the width; the export and the renderer lost it.** Order-8 SH mixed neighbouring directions, whose
  notches sit at other frequencies (+50 %); the renderer built its magnitudes on the data's 129 bins and interpolated
  them again for the minimum-phase FFT (+25 %). 512 taps instead of 256 alone changed nothing.
- **The fixes**: the decoder's last layer gives order-20 SH (`pattern-models4.py`, loss on 961 directions, plus a term
  for the deepest notch's depth), the page runs the decoder itself (`export-decoder.py`; 65 ms when the pad moves),
  and the renderer builds the magnitudes on the minimum-phase FFT's own grid, 512 taps. The notches now play as wide
  as a real head's.
- **Depth is what is left**: 71 % of a real head's as it plays. V4 holds the rest: 39 % closer than the KU100, the
  notch range 99 % of the heads', the pad's largest step 1.6× the median.

## 6. The horizontal cues

Measured (`horizontal.py`) on all 274 heads, 400 directions: the ILD smoothed to about one ERB (RMS dB over 1.5–16 kHz),
the ITD in µs. Distance from each head, median:

| | ILD | ITD |
|---|---|---|
| the KU100 as it is | 5.05 dB | 50.3 µs |
| the KU100 at the head's best head size | 4.95 dB (2 % closer) | 43.7 µs (13 % closer) |
| **the population average (of the other 273)** | **3.19 dB (37 % closer)** | **27.6 µs (45 % closer)** |

- **Head size explains little.** The KU100's ILD and ITD differ from human heads in shape, not size — the KU100 acts
  like a slightly large head (best ITD scales 0.89–1.00), and its ILD and ITD scales agree only moderately (r = 0.41).
- **The population average is the gain**, as it was for the baseline. It is the default of the match prototype's
  model cues, head size scaling it; the KU100 stays the reference in the plots.
- **The ILD's individuality is mostly smooth**: replacing a head's ILD baseline (3 ERB) by the population's moves it
  72 % of the way to the average, its notch layer 50 % — the reverse of the CLL.
- **The ITD is not one scale factor**: one factor takes out 14 % of a head's difference from the average; about 30 µs
  remains, alike at every elevation. Part of it may be where a subject sat rather than their head — a few millimetres
  off centre bends the ITD with azimuth — which this data cannot tell apart.

## 7. Notches as objects — a first look

The model of §5 averages notches that sit at different frequencies, and blurs them. The other way round: a notch is an
object — centre, depth, width — set on a smooth part. What is simple (head and torso, the smooth part) is split from
what is not (the pinna's cues), and the pinna's cues are a few features.

**Where the notches are.** Every notch below −3 dB at 4–16 kHz in the 3-ERB notch layer of the CLL, all 274 heads,
median plane (the points of `extract.py`'s notch plot):

| region | notches per head and direction | most of them at |
|---|---|---|
| front and front-below | 1.9–2.2 | 5–10 kHz |
| front-up | 1.25 | 8–9 kHz |
| overhead | 0.46 | 11–14 kHz |
| behind | 1.0–1.5 | 8–11 kHz |
| behind-below | 1.75 | 7–8 kHz, and above 11 kHz |

One trajectory shows through the whole population: 6–9 kHz in front, rising to 11–14 kHz overhead, falling again
behind. Overhead there is mostly no notch at all.

**The notch layer rebuilt from a few features** (`notches/build-notches.py`): per ear, on an ERB axis, Gaussian dips and
peaks fitted through the same 3-ERB split, at 3–18 kHz. Over the median-plane directions of the KU100 (35) and of one
human head (23, measured every 10–15°), both ears, 4–16 kHz:

| | notch layer kept — KU100 | human | deepest notch's depth — KU100 | human | from the real head, dB per ERB — KU100 | human |
|---|---|---|---|---|---|---|
| the smooth part alone | 0 % | 0 % | 0 % | 0 % | 1.78 | 1.71 |
| 2 notches, free in every direction | 83 % | 56 % | 81 % | 81 % | 0.68 | 1.16 |
| 2 notches and a peak | 89 % | 71 % | 83 % | 82 % | 0.52 | 0.88 |
| 3 notches and 2 peaks | 95 % | 86 % | 90 % | 91 % | 0.32 | 0.59 |
| 2 notches and a peak, each on one curve over the polar angle | 73 % | 54 % | 65 % | 50 % | 0.79 | 1.09 |

- **A few free features keep the notches deep**: 81–91 % of the deepest notch's depth. (§5.1's model plays 71 %,
  measured on the CLL in front only — not the same measure.)
- **A human head holds more than the dummy**: two notches keep 83 % of the KU100's notch layer and 56 % of the human
  head's; with a third notch and two peaks, 95 and 86 %.
- **On curves the depth goes**: with centre and depth a Fourier series of order 5 over the polar angle and one width
  (23 numbers a feature and ear), a narrow notch's centre is missed by enough to lose a third to a half of its depth.
  Order 3 was worse (53 % and 37 % depth). The human head's 15° grid is coarse for a curve.
- **Notches do not belong on spherical harmonics as spectra** (§5.1: order 8 widened them by half); their centre, depth
  and width per direction vary slowly and might. Untested off the median plane.
- **A fractional delay needs room ahead of the response**: delayed by a part of a sample from its first tap, a
  minimum-phase response lost up to 15 dB in a notch. Every response is led by 32 samples; the real version is then
  within 0.03 dB of the measured magnitudes.

**Whether it carries elevation is for ears** — `prototypes/decoder/notches/`, an A/B page: any measured direction of
the median plane, the real head against its smooth part alone and the four rebuilds, switched while the signal runs,
with the spectra and the notch layer of what plays ([LISTENING.md](../LISTENING.md)). If two or three free features
do not carry it, the idea ends here; if the curves do not, the features need a better description over direction
before they need fewer numbers.

### 7.1 The split itself

**A blur is the wrong split.** The smooth part of §5 and of the table above is the spectrum blurred with a 3-ERB
Gaussian. A notch drags the blur down: the smooth part keeps a dent where the notch is, and the layer gets a bump on
either side. One 15 dB notch on a flat spectrum:

| the notch's width (at half depth) | bump beside it | dent in the smooth part | depth left in the layer |
|---|---|---|---|
| 0.5 ERB | +2.1 dB | −2.5 dB | −12.5 dB |
| 1.0 ERB | +2.9 dB | −4.7 dB | −10.3 dB |
| 1.5 ERB | +2.9 dB | −6.7 dB | −8.3 dB |

So the peaks the fits above needed were mostly the split's own bumps. **What is wanted: a layer that is 0 dB except
in the notches, and a smooth part in which no notch can be seen.** A split that rides over the dips — blurred, the
spectrum lifted to the blur wherever it lies below, and again — gives the same notch no bump, no dent and its whole
depth; on the KU100 and one human head (median plane, both ears, 4–16 kHz) the layer above 0 falls from 1.6–1.8 dB
RMS to 0.11, and two plain dips, no peaks, keep 86 % and 79 % of it (63 % and 41 % with the blur). Run long, it
bridges wide valleys too, and the layer is no longer only notches: how wide a valley still counts as a notch is the
split's one open choice.

**The split is settled by looking**, in `prototypes/decoder/split/`: any head of the databases (`build-heads.py`
writes one file a head), as measured or made symmetric from its left or its right ear ([0165](../DECISIONS.md)); ITD,
ILD and CLL over the median plane or a horizontal ring at any elevation; the CLL beside its smooth part and its notch
layer, for four splits with their knobs:

- **the blur** — the reference for what goes wrong;
- **the sheet** that rides over the dips (blur width, passes);
- **a rolling maximum, then minimum**, whose knob is the widest notch filled;
- **the smooth part and a few dips fitted together** — the dips are taken out before the blur, so they leave no dent,
  and the layer is exactly the dips; what is neither is shown as left over. One 15 dB notch on a slowly varying
  spectrum comes back 14.8 dB deep, the smooth part within 0.25 dB of the true one.

Notches are allowed only above a chosen frequency, and the smooth part can be held to a spherical-harmonic order over
the sphere. **One setting for everything: flat above a chosen frequency** — each ear's spectrum fades over two ERB to
the level it has just below, since the top of the range varies widely between directions and heads and colours the
sound more than it places it. The page plays the place as measured, rebuilt from CLL, ILD and ITD, as smooth part
plus notch layer, as the smooth part alone, and as the notch layer alone on a flat CLL; each is within 0.1 dB of the
spectrum it is meant to have.

### 7.2 Three parts, over all heads

Settings chosen in the page: the smooth part and four dips fitted together, blur 3 ERB, dips above 4 kHz, the smooth
part as spherical harmonics of order 4 — on five HUTUBS heads the fourth dip takes what is left over from 1.2–1.5 to
1.1–1.3 dB and is used (four dips deeper than 3 dB in 43 % of the directions), and the order has no knee: 1.4–1.7 dB
from the free smooth part at order 4, about 0.3 dB less with each order.

`run-split.js` runs the page's own code (`split.js`) over every head, left ear mirrored and right ear mirrored: 552
symmetric heads (HUTUBS 192, 3D3A 76, SONICOM 280, the KU100 and one other), about a second each; the three parts of
[0166](../DECISIONS.md) go to `~/Documents/HRTFs/split-run/`. `analyse-run.py` takes the databases' offsets out and
measures:

| | |
|---|---|
| left over, neither smooth nor dip (4–16 kHz) | 1.01 dB RMS (0.77–1.26, 5th–95th percentile of the heads) |
| the order-4 pattern from the free smooth part (0.3–16 kHz) | 1.11 dB RMS (0.81–1.61) |
| dips deeper than 3 dB, a direction | four 47 %, three 28 %, two 15 %, one 7 %, none 3 % |
| such a dip | 7.1 dB deep, sigma 0.36 ERB (median) |
| the nearest such dip in the neighbouring direction, about 4° away | 0.11 ERB apart (median); 86 % within 0.5 ERB |

**The databases' offsets are larger than the heads' differences, and they go.**

| part | the databases' means apart | a head from its own database's mean | a linear classifier names the database: as it is → means out |
|---|---|---|---|
| the average | 2.05–2.75 dB | 1.48–1.68 dB | 96 % → 34 % |
| the pattern | 1.44–1.59 dB | 1.01–1.31 dB | 100 % → 31 % |
| the dips | — | — | 84 % → 30 % |

Balanced accuracy, chance 33 %; both ears of a head held out together, the means taken from the training heads only.

**No single axis holds the heads' variation.** Principal components, the offsets out:

| | 1 | 2 | 3 | 5 | 10 |
|---|---|---|---|---|---|
| the average (a head 1.56 dB from the mean) | 33 % | 59 % | 69 % | 82 % | 94 % |
| the pattern (a head 1.18 dB from the mean, over the sphere) | 25 % | 43 % | 48 % | 57 % | 71 % |

A share of variance is not a share of what is heard, and a stretch along frequency — what a larger head or ear does —
is not one straight axis to a principal-component analysis. What the first components are, and whether a stretch
holds more, is next.

**The dips form paths over direction**: nine in ten have a neighbour within one ERB in the next direction.

### 7.3 The dips as sheets, and ITD and ILD as patterns

Fitted alone, a direction's four dips have no names and nothing ties them to the next direction's: a dip drops out
for one direction and is back in the next. `prototypes/decoder/sheets/` (its own code, `lab.js`; it reads the data set
of §7.2) names the dips from the front outward — a dip takes the name of the nearest dip in the directions already
done — and fits them again with the neighbours in view. On three heads (HUTUBS, HUTUBS, SONICOM), over the sphere:

| | left of the notch layer, 4–16 kHz | gaps | step to the namesake next door |
|---|---|---|---|
| each direction alone, named | 1.22 / 1.12 / 0.80 dB | 35 / 44 / 26 % | 0.21 / 0.27 / 0.13 ERB |
| the neighbours' spectra count too (reach 6°) | 1.49 / 1.40 / 1.09 | 29 / 34 / 21 % | 0.15 / 0.14 / 0.09 |
| tied to the neighbours' dips (reach 8°, pull 0.3) | 1.40 / 1.37 / 1.00 | 14 / 20 / 6 % | 0.13 / 0.14 / 0.10 |
| — pull 1 | 1.68 / 1.57 / 1.30 | 13 / 15 / 5 % | 0.14 / 0.13 / 0.09 |

A gap: a dip deeper than 3 dB whose namesake in the nearest direction is shallower than 1.5 dB or more than 0.5 ERB
away. **Tying the dips' parameters to the neighbours' closes more gaps for less than averaging the neighbours'
spectra**, which is what a Gaussian over directions in the fit amounts to: a notch moves in frequency from one
direction to the next, and averaged spectra blur it.

**The tie invents notches, and more the harder it pulls.** It holds a dip's depth near its neighbours' too, so a
neighbour's notch is copied into a direction whose spectrum has none. Dips deeper than 3 dB where the spectrum itself,
around the dip's centre, is less than half as deep, ten heads: alone 0.2 %, tied with pull 0.3 0.5 %, pull 1 4.3 %,
pull 3 17.1 %.

**Tracks: sheets that begin and end** — the page's fourth way, after the partial tracking that work on pinna notches
borrows from sinusoidal modelling (peaks linked from frame to frame, tracks born and ended, short ones dropped):

1. a dip counts only where it is 3 dB deep and its own spectrum supports it — a direction has as many dips as count;
2. dips of neighbouring directions are linked where each is the other's nearest and their centres are within 0.6 ERB;
   links join dips into sheets, the closest first, never two dips of one direction in a sheet; sheets of fewer than
   8 directions go;
3. a hole inside a sheet gets a dip at the centre and width its neighbours suggest, its depth fitted to that
   direction's own spectrum, and it stays only if it is 1.5 dB deep and supported;
4. all dips are fitted once more, centre and width held near the sheet's neighbours, the depth by nothing but the
   spectrum.

On three heads (HUTUBS, SONICOM, SONICOM): nothing unsupported (0.0 %), 0.97–1.40 dB left (tied: 1.00–1.40), a step
of 0.06–0.10 ERB to the namesake next door, 2.5–2.9 dips deeper than 3 dB a direction — in 28 to 62 sheets, the
largest over a third to a half of the directions. Many of those are pieces of one notch that no link joined; joining
them is the next step. (Gaps by the measure above: 12–19 %, a sheet's edge counting as one.)

**From tracks to a few surfaces.** Tracks alone leave a notch in pieces — 43 sheets a head on four heads, 48 % of the
dips in the six largest. Two things gather them, and the page now runs the whole chain from the measured head itself
(`lab.js`'s `chain`, in a worker; 1.5–3 s a head, then under a second a setting):

1. **Prepare what the fit sees.** Blurred 1 ERB along frequency: 32 sheets, 54 % in the six largest, fewer and wider
   dips (2.2 a direction, from 2.7). Held to order 8 over the sphere: 34 sheets, 59 %, the largest over 51 % of the
   directions (from 35 %), the dips 1 dB shallower (6.8 from 7.9 dB) — less than a notch's move across directions
   suggests. Order 12 does little.
2. **Surfaces.** The largest sheets grow: a surface's centre over the sphere is a spherical-harmonic fit (order 4) of
   its dips' centres, and a dip next to it, within 0.5 ERB of where it passes, joins it. Six surfaces take 61–75 % of
   the dips unprepared, 70–90 % after order 8.
3. **Back to the input.** The dips, found on the prepared CLL, are fitted to the measured CLL of their own direction —
   the centre held, the depth from the spectrum alone; where a surface passes a direction without a dip, one is tried
   there; what the measured spectrum does not support goes.

With blur 0.75 ERB, order 8 and six surfaces, three heads: 1.2–1.6 dB left of the measured notch layer, nothing
unsupported, 1.7–2.3 dips deeper than 3 dB a direction. The settings are for looking at; none is chosen yet.

**The settings are found by search, against one cost.** A surface is in the end three patterns over the sphere —
its dips' centre, depth and width as spherical harmonics of one order, so 3 × surfaces × (order + 1)² numbers a head
(`asNumbers`; with centre and width from the patterns, a direction's depths are the ones that fit its measured CLL
best, and the depth's pattern is fitted to those). **The cost: what the dips rebuilt from those numbers, and the smooth
part, leave of the measured CLL, blurred to one ERB — dB RMS over the sphere and 4–16 kHz.** It holds both aims: the
likeness to the input, and few long sheets — a broken or jumpy surface cannot be told by a low-order pattern and
rebuilds badly. `search.js` (sixty-four random settings, then twice each knob a step either way from the best; twelve
heads, four a database; 16 minutes in eight processes) writes `presets.json`, which the page offers:

| surfaces × order | numbers a head | cost, settings by eye | found | on twelve other heads |
|---|---|---|---|---|
| 3 × 4 | 225 | 1.26 dB | 1.09 | 1.24 |
| 4 × 4 | 300 | 1.18 | 1.06 | 1.14 |
| 6 × 4 | 450 | 1.11 | 0.99 | 1.07 |
| 8 × 4 | 600 | 1.06 | 0.95 | 1.02 |
| 3 × 6 | 441 | 1.32 | 1.00 | 1.10 |
| 4 × 6 | 588 | 1.26 | 0.99 | 1.07 |
| 6 × 6 | 882 | 1.20 | 0.94 | 1.00 |
| 8 × 6 | 1176 | 1.23 | 0.95 | 0.95 |

The smooth part alone leaves 1.73 dB.

- **The sizes are close**: from 225 numbers to 1176 the cost falls by 0.14 dB; the dips take the smooth part's 1.73 dB
  to about 1.0 whatever the size. Six surfaces at order 4 is where more numbers stop paying.
- **No knob matters much**: a step of a knob moves the cost by 0.05 dB at most (the blur), the floor, the smallest
  sheet, the pull and the bridge by under 0.01.
- **The search ran into the edge of its ranges**: the neighbours' reach (18°) and the distance to join a surface
  (1–1.2 ERB) sit at the largest value offered in most sizes. Wider ranges are the next run.

**What a head still needs to be played is not in the data set yet**: the ITD is stored per direction, the ILD not at
all. As patterns on the sphere, 23 heads, left ear mirrored:

| order | 1 | 2 | 3 | 4 | 6 | 8 |
|---|---|---|---|---|---|---|
| ITD off its pattern (422 µs RMS over the sphere) | 47 µs | 44 | 27 | 21 | 13 | 11 |
| ILD, blurred 3 ERB, off its pattern (1–16 kHz) | 3.5 dB | 3.1 | 2.5 | 2.3 | 1.8 | 1.5 |
| ILD as measured off that pattern (13.2 dB RMS, fine structure 2.8 dB) | 4.8 dB | 4.5 | 4.0 | 3.8 | 3.5 | 3.3 |

An ILD without fine structure puts every dip of the CLL into both ears alike. The page shows both patterns beside
the measured ITD and ILD, their orders on sliders. **The ITD is taken at order 8** (11 µs). **The ILD loses its detail
to the blur, not to the order**: fitted on three quarters of the directions and compared with the measured ILD on the
rest (17 heads, 1–16 kHz), SONICOM's goes from 3.96 dB (order 4) to 3.31 (order 12) blurred 3 ERB, and from 3.59 to
2.44 not blurred; HUTUBS's stops near 3.8 dB at order 8, its 10° grid holding no more. **The ILD is taken not blurred, at order 10** — chosen by
looking and listening; both pages start there.

**A surface is seen as a map**: the sheets page's bottom row draws each surface over the whole sphere, every measured
direction a patch whose colour is the notch's frequency there and whose strength its depth — as found per direction,
or rebuilt from the numbers. On the first HUTUBS head, six surfaces at order 4: two cover most of the sphere (5.8–9.2 kHz
over 74 %, 12.8–17.8 kHz over 71 %), four a third each, between 9.4 and 15.2 kHz.

**Heard, not only plotted**: `sheets/listen.html` plays a head at any measured direction as measured, through CLL,
ILD and ITD, with the CLL from its numbers, with ILD and ITD from their patterns, all from the numbers, and as the
KU100. At one direction of one head (30° left, level; per ear, 1–16 kHz): through CLL, ILD and ITD 0.1–0.2 dB from
the measured responses; all from the numbers 2.4–3.2 dB; the KU100 5.9–6.9 dB. Whether that carries is for ears
([LISTENING.md](../LISTENING.md)).

### 7.4 Do the heads share their notches?

`run-sheets.js` runs the chain over all 552 symmetric heads with the search's settings (about four minutes in eight
processes; `~/Documents/HRTFs/sheets-run/`, and `sheets-run-3/` for three surfaces): **the smooth part alone leaves
1.78 dB, rebuilt from six surfaces 1.00 dB** (0.81–1.24, 5th–95th percentile of the heads) — the chain holds on
every head, not only the twelve it was tuned on.

`template.py` lays the heads' surfaces over each other: each folded onto the left half of the sphere (the heads are
symmetric), two surfaces apart by how far their centres differ where both have a notch plus 2 ERB for the share they
do not share, grouped around K typical surfaces.

| | six surfaces a head | three surfaces a head |
|---|---|---|
| groups, by frequency (K = 3) | 5.9–7.8, 8.4–10.6, 12.0–16.1 kHz | 6.5–10.0, 9.8–13.0, 13.0–16.3 kHz |
| heads that have each | 94, 99, 100 % | 97, 76, 55 % |
| separation of the groups (1 apart, 0 not) | 0.47 | 0.46 |
| heads with two surfaces in one group | 100 % | 68 % |
| a surface from its group's typical one: offset, then what is left | 0.6–1.0 ERB, 0.42–0.46 ERB | 0.56–0.65, 0.39–0.52 ERB |
| overlap with the typical surface | 39–50 % | 42–49 % |
| a head's offset, spread between heads | 0.77–1.19 ERB | 0.75–0.80 ERB |
| a person's left ear against the right (correlation of the offsets) | +0.48, +0.33, +0.32 | +0.59, +0.34, +0.48 |
| the groups' offsets against each other, over the heads | −0.05 … +0.27, mean +0.12 | −0.10 … +0.31, mean +0.12 |

- **The heads share notch bands, the lowest above all**: a notch at 6–10 kHz is in nearly every head, and how high it
  sits is a property of the person — the two ears agree (+0.59) and the offset falls with the measured ear (HUTUBS,
  186 ears: pinna height −0.33, cavum concha depth −0.26, cavum concha height −0.25).
- **Surfaces found head by head do not correspond cleanly**: the groups are only half apart, a surface shares half
  its notch with its group's typical one, and most heads put two surfaces into one group and none into another.
  Grouping afterwards does not name them.
- **The notches do not move together**: a head whose lowest notch sits high does not have its other notches high
  (mean correlation +0.12; one axis holds 44 % where independence would give 33 %). One slider that shifts every
  notch is not what the heads do; a slider for the lowest notch is.
- The matching is crude, and part of the weak agreement is its noise, not the heads'.

**What follows**: the template comes first, not last — the typical surfaces are fixed for all heads, and each head's
surfaces start from them (shifted to fit) instead of from its own largest tracks. Then a surface is the same notch on
every head by construction, and a head is how it bends the template.

### 7.5 The ladder: what a few numbers a head buy

`ladder.js`: a template — three notch surfaces, the same for every head, each its centre, depth and width over the
sphere at order 6, symmetric left to right — and per head a shift along frequency and a depth for each notch. The
template is learned from half of the people, in turns: each head's numbers fitted to its measured CLLs with the
template held; then each head's own dips (found and checked against its spectrum) given to the template notch they lie
nearest, the shifts taken out, and the template fitted to all of them. A surface is then the same notch on every head
by construction. Four turns settle it (1.55, 1.45, 1.42, 1.42 dB on the people it did not learn from); 22 minutes in
ten processes.

Every rung scored with §7.3's cost, median over the 274 heads of the people the template did not learn from (the 274
it learned from are within 0.03 dB):

| | numbers a head | cost left | of the way from the KU100's notches to the head's own surfaces |
|---|---|---|---|
| no notches: the smooth part alone | 0 | 1.80 dB | — |
| the KU100's notches for everyone | 0 | 1.79 | 0 % |
| the KU100's notches, one shift | 1 | 1.73 | 8 % |
| the template as it is | 0 | 1.71 | 10 % |
| **the template, one shift for all notches** | **1** | **1.53** | **32 %** |
| the template, a shift a notch | 3 | 1.45 | 43 % |
| the template, a shift and a depth a notch | 6 | 1.42 | 47 % |
| the better of two templates, a shift and a depth a notch | 7 | 1.38 | 52 % |
| the head's own six surfaces | 450 | 1.00 | 100 % |

- **The KU100's notches are worth nothing to another head**: 1.79 dB against 1.80 with no notches at all.
- **One number does most of what a few can**: one shift for all notches takes a third of the way; a shift a notch
  43 %; the depths and a second template add little.
- **The two lower notches move together**: fitted to the template, a head's shifts of the notches at 7.4 and 9.6 kHz
  (in front) correlate at 0.75; the third, near 14 kHz and shallow (2 dB in the template, left out in 7 % of the
  heads), goes its own way (0.29). §7.4's "the notches do not move together" was its matching's noise. The shifts
  spread by 0.71, 0.85 and 1.05 ERB between heads.
- **Half the way is not a shift**: from 1.42 to 1.00 dB is how a head's notches travel over the directions, which no
  shift or depth reaches.
- The numbers are fitted against the measured head; whether a listener finds them by ear is another question. And
  the smooth part here is the head's own: the ladder is the notches' part alone.

**Nobody's head and the ear slider, heard.** In the product no head is chosen: every part is the population's, and
the sliders make the head. `run-sheets.js` now stores every head's other parts too — the CLL's average, the smooth
pattern (order 4), the ITD's pattern (order 8) and the ILD's (order 10, not blurred) — and `generic.js` averages them,
each database's mean first, then the mean of those: nobody's head, in `sheets-run/generic.json`. `sheets/listen.html`
plays, against a listener's measured head: the listener from its own numbers; the KU100; nobody's head without notches
and with the template's notches under one ear slider, moved while the sound runs; and, on the listener's own smooth
part, the template with the slider, with the fitted six numbers, no notches, and its own notches. A button shows the
shift the ladder fitted ([LISTENING.md](../LISTENING.md)). At one place of one head (30° left, per ear, 1–16 kHz, from
the measured responses): the listener's own numbers 2.7–3.5 dB, nobody's head with the slider at rest 4.8–5.0, the
KU100 5.7–6.7, nobody's head without notches 6.1.

**The notches came out shallow, and it is heard.** Fitted per direction a notch is 7.1 dB deep (median; 20–25 dB in
front) and 0.36 ERB wide (sigma); a head's surfaces rebuilt from their numbers are 4.8 dB and 0.54 ERB; the template's
three notches 6.7, 4.5 and 2.4 dB (median over the sphere) and 0.5–0.6 ERB. At one place of one head the measured CLL's
deepest notch is 14.8 dB and the template's 3 dB. Three things did it: the cost compares after a blur of one ERB,
where a narrow deep notch and a wide shallow one of the same area are alike, so every fit could go wide and shallow;
a surface's depth, centre and width as low-order patterns even the depth out; and the template averages heads. *Heard
2026-10-05: the notch is gone, and the whole curve is further from the measured one for it — a blur of one ERB is not
what the ear forgives.* The listening page has the template's depth and its sharpness (narrower and deeper, the same
area) on sliders; twice as sharp gives 8.9 dB at that place, depth 1.5 and sharpness 2.5 gives 17.7.

**How deep and how sharp, from the data** (`calibrate.js`, 35 heads, each with the one shift the ladder fitted):

| asked how | depth × | sharpness × |
|---|---|---|
| closest point for point, the cost as it is (blurred) | 1 | 1 |
| closest point for point, without the blur | 0.75 | 1 |
| as the curves' own notches: the same reading of measured and template | 0.75 | 2.5 |

- **"Closest" cannot find it.** Point for point, a notch a little off costs more the deeper and narrower it is: at
  sharpness 2.5 the unblurred cost rises from 2.4 to 3.1–3.6 dB. Least squares hedges — which is how the notches came
  out shallow in the first place, and why the cost must not be the judge of a notch's shape.
- **Read the same way from both** — the deepest notch of a direction's CLL below its own 3-ERB blur, and its width at
  half depth: measured 8.4 dB deep (median), 0.60 ERB wide, deeper than 6 dB in 69 % of the directions; the template
  as it is 3.6 dB, 1.10 ERB, 5 %; **at depth × 0.75 and sharpness × 2.5: 9.6 dB, 0.60 ERB, 70 %.** The listening page
  starts there.
- The template claims a notch in places where the head has a dip in 47 % of the cases only: half of what it plays is
  a notch this head does not have there.

**Debt**: the cost without the blur, the widths held near the measured ones, and the search and the ladder run again —
and a judge of a notch's shape that is not point for point.

*Heard 2026-10-05: depth × 1.5 at sharpness × 2.5 is better and plainer than the data's × 0.75 — the template's notches
want to be deeper than a head's own to be heard as notches. The page starts there.*

### 7.6 The head: what one number buys for everything but the notches

`head-ladder.py`, on the parts `run-sheets.js` stores for every head (each database's mean out, the mean of the
databases in; learned on half of the people, scored on the 274 heads of the others — how far each part is from the
head's own, RMS over the sphere, median):

| | numbers | ITD | ILD (1–16 kHz) | smooth pattern | average |
|---|---|---|---|---|---|
| the KU100 for everyone | 0 | 49.7 µs | 4.50 dB | 2.63 dB | 3.53 dB |
| nobody's head — the mean of the heads | 0 | 34.3 | 3.34 | 1.26 | 1.66 |
| **+ the ITD scaled** | 1 | **20.6** | 3.34 | 1.26 | 1.66 |
| + the ILD scaled | 1 | 34.3 | 3.27 | 1.26 | 1.66 |
| + the pattern scaled | 1 | 34.3 | 3.34 | 1.21 | 1.66 |
| + a stretch along frequency (average, pattern, ILD) | 1 | 34.3 | 3.32 | 1.26 | 1.57 |
| + size: the ITD scaled, the spectra stretched the other way | 1 | 22.5 | 3.42 | 1.30 | 1.73 |
| + the heads' first component, all parts together | 1 | 34.9 | 3.35 | 1.24 | 1.35 |
| + components 1–3 | 3 | 35.4 | 3.27 | 1.24 | 0.87 |

- **The step is from the KU100 to nobody's head**: every part comes a quarter to a half closer with no number at all.
- **The head slider is the ITD's scale**: 34 µs to 21. Nothing else one number does moves the ILD or the smooth
  pattern by more than 0.07 dB — not a scale, not a stretch along frequency, not a head's size tying the two.
- **The components are timbre**: they take the average from 1.66 to 0.87 dB and leave the rest — and the timbre is
  the part [DECODER §5](DECODER.md) holds on the KU100's.

**The head slider is the ITD's scale with the low ILD going with it.** The table scores the ILD with all its detail,
which no smooth change can reach, and tries only a stretch nobody measured. Asked better — a head's ITD scale as its
size, and how the ILD's rough shape (order 4, blurred 3 ERB) changes with it, learned from half of the people:

| the ILD's rough shape, from the head's own | nobody's head | + what goes with the head's ITD scale |
|---|---|---|
| 0.3–1.5 kHz | 1.01 dB | 0.88 dB |
| 1.5–4 kHz | 1.14 | 1.13 |
| 4–16 kHz | 2.14 | 2.13 |

Below 1.5 kHz the ILD follows the head's size, above it does not — as the duplex picture has it. So the slider scales
the ITD and moves the ILD along that learned line below 1.5 kHz, the change gone by 3 kHz, the ILD above the
population's (`generic.js`: `ildWithSize`; at 90° to the side, a scale of 0.85, 1, 1.15 gives 620, 732, 839 µs and a
low ILD of 5.8, 6.1, 6.3 dB). A head's ITD scale spreads by 0.087 (0.85–1.14, 5th–95th percentile).

**The smooth CLL stays the population's.** It varies (1.2 dB from head to head, most of it above 4 kHz), but with
neither slider: moved with the ITD scale 1.22 → 1.21 dB; with the ear shift, above 4 kHz, 1.74 → 1.72 dB (the two
agree there, r = 0.60, and it buys nothing). Its own sliders would be timbre's.

**The timbre is the KU100's** ([0167](../DECISIONS.md)): `generic.js` stores the KU100's timbre beside nobody's head,
and the listening page gives it to every version — the measured listener too, through a minimum-phase filter of the
change — or leaves each its own. Relative to 1 kHz, averaged over the directions, at 4 and 16 kHz: a listener measured
+9.2 and −4.7 dB, nobody's head +6.1 and −6.6, the KU100 −1.0 and −2.3; with the KU100's for all, every version within
−0.6…−0.8 and −0.5…−1.8 dB. *Heard 2026-10-05, before the swap: nobody's head with both sliders is "very" good; the
KU100 sounds like white noise all around, the heads coloured.*

### 7.7 The template learned honestly

`honest.js`: the same three notches, learned without a least-squares bet. Per head the dips are fitted to the measured
CLL again with depth and width free (the way back's pull on them 0); a head's shift is found by a scan — the one under
which its dips meet the template's notches best — then the median of what is left; each notch's depth is fitted to the
dips given to it alone (how deep it is where a head has it), and its presence over the sphere to 1 where a head has it
and 0 where not; the template plays a notch where the presence passes 0.4. Four turns, four minutes in ten processes;
the heads' shifts spread by 0.58 ERB.

| on the 274 heads not learned from | measured | the ladder's template | learned honestly |
|---|---|---|---|
| a direction's deepest notch, median | 8.5 dB | 3.6 dB | 5.6 dB |
| its width at half depth | 0.60 ERB | 1.10 ERB | 0.90 ERB |
| directions with a notch deeper than 6 dB | 70 % | 5 % | 37 % |
| the heads' dips deeper than 3 dB met by a template notch within 0.5 ERB | — | — | 42 % |
| the template's notches with such a dip under them | — | 47 % | 59 % |

In front the honest template has its notches at 7.3, 9.6 and 14.2 kHz, 9.5, 9.6 and 9.2 dB deep, present in 61, 62
and 22 % of the heads. Depth and sharpness × 1 are now the plain template; the page holds both templates on a
switch, the ladder's with × 1.5 and × 2.5.

- **Deeper and more often a notch, and righter**, but not yet as the heads: the depth map over the sphere (order 6)
  evens the deep places out, and presence under 0.4 drops notches a third of the heads have.
- **The dips are wider than the notches they stand for.** Fitted with width free, a head's dips settle at a sigma of
  0.44–0.49 ERB (fresh fits on the measured CLL 0.35–0.40), where the curves' notches are 0.60 ERB wide at half depth,
  a sigma of 0.25: a notch is not a Gaussian in dB — a sharp bottom with wide skirts — and a Gaussian fitted to it
  goes wide and shallow.

**A dip shaped as a peaking filter's notch** (`lab.js`: 1/(1+u²) in dB, a sharp bottom and wide skirts; `setShape`)
**fits the notches as they are.** Fitted fresh per direction on two heads, the dips' own deepest notch a direction
comes out 8.7 and 9.4 dB deep and 0.70 and 0.60 ERB wide (bells: 7.5 and 8.4 dB, 0.80 and 0.70 ERB; measured 9.4 and
10.0 dB, 0.60 ERB), for 0.1 dB more left over point for point — the bet the bell was making. The honest template
learned with it, on the 274 heads not learned from:

| | measured | honest, bell-shaped | honest, peak-shaped |
|---|---|---|---|
| a direction's deepest notch, median | 8.5 dB | 5.6 dB | 5.9 dB |
| its width at half depth | 0.60 ERB | 0.90 ERB | **0.60 ERB** |
| directions with a notch deeper than 6 dB | 70 % | 37 % | 48 % |
| the heads' dips met by a template notch | — | 42 % | 44 % |
| template notches with a dip under them | — | 59 % | 60 % |

In front: 7.3, 9.6 and 14.2 kHz, 9.2–9.6 dB deep, widths 0.59, 0.33 and 0.26 ERB, present in 66, 64 and 23 % of the
heads. The widths are now the measured ones; the depth is still the population's typical depth, evened over the
sphere — a head's own deepest places stay deeper (16 dB where the template plays 6, at one place of one head). The
listening page holds all three templates on a switch; the peak-shaped one is its start, at depth and sharpness × 1.

### 7.8 The model, as it stands ([0168](../DECISIONS.md))

A head is synthesised from the population's parts and two numbers. All of it is in `sheets/model/`, read by
`sheets/listen.html`, which is the reference for the port.

| part | source | numbers | the slider |
|---|---|---|---|
| timbre — the CLL's mean over the sphere | the KU100's (`generic.json`: `timbreKU100`) | NF | — |
| smooth pattern of the CLL, orders 0–4 | the population's mean (`generic.json`: `pattern`, `average`; the average is replaced by the timbre) | 25 × NF | — |
| notches: three surfaces, centre, depth, width, presence at order 6, symmetric | `template.json` (shape `peak`, learned honestly, §7.7) | 3 × 4 × 49 | **ear**: one shift of all centres, ERB; depth × 2.5 |
| ILD, order 10, not blurred | the population's mean (`ild`), and its change a unit of ITD scale below 1.5 kHz (`ildWithSize`) | 121 × NF, twice | **head**: `ild + (size − 1) · ildWithSize` |
| ITD, order 8 | the population's mean (`itd`), µs | 81 | **head**: `itd · size` |

To play a direction: evaluate the patterns there; CLL = timbre + pattern (orders 1–4) + the template's dips (centre
shifted, depth × 2.5, where presence ≥ 0.4, as 1/(1+u²) in dB); the two ears from CLL and ILD; minimum phase; the
pair delayed to the ITD. The sliders' ranges in the page: the ear −2…2 ERB (a head's fitted shift spreads by 0.58
ERB), the head 0.75…1.3 (a head's fitted scale spreads by 0.087).

What is not in the model: a head's own smooth pattern, ILD detail and the shape of its notch paths over direction —
the half of the way from the KU100 to the head's own notches that no slider reaches (§7.5).

**No grid.** Every part is a pattern over the sphere — spherical harmonics of order 4, 6, 8 or 10 — and the dips are
curves with a centre, a depth and a width; a direction is evaluated, not looked up, and the measured grids are gone
from the model. What is limited is not the resolution but the detail: an order resolves about 180°/order over the
sphere, and the template's presence gate (0.4) is a hard edge a notch appears at when a source moves — a fade along
the presence would be smoother. Along frequency the patterns are sampled at 0.1 ERB; the dips are continuous.

**Without SONICOM** (`WITHOUT=SONICOM` for `generic.js` and `honest.js`; `model/generic-hutubs-3d3a.json`,
`model/template-hutubs-3d3a.json`), for the licence question of §1: nobody's head moves by 0.42 dB RMS in the smooth
pattern, 0.58 dB in the ILD and 7.8 µs in the ITD — a third of how far one head is from another. The template's
centres move by 0.11–0.15 ERB and its depths by 0.4–0.8 dB; only the third notch's presence changes (45 % against 23 %
in front). Scored on the 280 SONICOM heads it never saw, the template finds 42 % of their dips and is right in 53 % —
as the all-databases template does on its own held-out heads (44 %, 60 %). **The model does not hang on SONICOM.**

**The databases sit at different notch frequencies, and that offset is out too.** Fitted to one template, a
database's heads sit at a median shift of their own: against the mean of the three databases, HUTUBS +0.25 ERB, 3D3A
−0.75, SONICOM +0.50 — the same kind of offset [0166](../DECISIONS.md) took out of the other parts. Not in depth or
width: a database's heads are 0.94–1.07 times as deep as the template and 0.99–1.01 times as wide. `honest.js` now
takes the medians per database each turn (shift, depth factor, width factor, against the mean of the databases'),
recentres the template so that its zero is the mean of the databases, normalises depths and widths by the factors,
and stores a head's shift with its database's offset out (`offsets` in the template's file). The template's shape
hardly moves — each head was aligned on its own already — but its zero is no longer the biggest database's, and the
spread of the heads' shifts is the people's (± 0.54–0.81 ERB a database). Learned from HUTUBS and 3D3A alone the
offsets are ± 0.49, and the template's notches sit 0.3–0.4 kHz lower in front (7.0, 9.2 and 13.7 kHz).

**A notch fades in with its presence** (0.25 → 0.55, a smoothstep; `fade` in the template's file) instead of
appearing at a threshold of 0.4: nothing switches when a source moves. The template then plays 2.2 notches a
direction (1.7 before), finds 50 % of the heads' dips (44 %) and is right under 53 % of its notches (60 %) — shallow
notches where a third of the heads have one.

**The listening page, lean**: five versions — the listener measured, the listener from its own numbers, the KU100,
nobody's head without notches, nobody's head with the two sliders — the two sliders and the notches' depth, a switch
between the model learned from all three databases and from HUTUBS and 3D3A, the fitted settings on a button. The
timbre is the KU100's throughout; the sharpness, the ILD's blur and order, the ITD's order, the surfaces' preset and
the older templates are gone from it, decided ([0167](../DECISIONS.md), [0168](../DECISIONS.md)).

## 8. Next

**Debt**: `search.js` ran into the edge of its ranges — the neighbours' reach at 18° and the distance to join a surface
at 1–1.2 ERB are the largest values it was offered. It is run again with wider ranges before the settings are final.


1. **The unified data set** — *built, and lost*: `build-dataset.py` wrote it to a temporary folder, with the databases,
   and the folder was cleaned. The databases are downloaded again (2026-10-04, from sofacoustics.org: HUTUBS 96,
   3D3A 38, SONICOM 140 heads, and the KU100) into `~/Documents/HRTFs/`, which stays; the set itself is not rebuilt
   yet (it stays outside the repository: it holds SONICOM data). 274 heads: CLL and ILD at order 20 per bin, ITD at order 10, each equalised and
   normalised; the KU100 beside them in the same representation, unnormalised; the averages the normalisation used, so
   it can be undone. Every database's average sits on the common one; each one's spread between heads is unchanged.
   The ITD varies between heads by 24–37 µs (mean over directions); the databases' averages had differed from the
   common one by 11–20 µs.
2. **The model** — what parameterises a head along the horizontal (ILD, ITD) and the vertical (CLL) axis, with shifts
   in frequency handled; not PCA. If §7's test holds: the notches tracked per ear over all heads, and how few numbers
   tell one head's from another's — centres being numbers, heads can be compared without the blur of §5. A model that
   has to ignore the database could take the non-linear remainder out in its own training, where a fixed
   normalisation cannot.
3. **Licences** before any shipped model: SONICOM's is unclear (§1).
