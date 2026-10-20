# bambi Echo — send, controls, modulation and ducking

What the plugin offers and how it is driven. Part of [bambi Echo](ECHO.md). Parameters are
`core/include/bambi/echo/params.hpp` (keys never renamed or removed, new ones appended); the control layer is
`core/include/bambi/echo/control.hpp`, where `EchoResolver::resolve` is the one place settings become samples,
gains and radians.

## The send

What a send and a return are is the suite's ([REGIONS](../suite/REGIONS.md)): a region is the plugin's own, a
fixed slot with host parameters, automated and modulated, never shared between instances. **Echo has one slot, the
send**, applied **once to the input before the shared line**, so every tap hears the same sent field. Its settings
are the shared region block (`region1.*`): shape kind, side (inside or outside), orientation, size, softness and
the rates that turn it, plus a `send amount`. The default is *everywhere*, with no matrix applied at all.

**Echo has no return region.** A tap's own spin places its repeat; a return would be a second answer to the same
question. **A tap has no region of its own either**: masking what the taps hear is a region gate in front of Echo,
one application instead of four.

A Reverb's return region can be **copied** to Echo's send, by automation lane or by clipboard, so the echo feeds
exactly where the room comes back from, or with the side flipped exactly where it does not. It is a copy, not a
connection: nothing follows afterwards, so a bounce still matches what was heard.

## The controls

| group | control (key) | range | default |
|---|---|---|---|
| pattern | one row above the strip: `even · ping-pong · spiral · cascade · single` | | spiral |
| tap | on, synced (`tapN.on`, `.synced`) | | tap 1 on, rest off; all synced |
| timing, synced | `steps` (`.steps`) sixteenths a pass lasts | 1…16 | per tap, below |
| | `offset steps` (`.offset_steps`) | 0…16 | per tap, below |
| | `swing` (`.swing`) a fraction of one more step | 0…0.9 | 0 |
| timing, free | `time` (`.time`) ms a pass lasts | 20…2400, log | the synced time at 120 bpm |
| | `offset` (`.offset`) ms late | 0…2400 | likewise |
| level | `level` (`.level`) | −40…0 dB | −3 |
| | `feedback` (`.feedback`) per pass | −24…−0.3 dB | −6 |
| one pass | `axis azimuth` (`.axis_az`), `axis elevation` (`.axis_el`) | ±180°, ±90° | per tap |
| | `spin` (`.spin`) degrees per pass | ±180° | per tap |
| | `skew` (`.skew`) per pass | ±0.6 | 0 |
| | `blur` (`.blur`) vaguer per pass | 0…180°, tapered about 30° | 6° |
| | `low cut`, `high cut` (`.low_cut`, `.high_cut`), applied every pass | 20 Hz…2 kHz, 500 Hz…20 kHz, log | 80 Hz, 14 kHz |
| output | `wet`, `dry` (`output.wet`, `output.dry`) | −60…+12 dB, −60 is silence | 0, 0 |
| send | region settings and `send amount` | | everywhere, amount 1 |

