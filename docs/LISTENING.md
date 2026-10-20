# Listening — what waits on ears, an array or a host

What no amount of engineering answers: it needs a real loudspeaker array, someone listening, or a host session.
Edited in place; an item leaves when it is heard, and what was heard becomes a decision or a line on the concept page.

## Needs a real loudspeaker array

**How an array session runs.** Never debug at the array: arrive with rendered files (`bambi-render`, ACN/SN3D, the
file name encoding the variable, e.g. `width_A-cap_ord3_090.wav`) and a written list of questions. One variable per
set, blind where feasible. Write findings in the room; record the decode if the rig allows it.

- **Width: coherent vs decorrelated** ([ENCODER](encoder/ENCODER.md)). The width engine is provisional on this
  ([0001](DECISIONS.md)). The first judgement was made through a decode missing a `(2n+1)`; `prototypes/encoder/width`
  is rebuilt for the re-listen: the engine's cap against a frequency-dispersed encoder and velvet-noise
  decorrelation, each with the low end kept a point. Through a dome decoder, coherent loses 6.4 dB from 0° to 180°,
  dispersed 0.1 dB, velvet 0.9 dB.
- **Does amplitude-driven motion read spatially at all?** The Encoder's premise, never tested on an array.
- **Maximum angular velocity** — where motion stops reading as a moving source and starts smearing; it becomes a hard
  clamp in the code.
- **An open arc under a rotating transform** — predicted to be the strongest gesture in the design.
- **Reverb's 3 ms spread of a reflection's copies** (`kSpreadSeconds`) — tuned through a binaural render; may want
  another value on loudspeakers ([REVERB](reverb/REVERB.md)).
- **A loudspeaker array through its filter matrix** — a vendor's FIR matrix converted to SOFA FIR-E loads but is
  unheard on the device: does it image, and is the input normalisation (N3D assumed; the page has a switch) right?
  Confirm with the matrix's author, then remove the switch.
- **The Decoder's virtual array against the real one** ([DECODER](decoder/DECODER.md)) — the same scene through
  the array and through the headphone rendering of it, from the same spot: does the headphone version predict the
  room (the empty lower half, the rear at ±145°)?
- **The Decoder's colour correction of an array** ([#7](https://github.com/Looderso/bambi/issues/7)) — the fit can be
  built blind; whether a shelf and a peak make a 7.1.4 array sound like one loudspeaker is for the room.

## Needs ears, array or not

**Encoder**
- **The stereo source modes** — the mid/side and stereo defaults, mid/side against stereo for image, and whether the
  side's negative lobe sounds phasey when wide ([0111](DECISIONS.md)).
- **Turnaround easing at ping-pong ends.**
- **Tonal on distorted or ambiguous material** — the reference library's texture and FX categories are thin (51 and
  8 files against 194 drums).
- **Full-field energy on real material at order 1**, where four channels describe the field.

**Echo**
- **Every modulation destination, heard for the first time**: spin, axis, blur, the cuts and skew — and whether a
  moving send region needs a per-sample ramp or tolerates a per-hop step ([ECHO-CONTROLS](echo/ECHO-CONTROLS.md)).
- **The one-hop period crossfade** on rhythmic material at fast tempo changes.
- **A wash at short periods and high blur** — the setting, or a fault?
- **Delay-time modulation depth** — which depths sound like character and which like a fault
  ([DELAY-TIME](suite/DELAY-TIME.md)).

**Reverb**
- **Between the six rooms** — each was matched by ear; moving away from one "sounds bad". A large room with a short
  decay put the dry up to 27 dB above the input until the room buttons stopped gliding through such rooms;
  the knobs still reach them. Fine as it is until someone complains.
- **The tail runs 1.7–3 dB quieter than the prototype's model** — match the model, or keep what is heard?
- **The −1.25 dB dip while the arriving network fills** after a size change — seed it only if heard.
- **Sectors, dots, clouds and custom in Echo's send and Reverb's send and return** — does each kind sound like its
  picture, and does a modulated fill or dot size move cleanly?

**Decoder**
- **Does nobody's head with one ear slider beat the KU100?** (`prototypes/decoder/sheets/listen.html`,
  [DECODER-HRTF](decoder/DECODER-HRTF.md)). Heard so far: a head rebuilt from its own numbers places a sound nearly
  where the measured one does; nobody's head sounds right with the notches at depth × 1.5.
  Open: whether elevation at × 1.5 holds for other listeners, and whether the head and ear sliders alone give most of
  the individual gain.
- **Do a few synthesised notches carry elevation?** (`prototypes/decoder/notches/`) — whether two or three features
  place a source as the real head does, or whether the smooth part alone already does.
- **The ear slider's fixed dip near 9.9 kHz** — audible?
- **The array on headphones** (MagLS and head rotation) and whether a diffuse-field EQ hurts externalisation.
- **A phone as the tracker on Android** — whether its browser gives the same frame, and whether yaw, pitch and roll
  turn the field the way they turn the head ([DECODER-HEAD-TRACKING](decoder/DECODER-HEAD-TRACKING.md)).

**Live host**
- **The Live mix on headphones** — only loudspeakers have been tried.
- **A saved set reopened with bambi on** — never tried.

## Needs a host session

- **REAPER checks never run**: stereo input modes, ducking and its release, the Decoder on a real bus
  (`host-reaper-check` skill, checks 12–14).
- **Logic** is untested and has no format until AU returns as its own phase.
