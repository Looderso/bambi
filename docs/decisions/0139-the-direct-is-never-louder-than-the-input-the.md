# 0139 · The direct is never louder than the input; the probe repaints with the patch; displace has a turn

**Status:** accepted

*2026-09-23. Three reports from a session: "distance = 0 clips in longer reverbs", "viz doesn't update when
tap settings are updated in echo", and displace "isn't showing current position similar to how it is shown in
the transform… the double click of the displace therefore doesn't reset the position".*

**Reverb: the direct is never louder than what came in.** Close to the source the room's energy against the
direct, K, is small, and the dry sits at `1/sqrt(K)` to keep the direct-to-reverberant ratio real: a hall at
0.5 m put it 18.7 dB over the input, the output at +8 dBFS. **Where the dry as heard — trim included — would be
over 1, the whole output comes down by that much**, so the ratio is kept. The wet alone never gets there (a hall
at 0.5 m, dry off, peaks at −22.5 dB), so with the dry trimmed away nothing moves; at every room's own distance
the dry is already below 1, so nothing moves there either, and the goldens are unchanged. It is the same
`1/sqrt(K)` as 's; this bounds what it can do to the level, not the balance off the rooms.

**Echo and Reverb: the probe's answer repaints with the patch.** It is worked out as the views paint, and the
views repainted only when a region moved. When the controls change — a tap, a room, from here or the host —
the views paint again.

**The encoder: displace has a turn, as an angle does** ([UI §26](../suite/LOOK.md)). Its live mark is where the
source is, as the displace that would put it there with nothing travelled, the one nearest the set value, read
from the `s` every instance publishes. Double-clicking it clears the distance travelled — axis 3 of the zero
command, beside yaw, pitch and roll — so the source goes back to where displace alone puts it.

**Checked:** the hall at 0.5 m holds the dry at 1 and turns the room down by the dry's excess, and moves nothing
with the dry trimmed away — **3 of 3 mutations caught**; the distance travelled cleared — **1 of 1**. The probe's
repaint and displace's mark are for the eye: looked at in the host, not pixel-checked.
