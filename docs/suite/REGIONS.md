# Regions

A **region** says where on the sphere something happens: which part of the field an echo is sent, where a
reverb's tail returns, what the encoder's source passes through. The concept is shared by every plugin; **each
region itself belongs to one plugin, is fixed in number, automatable, and never shared between instances** — no
instance's audio depends on another instance's region. The reference prototypes are
`prototypes/regions/shapes` (shapes, softness, aim, custom) and
`prototypes/regions/editor` (the editor and how a region is drawn).
Code: `core/include/bambi/region/`, the key block in `patch/blocks.hpp`, the editor in `ui/` (`Region*.h`).

## The concept

**A region is a set of weights on the spherical harmonics, plus an orientation.**

- The weights are the field `g(direction)` up to the bus order, in the region's **own frame**. Where `g` is 1 the
  sound passes, where it is 0 it does not; between is a fade.
- Applied to an ambisonic signal, a region is a **gain matrix** on the SH channels:
  `M_ij = (2n_j+1)/4π ∫ g Y_i Y_j dΩ` — the `(2n_j+1)` factor belongs to the column. **Inside** is `M`,
  **outside** is `I − M`, exactly.
- **Inside or outside, and how much, belong to the use, not the region.** `Region::side` is the evaluation
  struct's field; in a plugin the side is a host parameter beside the shape (`regionN.side`, a choice,
  automatable, not a matrix target) and the amount is the matrix amount of the region source.
- **Linear: no clip, no clamp.** The region on the bus is exactly its weights; clamping to 0…1 would itself be a
  clip and make the field non-band-limited. Truncation ringing leaves 0…1 at the edges — "front" at order 3 spans about −0.2…1.2. Below 0 the
  sound from that direction is phase-inverted, above 1 boosted. The scene draws a region without hatching.
- **In the encoder a region is exact**: `g(direction)` is evaluated directly, so there is no ringing, no order
  ceiling, no "needs order K" and no matrix to rebuild (see the encoder below).

## Kinds and shapes

The kind row is `everywhere · spot · band · sectors · dots · clouds · custom`. There are no named presets
(*front*, *halo*); a shape's own parameters are what it is. Every shape is authored pointing up, which keeps its
weights sparse; the orientation does the pointing.

| kind | parameters | weights sit in | lowest useful order |
|---|---|---|---|
| **everywhere** | none | the omni channel | 0 |
| **spot** | size (half-angle) | the centre column (m = 0) | 1 |
| **band** | elevation, thickness | the centre column | 1–2 |
| **sectors** | count (1…32), fill | degrees m that are multiples of the count | the count |
| **dots** | count (2, 4, 6, 8, 12, 20: corners of a regular solid), size | the solid's symmetric harmonics | 6 and 8 dots: 4; 12 dots: 6 |
| **clouds** | seed, coverage, contrast, detail, evolve | everywhere, falling off with order | 1 |
| **custom** | the weights themselves | the pyramid | the order it was converted at |

- **Counts and seed are state**, set once; everything else is a host parameter. A shape that needs more order than
  the bus has flattens to its average, and the UI says *this shape needs order K*. The finest edge an order can
  draw is about 180°/(N+1): 45° at order 3.
- **Softness** is one control for every shape, in degrees. On the bus it is one gain per order, a cap blur
  (`math/legendre.hpp` `capWeights`): linear and free. The scalar `valueAt` uses the same blur in closed form —
  a disc of radius `softness/2` against a straight edge, `(acos t − t·√(1−t²))/π`, `t = −d/(softness/2)` — so
  both mean the same width, 0.5 on the edge and the same S-curve. Against a spherical quadrature it is off by at most 0.018 for a 45° spot blurred by 20° and 0.07 for a
  15° one.
- **Clouds** are random weights falling off with order: *detail* sets the fall-off, *coverage* is the average,
  *contrast* is twice the field's standard deviation, *evolve* turns between two perpendicular patterns and
  wraps at a whole turn. Each order's random vector is scaled to exactly its expected size, so the spread is
  exactly 0.3 for contrast 0.6 at every evolve angle, order and seed (tested to 1e-9). The draw is the
  prototype's mulberry32 and Box–Muller. Neither clouds nor custom has softness.
- **Custom** is a flag beside the kind (`RegionShape::custom`): **convert to custom** integrates the shape at its
  zero pose against each harmonic (`projectWeights`) and freezes the weights into a **pyramid** (order down the
  rows, degree across, up to the order the plugin renders at). A gain per order sits at the right of each row;
  `resolveRegion` plays weight × gain. The shape and its parameters stay underneath, so picking the shape again
  shows them as they were. Converting again replaces the custom. Custom weights are static (not modulated); the
  orientation and its rates still turn them.

## Frame and aim

**Orientation is yaw · pitch · roll** — the same rotation as a trajectory's placement: roll about the region's
own axis, then pitch about +y, then yaw about +z. Three angles, because an aim direction alone leaves a sectors
or dots pattern's roll undefined and mirrors it at straight down. **At zero, each kind sits the way it is
naturally used:**

