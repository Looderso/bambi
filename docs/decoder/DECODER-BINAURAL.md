# Decoder: binaural, heads and the pair

How the headphone output is made, which heads it can use, what the two personalisation sliders do, and the pair's
fold and slice. Part of [Decoder](DECODER.md); the head model's data and file are in
[DECODER-HRTF](DECODER-HRTF.md).

## Method

**MagLS** with the diffuse-field covariance constraint, the current standard for orders up to 3 and above, keeps the
high-frequency timbre that a least-squares decode at low order loses. Measured on the KU100 (magnitude error over
8802 directions, area-weighted):

| order | method | 1–4 kHz | 4–8 kHz | 8–16 kHz | diffuse field, worst |
|---|---|---|---|---|---|
| 1 | LS | 9.4 dB | 16.0 | 21.8 | 32.8 dB |
| 1 | MagLS | 3.4 | 6.4 | 7.6 | 0.0 |
| 3 | LS | 4.8 | 12.5 | 16.8 | 22.6 |
| 3 | MagLS | 1.8 | 3.1 | 5.4 | 0.0 |

The set's common delay is removed before the design: magnitude LS carries each bin's phase from the one below and
drops a bulk delay above the cutoff, which put the highs ahead of the lows and leaned a front source 3.7 dB right.

**No room.** The decoder adds nothing to the field; a room is Reverb's job, chained before it. With AirPods,
Apple's own spatial audio must be off for the host's output, or two renderers run in series.

### The filter builder

`BinauralFilterBuilder` (`core/include/bambi/decode/binaural.hpp`) designs the filters from the head on a
Gauss-Legendre × azimuth grid of 240 directions, which integrates products of spherical harmonics exactly to order
9: complex least squares with a pure interaural delay as the phase up to the cutoff order · c / (2π · 8.75 cm),
magnitudes with the carried phase above it, the covariance constraint, 64 samples of bulk delay and 256 taps with a
32-tap fade. The bulk delay keeps the part of a response ahead of the head's centre causal and stays in the sound.
Checked tap for tap against the same design in numpy. Measured against the synthesised head at size 1 (Release,
one core):

| order | 1–4 kHz | 8–16 kHz | diffuse field, worst | a slider move rebuilds in |
|---|---|---|---|---|
| 1 | 3.4 dB | 7.4 | 0.0 | 1.9 ms |
| 3 | 1.7 | 5.1 | 0.0 | 3.2 |
| 5 | 2.2 | 3.4 | 0.0 | 5.1 |
| 7 | 1.8 | 2.8 | 0.0 | 7.8 |

The deeper a notch, the less of it a low order holds: the table is for notches at depth × 1.5; at × 2.5 the
8–16 kHz column is 9.3, 6.5, 5.1 and 3.9 dB.

### The convolution and the swap

The decode is 2 × (N+1)² filters of 256 taps, uniformly partitioned in the frequency domain in 64-sample partitions
(`convolver.hpp`). The plugin reports one partition, 64 samples, as latency. Measured per 128-sample block at
48 kHz, one core, Release: 0.1 % at order 1, 0.3 % at order 3, 0.6 % at order 5, 1.1 % at order 7.

A finished set is cut into partitions and transformed on a worker, handed to the audio thread whole through the
link bus's triple buffer, and crossfaded in over one partition. `BinauralDecoder` (`decoder.hpp`) carries the
sliders from any thread to one worker and the filters to the audio thread, and builds before the first block so a
bounce hears the parameters. Checked against a direct convolution however the audio is cut, and the fade against
two convolvers playing each set alone.

### Headphone EQ

There is none: the decoder renders for the head it has and applies no correction for the headphones.

## Heads

The head segment of the headphones' page chooses between three, on the same chain:

| head | what it is |
|---|---|
| **model** (default) | nobody's head, with the two sliders; learned from HUTUBS and 3D3A. Placed a sound better than the KU100 in listening |
| **ku100** | the Neumann KU100 as measured (SADIE II set D1, Apache 2.0), the reference the model's timbre is held to. The head slider scales only its time difference and the ear slider rests |
| **sofa file** | any SOFA (AES69) file, fitted at load |

