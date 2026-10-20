---
name: conformance-suite
description: Check, extend or run bambi's layout and sidechain conformance suite, or answer what a plugin accepts — buses, channel layouts, ambisonic orders, sidechain routing, VST3 vs CLAP. Use before promising that a plugin accepts a layout or a sidechain, and when adding a plugin.
---

# The conformance suite

Every bambi plugin **negotiates its ambisonic layout in both formats, in a real host**, and the encoder and the
effects **accept a sidechain**.

## The rule

- **Main input: fixed.** Mono or stereo for the encoder; (N+1)² for the field effects.
- **Main output: ambisonic**, at the highest order that fits the channel count, the rest silent.
- **Sidechain: a real aux bus**, never a wide main bus read past its first pair. A wide main input is refused, so
  a send on channels 3/4 can only be the sidechain.
- **The Decoder** takes the field as its one input bus, has no sidechain, and its output is its loudspeaker array or
  headphone pair.

| | order ceiling | layout |
|---|---|---|
| VST3 | 7th (a format limit; core is verified far higher) | `kAmbi1stOrderACN` … `kAmbi7thOrderACN` |
| CLAP | none | the ambisonic port extension, ACN / SN3D, via the patched vendored `clap-juce-extensions` (`THIRD-PARTY.md`) |

AmbiX: ACN ordering, SN3D normalisation; x front, y left, z up. REAPER tracks have only even channel counts.

## Running and extending

`tools/conformance.sh` runs `bambi-conformance`, which asks every plugin the same questions of its sources and of
its installed VST3 and CLAP, loaded as a host loads them. Extend `bambi-conformance`
(`tests/conformance/Conformance.cpp`) rather than writing a new harness. The checks:

1. Every claimed layout is accepted; every unclaimed one is **refused**.
2. A wide main input is refused.
3. The sidechain arrives, sample for sample, at every supported order.
4. Unused output channels are silent.
5. Renders are bit-identical at block sizes 1…1024.
6. An offline bounce equals realtime in every sample.

**A new plugin joins** by a subject entry (name, factory, kind) in `tests/conformance/main.cpp` and its
`bambi-<name>-code` library in `tests/CMakeLists.txt`; the checks stay the same. Exit code 0 means every check passed. A
check that cannot fail is not a check — break the plugin deliberately once to see it fail.

## What a headless check cannot answer

Bus negotiation in a real DAW, pin connectors, track channel counts and automation in a bounce need a host and a
person; the `host-reaper-check` skill is that round (about ten minutes in REAPER). Say so rather than implying
coverage. Never report a layout as supported on the strength of `isBusesLayoutSupported` alone — that is the
plugin's claim, not the host's behaviour.

## Reporting

The checks that ran and passed; for a failure, the layout, format, order, exact numbers, and whether the cause is
ours or the wrapper's.
