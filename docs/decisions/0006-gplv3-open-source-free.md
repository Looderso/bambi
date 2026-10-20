# 0006 · GPLv3, open source, free

**Status:** accepted

**Date:** 2026-09-10  **Status when written:** decided

### What it settles

- **JUCE under its open-source licence — no licence fee.** The paid tiers exist for closed
  source. *(Written as "GPLv3"; JUCE is now AGPLv3 — see the correction below.)*
- **The IEM Plugin Suite becomes usable, not just readable.** It is GPLv3, so a GPLv3
  project may copy from it. That unlocks the part that was going to hurt most: **ambisonic
  bus and order negotiation across VST3/AU** (`IOHelper` and friends), which is
  known-horrible, zero-design-risk grind. Also their OSC interface.
- **SAF** stays available either way; its core is permissive.

### What to borrow — and the standard to hold it to

Take the **plumbing**: bus and order negotiation, channel/order helpers. Not the
LookAndFeel — the Ulm direction is a deliberate departure and inheriting IEM's chrome would
fight it. Attribution and licence headers per their terms.

**Treat it as a reference implementation, not as code to trust.** It is roughly a decade
old, written by one person, and its author says himself it is neither well written nor
performant. Borrowing it wholesale would import that.

So: read it to learn *what the problem actually is* — especially the host-by-host quirks in
ambisonic bus negotiation, which is knowledge that took someone years to accumulate and
cannot be derived — then measure before adopting. Specifically worth checking: per-block
allocation, denormal handling, how often SH coefficients are recomputed versus interpolated,
and UI cost per instance.

**Rewriting is the default whenever we know better.** The value in that repo is the
hard-won knowledge of what the host contract demands, not the lines themselves.

The spherical harmonics stay ours regardless — already written twice and verified to 1.0000
on every channel.

### The one-way door

GPLv3 means the plugin cannot later be made proprietary without rewriting every GPL part.
That is the intended direction here, recorded so it is a choice rather than a discovery.

### Correction — 2026-09-11: JUCE is AGPLv3, not GPLv3

The licence recorded above was out of date. JUCE's current `LICENSE.md` (JUCE 9) reads: *"The
JUCE Framework modules are dual-licensed under the AGPLv3 and the commercial JUCE licence."*
GPLv3 was JUCE 7's licence.

It does not change the decision:

- **bambi stays free and open source.** AGPLv3 is a GPL-family open-source licence; anyone
  distributing the plugin must offer its source, exactly as under GPLv3.
- **The combination is explicitly permitted.** GPLv3 section 13 allows GPLv3 code to be
  combined with AGPLv3 code in one work. Our code keeps its GPL-3.0-or-later headers; a plugin
  binary that includes JUCE is distributed under AGPLv3 terms for the combination.
- **The one clause AGPLv3 adds concerns users interacting with the software over a network.**
  A plugin running inside a DAW does not do that, so in practice nothing changes for bambi.
- **The core is unaffected.** `core/` contains no JUCE, so it stays plain GPLv3 and remains
  usable by GPLv3 projects, the IEM suite included.
- Pinning JUCE 7 to stay on GPLv3 was considered and not taken: it trades years of
  host-compatibility fixes for a distinction with no practical effect here.

Repo needs a `LICENSE` file and licence headers before the first public commit.
