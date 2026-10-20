# 0129 · A step's song position comes from an anchor, not from the block; one denormal environment

**Status:** accepted

*2026-09-23. The determinism risks `realtime-safety` read from the code on 2026-09-17
([RELEASE](../RELEASE.md)), closed in the one place  left for them, and the denormal debt
paid with them because both are "one instance disagreeing with itself".*

**A synced LFO's position at a step is the host's position at an ANCHOR plus the step's distance
from it in whole samples** (`bambi/mod/beatclock.hpp`, held by the control grid). It was the
block's ppq plus the step's offset into the block, which rounds differently depending on where the
block began: the same step, a different last bit, at block 64 and at block 1000. The anchor is the
first reading after a start or a locate, and moves only when the host stops agreeing with it -- **a
new tempo, or a position more than half a sample from where the anchor puts it**. A position within
half a sample of the anchor is the same position; beyond it the host knows something the anchor
does not (varispeed, a tempo ramp it reports only as positions). **A tempo within 1e-9 of the
anchor's is the same tempo**, so a tempo map's last bit does not re-anchor every block. **Stopped, the
clock runs at tempo 0**: the grid counts on while the song does not move, and a synced LFO holds
where the song stopped, as it did before.

**The samples are the grid's own count, not the host's timeline.** A CLAP wrapper hands every
sub-block its host block's reading; **a reading that has not moved since the last is ignored**, so a
stale one never re-anchors a later sample to an earlier position. A block that is not walked -- a
layout changed under it, a host over its own maximum -- still moves the grid on by its length
(`ControlGrid::pass`), so its steps stay on the timeline; before, the grid's phase fell behind by it
until the next locate.

**What a tempo change inside a block means:** it is taken at the first block that reports it, from
the host's position there. A host reports one tempo a block, so a change inside a block lands at a
block boundary whose place is the host's choice -- **the one residue left, at most one block**, and
knowable only with the host's tempo map, which no format hands a plugin.

**A host that gives no `timeInSamples`** now has its timeline from seconds; a host that says nowhere
at all still says when it plays, and each play starts from zero. Before, nothing ever reset.

**`bambi-render` uses the same clock and applies the input trim** as the plugin does, before the
detectors and the encoder. It accumulated ppq per step and applied no trim. A thirteenth encoder
scenario, `trim-level-speed`, is the trim's golden.

**One denormal environment:** `bambi::ScopedFlushDenormals` (`bambi/math/denormals.hpp`) -- FTZ and
DAZ on x86, FZ on arm64, the bits `juce::ScopedNoDenormals` set -- opened by both processors in
place of JUCE's and by all three render tools at entry. The render tools ran with denormals on while
Echo's loops and Reverb's tail are feedback paths.

**The goldens moved, and were re-recorded:** `lfo-synced-yaw` by the clock, **−186.6 dBFS at worst,
its trace byte-identical**; `env-displace` and every Reverb scenario by the flush, **−180.6 dBFS at
worst**, two Reverb traces by at most 1e-11. The two causes were separated by rendering each with
the flush compiled out.

**Checked:** `test_beatclock.cpp`, **5 of 5 mutations caught** (no stale-reading skip, a zero
tolerance, no tolerance, a tempo change ignored, an exact tempo comparison; and the old
extrapolation shown to differ); in `bambi-plugin-check`, a synced LFO and a yaw rate from sample
123457 bit-identical at blocks 1, 64 and 1000 and as split sub-blocks told their host block's
position, and a stopped synced LFO holding -- **3 of 3 mutations caught** (the old extrapolation, no
stale skip, the grid ignoring `playing`). The trim's golden fails when the render drops the trim.
`realtime-safety` reviewed it and found the stopped LFO running on, the skipped block, and the exact
tempo comparison; all three are fixed above.

**Left open:** a loop wrap the host does not split still lands its reset at the next block's start,
which depends on the block size -- unverified in REAPER, and a host question before it is a code one.
