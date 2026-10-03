"""Does a masker take a source's height, and do cue protection or spectral enhancement give it back?

A target, a train of 40 ms pink-noise bursts for 0.9 s, at one of nine places on the median plane (polar angle −30°
front-below, 0 front, 30, 60, 90 overhead, 120, 150, 180 behind, 210 behind-below); a steady pink-noise masker at the
side (azimuth 60°, on the horizon) at a fixed target-to-masker ratio. Both through their own KU100 HRTFs (objects), in
six conditions:
  quiet              the target alone — what the HRTF itself allows
  quiet, enhanced    the target alone, enhanced
  masked             target and masker
  protected          target and masker; the masker cleared from the target's notch bands where the target leads
  enhanced           target and masker, both through enhanced HRTFs
  enhanced, protected
Enhanced: the spectral-contrast enhancement of Brungart and Romigh (2009). Per direction, the CLL in dB less its mean
over the cone of confusion (the directions at the same lateral angle), scaled by ALPHA and added back — the same gain
to both ears, so ILD, ITD and phase stay; capped at ±20 dB.

Writes out/elevation/<condition>-<polar>.wav, one fixed scale for all (the masked mix about −30 dBFS RMS, peaks under
−12 dBFS), and manifest.json for elevation.html; prints each condition's notch fill in the target's notch bands.

  python elevation.py <KU100 sofa> <out directory> [target-to-masker dB, default 0] [alpha, default 1.5]
"""
import json, os, sys
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np
from scipy.signal import stft, istft, lfilter
from scipy.spatial import cKDTree
from scipy.io import wavfile

_d = os.path.dirname(os.path.abspath(__file__))
_s = spec_from_file_location("render", os.path.join(_d, "render.py")); rd = module_from_spec(_s); _s.loader.exec_module(rd)
FS, N, HOP = rd.FS, rd.N, rd.HOP
POLAR = [-30, 0, 30, 60, 90, 120, 150, 180, 210]
MASKER = (60, 0)
LENGTH, ONSET, TRAIN = 1.6, 0.35, 0.9
CONDITIONS = ["quiet", "quiet, enhanced", "masked", "protected", "enhanced", "enhanced, protected"]


def polar_dir(p):
    """The median-plane direction at polar angle p (degrees): 0 front, 90 overhead, 180 behind."""
    a = np.radians(p)
    return np.array([np.cos(a), 0.0, np.sin(a)])


def pink(rng, n):
    return lfilter([0.049922035, -0.095993537, 0.050612699, -0.004408786], [1, -2.494956002, 2.017265875, -0.522189400],
                   rng.standard_normal(n))


def enhance(H, dirs, alpha):
    """Brungart–Romigh: each direction's CLL deviation from its cone of confusion's mean, scaled by alpha."""
    cll = 10 * np.log10(np.maximum(np.abs(H[:, 0]) ** 2 + np.abs(H[:, 1]) ** 2, 1e-12))
    lat = np.degrees(np.arcsin(np.clip(dirs[:, 1], -1, 1)))
    out = H.copy()
    for i in range(len(H)):
        cone = np.abs(lat - lat[i]) < 3
        g = np.clip((alpha - 1) * (cll[i] - cll[cone].mean(0)), -20, 20)
        out[i] = H[i] * 10 ** (g / 20)
    return out


