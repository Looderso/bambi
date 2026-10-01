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

Headphones and a speaker output run **at once**, on separate outputs, so headphones on and speakers on can be
switched at the interface without touching the plugin. Which pair of outputs goes where is the operating
system's or the host's routing, not the plugin's.

## 3. Speakers — a layout, not a preset list

- **AllRAD** over a loaded layout: max-rE weighting, a **two-band decode** (basic below the crossover, max-rE
  above), **imaginary loudspeakers** filling the gaps of an incomplete layout (no lower half, no ceiling).
- **Per-speaker delay and gain from its distance**, so a room that is not a perfect sphere still images.
- **A layout comes in every way it is commonly found**, and the decoder is always designed here, at the suite's
  order, from the positions alone:
  - **IEM JSON** — the AllRADecoder's configuration, and the same format SPARTA's plug-ins import and export, so
    one reader serves both suites. The array on hand comes as one.
  - **AmbDec `.ambdec`** — the format of the Ambisonic Decoder Toolbox and the Linux decoders.
  - **The ITU BS.2051 layouts**, built in: 0+2+0 (stereo), 0+5+0, 2+5+0, 4+5+0, 4+7+0 (7.1.4), 4+9+0 (9.1.6) and on.
  - **Typed in**, or pasted as a list of azimuth, elevation and distance.
  - **Placed by looking** (§3.3).
- **A loudspeaker can be imaginary**: it takes part in the triangulation and its signal is dropped. A layout with
  nothing below the listener gets a point there, so a source below pans to the lowest ring instead of vanishing.
- **The subwoofer channel** is skipped by the decoder; bass management is a question of its own.
- **A stereo pair** is the smallest layout — a virtual-microphone downmix, which doubles as the check that the mix
  survives stereo.
- **Crosstalk cancellation** (3D from two speakers) is **not a feature**: a sweet spot of a head's width and a
  treated room. It stays a research question until a prototype says otherwise.

### 3.1 Hear the array on headphones

**The layout's speakers, rendered through the HRTF as virtual sources.** At home, with the array's own layout
file, one hears what *that* array will do to the mix: its gaps, its order limit, its missing lower half. This is
the bridge between the two worlds. SPARTA's AmbiDEC also renders its loudspeakers binaurally; what is this
decoder's own is doing it head-tracked, from the same layout file the room uses, in the workflow around it. *Opinion: the feature that makes
this decoder worth choosing.*

The loudspeakers are heard as points: each is encoded at order 7 and decoded by MagLS at order 7, well above the
array's own order, so what is heard is the array's panning and not a second blur on top of it. The head turns
against the room, as it would standing in it.

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
- **No room.** A little room helps a dry binaural render out of the head, but a room is Reverb's job, chained
  before the decoder when wanted. The decoder adds nothing to the field.
- **Latency**: 64 samples (1.3 ms), a bulk delay that keeps the part of a response ahead of the head's centre
  causal. The convolution is 2 × (N+1)² filters of 256 taps; its cost is measured before any claim.

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

### 4.1 Which HRTF

**Default: the KU100.** It sounds good out of the box, and the thesis behind §5 found it the clear winner on
timbre (colouration rated 16 against 8 for an individualised set) — its moderate spectral features suit a broad
population. A listener adapts to a set that is not theirs; they do not adapt to one that sounds bad.

**Every bundled set is checked for its licence first** — several databases forbid commercial use, which would
also bind any model built from them (§5.3).

**Loading a SOFA file** is supported, for anyone with a measured set of their own.

## 5. Personalisation — perceptual, not anthropometric

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
- **The loudspeaker must be heard through the headphones**: AirPods in transparency mode, or open headphones —
  the thesis used acoustically transparent ones for this reason.
- **Where, not what.** The real loudspeaker brings the room's reflections and the virtual one is dry; the task is
  to match the position, not the character.
- The neck's range limits the directions reached; how far is measured in the prototype.

### 5.1 Stage one — no database needed

Parameters with a known physical meaning, cheap to compute from the default set:

- **Head size** → scales the ITD. The largest single cue for azimuth, and the cheapest.
- **Ear size** → scales the direction cues along frequency. Scaling a DTF in frequency is the known first-order
  model of how pinnae differ between people; it moves the elevation notches where a bigger or smaller ear would
  put them.
- **Cue depth** → how strongly direction cues are expressed, from flattened to exaggerated. Exaggerated elevation
  cues are a known aid where the fit is imperfect.

*Opinion: ship this first.* It needs no dataset, and it may already carry most of the gain.

### 5.2 Regions by spherical-harmonic order

