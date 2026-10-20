---
name: new-prototype
description: Settle a question of feel, sound, interaction or whether a design works at all in a self-contained browser prototype, before any C++ is written. Use when about to compile a plugin to answer a design question, or when asked for a prototype, a listening test or an A/B page.
---

# A new prototype

**Anything decidable without compiling a plugin is decided without compiling a plugin.** Findings port to the
docs; prototype code never ports to the plugins. A prototype is throwaway, and saying so is what makes it fast.

## Before building

State the question in one sentence and what each answer would look like. A prototype worth building can **come
out wrong** — the valuable results here were falsifications (crest factor read near zero on exactly the material
it was meant for; an onset detector with a long release was an envelope follower). Say up front when the answer
needs ears or a real array rather than a measurement; those go to `docs/LISTENING.md`.

## How it is built

- `prototypes/<plugin or suite area>/<question>/index.html` — **one self-contained file**: no build step, no
  framework, Canvas 2D, plain ES modules, Web Audio where sound is needed. No 3D engine: the plugin's views are
  hand-drawn 2D projections too, so interaction code written this way transfers.
- Python for offline analysis sits beside it (`*.py`).
- **A new question gets a new page.** Never edit, refactor or import from a finished prototype; it is not
  maintained, and results it stored are not evidence. Shared helpers made for the purpose (e.g.
  `prototypes/decoder/common.js`) are fine.
- **Audio prototypes settle behaviour, not style.** Style comes from the built UI and `Theme.h`.
- **Serve it, do not open it as a file**: the `prototypes` entry in `.claude/launch.json` (port 8765) or
  `python3 -m http.server --directory prototypes`. A file:// page has no AudioWorklet.
- For listening, default to a **plain A/B page** — free switching between versions, plots of what plays — not a
  formal blind test, unless asked for one.

## Safe levels — every page that plays sound

- Every signal kind normalised to the same RMS; defaults low; level sliders cannot exceed the default reference.
- A **hard ceiling at −12 dBFS** on every output path before the device (`safeOutput(ctx)` in
  `prototypes/decoder/common.js`) — a ceiling, not the browser's compressor, which adds make-up gain.
- A new signal type (clicks, sweeps, impulses) is the risky case: crest factor hurts, not RMS. Measure the
  worst-case peak offline before handing the page over, and state it.

## Playing to a chosen device

1. A visible "list the output devices" button: `getUserMedia({audio:true})`, stop the tracks, `enumerateDevices()`.
2. Choose the device **before** start; start opens `new AudioContext({ sampleRate, sinkId })`; a device change
   closes it and opens a new one. `destination.maxChannelCount` gives every output, each discrete.
3. A visible line says where the sound goes and any failure.

Not `ctx.setSinkId()` on an open context (stays on the system output), not an `<audio>` element (8 channels at
most). Without speakers, verify on a loopback device (BlackHole 16ch) recorded with
`ffmpeg -f avfoundation -i ":<n>" -ac 16`; drive the page with real input events — an AudioContext created from
script evaluation stays silent.

## Reporting

1. The question. 2. What the page does and how to run it. 3. The finding, with numbers, and what would overturn
it. 4. Where it belongs: the doc that owns the concept and the sentence to write there — or a decision, or a
rejected decision if it was a dead end (see the `write-doc` skill).
