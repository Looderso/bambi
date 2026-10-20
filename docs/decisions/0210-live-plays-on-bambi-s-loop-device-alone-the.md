# 0210 · Live plays on bambi's loop device alone; the host is its sound card

**Status:** accepted

*2026-10-15. The owner, after hearing tracks bambi plays arrive late against tracks left in Live. Revises how
[ABLETON.md](../suite/LIVE-HOST.md) §8 wires the audio: an aggregate for Live, the binaural mix back on the bambi
track.*

**Live outputs everything to the loop device, and the host plays it all out**, through a private aggregate of the
loop device and the user's interface: Live's Main delayed by the plan's latency, the decoder's outputs to channels
of their own. Both reach the interface by the same way, so they line up to the sample; through Live, a track bambi
plays came a round trip late, and Live could not know. Choosing "bambi" in Live is the whole setup; without the
host it is silent, as any loop device is.

**The host is for playing a set, not for recording one**: inputs of the interface may be sources of the field in
the host; recording is done with Live on the interface and bambi off. What does not come back into Live is not
rendered by it. Planned in ABLETON.md §9; not built.
