# 0242 · Rejected: “Hysteresis is the fix for a thresholded feature decision.”

**Status:** rejected

*Found false by 2026-09-17.* A dead end, recorded so it is not walked twice.

Written here under *Found, not fixed* and
in [0038](0038-a-bounce-matches-what-was-heard-on-the-machine.md)'s *would overturn* from 2026-09-17, and false: **a latch is still a step**. A
one-ULP difference changes *which* hop opens, and the value either side differs by the full swing — a
latch with 6 dB of separation was built as a mutation and produced **the identical 0.556 worst step the
bare branch does**. It is also worse than the branch in two ways nobody had counted: it makes a
measurement **history-dependent**, so a bounce from bar 5 reads differently from realtime through bar 5
([0026](0026-the-link-across-plugins-definitions-travel.md)), and it adds state, so `reset()` has to change — the omission that makes a bounce
diverge on the first block and stays invisible until someone renders twice. A ramp does neither
([0043](0043-a-threshold-that-feeds-a-measurement-is-a-ramp.md)). What survives the retraction is the distinction: **hysteresis belongs where the
output is an event** — `EnvTrigger`, which latches and does save its state — **a ramp where it is a
measurement.**
