# 0035 · Core is layered, and each plugin's engine is a library inside it

**Status:** accepted

**Date:** 2026-09-17  **Status when written:** decided  **Scope:** suite

`core/` was twenty-five headers in one folder. It is now grouped, and it builds **more than one
library**.

```
math/   vec3 sphere legendre sh          patch/  parameters state undo json
dsp/    fft features                     link/   link directory identity latest spsc
path/   trajectory authoring generator   scene/  view
mod/    modulation matrix sources        io/     wav
encode/ encoder            <- the encoder's engine
```

| library | holds | linked by |
|---|---|---|
| **`bambi-core`** | everything shared | every plugin, every tool |
| **`bambi-encode`** | `encode/` | the encoder, and the tools that encode |
| *`bambi-echo`, `bambi-reverb`* | their engines, when written | their plugins |

### Why the engines are in core and not in `plugins/`

[VISION §4](../VISION.md) says a plugin owns its engine, and that is still true — but **the encoder's
engine has always been in `core/`, and that is exactly why 65,121 assertions run in a second without
JUCE** ([0012](0012-the-plugin-native-juce-ui-headless-until-the.md)). Moving engines
out to `plugins/` to satisfy the wording would have put the most testable code in the tree that needs
JUCE to build.

So the wording gives way instead: **a plugin owns its engine, and its engine lives in `core/` because
`core/` is where JUCE-free code is tested in seconds.** What keeps that honest is the *build*: the
encoder links `bambi-encode` and will never link Reverb's feedback delay network. **The boundary is
expressed in the build graph rather than in a folder convention**, which is the part a convention
cannot enforce.

### What the grouping fixes besides navigation

- **`matrix.hpp` was the modulation matrix**, sitting in a folder that was about to receive region
  *gain* matrices and *rotation* matrices ([REGIONS §6](../suite/REGIONS.md)). `mod/matrix.hpp` says
  which one it is, without renaming anything.
- **`view.hpp` is scene geometry** and **`sources.hpp` is modulation-source settings**, not audio
  sources. `scene/` and `mod/` say so.
- **The header graph is acyclic**, which is what makes the layering real: `math/` and the header-only
  primitives depend on nothing, and nothing lower includes anything higher. It was checked before the
  move, not assumed.

**Would overturn:** an engine that cannot be written without JUCE — which would be a reason to
question the engine, not the layout.

### The check, and what writing it found

`tools/check-layers.sh` fails a header or source in `core/` that includes from a higher layer. Same
layer is allowed; **tests are exempt**, because a test may reach anywhere — it is not part of what the
libraries link. It is verified against a deliberate violation, because a check that cannot fail is not
a check.

```
math io  <  dsp path  <  patch  <  mod  <  link scene encode
```

**Writing it found two cycles the file-level graph did not have**, both created by where the grouping
put things, and both real:

- **`patch/state.hpp` included `link/identity.hpp`**, while `link/link.hpp` includes `patch/state.hpp`.
  Identity is not the bus's — it is what an instance *saves*, and the bus keys on it. Moved to
  `patch/`.
- **`patch/state.hpp` defined the trajectory's own types** — `Node`, `TrajectoryState`,
  `TrajectoryKind`, `GeneratorType`, `kMaxNodes` — while every header in `path/` included
  `patch/state.hpp` to reach them. They are what a trajectory *is*, so they moved to
  `path/shape.hpp`. The patch **contains** a trajectory, so `patch/` includes `path/` and `path/`
  includes nothing of the patch's. One direction, and the right one.

Neither was a defect while `core/` was flat, because a flat folder cannot express a direction. That is
the argument for the grouping, made by the grouping.
