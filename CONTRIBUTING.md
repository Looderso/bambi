# Contributing to bambi

Thank you for helping. bambi is three plugins built as one program, so a change is held to rules that keep
the suite uniform. The rules, and how bambi is designed, are on the website:
**[bambi.wiki/contribute](https://bambi.wiki/contribute)**. This file is the short version.

## Reporting a problem

Open an [issue](https://github.com/Looderso/bambi/issues) with:

- the plugin, its version (the release name) and its format, VST3 or CLAP;
- your system and your host, with their versions;
- what you did, what you expected, and what happened instead;
- a session or a short audio file that shows it, if you can.

## Before you write code

**Open an issue first** for anything that changes the design, how something behaves for a musician, or a
plugin's parameter keys. Hosts save sessions by those keys, so they are the one thing that cannot be taken
back lightly. A bug fix, a test or a cleanup can go straight to a pull request.

## Building and checking

```bash
git clone --recurse-submodules https://github.com/Looderso/bambi
cmake -B build -G Ninja && cmake --build build && ./build/bambi-tests
cmake -B build-plugin -G Ninja -DBAMBI_BUILD_PLUGIN=ON && cmake --build build-plugin
tools/verify.sh
```

What you need on each system is in [BUILDING.md](BUILDING.md). A change is ready when `tools/verify.sh`
passes.

## The rules, in brief

- **The musical logic lives in `core/`**, which has no JUCE and is tested without a host. A plugin owns its engine
  and its parameter list; everything else is shared. [Where code lives](https://bambi.wiki/contribute/where-code-lives)
- **Design each thing once.** A copy of a shared component is a defect: the component grows an option instead.
- **Nothing on the audio thread allocates, frees or locks, and a bounce matches what was heard.**
  [The audio thread](https://bambi.wiki/contribute/the-audio-thread)
- **Tests are mutation-checked.** Break the code the way a real mistake would, see the test fail, and name the
  mistake in a `Catches:` comment. [Tests and checks](https://bambi.wiki/contribute/tests)
- **Comments say what the code does**: units, invariants, threads, a reason true now. Never history, names or
  references. [Code style](https://bambi.wiki/contribute/code-style)
- **Colours, sizes and strokes live in the theme file** and nowhere else. [The look](https://bambi.wiki/contribute/the-look)

## Commits and pull requests

- A change of form and a change of behaviour are **separate commits**.
- A commit's subject is a sentence that says how things are now: "A stale shared segment is refused at once on
  macOS", not "fix segment bug".
- Say what the change did to the **golden renders**: unchanged, or which moved and why.
- A change to the window comes with a **picture** from the plugin's picture tool.
- A change you can hear was **tried in a host**, after rebuilding the plugins and restarting the host.

The pull request template asks for each of these. More in
[making a change](https://bambi.wiki/contribute/making-a-change).

## Licence

bambi's own code is GPL-3.0-or-later, and plugin builds are distributed under the AGPLv3 ([README](README.md)).
By contributing you agree that your contribution is licensed under the same terms.
