# plugins/ — one directory per plugin

Each `plugins/<name>/` holds a processor, an editor, a check suite and a picture tool. Everything a plugin does with a
host that is not its own lives in `host/`, its look in `ui/` and `editor/`, its engine in `core/`. Built only in the
plugin tree: `cmake -B build-plugin -G Ninja -DBAMBI_BUILD_PLUGIN=ON`.

## Working here

- **A plugin owns two things: its engine and its parameter set.** A third thing belongs in a shared layer.
- **Buses**: the main input is fixed by the plugin's kind (never negotiated wider than it reads); an ambisonic output
  carries the highest order that fits, the rest silent; a sidechain, where a plugin has one, is a real aux bus, never
  a wide main bus read past its first pair. VST3 stops at 7th order; CLAP has no ceiling. The `conformance-suite`
  skill holds the checks.
- **Fixed at the first release**: a plugin's identity (codes, bundle id) and bus layout. Parameter keys are frozen in
  `core/tests/patch/frozen-parameters.txt` (`add-parameter` skill).
- **A check suite exits 0 only when every check passed**; it runs the shared kits from `editor/`. `tools/verify.sh`
  runs every suite and every picture tool. A check that cannot fail is not a check.
- An installed build is not live in a running host until the host restarts.
- A new plugin: the `add-plugin-target` skill.
