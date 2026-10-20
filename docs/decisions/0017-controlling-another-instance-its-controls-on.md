# 0017 · Controlling another instance: its controls on the bus, its patch edited whole

**Status:** accepted

**Date:** 2026-09-11  **Status when written:** built

Selecting another instance in the scene used to show a notice: the bus carried where each source was and
the path it travelled, but not the parameters or the patch behind them, so a window had nothing to show
and no way to change it (UI.md §10).  asks for exactly that — edit any instance from any
window — and 0011 designed the trajectory half. This builds all of it, for every control the editor has.

### What travels

Link version 3 adds to each slot:

| | written by | carries | when |
|---|---|---|---|
| **controls** | the owner | every host parameter's value, the patch, can-undo / can-redo, the envelope learning a note, the last edit applied | when any of it changes, generation-counted like the static section |
| **edit cell** | any other window | a whole patch after an edit, its undo name and coalescing key, an end flag | on each edit; latest wins |
| **dynamic** | the owner | as before, plus what each of the 18 sources sends | every frame |

Undo, redo and learn travel as commands in the existing inbox, with `param` outside every `ParamId` so
the gesture receiver never mistakes one for a parameter, and sent as gesture endpoints so a full inbox
still takes them. Parameters travel exactly as before: commands that land on the target's own host
parameter, inside gestures the receiver owns .

### The patch travels whole, as a fixed binary record

**Whole, not as operations**, for 0011's reason: operations need ordered, exactly-once delivery, and a
whole state is superseded by the next one like a dragged value. The editing window runs the same pure edit
functions on a copy of what the target published and sends the result; nothing the editor does needed a
remote version.

**Binary, not the JSON the state format uses.** JSON is the most version-tolerant format the project has,
and the bus does not need that: two builds with different layouts refuse to share a segment (0010). A
fixed record is 5.3 KB at its limits — 24 nodes, one cell per (source, target) pair, which the core cannot
exceed — where a full matrix in JSON is about 20 KB. It needs no allocation to read, and doubles stay
doubles, so a patch that goes to another window and back is bit-identical and a remote drag cannot wear
nodes down by rounding. Every byte is a named field, pinned by `static_assert`, so no padding the compiler
chose is copied between builds.

**Unpacked as untrusted input.** A count out of range or a number that is not finite refuses the patch;
enums clamp; cells go through `setCellDepth`, which drops what is not a real pair and collapses duplicates;
triggers go through the setters that keep them valid. The target validates before the edit becomes an
undo step.

### The edit cell has many writers, so it has a lock

The owner reads the cell under a seqlock, which is sound only with one writer, and several windows may
edit one instance. Writers take a lock first — a CAS'd timestamp. A sender that dies holding it keeps it
for 100 ms, after which the next writer takes it over; releasing is also a CAS, so a writer whose lock was
taken over cannot free its successor's. Nothing real-time is involved: every writer is a UI thread.

### The target applies it, through its own undo stack

On its link tick the target takes the newest edit, then drains commands. A single edit is one undo step;
a drag's edits coalesce under the sender's instance id and key, so one window's drag never merges with
another's, and the end flag closes the run. The target's own history is the only history: undo from
another window is an undo command, and can-undo comes back in the controls. Identity and host parameters
are never part of a patch.

Learned notes moved from the editor into the target's link tick, so a note learned for another window, or
after the editor closed, becomes an undoable edit all the same.

### The editing window shows its edit at once

A target applies an edit on its next tick and publishes it on the one after, up to about 50 ms later. A
control that waited for that would lag the mouse. So the editing window shows its own edit as pending
until the target's controls report an applied sequence at least as new — sequences only grow — or for a
second if they never do. A dragged parameter shows the value sent until the target's own catches up. The
editor reaches either instance through one interface (`ControlTarget`), so the matrix, the tabs and a
source's settings have no remote code in them.

### What stays local

The output meter is this window's instance: level is read from the audio thread, and another instance's
level would be a frame late and belong in the scene. Provisional rows, open tabs and a source's settings
tab are the window's, not the instance's.

### Costs

A slot grows by about 11 KB, and a 64-instance segment to about 900 KB — mapped, and faulted in only for
slots in use. A tick compares 60 parameter values and packs the patch only when the document sequence
moved; while nothing changes, nothing is published.

### Found by the timing trace: a stale segment froze every window

The scene stuttered in REAPER for a quarter of a second every 2.4 seconds. The editor's timing trace
(UI/FrameTrace.h) showed frames, paints and link ticks stopping together while audio ran on, so the
message thread was blocked — and the instance had no peers at all.

Both had one cause. A POSIX shared-memory object **outlives every process that opened it**, and macOS
permits `ftruncate` on one **exactly once**. Version 3's segment was still in the kernel from the previous
build; version 4's is larger; sizing it was refused for good. `mapShared` then spent 400 × 500 µs waiting
for a "creator mid-flight" that did not exist, the join failed, and `linkTick` retried two seconds later —
for as long as the session was open, freezing every plugin window in the host each time.

So the **layout version is part of the segment's name** (`/bambi.4.<session>`, and the session directory
likewise): two layouts can no longer meet, which is better than recovering when they do. And the waits
that cover genuine creation races are now 20 ms rather than 200: they run on a message thread, where
sleeping is a visible freeze, and every caller already retries on its own timer.

The general rule this makes explicit: **nothing on the message thread may wait for another process.** It
was already true of the audio thread (0010); a host's message thread is shared by every plugin window it
has open, so a wait there is everyone's freeze.
