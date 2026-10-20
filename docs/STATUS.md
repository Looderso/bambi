# Status

What exists now. What bambi is after: [VISION](VISION.md). Why: [DECISIONS](DECISIONS.md). How the docs are filed:
[README](README.md).

## What is built

| part | state |
|---|---|
| `core/` | everything musical, no JUCE: spherical harmonics, paths, features, modulation, regions, state, undo, parameters, the link bus, every engine; mutation-checked tests and golden renders at block sizes 1…1024 |
| shared layers | `host/` (buses, parameters, transport, the field processor), `ui/` and `editor/` (one look, one window frame, shared components and check kits) |
| Path Encoder | built — trajectories, transform, width, input modes, modulation, the scene ([ENCODER](encoder/ENCODER.md)) |
| Echo | built — taps of the field by direction, four loops ([ECHO](echo/ECHO.md)) |
| Reverb | built — image-source reflections and a feedback-delay tail read from the bus ([REVERB](reverb/REVERB.md)) |
| Decoder | built — binaural, loudspeakers, a pair; head tracking ([DECODER](decoder/DECODER.md)) |
| live host | built, macOS — Live plays through the suite on bambi's loop device ([LIVE-HOST](suite/LIVE-HOST.md)) |
| formats and platforms | VST3 and CLAP; macOS, Windows and Linux built and checked in CI; release zips on a `v*` tag ([PORTABILITY](suite/PORTABILITY.md)) |
| hosts | checked in REAPER in both formats ([HOST-CHECKS](HOST-CHECKS.md)); Live through the live host; Logic waits for AU |

Costs are in [status/MEASURED.md](status/MEASURED.md), each with the run it came from.

## What is next

The first release: what must be true before it ships — the one-way doors (identity, bus layout, parameter keys),
packaging, signing, the licence, the website — is [RELEASE](RELEASE.md).

## Where open work lives

| what | where |
|---|---|
| pressing work, bugs | [GitHub issues](https://github.com/Looderso/bambi/issues) |
| ship blockers | [RELEASE](RELEASE.md) |
| waits on ears, an array or a host session | [LISTENING](LISTENING.md) |
| an idea or a feature you need | open an issue and say what for — ideas nobody has asked for are not tracked as issues |
| dead ends | decisions with status *rejected* in [DECISIONS](DECISIONS.md) |
