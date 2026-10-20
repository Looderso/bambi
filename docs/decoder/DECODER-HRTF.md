# Decoder: HRTF databases and the head model

Which measured HRTF databases the suite may ship, and the head model the decoder ships. Part of
[Decoder](DECODER.md); how the model is used (sliders, filters, other heads) is in
[DECODER-BINAURAL](DECODER-BINAURAL.md).

## Databases and their licences

A model that ships with the plugin may only be trained on data whose licence allows it. **The licence written inside
a database's files is the one that counts**; it disagrees with the databases' web pages more often than not.
Analysing a database to make data comparable is a different question from shipping a model trained on it.

| database | heads | directions | elevation | distance | rate | licence in the files | ships? |
|---|---|---|---|---|---|---|---|
| **HUTUBS** | 96 | 440 | −90…90°, whole sphere | 1.47 m | 44.1 kHz | CC BY 4.0 | **yes** |
| **3D3A** | 38 | 648 | −57…75° | 0.76 m | 96 kHz | CC BY 4.0 | **yes** |
| **SADIE II KU100** (D1) | 1 | 8802 | whole sphere | 1.2 m | 48 kHz | Apache 2.0 | **yes**, the base |
| SONICOM (= AXD) | ~300 | 828 | −45…90° | 1.5 m | 48 kHz | CC BY-SA 3.0 in the mirror's files; MIT in the 2025 paper; "all rights reserved" on its own site | ask |
| ARI | >250 | 1550 | −30…80° | 1.2 m | 48 kHz | CC BY-SA 3.0, not compatible with GPLv3 (4.0 would be) | ask |
| SADIE II humans | 18 | 2818 | whole sphere | 1.2 m | 48 kHz | University of York non-commercial licence | ask |
| LISTEN | 50 | 187 | −45…90° | 2.06 m | 44.1 kHz | IRCAM, all rights reserved | ask |
| ITA Aachen | 48 | 2304 | 5° × 5° | 1.5 m | n/a | CC BY or CC BY-NC-SA, sources disagree | ask |
| RIEC | 105 | 865 | −30…90° | n/a | n/a | non-profit academic only | no |
| BiLi | 56 | 1680 | −51…86° | n/a | n/a | IRCAM, all rights reserved | no |

The shipped model is learned from HUTUBS and 3D3A alone; SONICOM, the largest, stays outside it and serves only as
a held-out check. Dropping it moves the model by a third of how far one head is from another and leaves the notch
template's quality as it was.

## The shipped head model

One file, `core/data/decode/head.json` (about 0.36 MB), read by `parseHeadModel`
(`core/include/bambi/decode/headmodelfile.hpp`; the core never opens a file). `core/data/decode/ku100.json` is the
other bundled head, the KU100 as measured, in the same format without a notch layer.

**What the model is for:** a quick gain over the KU100 from two sliders, not the reconstruction of a measured head.
Heads are taken as symmetric (one ear over the sphere, mirrored). A head is synthesised from the population's parts
and the two slider values; every part is a pattern over the sphere, so a direction is evaluated, not looked up, and
no measured grid is in the file. An order resolves about 180° / order over the sphere; along frequency the tables
are sampled at 0.1 ERB.

| part | in the file | slider |
|---|---|---|
| timbre: the summed level's mean over the sphere | the KU100's, folded into `smooth`'s order-0 row | none |
| smooth pattern of the summed level (CLL = 10·log₁₀(P_L + P_R)) | `smooth`, order 4, the population's mean | none |
| ILD (left minus right, dB) | `ild`, order 10, not blurred, and `ild.withSize`, its change per unit of head size, learned below 1.5 kHz | **head**: `ild + (size − 1) · withSize` |
| ITD (µs, left arrival minus right) | `itd`, order 8 | **head**: `itd · size` |
| notches: three, each a centre, depth, width and presence map | `notches`, order 6, shape `peak`, symmetric | **ears**: one shift of all centres in ERB; depths × 1.5 |

Slider ranges: head 0.75 … 1.3 (a head's fitted scale spreads by 0.087), ears −2 … +2 ERB (a head's fitted shift
spreads by 0.58 ERB).

**To play a direction**: evaluate the patterns there; CLL = timbre + smooth pattern (orders 1–4) + the three
notches (centre shifted, depth × 1.5, faded in with their presence as 1/(1+u²) in dB); the two ears from CLL and
ILD; minimum phase; the pair delayed to the ITD.

**Notches fade in with their presence** (0.25 → 0.55, a smoothstep, `notches.fade`), so nothing switches when a
source moves. The template plays 2.2 notches a direction, at about 7.0, 9.2 and 13.7 kHz in front. A shifted centre
stays inside `notches.band`; widths stay inside `notches.widthRange` (0.1 … 1.5 ERB); depths floor at −60 dB.

### File format (version 1)

JSON with `version`, `learnedFrom` (the databases, for an about box), `convention` (real spherical harmonics, SN3D,
ACN; x front, y left, z up; dB; the axis in ERB numbers, f = (10^(e/21.4) − 1) / 0.00437), `axis`
(`erb0`, `step`, `points`: 7.7853, 0.1, 339), then `smooth`, `ild` (with `withSize`), `itd` and `notches`, each with
its `order` and `coef`. A table holds (order+1)² rows of `axis.points` values; a notch map holds (order+1)²
coefficients. `HeadModel::consistent` checks that the tables fit together.

### The synthesiser

`Head::prepare` evaluates every map once at a fixed set of directions and frequencies (the SH basis per direction,
the tables resampled onto the decoder's FFT bins); `Head::synthesise(earShiftErb, headSize)` then produces both ears
and the ITD from those tables alone, with no allocation, no spherical-harmonic evaluation and no minimum phase.
Measured, Release, one core: `prepare` 9.4 ms; `synthesise` 1.28 ms for 240 directions × 513 bins. A slider move is
rebuilt on a worker, never on the audio thread. The core reproduces the reference implementation's ears to 2 mdB
and its ITD to 2 ns on 7 directions × 3 slider settings (`core/tests/decode/fixtures.json`).

### What is not in the model

A head's own smooth pattern, ILD detail and the shape of its notch paths over direction: the half of the way from
the KU100 to a head's own notches that no slider reaches. The ITD's shape (how it rises from the front to the side,
a front/back asymmetry) has no slider; the head slider scales it only.

## How the model is learned

Databases differ in measurement, not in people, so they are made comparable first:

1. **Grids are unified** by spherical-harmonic fits at order 20 with a roughness penalty λ·n(n+1) (without it a
   fit explodes where a grid has no points); a gap is filled with the deviation from a population average, never
   from one other head, and never from the KU100 alone.
2. **Each head is equalised** onto the KU100's common transfer function (per ear), which brings the databases'
   average levels from up to 17.6 dB apart to 2.1–2.5 dB, less than heads differ within one database.
3. **Each database's average is moved onto the common one** (each database weighted equally; spreads between heads
   unchanged), which removes the database's linear fingerprint: a linear classifier then names the database at
   chance. Two random groups of heads inside one database differ by under 1.1 dB, a third of the gap between
   databases, so what is removed is measurement.
4. **The notch template is fitted honestly**: each head is aligned to it by its own shift, depth and width, the
   databases' median offsets are taken out, and the template is scored on held-out heads.

The data-preparation scripts and the model's browser reference live in `prototypes/decoder/`; the unified data set
holds SONICOM data and stays outside the repository.

Background: decisions 0165, 0169.
