# Modulation — types and contracts

The shape every modulation source, destination and feature fits into. The engine is `ModulationEngine`
(`core/include/bambi/mod/modulation.hpp`), shared by every plugin; each plugin supplies a **manifest**
(`mod/manifest.hpp`) that binds the engine's roles to its own parameter keys. The rows and their ranges are the
plugin's; the arithmetic is not.

## The arithmetic

```
contribution = source value × source amount × cell depth
destination  = base + Σ contributions × global amount × (max − min)     clamped to [min, max]
```

- **Source amount** is `mod.amount.<source>`, 0 to 1, default 1, a host parameter: automation rides the amount,
  never the cell. **Global amount** is `mod.global_amount`.
- **Cell depth** is −1 to 1; zero removes the cell. One cell per (tab, column, target).
- Modulation spans the destination's own range, so a depth of 1 at full deflection covers its full span on every
  destination.
- A destination owns its **base value** (a parameter, not a matrix cell) and its range. The UI shows when a
  destination rails, since clamping hides every individual contribution.
- Control runs on a fixed **256-sample grid**, so nothing depends on the host's block size.

## The clock is not in the matrix

The position clock (`MotionClock`) is monotonic, wraps, and has no shape, polarity or slew. It is not a source;
its **rate** is a destination (Encoder `motion.speed`) and the clock is the integrator. Constant motion is a
non-zero speed base, never a cell. Routing it through the matrix would put a wrapping ramp behind a destination
smoother.

## Sources

Nineteen slots, in a fixed order. Each tab of the matrix holds six columns, the region tab one.

| slots | tab | source | polarity | deterministic |
|---|---|---|---|---|
| 0–5 | Features | the plugin's own input: level, attack, tonal, low, mid, high | unipolar [0, 1] | yes, a function of the audio |
| 6–11 | Sidechain | the same six, from the sidechain bus; an absent sidechain reads 0, never a stale value | unipolar [0, 1] | yes |
| 12–14 | Generators | LFO 1–3 | bipolar [−1, 1] at rest, set by its polarity knob | see Determinism |
| 15–17 | Generators | envelope 1–3 | unipolar [0, 1] at rest, set by its polarity knob | inherits its trigger's |
| 18 | Region | the plugin's own region (Encoder only) | unipolar [0, 1], exactly | yes |

**External inputs are not sources.** Host automation moves a base parameter; a MIDI note triggers an envelope.
Neither passes through the matrix. Feature values never travel over the link bus.

Every source is normalised at its own boundary, so a coefficient means the same in every cell: this source at
full deflection covers the destination's full span.

### Polarity

> Audio features are unipolar measurements. Generators are bipolar by default. Signed behaviour is a routing
> choice, not a property of the measurement.

A feature has a floor and a ceiling and no natural zero, so it is never made bipolar inside the detector. Sign
comes from the cell's depth. A generator re-centres through its own **polarity** knob (0 to 1):
`polarise(v, p) = v · (1 + p) − p`, applied to the shape in [0, 1]. An LFO's default is 1 (bipolar); an
envelope's is 0 (unipolar). The knob moves the floor and pins the ceiling at 1, so a value never leaves [−1, 1].

### LFO

Four shapes (sine, triangle, saw, sample-and-hold); rate 0.01 to 2 Hz free or one of sixteen note
divisions (8/1 to 1/16, each with its triplet) synced; phase 0 to 360°; retrigger **restart** or **continue**.
Sample-and-hold draws once per phase wrap.

### Envelope

ADSR, unipolar at rest: attack 0.1 to 2000 ms, decay 0.1 to 5000 ms, sustain 0 to 1, release 0.1 to 8000 ms, and
a curve per timed stage (−1 to 1). A curved stage reaches its target at exactly its labelled time for every
curve. A hit while the envelope still sounds rises from where it is; release from sustain takes the labelled time
scaled by the distance to fall.

An envelope fires from **MIDI** (a note or range, one channel or any, velocity amount 0 to 1, gate *held* or
*one-shot*) or from **audio** (a feature slot 0–11 crossing a threshold, released below threshold minus
hysteresis). Hysteresis belongs here, where the output is an event; a measurement is made continuous with a
ramp, never a latch.

### Region

The plugin's own region evaluated where the source was **one control step ago**, clamped to [0, 1], so it is the
same at every block size. After play, locate, loop or a loaded state it reads the position worked out from the
parameters as set. Only the Encoder feeds it; Echo, Reverb and Decoder offer no region tab, since their regions
are a send and a return rather than a source.

## The feature contract

Every audio feature satisfies all of these.

1. **Reads 0 in silence.** Silence parks the source. All but Level are multiplied by a silence gate (−55 dB,
   opening over a 3 dB band). Level reaches 0 through its own release, so a long release is audible as a long
   fall and a ducker's wet does not return at once.
2. **One loudness feature.** Level is the only one that tracks loudness; Attack is a ratio, Tonal a flatness, the
   bands are a spectrum share scaled by the level.
3. **Fixed, interpretable calibration.** Floor and ceiling are constants calibrated against real material, not
   controls. The UI shows them (`featureCalibration`). Level's fall is the one detector parameter
   (`level.release`, `sc_level.release`, 20 ms to 2 s).
4. **Zero lookahead.** Latency would break "responds to what you hear" and impose delay compensation on a plugin
   that needs none.
5. **A guide to destinations.** Impulsive sources (Attack) suit direct destinations; continuous ones suit rates.

### The six

