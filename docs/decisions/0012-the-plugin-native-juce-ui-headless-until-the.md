# 0012 · The plugin: native JUCE UI, headless until the end, host contract first

**Status:** accepted

**Date:** 2026-09-11  **Status when written:** decided

### Native JUCE components, not a WebView

A WebView UI would have let the interface be developed in a browser with hot reload, reusing
the prototypes almost directly. It was declined for performance: no browser engine per open
editor, no WebView2 runtime to depend on under Windows, and no JavaScript bridge between the
processor and a scene redrawing at 60 Hz.

The cost is known and accepted: iterating on the UI means compiling the plugin. That is
acceptable because nearly everything else never has to be. The framework is built and verified
headless, and the slow stage comes only near the finish line.

### What that means for UI code

Keep the JUCE layer thin. Anything in the UI that is logic rather than drawing belongs in
`core/`, where it builds in a second and is tested without a plugin:

- projections — `forward(vec3) -> {x, y, depth}` and `inverse(x, y) -> vec3 | null`
- hit-testing and nearest-point search, as `nearestOnPath` already is
- the interaction state machines — press versus drag by a movement threshold (ROADMAP P0
  finding 1), and node or handle drags that produce a new `TrajectoryState` applied through
  `UndoStack` 

JUCE then draws and forwards events. The browser prototypes remain the interaction spec.

### The host contract is P5's first milestone, not a throwaway spike

P2 was planned as an empty plugin, timeboxed, then deleted. It is now the first thing P5
builds, over the real core:

- ambisonic output negotiation at every order the format allows 
- a sidechain input alongside the main input
- a MIDI event input on an audio effect
- offline bounce identical to realtime, twice

That last check means more now than when P2 was planned. The core render is deterministic and
byte-identical across block sizes 1 to 1024, so a bounce that diverges is the host contract,
not our arithmetic.

VST3, CLAP, AU and Standalone  are built once for those tests. After that, development builds
Standalone only, as IMPLEMENTATION P5 already requires.
