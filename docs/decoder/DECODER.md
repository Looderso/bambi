# Decoder — feature set

The last plugin in the chain: ambisonics in, headphones or loudspeakers out. It holds what was the Lens
monitoring tool as one of its outputs. Input to prototyping; describes *what the plugin does*, not how. Opinions
are marked; open questions are listed rather than guessed. Nothing here is built; the first prototype is
`prototypes/decoder/listen/` (§10).

---

## 1. What it is for

**Compose in 3D at home, move to an array without changing the mix.** The ambisonic mix is the work; the decoder
is how it is heard. Headphones and a stereo pair are what nearly everyone has, an array is what some have
sometimes — the decoder serves all three from the same scene, and the step from one to another is a change of
output, not of session.

It is **not an effect**. It sits once, on the master, and it is the one plugin whose output is not ambisonics.
It draws no "what it added"; it draws what comes out where.

## 2. Outputs

One ambisonic input (order 1–3, AmbiX), three things it can produce:

| output | for | engine |
|---|---|---|
| **Headphones** | listening to the scene as a listener | binaural decode, head-tracked (§4, §5) |
| **Speakers** | a stereo pair, a 5.1/7.1.4 room, a dome | AllRAD decode of a loaded layout (§3) |
| **Lens** | inspecting one slice of the scene on a stereo pair | the field turned by the head, then a stereo layout's decode (§6) |

**Every output is a set of channels on the bus the host gives** ([0178](../DECISIONS.md)). The bus is as wide
as the host makes it — a REAPER track's channel count, say 18 — and is never renegotiated: hosts fix a plugin's
channel count when it is inserted. Headphones play on a pair, 1–2 by default and movable; a layout on its own
channel numbers, from its file. **The Lens plays on the loudspeakers** — they are all there is to play it on — on the
layout's own channels, with the tracker turning what they play; so the Lens and the speakers play one at a time.
Switching an output changes which channels the plugin writes.

- **Two at once whenever their channels do not collide.** Headphones on 13–14 and a 7.1.4 layout on 1–12 both
  play; headphones on 1–2 beside an array from 1 do not, and lighting one stops the other, saying so. Getting 13–14
  to a headphone amplifier is the interface's routing (RME's TotalMix) or a virtual device's, not the plugin's.
- **An output that needs more channels than the bus has does not play**, and says how many it needs. A host that
  gives two channels runs headphones, a stereo pair and the Lens.
- **Debt: a compact spherical array that points outwards** — sonible's IKO: beams reflected off the room's walls,
  fed, as far as known, with ambisonics its own processing turns into beams. Not a layout AllRAD can decode; an
  output of its own, if it comes.

**A trap, found on the first listen:** REAPER's master track has two channels, so a decoder inserted there reads a
two-channel field — order 0, W alone, heard as mono from everywhere — whatever the tracks feeding it carry. The
decoder sits on the 16-channel track with the rest of the chain, and the head strip says in words when the input
holds no direction.

## 3. Speakers — a layout, not a preset list

- **AllRAD** over a loaded layout, with **imaginary loudspeakers** filling the gaps of an incomplete layout (no
  lower half, no ceiling).
- **The weighting is the listening situation** ([0174](../DECISIONS.md)): *one listener*, the default, decodes
  unweighted (basic); *audience* decodes max-rE, which widens the sweet area for listeners spread across a room
  and costs the centred one sharpness, decorrelation and treble. **Both are energy-preserving**: a source panned
  anywhere gives, averaged over all directions, the energy of one loudspeaker playing it — so switching keeps
  the level by construction, and so does changing layout.
- *Later:* **a colour correction** per layout, order and weighting — one high shelf and one peak, after Frank et
  al. (DAGA 2023) — measured on the KU100 through §3.1.
- **Per-speaker delay and gain from its distance**, so a room that is not a perfect sphere still images.
- **A layout comes in every way it is commonly found**, and the decoder is always designed here, at the suite's
  order, from the positions alone:
  - **IEM JSON** — the AllRADecoder's configuration, and the same format SPARTA's plug-ins import and export, so
    one reader serves both suites. The array on hand comes as one.
  - **AmbDec `.ambdec`** — the format of the Ambisonic Decoder Toolbox and the Linux decoders.
  - **The ITU BS.2051 layouts**, built in: 0+2+0 (stereo), 0+5+0, 2+5+0, 4+5+0, 4+7+0 (7.1.4), 4+9+0 (9.1.6) and on.
  - **Typed in**, or pasted as a list of azimuth, elevation and distance.
  - **Placed by looking** (§3.3).
- **A loudspeaker can be imaginary**: it takes part in the triangulation and its signal is dropped — never mixed
  back into its neighbours, which cuts the effective order by about a third (Zotter, Riedel & Frank, DAGA 2022).
  A layout with nothing below the listener gets a point there, so a source below pans to the lowest ring instead
  of vanishing. **A ring with nothing above or below** is designed from virtual loudspeakers on the ring only.
- **The subwoofer channel** is skipped by the decoder; bass management is a question of its own.
- **A stereo pair** is the smallest layout — a virtual-microphone downmix, which doubles as the check that the mix
  survives stereo.
- **Crosstalk cancellation** (3D from two speakers) is **not a feature**: a sweet spot of a head's width and a
  treated room. It stays a research question until a prototype says otherwise.

