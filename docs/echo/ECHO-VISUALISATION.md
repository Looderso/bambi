# bambi Echo — visualisation

The strip, the ping and the energy picture. Part of [bambi Echo](ECHO.md). The picture components are shared with
the other plugins ([suite/VISUALISATION.md](../suite/VISUALISATION.md)); what is Echo's is what they are given.

**The incoming field is orange; what Echo adds is blue.** The selected tap's answer in the ping is in Echo's own
colour (sage), the same colour as its row in the strip.

## The energy picture

The audio thread accumulates **two covariances, a window of 50 ms each**: what arrived and what the plugin added
(upper triangle: 136 numbers at order 3, 2,080 at order 7). Colouring by their ratio is the only way the picture
can say which energy is the echo and which came in. The picture is built on the message thread.

| | |
|---|---|
| read through | a **max-rE beam** a texel, as a decoder does, so the view has the order's real sharpness rather than the plain basis's wide lobe and sidelobes |
| texture | 128 × 64 equirectangular; the globe resamples the finished texture through a table rebuilt only when the camera turns |
| colour | per texel, orange to blue by the share of the energy there that is the echo's |
| cost split | the beam energies only when a covariance lands; ballistics, colour and both projections every frame. Painting stops 1.6 s after the last covariance |

**The scale is absolute, with one ballistic.** A texel's brightness depends on its own level alone: its beam energy
in dB under a full-scale point source's peak, 0 at −42 dB and below, 1 at −3 dB and above, smooth between. Nothing
is hung from the field's peak or mean, so a quiet source is drawn the same whether or not a loud one plays, and a
diffuse tail shows as an even glow. A one-pole per texel of **110 ms** settles what survives rather than letting it
flicker.

## The ping

**This is the comprehension tool of the plugin.** Point at a direction and every enabled tap draws what it would do
to it, so a user sees where a sound would be carried. It is the same shared component as Reverb's reflection
overlay; only the answer differs.

**Every spiral is drawn from the direction last clicked, never from a source.** On a bus the plugin has no idea
where anything stands; what it can always answer is what each tap would do to a direction you point at.

- **A click places it, in either view; a drag never does.** Until placed it sits at the front with a dashed ring,
  so it says it was never placed rather than looking like a choice.
- **The taps as played.** The picture is drawn from the tap settings with modulation included, through the same
  `EchoResolver` the engine's control step uses, so it cannot say a period or an angle the plugin is not playing.
- **Selected tap and others.** The selected tap's answer is drawn in full and in Echo's colour; the others are
  dimmed and grey.

Each tap contributes (`core/include/bambi/echo/ping.hpp`):

- **A spiral**: the direction carried round the loop for 16 passes. A spiral is sampled in degrees of turn (3° a
  step, 48…480 steps), not in passes, because the turn is linear in passes and the skew is not (a skew is the tanh
  of a rapidity and passes add rapidities). Whole passes are composed and the part-pass between them interpolated,
  so a long curve never draws as a staircase. The stroke's opacity follows level to the power **0.45**, compressed
  the way a meter's is: drawn at the level it is heard at, a spiral with any useful feedback is gone after two
  passes.
- **A bead a pass**: where pass j lands, with its gain and its blur, `blur × √j`, the law the audio follows. A
  bead is a soft sprite stamped at the width of sky that pass arrives from, clamped to 2.5…45°, with a hard core
  that keeps the pass locatable. **The same energy spread wider is fainter**: opacity is divided by
  (1 + width / 14°), so the wash dims as it grows.
- **The flow**: twelve spokes seeded evenly on the axis's own equator and carried through the loop. Where the
  lines crowd, the field gathers. It is drawn only when the tap turns or slides, is fixed to the axis's frame and
  never to the probe, and goes under the spirals as the reference a click is read against.
- **Nothing is hidden**: the far side of the globe is drawn too, dimmed, so a curve never breaks in half.

## The strip

The strip puts **every enabled tap on one shared time axis**, a row a tap, because interleaving is what you are
looking at. It is also the **tap selector and it is pinned**: each of the four rows carries the tap's on/off at one
end and its level at the other, and the row itself selects the tap ([controls](ECHO-CONTROLS.md#the-panel)).

- **A click on a row selects it; a drag on the level edits it and never selects; the on/off takes no selecting
  click**, because silencing a tap must not drag its panel open. It is the only place a tap can be silenced, so it
  is a control surface and not only a picture.
- **The span follows the longest enabled tap**, rounded up to a whole beat so bar and beat marks stay readable. The
  axis therefore moves under all four taps whenever any one of them changes: the price of one shared axis. A ruler
  under the rows marks beats, with bar numbers.
- **Where the walk stops**: each tap's marks continue until 48 dB down, 4 s, or 32 passes, whichever comes first.
- **The strip is drawn from the taps as played**, modulation included, using the same resolver as the engine.

**A pass mark carries position, the level fall as opacity, the band as vertical extent, and vaguer as softness
that compounds.**

| carried by | how |
|---|---|
| position | `offset + j × period` on the shared axis |
| level | opacity, to the power 0.45; the selected tap's row in full, the others at a third |
| the band | vertical extent: the row is 20 kHz at the top to 20 Hz at the bottom, and each pixel row is lit by the band's magnitude there raised to j, so the mark thins as the band closes. It shows *that* the band is closing, not where the corners are |
| vaguer | horizontal softness, growing with `blur × √j` (the audio's law), horizontal only because the vertical axis is frequency |

Nothing caps a mark's softness by the gap to the next pass: where fast passes at a high blur smear into one
another, they smear in the ear too, and the picture is right to show it.


