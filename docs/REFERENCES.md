# References

What bambi rests on: the papers and standards its methods come from, and the data its heads are learned from.
Grouped by what uses them. Where a method is implemented from the paper rather than from a library, the file
is named.

## Ambisonics

- ITU-R BS.2051, *Advanced sound system for programme production*. The ten loudspeaker layouts built into the
  Decoder, by name, with the recommendation's channel order (`core/src/decode/layouts.cpp`).
- F. Zotter and M. Frank, "All-round ambisonic panning and decoding", *Journal of the Audio Engineering
  Society* 60 (10), 2012. AllRAD: the loudspeaker decoder designed from the positions alone
  (`core/src/decode/allrad.cpp`), with the max-rE weighting the Decoder calls *audience*.
- F. Zotter, T. Riedel and M. Frank, DAGA 2022 — on what mixing imaginary loudspeakers back into their
  neighbours costs in effective order; the reason the Decoder drops them instead.
- M. Frank et al., DAGA 2023 — the spectral correction of an AllRAD decode by one shelf and one peak, planned as
  the array's colour correction.
- H. Pomberger and F. Zotter, "Warping of 3D ambisonic recordings", Ambisonics Symposium 2011. The vertical
  warp Echo's taps use.
- C. Schörkhuber, M. Zaunschirm and R. Höldrich, "Binaural rendering of ambisonic signals via magnitude least
  squares", DAGA 2018; and M. Zaunschirm, C. Schörkhuber and R. Höldrich, "Binaural rendering of Ambisonic
  signals by head-related impulse response time alignment and a diffuseness constraint", *JASA* 143 (6), 2018.
  MagLS with the diffuse-field covariance constraint: the Decoder's binaural decode (`core/src/decode/binaural.cpp`).

## Heads

- F. Brinkmann et al., "A cross-evaluated database of measured and simulated HRTFs including 3D head meshes,
  anthropometric features, and headphone impulse responses", *JAES* 67 (9), 2019 — the HUTUBS database, one of
  the two the Decoder's learned head comes from.
- R. Sridhar, J. G. Tylka and E. Choueiri, "A database of head-related transfer functions and morphological
  measurements", AES Convention 143, 2017 — the 3D3A database, the other.
- C. Armstrong et al., "A perceptual evaluation of individual and non-individual HRTFs: a case study of the
  SADIE II database", *Applied Sciences* 8 (11), 2018 — the SADIE II measurement of the Neumann KU100, the
  Decoder's second head, under Apache 2.0.
- AES69, *AES standard for file exchange — Spatial acoustic data file format* (SOFA). How a measured head
  arrives, read with libmysofa.
- A 2024 master's thesis on perceptual HRTF individualisation (KUG Graz): the split of a head into what all
  heads share and what is one head's, the two sliders, and the personalisation procedure against a real
  loudspeaker. The Decoder's head model starts from it.
- D. Ausili et al., 2025 — what an earbud's transparency mode does to the interaural level difference and the
  spectral cues; why the personalisation procedure asks for open headphones.

## Rooms

- J. B. Allen and D. A. Berkley, "Image method for efficiently simulating small-room acoustics", *JASA* 65 (4),
  1979. Reverb's early reflections.
- C. F. Eyring, "Reverberation time in 'dead' rooms", *JASA* 1 (2A), 1930. The absorption behind Reverb's decay.
- M. R. Schroeder and B. F. Logan, "'Colorless' artificial reverberation", *JAES* 9 (3), 1961. The rule of
  thumb for the tail's total delay.
- J.-M. Jot and A. Chaigne, "Digital delay networks for designing artificial reverberators", AES Convention 90,
  1991. The feedback delay network and its tonal correction.
- A. Lindau, L. Kosanke and S. Weinzierl, "Perceptual evaluation of model- and signal-based predictors of the
  mixing time in binaural room impulse responses", *JAES* 60 (11), 2012. Where early reflections hand over to
  the tail.

## Formats read and written

- IEM Plug-in Suite, AllRADecoder: its JSON configuration, the layout file the Decoder reads first.
- SPARTA (L. McCormack and A. Politis, "SPARTA & COMPASS: real-time implementations of linear and parametric
  spatial audio reproduction and processing methods", AES Conference on Immersive and Interactive Audio, 2019):
  the same layout files, and the OSC orientation forms the Decoder's tracker reads.
- AmbDec (F. Adriaensen): its decoder configuration, read as a layout.
- jconvolver (F. Adriaensen): the filter sets a loudspeaker array's maker ships, converted to SOFA FIR-E.
- AutoEq (J. Pasanen): the headphone corrections the Decoder is to read, in their parametric and FIR forms.

The vendored libraries and their licences are in [THIRD-PARTY.md](THIRD-PARTY.md).
