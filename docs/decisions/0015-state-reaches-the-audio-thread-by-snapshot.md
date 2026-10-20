# 0015 · State reaches the audio thread by snapshot, never by lock

**Status:** accepted, revised in part by [0049](0049-a-parameter-is-a-modulation-destination-only-if.md)

**Date:** 2026-09-11  **Status when written:** decided

### Why now

The UI will edit trajectories and the matrix while audio runs — a node dragged at 60 Hz. The P2
shell swapped state under the audio callback's lock, which stalls the audio thread for as long as
the message thread holds it, and `ModulationEngine::setState` allocates. Tolerable for a rare
state load; wrong for a drag. And `getStateInformation` read the audio thread's copy of the state
from the message thread — a race.

### Decision

- **The message thread owns the document**: the patch as the user has it, and the path built from
  it. State is saved from it, and the scene is published from it. The audio thread never reads it.
- **Every change becomes an immutable snapshot** — the compiled modulation patch, the arc-length
  trajectory, its centre — built entirely on the message thread, so all allocation happens there,
  and posted with one atomic exchange (`commitDocument`).
- **The audio thread adopts it at a block boundary**, by exchanging the pending pointer out. It never
  waits and never allocates.
- **Replaced snapshots travel back** through a wait-free single-producer queue (`SpscQueue`) and are
  freed on the message thread. With the queue full, the audio thread keeps its current snapshot and
  tries again next block: nothing is freed on the audio thread and nothing is dropped.
- A snapshot superseded before it was adopted is freed by whoever displaced it — only an exchange
  ever takes one out of the mailbox.
- **`ModulationPatch`** is what the matrix reads from a patch — the cells that can do something, and
  the envelope triggers — compiled once. `ModulationEngine::usePatch` adopts one by pointer.
  `setState` stays for offline rendering and tests and compiles a patch the engine owns; all 11
  golden scenarios are bit-identical across the change.
- **Adopting a patch does not restart the LFOs**: an edit to the routing must not reset a generator.
  The LFO seeds moved into the engine's constructor, because an engine that only ever adopts patches
  would otherwise run all three from one seed.
- **`snap`**: a state load jumps the source onto the new path; an edit will move it there instead.

### Verified

In `bambi-plugin-check`, against a real processor:

- a state load returns in about 0.1 ms while another thread holds the audio callback's lock for a
  second
- 440 state loads against an audio thread running flat out, 40 of them with the message thread not
  collecting, so the retire queue fills (1,341 times in one run): the audio thread ends on the last
  state, no snapshot is ever freed inside `processBlock`, and none leaks
- nine mutations, each caught by the check written for it — no LFO seeding, a patch ignored,
  generators restarted on adoption, the queue overwriting when full; the callback lock put back,
  freeing on the audio thread, a displaced snapshot leaked, retired snapshots never collected, a
  full queue ignored
