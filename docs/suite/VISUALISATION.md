# The scene — shared by every plugin

What every bambi plugin draws on the sphere, and how. One component, used by all of them; a plugin adds what
only it can show. Geometry and interaction rules are in `core/include/bambi/scene/` (headless, tested); drawing is in
`ui/` (`SceneStrip`, `EnergyLayer`, `RegionOverlay`, `ProbeView`). The look is [LOOK.md](LOOK.md); the encoder's
reasoning for each interaction is in [encoder/UI.md](../encoder/UI.md).

## One component, N projections

A projection is `forward(vec3) -> {x, y, depth}` and `inverse(x, y) -> vec3 | null` in normalised [−1, 1] space.
Drawing, hit-testing, dragging and hover sync are written once against that interface.

| view | what it is | editable |
|---|---|---|
| **globe** | orthographic, orbitable; presets `top`, `front`, `side` set its camera | yes |
| **equirect** | azimuth × elevation, flat, left to right back · left · front · right · back; stretched to its frame | yes, and the only place elevation is edited |

The globe's top preset is orthographic (radius = cos elevation): an azimuthal equidistant view would put the horizon
at half radius and spend three quarters of the disc on elevation nobody reads there. The listener's left is screen
left in both. **The graticule lives in world space as open polylines**, defined once as unit vectors and drawn through
whichever projection, so no projection special-cases a seam.

### Interaction

- **Press-and-drag splits from click on a movement threshold, not on where the press landed:** 4 px
  (`kClickSlopPixels`).
- **A rotatable view reserves drag for the camera** and adds on release; **a flat view has no camera**, so a press
  can add and position in one gesture.
- **Nothing is dragged that is not free to move.** In the encoder a source is on its trajectory, so dragging orbits
  the view. The same rule governs anything an effect draws.
- **The arc-length LUT is the only per-frame array** (at least 1,024 points, `kMinLutSize`): it serves drawing,
  nearest-point and exact insertion, so the frame cost does not depend on node count. The software renderer is far
  less forgiving than canvas.
- The scene's corner holds `regions always`, the energy switch and, where a view has it, the `decoder` toggle with
  its list. These are window state (`SceneViewState`), never a parameter and never saved.

## The energy visualiser

Every plugin has one, the encoder included. It shows **energy by direction**, on the globe and the equirect through
the same projections as everything else, by one layer every scene view holds (`EnergyLayer`; the data and its
constants are `core/include/bambi/scene/energy.hpp`).

**Orange is what arrived and blue what the plugin added, never mixed:** where what arrived would be drawn by itself
it wins, and a texel is one or the other. An encoder has nothing arriving, so its picture is all blue under its
orange source, and shows what position and width have become at the order the track renders at.

- **Off when a window opens, and not remembered.** It is the one thing in a window that costs, to draw and to
  gather for. **While it is off the audio thread gathers nothing**, and it never gathers during an offline render.
- **It is the energy of the instance the window shows.** Showing another, the window asks it over the link bus;
  that instance gathers only while somebody keeps asking (the ask lapses a second after the last) and sends its
  covariance pair cut to order 3. The window that looks does the drawing.
- **The data is a small per-block summary**, the channel covariance, accumulated on the audio thread
  (`CovarianceWindow`, double precision, no allocation after `prepare`) and handed to the editor as a snapshot. The
  picture is built on the message thread: a 128 × 64 equirect texture, each texel read through a max-rE beam as a
  decoder would. The audio thread never draws, allocates or blocks.
- **Range:** an absolute scale. A texel starts to show at −42 dB under a full-scale point source's peak and is fully
  drawn at −3 dB; between them brightness rises smoothly, and nothing is scaled against the loudest source. The
  picture stops being painted 1.6 s after the last covariance.

### The two colours

| | in (orange) | added (blue) |
|---|---|---|
| **Encoder** | the source position; the input is a mono or stereo signal with a place | the energy blob the encode produces |
| **Echo** | the incoming field's energy | the taps, where they land |
| **Reverb** | the incoming field's energy | the reflections and the tail |

