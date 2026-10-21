# bambi Reverb — the room

An effect on the ambisonic bus: it takes any (N+1)²-channel field — encoded sources, recordings, a whole mix —
and gives it a room, by direction. Regions are [suite/REGIONS.md](../suite/REGIONS.md); the look is
[suite/LOOK.md](../suite/LOOK.md) and the shared scene [suite/VISUALISATION.md](../suite/VISUALISATION.md).

Reverb is **one module**: early reflections and a tail from one room model. Echo and Reverb are separate plugins,
chained on the track, so an echo *of* the room is Echo after Reverb.

- **The dry is the host's.** A gain by direction — a hole, a spotlight, a gate — is a region applied to the input.
- **A module that cannot be heard is not computed:** switched off, or its level at −∞. It fades out over ~20 ms
  before sleeping and its buffers are cleared, so a woken reverb starts clean.
- Code: `core/include/bambi/reverb/` (engine, early, tail, room, control, params, presets),
  `plugins/reverb/`, offline renderer `tools/reverb-render`.

## Controls

| group | controls |
|---|---|
| room | one row of six rooms (ambience · room · chamber · hall · large hall · cathedral), and under it **size · decay · tone · roughness · distance**. A room is a starting point: it writes the room's numbers and the cuts, stays selected while they are edited, and a double-click brings its values back. **Shape — room / hall / tall — rides with the selection, in state; it is not a parameter** |
| output | **wet · dry** (dB, −60 is silence, both open at 0 dB) in the level column, and **pre-delay** (0–200 ms, added to what the geometry gives) |
| regions | **send** and **return**: two slots, each a region of the plugin's own with its own inside/outside and amount |
| input | low cut (20–500 Hz) · high cut (2–20 kHz) |
| ducking | not a control: the sidechain's or the input's `level` onto `wet` in the matrix |
| quality | on the settings page: while playing, **efficient** (8 tail lines) or **realistic** (16), a host parameter; when rendering, same or realistic, state. A bounce can play light and render at full quality |

- **size, shape** set when reflections arrive, how dense they are and when they become a tail; a small room and a
  hall with the same 2 s sound different. **decay** is RT60 at mid frequencies. **tone** is how much faster the
  highs die and how much longer the lows ring. **roughness**: smooth walls give mirror reflections and an
  undiffused onset, rough ones scatter both; 0 is no diffusion. **distance** is where the source is assumed to
  be; a bus effect does not know its sources' distances, so it is one value.
- **Wet and dry are two levels, not a mix.** The wet level is the room's, the dry is where the source is. Distance
  sets the dry: the room's energy grows as r² against a direct sound of 1. Both are an output stage; the
  reflections, the tail and their balance are untouched by them.
- **The balance of reflections and tail is not a control.** It follows from the room.
- **Not controls:** absorption, scattering, diffusion, damping, mixing time, line count, reflection order, tail
  order, modulation. They are derived or fixed.

**The rooms:**

| room | size | shape | decay | tone | roughness | distance | low cut | high cut |
|---|---|---|---|---|---|---|---|---|
| ambience | 4 m | room | 0.35 s | 0.50 | 0.80 | 1.5 m | 180 Hz | 12 kHz |
| room | 5 m | room | 0.5 s | 0.42 | 0.75 | 2 m | 140 Hz | 12 kHz |
| chamber | 7 m | tall | 1.2 s | 0.55 | 0.70 | 3 m | 130 Hz | 13 kHz |
| hall | 20 m | hall | 1.9 s | 0.50 | 0.60 | 6 m | 150 Hz | 11 kHz |
| large hall | 28 m | hall | 2.8 s | 0.45 | 0.60 | 10 m | 140 Hz | 10 kHz |
| cathedral | 38 m | tall | 6.5 s | 0.35 | 0.50 | 14 m | 120 Hz | 9 kHz |

A room preset never touches regions, level or quality. Selecting a room switches the settings whole rather than
gliding through the settings no room has; the tail's two networks crossfade (below).

## Modulation

The shared matrix, with the sidechain and the input's level (W, the field's power, the sidechain) as sources.
**Ducking is a row:** `level`, or the sidechain's, onto `wet`, negative; each level source has a settable release.

- **Targets:** size, decay, tone, roughness, distance, wet, dry, pre-delay, low cut, high cut, each region's
  orientation (with a rate per axis), size and softness, and the send and return amounts. Quality and a region's
  kind and counts are not targets.
- **The room is modulated as a whole:** one level for reflections and tail together, so their balance never moves.
- **Size is smoothed over 250 ms**, other room values over 40 ms: a size that jitters would crossfade networks
  forever.

## The room's engine

Early reflections from **image sources read from the bus**, the tail from a **feedback delay network**, both
derived from one room. Not convolution: convolution is static, costs one convolution per channel and cannot be
modulated.

### One room

