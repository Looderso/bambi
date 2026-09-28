# Third-party code

bambi is licensed **GPL-3.0-or-later**. Every dependency below is under a permissive
licence that is one-way compatible with GPLv3: permissive code may be combined into a
GPL work, and the combined result is distributed under the GPL. None of them is copyleft,
so none imposes conditions on the rest of bambi, and none is incompatible with GPLv3 (no
BSD-4-clause advertising clause, no CDDL; Apache-2.0 conflicts only with GPLv2).

**JUCE** is the exception, and it arrives with the plugin rather than the core. It is AGPLv3
(or commercial). GPLv3 explicitly permits combining GPLv3 code with AGPLv3 code (section 13),
so plugin binaries are distributed under AGPLv3 terms while everything in `core/` stays
GPL-3.0-or-later.

**clap-juce-extensions** (MIT) arrives with it, for the CLAP format, which JUCE does not build
natively, and it is the only plugin format that carries ambisonics above 7th order.

The obligation these licences *do* impose is attribution: their copyright and permission
notices must travel with any distribution, source or binary. That is what the `LICENSE`
file beside each vendored copy is for, and why they are committed rather than fetched.

**JUCE brings its own vendored libraries**, listed with their licences in
`third_party/JUCE/JUCE.spdx.json`. The ones compiled into the plugins are zlib, libpng, the IJG JPEG
library, HarfBuzz, SheenBidi (Apache-2.0), LunaSVG, PlutoVG, FLAC, Ogg Vorbis and the VST3 SDK (MIT); all
permissive. Those JUCE switches off or bambi never builds — ASIO, AAX, Audio Unit, LV2, Oboe — are not.
`THIRD-PARTY-NOTICES.txt` carries every notice the plugins need and ships in every release;
`tools/release/notices.py` writes it from the bill of materials, and `--check` says when it is stale.

**A release carries its complete source**, JUCE included, as the AGPL requires of the binaries: the
pipeline attaches a source archive to every release.

pffft ships no version number of its own, so the pinned copy is identified by content hash.
That is the only identifier that actually distinguishes one snapshot from another here.

Two exceptions to the rules below, both confined to plugin builds:

- **JUCE is a git submodule**, pinned to the 9.0.2 tag, because it is far too large to commit as
  a copy. It is never needed to build or test `core/`.
- **clap-juce-extensions is vendored *and patched*.** Upstream cannot describe an ambisonic port
  over CLAP — it asserts mono or stereo and leaves `port_type` unassigned otherwise — and CLAP is
  the only format that carries ambisonics above 7th order. The changes are
  generic, marked `bambi patch` in the source, and listed in
  `third_party/clap-juce-extensions/bambi-PATCHES.md`, so they can go upstream.

Everything is **vendored** — committed into this repo at a pinned version, not fetched at
configure time. A build that reaches the network is a build that breaks later, and a
dependency that can change under you is a dependency that can change your test results.

| Component | Version | Licence | GPL-3 compatible | Used for |
|---|---|---|---|---|
| [pffft](https://bitbucket.org/jpommier/pffft) | unversioned; `pffft.c` md5 `8d86728f` | BSD-3-Clause (FFTPACK) | yes | real FFT for the spectral features |
| [dr_wav](https://github.com/mackron/dr_libs) | 0.14.6 | Public domain **or** MIT-0 | yes | WAV read/write in the offline tools |
| [yyjson](https://github.com/ibireme/yyjson) | 0.13.0 | MIT | yes | preset / state parsing and serialisation |
| [doctest](https://github.com/doctest/doctest) | 2.4.11 | MIT | yes | test framework (test binary only, not shipped) |
| [JUCE](https://github.com/juce-framework/JUCE) | 9.0.2 (submodule, pinned) | AGPLv3 or commercial | yes, via GPLv3 §13 | the plugin: formats, buses, parameters, editor — plugin builds only, never `core/` |
| [clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions) | `9fbefae`, **vendored with bambi patches** | MIT | yes | the CLAP format, which JUCE does not build natively |
| [clap](https://github.com/free-audio/clap) | 1.2.7 (inside clap-juce-extensions) | MIT | yes | CLAP headers |
| [clap-helpers](https://github.com/free-audio/clap-helpers) | `a61bcdf` (inside clap-juce-extensions) | MIT | yes | CLAP plugin scaffolding |

`doctest` is a build-time dependency of `bambi-tests` and is not linked into anything that
ships, but its notice is preserved anyway because the source distribution contains it.

## What is deliberately *not* a dependency

Three pieces of numerical code here are written by hand. Each is a considered choice
rather than an oversight, and each is stated so it can be revisited:

- **`sh.cpp` — spherical harmonics (ACN/SN3D).** No general library computes *this*
  convention. The normalisation, the channel ordering, and specifically the **absence of
  the Condon–Shortley phase** are ambisonics conventions; a general-purpose `sph_harm`
  gets all three wrong for our purposes, and wrong in a way that preserves magnitudes and
  so hides from every test that is not an explicit sign check. It is O(order²) via the
  standard recurrences, and it is asserted against hardcoded closed forms to 1e-12.
- **`legendre.cpp` — Legendre polynomials, cap weights, max-rE.** `legendreP` is the
  three-term recurrence, which is what Boost.Math's `legendre_p` also is; at our degrees
  (n ≤ 8) an extra ~150k lines of headers buys nothing but compile time. `capWeights` and
  `maxRE` are ambisonics-specific and exist in no library. All three run at control rate,
  not per sample.
- **`trajectory.cpp` / `authoring.cpp` — Bézier evaluation, arc-length LUT, curvature-based
  node placement.** These are the product, not infrastructure.

The line this project applies: **hand-rolled numerics need a reason that is about
correctness or convention, never about effort.** Performance is not a valid reason to
write your own — that argument was tried here for the FFT, and the hand-written version
was both 20× slower than the library and numerically wrong.
