# 0263 · Gap filling cancels crosstalk up to 4.5 kHz, is level-matched to the decoder, and a layout is checked at one depth

**Status:** accepted

*2026-10-22. In the plugin the fill rang where the prototype had not.*

**Revises** [0262](0262-a-layouts-gaps-play-binaurally-through-crosstalk.md)'s *"cancellation up to 7 kHz"*, and adds
what the prototype did and the plugin had left out ([DECODER-FILL](../decoder/DECODER-FILL.md)).

**Up to 4.5 kHz.** With the default depth the fill in the plugin rang, and rang longer the more depth was set; with the
fill's binaural part on headphones it did not. For a narrow loudspeaker pair the crosstalk cancellation filters have a
resonance peak, near 5.9 kHz with the model head and 7 kHz with the KU100; with the band up to 7 kHz that peak fell
inside it. Ending the band at 4.5 kHz puts the peak above it, and on the laptop it sounded much better.

**Level-matched to the decoder.** As in the prototype, in two steps: the fill's loudspeaker feeds are scaled so that a
sound from a loudspeaker's own direction has the same level through them as through the layout's decoder; then the
decoder part and the fill are scaled together, so that every direction has on average the same level as with the
decoder alone. Without the second step a laptop pair played 1.6 dB quieter with the fill, and a 5.0 layout 3.0 dB
louder, so switching the fill changed the level as well as the sound.

**Checked at one depth.** A layout was refused when it reached less than 25 dB of separation at the depth set, so a
pair was filled at a depth of 14 dB and refused at 12 dB (24.3 dB of separation): lowering the depth switched the fill
off. The check now always uses a depth of 30 dB, whatever depth is set, so whether a layout can be filled depends on
the layout alone.

**Contingent on** the resonance staying above the band for the HRTFs offered; with an HRTF from a SOFA file that
resonates lower, *up to* should be lowered, which the *fill* sub-tab allows.
