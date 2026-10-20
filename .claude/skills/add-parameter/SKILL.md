---
name: add-parameter
description: Add, change or remove a plugin parameter (a host-automatable key) in any of bambi's plugins, or make one a modulation target. Use before touching a plugin's params.hpp list, frozen-parameters.txt or the target lists.
---

# Adding a parameter

Hosts save automation by key, as a position from 0 to 1. A parameter's **key, type, range, skew and choices** decide
what every saved point plays, so they are frozen. A display name, a unit or a default may change freely.

## First

**Ask the owner.** Before the first release the key list can still change, but never without a yes — not as a side
effect of a fix. Say which key, why, and whether anything existing moves.

## Steps

1. **Add one `X(...)` line to the plugin's list**, at the end of its block — never rename, remove or reorder a key:
   `core/include/bambi/encode/params.hpp`, `echo/params.hpp`, `reverb/params.hpp` or `decode/params.hpp`.
   Columns: `group | id | key | display name | unit | type | min | max | default | choices`.
   - Key: lowercase, `area.name` with `snake_case` (`output.wet`, `tap1.axis_az`). A key shared by a block (regions,
     generators, rates) comes from the block's macro in `core/include/bambi/patch/blocks.hpp`, not a new line.
   - Units at the boundary: degrees, ms, dB, Hz — radians only inside.
   - A `Choice` lists exactly one label per value of its range.
   - The list is a hand-aligned table inside `// clang-format off`; keep the columns aligned.
2. **The engine reads it** through its manifest (`encodeParams()`, `echoParams()`, `reverbParams()`,
   `decodeParams()` — a `ParamManifest`) — never a second copy of the range or default.
3. **Freeze it**: `BAMBI_FREEZE_WRITE=1 ./build/bambi-tests -tc="*frozen*"` rewrites
   `core/tests/patch/frozen-parameters.txt` from the manifests. Read the diff: **only added lines**. A changed line breaks saved
   sessions. The encoder also lists its keys in order in `core/tests/patch/test_parameters.cpp` (`kFrozenKeys`):
   append there too.
4. **A modulation target?** Which keys answer the matrix is decided by key in the plugin's `destOf` (and
   `sharedDestKind` for region slots); the kinds are `Rate`, `DirectAngle`, `DirectScalar`. Add it to the plugin's
   expected list in `core/tests/patch/targets.hpp` — the test fails on a target added, dropped or misspelled.
5. **Tests**: `./build/bambi-tests` (keys fit `keyOf`'s buffer, choices match ranges, defaults round-trip), then the
   plugin's check suite and `tools/verify.sh`.
6. **The UI**: a control for it follows `ui/AGENTS.md`; a picture tool shows it.

## Removing or changing one

Don't, after the first release. Before it, with the owner's yes: change the key list, rewrite the frozen file with
`BAMBI_FREEZE_WRITE=1`, and say in the commit which keys moved — saved test presets may need
`tools/clear-presets.sh`.