| kind | axis at zero | own x at zero |
|---|---|---|
| spot, everywhere | front | up (a fixed, exact axis permutation) |
| band, sectors, dots | up, as authored | front (sector 0 is centred on front) |
| clouds, custom | the world frame | none; weights are fixed in the world frame |

- A **band at zero is the horizon**, and its elevation is a world elevation. **Four dots at zero are the A-format
  tetrahedron** (FLU, FRD, BLD, BRU); two dots are up and down.
- A natural axis lies in the x–z plane, because pitch is about y. Converting between spot and an upright kind
  moves the aim 90° while yaw, pitch and roll keep their values.
- `aimOf` gives the axis's world direction. **`aimAt` carries the pattern along the shortest arc**: it takes the
  yaw/pitch solution nearer the present orientation and rewrites roll where roll can be seen (sectors, and dots
  other than two). Yaw is kept at the poles. `aimOnMap` reads the aim off the angles, so it stays defined at a
  pole.

## How a plugin holds and applies a region

| what | where it lives |
|---|---|
| what the region **is**: kind, sector count, dot count, seed, custom flag, weights, gains | state, sparse and by name: `"regions": {"region1": {...}}`; a spot, the default shape, writes no entry (a new Echo starts everywhere, which it writes) |
| what it is **set to**, and its side | host parameters `region1.*` (key block below) |
| **amount** | `mod.amount.regionN`, one per slot |

- `resolveRegion(shape, side, settings, turn…)` returns the `Region` that `valueAt` reads. It knows no parameter
  ids, holds dot size below touching, and gives a setting that is not a number its default. `sanitised` holds
  sectors to 1…32 and makes a dot count that is no regular solid 6, so anything loads as something that can be
  evaluated. A document naming a slot the plugin does not have is ignored.
- **One macro stamps the key block in every plugin**, `BAMBI_REGION_BLOCK`: `yaw`, `pitch`, `roll`, `yaw_rate`,
  `pitch_rate`, `roll_rate`, `softness`, `size`, `band_el`, `thickness`, `fill`, `dot_size`, `side`, `coverage`,
  `contrast`, `detail`, `evolve`. Keys are by position (`region1`, `region2`), display names by role (*send
  size*, *return size*). Up to four slots exist (`kMaxRegions`).

| plugin | regions | use |
|---|---|---|
| **Encoder** | 1 | a modulation source (below) |
| **Echo** | 1 | send: `M` on the way in |
| **Reverb** | 2 | send and return |

A **send** decides which part of the incoming field enters the effect; a **return** decides where what the
effect produced is put back. Each is a region applied at a point in the chain, and the inside/outside split gets
`I − M` for free. Reverb reduces the field to twelve virtual sources and K lines before either region is
reached, so its send and return are **28 scalars**, exact for every kind; only the reflections' half of the
return goes back over the whole bus through the projection.

### The encoder: a region costs a scalar

One source has one direction, so the encoder evaluates `g(direction)` where the source is. The region is a
**modulation source** (slot 18, its own matrix tab `MatrixTab::Region`, one column, amount `mod.amount.region1`),
read by the matrix like a feature: *slower inside this region* is a `Rate` destination, *quieter* a
`DirectScalar`, one row each. Weights kinds are clamped to 0…1 here.

- **The cycle is broken by delay.** The source's direction comes from the trajectory, which the matrix drives,
  and the matrix holds a row reading that direction. `EncoderControl::step` evaluates the region at the position
  the **previous** control step produced (256 samples, 5.3 ms): inaudible, bounded and deterministic, so a bounce
  matches. The region itself is read from the destinations of that step plus its own rates' turn.
- **When continuity has just broken** (play, locate, loop, a loaded state) there is no previous step: the
  position is worked out from the parameters as set with the clocks as they stand, so a source parked inside a
  region does not fade in at every play.
- The plugin and `bambi-render` share the one control step; renders are bit-identical at block sizes 1…1024.

### Echo, Reverb: the same region elsewhere

A region travels between plugins and tracks by **copy and paste through the system clipboard** (`RegionClip`):
an action, nothing follows afterwards. It carries the definition by field (so `yaw` copied from Echo's send is
`yaw` on Reverb's return) and the matrix rows driving it **from LFOs set to restart**, each with its LFO's
settings and amount, keeping the LFO's index (the sample-and-hold seed). It never carries side, amount, rows
from envelopes or continue-mode LFOs, or a rate's accumulated turn. A host automation lane copied between
tracks drives a second plugin's region the same way.

Other instances' regions are **drawn** in the scene, silver and dotted, from `LinkRegionShape` in the link's
dynamic section: the region as its owner's audio thread resolved it (turn and modulation included, weights and
seed carried), no side, no amount. Display only; nothing in it reaches audio. The patch's own region record
(`LinkRegion`) carries kind and counts only.

