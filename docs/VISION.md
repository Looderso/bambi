# bambi — the suite

What the suite is, what its plugins share, and what each owns alone. [STATUS](STATUS.md) is what exists,
[DECISIONS](DECISIONS.md) is why. How the project works is in [AGENTS.md](../AGENTS.md).

## The point

**Design each thing once.** A suite that reuses one slider, one matrix and one undo stack is cheap to extend; one
that redraws them per plugin is several codebases wearing a family name. The shared thing is the reference and a
plugin may not fork it: if a plugin needs a component to behave differently, the component grows an option. A copy
is a defect.

## The plugins

| plugin | domain | engine |
|---|---|---|
| **Encoder** | mono or stereo in, ambisonic out | places a source on the sphere along an authored trajectory, moved by the input's own features, LFOs and envelopes; sum, mid/side or stereo source modes |
| **Echo** | ambisonic in and out | four loops fed by one send region, each placed, rotated, skewed and order-cut as it repeats ([echo/ECHO.md](echo/ECHO.md)) |
| **Reverb** | ambisonic in and out | one room: image-source early reflections read from the bus, and a feedback-delay-network tail ([reverb/REVERB.md](reverb/REVERB.md)) |
| **Decoder** | ambisonic in, headphones or speakers out | binaural with head tracking, loudspeaker decoding over a loaded or typed layout; a monitor, not an effect ([decoder/DECODER.md](decoder/DECODER.md)) |

The **live host** (`apps/live-host/`) is an app, not a plugin: it plays an Ableton Live set's tracks through the
installed plugins ([LIVE-HOST](suite/LIVE-HOST.md)). A *Colour* plugin (decode, process per direction,
re-encode) is planned.

Echo and Reverb are separate plugins because the engine changes there: an echo is a delay buffer read through
gain matrices, a reverb is an image-source model plus a feedback delay network. They share regions, not code
paths, and chain when both are wanted.

Docs and prototypes are filed under the plugin they belong to: `docs/<plugin>/`, `prototypes/<plugin>/`. What two
plugins share lives in [suite/](suite/).

## What is shared

The boundary is not what looks similar; it is **what would have to change in two places if it changed at all.**

### Core

`core/` carries no JUCE and builds and tests in seconds: spherical harmonics, trajectories, features, modulation,
regions, state, undo, parameters, the link bus, the scene geometry and every engine. Each engine is its own
library inside it (`bambi-encode`, `bambi-echo`, `bambi-reverb`, `bambi-decode`); a plugin links only its own.
Shared by the field effects: **regions** ([suite/REGIONS.md](suite/REGIONS.md)), **delay lines over SH channels**
and what a delay time that changes may do ([suite/DELAY-TIME.md](suite/DELAY-TIME.md)), and the **energy summary**
the visualiser reads.

### Infrastructure

One implementation for every plugin:

- **Undo / redo**: snapshot undo over the patch.
- **State and parameters**: a key list per plugin read by name, so growth needs no migration. The pattern is
  shared; each key list is its own.
- **State to the audio thread** by immutable snapshot, lock-free, no allocation in `processBlock`.
- **The host layer** (`host/`): buses, parameters, transport, the 256-sample control grid, and the field
  effects' processor, written once.
- **Ducking is a row in the shared matrix**, not a module: Level, from the input or the sidechain, onto `wet`
  with a negative depth ([suite/MODULATION.md](suite/MODULATION.md)).
- **Regions**: the concept, code and parameter layout are shared, not the objects. A region is a plugin's own, a
  fixed set per plugin whose host parameters carry the same keys, names and ranges everywhere. No instance's
  audio depends on another's region; a region moves between instances as a clipboard copy
  ([suite/REGIONS.md](suite/REGIONS.md)).

### The look

[suite/LOOK.md](suite/LOOK.md) is the checklist; the enforcement is one implementation.

- **`Theme.h`** holds every colour, size, stroke, radius and spacing as a named role; `tools/check-style.sh`
  fails on a literal anywhere else.
- **Controls**: one bar for every value, a value is one box, a value says what you set and a mark says where the
  engine has it.
- **Layout**: scene top left, matrix below it, the plugin's tabs on the right, meters far right.
- **Header**: wordmark, plugin name, undo/redo, the plugin's instances as tabs, then preset, link and settings.
  Presets are the whole sound, files of the user's, one browser in every plugin ([suite/PRESETS.md](suite/PRESETS.md)).
- **Settings page**: one frame in every plugin (the instance's name, the link session, the order the host gave
  it), extended with what that plugin alone needs. No normalisation: SN3D only.
