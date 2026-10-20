# 0228 · One latency for every path, the tracker held in a bounce, the rates on the transport, and a matrix swap that is acknowledged

**Status:** accepted

*2026-10-13. The four review passes before the merge: what blocked it and the risks beside it.*

- **One latency.** The array path had no partition delay and arrived 64 samples before the headphones beside
  it, while the host was told one latency. The renderer now holds every output by the partition on top of its
  own distance delay, and the check tool hears an impulse on the array exactly the reported latency late. A
  latency per path was the alternative and is refused: hosts re-apply a changed latency with a dropout, or not
  until the next start, and a bounce could then differ from what was heard.
- **The tracker in a bounce.** A pose arriving mid-render is a live value; an offline render holds the pose it
  began with, and the doc says so.
- **The rates on the transport.** The scene's turn observed nothing of the transport and ran from zero at every
  prepare. It now takes the shared transport reading as the encoder does: a start or a locate restarts it unless
  `rates.retrigger` says continue, and the target is set per 64-frame rotation hop so a turning scene renders
  the same at every block size — checked at 64 against 512 with a yaw rate of 90.
- **The matrix swap.** The rebuild waited up to 100 ms for a block counter to move, a guess at the audio thread
  having left the convolver. It now raises a flag around the convolution and the rebuild waits on that flag,
  both sequentially consistent so neither side can miss the other; the convolver is reallocated only when its
  shape changes, and a new set of the same shape travels the published triple buffer while the old one plays.
- **No bus views on the audio thread.** A bus view of 32 channels or more allocates; the one-in one-out
  processor indexes the host's buffer directly. The same defect stands in the field effects' shared processor and
  the encoder's, tracked in RELEASE.
- **The ring loop.** The array renderer walked all 64 possible outputs every frame; it walks the live design's
  now, emptying the lines a narrower design stops feeding, so a wider one later plays no ghost. A 7.1.4 array
  fell from 5.8 to 1.1 percent of a core, measured in Release.
- The tabs a platform cannot offer are drawn as not offered, the setup tab says what is needed, and five checks
  cover the bus edge for headphones, a stereo input, a narrowed bus, the latency of every path and the rate.
