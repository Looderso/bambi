# 0031 · The suite stays on JUCE

**Status:** accepted

**Date:** 2026-09-17  **Status when written:** decided  **Scope:** suite

JUCE 9 remains the plugin framework for every plugin in the suite. iPlug2 was considered and
declined for now. Detail in [VISION §6](../VISION.md).

### Why the question was real

`core/` carries no JUCE, and the editor turned out to use only **twelve JUCE types** — `String`,
`Rectangle`, `Graphics`, `Colour`, `MouseEvent`, `Point`, `Font`, `Path`, `Image`, `Component`,
`Timer`, `AffineTransform` — with no `Slider`, no `Button` and no `LookAndFeel`, because everything is
drawn by hand through `Theme.h` and `Draw.h`. The port is therefore *possible*, which is not the same
as worth doing.

### Why not now

- **Core being framework-free means the switch buys core nothing.** What is JUCE-shaped is the editor
  and the host layer — exactly what is built and what passed REAPER's contract
  ([P2-REAPER.md](../../.claude/skills/host-reaper-check/SKILL.md)). A port re-opens risk that is closed.
- **The pain that prompted the question was AU's, not JUCE's**, and AU is gone
  ([0029](0029-vst3-and-clap-only-au-is-removed.md)). Ambisonic layouts are exotic in every wrapper.
- **iPlug2's CMake support is a funding goal, not a shipped feature.** This project's cold build is
  ~1 s and `tools/watch.sh` reruns the tests on save; trading that for Xcode and Visual Studio
  projects is a daily cost against a licence benefit that is not yet due.
- **IGraphics is NanoVG or Skia.** JUCE's software renderer is the easier ground for hairline rules,
  1 px strokes and tabular figures, and there is no pixel harness to catch what a renderer swap would
  break ([STATUS, deferred debt](../STATUS.md)).

### The licence, stated plainly

JUCE 9 is **AGPLv3 or commercial**. bambi is GPL-3.0-or-later and GPLv3 §13 permits the combination,
so plugin binaries ship AGPLv3 ([0006](0006-gplv3-open-source-free.md)). **For a free and open suite that costs
nothing.** It costs something on the day a closed or paid build is wanted — then it is a JUCE licence,
or this port.

### What is done instead, in S1

The reorganisation keeps that day cheap: the shared UI library's public API is addressed through
**`bambi::ui` aliases** rather than `juce::` directly (`Draw.h` and `Theme.h` are most of the way
there), and the host layer lives in **one shared processor base**. A framework swap then touches two
libraries instead of every component.

**Would overturn:** a closed or paid release, or the CLAP patch becoming unmaintainable. The
alternative to price then is not only iPlug2 — **CLAP-native with `free-audio/clap-wrapper` producing
the VST3**, both MIT, wants the same seam and needs only a window and a text layer on top of it.
