# The decoder's heads

Two head models, the format `core/include/bambi/decode/headmodel.hpp` describes: spherical-harmonic tables over an
ERB axis, read by `parseHeadModel`.

- `head.json` — nobody's head and the template of its notches, learned from the HUTUBS and 3D3A databases
  (both CC BY 4.0) in the browser; the two sliders make it the listener's.
- `ku100.json` — the Neumann KU100 as measured in SADIE II (set D1, Apache 2.0), fitted at order 20 on a 0.2-ERB
  axis by `tools/head` (`bambi-head`): no notch layer, the head slider scales only its time difference.
