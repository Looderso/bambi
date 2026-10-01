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

Measured (`equalise.py`) on the two databases that may ship, HUTUBS (96 heads) and 3D3A (38): each head resampled to
48 kHz, windowed to 256 samples from its onset, equalised onto the KU100's CTF as the thesis did — the CTF a head's
magnitude averaged over directions, here weighted by area since the grids are not uniform — and unified by the
deviation-from-average fit (3D3A's gaps filled from the HUTUBS average). Compared on the directions both measured
(−57…75°), 2–16 kHz:

| | the two databases' averages apart (CLL / ILD) | a head from its own database's average | a classifier tells the database |
|---|---|---|---|
| raw | 12.37 / 2.73 dB | 4.4 / 5.4–5.9 dB | 99 % |
| equalised | **2.35** / 2.69 dB | 3.4 / 4.8–4.9 dB | **98 %** |

- **Equalisation removes the level and colour a database carries**: the averages' CLL comes 12.4 → 2.4 dB apart,
  less than heads differ within one database.
- **The fingerprint survives it**: a classifier still names the database 98 % of the time — as the study the thesis
  cites found (> 95 %). Removing it is the next step; it is what PCA or a per-direction average normalisation is for.
- **The CTF is taken per ear**, so a fixed left/right difference of the measurement chain leaves the ILD too (the
  thesis held the ILD unchanged). The difference averages about 2 dB but passes 6 dB at some frequency for 56 of
  the 96 HUTUBS heads and 14 of the 38 3D3A heads.
- **Part of the ILD gap is distance, not fingerprint.** To the side, below 1.5 kHz, 3D3A's ILD is 7.5 dB against
  HUTUBS's 5.7: 3D3A measured at 0.76 m, HUTUBS at 1.47 m, and a nearer source makes a larger ILD. At 2–16 kHz the
  two agree within 1 dB. A correction for distance would take that part out before the fingerprint is judged.

## 4. Removing the database fingerprint

Measured (`normalise.py`): each direction normalised — a head minus its database's average, plus the average of the
databases, each database weighted equally. In the cross-validation a test head is normalised with the average of
its database's training heads only. 2–16 kHz, on the directions both measured:

| | the classifier names the database — linear | — kernel SVM | heads around their own database's average (CLL / ILD) | all heads around the common average |
|---|---|---|---|---|
| equalised | 98 % | 97 % | 3.39–3.46 / 4.80–4.93 dB | 3.61 / 5.07 dB |
| normalised | **50 %** | **55 %** | 3.39–3.46 / 4.80–4.93 dB | 3.41 / 4.89 dB |

- **The fingerprint lived in the average.** Once each database's average is moved to the common one, neither a
  linear classifier nor a kernel SVM — the kind the literature used to find the fingerprint — tells the databases
  apart better than chance (50 %; 55 % is within the noise of 134 heads).
- **The differences between heads are untouched**: a shift leaves each head's distance from its own database's
  average as it was (3.39–3.46 dB CLL, 4.80–4.93 dB ILD). What the pooled spread loses, 3.61 → 3.41 dB (CLL), is the
  offset between the databases.
- **The two databases spread alike**: 3D3A's heads vary 1.02× as widely as HUTUBS's in CLL and 0.98× in ILD (median
  over directions, 2–16 kHz) — no second-order fingerprint to remove, so no PCA step is needed for these two.
- **The caveat of two databases**: normalising the average also removes any real difference between the two groups
  of people, and the measurement distance's effect on the low-frequency ILD (§3) with it. A third database is what
  would separate a fingerprint from a population.

## 5. Next

1. **The unified data set**: every HUTUBS and 3D3A head, equalised, normalised and on one SH representation — 134
   heads, the input to the model.
2. **The model** — what parameterises a head along the horizontal (ILD, ITD) and the vertical (CLL) axis, with shifts
   in frequency handled; not PCA.
