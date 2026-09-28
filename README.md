# bambi

A free and open-source ambisonics plugin suite: an **Encoder**, an **Echo** and a **Reverb**, sharing one
core, one look, one modulation matrix and a link between instances. VST3 and CLAP, on macOS, Windows and
Linux.

- **Encoder** — places a mono or stereo source on the sphere and moves it along a path you draw. How fast it
  travels can follow the input: silence parks the source, loud material moves it.
- **Echo** — four loops fed from a region of the field, each placed, rotated and filtered a little further
  with every pass.
- **Reverb** — one room around the whole field: early reflections from the direction each part of it arrives
  from, and a diffuse tail.

The website is **[looderso.github.io/bambi-site](https://looderso.github.io/bambi-site/)**, and builds are on the
[releases page](https://github.com/Looderso/bambi/releases). bambi is in **alpha**: expect changes, and
report what breaks.

## Conventions

- **AmbiX**: ACN channel ordering, SN3D normalisation. FuMa only as import or export.
- **Coordinates**: right-handed, `x` front, `y` left, `z` up. Azimuth counter-clockwise from front,
  elevation positive up. Degrees at the interface and the parameters, radians everywhere inside.

## Layout

```
core/         the engines, modulation, state and the link bus: C++20, no JUCE, tested in seconds
host/         the shared plugin processor: buses, parameters, state handoff, the link
ui/ editor/   the shared interface: theme, controls, scene, matrix, the editor frame
plugins/      encoder, echo, reverb: each plugin's own processor, panel and check suite
tools/        render, bench and inspection tools, and the repository's checks
tests/        golden scenarios and the host-contract conformance suite
third_party/  vendored dependencies, and JUCE as a submodule (THIRD-PARTY.md)
```

`core/` carries no JUCE dependency on purpose: everything musical here is pure computation over buffers
and stays testable without building a plugin. How to build and test is in [BUILDING.md](BUILDING.md).

## Licence

Copyright © 2026 Lorenz Häusler and the bambi contributors.

bambi's own code is GPL-3.0-or-later ([LICENSE](LICENSE)). Plugin builds include JUCE, which bambi uses under
the AGPLv3, so they are distributed under the AGPLv3 ([LICENSE-AGPL-3.0.md](LICENSE-AGPL-3.0.md)). Every
third-party component and its licence is in [THIRD-PARTY.md](THIRD-PARTY.md) and
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

VST is a registered trademark of Steinberg Media Technologies GmbH.
