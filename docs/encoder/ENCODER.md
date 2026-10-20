# The Encoder

One mono or stereo source in, an ambisonic field of order N out (AmbiX: ACN, SN3D). The source sits on a
**trajectory**, is turned by a **transform**, moves along the path by **motion**, and is drawn into the field with a
**width**. Everything that moves can be driven by the **modulation matrix**. The window is
[UI.md](UI.md); a source's settings are [ENCODER-SOURCES.md](ENCODER-SOURCES.md); the look is
[LOOK.md](../suite/LOOK.md).

## Trajectory — the shape

Two kinds behind one position-at-`s` contract (`core/include/bambi/path/`):

| kind | authoring | automatable |
|---|---|---|
| **parametric** | a shape and its settings | yes: continuous settings are host parameters |
| **custom** | a Bézier chain: nodes, handles, insert and delete on the sphere | no: a node list is not a parameter |

**Shapes:** orbit, lissajous, wave, polygon, spiral. The shape declares whether the path is closed or open. Each
shape keeps its own values when another is chosen.

| shape | parameters (host-automatable) | state (whole numbers, they jump) |
|---|---|---|
| orbit | tilt, axis az, aperture | |
| lissajous | az amount, el amount, phase | az ratio, el ratio |
| wave | el amount, el offset | turns, wobbles |
| polygon | radius, rotation | sides |
| spiral | from el, to el, start az | turns |

**Custom paths.** A chain holds 2 to 24 nodes (`kMaxNodes`); a refused insert or delete is said in the scene. The
**parametric · custom** switch only chooses which path plays and never writes the chain. A chain comes from one place:
**convert to custom**, a button in the parametric view, which converts the shape as it is now, always replaces any
chain (the button says so), and switches to it as one undoable edit. With no chain, *custom* is greyed with "no
custom path yet". The node panel keeps only what has no gesture: smooth or corner, delete, make start. Node
positions and handle lengths are edited on the sphere, never automated.

**Modulating a shape.** A modulated shape setting rebuilds the path (arc-length table) in the control step, in room
reserved at construction, so nothing allocates. The source keeps its place as a fraction of the lap, so a shape that
changes smoothly carries the source smoothly. The rebuild runs only when a setting has moved since the last build.

## Transform — the orientation

Applied after the path is evaluated, so it never touches the arc-length table and costs nothing. Three rotations,
each with a rate beside it, and an extent:

| value | range | rate |
|---|---|---|
| yaw | ±180°, wraps | yaw rate, ±180 °/s |
| pitch | ±180°, wraps | pitch rate |
| roll | ±180°, wraps | roll rate |
| extent | 0…2 | |

Extent 0 collapses the path onto its own centre, 1 is the path as authored, 2 collapses a great circle onto the
point opposite its centre. A rate's integrated turn is added to the angle, never written into it: the number you
drag stays what automation records and undo restores, and a **live mark** on the tile shows where the engine has the
angle now. Transform works the same for parametric and custom paths, which is what shape modulation cannot do for a
custom one.

## Motion — position along the path

| value | meaning |
|---|---|
| speed | deg/s along the path, ±180; integrated |
| displace | 0…1, one lap: the place itself, added to speed's integral. On a loop 1 is 0 again; on an open path 0 and 1 are its ends |
| direction | forward, reverse |
| mode | wrap, ping-pong (default), once; displace past an end is folded the same way |

Speed and displace are the FM/PM pair. The source sits on its trajectory and is never dragged. **Restart or continue**
is one switch per LFO and one for the motion (`rates continue` in the footer, which governs the motion, the
placement's rates and the region's): *restart* links them to playback (hold while stopped, start from zero on play,
locate and loop); *continue* means never restart from the transport, so a bounce no longer repeats what was heard.
Preparing and an offline render's start always restart everything.

## Input modes

What a stereo input puts on the sphere (`input.mode`, set once, takes no modulation):

