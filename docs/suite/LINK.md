# The link between instances

Every bambi instance in a project sees the others: a window draws other instances' sources and regions, edits
another instance's controls, and copies a preset onto it. The link bus is what carries that
(`core/include/bambi/link/`, `core/src/link/`).

## What it is

- **One bus, one session, every kind of instance on it.** Transport is machine-local shared memory keyed by the
  session's id, so two open projects never meet. Liveness is a heartbeat, never a process id.
- **What travels**: what an instance is, where its sources are now, its regions as resolved, a window's edits to
  another instance and that instance's controls.
- **Edits land on the target's own host parameter**, so a remote change is automatable and undoable there, and the
  receiver owns the gesture.
- **The layout is versioned** (`kLinkVersion`): a segment's name carries it, and an instance never joins a segment of
  another layout. Every layout change that reaches installed plugins is a new version (below).

## Changing what travels

1. Change the record or the slot, then **bump `kLinkVersion`** (`core/include/bambi/link/link.hpp`) and update the
   `static_assert(sizeof(Slot) == …)` in `core/src/link/link.cpp` that pins the layout.
2. **One number per installed layout**, even mid-work: a build whose layout reaches installed plugins gets a number no
   earlier installed build used. Two layouts sharing a number refuse each other ("created by an incompatible build")
   and windows lose their instance tabs.
3. Run `core/tests/link/` (joins, gestures, the fuzz), then rebuild and reinstall **every** plugin — a host keeps an
   old binary until it restarts.
4. Platform calls (shared memory, the clock) stay behind `core/src/link/os.hpp`; no `#ifdef` in the bus's logic.

`core-architecture` and `realtime-safety` review a change here.

## What never travels

**Definitions travel, values do not.** No instance's audio depends on another's live value — a freeze or a stem
render processes tracks separately, and a bounce must match what was heard. Feature values (level, attack, …) are
computed locally in every instance and never cross the bus. Nothing on the audio thread touches it; a message-thread
publisher does.

Background: decisions 0009, 0010, 0017, 0026, 0030, 0038.