| feature | answers | how | range |
|---|---|---|---|
| **Level** | how much | RMS envelope, attack 10 ms, release 200 ms by default | −55 to −6 dB |
| **Attack** | how sharp | fast/slow envelope ratio (3 ms / 150 ms), gated and opened by a ramp | ratio 1.0 to 2.2 |
| **Tonal** | how sustained | magnitude spectral flatness over 100 Hz to 8 kHz, smoothed 40 ms | −2 to −13 dB |
| **Low / Mid / High** | where in the spectrum | share of spectral energy below 250 Hz, up to 2 kHz, above; scaled by the level | −55 to −12 dB |

Analysis uses a 2048-sample Hann window every 256 samples. Only Tonal and the bands need the FFT; the engine
skips it when no cell uses them.

**Attack and Tonal are anti-correlated in time** (strike, then ring), so on different destinations they give
"attack does X, then sustain does Y" with no envelope. Bands are interpretable but not orthogonal: all three rise
on a dense mix and separate on sparse material.

### In a field effect

A field effect never sees a source, only the bus, so its detector reads the **whole field**. `fieldPower` is the
sum of every channel's square over (order + 1): a single source reads exactly what W alone reads at every order,
and nothing in it can cancel (an antiphase pair cancels in W). Level, Attack and the gate read it; Tonal and the
bands take their spectrum from W, being ratios that need a wave, scaled by the field's level. A sidechain is a
second, local detector input, never required. Ducking is a row: Level, from the input or the sidechain, onto
`wet` with a negative depth.

## Destinations

| type | memory | modulation means | smoothing |
|---|---|---|---|
| **Rate** | integrated by a clock | how fast, and which way | one-pole, τ = 20 ms |
| **Direct scalar** | none | where / how much | one-pole, τ = 40 ms (Reverb `room.size`: 250 ms) |
| **Direct angle** | none | where | rate limit, 720 °/s (Encoder `motion.displace`: 4 laps/s) |
| **Not modulatable** | none | nothing: the parameter's own value, unsmoothed | none |

- A rate destination accumulates, so `base + Σ(source × coefficient)` gives constant motion, feature-driven
  motion or any mixture with independent signs. Position destinations need a real maximum angular velocity, not
  a one-pole.
- A parameter that is one turn wraps: it is kept within one period of its minimum, smoothed the short way round,
  and never rails. Encoder `motion.displace` is one lap, folded by the movement mode (wrap, ping-pong, or run
  once), set before the engine runs.
- `isModulationTarget` is "kind is not Not-modulatable": one list answers both "does it get a matrix row" and
  "is it smoothed at all".
- Time constants are seconds inside the engine and milliseconds only at the UI boundary.
- Gains reach the audio per sample, not per step: each engine ramps a frame's changes over one hop from an
  absolute position (`dsp/ramp.hpp`), so a staircase of control steps is never heard.

### What may be a destination

> A parameter can be a destination if its effect recomputes synchronously on the audio thread, in bounded work,
> with everything sized at `prepare`, and is interpolated per sample across the hop if it is a gain.

A modulated value exists only on the audio thread, so a message-thread rebuild adopted when it arrives would land
at a different sample in an offline bounce. A region matrix rebuilt by quadrature (3 127 µs at order 7, 58.6 % of
a 5.333 ms control hop) fails; the same rebuild from per-order parts (2.43 µs) passes. A destination that is a
vector of gains needs a ramp, not a smoother.

### Rows and targets

The component and the arithmetic are shared; each plugin chooses its rows. Every plugin has a **base target**, a
row that exists whether or not anything drives it. Any other target gets a row while it has depth on any tab, in
the order it first got depth.

| plugin | base row | rates | direct scalars | direct angles |
|---|---|---|---|---|
| Encoder | `motion.speed` | speed, transform yaw/pitch/roll rates, region rates | transform extent, render width, output gain, input trim / spread / offset, a parametric path's non-turning settings, region shape settings | motion displace, transform yaw/pitch/roll, a path's turning settings, region angles |
| Echo | `output.wet` | tap spin, region rates | wet, dry, tap level / feedback / skew / blur / low cut / high cut, region shape settings | tap axis az/el, region angles |
| Reverb | `room.distance` | region rates | room size / decay / tone / roughness / distance, wet, dry, pre-delay, input cuts, region shape settings | region angles |
| Decoder | `head.yaw` | head yaw/pitch/roll rates, region rates | output gain, region shape settings (the decoder reads nothing from its region) | head yaw/pitch/roll, region angles |

Not destinations anywhere: a region's kind, counts or side; a generator's or detector's own settings; a tap's
switches and step counts; Echo's period and offset; the quality switch.

## Determinism

An offline bounce is sample-identical to the realtime pass.

- **Feature** — a function of the audio.
- **LFO, synced, restart, not sample-and-hold** — a function of timeline position (`ppq`): a bounce agrees from
  any start point.
- **LFO, free** — runs while the transport plays and restarts on play, locate and loop; a bounce agrees only from
  the same start point. Sample-and-hold and every rate behave this way too.
- **LFO set to continue** — never reset by the transport (reset only on prepare and an offline start). A bounce
  then differs from what was heard, by the user's choice; restart is the default and the UI says when it is not.
- **Sample-and-hold** — seeded: the seed is a constant per LFO index (not in state), a xorshift generator
  identical on every platform.
- **Envelope** — inherits its trigger's.
- **Region** — reads the previous step's position, so block size cannot matter; its rates integrate on a clock
  that resets, holds and continues as the placement's does.

Only a tempo-synced, restarting, non-sample-and-hold LFO is a function of timeline position; every other
generator locks to the play start, which decides what may be copied between instances.

A source is checked against this when it is designed, not when a bug is filed.

Background: decisions 0030, 0049, 0242.
