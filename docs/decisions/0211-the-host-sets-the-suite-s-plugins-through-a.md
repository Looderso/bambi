# 0211 · The host sets the suite's plugins through a control interface of their own

**Status:** accepted

*2026-10-16. The owner, drafting the live host's audio sheet. Extends [0210](0210-live-plays-on-bambi-s-loop-device-alone-the.md).*

**The host remote-controls a suite plugin through an interface only bambi's plugins offer it**, asked for through
the plugin format's own interface lookup, never through its parameters or its saved state. The Decoder's outputs
are state, not parameters — a layout is a list, not a number — and a host editing a plugin's private state would
race its open window. Through the interface the plugin stays the one owner of its rules; the host only asks. The
first use is the audio sheet: the Decoder's outputs, their channels, its layout and level, and the channels the
host keeps for Live's Main. Versioned as the link bus is. What it carries beyond that is settled when it is
designed; planned in [ABLETON.md](../suite/LIVE-HOST.md) §9, not built.

**The audio sheet holds the interface, the order, Live's Main, the Decoder's outputs and the inputs**, over the
host's window rather than a view of its own. The rate is Live's and only shown; the load beside the order is
measured by the host as it plays, never predicted from a benchmark.
