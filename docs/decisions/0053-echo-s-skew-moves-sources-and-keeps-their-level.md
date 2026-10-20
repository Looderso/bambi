# 0053 · Echo's skew moves sources and keeps their level: no Jacobian, no scaling

**Status:** accepted

**Date:** 2026-09-18  **Status when written:** decided by the owner, by ear, on the corrected `loops`  **Scope:** echo

The warp is the plain composition with the map -- `M_ij = (2n_j+1)/4π ∫ Y_i(T d) Y_j(d) dΩ`, no Jacobian -- and
it is not scaled. A source then lands where the map says at unit gain, and a point source never exceeds +0.4 dB
in any pass (order 3, skew 0.6). The unscaled spectral radius is exactly 1 at every order and skew, so a loop
whose skew stands still never diverges at any feedback below 0 dB.

**Heard and rejected:** *with the Jacobian*, the energy does not settle at the target but travels round the
sphere; *divided by the largest gain*, the tail dies far sooner than −0.3 dB of feedback suggests (skew eats
5 dB a pass); *scaled to keep a diffuse field's energy* ("as before") is this matrix 0.63 dB louder a pass, which
grows without bound above −0.65 dB of feedback.

**What it leaves, and what covers it:** a coherent omni input can swell by up to +11 dB around pass 9 before it
decays (order 3, skew 0.6, feedback −0.3 dB), and while the axis or the skew is being modulated the bound is the
largest gain, not the spectral radius, so a loop can grow though every still setting decays. The NaN guard and
the clamp at the ring write are therefore part of the engine, not a backstop for bugs.

**Would overturn:** the swell or growth under modulation being met in real use rather than at the extremes --
then the largest-gain table is the remedy, applied only while a skew or an axis moves.