**Ported** (`core/include/bambi/decode/allrad.hpp`, `layout.hpp`, `layoutfile.hpp`): `designAllrad` takes a layout,
an order and the weighting and returns the matrix for AmbiX in, one row per output channel, with each
loudspeaker's gain and delay from its distance. The loudspeakers are sorted by channel first, the hull is
convhull_3d's in double precision on the unit vectors (its tie-breaking noise is a function of the point index, so a
layout designs the same decoder however it is listed), 5200 directions on a Fibonacci sphere are panned by VBAP
over the hull, the decoder is that panning sampled against the harmonics with (2n+1) and the order weights, and the
level is set on a second grid of 4000. A point within 10° of the horizon counts as on it: a layout with every point
there is a ring and is designed on 360 virtual loudspeakers around it; otherwise a missing lower or upper half gets
its pole. Four or more loudspeakers on one plane — 7.1.4's height square — are panned around their centre, whose
gain they share, instead of along whichever diagonal the hull picked ([0179](../DECISIONS.md)). A layout whose hull
does not hold the listener is refused with the reason. Layouts come from the IEM reader and from the built-in
ITU-R BS.2051 table (`layouts.hpp`: the ten systems by name, channels in the recommendation's order, the LFE a
gap); AmbDec and typed-in layouts are still to come. Checked against the IEM plug-in's own matrix for the array
(0.02 %), against the prototype's order 1–3 decoders tap for tap, against §3.2's table, and for symmetry on 7.1.4.
Designing a 64-loudspeaker layout takes 2 ms wall at order 1 and 6 ms at order 7 in Release; it runs on a worker.

**The matrix, the delays and the swap** (`speakers.hpp`, [0177](../DECISIONS.md)): `SpeakerRenderer` applies the
matrix per frame with the distance gains folded in, delays each output by whole samples, and takes a new design
whole from the worker, fading over 10 ms once the new design's delay lines hold its signal; `SpeakerDecoder` carries
a layout request from the message thread to one worker and the design to the audio thread, and keeps the previous
design playing when a layout is refused. Measured per 128-sample block at 48 kHz, one core: 0.8 % at order 3 on the
array, 4.8 % at order 7 on 64 loudspeakers. Checked against the design applied directly however the audio is cut,
the fade against each design played alone, a delay change for steps, and the decoder bit for bit against a direct
publish. Still to come: the AmbDec, built-in and typed layouts, the stereo pair, the colour correction, and the
plugin around all of it.

### 3.1 Hear the array on headphones

**The layout's speakers, rendered through the HRTF as virtual sources.** At home, with the array's own layout
file, one hears what *that* array will do to the mix: its gaps, its order limit, its missing lower half. This is
the bridge between the two worlds. SPARTA's AmbiDEC also renders its loudspeakers binaurally; what is this
decoder's own is doing it head-tracked, from the same layout file the room uses, in the workflow around it. *Opinion: the feature that makes
this decoder worth choosing.*

The loudspeakers are heard as points: each is encoded at order 7 and decoded by MagLS at order 7, well above the
array's own order, so what is heard is the array's panning and not a second blur on top of it. The head turns
against the room, as it would standing in it.

**Ported** (`core/include/bambi/decode/virtualarray.hpp`): the array's decoder and the encoding of its loudspeakers
collapse into one matrix from the bus to an order-7 field, which a `SpeakerRenderer` plays and swaps as it would a
layout, and a `BinauralConvolver` at order 7 takes it from there. Every loudspeaker sits at one distance: the room's
distance gains and delays undo distances a virtual room does not have. Measured per 128-sample block at 48 kHz, one
core: 1.7 % at bus order 1, 1.9 % at order 3, for 7.1.4. The head's turn against the room goes on the order-7
field, between the array's decoder and the convolver (§7.2).

### 3.2 Measured on the array on hand

16 loudspeakers and one imaginary point below: seven at ear level (0°, ±30°, ±90°, ±145°), four at about 25°, four
at about 56°, one at the top; 1.30–1.65 m away.

- **The design matches the IEM plug-in's.** At the file's order 5, N3D, unweighted, the matrix designed here is
  within 0.02 % of the file's — the difference shrinks as the design grid grows (0.20 % at 800 points), so both
  converge on the same AllRAD.
- **Order 3 is this array's order.** 16 loudspeakers carry (3+1)² channels; the file's order 5 buys nothing.

| order | energy above −30° | energy below | rE above | direction error above | below |
|---|---|---|---|---|---|
| 1 | −0.8 … +0.9 dB | −1.5 … +0.6 | 0.64 | 7.1° | 52° |
| 2 | −0.9 … +1.7 | −5.6 … +1.0 | 0.80 | 3.8° | 49° |
| 3 | −1.0 … +2.0 | −9.7 … +1.1 | 0.86 | 3.9° | 49° |

Above −30° the array is even and sharp. Below it has nothing: a source there comes from the lowest ring, about 50°
off, and up to 10 dB quiet at order 3. Heard through the prototype, a source straight behind comes out 5 dB louder
on the array than in the reference — the two loudspeakers at ±145° reach the ears more than a source at 180° does.

### 3.3 Placing the loudspeakers by looking at them

