# The Encoder's sources and the matrix

How the modulation matrix is laid out, what each source's settings page holds, and how LFOs and envelopes behave.
The page code is shared (`ui/src/SourceSettings.cpp`, `ui/include/bambi/ui/EnvelopeGraph.h`), so Echo and Reverb draw
the same pages for their own generators. What the sources are and what they can drive: [ENCODER.md](ENCODER.md).

## The matrix

Targets are rows, sources are columns. Three tabs of six columns share one list of rows (so the column count is
fixed and identical): **features** (level, attack, tonal, low, mid, high of the input), **sidechain** (the same six
from the sidechain input), **generators** (lfo 1–3, env 1–3). The encoder adds a **region** tab with one column. A row
modulated only on another tab still appears, with a marker showing where its modulation lives.

- **One row is always there, to show what a row is:** `speed` in the encoder. Every other target is a touch away.
- **Touching a modulatable value opens its row, dashed.** The next touch replaces it; it becomes permanent as soon as
  a cell in it has depth. Clicking a value that takes no modulation (direction, mode) steps it instead.
- **A row is not a claim that anything is routed.** The underline on a control and the count above the matrix both
  mean *driven by something*; an untouched instance reads "0 targets".
- **A cell is a bar that fills from the middle**, left for negative, right for positive, with the depth written in it;
  dragging is vertical (up for more), a click switches it on or off. Zero is no fill and removes the cell.
- **A column head shows what its source is sending**: a thin live bar in the source's own range (0…1 for a feature or
  an envelope, centred for an LFO), above the thicker amount bar. An idle envelope reads empty, a running LFO sweeps.
  The value comes from the per-frame section of the link bus, so it works for another instance too.
- **The matrix scrolls its rows under its column heads**, so a depth never sits in a column you can no longer name.
- The global amount sits in the matrix's footer.

## A source's settings

Clicking a column's name opens that source in the right-hand panel as a **temporary fourth tab** named for it
(`lfo 1`, `env 2`, `sc attack`, `region`). Clicking the name again closes it, as does choosing any tab; prev and next
chevrons step to the neighbouring source without going back to the matrix. The open column's name is in the plugin's
colour. Every page shows the source's **live output** under its selector. The parameters on a page stay assignable
where they are targets.

| source | page |
|---|---|
| **feature** | live output, amount, and for level a `release` tile (20 ms…2 s, default 200 ms; the input and the sidechain each have their own; it is the release of whatever level drives, ducking first). Under *calibration*, the detector's fixed constants, read-only from the detector's own configuration so the text cannot drift from what runs |
| **LFO** | one flat page, see below |
| **envelope** | two sub-tabs, *shape* and *trigger*, see below |
| **region** | the region editor's tabs |

Every page has one main selector first, with no title above it; no tile spans the full width.

### LFO

Order: wave selector (sine, triangle, saw, s&h), the wave preview, live output, **timing**, **polarity**.

- **Free or synced draws both timings**, the unused one greyed and inert, never replaced. Free reads `rate`
  (0.01…2 Hz, skewed so the default 0.25 Hz sits at mid-travel); synced reads `division` (8/1 to 1/16, each followed
  by its triplet, sixteen values, default 2/1, which is 0.25 Hz at 120 bpm). The host, the editor and an edit from
  another instance mean the same rate by the same position. `phase` sits with rate: both say where in time the wave is.
- **The drawn wave is the played wave.** `lfoShapeValue` is the one definition of each shape; sample-and-hold draws
  illustrative steps because the played draw is seeded. LFO seeds are fixed per LFO and never shown, so a bounce is
  repeatable.
- **Polarity** (0…1) replaces independent amplitude and offset: the ceiling is pinned at 1 and only the floor sweeps
  from 0 (unipolar) to −1 (fully bipolar), `offset = −polarity`, `amplitude = 1 + polarity`. A source can therefore
  never leave [−1, 1], so a cell depth of 1 means full deflection on every source. The default is bipolar for an LFO
  and unipolar for an envelope. The drawn shape never moves with polarity; only the axis labels do.

