# 0058 · A parameter manifest is a value a plugin owns, not a global enum

**Status:** accepted

**Date:** 2026-09-18  **Status when written:** decided by the owner (the split; the keys are unchanged)  **Scope:** suite

0054 §3 held the manifest split until all three engines existed in `core/`. They do, so it starts. **The encoder's 79 keys do not change — only how they are declared.**

**The shape: a manifest read at run time, not a type each plugin instantiates.** `mod/` and `link/` sit
*below* a plugin's engine in the layering (`tools/check-layers.sh`: `math io < dsp path < patch < mod < link
scene encode`), so they cannot name a type that lives in `encode/`. Templating them on one would move the
modulation engine out of its `.cpp` and instantiate it three times, against the cold-build discipline
([0012](0012-the-plugin-native-juce-ui-headless-until-the.md)). Indexing an integer costs exactly what indexing the enum already cost. Each plugin keeps its
own readable `enum class ParamId` over its own X-macro; the rules that enum lives under are unchanged and
absolute.

| | |
|---|---|
| `ParamManifest` | a view over one plugin's descriptors plus three side tables — skew, destination kind, smoothing |
| `kMaxParams = 128` | the cap every engine and the bus size their arrays by. **`kLinkMaxParams` is defined as it**, not as a second 128 that has to be kept in step; the Segment's `static_assert`s ([0044](0044-cross-architecture-agreement-is-checked-by.md)) make a change to the cap a compile error |
| `kNoParam = -1` | a sentinel **outside** every manifest. `ParamId::Count` is one past the end of the only list there was, which stops being safe the moment there is a second |
| `DestKind` | moves down from `mod/` to `patch/`: how a parameter answers modulation is a property of what it *means*, not of the matrix |

**Side tables, not columns of the X-macro.** Skew, destination kind and smoothing could each be a field on
`ParamDesc`. They are kept beside the list instead, because the macro holds the **keys** and every edit to it
is an edit to the one thing that can never change; adding two columns to 79 rows to express twenty facts is
the wrong trade. The cost is that `destinationKind` is no longer a `switch` the compiler checks — it is built
from key strings, where a misspelling reads as "not modulatable", a cell that would simply never move. **A
frozen list in `test_parameters.cpp` replaces the compiler**: twenty keys with their kinds, plus an assertion
that nothing else is modulatable. Three mutations caught — a misspelled key, a key added to the table only,
and `motion.displace` losing its own smoothing limit.

**The smoothing table is `double`.** Written as `float` it moved the last bits of every smoothed sample, and
two goldens caught it — `gain-duck` and `region-speed`, the two scenarios that drive a smoothed destination.
*"Audio changed, control trace did not"* named the class of fault before the diff was read.

**The bus carries every kind, and an edit crosses none** (`kLinkVersion` 10). One bus, one session, all
three plugins — the scene is kind-agnostic and must stay so, because a window should draw every instance's
path whatever made it. What is *not* kind-agnostic is anything addressed by **parameter position**:
`LinkCommand::param` is an index into a manifest, and `LinkPatch` is shaped like the encoder's state
entirely. An index meaning `render.width` in one plugin means something else in another, and would be
applied in perfect silence. So an instance declares its `LinkProduct` at join — beside its identity,
written once before the heartbeat that makes the slot visible, and read without a seqlock exactly as the
identity is — and both send paths refuse a target of another kind. `open()` does **not** default it: a
plugin that forgot to say would silently become an encoder to everything else on the bus. 3 of 3 mutations
caught, and the cross-process harness still passes.

**The list is out, and the layering proves it** (2026-09-18). `ParamId` is now an **opaque enum** declared in
`patch/` and defined by each plugin: an opaque enumeration with a fixed underlying type is a complete type, so
shared code can hold one, pass one, compare one and cast one — everything it ever needed — while being unable
to *name* one. `encode/params.hpp` holds the X-macro, the enumerators, the tables and the ParamId-taking
overloads that keep the encoder's own code reading as it always did.

What the move actually cost was not the enum. `mod/` and `patch/` did not merely hold a `ParamId`; they
consulted the encoder's **meaning** of one. So `isModulationTarget`, `matrixTargets`, `sourceColumn`,
`modulationReach`, `setCellDepth`, `unpackPatch`, `ModulationPatch::compile` and all four of `toJson`,
`fromJson`, `saveState`, `loadState` take a manifest now, and `ModulationEngine::useManifest` tells the shared
engine whose parameters it is reading. Two consequences worth naming, because both were silent faults waiting:

- **`PluginState`'s default constructor no longer fills parameter defaults** — it does not know whose state it
  is. A state that belongs to a plugin is built as `PluginState{manifest}`. The one that caught this was
  `fromJson` building its own state internally and quietly losing the envelope triggers' defaults.
- **`LinkCommand::paramId()` no longer validates.** A command is a POD in shared memory with no manifest to
  check against, and the only manifest that means anything is the **receiver's**.

The owner chose parameter-passing over a manifest installed once per process. A binary really is one plugin, so
the global would be true — until `bambi-tests`, which is one binary over all three engines, needs two.

**What this does not decide.** `PluginState` is **not** split: it stays encoder-shaped apart from its
parameter array. Echo and Reverb needing their own state is a second and larger question, and 0054 §3 gated
only the parameter manifest. Echo's and Reverb's own key lists are a separate proposal.

**Would overturn:** a plugin needing a parameter list that changes at run time, which nothing does — a host
fixes its list at load.