**The direction-dependent part of the HRTF, expanded in spherical harmonics, is personalised order by order — coarse
regions first, finer ones only where needed.** In place of the thesis's fixed directions with a Gaussian weight
around each, a region is what an SH order resolves, and a blur of a given width is a spherical cap — in SH, one
weight per order, the same kernel the encoder's width uses.

- **What is expanded**: the direction-dependent magnitude, in dB, per frequency band. The ITD stays apart, as head
  size (§5.1).
- **Order 0 is the timbre.** The order-0 coefficient of that expansion is its average over all directions — the
  diffuse-field spectrum. Keeping order 0 on the KU100's and personalising orders 1 and up is the split of §5 made
  exact: space is personalised, colour is not.
- **Rising order, rising resolution.** Order 1 is the broad front/back and up/down balance, orders 2–3 quadrants;
  the pinna notches that carry elevation sit at high frequencies and need high orders. A listener tunes a few
  parameters first and more regions only as far as they help.
- **Without a database** (stage one): a cue depth per order on the KU100 itself — for instance the order-1
  front/back difference between 4 and 10 kHz, exaggerated or flattened.
- **With a database** (stage two): the clustering or PCA across listeners is done per order band, and the tuning
  walks the bands from low to high.
- **Open**: which order the KU100's elevation notches need, measured on the set before any control is designed
  around it; and whether regions tuned independently still sound like one head.

### 5.3 Stage two — a model from many listeners

The thesis's own future work: **PCA over a unified multi-database set**, and a few sliders each moving one
principal component, in place of the thesis's single sorted-index slider. It ships as a mean and a few
components, not a database. **The thesis's pipeline is not available**, so this is new work: the dataset
unification, the normalisation and the model are rebuilt, and the licence of each source database decides what
may go in.

### 5.4 Portability

**The session stores everything it can**: the personal numbers, the layout's contents (not a path to its file),
the decoder's settings, the OSC configuration. A session reopened on another machine renders as it did. A
per-user profile may *seed* a new instance; it never silently changes an existing one.

## 6. Lens — a stereo pair as a probe

**Headphones experience the scene; the Lens inspects it.** Binaural elevation is ambiguous for many listeners,
and a dense scene makes it hard to tell a spatial problem in the mix from one in the HRTF. The Lens plays one
slice of the sphere on the physical stereo pair: a second, physically grounded view of the same field,
independent of any HRTF.

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
| **SPARTA** | `/ypr`, `/quaternion` | degrees; quaternion w, x, y, z |
| **IEM SceneRotator** | `/SceneRotator/ypr`, `/SceneRotator/quaternions` | the same two forms |
| **Learn** — anything else | whatever arrives | the plugin shows each incoming address; the user picks quaternion (w first or last) or yaw-pitch-roll, degrees or radians, and which axes flip. Saved as a preset |

Headitude (AirPods), the bridge apps of hardware trackers and most existing setups send one of the first three.
Phone apps — Sensors2OSC on Android (`/rotationvector`, w last), ZIG SIM, GyrOSC — go through learn.

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

### 7.2 What it drives

- **The scene before the binaural decode** — a rotation of (N+1)² × (N+1)² per block, negligible.
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

- **The sphere shows the incoming scene's energy**, with the output drawn over it — the layout's speakers as
  dots, or a head at the tracked orientation, or the Lens's window.
- **An output selector** where the effects keep their pages: Headphones · Speakers · Lens, and the layout
  loader under Speakers.
- **Personalisation is its own page**, entered on purpose, left when done.

## 9. Open questions

- **Name.** "Decoder" is a working title. Does it keep the name *Lens*, or take another?
- **Buses**: headphones and a speaker output at once is an output of 2 + N channels. **REAPER is the host for it**
  (128 channels a track); Pro Tools Ultimate takes ambisonic tracks to order 7, Nuendo, Cubase and Studio One to
  16 channels, so they can feed the decoder but not run headphones and an array of 16 at once. Those hosts get
  headphones or a standard layout. What each negotiates is settled with the conformance suite.
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
   *Waiting on real devices*: AirPods through Headitude, an Android phone through Sensors2OSC.
4. **Personalisation against a real loudspeaker** (§5.0), head-tracked through the bridge, with the parameters of
   §5.1 and §5.2 on the KU100. A browser can send two audio contexts to two output devices.
5. **The Lens** (§6), on the stereo pair.
6. **Placing loudspeakers by looking** (§3.3): how far head pointing scatters, and how far yaw drifts over a
   calibration.
7. **Layout import** (§3): AmbDec and the BS.2051 layouts beside the IEM JSON already read.
