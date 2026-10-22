# Decoder: filling a layout's gaps

Most loudspeaker layouts leave part of the sphere uncovered: a pair everything off its arc, a 5.1 or 7.1 ring what is
above and below, a dome what is below. With *fill the gaps binaurally* on (the speakers page's *gaps* sub-tab; off by
default), what the layout covers plays as its decoder plays it, and what it does not cover plays binaurally on the same
loudspeakers through crosstalk cancellation, so each ear hears its own channel. It is for one listener in the sweet
spot, facing the layout as it was measured. Why, and what was tried: [0262](../DECISIONS.md).

## How it plays

- **What a layout covers** comes from its own design: for a pair, the arc between the two; for a ring, the arcs between
  real neighbours; for a dome, the AllRAD triangles whose corners are all real loudspeakers (a coplanar group's centre
  counts when its corners do). A layout that covers the sphere has nothing to fill, and the page says so.
- **Two windows on the ambisonic channels** split the ambisonic signal by direction: the layout's own decoder alone
  within *no fill within* of what it covers (20°), then an equal-power crossfade to binaural over *crossfade width*
  (60°). They are projected to the bus's order: at order 3, 99 % of a source between a pair's loudspeakers plays through
  the decoder, 0 % of one behind.
- **The decoder part** is the layout's decoder after the first window, through the speaker renderer.
- **The binaural part** is an order-3 decode of the second window, from its own build of the current HRTF (the model
  with its two sliders, the KU100 or a SOFA file) with a 32-sample bulk delay; the headphones' filters stay as they are.
- **The cancellation** takes the two ear signals to every real loudspeaker at once: per frequency the least-energy
  loudspeaker signals that give each ear its own channel, G = Cᴴ(CCᴴ + β)⁻¹, with C the HRTF from each loudspeaker to
  both ears — a 2 × 2 inversion however many loudspeakers there are and wherever they stand. 128 samples late, 384 taps,
  over 300 Hz – *up to* (7 kHz), regularised by *depth* (25 dB). Less depth lets the inversion use less gain: it cancels
  less and sounds closer to binaural played straight; more depth relies more on the listener sitting where it was
  designed and on the HRTF matching their ears, and sounds phasy where they do not. Outside the band each ear's channel
  plays uncancelled on the loudspeaker nearest 30° to its side; for larger layouts that waits on an array
  ([LISTENING](../LISTENING.md)). Each loudspeaker's distance gain and delay are folded into its feeds, as the renderer
  gives them to the decoder part.
- **On a loudspeaker's own direction** the HRTF followed by the cancellation is that loudspeaker's signal alone, so the
  crossfade between the decoder and the binaural part is continuous.
- **Refused** when the loudspeakers cannot reach 25 dB of channel separation between the ears — all on one side, or on
  the median plane; the page then shows the separation reached, and while the fill plays, the separation it keeps.
- **Latency**: 256 samples with the fill on (the two convolvers' 64 each and the cancellation's 128), 64 with it off;
  the host is told, and every output, the headphones included, waits the same. Switching it changes over with the
  renderer's crossfade: the fill's gain travels in the renderer's design and fades with it.

## Settings

On the speakers page's *gaps* sub-tab, beside the head the binaural part is made from (*hrtf*: the same choice
and sliders as the headphones page). Session state, never automated; a session without them opens with the fill off.
While the gaps of a pair are filled, its *fold · slice* is greyed on *fold*, the combination the fill was tuned with.

| on the page | state key | range | default |
|---|---|---|---|
| fill the gaps binaurally | `fill` | on, off | off |
| crossfade: no fill within | `fill_hold` | 0–60° | 20° |
| crossfade: crossfade width | `fill_fade` | 5–120° | 60° |
| crosstalk cancellation: depth | `fill_strength` (the regularisation, negated) | 10–40 dB | 25 dB |
| crosstalk cancellation: up to | `fill_up_to` | 3000–12000 Hz | 7000 Hz |

## Measured

`bambi-bench`, Release, 128-sample blocks at 48 kHz, KU100, 2026-10-22; separation with the head the filters were
designed for, mean over the band:

| layout | audio thread | window / binaural / cancellation | separation | design on the worker |
|---|---|---|---|---|
| a pair, order 3 | 0.37 % | 0.04 / 0.24 / 0.05 % | 36 dB | 0.4 ms |
| 7.1.4, order 3 | 0.56 % | 0.04 / 0.24 / 0.22 % | 39 dB | 0.7 ms |
| 64 loudspeakers on a half dome, order 7 | 1.70 % | 0.21 / 0.24 / 1.21 % | 39 dB | 3.7 ms |

The fill's own binaural build costs 3.0 ms per slider move. In the prototype, turning the head 10° cost most of the
separation (down to 4–14 dB on a pair, 5.0 and 7.1.4), so beyond a pair the fill wants head tracking, which it does not
follow yet.

Code: `core/include/bambi/decode/fill.hpp` designs; `DecoderEngine` plays it.