**The plugin plays a burst on one channel; the listener looks at the loudspeaker that sounds and presses a key.**
One gesture gives both the loudspeaker's direction and which channel it is — and a wrong channel mapping is the most
common fault of a new array. It needs nothing but the head tracker the listener already wears (§7), and it is the
same "where am I looking" the Lens uses. *Opinion: a USP, and the strongest use of head tracking beyond the
binaural decode.*

- **The reference first.** A tracker's orientation is relative to where it started, and its yaw drifts. The
  calibration begins by looking at the front loudspeaker and recentring, and ends by looking at it again: the drift
  over the calibration is shown as a number, and a large one asks for a repeat.
- **From the listening position.** The directions are as seen from where the head is; standing off-centre puts a
  parallax error on every loudspeaker.
- **Accuracy is measured, not assumed.** People aim with their eyes more than their head; head pointing may scatter
  by 5–10°, and more in elevation. AllRAD tolerates a few degrees. The scatter is found by repeating the
  calibration, and a loudspeaker whose repeats disagree is flagged.
- **Distance is not seen.** It is typed in, or measured: with a measurement microphone at the listening position the
  same burst gives each loudspeaker's delay and level, which is the distance compensation of §3 without a tape.
- **The result is a layout file**, the same as an imported one, and editable afterwards.

## 4. Binaural

- **MagLS**, with the diffuse-field covariance constraint — the current standard for orders ≤ 3, which keeps the
  high-frequency timbre that a least-squares decode at low order loses.
- **Headphone EQ** as an optional loaded curve; off by default.
- *Nice to have:* **a list of headphones to choose from**, each with its correction, from the many published
  measurements (AutoEq gathers thousands). AirPods suit it unusually well: their adaptive EQ corrects the fit from
  an inward microphone, so the response at the eardrum barely changes from wear to wear. Open before it is built:
  the licence of each source's measurements (AutoEq's code is MIT, the measurements are their makers'); the target
  — published corrections aim at a preference curve such as Harman's, a binaural render wants the response its
  head was measured against, diffuse field for the KU100 — and how it sits with the KU100's timbre
  ([0167](../DECISIONS.md)). With AirPods, Apple's own spatial audio must be off for the host's output, or two
  renderers run one after the other.
- **No room.** A little room helps a dry binaural render out of the head, but a room is Reverb's job, chained
  before the decoder when wanted. The decoder adds nothing to the field.
- **Latency**: 64 samples (1.3 ms) of bulk delay inside the filters, which keeps the part of a response ahead of
  the head's centre causal and stays in the sound; plus one partition of the convolution, 64 samples, which the
  plugin reports to the host ([0173](../DECISIONS.md)). The convolution is 2 × (N+1)² filters of 256 taps,
  uniformly partitioned in the frequency domain (`core/include/bambi/decode/convolver.hpp`); measured per
  128-sample block at 48 kHz, one core: 0.1 % at order 1, 0.3 % at order 3, 0.6 % at order 5, 1.1 % at order 7.

**Measured on the KU100** (the prototype's `build-filters.py`, magnitude error over all 8802 directions, weighted
by area):

| order | method | 1–4 kHz | 4–8 kHz | 8–16 kHz | diffuse field, worst |
|---|---|---|---|---|---|
| 1 | LS | 9.4 dB | 16.0 | 21.8 | 32.8 dB |
| 1 | MagLS | 3.4 | 6.4 | 7.6 | 0.0 |
| 3 | LS | 4.8 | 12.5 | 16.8 | 22.6 |
| 3 | MagLS | 1.8 | 3.1 | 5.4 | 0.0 |

Plain least squares loses the top octaves at any order the suite uses; MagLS keeps them. **The set's common delay
must be removed before the design**: magnitude LS carries each bin's phase from the one below and so drops a bulk
delay above the cutoff — with the KU100's 108 samples left in, the highs came out ahead of the lows, wrapped round
the filter's end, and a front source leaned 3.7 dB to the right.

