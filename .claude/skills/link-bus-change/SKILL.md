---
name: link-bus-change
description: Change what travels on bambi's link bus between plugin instances — the shared-memory slot layout, a record, the session directory — or the live host's protocol with the Live device. Use before editing core/include/bambi/link/ or core/include/bambi/live/protocol.hpp.
---

# Changing the link bus

The link bus is cross-process shared memory between every bambi instance on one machine: a seqlock, a versioned
header, a heartbeat, a session directory (`core/include/bambi/link/`, `core/src/link/`).

## What may travel

- **Definitions travel, values do not.** Paths, regions, a patch record, an instance's identity — never a live value
  that drives another instance's audio. Freeze and stem renders process tracks separately; a bounce must match what
  was heard. If the change would make one instance's sound depend on another's live state, stop: it needs a decision.
- **Nothing on the audio thread touches shared memory.** The audio thread hands state to the message-thread publisher.
- Remote edits land on the target's own host parameter, as one gesture; the receiver owns gestures.

## Every layout change is a version bump

1. Change the record or slot.
2. **Bump `kLinkVersion`** (`core/include/bambi/link/link.hpp`). The segment's name carries the version, so a build
   with a different layout never joins a stale segment — it makes its own.
3. Update the `static_assert(sizeof(Slot) == …)` in `core/src/link/link.cpp`, which pins the layout.
4. **One number per installed layout**, even mid-work: if a build with a changed `sizeof(Slot)` reaches installed
   plugins, it gets a number no earlier installed build used. Two layouts sharing a version refuse to join each
   other ("created by an incompatible build") and the windows lose their instance tabs.
5. Tests: `core/tests/link/` (the version is in the segment name; joins, gestures, the fuzz). Concurrency tests stop
   on a sample count, never a clock.
6. Rebuild and reinstall **every** plugin — a host holding an old binary until it restarts is still on the old
   version.

## The platform seam

Platform calls (shared memory and the clock) live only behind `core/src/link/os.hpp`, with `os_posix.cpp`
and `os_windows.cpp`. Liveness is a heartbeat, never a pid. No `#ifdef` in the bus's logic.

## The live host's protocol

A change to the messages between the Live device and the host (`core/include/bambi/live/protocol.hpp`) bumps
`kProtocol` and says in its comment what the new number added. A host meets a device in another protocol once and
then ignores it.

`core-architecture` and `realtime-safety` review a bus change.
