# 0044 · Cross-architecture agreement is checked by nulling two builds and by `static_assert`, not by hashes

**Status:** accepted

**Date:** 2026-09-18  **Status when written:** decided, built  **Scope:** suite

Answers both items [0038](0038-a-bounce-matches-what-was-heard-on-the-machine.md) left under
*what this does not decide*. Approved by the user.

**`tests/golden/manifest.txt` keeps exact hashes and is a per-toolchain manifest, named as one in
`tools/golden.sh`'s header. The architecture-independent check is a different instrument:**
`tools/check-portable.sh` gained a third guard that renders every scenario with **both** builds and nulls them
at **−120 dBFS**, and compares the control traces byte for byte. The scenario list moved out of
`tools/golden.sh` into **`tests/golden/scenarios.txt`**, read by both scripts, so the two checks cannot drift
apart.

### Why a quantised hash cannot work, at any word length

A one-ULP difference near 0.1 full scale is ~7.5e-9. A 24-bit LSB is 1.2e-7. So **~6 % of samples sit close
enough to a rounding boundary to cross it**, and a scenario has millions of samples. Every coarser word length
has the same problem: quantisation moves the boundary, it does not remove it. A hash is the wrong shape for a
tolerance, and no choice of bits makes it the right one.

### Why −120 dBFS

Chosen against the measurement, not picked: the third guard reports **11 scenarios null, worst −144.5 dBFS,
2 bit-identical, every control trace identical** (2026-09-18). That is **24 dB of headroom below what rounding
explains**, far below anything audible, and tight enough that a real determinism bug cannot hide under it.

### The link segment's layout is declared, not compared

`sizeof(Slot)`, `alignof(Slot)`, `sizeof(Header)` and `sizeof(Segment)` are `static_assert`ed in
`core/src/link/link.cpp`, `Segment` as arithmetic over `Header` + `kLinkMaxInstances` slots so an accidental
gap fails the build. It was a runtime comparison, which checks the layout where it is *used*; it is now a
compile error on the day a second toolchain is first configured. The comment names the one place the compiler
chooses padding: four bytes after `editSequence`, because `editLock` is a 64-bit atomic that both targets
align to 8.

**Contingent on** an x86-64 cross-build staying available as a second architecture at seconds of cost
([PORTABILITY §3](../suite/PORTABILITY.md)); on the divergence staying rounding, which is what −144.5 dBFS says;
and on `bambi-nulltest` remaining the tolerance instrument.

**Would overturn:** a worst-case null that climbs toward −120 dBFS — then the gate moves and the cause is
found, rather than the gate being widened; a second toolchain whose goldens are wanted in the manifest, which
needs the manifest to become per-toolchain in file layout rather than by declaration; or a portable libm and
FFT arriving at no cost, which would make exact hashes portable and this guard redundant
([0038](0038-a-bounce-matches-what-was-heard-on-the-machine.md)).
