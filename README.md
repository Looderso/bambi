# bambi

Intuitive yet powerful spatialization: a free and open-source ambisonics plugin suite. Every plugin shares one
core, one look, one modulation matrix and a link between instances, and works in AmbiX at any order the host
carries. VST3 and CLAP, on macOS (14 and later), Windows and Linux. The plugins today: the **Encoder** places a source, **Echo**
and **Reverb** are effects on the field, and the **Decoder** is the monitor — binaural with head tracking, or a
loudspeaker layout. The Decoder's AirPods and phone tracking need macOS, where a helper app reads them; OSC
tracking works everywhere.

The website is **[bambi.wiki](https://bambi.wiki)**, with what each [plugin](https://bambi.wiki/plugins)
does, and builds are on the [releases page](https://github.com/Looderso/bambi/releases). bambi is in **alpha**:
expect changes, and report what breaks.

## Conventions

- **AmbiX**: ACN channel ordering, SN3D normalisation. FuMa only as import or export.
- **Coordinates**: right-handed, `x` front, `y` left, `z` up. Azimuth counter-clockwise from front,
  elevation positive up. Degrees at the interface and the parameters, radians everywhere inside.

## Layout

```
core/         the engines, modulation, state and the link bus: C++20, no JUCE
host/         the shared plugin processor: buses, parameters, state handoff, the link
ui/ editor/   the shared interface: theme, controls, scene, matrix, the editor frame
plugins/      one folder per plugin: its own processor, panel, check suite and picture tool
tools/        render, bench and inspection tools, and the repository's checks
tests/        golden scenarios and the host-contract conformance suite
docs/         what is true now, and why: concept pages and the decision log
.claude/      skills (recurring procedures) and reviewer agents, usable with any coding agent
third_party/  vendored dependencies, and JUCE as a submodule (docs/THIRD-PARTY.md)
```

`core/` carries no JUCE dependency on purpose: everything musical here is pure computation over buffers
and stays testable without building a plugin. How to build and test is in [docs/BUILDING.md](docs/BUILDING.md), how
to contribute in [CONTRIBUTING.md](CONTRIBUTING.md), and what a coding agent needs in [AGENTS.md](AGENTS.md).

## Licence

Copyright © 2026 Lorenz Häusler and the bambi contributors.

bambi's own code is GPL-3.0-or-later ([LICENSE](LICENSE)). Plugin builds include JUCE, which bambi uses under
the AGPLv3, so they are distributed under the AGPLv3 ([LICENSE-AGPL-3.0.md](LICENSE-AGPL-3.0.md)). Every
third-party component and its licence is in [docs/THIRD-PARTY.md](docs/THIRD-PARTY.md) and
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

VST is a registered trademark of Steinberg Media Technologies GmbH.
