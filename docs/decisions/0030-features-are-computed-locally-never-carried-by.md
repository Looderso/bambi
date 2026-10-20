# 0030 · Features are computed locally, never carried by the link

**Status:** accepted

**Date:** 2026-09-17  **Status when written:** decided  **Scope:** suite

**Every plugin computes its own detector values from audio it holds.** The encoder from its mono or stereo
input; a field effect from the **full-field energy** of the bus, `Σ_ch x_ch²`. A sidechain is an optional second
local input. Nothing reads a feature from another instance.

### Why

- **The W channel alone is not a detector.** Antiphase content cancels in W, and a field with no order-0
  component has none. Already written down for ducking ([REVERB §3](../reverb/REVERB.md)); this makes it the rule.
- **Full-field energy cannot cancel**, is one pass over channels the plugin already holds, and needs no routing
  from the user.
- **Features over the link would break a bounce.**
  [0026](0026-the-link-across-plugins-definitions-travel.md) already says why: freeze and stem
  renders process tracks separately, so a render that depended on another instance's live value would not match
  what was heard. **The low sample rate is not the objection — determinism is.** Latency (60 Hz, up to ~33 ms
  late) is a second, smaller objection.

**Contingent on** full-field energy behaving on real material at **order 1**, where four channels is a coarse
picture of a field. To be checked when Echo's ducking is built.

**Would overturn:** the capture-to-automation fallback (ROADMAP tier 2) being built, which makes
a shared feature deterministic and turns this from a rule into a default.
