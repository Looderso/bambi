# The picker — one row, the parameters under it

*The same in every plugin.*

## What it is

Every panel that offers a choice of *what this thing is* offers it the same way: **one row of
entries, and the parameters of the chosen entry under it.** Nothing above the row. The word
*preset* belongs to the preset browser alone ([PRESETS](PRESETS.md)) and appears on no panel.

There are two kinds of entry, and a panel has one kind or the other:

| entry | where | what choosing it does | after an edit underneath |
|---|---|---|---|
| **a kind** — orbit, lissajous, wave, polygon, spiral · everywhere, spot, band, sectors, dots, clouds | trajectory, region | shows that kind's own live parameters; those parameters *are* the kind, and the ones that reshape smoothly stay modulatable  | the kind stays selected — its parameters were edited, it was not left |
| **a starting point** — ambience … cathedral · even, ping-pong, spiral, cascade, single | room, echo pattern | writes every parameter of its panel — the room's, or all four taps' | the entry stays selected as where you came from; **double-click** returns to its values |

The plugin preset above both sets **everything** — regions, trajectories, the room, the taps, send
and return, the matrix — and is the only named snapshot. A plugin preset such as *ping-pong with a
spin behind a huge hall* is a plugin preset and needs nothing beyond that.

## Custom

**Where a second representation exists — nodes for a trajectory, spherical-harmonic weights for a
region — the row ends in `custom`.** It is greyed until one exists. It is reached only through
**convert to custom**, a button under the kind's parameters:

- it freezes what is there into **one static state** — nodes fitted to the generator, weights
  projected from the shape — and disconnects it from modulation: custom weights and custom nodes are
  not modulation targets ([REGIONS](REGIONS.md)); the transform still carries a custom trajectory
  ([ENCODER](../encoder/ENCODER.md));
- **the kind and its parameters are kept underneath.** Picking the kind again shows them as they
  were and leaves the custom as it is; picking `custom` again shows the kept custom;
- converting again from a kind **replaces** the custom. It is an undoable command, so there is no
  confirmation — a hint line under the button says what will be replaced, as the encoder already does.

Room and echo have no second representation and never get a `custom` entry.

## Each panel

- **Trajectory** (encoder): the row is the five generators then `custom`. A generator is picked in
  the row; `custom` is greyed until a chain exists.
- **Region** (every plugin, every slot): `everywhere · spot · band · sectors · dots · clouds · custom`,
  wrapped at four a line because seven do not fit one ([REGIONS](REGIONS.md)). Clouds and custom
  are both **weights kinds**: what they are is their weights.
- **Room** (reverb): the six rooms. **Shape — room / hall / tall — is inside the room** and is not a
  control; it is what the six rooms differ in besides their numbers.
- **Pattern** (echo): `even · ping-pong · spiral · cascade · single`, at the echo level, above the
  four taps. The strip that draws the taps is unchanged.

## The component

`ui/Widgets.h drawSegments` draws a row with a selected and an unavailable entry, wrapped at a column count
(a row of seven breaks into lines of four); `ParameterPage::paintSegments` wires it, with **a double-click** per
entry. A starting-point row remembers its selection in
state, under the panel's own name — `"room": {"name"}`, `"echo": {"pattern"}` — because nothing
else can say where the values came from once they have been edited.


