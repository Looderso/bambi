# Decoder

The last plugin in the chain: ambisonics in, headphones or loudspeakers out. It sits once, on the master, and is
the one plugin whose output is not ambisonics. The mix is the work and the decoder is how it is heard: headphones,
a stereo pair and an array are served from the same scene, and moving from one to another changes the output, not
the session. It is not an effect and draws no "what it added"; it draws what comes out where.

Pages: [binaural, heads and the pair](DECODER-BINAURAL.md), [head tracking](DECODER-HEAD-TRACKING.md),
[HRTF databases and the head model](DECODER-HRTF.md), [filling a layout's gaps](DECODER-FILL.md).

## Outputs and the channel model

One ambisonic input (AmbiX, ACN / SN3D, at the order that fits the input bus), two outputs:

| output | for | engine |
|---|---|---|
| **Headphones** | the scene heard as a listener, head-tracked | binaural decode, MagLS ([binaural](DECODER-BINAURAL.md)) |
| **Speakers** | a stereo pair, a 5.1 / 7.1.4 room, a dome, a filter matrix | AllRAD decode of a layout; or a convolved matrix |

Every output is a set of channels on the bus the host gives. The bus is as wide as the host makes it (2 to 64) and
is never renegotiated. Headphones play on a pair, 1–2 by default and movable. A layout plays on its own channel
numbers, from its file, moved as a block to start on any channel.

- **Both play whenever their channels differ**: headphones on 13–14 and a 7.1.4 layout on 1–12 both play. Getting
  13–14 to a headphone amplifier is the interface's routing, not the plugin's.
- **Lit on the other output's channels**, an output moves to the first free channels after it while the bus has
  room; when it has not, the other stops and the page says so.
- **An output that needs more channels than the bus has does not play** and says how many it needs. A host that
  gives two channels runs headphones or a stereo pair.
- **A two-channel input is order 0.** A decoder inserted on REAPER's two-channel master reads W alone and plays it
  as mono from everywhere; the orientation strip says in words when the input holds no direction. The decoder
  belongs on the wide track with the rest of the chain.
- **Buses**: the field on one input bus, no sidechain, an output of any width from 2 to 64 that the input never
  moves. VST3 and CLAP; in CLAP the port configurations move the field's order and leave the output the bus it was.

## Latency

One latency for every path, reported to the host: **64 samples** (the convolver's partition; 1.3 ms at 48 kHz).
The array's feeds are held by that partition on top of their own delays, so binaural, matrix and speaker outputs
land together and the host is never told a latency that changes with what plays.

## Speakers: a layout, not a preset list

`designAllrad` (`core/include/bambi/decode/allrad.hpp`) takes a layout, an order and the weighting and returns the
matrix for AmbiX in, one row per output channel, with each loudspeaker's gain and delay from its distance.

- **AllRAD** over the layout, with **imaginary loudspeakers** filling the gaps of an incomplete layout. An
  imaginary loudspeaker takes part in the triangulation and its signal is dropped, never mixed into its
  neighbours (which would cut the effective order by about a third). A layout with nothing below the listener gets
  a point there, so a source below pans to the lowest ring instead of vanishing. Points within 10° of the horizon
  count as on it; a layout with every point there is a ring and is designed on 360 virtual loudspeakers.
- **Design**: loudspeakers sorted by channel; convex hull in double precision on the unit vectors (so a layout
  designs the same however it is listed); 5200 directions on a Fibonacci sphere panned by VBAP over the hull; the
  decoder is that panning sampled against the harmonics with the (2n+1) and order weights; the level is set on a
  second grid of 4000. Four or more loudspeakers on one plane (7.1.4's height square) are panned around their
  centre, whose gain they share. A layout whose hull does not hold the listener is refused with the reason.
- **Weighting is the listening situation**: *one listener* (default) decodes unweighted (basic); *audience*
  decodes max-rE, which widens the sweet area and costs the centred listener sharpness and treble. Both are
  energy-preserving: a source panned anywhere gives, averaged over all directions, the energy of one loudspeaker
  playing it, so switching weighting or layout keeps the level.
- **Distance**: per-loudspeaker delay and gain from its distance, switchable, so a room that is not a sphere still
  images.
- **The subwoofer channel** is skipped; bass management is not the decoder's.
- **A stereo pair** is two virtual cardioids, the field's zeroth and first order, a quarter turn to either side of
  the pair's middle in the pair's plane: a source on a loudspeaker's axis lands three to one on its side, one in
  the middle in both alike, one behind in both at the front's level. One loudspeaker is an omni. The pair's
  *fold* and *slice* designs are in [the binaural page](DECODER-BINAURAL.md#the-pair-fold-and-slice).
- **The gaps** a layout leaves can play binaurally on its loudspeakers through crosstalk cancellation, for one
  listener in the sweet spot: [filling a layout's gaps](DECODER-FILL.md).

Designing 64 loudspeakers takes 2 ms at order 1 and 6 ms at order 7 (Release); it runs on a worker.

### Layout formats

| source | notes |
|---|---|
| **IEM JSON** | the AllRADecoder's configuration, the format SPARTA imports and exports |
| **AmbDec `.ambdec`** | read: each `add_spkr` line's id, distance, azimuth, elevation; the JACK port's trailing number is the channel (none: the loudspeaker's position). The file's own matrices are not read, the decoder is designed here. One chooser takes both files and tells them apart by the first character |
| **ITU-R BS.2051** | built in, ten systems by name, channels in the recommendation's order, the LFE a gap: 0+2+0, 0+5+0, 2+5+0, 4+5+0, 4+7+0 (7.1.4), 4+9+0 (9.1.6) and on |
| **typed or pasted** | a table of azimuth, elevation, distance; a loaded layout is edited in the same table |
| **SOFA FIR-E filter matrix** | a decoder already designed as filters (a compact array's jconvolver set, made by `tools/matrix/jconvolver-to-sofa.py`) |

A layout comes from the session, not from a path: its loudspeakers are state. Checked against the IEM plug-in's own
matrix for a 16-loudspeaker array (within 0.02 %) and for symmetry on 7.1.4.

### A decoder as filters

`MatrixConvolver` is the binaural convolver generalised to any outputs, in the same 64-sample partitions. The SOFA
file holds the inputs as emitters and the drivers as receivers, cut to the taps that hold the energy, with the
input's order, channel order and normalisation as attributes; filters are resampled to the host's rate on load
(`dsp/resample.hpp`, a windowed sinc). The file is read again from its path on open, not carried in the session;
the page says when it is not there and the speakers stay silent. There is no weighting, no distance compensation
and no positions, so nothing to draw and nothing to hear through on headphones. The normalisation the filters
expect is a switch on the page (as the file says, SN3D, N3D). Measured: a 16 × 20 matrix at 1024 taps costs 7.9 %
of one core at 128 samples / 48 kHz, Release.

### The renderer

`SpeakerRenderer` (`speakers.hpp`) applies the matrix per frame with the distance gains folded in, delays each
output by whole samples, and takes a new design whole from the worker, fading over 10 ms once the new design's delay
lines hold its signal. `SpeakerDecoder` carries a layout request from the message thread to one worker and the
design to the audio thread, and keeps the previous design playing when a layout is refused. Measured, per
128-sample block at 48 kHz, one core, Release: 0.8 % at order 3 on a 16-loudspeaker array, 4.8 % at order 7 on 64
loudspeakers.

### Example: a 16-loudspeaker array

Seven loudspeakers at ear level, eight above, one on top, nothing below (one imaginary point), 1.30–1.65 m away.
Order 3 is its order; 16 loudspeakers carry (3+1)² channels.

| order | energy above −30° | energy below | rE above | direction error above | below |
|---|---|---|---|---|---|
| 1 | −0.8 … +0.9 dB | −1.5 … +0.6 | 0.64 | 7.1° | 52° |
| 2 | −0.9 … +1.7 | −5.6 … +1.0 | 0.80 | 3.8° | 49° |
| 3 | −1.0 … +2.0 | −9.7 … +1.1 | 0.86 | 3.9° | 49° |

Above −30° the array is even and sharp; below it a source comes from the lowest ring, about 50° off and up to
10 dB quiet at order 3.

### Hear the array on headphones

The layout's loudspeakers rendered through the HRTF as virtual sources, head-tracked, from the same layout file the
room uses: one hears the array's gaps, order limit and missing lower half at home (`virtualarray.hpp`). Each
loudspeaker is encoded at order 7 and decoded by MagLS at order 7, well above the array's order, so what is heard
is the array's panning and not a second blur. The array's decoder and the loudspeakers' encoding collapse into one
matrix from the bus to an order-7 field, which a `SpeakerRenderer` plays and swaps as it would a layout, and the
order-7 `MatrixConvolver` takes from there. Every loudspeaker sits at one distance. The head's turn goes on the
order-7 field. Measured per block: 1.7 % at bus order 1, 1.9 % at order 3, for 7.1.4.

### A test tone per loudspeaker

`test` on a loudspeaker's row plays 0.4 s of noise at −20 dBFS on its bus channel while the array's feeds hold
back, so a wrong channel map shows itself. A filter matrix gets one per output. The burst is the same samples at
every press.

## The plugin's UI

The shared shell, theme, controls and scene view with a different centre. Built in `plugins/decoder/`;
`bambi-decoder-shot` draws it to a PNG and `bambi-decoder-check` is its headless contract.

- **One scene in the room's frame**: the session's sources turned with the scene, the layout's loudspeakers, the
  hull the layout pans across (while the speakers' page is open), a head turned by the tracker, and, switched on at
  the equirect's corner, the energy picture of the field every output hears. What plays is full ink: the head
  while headphones play, the loudspeakers while a layout plays or headphones listen through it. A *head frame*
  toggle turns the view with the gaze, the energy picture with it; the head is a circle with two eyes on its rim,
  facing front without a tracker, since the sliders turn the scene and not the head.
- **The outputs are tabs in the header**, *headphones* and *speakers*. The name opens the output's page; a square
  in the tab plays it, so opening the speakers' page while listening on headphones changes nothing heard.
- **The right panel shows the open output's page.** The headphones page: the head as *model · ku100 · sofa file*
  with its two sliders, then *channels* (the pair on the bus), then *options* (the layout heard through on
  headphones). The speakers page has three sub-tabs: *layout* (*itu · file · typed* with the chosen kind's own
  controls, `loudspeakers…`, then *channels*), *decoding* (*fold · slice* for a two-loudspeaker layout, the listening
  situation, distance compensation) and *fill* ([filling a layout's gaps](DECODER-FILL.md)), which holds the head as
  the headphones page does. *Channels* is the bus's channels with what plays on each, a key for the two outputs'
  inks and a line naming each output's channels, the one that does not play marked *not playing*. The loudspeaker table opens
  as a temporary tab from `loudspeakers…`; the tracker's setup does the same, each with a close cross. The head and
  ears sliders show their values without units.
- **The orientation strip sits where the effects have their matrix**: tabs for the tracker (none · osc · airpods ·
  phone), then the tracker's word (whether it speaks and how often, or that the input has no direction), recentre
  and setup; under them the scene's yaw, pitch and roll, each with a `flip` for the tracker's sign on that axis,
  their rates under them, the current rotation of each axis (its angle plus what its rate has accumulated) as the
  live mark. Recentre, setup and the flips grey under *none*. Double-clicking an angle to reset it also clears the
  rotation its rate has accumulated. Whether the tracker tracks the head or rotates the scene is chosen in its
  setup. See [head tracking](DECODER-HEAD-TRACKING.md).
- **The level column** draws one bar per channel of the output bus.
- **A second decoder in a session** gets a small instance switch left of the link line, drawn only while there are
  two or more. A followed decoder shows the other's orientation and parameters, not its pages: the link's patch
  record carries no decoder state, so the pages show defaults, greyed.
- **The layout goes to the other plugins**: while a layout plays, the decoder publishes its loudspeakers on the link
  bus and every plugin's scene can draw them as an overlay; the decoder's scene can show the encoders' sources.

## State and parameters

Parameters are few: yaw, pitch, roll and their rates (they turn the scene for every output and are what an offline
render turns it by) and the output gain. Everything else is state, never automated, so a bounce hears it whole:
what plays and where, the layout's contents, the head file's text, the sliders, weighting, the tracker's setup. A
session reopened elsewhere renders as it did.

**Rendered offline** (`tools/decode-render`, `bambi-decode-render`): the engine the plugin plays through
(`core/include/bambi/decode/engine.hpp`), with no plugin and no host; the input generated from a seed, the heads read
from `core/data/decode`. Six goldens in `tests/golden/scenarios.txt` hash the bus and the control trace: headphones at
the default head and with the head turning, the scene turning at its rates through a locate, ten loudspeakers through
their distance delays and gains, a filter matrix resampled and read as N3D, and a first-order field through a layout
heard on headphones. The render is identical at block sizes 1, 64, 128, 333 and 1024 with the scene and the head
turning, the level moving, the outputs rebuilt and a locate inside it.

Background: decisions 0165, 0228.