Orange is what the plugin was given; blue is what it did. Both are named roles in `Theme.h` (`colour::in`,
`colour::added`), changed in one place for every plugin. The suite reads the same way: the encoder, where sound comes
in, wears orange as its own colour, and the decoder, where it goes out, blue. What is live on the controls is grey, so
it never meets either.

### Regions over the energy

A region is drawn **in ink**, so colour stays with *in* and *added*.

| part | how it is drawn |
|---|---|
| edge | the 0.5 contour, in ink, 1.3 px |
| soft edge | an ink wash at 0.18 alpha, under the energy |
| limit lines, hatching | none |
| another instance's region | silver, dotted (dash 1, 3), edge only, label on hover only |

| the region's tab | what shows | the energy |
|---|---|---|
| closed | edge and role label | full |
| open | edge, wash and handles | ×0.5 |

- **`regions always`** is a toggle in the scene corner, on by default. Off shows only the region whose tab is
  open. It defaults on because, with it off, the blue is still shaped by a region nobody can see.
- **Why ×0.5:** against an energy peak the wash measures ΔL\* 7.4 with the energy at ×0.5 and 1.6 undimmed, at a cost
  of about 4 dB of visible range. At ×0.35 the orange starts to go.
- **The orange is never masked by a send:** the field passes through, and masking is not a send.
- **The blue already carries the send and the return**, because it is measured after them; a region's effect on the
  output shows with no drawing code.
- **A truthful mask would not match the edge:** energy goes as gain², so a mask drawn from it would end at the outer
  limit line and not at the 0.5 contour, a second shape beside the edge. It is not drawn.

## The probe

**A shared component** (`ProbeView`). Click a direction on the globe or the equirect, and the plugin draws what it
would do to whatever is there, **computed from its parameters, never from audio**, so it costs the audio thread
nothing. A plugin supplies marks (where each thing arrives from, its weight, the patch of sky it arrives from) and
traces (curves with a per-point fade); everything else — graticule, energy, the probe and its interaction (a click
places it, a drag orbits) — is shared.

| plugin | its answer |
|---|---|
| **Echo** | every enabled tap's spiral from that direction: a bead a pass, at that pass's level and blur |
| **Reverb** | the early reflections that direction would produce, each where it arrives from, at its weight |

- **A mark's weight is drawn as opacity**; a spread is a soft sprite drawn fainter as it widens, so the quiet late
  passes are not all that is left.
- **The answer is the plugin's own parts in the plugin's own colour:** Reverb's reflections violet; Echo's selected
  tap sage, as its row in the strip is, the other taps grey. **The probe itself, the direction in, is the incoming
  orange**, a small dot.
- **An indicator, not energy.** The measured energy is the separate blue wash, and a plugin shows both at once.
- **The probe, not a source.** A bus effect has no idea where anything stands; what it can always answer is what it
  would do to a direction it is pointed at.
- Echo's probe is what is called the *ping*. It ships.

## The playback layout: an overlay from the decoder

Every plugin's scene can show the loudspeakers the mix will be played on, so a source is placed against the layout it
will be heard on.

- **The decoder publishes what a scene needs** on the link bus: its layout while it plays one (each loudspeaker's
  direction, real or imaginary), what it plays, and where its head looks as it resolved it, at scene rate. With
  nothing playing, or no decoder in the session, the overlay is absent, not empty.
- **Drawn once, in the shared scene**, through every projection like the graticule; a plugin adds nothing.
- **A view setting, off by default:** the `decoder` toggle on the globe's strip (and on the equirect's while it has
  the row), beside the energy switch. Not a parameter: the overlay never reaches the audio.
- **What an overlay draws is switched item by item:** the corner toggle is the master, and a small triangle beside it
  opens a list with a box for each instance on the bus (a decoder's speakers and head are two rows under it). With
  several decoders each is its own row. The model is `core/include/bambi/scene/overlay.hpp` (`OverlaySelection`), the
  menu `ui/OverlayMenu`; a newcomer takes the overlay's default, and an instance that leaves takes its choice with it.
  The choices are the window's own and never saved.
- **The decoder shows the encoders' sources** the same way, as every plugin's scene shows the session's instances.

Background: decisions 0015.
