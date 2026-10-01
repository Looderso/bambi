# HRTF data — making the databases comparable

The decoder's personalisation (DECODER §5) is to modify the KU100 with a model learned from many measured heads.
This page is the data side: which databases, how their grids are unified, and what was measured on the way. The
model itself comes after, and is not PCA — PCA is not shift-invariant, and pinna notches move in frequency from
head to head; PCA may still serve to remove database-specific patterns while keeping the binaural cues.

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

`export-model.py` turns it into `prototypes/decoder/match/model.js` (outside version control: SONICOM data), which the
match prototype plays as "Model" cues on a pad — pattern across, shift up and down.

## 6. Next


1. **The unified data set** — *built* (`build-dataset.py`, kept outside the repository: it holds SONICOM data). 274
   heads: CLL and ILD at order 20 per bin, ITD at order 10, each equalised and normalised; the KU100 beside them in
   the same representation, unnormalised; the averages the normalisation used, so it can be undone. Every database's
   average sits on the common one; each one's spread between heads is unchanged. The ITD varies between heads by
   24–37 µs (mean over directions); the databases' averages had differed from the common one by 11–20 µs.
2. **The model** — what parameterises a head along the horizontal (ILD, ITD) and the vertical (CLL) axis, with shifts
   in frequency handled; not PCA. A model that has to ignore the database could take the non-linear remainder out
   in its own training, where a fixed normalisation cannot.
3. **Licences** before any shipped model: SONICOM's is unclear (§1).
