# 0169 · The model ships learned from HUTUBS and 3D3A

**Status:** accepted

*2026-10-07. The owner, after hearing the two models side by side: "similar enough … the one without SONICOM is
totally fine; the higher notch is actually more pronounced."*

**Nobody's head and the template are learned from HUTUBS and 3D3A alone** (`prototypes/decoder/sheets/model/
generic-hutubs-3d3a.json`, `template-hutubs-3d3a.json`), both CC BY 4.0. SONICOM's licence is unclear
([HRTF-DATA §1](../decoder/DECODER-HRTF.md)); learning without it moves nobody's head by a third of a head-to-head
difference and the template's notches by 0.1–0.4 kHz, and the template fits SONICOM's heads, which it never saw, as
well as its own held-out ones ([HRTF-DATA §7.8](../decoder/DECODER-HRTF.md)). The model learned from all three stays
beside it for comparison and is not shipped.

**Contingent on:** 0168. **Would overturn:** a cleared SONICOM licence with a measured gain from its heads.
