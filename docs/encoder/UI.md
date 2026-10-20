# The Encoder's window

One window shows every instance of the Encoder in a session; select any and edit it. The look and the control rules
every plugin shares are [LOOK.md](../suite/LOOK.md); what the parameters do is [ENCODER.md](ENCODER.md); sources and
the matrix are [ENCODER-SOURCES.md](ENCODER-SOURCES.md). The built UI is the reference: `plugins/encoder/Source/UI/`
and the shared `ui/`.

## Layout

The window is designed at **1020 × 600** and scaled uniformly by the shared editor; proportions are designed, not
responsive (`metrics::windowWidth`, `windowHeight`).

| area | holds |
|---|---|
| header (44 px) | wordmark and plugin name, undo and redo, the instances as tabs, preset, link status, settings |
| left column, top | the scene: globe and equirect, two 286 px square panels |
| left column, bottom | the modulation matrix, with its global amount under it |
| right column | the tab bar, the page, and the footer under it |
| level column, far right | the gain over the output meter |

Everything is visible at once except the page: only the right column's tabs change. The matrix, the scene and the
instance tabs are never behind a tab, so the assign flow (click a value, click a source's cell) is two clicks in two
halves of one window.

A page opens from its main selector, which comes first with no title or description above it. **Settings** is not a
tab: it opens from the header's glyph over the scene, the matrix and the tabs, with the instance's name, the link
session, the order and the host's reported buses; the footer and the level column stay in front.

## Instances

**The instances are the header's tabs.** The tabs fill whatever middle the header has left (computed during paint,
since the right cluster's width changes with the link status text). The selected tab is a black block standing on the
header's rule, which is the plugin's own colour at 2 px, running on unbroken beneath it.

- **Overflow.** A session holds up to 64 instances. Tabs shrink to 62 px, below which they scroll: the wheel scrolls
  them, and a selection made anywhere else (the scene, another window, an instance closing) scrolls itself into view.
  A scroll the user made is left alone. Chevrons at each end appear only while tabs overflow and grey at the end with
  nothing past it; both ends are reserved together so tabs do not shuffle sideways as one appears.
- **The header repaints when what it would draw no longer matches what it drew** (`Header::refresh()` compares the
  selection, names and count with the last paint), because nothing else repaints it.
- **Selecting another instance turns every control to it:** the matrix, the three tabs, a source's settings, undo and
  redo. The footer names the instance the controls show. Controls talk to a `ControlTarget`, this instance or another
  through the link bus, and nothing in a control is remote-specific.
- **Edits show at once**: the editing window shows its own edit until the other instance reports having applied it.
- **Undo is the edited instance's**, through its own history; a drag from another window is one step there.
- **Switching instance ends what was open** on the one being left: a learn, a drag, the provisional row.
- **The output meter stays this window's** (its level comes from this audio thread); **the level column edits the
  instance shown**.
- A source's settings show the other instance's live output from the per-frame section of the bus.
- Clicking the header's icon or name returns the window to its own instance.

## Scene

Two views of the sphere: the **globe** (top, front and side presets and a free camera) and the **equirect** (stretched
to a square so elevation gets the pixels), with a strip of view switches over each. Orange is what came in, blue is
what the plugin adds ([VISUALISATION](../suite/VISUALISATION.md)).

- **Drag orbits the globe; the equirect does not orbit.** A source sits on its trajectory and is never dragged.
- **Selecting.** Click a dot or any part of a path to select its instance; a hovered dot shows its name; hit radii are
  generous and only what faces you on the globe can be hit.
- **Each instance draws its path and source**, other instances in silver. Only the selected instance's source is
  drawn with its **width**: a low-opacity fill in the source's orange, the rendered width (modulation included) as a
  true cap on the sphere (`capCoverage`), fainter on the far side of the globe, wrapping across the equirect's seam.
- **What the engine plays is drawn beside what is set.** The selected path and the region are drawn as set; where the
  engine has them elsewhere (a setting modulated, automated or still gliding) a thin outline in the live colour shows
  where they are now. Nothing moving it, nothing is drawn twice.
- **Editing a custom chain**, in both views, on the trajectory tab: click a node to select it, drag it (its handles
  ride along); drag a handle to bend, alt-drag first to break the symmetry and make the node a corner; click the curve
  to insert a node exactly there and drag it in the same gesture; shift-click a node to delete it. Hit radii are 13 px
  (node), 12 px (handle), 14 px (insert). Nodes are edited where they are drawn, through the placement transform and
  its inverse, and editing is withdrawn where extent has collapsed the path onto its centre. The two-node floor and
  the 24-node cap are said in the scene for a moment. A drag is one undo step, and editing another instance's path
  needs no code of its own.

## The right column

Three tabs, each one job, plus the temporary source tab. Every value is a tile ([LOOK.md](../suite/LOOK.md)) and a
click on a modulatable one opens its matrix row.

| tab | acts on | holds |
|---|---|---|
| **trajectory** | the path's shape | the parametric · custom switch; for parametric: closed or open, the shape row, the shape's tiles, **convert to custom**; for custom: the selected node's panel (smooth or corner, delete, make start). A custom path draws neither the shape list nor the shape's parameters |
| **transform** | the path's orientation | yaw, pitch, roll each beside its rate, then extent |
| **source** | the moving source | *input* first (sum · mid/side · stereo, then spread, offset and input trim, the one not in use greyed), *motion* (displace, speed, direction, mode), *render* (width and its range) |

**Input first on source**, because it decides what "the source" is. On a mono track the choice is greyed and says "a
mono track has no side". The gain is not a tile: it is the level column's. `rates continue` is the footer's.

**The footer** (shared) carries on the left the switches that govern the whole instance (`rates continue`), and on the
right the instance shown, the order and `dsp`, the audio thread's share of a block's budget. What the window costs to
draw is the main thread's and is not in it. **The level column** (shared) holds the gain, moved like a tile and also
a matrix target, over the output meter.

**Live marks.** The placement's four values (yaw, pitch, roll, extent) and displace carry a second mark straddling the
bar where the engine has the value this frame, because a rate keeps turning an angle the parameter still reads as set.
A selected remote instance shows its drift through the same path, since the bus publishes the applied transform. A
mark is drawn only while something moves it. Speed, width and gain have none, since nothing publishes their applied
value. Double-clicking yaw, pitch or roll resets the angle and clears that axis's accumulated turn; double-clicking
displace clears the distance travelled; both reach another instance as link commands (`kLinkCommandZeroTurn`).

## Scrolling

The right column's page scrolls under its tab bar and the matrix scrolls its rows under its heads. Each painter
receives the full height it would have had and lays itself out whole; the viewport decides what shows, and regions
hit-test at the coordinates they are drawn at, so the drawing and the click targets cannot disagree. Every thing that
lays content out reports its bottom (a tile grid, a group title, a segment strip, a source's output meter, the
matrix rows), and where a painter draws straight through `drawTile` its caller takes the bottom from the return
value. The scroll position is clamped after every paint, so a panel that shrinks while scrolled comes back within
reach. At 600 px the three base matrix rows and a fourth fit without travel; `transform` and `source` fit; the
trajectory tab with the node editor open and an envelope's settings scroll.

## Links and bus

Every instance publishes its path, position, transform, width and per-frame values; commands travel the other way
(undo, redo, learn, zero turn, parameter edits). The bus is foundational: the scene needs every instance, so it ships
in every Encoder.

Background: decisions 0017, 0139.