| derived | from | how |
|---|---|---|
| volume, surface | size, shape | a box, width × depth × height: room 1 : 1.25 : 0.65, hall 1 : 1.8 : 0.6, tall 1 : 1.1 : 1.2 |
| absorption per band | decay, tone | Eyring, inverted: α = 1 − exp(−0.161 V / (S · RT60)). Tone 0…1 sets the highs' decay to 0.35…1 × mid and the lows' to 1.3…1 × mid; the air adds its own loss to the highs |
| shortest decay | size, shape | α = 0.95: a room cannot be drier than that |
| mixing time | volume | √V ms, held between 25 and 80 ms |
| each reflection's tone | absorption, path | every bounce loses what the highs lose beyond the mids, plus the air along the path |
| scattering, diffusion | roughness | a share 0.8 · roughness scatters per bounce, so (1 − s)^order of an image stays a mirror image; diffusion = roughness |
| the tail's pre-delay | geometry | the first wall reflection, or 0.7 · mixing time if later, at most 60 ms |
| line lengths | volume | 100–200 ms × 1.2 · ∛(V / 8640 m³), between 0.55 and 2 |

**Reflections and tail are one decay.** The reflections arriving in the second half of the mixing time give the
level of the exponential decay they are the start of,
`K = E(t_mix/2 … t_mix) / (t_mix/2) · RT60/13.8 · e^(13.8 · 0.75 · t_mix / RT60)`; whatever of `K` is not heard as a
discrete reflection comes out of the tail. Reflections and tail together are held at unit energy, so distance moves
energy between them and the level moves both. Discrete reflections fade out between **0.7 and 1.3 × the mixing
time**; what fades or scatters enters the tail at its own delay, so no energy is lost or added.

### Early reflections on the bus

Reverb never sees a source, only the bus. Per-image panning from a true source position would externalise best but
is phasy and chorus-like on a moving source, and needs the source; the bus read does not.

- **Image sources of the box to order 3** — 62 images.
- **The bus is read at 12 virtual sources**, evenly spread, each with an order-3 max-rE beam. Each is a source at
  the assumed distance in its direction, with its own images; every image is filtered and encoded back into the
  bus from where it arrives.
- **Copies of one reflection land within ±3 ms** (`kSpreadSeconds`): neighbouring virtual sources give the same
  image a few ms apart, and each copy keeps its own offset from their average, scaled so none is more than 3 ms
  away. Copies within half a millisecond comb (up to +6 dB at 500 Hz, −6 dB at 8 kHz), so the spread stays well
  above that.
- **The level is matched:** when the room changes, the unpanned reflections are rendered for six directions and
  matched to the exact model; the match is within half a decibel of 1 in every room.
- **At run time only 12 gains move.** A moving source changes how the bus reads at the virtual sources: no
  Doppler, nothing rebuilt. A room change recomputes the taps (65 to 100 µs, once).
- **Later reflections are encoded at order 1:** a reflection that is both past the mixing time and more than 20 dB
  under the loudest is encoded into four channels rather than sixteen (and a cutoff class that is all narrow is
  filtered at order 1 too). Both conditions are needed: what the lever saves goes with a count, what it risks with
  an energy. The narrowed taps hold 0.16 % of the reflections' energy in ambience, 0.96 % in the hall, 0.26 % in
  the cathedral, and the omni and first-order output are unchanged to the bit.

**Early reflections, measured:** Release, arm64, order-3 bus, 128 samples / 48 kHz, share of one core:

| room | reflections heard | narrowed to order 1 | cost |
|---|---|---|---|
| ambience | 712 | 118 | 3.36 % |
| room | 613 | | 2.95 % |
| chamber | 299 | | 1.68 % |
| hall | 359 | 185 | 1.78 % |
| cathedral | 70 | 24 | 0.98 % |

Cost goes with the count of reflections inside the mixing time, which the room decides: a small room has far more
than a cathedral.

### The tail: a feedback delay network

| part | what |
|---|---|
| diffuser | 4 steps of per-channel delay, a shuffle with polarity flips, a Hadamard mix; the steps add up to the mixing time |
| loop | 16 lines (8 in efficient) of 100–200 ms × size, primes; a **Householder** matrix; an allpass (g = 0.5, 5–15 ms × size) in every line |
| damping | a low and a high **shelf per line** (250 Hz, 4 kHz), designed from the decay each band asks per pass |
| tonal correction (Jot) | shelves on the input restoring each band's energy, so a band's decay changes how long it rings, not the tone; worked out from the energy the lines hold over every pass, which parts from the decay time once a pass loses several decibels |
| input | 12 dB/oct low cut and high cut; a sub-sonic blocker at 5 Hz in the loop, unity at Nyquist |
| modulation | each line's length drifts slowly at random (smoothstep segments of 0.3–0.9 s within ±2 ms), never changing pitch by more than 4 cents; read by a third-order Thiran allpass, flat in level, so the drift takes nothing from any band's decay |
| output | taken where each line starts and a third and two thirds along, at the decay there; every loudspeaker gets an orthogonal, sign-flipped mix of all lines |