**Restart or continue** is one switch per LFO and one for the motion, separate from free and synced:

| | restart (default), linked to playback | continue |
|---|---|---|
| free | holds while stopped; starts from zero on play, locate and loop | runs all the time, never jumps |
| synced | holds while stopped; follows the song position, so it lines up with bars | runs at the tempo's rate, not tied to bars |
| motion | holds while stopped; the source starts from its path's beginning, the placement's turn from zero | both run all the time |

Continue means never restart from the transport. Preparing, loading and an offline render's start restart everything.
When continue is on, the LFO page and the motion group say that a bounce will not repeat what was heard.

### Envelope

**Shape** holds the graph, attack, decay, sustain, release, amount and polarity; **trigger** holds what sets it off.
The two sub-tabs are a segmented choice (black block), separate from the panel's own tab row.

**Times and levels.** Attack 0.1 ms…2 s, decay up to 5 s, release up to 8 s, sustain 0…1 (a level, not decibels).
Envelopes are matrix sources in [0, 1] before polarity. Feature envelopes (the ballistics inside level and attack)
are a different thing: always running, never triggered.

**The graph** draws one system with the LFO's plot: the same nice-tick grid (`drawNiceGrid`, 4 ticks for the envelope,
3 for the LFO's 56 px canvas), zero emphasised, the LFO's cycle boundaries standing in for the stage edges. Both curves
are drawn in `output` (the source's produced signal, the plugin's colour), the same role as the live bar above them.

- **Time axis is a square root.** Stages from 0.1 ms to 8 s cannot share a linear axis; each stage is as wide as the
  square root of its time, so a longer one is still wider and every one is visible.
- **Sustain is a hold.** The engine holds at the sustain level until the gate closes. The static preview draws a
  fixed 300 ms plateau (`kHeldSustainMs`) as a light fill, not a stroked segment, so it reads as a preview
  convention; a one-shot has no plateau.
- **Curves keep the labelled time.** Attack, decay and release each carry one `curve`, −1…1, through
  `ease(u, k) = (1 − e^(−k·u)) / (1 − e^(−k))`: `k > 0` starts fast, `k < 0` starts slow, `k → 0` is linear, and
  `ease(1, k) = 1` for every `k`, so a stage reaches its target at exactly its printed time. A curve bows a stage and
  never changes how long it lasts. `envelopeCurve()` samples the function the engine steps through, so the drawn
  shape is the played shape.
- **Dragging.** A point sets that stage's time (and at the decay corner the sustain level); the body of a segment bows
  its curve. A rising and a falling segment bow in opposite senses for the same drag direction. The axis scale is
  frozen while a drag is held and only ever grows, and every drag target is solved by inverting the drawing each
  frame, so a point never detaches from the pointer. The bow handles are `handle`, ink with the held one in the
  `touched` grey.
- **The y axis** is round displayed values first (0, 0.5, 1), each tick's pixel row solved from the current polarity,
  so the whole grid moves as one system; a zero line is drawn heavier when the range crosses zero.

**Trigger.** *midi* (`held` or `one-shot`, note range, channel, velocity amount, learn) or *audio* (source,
threshold, hysteresis). MIDI and audio blocks are both always drawn, the one not in use greyed. An audio trigger
listens to a feature of either input, never to an LFO or another envelope. The release point is
`threshold − hysteresis` and hysteresis never exceeds the threshold, since below zero no feature reads and a fired
envelope would never let go; the setters clamp it. **Learn** takes the next note-on on any channel, sets the range
to that note and the channel to its channel, and disarms; the audio thread only catches the note and the editor makes
the edit, so learning undoes like any change. Switching source, tab or trigger input disarms it.

## Panel height

The panel's content scrolls under its tab bar. An LFO page fits the panel with room to spare; an envelope's two
sub-tabs each fit on their own, which is the reason they are sub-tabs. See [LOOK.md](../suite/LOOK.md) for the
scrolling rule.


