# 0243 · Rejected: “The goldens can be made architecture-independent by hashing at 24-bit.”

**Status:** rejected

*Found false by 2026-09-17.* A dead end, recorded so it is not walked twice.

Proposed here, in
[0038](0038-a-bounce-matches-what-was-heard-on-the-machine.md) and in [PORTABILITY §6](../suite/PORTABILITY.md) on 2026-09-17. Quantisation moves a
rounding boundary, it does not remove one: a one-ULP difference near 0.1 full scale is ~7.5e-9 against a
24-bit LSB of 1.2e-7, so **~6 % of samples sit close enough to a boundary to cross it**, and a scenario
has millions of samples. Every coarser word length has the same problem. A hash is the wrong shape for a
tolerance. Replaced by nulling two builds at −120 dBFS ([0044](0044-cross-architecture-agreement-is-checked-by.md)).