- **Its level:** for a unit impulse into the lines (past the input's cuts), every pass keeps g² = 10^(−6 L / RT60)
  and the three taps each carry a third: `(1 + g^⅔ + g^⁴⁄₃) / 3 / (1 − g²)`, times (1 − a)² + a² for a diffusion
  of *a*, because the lines are fed the input as it came and the input diffused, two signals whose energies add.
  The engine's balance uses this.
- **Sixteen lines are 2.3 s of delay**, enough for a colourless tail up to ~15 s (total delay ≥ 0.15 · T60 · fs).
- **Spatialised through directions:** the input is read at K directions, one per line, and the tail played back at
  K directions at the tail order, 3. The tail order is not a CPU lever (3 → 1 saves 0.05 % of a core); the **line
  count** is.
- **A room that changes size does not restart the tail.** There are two networks: the new one is built empty and
  fed the same input, and the two fade past each other in equal power over `kTailFadeSeconds` = 0.25 s, each
  through its own normalisation (a longer line holds less energy for the same decay). A fade already running holds
  the newest layout back and takes it up when it ends, so a dragged size costs one fade a quarter second. The
  guard compares against the layout that is live, not the last control step. A change of line count restarts the
  tail, since the networks would not have the same number of outputs.
- **Send and return are gains, not matrices.** The engine has reduced the field to directions before either region
  is reached: the send is the region's gain at each of the twelve virtual sources and each of the K lines, the
  return the same K gains on the lines' placement, twenty-eight numbers a control step. A gain is what the
  region's matrix does to a wave from that direction (`RegionOperator::gainAt`), so every kind is projected, sectors
  and dots included. The reflections' half of the return goes back over the whole bus through the shared
  projection (`region/projection.hpp`). Gains are reached over a hop, so a moving region never steps the input of
  twelve delay lines.

### Measured

| | |
|---|---|
| decay at mid, set → measured | room 0.50 → 0.484 s · hall 1.90 → 1.901 · cathedral 6.50 → 6.54; the tail alone 0.500 · 1.902 · 6.50, and its bands within 1.5 % (2.76 s for 2.8 below 140 Hz, 0.89 for 0.9 above 9 kHz) |
| tone | against a tail ringing evenly at the mid's decay, every band of a 2.8 / 2 / 0.9 s tail within 0.2 dB |
| C80 | omni, four directions: ambience 19.5 dB · room 14.1 · chamber 4.6 · hall 6.5 · large hall 4.7 · cathedral 1.1 |
| reflections into tail | in ambience and room the energy in 30 ms windows never climbs back after its peak, and past twice the mixing time the tail is 0.7 dB under what the balance gives it |
| drift | never more than 4 cents, never more than 2 ms from a line's length; decay unchanged to 3 % |
| distance | the reflections' share of the room's energy barely moves (0.17–0.21 in the hall from 1 to 14 m); in the wet, 6 → 14 m is worth 0.84 dB, in the dry 19 dB. Automated distance: 0.09 % of a core |
| quality | 8 or 32 lines for 16: level within 1.5 dB, decay within 8 % |
| size change | steady noise, 30 ms windows: a run with no change reads +0.22 / −0.28 dB (the instrument's floor); a 20 → 35 m change +0.40 / −1.25 dB; a 20 → 60 m drag over half a second −0.79 dB |
| send and return | a send is free (28 gains). A return adds the projection over the bus: +0.16 % of a core at order 3, +0.80 % at order 7. A send everywhere at amount 1 is byte-identical to no send |

**Cost of the whole engine**, Release, arm64, 128 samples / 48 kHz, share of one core, order-3 bus:

| | |
|---|---|
| ambience | 6.81 % |
| hall | 5.22 % (8 lines: 3.49 %) |
| cathedral | 4.44 % |
| tail alone | 1.42 % at 8 lines, 2.80 % at 16, 5.48 % at 32 |
| a size change in flight | 9.73 % in the hall, only while the size moves (a second `FdnTail`, 5.10 MB at 32 lines, allocated at `prepare`) |

Order 7 costs what order 3 does: reflections and tail run at order 3 whatever the bus; order 1 is cheaper only
because the bus is four channels.

**Rendered offline** (`tools/reverb-render`, `bambi-reverb-render`): the engine with no plugin and no host, input
generated from a seed so no audio lives in the repo. Thirteen goldens in `tests/golden/scenarios.txt` hash the
audio and the control trace separately. The render is identical at block sizes 1, 64, 128, 333 and 1024 with the
room, both regions, the amounts, the line count and a reset moving inside it.

## Visualisation

The globe is the shared energy visualiser ([suite/VISUALISATION.md](../suite/VISUALISATION.md)). **The incoming
field's energy is orange; what Reverb adds is blue.** The reflections are drawn by the probe, an indicator rather
than energy: point at a direction and Reverb answers *what would be added, for a source there*, a mark per
reflection where it arrives from at its weight, computed from the parameters so it costs the audio thread nothing.
It is the same component as Echo's ping; only the answer differs. The matrix sits bottom left, as in the Encoder.

## Layout and sidechain

The main input is (N+1)² channels in both formats; a sidechain, if offered, is a stereo aux bus after those pins.
The suite's conformance harness asserts it.

Background: decisions 0026, 0064.