## Host parameters and matrix targets

Every continuous setting is a host parameter with the same keys, names and ranges in every plugin, and the
matrix reaches most of them (`sharedDestKind`):

| matrix target | kind |
|---|---|
| yaw, pitch, roll; clouds evolve | `DirectAngle` (wraps) |
| yaw rate, pitch rate, roll rate | `Rate` |
| size, softness, band elevation, thickness, fill, dot size, coverage, contrast, detail | `DirectScalar` |
| kind, counts, seed, custom weights, side | not targets |

- Host automation follows the piece ("at bar 64 the send narrows"); the matrix follows the content (a region
  that opens with Level).
- **A rate on each axis.** All angles wrap; the accumulated turn is reset, held and continued exactly as the
  placement's is, in every plugin, by one key, `rates.retrigger` (the `rates continue` switch in every
  window's footer). The turn resets with the transport, so a bounce repeats from the same start.
- Kind, counts and seed are set once: they change what the region *is*.
- On the bus the matrix is recomputed once per 256-sample hop and its entries are **interpolated per sample across
  the hop**, so a moving region does not step.

## What a region costs

Release, arm64, 128-sample block at 48 kHz unless a row says a hop (256 samples, 5.333 ms); in benchmark code over
`bambi-core`'s maths, not a plugin build.

| | order 3 | order 7 |
|---|---|---|
| a dense (N+1)⁴ region applied, per block | 0.071 % of a core | 1.171 % |
| block-diagonal SH rotation applied, per block | 0.025 % | 0.190 % |
| matrix rebuilt by quadrature, per hop | 44.5 µs (0.83 % of a hop) | 3 127 µs (58.6 %) |
| matrix from N+1 per-order parts, per hop | 0.083 µs | 2.43 µs |
| matrix from (N+1)² parts, per hop | 0.225 µs | 18.3 µs (1.0 MB of parts) |
| `valueAt`, one direction (the encoder's whole cost) | 25.9 ns | 25.9 ns |

- The dense apply vectorises (~17 GMAC/s at both orders).
- **Resizing on the bus is cheap if and only if the matrix is stored decomposed by order.** Softness, spot size
  and dot size are cap tapers per order and exact in the N+1 form; band thickness and sector fill need the
  (N+1)² form, still 170× faster than quadrature at order 7.
- **Orientation:** the matrix stays in the region's own frame and the *signal* is rotated in and out.
- **Every kind is rebuilt in the control step** (dense kinds contract the region's harmonics to order 2N with
  Gaunt's integrals), paid only by a step whose shape moved; a turn pays the rotation alone:

  | per hop | order 1 | order 3 | order 7 | order 10 |
  |---|---|---|---|---|
  | sectors | 6 µs | 12 µs | 51 µs (0.96 % of a core) | 190 µs (3.6 %) |
  | 20 dots | 2 µs | 4 µs | 28 µs (0.52 %) | 104 µs (2.0 %) |
  | clouds, custom | 0.7 µs | 1.2 µs | 11 µs (0.21 %) | 35 µs (0.65 %) |

  `prepare` builds Gaunt's table and the dot sets once: 1.1 ms at order 3, 26 ms at order 7, about 90 ms at
  order 10.

## The editor

**One component** (`ui/RegionEditor`) is the encoder's region source page and an effect's slot tab; the scene
half is `ui/RegionOverlay`, and how a region is drawn (ink edge and wash, other instances dotted) is
[VISUALISATION](VISUALISATION.md). The encoder's page is opened from the region matrix tab, offered only where
the region is a source (`regionIsSource`).

- **Handles** on the globe and the equirect: aim (calls `aimAt`), roll (on a stem 45° from the axis) and edge.
  A held handle is re-applied every frame so a turning region does not slip from under a still pointer. Hit
  radii are 13 px for the aim, 12 px for the others. A handle is written through the
  slot's parameters with the rate's turn subtracted; the editor draws from the turn the audio thread
  publishes. Handles show only while the region's page is open, and the encoder's scene drives its region with
  the same handles (`RegionGrip`). Double-click returns a handle to its default.
- **Sub-tabs** (segments, as the envelope's): **shape** — the kind, its settings or the pyramid, convert to
  custom, copy · paste; **transform** — each angle beside its rate, then the use (side and amount).
- **`kind`** is one row wrapped at four a line, the shared picker; `custom` is greyed until weights exist.
- **Greyed controls:** roll for spot, band and two dots; all orientation tiles for everywhere. Convert to custom
  sits under the shape's tiles, greyed for everywhere.
- **Matrix row names:** `region yaw` in the encoder; `send yaw`, `return size` in an effect.
- **Copy · paste** are two buttons at the bottom of the shape sub-tab; paste greys on a clipboard that is not a
  current region, and a line under the buttons says what it holds (*band, 78 % soft, 1 lfo row*).


