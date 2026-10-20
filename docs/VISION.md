# bambi — what we are after

**Immersive 3D audio that anyone can make, with what they already own.**

Spatial audio has been the domain of specialists: loudspeaker domes, measured heads, tools that expect you to think
in harmonics before you hear anything. bambi is a free and open-source suite of ambisonics plugins that turns that
around — a beginner places and moves sound in minutes, and someone who knows ambisonics still finds every setting
they need.

## Quick to start, deep when you need it

- **The first minutes work without knowing the theory.** A source moves along a path you draw; a room is a button;
  the scene shows what you hear. Sensible defaults, nothing to configure before the first sound.
- **Depth is one step away, never in the way.** Orders, regions, the modulation matrix, decoder weightings,
  measured heads and filter matrices are all there for those who want them — behind the same controls, not in a
  second expert mode.
- **One way of working across every plugin.** Every plugin looks and behaves the same: one slider, one matrix, one
  scene, one undo. Learn one and you know them all — and each new plugin joins that family rather than inventing
  its own.
- **Movement that follows the music.** Sound is moved by the audio itself — its level, attack, tone — and by LFOs
  and envelopes, so space becomes part of the arrangement, not a static mix setting.

## With what you already own

Immersive audio should not start with buying hardware. Everything bambi needs to be heard in 3D is meant to work
with what is already on a desk or in a pocket:

- **Headphones** are the first monitor: binaural decoding, with a head that can be personalised by ear with a couple
  of sliders instead of a measurement lab.
- **AirPods** follow the head through a small helper app; **a phone** becomes a head tracker through a web page.
- **A phone's camera** helps lay out real loudspeakers: the tools on the website turn pictures of a room into a
  loudspeaker layout for the Decoder.
- **The DAW you use**: VST3 and CLAP on macOS, Windows and Linux, and Ableton Live through the live host.

The same session scales up unchanged: from headphones to a home setup to a full loudspeaker array.

## What holds it together

- **What you heard is what you bounce.** An offline render matches realtime sample for sample; nothing depends on
  another plugin's live state.
- **Listening decides.** Feel and sound are settled by ear in quick prototypes before they are built; numbers are
  measured, not assumed.
- **Open, and built to be worked on.** AGPL, and everything a contributor — or their coding agent — needs is in the
  repository: [AGENTS.md](../AGENTS.md) for the rules, the concept pages for what is true now, the decisions for why.

## Where it is going

More plugins in the same family (a direction-dependent *Colour* is the next idea), the first release and its
website, and a suite that grows by contributions rather than by one person. What exists today is in
[STATUS](STATUS.md); what must be true before the first release is in [RELEASE](RELEASE.md).

Background: decisions 0006, 0012, 0038, 0165, 0170.
