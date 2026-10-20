# bambi — for agents and contributors

A suite of ambisonics plugins — encoders that place sound on the sphere, effects that work on the ambisonic field
itself, decoders to headphones and loudspeakers — that share one core, one look and one link between instances, plus
a live host that lets Ableton Live play through them. More plugins will join; everything shared is built for that.
VST3 and CLAP, macOS, Windows and Linux. C++20, JUCE only above `core/`.

This file is what holds everywhere. A directory with its own `AGENTS.md` adds what holds there — read it before
changing code in that directory.

## Build and verify

```bash
cmake -B build -G Ninja && cmake --build build && ./build/bambi-tests   # core + tools, no JUCE: seconds
tools/verify.sh --core                                                  # tests, goldens, style, layers, format
cmake -B build-plugin -G Ninja -DBAMBI_BUILD_PLUGIN=ON && cmake --build build-plugin   # the plugins: minutes
tools/verify.sh                                                         # everything, incl. plugin checks
```

- Fast loop: `./build/bambi-tests -tc="*name*"` runs one test; `tools/watch.sh` reruns on save.
- Slow, before a release only: `tools/conformance.sh` (loads the installed bundles as a host does).
- `tools/format.sh` formats with `.clang-format`; `verify.sh` fails on an unformatted file. A new file must be
  `git add`ed before formatting, or it is skipped.
- Release is the default build type on purpose.

## Where things live

| path | what |
|---|---|
| `core/` | everything musical, **no JUCE**: math, DSP, paths, modulation, regions, state, undo, parameters, the link bus, the engines. Layered: `math io < dsp path region < patch < mod < link scene encode echo reverb decode < live` |
| `host/` | what every plugin does with a host: buses, parameters, transport, the field-effect processor |
| `ui/`, `editor/` | the shared look and components (`ui/include/bambi/ui/Theme.h` is the one file for the look), the shared editor |
| `plugins/<name>/` | each plugin: its processor, editor, check suite and picture tool |
| `apps/live-host/`, `platform/`, `driver/` | the live host app, its device and loop layer, the loop driver |
| `tools/` | headless renderers, benches, checks, scripts |
| `docs/` | concept pages, decisions (`docs/DECISIONS.md` is the index), trackers |
| `prototypes/` | browser prototypes that settle feel and sound (not published yet; the `new-prototype` skill says how one is built) |

## Conventions

**AmbiX** — ACN ordering, SN3D normalisation (FuMa only as import/export). **Coordinates** — right-handed, x front,
y left, z up; azimuth counter-clockwise from front, elevation positive up. **Degrees at the UI and parameter
boundary, radians everywhere inside.**

## The rules

1. **Decide without compiling when you can.** `core/` builds and tests in seconds; anything decidable there or in a
   browser prototype is not decided by building a plugin.
2. **Feel and sound are settled in a prototype first** (`prototypes/`); findings port, prototype code does not.
3. **Design each thing once.** The shared component is the reference; a copy of it is a defect — it grows an
   option instead.
4. **Nothing on the audio thread allocates, frees or locks**, and nothing depends on another instance's live
   value: an offline bounce must equal what was heard, sample for sample.
5. **Measure, do not assert.** Benchmark in Release; report cost as a share of one core at 128 samples / 48 kHz.
6. **Tests are mutation-checked.** Each test names the mistake it catches in a `Catches: …` comment, and breaking
   the code that way makes it fail.
7. **Comments say what the code does** — units, invariants, thread constraints, a reason true now. Never a
   decision number, a doc reference, a name, a date or how the code came to be.
8. **The look comes from `Theme.h` only**; `tools/check-style.sh` fails on a literal colour, size or radius.
9. **An include never runs upward** through `core/`'s layers (`tools/check-layers.sh`); move the type instead.
10. **The built UI is the reference** for how things look, not a draft or a canvas.

## Don't

- Don't benchmark in Debug — it once reversed this project's picture of its own cost.
- Don't rename, remove or repurpose a parameter key, or change a frozen line in
  `core/tests/patch/frozen-parameters.txt`: saved automation depends on them.
- Don't use `-ffast-math`, `/fp:fast`, `-Ofast`, or intrinsics in `core/` outside `math/denormals.hpp`.
- Don't retry a route recorded as a dead end: `docs/DECISIONS.md` lists them with status **rejected**.

## Decisions and dead ends

Why things are the way they are is in `docs/decisions/` — one numbered file each, with a **Status** line
(accepted, revised in part by, superseded by, deprecated, rejected). Look one up when a rule here or in a
directory's `AGENTS.md` names it, or before changing something that looks deliberate. A decision that changes gets
a new number; the old one is never rewritten.

## Working on bambi

- Every change starts from a GitHub issue and lands as one pull request into `main`, from a branch named for the
  issue. Pull requests merge with a merge commit.
- Commit messages say what is now true, in a sentence; the body says why.
- Work made with an AI agent says so (a `Co-Authored-By` trailer on commits; a line in the pull request).
- Published text — issues, pull requests, commit messages, code — names no one; code and commit messages carry no
  decision numbers (the docs link decisions freely).
- Procedures for recurring tasks are in `.claude/skills/*/SKILL.md` — readable as plain instructions with any
  tool: adding a parameter, adding a plugin, writing a doc, a comment pass, a prototype, issues and pull requests.
  Four reviewers in `.claude/agents/` check finished work: architecture, real-time safety, DSP cost, UI uniformity.
