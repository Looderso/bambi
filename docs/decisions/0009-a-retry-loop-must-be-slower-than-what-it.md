# 0009 · A retry loop must be slower than what it retries against

**Status:** accepted

**Date:** 2026-09-11  **Status when written:** decided

The link bus reads observations through a seqlock: the writer marks the slot odd while it
writes, and a reader that sees an odd counter, or a counter that changed across its read,
discards what it got and tries again. Standard, and correct.

The retry was **eight tight iterations**, and that was wrong in a way worth recording,
because the code looked entirely reasonable and the unit tests passed.

Eight iterations of a relaxed load complete in a few **nanoseconds**. The write they are
waiting out — copying a 72-byte observation — takes on the order of a **hundred**. So a
poll that landed inside a publish had all eight of its retries land inside *the same*
publish. The slot was skipped and the source vanished from the scene for one frame.

It showed up as one frame in eighty. In a unit test that is invisible. In a scene window
redrawing at 60 Hz it is a source flickering out roughly once a second.

**The rule:** a bounded retry has to span longer than the operation it is contending with,
or the bound is decoration. Yielding between attempts costs nothing on a UI thread and
widens the window from nanoseconds to milliseconds.

### The one that was not a race at all

The same symptom — a source vanishing for one frame, about one frame in eighty — survived
the retry fix at a lower rate, and the remaining cause was **arithmetic, not concurrency**.

`poll()` samples the current time once and then walks all 64 slots. A peer that publishes
*during* that walk stamps a heartbeat later than the sampled `now`, so `now - hb` on
unsigned values wraps to about 1.8e19 microseconds — roughly 584,000 years — which is past
any timeout, and the live peer is dropped.

It presented as a race, in a module full of real concurrency, right after a genuine race had
been found and fixed in the same function. That is the trap: once you have found one race,
every symptom looks like another one. **A heartbeat in the future is by definition not
stale**, and the guard is now a named, unit-tested predicate rather than an inline
subtraction.

### The related one, from the same module

macOS permits `ftruncate` on a POSIX shared-memory object exactly **once**. Two instances
opening a session simultaneously both see size zero, and the loser gets `EINVAL` rather than
a harmless no-op — so treating a failed `ftruncate` as fatal made joining a session fail
about **two thirds of the time when two plugins loaded together**, which is precisely what
opening a project does. The question to ask was never "did `ftruncate` succeed" but "is the
segment big enough now".

Both bugs share a shape: **they cannot occur in one process**, and neither would have been
found by any amount of in-process testing. `tools/link-test.sh` found the first on its first
run. Cross-process code needs cross-process tests, and the harness is part of the module.
