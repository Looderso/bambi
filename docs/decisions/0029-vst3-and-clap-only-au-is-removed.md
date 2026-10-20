# 0029 · VST3 and CLAP only; AU is removed

**Status:** accepted

**Date:** 2026-09-17  **Status when written:** decided  **Scope:** suite

The suite builds **VST3 and CLAP**. AU is removed, including `auval` and the AU hosting in
`bambi-host-probe`. This revises the format list in
 and **supersedes**
.

### Why

- **One bus rule for the whole suite.** `isBusesLayoutSupported` accepts a main input of any width *only*
  because JUCE's AU wrapper requires it. That exception is already narrowed to AU — which is what fixed the
  sidechain defect on 2026-09-13 — and with AU
  gone it disappears entirely. The rule stops being format-conditional and becomes **fixed main input,
  ambisonic main output, sidechain as a real aux bus**, the same sentence for every plugin.
  **This does not fix anything that is broken; it removes the last `wrapperType` test from the bus logic.**
- **It makes one conformance suite possible** across every plugin and both formats
  ([VISION §5.4](../VISION.md)) — the thing that has to be boring before anything is built on top of it.
- Three formats was three negotiations for two plugins that do not exist yet.

### What it costs

**Logic and Live are unreachable until AU returns** — they were already untested, with no access to either.
AU comes back as its own phase after the first release, with the wide-input case re-argued rather than
inherited.

**Would overturn:** a release for Logic users being more valuable than the simplicity, which is a commercial
judgement, not a technical one.
