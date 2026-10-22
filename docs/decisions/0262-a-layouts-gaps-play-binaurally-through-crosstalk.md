# 0262 · A layout's gaps play binaurally through crosstalk cancellation, as settled by ear on a pair

**Status:** accepted, revised in part by [0263](0263-the-fill-cancels-up-to-4-5-khz-at-the-decoders.md)

*2026-10-22. A laptop's two speakers could not reproduce most directions; a prototype filled them in.*

The Decoder can play the directions a loudspeaker layout does not cover binaurally on that layout's own loudspeakers,
through crosstalk cancellation, while what it covers plays as before ([DECODER-FILL](../decoder/DECODER-FILL.md)). Until
now the decoder's page called crosstalk cancellation no feature, for its sweet spot of one head; it stays a choice for
one listener, off by default, and the decoder part keeps playing as the layout's decoder.

**Why it is built this way.** In a browser prototype that used the core's own designs (the pair's decoders, the binaural
filters), on a laptop's built-in speakers 34° apart, these were chosen by ear over their alternatives: *fold*, the
downmix, in front (over amplitude panning and *slice*), an equal-power crossfade (over equal amplitude), the crossfade
applied to the ambisonic signal with 20° of decoder alone and 60° of crossfade (without the 20°, only 62 % of a source
between the loudspeakers stays on the decoder at order 3), the binaural part at order 3 (higher orders made audible
jumps in the treble on a moving source), regularisation −30 dB (eased to −25 in the plugin, where −30 sounded
phasy with the model head), cancellation up to 7 kHz, and 128 samples of delay (no
difference heard from 512). The 128 samples became possible by designing the fill's binaural filters with a 32-sample
bulk delay, which the cancellation undoes; at 32 they stay within 0.5 dB of the headphones' 64 on 95 % of bins, at 16
they do not.

**Split, not one matrix.** The decoder part, an order-3 binaural decode and a 2 × L cancellation cost (N+1)² × 2 + 2 × L
filters: 1.70 % of a core measured for 64 loudspeakers at order 7. One (N+1)² × L filter matrix designed per direction
would be exactly the decoder on every loudspeaker but is estimated at about 38 % there, and loses the treble at order 3
unless its fit reproduces MagLS.

**Tried and not taken.** Minimum-phase band edges for the cancellation: they do not sum to the identity with the
uncancelled path inside the band, and left the other ear 6.5 dB down at every delay. A causal least-squares design for a
short delay: no better than the cut inverse (35 against 36 dB at 128 samples) once the inverse's start is no longer
faded.

**Contingent on** one listener facing the layout as measured; turning the head 10° costs most of the separation, so
larger layouts want the fill to follow a head tracker. What plays outside the band beyond a pair has not been listened
to yet ([LISTENING](../LISTENING.md)). What would overturn it: an array session where the fill images worse than the
layout's decoder alone, or where another out-of-band choice plainly wins.
