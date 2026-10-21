# Contributing to bambi

Thank you for helping. bambi is a suite of plugins built as one program, so a change is held to rules that keep
the suite uniform. The rules are in [AGENTS.md](AGENTS.md), with each module's own beside its code, and how
bambi is designed is in [docs/](docs/); the overview is on **[bambi.wiki/contribute](https://bambi.wiki/contribute)**.
This file is the short version.

## Reporting a problem

Open an [issue](https://github.com/Looderso/bambi/issues) with:

- the plugin, its version (the release name) and its format, VST3 or CLAP;
- your system and your host, with their versions;
- what you did, what you expected, and what happened instead;
- a session or a short audio file that shows it, if you can.

## Before you write code

**Open an issue first** for anything beyond a typo or a one-line fix, above all for anything that changes the
design, how something behaves for a musician, or a plugin's parameter keys. Hosts save sessions by those keys,
so they are the one thing that cannot be taken back lightly.

## Building and checking

```bash
git clone --recurse-submodules https://github.com/Looderso/bambi
cmake -B build -G Ninja && cmake --build build && ./build/bambi-tests
cmake -B build-plugin -G Ninja -DBAMBI_BUILD_PLUGIN=ON && cmake --build build-plugin
tools/verify.sh
```

What you need on each system is in [docs/BUILDING.md](docs/BUILDING.md). A change is ready when `tools/verify.sh`
passes.

## The rules, in brief

- **The musical logic lives in `core/`**, which has no JUCE and is tested without a host. A plugin owns its engine
  and its parameter list; everything else is shared. [Where things live](AGENTS.md#where-things-live)
- **Design each thing once.** A copy of a shared component is a defect: the component grows an option instead.
- **Nothing on the audio thread allocates, frees or locks, and a bounce matches what was heard.**
  [core/AGENTS.md](core/AGENTS.md), [host/AGENTS.md](host/AGENTS.md)
- **Tests are mutation-checked.** Break the code the way a real mistake would, see the test fail, and name the
  mistake in a `Catches:` comment. [The rules](AGENTS.md#the-rules)
- **Comments say what the code does**: units, invariants, threads, a reason true now. Never history, names or
  references. [The rules](AGENTS.md#the-rules)
- **Colours, sizes and strokes live in the theme file** and nowhere else. [ui/AGENTS.md](ui/AGENTS.md)

## Working with an AI agent

bambi is built to be worked on with coding agents, and everything they need is in the repository:

- **`AGENTS.md`** at the root holds the rules, the build and where things live; each directory with its own
  `AGENTS.md` adds how to work there. Most agents read these on their own; Claude Code reads them through
  `CLAUDE.md`.
- **`docs/`** holds what is true now (concept pages) and why (`docs/decisions/`, each with a status; a *rejected*
  one is a dead end, so don't walk it again).
- **`.claude/skills/`** holds the recurring procedures — adding a parameter or a plugin, writing a doc, a comment
  pass, a prototype, issues and pull requests. They are plain Markdown, usable with any tool. Testing in a real host
  is a person's checklist, `docs/HOST-CHECKS.md`.
- **`.claude/agents/`** holds five reviewers to run over finished work: architecture, real-time safety, DSP cost,
  UI uniformity and wording.

Work made with an agent says so: its `Co-Authored-By` trailer on the commits and a line in the pull request. The
rules are the same as for anyone — `tools/verify.sh` decides when a change is ready.

## Commits and pull requests

- A change of form and a change of behaviour are **separate commits**.
- A commit's subject is a sentence that says how things are now: "A stale shared segment is refused at once on
  macOS", not "fix segment bug".
- Say what the change did to the **golden renders**: unchanged, or which moved and why.
- A change to the window comes with a **picture** from the plugin's picture tool.
- A change you can hear was **tried in a host**, after rebuilding the plugins and restarting the host.

The pull request template asks for each of these. More in
[working on bambi](AGENTS.md#working-on-bambi).

## Licence

bambi's own code is GPL-3.0-or-later, and plugin builds are distributed under the AGPLv3 ([README](README.md)).
By contributing you agree that your contribution is licensed under the same terms.
