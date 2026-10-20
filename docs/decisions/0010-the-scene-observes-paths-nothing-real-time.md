# 0010 · The scene observes paths; nothing real-time touches the segment; a source is never dragged

**Status:** accepted

**Date:** 2026-09-11  **Status when written:** decided, with two open items

Three corrections to the link bus as first built, all prompted by asking what it was actually
*for* rather than whether it worked.

### Nothing on the audio thread touches the shared segment

The first bus published position from the audio thread. That is the standard
shared-memory-meter pattern — no lock, no allocation, no syscall, 0.021 µs — and it is not
what causes dropouts. It was still the wrong call, for one reason that outweighs the rest:
**the benefit was zero.** A scene redraws at 60 Hz; nobody can see a position arrive 16 ms
sooner. Against that, a mapped segment is faulted in lazily — a kernel transition on the
first touch of each page, and the first version never pre-faulted — and a function blessed
as "audio-thread safe" is an open invitation for the next change to add something that is not.

Now the audio thread's entire contribution is `LinkPublisher::submit`: a copy into
process-local memory and one atomic exchange, **1 ns**, through `LatestValue<T>` — a triple
buffer, with no retry loop and therefore no retry bound to get wrong (0009). A message-thread
timer calls `tick()` and publishes.

That move fixed something nobody had noticed. The heartbeat used to ride on the audio thread,
so a host that stops calling the audio callback — transport stopped, track frozen, plugin
bypassed — would have let the instance be reaped and vanish from every other window while it
was still in the project. Liveness now comes from the timer. **An instance belongs in the
scene because it is in the project, not because audio is running.** `tools/link-test.sh`
checks this by stopping a publisher's audio thread and looking again three seconds later.

### The bus carries the path, not just a point

Publishing only a resolved position made it impossible for the scene to draw a trajectory at
all. A slot is now two sections with separate seqlocks:

| | changes | carries |
|---|---|---|
| **static** | on edit, generation-counted | label, colour, order, the path as a 128-point polyline, its centre |
| **dynamic** | every frame | `s`, the resolved position, width, level, **the transform** |

Two choices inside that are not obvious:

- **The transform is dynamic, not static.** Centre az/el, roll and extent are modulation
  destinations and can move every control step. A path published already transformed would
  change every frame and defeat the generation counter, so the path travels untransformed and
  the receiver applies the current transform.
- **The path is a polyline, not a descriptor.** A generator descriptor or node list would be
  smaller, but every receiver would have to rebuild the arc-length table — coupling the scene
  to generator code, and letting two plugin versions draw one descriptor differently. 128
  points is draw-ready, version-proof, and more than a scene sphere can show.

`LinkScene` copies a peer's path only when its generation moves, so in a session where nobody
is editing, **updating a nine-instance scene costs 0.095 µs** — no path is copied at all. That
makes the generation load-bearing: a wrong one caches a stale path forever. It is written
inside the same seqlock as the path, and the unit test pins it as strictly as possible — the
writer publishes path *n* as its *n*-th publish, so content and generation must be equal on
every read.

A slot is visible the moment its instance joins, before it has said where it is, and the
default it holds until then is a source facing front. Peers therefore report `hasPosition`,
read exactly from the seqlock counter (0 means no publish has completed). The end-to-end test
found this as 81 "torn" reads that were really that default.

### A source is never dragged

The first bus justified its command channel with "dragging a source in the scene". There is no
such interaction: a source's position is its trajectory evaluated at the clock, and dragging
it would be overwritten at the next control step. Dragging the scene **orbits the view**
([UI.md §8](../encoder/UI.md)). The command channel stands on the ground  actually gives
it — editing the selected instance from another instance's window.

### Found while restructuring: the suspended instance

A process suspended past the heartbeat timeout — a closed laptop lid, a debugger breakpoint —
is reaped, and a newcomer may claim its slot. On resuming, the old bus carried on writing into
that slot, breaking the single-writer rule every seqlock here depends on, and its `close()`
would then have evicted the newcomer. Publishing now checks ownership and rejoins as the same
instance in a free slot, republishing its last path, and `close()` releases only a slot it
still owns. Tested across real processes with `SIGSTOP` and `SIGCONT`.

### Open — both settled in 0011

1. **A refused gesture boundary is not harmless.** A full inbox refuses, which is right for a
   stream of values — the next supersedes the last — but a refused *end* leaves the target's
   host believing an edit is still in progress. Needs reserved inbox capacity for gesture
   begin/end, or a sender that retries them. Not decided.
2. **Bézier node edits cannot be expressed as commands.** `LinkCommand` carries one parameter
   and one value. Editing the selected instance's custom trajectory from another window, which
    implies, needs a state-edit command — larger, and not superseded by the next
   one. Not designed.
