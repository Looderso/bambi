# 0240 · Rejected: “Generators are transport-locked, so a copied region moves identically.”

**Status:** rejected

*Found false by 2026-09-17.* A dead end, recorded so it is not walked twice.

row
(b). "Generators" is not one class. Only an LFO that is tempo-synced, set to restart and not
sample-and-hold is a function of timeline position. Free LFOs, sample-and-hold and rates are locked to
the **play start**, so a bounce agrees only from the same start point; envelopes fire from the track's
own MIDI or audio and are never copy-safe. Found by `realtime-safety` reading
the code; nothing was run. The same reading found that **the sample-and-hold seed is not in plugin
state** — it is a constant per LFO index — although `modulation.hpp:79-80` says it is and
[MODULATION §5 and §7](../suite/MODULATION.md) did until that day.
