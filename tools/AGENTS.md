# tools/ — renderers, benches, checks and scripts

| script | what |
|---|---|
| `verify.sh` / `verify.sh --core` | everything that says a change is sound / the part that needs no plugin build |
| `golden.sh` (`--update`) | renders `tests/golden/scenarios.txt` and compares audio and control-trace hashes with the manifest |
| `check-portable.sh` | public headers compile standalone; core builds for x86-64; goldens null across architectures |
| `check-style.sh`, `check-layers.sh`, `format.sh` | literals outside `Theme.h`; upward includes in `core/`; clang-format |
| `conformance.sh` | every installed plugin asked the same layout and sidechain questions — slow, before a release |
| `watch.sh` | reruns the tests on save |

Headless tools (`bambi-render` and the engines' renderers, `bambi-bench`, `bambi-head`, …) share their scaffolding
in `common/`; a new tool uses it rather than copying it.

## Working here

- **The golden manifest is per toolchain.** Across architectures the instrument is `check-portable.sh`'s null, not a
  hash. A changed hash after `--update` is reviewed in the diff, never accepted blind.
- Inputs are generated (fixed seed, a hand-written LCG) — no audio fixtures, and never a standard-library random
  distribution, whose sequence differs between libraries.
- A renderer runs the same control step, clock and denormal environment as the plugin, so a render is what a host plays.
- `format.sh` skips untracked files: `git add` a new file first. Vendored code is never formatted.
- Output and test names carry no decision numbers.
