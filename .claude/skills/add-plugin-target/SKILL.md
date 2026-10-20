---
name: add-plugin-target
description: Add a new bambi plugin (a new product beside Encoder, Echo, Reverb, Decoder), or a new tool target for an existing plugin — CMake, identity, formats, engine library, check suite, picture tool, CI and verify. Use when a new plugin or plugin tool is planned.
---

# Adding a plugin

The template is the nearest existing plugin — for a field effect, `plugins/reverb/`. Copy its **shape**, not its
code: anything the two would share belongs in `host/`, `ui/` or `editor/` (`core-architecture` reviews this).

## 1. The engine, in core first

`core/include/bambi/<engine>/` and `core/src/<engine>/`, its own library beside `bambi-core` (`core/CMakeLists.txt`),
with its parameter list (`params.hpp`, the X-macro table — see `add-parameter`) and tests in `core/tests/<engine>/`.
It builds and is tested with no JUCE before any plugin code exists. Add its folder to `LAYERS` in `tools/check-layers.sh`
(an engine sits in layer 4 beside `encode echo reverb decode`).

## 2. The plugin

`plugins/<name>/CMakeLists.txt`, following Reverb's:

- `set(BAMBI_<NAME>_NAME ...)`, `BAMBI_<NAME>_CODE` (four characters, exactly one upper-case), `BAMBI_<NAME>_BUNDLE_ID`
  (`org.bambi.<name>`, also the CLAP id); `bambi_check_codes(...)` checks the codes at configure time.
  **Identity is fixed forever at the first release** — choose it once.
- `juce_add_plugin(... FORMATS VST3 ...)` then `clap_juce_extensions_plugin(TARGET ... CLAP_ID ... CLAP_FEATURES ...)`.
  No AU.
- `bambi_plugin_code(bambi-<name>-code NAME ... LIBRARIES bambi-<engine> SOURCES ...)` — the plugin's code as a
  library, linked by the plugin and its tools. `Source/PluginEntry.cpp` stays on the plugin target, not the library.
- Tools: `bambi_plugin_tool(bambi-<name>-check ...)` (the check suite) and `bambi_plugin_tool(bambi-<name>-shot ...)`
  (the picture).
- `add_subdirectory(<name>)` in `plugins/CMakeLists.txt`.

## 3. Its parameters in the frozen lists

Add the manifest to `core/tests/patch/test_frozen.cpp`'s plugin list and write its lines
(`BAMBI_FREEZE_WRITE=1 ./build/bambi-tests -tc="*frozen*"`); add its targets to `core/tests/patch/targets.hpp`.

## 4. Checks everywhere

- The check suite runs the shared kits from `editor/` (`EffectChecks.h`, `PresetChecks.h`, `RegionChecks.h`,
  `AllocationCheck.h`) — it does not write its own.
- `tools/verify.sh`: a step for the check suite and for the picture tool.
- The conformance suite: a subject (name, factory, kind) in `tests/conformance/main.cpp` and `bambi-<name>-code` in
  `LIBRARIES` in `tests/CMakeLists.txt` (`conformance-suite` skill).
- `.github/workflows/build.yml`: the check suite in the plugin job and the plugin in the bundle loop.
- `tools/release/`: its install notes, and its third-party notices if it adds a dependency.
- If it joins the link bus, the slot layout changes: `link-bus-change`.

## 5. Its colour and its page

A plugin colour is a named role in `ui/include/bambi/ui/Theme.h` with a meaning (`ui-uniformity` reviews it). Its
concept page goes in `docs/<name>/`, and a decision records its identity and buses.
