# 0111 · A stereo input: two encoders always fed mid and side, and a side that is a difference

**Status:** accepted

**2026-09-21. Decided with the user** — *"go with the difference, keys and placement are fine"* — after
`prototypes/encoder/input-mode` had measured what [VISION §5.3](../VISION.md) only described. **The key
list grew by three, approved:** `input.mode` (sum · mid/side · stereo; set once, not modulated, per), `input.spread` and `input.offset`, which may move. Appended, as new keys must be.

**The side is NOT a wider blob at the source.** That is what §5.3 said, and it cannot be
mono-compatible: the cap-convolution width ([0001](0001-source-width-uses-sh-cap-convolution-method-a.md)) leaves W at 1 for EVERY width, so a blob's
W is S itself and the output's W is M + S — **the left channel**. Predicted from the width method, then
measured because a prediction is not a number: a hard-right input folds to **−∞ dB**, and even the
binaural decode renders left +5.3 dB against right −9.9.
**The side is the DIFFERENCE of two points either side of the source**, +S to its left and −S to its
right, `spread` apart. S cancels in W exactly, at every spread, width and order, so a mono fold-down
of the output is the mid alone; spread 0 is mono; and it is a width control a person already knows.

**Two encoders, ALWAYS fed the input's mid and its side, and a mode is only what each is aimed at.**
That is the design, and three things fall out of it:

    sum        mid at the source; the side at nothing.
    mid/side   mid at the source; the side at  Y(left) − Y(right).
    stereo     L at one point and R at another, written in mid and side:
               mid at g (Y(L) + Y(R)),  side at g (Y(L) − Y(R)).

- **A mode change is a ramp, never a switch of signal** — no feed changes, so there is nothing to click.
- **Spread 0 and offset 0 are exactly `sum`**, not nearly: said outright in the control step, because
  the dot of a direction with itself can fall a bit short of 1.
- **`sum` is to the bit what it was.** The mid is aimed by the one-encoder form the goldens hash, and
  the side, aimed at nothing, is not run: adding exact zeros is still adding. **All 34 goldens that
  existed match, untouched.**

**Left and right are along the level great circle through the source, not an offset in azimuth.** An
azimuth offset closes as the source climbs — 90° of it is 14° at elevation 80 and nothing at the pole.
And AT the pole "left" flips sign as the source passes over, which would invert the side, so the
spread closes smoothly over the last ten degrees: whole to elevation 80, −2 dB at 85, −8.7 at 88
(the prototype's numbers). A path-tangent axis has no pole, and turns the pair vertical on a path over
the top, and a still source has no tangent.

**Stereo's two points are either side of where the source IS, by the path's own parameter** — not by
which way it travels, so reversing the motion does not swap the channels. The source's own position
stays the middle: what a click selects and what the region source reads.

**Centred material stays at one point's level**: each of two points sharing a signal is scaled by
`1/√(2(1 + overlap))`, a half when they coincide and 0.707 when they share nothing. **The overlap is in
ENERGY** — order n weighted by (2n+1), which is what N3D is. The first build used the plain dot product
of the SN3D gains; `dsp-performance` measured that **1.35 dB low** at order 3, 69° apart.

**A mono input is `sum` whatever the parameter says.** Both reviewers found it independently: `stereo`
still aimed the MID at two points, so a mono source landed on both while the window showed a greyed
`sum` — from a preset, an automation lane, or a track that was stereo yesterday. The control step is
told whether the input has a side. The check that should have caught it tried two of the three modes
on a mono bus; it tries all three. **REAPER's tracks are at least two channels wide**, so a mono item
arrives there as L = R on a STEREO bus, the grey never shows, and all three modes are live. With L = R
the side is silence, so **mid/side is `sum` by arithmetic** — and **`stereo` is NOT**: its mid is aimed
at both points, so the one signal sits at two places at one point's level. That is what `stereo` means
for anything centred, a mono item included. (This entry first said all three were then `sum`; asked
whether a mono input shows every option, the answer turned up the error.)

**The two channels are LEFT and RIGHT, and that is not a setting — decided by the user.** Two channels
could as well carry an already-encoded mid and side, or one signal twice, and nothing in the audio or
from the host says which; a guess would be wrong quietly. A format choice (L/R · M/S · mono) was
proposed and declined: M/S input is uncommon, no plugin is expected to take it, and whoever has one
makes a stereo signal of it first. The mode's `mid/side` is about how a left/right input is PLACED.

**In the window:** the mode is three segments at the top of the source tab, because it decides what
"the source" is. Under it an `input` group — spread, offset, and the input trim, which moved up from
`render` — both values on screen whatever the mode says, the one not in use greyed and inert. **In the scene** filled is where sound is and a ring is a place that is not a point
of sound: mid/side is the dot with two rings, stereo two dots with a ring between them where the
source is. Other windows draw the same from the bus.

**`kLinkVersion` 13**, for both reasons at once: the two points beside the position, and two more
modulation targets — 22, so 418 cells where there were 380. The slot grew 1248 bytes and its
`static_assert` says why. An old build and a new one never share a segment — the version is in the
segment's name — so they form two buses rather than misreading one.

**Cost** (`dsp-performance`, Release): a stereo control step is 2.2× the old one at order 3 and 3.6× at
order 10, which is 0.003 % and 0.010 % of a core; the second `process()` is 0.008 % and 0.052 %. CPU is
not the constraint. Sharing the harmonics between the two encoders would save at most 0.007 %, and was
left alone.

**Goldens: 37.** Three added — mid/side, stereo, and a spread the level moves — over a built-in stereo
noise (`bambi-render --stereo`). What they reach that no unit test does is two encoders ramping on
one control grid, rendered at five block sizes.

**Mutation-checked, 9 of 9:** the second weight's sign lost, the pole zone gone, left and right
swapped, the overlap ignoring the width, the side a sum, a fixed half each, left as "behind", the side
encoder fed the mid, and a mono input not asked about. **Two of my own claims were wrong and are not
made:** that the tests catch `sum` going through the pair target, or the side being left aimed — both
produce the identical bits, so neither is a defect to catch.

**Needs ears**, and nothing here pretends otherwise: whether the side's negative lobe sounds phasey at
wide spreads, mid/side against stereo for image, and every default (spread 90°, offset 0.15).

**Found by the prototype, beyond its brief:** the WIDTH prototype's decoder is missing a (2n+1) — its
energy vector is 0.557 long where max-rE gives 0.861. [0001](0001-source-width-uses-sh-cap-convolution-method-a.md)'s *"A ≈ B by ear"* was judged
through that blurred decode and may deserve a re-listen.