**Ported** (`core/include/bambi/decode/binaural.hpp`): `BinauralFilterBuilder` designs the filters from the
synthesised head ([§5.3](#53-stage-two--a-model-from-many-listeners)) on a Gauss-Legendre × azimuth grid of 240
directions, which integrates products of spherical harmonics exactly to order 9 — complex least squares with a
pure interaural delay as the phase up to the cutoff order · c / (2π · 8.75 cm), magnitudes with the carried phase
above it, the covariance constraint, 64 samples of bulk delay, 256 taps with a 32-tap fade
([0171](../DECISIONS.md)). It is checked tap for tap against the same design in numpy
(`prototypes/decoder/sheets/check-filters.py`). Measured against the synthesised head, nobody's head at size 1
(Release, one core; the fit's grid, weighted):

| order | 1–4 kHz | 8–16 kHz | diffuse field, worst | a slider move rebuilds in |
|---|---|---|---|---|
| 1 | 3.4 dB | 9.4 | 0.0 | 1.9 ms |
| 3 | 1.8 | 6.6 | 0.0 | 3.2 |
| 5 | 2.5 | 5.0 | 0.0 | 5.1 |
| 7 | 2.1 | 3.5 | 0.0 | 7.8 |

The model's notches are deeper than the KU100's, which is where the 8–16 kHz error above the KU100's comes from.

**The convolution and the swap** (`convolver.hpp`, `decoder.hpp`): a finished set is cut into partitions and
transformed on the worker, handed to the audio thread whole through the link bus's triple buffer in place, and
crossfaded in over one partition; `BinauralDecoder` carries the sliders from any thread to one worker and the
filters to the audio thread, and builds before the first block so a bounce hears the parameters
([0173](../DECISIONS.md)). Checked against a direct convolution however the audio is cut, and the fade against two
convolvers playing each set alone. What is not written yet is the plugin around it: the JUCE wrapper, its worker
thread, and the two sliders in the UI.

### 4.1 Which HRTF

**Default: the KU100.** It sounds good out of the box, and the thesis behind §5 found it the clear winner on
timbre (colouration rated 16 against 8 for an individualised set) — its moderate spectral features suit a broad
population. A listener adapts to a set that is not theirs; they do not adapt to one that sounds bad.

**Every bundled set is checked for its licence first** — several databases forbid commercial use, which would
also bind any model built from them (§5.3).

**Loading a SOFA file** is supported, for anyone with a measured set of their own.

## 5. Personalisation — perceptual, not anthropometric

**The aim ([0165](../DECISIONS.md)): a quick gain over the KU100, to make 3D audio more accessible** — in the best
case one slider for head and torso and one for the ears. Not a replacement for a measurement in a loudspeaker array
or a 3D scan, and not the reconstruction of a measured head. Heads are symmetric, as the KU100 is: one ear over the
sphere, the other its mirror. The timbre — a head's mean over all directions — is the KU100's
([0167](../DECISIONS.md)).

What a 2024 master's thesis on perceptual HRTF individualisation (KUG Graz) established, and what follows from it:

- Choosing by ear, in two phases — rough (up only), then fine (front, front-up, up, back-up, back) — took a
  median 17 minutes.
- The chosen set **beat the KU100 on externalisation**, most at the rear (15 against 9) and overhead (13 against
  8), and slightly on front-up localisation.
- It **lost on timbre**. Individualisation bought space and paid in colour.

So the design splits the two: **the direction-dependent cues are personalised, the timbre stays the KU100's.**
The adjustment moves how one direction differs from another; the average across directions is held on the
KU100's. *Caveat: the thesis evaluation disabled SPARTA's diffuse-field EQ because it hurt externalisation. That
was a flat EQ on the output, which this is not — but it is a warning, and the split is settled by ear.*

Personalisation is a **guided page**, rough then fine, as in the thesis — not controls always on screen. Its
result is a handful of numbers. **No SOFA file is needed**: the people this is for have none.

### 5.0 Against a real loudspeaker

**The stereo pair the listener already owns is the reference.** A burst from one real loudspeaker, then the same
burst from a virtual source placed where that loudspeaker is; the listener adjusts until the two come from the same
place.

- **Alternated, never at once.** The two paths arrive with different delays — Bluetooth headphones add 150–250 ms —
  so the comparison is a ping-pong with a gap, and latency does not matter.
- **Head tracking multiplies the directions.** The virtual source is world-fixed. Turning the head puts the real
  loudspeaker at another azimuth relative to the head; tilting it puts the loudspeaker above or below. One
  loudspeaker and a moving head sample many head-relative directions — elevation included, which is where
  personalisation matters most. If the HRTF fits, real and virtual stay together however the head moves.
- **The loudspeaker must be heard through the headphones**: open headphones — the thesis used acoustically
  transparent ones for this reason. **Not an earbud's transparency mode**: measured on AirPods Pro, it changes the
  level difference between the ears by up to 8 dB and removes the spectral cues above 5 kHz (Ausili et al., 2025);
  the loudspeaker heard through it has lost what the ear slider is matched by. No EQ restores them: they depend on
  direction and are made at the ear canal's entrance, which the earbud fills.
- **Where, not what.** The real loudspeaker brings the room's reflections and the virtual one is dry; the task is
  to match the position, not the character.
- The neck's range limits the directions reached; how far is measured in the prototype.
- **Parked — one axis renders, the other guards.** While one part is personalised, the other is held at 0 (CLL 0
  while turning for ITD and ILD; ILD and ITD 0 while nodding for the CLL). One axis of the tracker drives the
  rendering; the other is not rendered but watched, after centring on the real loudspeaker, with visual feedback
  when the head drifts in it.
- **The model behind the controls** is learned from many measured heads — the data side is
  [HRTF-DATA.md](HRTF-DATA.md).

### 5.1 Stage one — no database needed

The HRTF is split as the thesis split it: what the ears share, the **CLL** = 10·log₁₀(P_L + P_R), where the pinna's
elevation cues are; and how they differ, the **ILD** = 10·log₁₀(P_L / P_R) and the **ITD**. The two ears come back
from CLL and ILD exactly. Two parameters, each acting on its own part:

- **Head size** → scales the ITD and stretches the ILD in frequency: a larger head shadows from a lower frequency
  up. The horizontal cues.
- **Ear size** → stretches the CLL's direction-dependent part in frequency. Scaling a DTF in frequency is the known
  first-order model of how pinnae differ between people; it moves the elevation notches where a bigger or smaller
  ear would put them. The vertical cues. The CLL's order 0 — its average over all directions, the timbre — stays
  the KU100's.

*Opinion: try this first.* It needs no dataset, and it may already carry most of the gain.

**How the KU100 is rendered for it** (`prototypes/decoder/match/`): CLL and ILD as spherical harmonics per frequency
bin, turned into the two ears and made minimum-phase for whatever direction the head is at; the measured ITD added
on top, less what the minimum-phase pair already carries.

- **Order 20** holds the CLL to 0.41 dB RMS in the median plane at 8–16 kHz, the ILD to 1.0 dB (2.1 dB over the
  whole sphere, where the far ear's notches are — the ILD is held at ±40 dB).
- **The ITD is the KU100's own**, measured below 1.5 kHz and expanded to order 10 (3 µs RMS residual), so it keeps
  its elevation dependence. A spherical-head model fits it only with an effective radius of 11.9 cm and errs by up
  to 95 µs.
- **The minimum-phase pair carries up to 193 µs of ITD by itself**, and how much changes as head size reshapes the
  ILD — so the page measures it on every update, with the same estimator the KU100's ITD was measured with.
- **At the KU100's own size** the chain comes within 0.60 dB (median) of the measured magnitudes and 3 µs (median,
  10 µs worst) of the measured ITD, over 300 directions.
- **Each control moves its own part**, measured on the rendered filters at 60° left: ear size ±10 % moves the CLL
  by up to 14 dB and the ILD by 1.6 dB at most — the minimum-phase reconstruction, not the design; head size ±10 %
  moves the ITD by ±70 µs and the ILD by up to 16 dB, the CLL by 0.05 dB at most. Ear size moves the left ear's main
  notch in front, 7.7 kHz, to 6.9 kHz at 110 % and 8.4 kHz at 90 % (it rises to 10.1 kHz at 30° up). A dip at
  9.9 kHz stays where it is at every ear size: part of it is in the average over all directions, the timbre, which
  ear size leaves alone — the price of a fixed timbre, and one to listen for.

### 5.2 Averaging by spherical-harmonic order — to be discussed

The idea: average an HRTF set's direction information with spherical-harmonic orders as the weights, so that a set
has a directional average that sharpens as the order rises — and average the same way *across* sets, giving a
coarse-to-fine family between "what all heads share" and "this head". In place of the thesis's fixed directions
with a Gaussian weight around each, the region averaged over is what an order resolves; a blur of a given width
is a spherical cap, one weight per order, the same kernel the encoder's width uses.

What the renderer of §5.1 already establishes for it: the KU100's magnitude is held to 0.74 dB by order 20 in the
median plane, and its order-0 term is the average over all directions — the timbre. How the averaging becomes
something a listener chooses between is not settled; it is discussed before anything is built.

### 5.3 Stage two — a model from many listeners

**Built as a prototype, decided ([0168](../DECISIONS.md)):** a head is made from the population's parts and two
sliders — nobody's head (the mean of 548 symmetric heads in every part but the notches, with the KU100's timbre), an
ear slider that shifts a learned template of three notch surfaces along frequency, and a head slider that scales the
ITD with the low ILD going along. [HRTF-DATA §7.8](HRTF-DATA.md) is the recipe, `prototypes/decoder/sheets/model/`
the files, `sheets/listen.html` the reference. Not PCA, and not the thesis's pipeline: the dataset unification, the
split of a head into parts and the template were built here, and the licence of each source database decides what
may ship: the shipped model is learned from HUTUBS and 3D3A ([0169](../DECISIONS.md), [HRTF-DATA §1](HRTF-DATA.md)).

**Ported**: the model is one file, `core/data/decode/head.json`, and `core/include/bambi/decode/` synthesises both
ears and the ITD from it for any direction set and frequency grid — prepared once, then rebuilt per slider move on a
worker without allocating, checked against the page's own numbers — and designs the decode filters from it (§4).

### 5.4 Portability

**The session stores everything it can**: the personal numbers, the layout's contents (not a path to its file),
the decoder's settings, the OSC configuration. A session reopened on another machine renders as it did. A
per-user profile may *seed* a new instance; it never silently changes an existing one.

## 6. Lens — a stereo pair as a probe

**Headphones experience the scene; the Lens inspects it.** Binaural elevation is ambiguous for many listeners,
and a dense scene makes it hard to tell a spatial problem in the mix from one in the HRTF. The Lens plays one
slice of the sphere on the physical stereo pair: a second, physically grounded view of the same field,
independent of any HRTF.

**It plays on the loudspeakers, never on a pair of its own** ([0178](../DECISIONS.md)): it is a way of playing the
speakers, so it shares their channels and replaces them while it plays.

**The loudspeakers stay where they are; the head chooses the slice.** The field is turned by the head's
orientation before a stereo layout's decode, so whatever the head points at lands in front, between the two
loudspeakers. On headphones the same turn keeps the scene still; here it moves the scene past loudspeakers that
do not move — look up, and what is above plays on the pair.

**There is no Lens engine.** It is the rotation of §7 and a speaker decode (§3), as any layout is: what lies in a
loudspeaker's region plays on that loudspeaker. A stereo layout alone would fold everything behind and above onto
the pair; the rest of the sphere is filled with **imaginary loudspeakers**, whose signals are dropped, so only the
slice in front reaches it. How wide the slice is, is where that imaginary ring sits.

- **Steered by hand** when no tracker is present: the slice is dragged on the sphere view.
- It is **diagnostic, not a simulation** of the headphone render, and the UI never presents it as one.

## 7. Head tracking and OSC orientation

**The decoder receives orientation over OSC, and keeps it.** A head, a phone or any tracker turns the whole scene,
and the decoder is the one place at the end of the chain where that happens — nothing is forwarded to other plugins.

### 7.1 What it accepts

There is one standard and two formats that became standards by use; everything else is read through a learn mode.

| family | addresses | form |
|---|---|---|
| **ADM-OSC** — the industry standard, understood by L-ISA, Spat Revolution, Holophonix and others | `/adm/lis/ypr` | degrees; yaw right negative, pitch down negative — the suite's own convention, no remapping |
| **SPARTA** | `/ypr`, `/quaternion` | degrees, **pitch down-positive**; quaternion w, x, y, z |
| **IEM SceneRotator** | `/SceneRotator/ypr`, `/SceneRotator/quaternions` | the same two forms, the same pitch |
| **Learn** — anything else | whatever arrives | the plugin shows each incoming address; the user picks quaternion (w first or last) or yaw-pitch-roll, degrees or radians, and which axes flip. Saved as a preset |

Headitude (AirPods), the bridge apps of hardware trackers and most existing setups send one of the first three.
Phone apps — Sensors2OSC on Android (`/rotationvector`, w last), ZIG SIM, GyrOSC — go through learn.

**AirPods need no bridge app on a Mac.** The decoder carries a helper app in its bundle, `Contents/Helpers/`, that
reads the headphones' motion with CoreMotion and sends the quaternion to the receiver on localhost
([0170](../DECISIONS.md)):

- **Not in the plugin's process.** macOS asks the *host* for a motion usage text and terminates a host without one,
  which is every DAW. The helper carries its own.
- **Launched through Launch Services**, never as a child process, which would be charged to the host and crash it
  the same way. The permission dialog names the helper, once a machine.
- **Started from the decoder, ended with it**: on a button, quitting when the decoder goes or the host exits.
  Nothing in the Dock, nothing to install apart from the plugin.
- **The look-and-nod calibration of Headitude** (MIT) tells the decoder how the AirPods sit; recentring stays as
  below, yaw only.
- **macOS only.** Elsewhere AirPods are a phone's job, through the formats above.
- **Built** (`plugins/decoder/Helper/`, `Source/HelperLink.mm`): the helper is a target of the plugin build, nested
  into the VST3 and CLAP bundles after they are built and copied, both signed again; `airpods` in the head strip
  launches it with the receiver's port and ends it when the source changes; it sends `/quaternion` as SPARTA does,
  so the receiver needs no new form, and `/bambi/helper/status` with the permission's state, which the strip shows
  while it starts. **The AirPods report a head frame, not Apple's phone-like one**: worn upright, x is the head's
  right, y its front, z its up (`Mounting::AirPods`, the frame Headitude's `(x, y, z) → (y, −x, z)` remap stands
  on); the documented "y up, z toward the back" was tried first and read a level head as looking straight up,
  which put the recentred front at the zenith and turned a nod backwards. The frame is assumed until **the
  look-and-nod calibration** replaces it for a pair that sits askew: one button on the tracking tab, pressed while
  facing ahead and level, then pressed again while looking down — one button, because nobody sees where to click
  while nodding. The level pose yields the head's up in the sensor's frame, the nod its left axis, the front
  follows (`axesFromLookAndNod`), and the axes are state, kept with the session; *forget* returns to the frame
  above.

**Any phone, with no app: a page.** The helper also serves a page to the local Wi-Fi, reached by a QR code in the
decoder, that sends the phone's orientation to the receiver ([0172](../DECISIONS.md)):

- **HTTPS, on a certificate the helper makes for this computer's address**, because a browser gives motion data only
  to a secure page. The phone warns once that the connection is not private; past the warning the page counts as
  secure. A short tutorial covers the two taps (iPhone: *Show Details → visit this website*; Android: *Advanced →
  Proceed*).
- **A WebSocket on the same address carries the data**, with POSTs as a fallback for a browser that refuses it.
- **Worn or held**: on the headphones' band, screen up and top edge forward, or held upright with the screen facing
  the listener. Both map into the head's frame on the phone; recentring is yaw only, as for every tracker.
- **The relative orientation, not the compass**: the page reads `deviceorientation`, which on Android is the
  gyroscope's own frame; a compass is disturbed by the magnets in headphones and loudspeakers.
- **Without a warning, later**: the page served from the suite's website over its real certificate, the data going
  phone to helper directly over WebRTC, a small signalling service carrying only the handshake. A relay through a
  server is not an option: it adds an internet round trip to every update.

- **Pitch has two signs in use.** ADM-OSC's is up-positive, as the suite's. IEM's and SPARTA's yaw-pitch-roll are
  Tait-Bryan angles about z, y, x on the same axes, so pitch turns about +y (left) and looking *up* is negative —
  confirmed for IEM in Headitude's conversion, assumed for SPARTA, which uses the same order. Read with the wrong
  sign, looking up turned the scene down. Quaternions carry no such choice.
- **Quaternions inside.** Phones and AirPods measure a quaternion; Euler input is converted on arrival, so nothing
  gimbal-locks.
- **A phone points with one of its axes** — its top edge or its screen normal, chosen — since its orientation is
  against the world, with yaw a compass heading. Point at the front and recentre.
- **Smoothed, with the rate shown.** Phones over Wi-Fi send unevenly at 50–100 Hz; the incoming rate is displayed so
  a sluggish feel has a visible cause.
- **Local only until asked.** A phone needs the port open on the network, which opens it to the whole network; the
  receiver binds to this computer until the user switches the network on.
- **One port, one receiver, its number chosen.** The decoder exists once per session, so one listening port is
  enough — but 9000 is everyone's default, and on the machine the prototype was built on REAPER itself already held
  it. A port that is taken is said so plainly, with the program to look for, not left silent.
- **Recentring takes off the yaw only.** Pitch and roll stay against gravity: recentring while looking down and then
  turning keeps the head looking down, where taking off the whole orientation added a false roll of 9° to a plain
  turn.
- **Applied on arrival, not on a frame.** The orientation is applied as each message lands; the prototype first
  applied it on the screen's frame, and it froze whenever its window was hidden.

**Ported, the conversion** (`core/include/bambi/decode/orientation.hpp`): a sender's numbers — a quaternion w-first
or w-last, or yaw, pitch and roll in degrees with either pitch sign or in radians — into the head's orientation in
the world, with learn mode's flips, the phone's mounting (on the band or held in front) mapped into the head's frame,
the yaw-only recentre, and the table of well-known addresses. Checked against the bridge's own quaternion for every
family to 0.01°, for the pitch signs, the mountings on canonical poses, and the recentre keeping a plain turn a plain
turn. What stays in the plugin: the socket, the OSC bytes, gathering values sent under split addresses, the rate
readout, and learn mode's picking.

### 7.2 What it drives

- **The scene before the binaural decode** — a rotation of (N+1)² × (N+1)² per block, negligible. *Ported*
  (`core/include/bambi/decode/headrotation.hpp`, [0180](../DECISIONS.md)): the field turned by the inverse of the
  head's orientation, which arrives from any thread; a 30 ms glide stepped per 64-frame hop and crossfaded within
  it, so nothing snaps; the same bits however the frames are cut; 0.1 % of a core at order 3, 0.5 % at order 7
  while turning. What converts a tracker's quaternion or yaw, pitch and roll into the orientation matrix is the
  receiver's (§7.1), ported as `orientation.hpp`; the plugin's receiver is a `juce::OSCReceiver` on the message
  thread, the known addresses only, with the sliders taking over while no tracker speaks.
- **The Lens's window** (§6).
- **Placing the loudspeakers** of an array (§3.3).
- **Nothing on an array's output**: the listener's head is really in the room.
- **Recentre** on a key.
- **OSC on overrides; OSC off returns.** With OSC switched on, the orientation it sends replaces yaw, pitch and
  roll — their automation and their sliders are ignored. Switched off, automation and sliders apply again.
- **An offline render takes the automation.** No OSC arrives in an offline render, so it renders yaw, pitch and
  roll as automated: a bounce never captures a head's movements.

### 7.3 Not here: OSC into the encoder

Moving a *source* belongs to the encoder, which already keeps OSC as a modulation source for its transform — a
controller, a show-control cue, another application driving yaw. Pointing a phone to place a source would be one
more input there. It is a later feature of the encoder, not of the decoder: each encoder instance takes its own
port or its own ADM-OSC object (`/adm/obj/n/azim`, `/elev`, `/aed`), so nothing fans out through the decoder. And
since a bounce must match what was heard, it lands on parameters the host records as automation, never as a live
value only.

**Shared between the two: the receiver** — parsing, the format presets, learn, smoothing — extracted into the shared
library when the second plugin needs it, not before.

## 8. The layout of the plugin

**Not the effects' layout.** The same shell, theme, controls and sphere view; a different centre:

- **One scene in the room's frame** shows the session's sources, the layout's loudspeakers, a head turned by the
  tracker and the Lens's window. What plays decides what is full ink — the head while headphones or the Lens play,
  the loudspeakers while a layout plays or headphones listen through it — not what is drawn. A *head frame* toggle
  turns the view with the gaze.
- **The outputs are tabs in the header** ([0178](../DECISIONS.md)), where the effects keep their instances:
  headphones · speakers · lens. **The name opens an output's page; a square in the tab plays it**, so opening the
  speakers' page while listening on headphones changes nothing heard. A second decoder in a session gets a small
  instance switch, only then.
- **The right panel shows the open output's page only**, each with its channel map: the bus's channels, what plays
  on each. **The hull the layout pans across is drawn on the scene while the speakers' page is open.**
- **The head strip sits where the effects have their matrix**: what turns the head (none · OSC · AirPods · phone),
  whether data arrives and how evenly, yaw, pitch and roll with the tracker's value as the live mark, recentre. Its
  setup opens as a temporary tab, as **personalisation** does from the headphones' page.
- The sketch is `prototypes/ui/decoder/`. **Built** (`plugins/decoder/`, [0182](../DECISIONS.md)) on the shared
  window: the scene views draw the room's frame, the loudspeakers, the hull while the speakers' page is open, the
  head and the session's sources; the head strip stands in the matrix's place (the shared window's one new slot);
  the outputs are the header's tabs with a square each (its one new option); the panel holds the open output's page
  and the temporary tabs for the tracker and personalisation. The shared level column at the right edge draws one
  bar per channel of the output bus in place of its one meter (its one option), so what each output writes is read
  off the column and the channel map on the page says whose channel it is. Grey where the sketch says *later*: the lens, the
  KU100 and a SOFA file, headphone correction, the AirPods helper and the phone's page, learn mode, placing by
  looking. `bambi-decoder-shot` draws it to a PNG; `bambi-decoder-check` is its headless contract.
- **The layout goes out to the other plugins**: while it plays a layout, the decoder publishes its loudspeakers on
  the link bus, and every plugin's scene can draw them as an overlay; the decoder's own scene can show the
  encoders' sources ([0176](../DECISIONS.md), [VISUALISATION §5](../suite/VISUALISATION.md)). Its link record
  carries the layout from the first version, not added later.
- **Its buses are the conformance suite's third shape** ([0183](../DECISIONS.md)): the field on one input bus at
  the order that fits, no sidechain, an output of any width from 2 to 64 that the input never moves — asked of
  the sources in both formats, of the installed VST3 as a host loads it, and of the installed CLAP through its
  entry point, where the port configurations move the field's order and leave the output the bus it was. The
  widths a host offers (REAPER up to 128 a track, the Steinberg hosts 16) are inside what it claims.

## 9. Open questions

- **Name.** "Decoder" is a working title. Does it keep the name *Lens*, or take another?
- **Orders**: 1–3 like the rest of the suite, or higher for arrays that can use it?
- **Does stage one (§5.1) already give most of the thesis's gain?** Settled by listening, against the KU100 and
  against a deliberately poor set, as the thesis did.

## 10. Prototypes, in order

All in the browser, under `prototypes/decoder/`:

1. **MagLS binaural with head rotation** from the mouse — *built*, `listen/`; waiting on ears.
2. **The array heard on headphones** (§3.1) — *built*, `listen/`, against the direct decode and an order-7
   reference, with distance compensation on and off; waiting on ears.
3. **An OSC → WebSocket bridge**, since a browser cannot receive UDP — *built*, `osc-bridge.py` with the tracker
   section of `listen/`. Every format of §7.1 reads back the orientation it was sent, to 0.01°, including the
   split addresses some apps use and both ways of holding a phone; a synthetic head (`--demo`) drives it end to end.
   *Waiting on real devices*: an Android phone through Sensors2OSC.
4. **Personalisation against a real loudspeaker** (§5.0, §5.1) — *built*, `match/`: head size and ear size on the
   KU100, the loudspeaker located by looking at it, real and virtual bursts alternated, one audio context per output
   device. The renderer and the audio path check against the Python chain exactly. *Waiting on ears.*
5. **The Lens** (§6), on the stereo pair.
6. **Placing loudspeakers by looking** (§3.3): how far head pointing scatters, and how far yaw drifts over a
   calibration.
7. **AirPods read by a bundled helper** (§7.1) — *built and ported*, `airpods-helper/`, a throwaway VST3 with the
   helper nested in its bundle; the helper is the decoder's now. In REAPER the helper asked for permission, the orientation arrived smooth over
   localhost UDP, and the helper quit with the host. The plugin's own CoreMotion request, the spike before it,
   crashed REAPER.
8. **A phone as the tracker, through a page** (§7.1) — *built*, `phone-gyro/`: a standard-library server with a
   self-signed certificate, the phone page and a dashboard with the QR code, sending OSC to SPARTA's ambiBIN. On an
   iPhone the page was a secure context past the warning, the WebSocket held, the round trip was about 9 ms, and it
   felt usable with a short tutorial. *Open*: an Android phone; the signs of yaw, pitch and roll against a
   renderer, unchecked.
7. **Layout import** (§3): AmbDec and the BS.2051 layouts beside the IEM JSON already read.
8. **A few notches in place of the notch layer** ([HRTF-DATA §7](HRTF-DATA.md)) — *built*, `notches/`: a head's smooth
   part with two or three synthesised notches and a peak, A/B against the head itself at any measured direction of the
   median plane, with plots of what plays. *Waiting on ears.*
9. **The split of the CLL into a smooth part and a notch layer** ([HRTF-DATA §7.1](HRTF-DATA.md)) — *built*, `split/`:
   any measured head, as measured or symmetric from one ear; ITD, ILD and CLL over a slice; four splits with their
   knobs; the top of the range faded to flat. *For looking at, and being decided by looking.*
10. **The dips as sheets over the sphere** ([HRTF-DATA §7.3](HRTF-DATA.md)) — *built*, `sheets/`: from a measured
    head to a few surfaces of notches — the CLL prepared, split, the dips linked into tracks, gathered into surfaces
    and fitted to the measured CLL again — beside the ITD and the ILD as low-order patterns. No sound; *for looking
    at.*
11. **Nobody's head and two sliders, heard** ([HRTF-DATA §7.8](HRTF-DATA.md)) — *built*, `sheets/listen.html`: a
    listener's measured head against the KU100 and against the model — nobody's head with the head slider and the
    ear slider, learned from all three databases or from HUTUBS and 3D3A — at any measured direction. *Heard: the
    model places a sound nearly where the listener's own head does, and better than the KU100.*