**A measured head becomes a head file by one route**, `fitHeadModel` (`core/include/bambi/decode/measuredhead.hpp`):
the two ears' summed level and their difference as spherical-harmonic tables over the ERB axis, fitted with a
roughness penalty, the time difference taken from the low-passed cross-correlation; no notch layer and no size
morph. `tools/head` (`bambi-head`) runs it on a measured set and writes the file. Measured on the KU100 against its
own 8802 directions: order 20 holds the summed level to 0.41 dB and the difference to 1.7 dB RMS over 2–16 kHz, and
the time difference to 3.3 µs; a 0.2-ERB axis costs 0.02 dB against 0.1 for half the file (1.1 MB).

**Loading a SOFA file** runs the same route at run time. The file is read through libmysofa (vendored, reader only,
with the inflate half of zlib; see [THIRD-PARTY.md](../THIRD-PARTY.md)), fitted with the KU100's settings, and the
result goes into the state as a head file's text (about 1 MB, held by reference so undo steps do not copy it): a
session opens and bounces without the file, and a bounce elsewhere hears the same head. The path is kept for the
page only; the caption says the set and how close the fit came. A measured head of 793 directions at 512 taps loads
in about a second and fits within 0.39 dB of its summed level, 2.2 dB of its difference and 15 µs of its time
difference. A file that breaks AES69 (two receivers, the left first, cartesian and near the head) is refused with
the reason, never read wrong.

The chain takes another head between builds (`setModel`), never a re-prepare while audio runs.

## Personalisation

**The aim: a quick gain over the KU100, to make 3D audio more accessible.** One slider for head and torso and one
for the ears, on symmetric heads (one ear over the sphere, the other its mirror). Not a replacement for a
measurement in a loudspeaker array or a 3D scan, and not the reconstruction of a measured head. No SOFA file is
needed.

**The direction-dependent cues are personalised; the timbre stays the KU100's.** A head's mean over all directions
is held at the KU100's. This follows a perceptual study of HRTF individualisation, in which the chosen set beat the
KU100 on externalisation (most at the rear and overhead) and lost on timbre; the split buys the space and keeps the
colour.

| slider | range | moves | how |
|---|---|---|---|
| **head** | 0.75 … 1.3 | the horizontal cues | scales the ITD; the ILD below 1.5 kHz morphs with it: `ild + (size − 1) · ildWithSize` |
| **ears** | −2 … +2 ERB | the vertical cues | shifts the centres of the model's notches along frequency (a bigger or smaller pinna moves its notches) |

Each slider moves its own part. Measured on the rendered filters at 60° left: ear size ±10 % moves the summed level
by up to 14 dB and the ILD by 1.6 dB at most; head size ±10 % moves the ITD by ±70 µs and the ILD by up to 16 dB, the
summed level by 0.05 dB at most. The ear slider moves the front's main notch from 7.7 kHz to 6.9 kHz at 110 % and
8.4 kHz at 90 %. A dip near 9.9 kHz stays at every ear size, since part of it is in the timbre, which the ear slider
leaves alone.

**How the KU100 renders for it.** The summed level (CLL = 10·log₁₀(P_L + P_R), where the pinna's elevation cues
are) and the ILD are spherical harmonics per frequency bin, turned into the two ears and made minimum-phase for
whatever direction the head is at; the measured ITD is added on top, less what the minimum-phase pair already
carries (up to 193 µs, measured on every update with the same estimator the ITD was measured with). At the KU100's
own size the chain comes within 0.60 dB (median) of the measured magnitudes and 3 µs of the measured ITD over 300
directions.

**Portability.** The session stores the two sliders, the layout's contents (not a path), the head file's text and
the tracker's setup.

## The pair: fold and slice

A layout of two loudspeakers is a pair of virtual microphones, and one switch on the speakers page chooses between
two designs:

- **Fold**: the microphones a quarter turn to each side, wide, the whole field on the pair as a stereo listener
  hears the mix.
- **Slice**: the microphones at the loudspeakers, as narrow as the field's order allows (the cardioid to that power,
  without a rear lobe of the other sign), and the field turned by the head's orientation as the headphones do, so
  whatever the head points at lands between the loudspeakers and the rest is quiet.

The pair is for checking a dense scene: it is the view that owes nothing to a head model, so a spatial problem in
the mix can be told from one in the HRTF.

Background: decisions 0165, 0169.