| mode | the source is |
|---|---|
| sum | one point |
| mid/side | one point with the side signal spread about it by `spread` (0…180°), the side as a difference so it survives a mono fold |
| stereo | two points along the path, `offset` apart in laps (0…0.5), each trailing or leading the source in phase whichever way the motion runs; the signal keeps one point's level in total |

Spread and offset are both on screen whatever the mode, the one not in use greyed. A mono track has no side and is
always `sum`. The sidechain is a second input that only feeds detectors. **Input trim** (±24 dB) is applied before the
detectors.

## Rendering

- **Width** 0…180°: the angular radius of a spherical cap, weighted per order. 90 is a hemisphere, 180 the whole
  sphere. A **width range** (min, max) bounds what modulation can reach. The scene draws the selected source's
  width as a low-opacity cap, modulation included.
- **Gain** −60…+12 dB, in the level column.
- The path of a point is rendered with per-sample gain ramps, so a moving source does not click and the output does
  not depend on the host's block size.

## Modulation

One matrix, sources as columns, destinations as rows ([MODULATION](../suite/MODULATION.md)).

| source group | members |
|---|---|
| features of the input and of the sidechain | level, attack, tonal, low, mid, high |
| generators | 3 LFOs, 3 envelopes |
| region | the encoder's one region, read where the source is |

| destination group | destinations | smoothing |
|---|---|---|
| motion | speed, displace | rate-limited |
| transform | yaw, pitch, roll, their three rates, extent | angles rate-limited, rates smoothed |
| render | width, gain, input trim, spread, offset | smoothed |
| shape | a parametric shape's continuous settings | by kind; angles rate-limited |
| region | the region's own settings | by kind |

Not modulatable: direction, mode, the three restart switches, the width range, every LFO and envelope setting, and
node positions. Matrix cell depths are state, not parameters; automation rides one **amount** per source and one
global amount. Triggers for envelopes come from MIDI or from a feature, never from another generator.

Deliberately not built: modulation of modulation, per-cell response curves, spectral centroid or consonance as
user-facing sources, node-level modulation of a custom path, tempo detection from audio (the transport gives it).

## Scene and many instances

One window shows every instance. The link bus carries each instance's path, position, transform and width, and a
command channel: an edit in one window is a command that the target instance applies through **its own host
parameter** inside a gesture, so it stays automatable, undoable and recordable. Nothing writes another instance's
state directly. Selecting an instance in the scene or the header retargets every control; see [UI.md](UI.md).

## Parameters and state

Host parameters are the continuous macros: the transform and its rates, speed, displace, width and its range, gain,
input trim, spread, offset, a parametric shape's continuous settings, the LFO and envelope settings, the source
amounts and the region block. State is the trajectory kind, the shape and its whole-number settings, the custom
chain, the matrix cells, envelope triggers and instance identity. Keys are append-only (`core/tests/patch/frozen-parameters.txt`
enforces it); display names, ranges and defaults may change.

## What a user does

Each flow is short enough to be a vocabulary: two clicks and a drag is every modulation.

| goal | steps |
|---|---|
| the kick nudges the source on every hit | pick a short arc; click **displace**, which opens a dashed row; click **attack**, which makes a cell; drag the cell |
| the pad opens up while it rings | click **width**, then **tonal**, then drag: the same shape |
| find and edit another source | click its dot or header tab; every control retargets |
| the hats push the pad around | send the hats to the pad's sidechain; on the sidechain tab, `attack` → yaw |
| drift slowly, faster when loud | set speed's base, add level → speed: base and modulation sit side by side |
| undo a generate or reset | ⌘Z or the header's undo, one step per drag |
| ride depth from the arrangement | each source's amount is a host parameter (`mod.amount.lfo1`, …); the matrix shows which source it is |

What this requires of the window: the matrix is visible while clicking a parameter, clicking a modulatable value
opens its row, scene and header tabs are always visible, cells are compact bars with a number only while dragging.

Background: decisions 0001, 0017, 0111, 0129, 0139.