- **Scene**: globe and equirect, graticule, trajectories, nodes, dots, regions: one component, N projections.
- **The probe**: point at a direction and see what this plugin would do to it. One component; each plugin
  supplies the answer.
- **Energy picture**: toggleable in every plugin from the same per-block covariance summary. **Orange is what
  came in; blue is what this plugin adds**, both theme roles ([suite/VISUALISATION.md](suite/VISUALISATION.md)).
- **Repeating patterns are one component**: synced-versus-free against host tempo is one control in the LFOs, in
  Echo's timing and in its taps.

### The modulation matrix

The component and the arithmetic are shared; the rows each plugin offers are its own
([suite/MODULATION.md](suite/MODULATION.md)). LFOs and envelopes are in every plugin. **Features are always
computed locally**: the encoder's from its input, a field effect's from the whole field (`fieldPower`, the sum of
every channel's square over order + 1, which cannot cancel the way W can), with an optional sidechain as a second
detector input.

## What each plugin owns

Two things: **its engine** and **its parameter set**. Everything else is a call into the shared layers. A third
thing on this list is the signal to ask whether it belongs above.

## The link between instances

One bus, one session, all kinds of instance on it (`core/include/bambi/link/`). Transport is machine-local shared
memory keyed by the session's uuid, so two open projects never meet; liveness is a heartbeat. It carries what an
instance is, where its source is now, a window's edits to another instance (applied through the target's own host
parameter, so they stay automatable and undoable) and its controls. **Definitions travel, values do not**: no
instance's audio depends on another's live value, because a freeze or stem render processes tracks separately and
a bounce must match what was heard. Feature values never cross it. Nothing on the audio thread touches the bus.

## Formats and hosts

**VST3 and CLAP**, on macOS (a universal arm64 and x86-64 binary), Windows and Linux; CI builds and checks all
three. AU is not built.

| | ambisonic layout | sidechain |
|---|---|---|
| **VST3** | the format's ambisonic arrangements, orders 1 to 7 (`kAmbi1stOrderACN` … `kAmbi7thOrderACN`) | aux input bus |
| **CLAP** | the ambisonic port extension, ACN / SN3D, through a patched `clap-juce-extensions` ([THIRD-PARTY.md](../THIRD-PARTY.md)) | aux input port |

The host layer offers orders 1 to 10 (up to 121 channels, reachable through CLAP) and takes the highest order whose
channels fit; core is verified to order 35. One rule holds in every format:

- **Main input fixed**: mono or stereo for the Encoder, (N+1)² for the field effects and the Decoder.
- **Main output ambisonic** (the Encoder and the effects), at the highest order that fits; the Decoder's output is
  its array or pair.
- **Sidechain a real aux bus** (the Encoder and the effects), never a wide main bus read past its first pair. The
  Decoder has none.

One conformance suite (`tests/conformance/`) runs the same six checks on every plugin in both formats: the
layouts it claims are accepted and the rest refused, a wide main input is refused so channels 3/4 can only be the
sidechain, the sidechain arrives at every order, unused outputs are silent, renders are bit-identical across
block sizes, and an offline bounce equals realtime. A new plugin joins by declaring its buses.

**A bounce matches what was heard on the machine that heard it**: bit-identity is a promise within one build, not
across platforms ([suite/PORTABILITY.md](suite/PORTABILITY.md)).

### Framework and licence

The plugins are built on JUCE. bambi's own code is GPL-3.0-or-later; plugin binaries include JUCE under the AGPLv3
and ship under it. The editor's JUCE surface is small (drawing goes through `Theme.h` and `Draw.h`, no stock
widgets), so a framework swap would touch the shared UI and host libraries, not the engines.

## The repository

| path | what |
|---|---|
| `core/` | JUCE-free, layered `math io < dsp path region < patch < mod < link scene encode echo reverb decode < live` |
| `host/` | what every plugin does with a host |
| `ui/`, `editor/` | the shared component library and look; the shared window frame where ui meets host |
| `plugins/<name>/` | each plugin's processor, editor, check suite and picture tool |
| `apps/live-host/`, `platform/`, `driver/` | the live host, its device and loop layer, the loop driver |
| `tests/` | the conformance suite and goldens |
| `tools/` | headless renderers, benches, checks, scripts |
| `docs/`, `prototypes/` | concept pages and decisions; browser prototypes, whose findings port and whose code does not |

## How the project works

The rules are in [AGENTS.md](../AGENTS.md). Reviewer agents and procedures for recurring tasks are in
`.claude/agents/` and `.claude/skills/`, listed in [CLAUDE.md](../CLAUDE.md).

Background: decisions 0001, 0006, 0012, 0026, 0029, 0030, 0031, 0035, 0038, 0111.
