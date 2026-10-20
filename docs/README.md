# Docs

bambi is a suite: an **encoder** that places sources on the sphere, and **effect plugins** that work on the
ambisonic field itself — **Echo** and **Reverb** ([](DECISIONS.md)), with Colour and the Decoder
later. Documents are filed by what they apply to.

| where | what |
|---|---|
| [VISION.md](VISION.md) | what the suite is, what is shared, how this project works |
| [STATUS.md](STATUS.md) | the entry point: what exists, what is measured, what is next |
| [DECISIONS.md](DECISIONS.md) | the index of the one numbered log for the whole suite; each entry is its own file in [decisions/](decisions/) |
| [status/](status/) | the longer record behind STATUS: [measured](status/MEASURED.md) |
| [suite/](suite/) | shared by every plugin: [the look](suite/LOOK.md), [the scene](suite/VISUALISATION.md), [modulation](suite/MODULATION.md), [regions](suite/REGIONS.md), [changing a delay time](suite/DELAY-TIME.md), [the picker](suite/PICKER.md), [presets](suite/PRESETS.md), [portability](suite/PORTABILITY.md), [the live host](suite/LIVE-HOST.md) |
| [encoder/](encoder/) | bambi Encoder: [features](encoder/ENCODER.md), [sources and the matrix](encoder/ENCODER-SOURCES.md), [the window](encoder/UI.md) |
| [echo/](echo/) | bambi Echo, taps of the field by direction: [the model](echo/ECHO.md), [engine](echo/ECHO-ENGINE.md), [controls](echo/ECHO-CONTROLS.md), [visualisation](echo/ECHO-VISUALISATION.md) |
| [reverb/](reverb/) | bambi Reverb, the room: [REVERB.md](reverb/REVERB.md) |
| [decoder/](decoder/) | the Decoder, headphones and loudspeakers: [DECODER.md](decoder/DECODER.md), [binaural](decoder/DECODER-BINAURAL.md), [head tracking](decoder/DECODER-HEAD-TRACKING.md), [HRTF data and the head model](decoder/DECODER-HRTF.md) |

## Rules for filing

- **A document lives in the narrowest folder that covers everything it says.** Anything two plugins share goes
  in `suite/`, even if only one plugin exists yet.
- **Decisions stay in one log** (an index plus a file per entry), so a number always means one thing. Code never cites one.
- **File names are unique across the folders**, so a bare name stays unambiguous: `UI.md` is the
  encoder's, and an effect's will be `ECHO-UI.md`.
- **Prototypes mirror this:** `prototypes/encoder/`, `prototypes/echo/`, `prototypes/reverb/`, plus
  `prototypes/regions/` and `prototypes/ui/` for what serves both effects. Findings port to these docs;
  prototype code does not port to the plugins.
- **A decision that changes gets a new number**, and the old entry gains a "revised by" line rather than being
  rewritten.
