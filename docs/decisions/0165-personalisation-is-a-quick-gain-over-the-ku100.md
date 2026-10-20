# 0165 · Personalisation is a quick gain over the KU100: one slider for the head, one for the ears, on symmetric heads

**Status:** accepted

*2026-10-04. The owner, after the work on HRTF data had drifted towards reproducing measured heads, asymmetries
included: "We are not trying to distill perfect HRTFs for realistic unsymmetric heads."*

**The aim is a quick improvement on a very low baseline, to make 3D audio more accessible.** Most people listen
through the KU100, one dummy head for everyone. The decoder's personalisation is to beat that with almost no effort
from the listener — in the best case **one slider for head and torso and one for the ears**.

**It is not** a replacement for a measurement in a loudspeaker array, not a route through a 3D scan of the head, and
not a reconstruction of anyone's measured HRTF. How close a model comes to a measured head is a tool for finding the
sliders, never the goal.

**Heads are symmetric: one ear over the whole sphere is a head.** The other ear is its mirror image; the KU100 is
symmetric too. The simplification is necessary, not a compromise to revisit: it halves what a model has to describe,
makes CLL, ILD and ITD three views of one function, and turns every measured head into two (its left ear mirrored,
its right ear mirrored). Measured on HUTUBS (96 heads, 4–16 kHz, smoothed to one ERB): a head's two ears differ by
3.25 dB RMS, two thirds of what two different heads differ by (4.91 dB) — so the two are distinct heads, not copies.
The heads sat within 0.6° of straight ahead (median; 1.8° at the 95th percentile), so turning them before mirroring
is a small correction.

**The model has two parts** ([HRTF-DATA.md](../decoder/DECODER-HRTF.md)): a smooth part — head and torso, a low-order
pattern on the sphere, varied between heads along a few smooth axes — and a notch layer, flat except for the ears'
few notches.

**Contingent on:** symmetric heads localising no worse than the KU100 does, which is the bar. **Would overturn:** a
listening result where a symmetrised own head falls behind the KU100.