- **The synced and the free rows are both on screen whatever `sync` says**, the inactive pair greyed and inert;
  swing sits on its own row under them. The mechanics are [ECHO.md](ECHO.md#time-synced-or-free-per-tap).
- **Blur's travel is tapered, not linear.** Blur compounds as √passes, so 30° a pass is 120° by the sixteenth:
  nearly everything a user reaches for is in the first sixth of the range, and the rest is the far end. The taper
  puts 30° at mid-travel, so the left half of the bar is 0…30° and the right half holds what 30…180° reaches.
- **180° of spin is ping-pong**: twice is back where it started.
- **Wet and dry are the plugin's, not a tap's.** They are two levels, not a mix: one knob cannot keep the echoes
  where they are and bring the source forward. They are in the level column, always on screen, and they are
  modulation targets.

### The four taps as they open

Switching on the second gives a pair turning opposite ways a sixteenth apart; the fourth is a ping-pong on dotted
eighths.

| tap | on | time | offset | axis | spin per pass |
|---|---|---|---|---|---|
| 1 | yes | 2 · `1/8` | 0 | up (elevation 90°) | 45° |
| 2 | no | 2 · `1/8` | 1 | up | −45° |
| 3 | no | 4 · `1/4` | 0 | left (azimuth 90°, elevation 0°) | 60° |
| 4 | no | 3 · `1/8.` | 0 | up | 180° |

### Patterns

A pattern is a starting point (`echo/patterns.hpp`). It writes, per tap, `on`, `synced` (to synced), `steps`,
`offset steps`, axis, `spin`, `skew` and `feedback`, and nothing else, through the host's own parameters inside
gestures, so it automates and undoes like a hand edit. The chosen pattern stays lit while the taps are edited;
double-clicking it brings its values back. A tap a pattern leaves empty is off, with a fresh tap's settings.

| pattern | what it writes |
|---|---|
| even | tap 1: 1/8, no spin |
| ping-pong | tap 1: 1/8., spin 180° |
| spiral | tap 1: 1/8, spin 45° (a fresh instance) |
| cascade | taps 1 and 2: 1/8, a sixteenth apart, spin ±45°, skew 0.3 |
| single | tap 1: 1/8, no spin, feedback −24 dB |

## The panel

Two tabs, mirroring Reverb's frame: **taps · send**. The count is the plugin's; the frame is shared. A matrix
column opens a temporary third tab after them.

- **taps** holds the pattern row, the pinned strip, and under it a segmented choice **timing · axis · every pass**
  drawing only that category for the selected tap.
  - **Each of the four strip rows carries a tap's on/off at one end and its level at the other**, and the row
    selects the tap ([ECHO-VISUALISATION.md](ECHO-VISUALISATION.md#the-strip)). The row says *which tap*, the tab
    says *what*, so a delay is built by setting four periods, then four axes, then four per-pass behaviours with
    all four taps never leaving the screen. Switching tab never moves the selection, and selecting never moves the
    tab.
  - **axis** is the azimuth and elevation. **every pass** reads as two pairs, one to a row: `spin` beside `skew`
    (where each pass goes, along and around the axis) and `feedback` beside `blur` (how each pass differs from the
    one before it), then the band (`low cut`, `high cut`).
- **send** is the shared region editor for the one slot, with no send/return sub-selector. It is a permanent tab
  for a fixed slot, not a temporary source page.
- **Ducking has no tab**: it is a row in the matrix, which is on screen already.

## Modulation and ducking

The shared matrix, **generators only** (LFOs, envelopes), plus the detectors below. A parameter is a destination
only if its effect recomputes on the audio thread, in bounded work, from what `prepare` sized: a modulated value
exists only there, so nothing the matrix drives is built elsewhere and adopted later.

| destination | kind | smoothing |
|---|---|---|
| `spin` | rate | 20 ms |
| `axis azimuth`, `axis elevation` | direct angle, wrapping azimuth | 720 °/s limiter |
| `level`, `feedback`, `skew`, `blur`, `low cut`, `high cut` | direct scalar | 40 ms |
| `wet`, `dry` | direct scalar | 40 ms |
| the send region's settings, `send amount` | the shared region block's | |
| **not destinations**: `on`, `synced`, `steps`, `offset steps`, `swing`, `time`, `offset` | | |

- **The period is not a destination**: changing it would need the read to move continuously, and the engine's
  answer to a period change is a one-hop crossfade, not tape-style motion. The offset and swing are integer reads
  of the input line and are not destinations either.
- **`spin` is the best destination in the plugin.** One angle, no matrix, orthonormal: a step moves the image
  rather than its amplitude. It acts inside the loop, so it also spins material already circulating.
- **`blur` produces N+1 gains applied to the signal**, so the engine ramps them per sample, not per hop. It
  compounds as √passes, so the audible response lags by passes.
- **`low cut` and `high cut`** compound every pass and the state is cleared only when the tap sleeps or resets, so
  modulating them has a memory as long as the tail.
- **`skew`, `axis` and `feedback` together are a hazard.** With feedback at its top (−0.3 dB) a skewed loop can
  swell, and while the axis or the skew moves a loop can grow though every still setting decays; the energy stays
  in the ring after the modulation passes. The ring-write guard
  ([ECHO-ENGINE.md](ECHO-ENGINE.md#the-ring-write-guard)) holds it at +80 dBFS and counts it.
- Four taps with axis and skew rebuilt every hop cost about 0.8 % of a core at order 7
  ([ECHO-ENGINE.md](ECHO-ENGINE.md#cost)). The send region's orientation is not rebuilt as a matrix: the matrix
  stays in the region's own frame and the signal is rotated in and out.
- **The region is never a matrix source in an effect**: an effect has no source position to read it at, so the
  control step gives the matrix a region value of zero. The region is only ever a target.

### Ducking

Ducking is a row in the shared matrix: the input's `level`, or the sidechain's, onto `wet` with a negative depth.
The depth is the amount and the column is the detector. A depth of 1 spans `wet`'s whole 72 dB, so −0.25 takes up
to 18 dB off at the loudest input. The sidechain (a stereo aux bus) feeds the detectors only, never the field
([ECHO-ENGINE.md](ECHO-ENGINE.md#buses-and-sidechain)).

## What Echo does not have

| not here | because |
|---|---|
| a return region | a tap's spin places its repeat |
| a region per tap | a region gate in front of Echo does it once instead of four times |
| one feedback path for all taps | each loop closes on itself; summed feedback made every tap re-read every tap |
| a separate order cut | `blur` at 180° is it, without the ringing of a hard edge |
| a mix control | `wet` and `dry` are two levels |
| paths other than orbits | they belong to the encoder |
| a skew that rises then falls in one tap | skews compose and never come back; use two taps |

Background: decisions 0049, 0064.
