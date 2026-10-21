# Changing a delay time

A delay time that changes has three honest behaviours. They differ in what they do to pitch, to the tail in
flight and to memory, and they are not interchangeable. Echo (a tap's period) and Reverb (the room, the
pre-delay) both need them, so the page is shared.

What exists in `core/`: `dsp/fielddelay.hpp` (`FieldDelayLine`, a ring over a whole field), the Echo engine's
rings (`echo/engine.hpp`), and Reverb's rings (`reverb/tail.hpp`, `reverb/early.hpp`). `echo/warp.hpp` is Echo's
skew, not a delay.

## The three behaviours

| behaviour | what it does | artefact | where it is built |
|---|---|---|---|
| **Read-head motion** | the read position moves smoothly through the line | **pitch moves** by `1200 · log2(1 + d)` cents for a delay changing `d` samples per sample; a third-order Thiran allpass read keeps every frequency's level, so the motion costs no band its decay | Reverb's tail lines: each line's length drifts, bounded to 4 cents of pitch and a 2 ms swing, in segments of 0.3 to 0.9 s |
| **Crossfaded relocation** | two reads of one buffer, faded from the old position to the new | comb filtering where the two reads correlate, a level dip where they do not; follows steps, not a continuously moving target | **Echo's period**: old and new period are both read from the tap's ring and crossfaded linearly over one control hop (256 samples, 5.3 ms at 48 kHz). **Reverb's room size**: two tail networks fade past each other in equal power over 0.25 s |
| **Clear** | the buffer is forgotten | **destroys the tail in flight** | Echo on play, locate and loop, and a tap that is off and faded out; Reverb when the tail's line count changes |

Clearing is virtual: `FieldDelayLine::restart()` zeroes a count of frames written, and a read before that count
returns silence, so it is O(1) however large the ring.

### Echo's period

- The period is a whole number of samples. The resolver holds it and releases it only when the time has moved by
  more than rounding explains, so a tempo's last bit does not move it.
- The crossfade runs from an absolute position in the hop, so it is the same at every block size. A processing
  chunk is never longer than the shorter of the two periods, so a read never overlaps the write.
- A fetch reaches past what was written since the last restart as silence. A period lengthened beyond the history
  the ring holds therefore fades toward silence for as long as the history is short; once the ring has filled, a
  lengthening crossfades like a shortening. This is causality, not an engine limit.
- No pitch bend: the loop's content is relocated, not stretched.
- The matrix cannot move a tap's time: `period` and `offset` are not modulation destinations. The host can
  automate them, which is a step through the crossfade.

### Echo's offset

The offset reads the shared input line (`offset` samples back) to feed each ring. A changed offset moves that read
at once, with no crossfade and no interpolation. It is not a destination.

### Memory

Every ring is allocated at `prepare` at the order's longest tap and touched, so the audio thread never faults a
page. The longest tap falls with the order: 2.4 s up to order 3, 1.6 s to order 5, 1.2 s above. An instance holds
the input line and four rings, each `(order + 1)²` floats a frame. Arithmetic at 48 kHz:

| order | longest tap | channels | five rings |
|---|---|---|---|
| 1 | 2.4 s | 4 | 9 MB |
| 3 | 2.4 s | 16 | 37 MB |
| 5 | 1.6 s | 36 | 55 MB |
| 7 | 1.2 s | 64 | 74 MB |

At 96 kHz each figure doubles.

### Reverb

| what moves | behaviour |
|---|---|
| room size, shape, decay | the tail's layout is rebuilt only when the line scale moves by more than 0.02 or the mixing time changes; the new network is built empty and faded in against the old one (equal power, 0.25 s). A held-back layout waits for a fade in flight to finish. A different line count restarts the tail |
| early reflections | each image is an integer-sample tap on one of twelve virtual-source lines; a room change rewrites the tap list in place, with no crossfade. Room size is smoothed at 250 ms before it gets there |
| pre-delay | an integer-sample read of a per-line ring of 0.25 s, taken from the smoothed destination each control step. The room supplies the base (the first wall reflection, or 0.7 of the mixing time if later, at most 60 ms); the parameter adds to it, 0 to 200 ms, total at most 240 ms. A moving pre-delay steps in whole samples; there is no interpolation and no crossfade |
| distance | moves the dry only; the geometry is not rebuilt |

Reverb's drift is the only place the pitch budget is spent on purpose.

## The pitch budget

A delay changing by `d` samples per sample shifts pitch by `1200 · log2(1 + d)` cents. For a sine of depth `D`
samples at `f` Hz, `max|d| = 2πfD/fs`, so at 48 kHz:

| staying under | needs | at 1 Hz | at 5 Hz |
|---|---|---|---|
| 10 cents | `D · f ≤ 44` samples · Hz | ±0.9 ms | ±0.18 ms |
| 50 cents | `D · f ≤ 220` | ±4.6 ms | ±0.9 ms |

A chorus-depth modulation of a delay time stays inside it. Sweeping a delay across a beat is a tape effect by
definition. Reverb's drift segments are chosen so the step to the next target is limited to the 4-cent slope over
the segment's own duration.

Cost of a cubic fractional read over an integer one, measured on a scratch implementation (Release, arm64):
+0.068 % of a core a line at order 3, +0.286 % at order 7.

## What a moving delay owes the bounce

A bounce must match what was heard. So the length in samples is a pure function of the control-step value and the
sample rate, with one fixed rounding rule; buffers are sized at `prepare`; a crossfade or glide runs on the
control grid with a length fixed in samples, not blocks; and a transport restart clears the same state in both
passes. A tail agrees from the same start point, never from anywhere on the timeline.

Background: decisions 0038, 0049.