def main(sofa, out_dir, tmr="0", alpha="1.5"):
    tmr, alpha = float(tmr), float(alpha)
    out_dir = os.path.join(out_dir, "elevation")
    os.makedirs(out_dir, exist_ok=True)
    az, el, dirs, hrir, H, fr = rd.load(sofa)
    tree = cKDTree(dirs)
    He = enhance(H, dirs, alpha)
    masks = {False: rd.notch_mask(H, fr), True: rd.notch_mask(He, fr)}
    m_idx = tree.query(rd.xyz(np.radians(MASKER[0]), np.radians(MASKER[1])))[1]
    n, on, ramp = int(LENGTH * FS), int(ONSET * FS), int(0.003 * FS)
    spec = lambda x: stft(x, FS, window="hann", nperseg=N, noverlap=N - HOP)[2]
    band = (fr >= 2000) & (fr <= 16000)

    renders, fill = {}, {c: [] for c in CONDITIONS}
    for p in POLAR:
        rng = np.random.default_rng(1000 + p)
        target = np.zeros(n)
        burst, gap = int(0.04 * FS), int(0.02 * FS)
        env = np.ones(burst); env[:ramp] = np.linspace(0, 1, ramp); env[-ramp:] = np.linspace(1, 0, ramp)
        for t in range(on, on + int(TRAIN * FS) - burst, burst + gap):
            target[t:t + burst] = pink(rng, burst) * env
        masker = pink(rng, n)
        fade = int(0.05 * FS); masker[:fade] *= np.linspace(0, 1, fade); masker[-fade:] *= np.linspace(1, 0, fade)
        active = target != 0
        target /= np.sqrt(np.mean(target[active] ** 2))
        masker *= 10 ** (-tmr / 20) / np.sqrt(np.mean(masker[active] ** 2))
        S = np.stack([spec(target), spec(masker)])                            # 2 x bins x frames
        P = np.abs(S) ** 2
        dom = P / np.maximum(P.sum(0, keepdims=True), 1e-12)
        t_idx = tree.query(polar_dir(p))[1]
        solo_cll = 10 * np.log10(np.abs(H[t_idx, 0]) ** 2 + np.abs(H[t_idx, 1]) ** 2)
        for c in CONDITIONS:
            enh = "enhanced" in c
            Hc = He if enh else H
            Hs = Hc[[t_idx, m_idx]]                                           # 2 x ears x bins
            Sc = S[:1] if c.startswith("quiet") else S
            if "protected" in c:
                Sc = Sc * rd.protect_gains(dom, masks[enh][[t_idx, m_idx]])
            O = np.einsum("sek,skt->ekt", Hs[:len(Sc)], Sc)
            renders[(c, p)] = np.stack([istft(o, FS, window="hann", nperseg=N, noverlap=N - HOP)[1][:n] for o in O], 1)
            # Notch fill against the plain target alone, in tiles the target leads, its (plain) notch bands.
            r = 10 * np.log10(np.maximum(np.abs(O[0]) ** 2 + np.abs(O[1]) ** 2, 1e-12) / np.maximum(P[0], 1e-12))
            sel = (dom[0] >= 0.5) & band[:, None] & (P[0] > 1e-6 * P[0].max()) & (masks[False][t_idx][:, None] >= 0.5)
            fill[c].append(np.median((r - solo_cll[:, None])[sel]) if sel.any() else np.nan)   # overhead: no notch ≥ 0.5

    scale = 10 ** (-30 / 20) / np.sqrt(np.mean(np.concatenate([renders[("masked", p)] for p in POLAR]) ** 2))
    files = {}
    for (c, p), x in renders.items():
        name = f"{c.replace(', ', '-')}-{p}.wav"
        y = np.clip(x * scale, -10 ** (-12 / 20), 10 ** (-12 / 20))
        wavfile.write(os.path.join(out_dir, name), FS, (y * 32767).astype(np.int16))
        files.setdefault(c, {})[p] = name
    with open(os.path.join(out_dir, "manifest.json"), "w") as js:
        json.dump({"polar": POLAR, "conditions": CONDITIONS, "files": files, "tmr": tmr, "alpha": alpha,
                   "masker": MASKER}, js, indent=1)
    print(f"target-to-masker {tmr:+.0f} dB, enhancement alpha {alpha}; masker at azimuth {MASKER[0]}°. Notch fill in the "
          f"target's notch bands, tiles it leads, dB against the plain target alone — per polar angle {POLAR}:")
    for c in CONDITIONS:
        print(f"  {c:22s} median {np.nanmedian(fill[c]):+5.1f}   " + " ".join("   – " if np.isnan(v) else f"{v:+5.1f}" for v in fill[c]))
    print(f"wrote {len(renders)} files and manifest.json to {out_dir}; peak {np.max([np.abs(x).max() for x in renders.values()]) * scale:.3f}")


if __name__ == "__main__":
    main(*sys.argv[1:])
