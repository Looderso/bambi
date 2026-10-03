"""Do concurrent sources fill each other's notches on headphones, and which rendering keeps them?

A scene of three sources (noise bursts with irregular gaps, so they overlap only part of the time) at different
directions, encoded to third-order AmbiX, rendered binaurally with the KU100 in five ways:
  1 linear          MagLS, order 3 — the decoder as it is
  2 parametric      per time-frequency tile (STFT 1024, hop 256), the direction and diffuseness from the first-order
                    part (the intensity vector against the energy, smoothed over about 30 ms): the direct share,
                    W · √(1−ψ), through the HRTF of that direction; the rest, √ψ, through the linear decode
  3 parametric, cue protection   as 2; where the dominant direction's HRTF has a notch at that frequency, the rest is
                    lowered by the notch's share — what fills the notch is mostly other sound
  4 objects         every source through its own HRTF, no ambisonics — what metadata would allow; the upper bound
  5 objects, cue protection      as 4; each source's notch bands are cleared of the other sources in proportion to how
                    much that source dominates the tile
A notch here: where a direction's CLL (both ears' power) lies below its 3-ERB-smoothed self; its share, 0…1, is that
gap over 6 dB.

Measured per source, in the tiles it dominates (≥ 70 % of the energy), against what that source alone would give:
  notch fill   how far its notch bands come up, dB (0: as deep as alone)
  colour       how much the rest changes, dB RMS — the check that a mode does not trade notches for artefacts
Writes WAVs of every mode and of each source alone for listen.html into <out directory>/<signals>, normalised to
−30 dBFS RMS, peaks under −12 dBFS.

The signals:
  noise    pink-noise bursts, 80–400 ms, with gaps of 50–600 ms
  music    1 plucked notes, 2 a shaker, 3 a brass-like tone with vibrato — each in phrases with rests
  three mono WAV paths, one per source, at 48 kHz: your own (cut or looped to the scene's length)

  python render.py <KU100 sofa> <out directory> [noise | music | <a.wav> <b.wav> <c.wav>]
"""
import os, sys
from importlib.util import spec_from_file_location, module_from_spec
import h5py
import numpy as np
from scipy.ndimage import gaussian_filter1d
from scipy.signal import stft, istft, lfilter
from scipy.spatial import SphericalVoronoi, cKDTree
from scipy.io import wavfile

_d = os.path.dirname(os.path.abspath(__file__))
_s = spec_from_file_location("bf", os.path.join(_d, "..", "listen", "build-filters.py")); bf = module_from_spec(_s); _s.loader.exec_module(bf)
FS, N, HOP, ORDER = 48000, 1024, 256, 3
SECONDS = 12
SOURCES = [("front, up", 0, 40), ("left, a little down", 70, -10), ("behind, up", 150, 30)]
AMOUNT = 1.0


def xyz(az, el):
    return np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], -1)


def noise(rng):
    """Pink-ish noise bursts, 80–400 ms, with gaps of 50–600 ms, different for each source."""
    out = []
    n = FS * SECONDS
    for _ in SOURCES:
        w = rng.standard_normal(n)
        b0 = b1 = b2 = 0.0
        x = np.empty(n)
        for i in range(n):                                               # pink: Paul Kellet's three-pole filter
            b0 = 0.99765 * b0 + w[i] * 0.0990460; b1 = 0.96300 * b1 + w[i] * 0.2965164; b2 = 0.57000 * b2 + w[i] * 1.0526913
            x[i] = b0 + b1 + b2 + w[i] * 0.1848
        env = np.zeros(n)
        t = int(rng.uniform(0, 0.3) * FS)
        while t < n:
            on = int(rng.uniform(0.08, 0.4) * FS)
            fade = min(240, on // 4)
            seg = np.ones(on); seg[:fade] = np.linspace(0, 1, fade); seg[-fade:] = np.linspace(1, 0, fade)
            env[t:t + on] = seg[:max(0, min(on, n - t))]
            t += on + int(rng.uniform(0.05, 0.6) * FS)
        out.append(x * env / np.std(x))
    return np.array(out)


def phrases(rng, n, note, gap):
    """Note onsets in phrases of 3–8 notes, note(rng) seconds apart, rests of gap seconds between phrases."""
    on, t = [], int(rng.uniform(0, 0.4) * FS)
    while t < n:
        for _ in range(rng.integers(3, 9)):
            if t >= n:
                break
            on.append(t); t += int(note(rng) * FS)
        t += int(rng.uniform(*gap) * FS)
    return on


def music(rng):
    """Plucked notes (Karplus–Strong, a bright attack), a shaker (noise ticks with a fast decay, high-passed) and a
    brass-like tone (a saw with vibrato and a slow swell). Each at the same RMS."""
    n = FS * SECONDS
    scale = 2 ** (np.array([0, 2, 4, 7, 9, 12, 14, 16, 19]) / 12)
    pluck = np.zeros(n)
    for t in phrases(rng, n, lambda r: r.choice([0.15, 0.3, 0.3, 0.45]), (0.3, 1.2)):
        f = 330 * rng.choice(scale); L = int(FS / f)
        burst = np.zeros(int(1.2 * FS)); burst[:L] = rng.uniform(-1, 1, L)
        y = lfilter([1], np.r_[1, np.zeros(L - 1), -0.497, -0.497], burst)
        k = min(len(y), n - t); pluck[t:t + k] += y[:k]
    shaker = np.zeros(n)
    hp = lambda x: lfilter([1, -1], [1, -0.9], x)
    for t in phrases(rng, n, lambda r: r.choice([0.125, 0.125, 0.25]), (0.4, 1.0)):
        m = int(0.09 * FS); e = np.exp(-np.arange(m) / (0.018 * FS)) * np.minimum(1, np.arange(m) / 48)
        k = min(m, n - t); shaker[t:t + k] += (hp(rng.standard_normal(m)) * e * rng.uniform(0.6, 1))[:k]
    brass = np.zeros(n)
    for t in phrases(rng, n, lambda r: r.choice([0.5, 0.75, 1.0]), (0.5, 1.5))[::2]:
        m = int(rng.uniform(0.4, 0.9) * FS); tt = np.arange(m) / FS
        f0 = 196 * rng.choice(scale[:6]) * (1 + 0.007 * np.sin(2 * np.pi * 5.5 * tt) * np.minimum(1, tt / 0.3))
        ph = 2 * np.pi * np.cumsum(f0) / FS
        saw = sum(np.sin(h * ph) / h for h in range(1, int(16000 / f0.max()) + 1))
        env = np.minimum(1, tt / 0.06) * np.minimum(1, (tt[-1] - tt) / 0.08) * (0.7 + 0.3 * np.minimum(1, tt / 0.4))
        k = min(m, n - t); brass[t:t + k] += (saw * env)[:k]
    return np.array([x / np.std(x) for x in (pluck, shaker, brass)])


def from_files(paths):
    out = []
    for p in paths:
        fs, x = wavfile.read(p)
        assert fs == FS, f"{p}: {fs} Hz, needs {FS}"
        x = x.astype(float); x = x.mean(1) if x.ndim > 1 else x
        x = np.resize(x, FS * SECONDS)                                    # cut, or looped to length
        out.append(x / np.std(x))
    return np.array(out)


def main(sofa, out_dir, kind="noise", *files):
    out_dir = os.path.join(out_dir, kind if kind in ("noise", "music") else "files")
    os.makedirs(out_dir, exist_ok=True)
    f = h5py.File(sofa)
    pos, hrir = f["SourcePosition"][:], f["Data.IR"][:]
    az, el = np.radians(pos[:, 0]), np.radians(pos[:, 1])
    dirs = xyz(az, el)
    tree = cKDTree(dirs)
    H = np.fft.rfft(hrir, N, axis=-1).astype(np.complex64)                # dirs x ears x bins
    fr = np.fft.rfftfreq(N, 1 / FS)
    nb = len(fr)

    # Notch shares per direction and bin: the CLL below its 3-ERB-smoothed self, over 6 dB.
    cll = 10 * np.log10(np.maximum(np.abs(H[:, 0]) ** 2 + np.abs(H[:, 1]) ** 2, 1e-12))
    erb = 21.4 * np.log10(1 + 0.00437 * fr)
    grid = np.arange(erb[1], erb[-1], 0.25)
    on_grid = np.stack([np.interp(grid, erb, c) for c in cll])
    base = gaussian_filter1d(on_grid, 3 / 2.355 / 0.25, axis=-1, mode="nearest")
    gap = np.stack([np.interp(erb, grid, b) for b in base]) - cll
    mask = np.clip(gap / 6.0, 0, 1).astype(np.float32)
    mask[:, (fr < 2000) | (fr > 16000)] = 0

    # The linear decode: MagLS at order 3 on these bins, the HRIRs' bulk delay put back so it lines up with the direct part.
    w = SphericalVoronoi(dirs).calculate_areas(); w /= w.sum()
    bulk = int(np.round(np.median(np.abs(hrir).argmax(-1).mean(-1))))
    hf = np.fft.rfft(hrir, N, axis=-1).transpose(2, 0, 1) * np.exp(2j * np.pi * fr * bulk / FS)[:, None, None]
    y3 = bf.sh_sn3d(ORDER, az, el)
    b, _, _ = bf.design(ORDER, y3, hf, w, fr, "magls")                   # bins x channels x ears
    b = b * np.exp(-2j * np.pi * fr * bulk / FS)[:, None, None]

    rng = np.random.default_rng(3)
    src = noise(rng) if kind == "noise" else music(rng) if kind == "music" else from_files((kind,) + files)
    sdirs = np.array([xyz(np.radians(a), np.radians(e)) for _, a, e in SOURCES])
    sidx = tree.query(sdirs)[1]
    gains = bf.sh_sn3d(ORDER, np.radians([a for _, a, _ in SOURCES]), np.radians([e for _, _, e in SOURCES]))   # S x 16
    amb = gains.T @ src                                                    # 16 x samples

    def spec(x):
        return stft(x, FS, window="hann", nperseg=N, noverlap=N - HOP)[2]   # ... x bins x frames
    A = spec(amb)                                                         # 16 x bins x frames
    Ssrc = spec(src)                                                      # S x bins x frames

    linear = np.einsum("kce,ckt->ekt", b, A)                              # ears x bins x frames
    # First-order analysis: ACN 0 W, 1 Y, 2 Z, 3 X; SN3D, so a plane wave has X = W·ux, and |X|²+|Y|²+|Z|² = |W|².
    W, Y, Z, X = A[0], A[1], A[2], A[3]
    I = np.stack([np.real(np.conj(W) * X), np.real(np.conj(W) * Y), np.real(np.conj(W) * Z)])
    E = 0.5 * (np.abs(W) ** 2 + np.abs(X) ** 2 + np.abs(Y) ** 2 + np.abs(Z) ** 2)
    alpha = np.exp(-HOP / FS / 0.03)
    Is, Es = np.zeros_like(I), np.zeros_like(E)
    for t in range(I.shape[-1]):                                           # recursive smoothing, about 30 ms
        Is[..., t] = alpha * (Is[..., t - 1] if t else 0) + (1 - alpha) * I[..., t]
        Es[..., t] = alpha * (Es[..., t - 1] if t else 0) + (1 - alpha) * E[..., t]
    norm = np.linalg.norm(Is, axis=0)
    psi = np.clip(1 - norm / np.maximum(Es, 1e-12), 0, 1)
    d = (Is / np.maximum(norm, 1e-12)).reshape(3, -1).T
    didx = tree.query(d)[1].reshape(W.shape)                              # bins x frames: dominant direction
    kk = np.broadcast_to(np.arange(nb)[:, None], W.shape)
    Hd = H[didx, :, kk]                                                   # bins x frames x ears
    direct = (W * np.sqrt(1 - psi))[..., None] * Hd
    para = direct.transpose(2, 0, 1) + np.sqrt(psi)[None] * linear
    protect = 1 - AMOUNT * mask[didx, kk] * (1 - psi)                     # lower the rest where the dominant direction has a notch
    para_p = direct.transpose(2, 0, 1) + (np.sqrt(psi) * protect)[None] * linear

    Hs = H[sidx]                                                          # S x ears x bins
    objects = np.einsum("sek,skt->ekt", Hs, Ssrc)
    P = np.abs(Ssrc) ** 2
    dom = P / np.maximum(P.sum(0, keepdims=True), 1e-12)                  # S x bins x frames
    ms = mask[sidx]                                                       # S x bins
    g = np.ones_like(dom)
    for s in range(len(SOURCES)):
        for t in range(len(SOURCES)):
            if t != s:
                g[t] *= 1 - AMOUNT * ms[s][:, None] * dom[s]
    objects_p = np.einsum("sek,skt->ekt", Hs, Ssrc * g)

    modes = {"1 linear": linear, "2 parametric": para, "3 parametric, cue protection": para_p,
             "4 objects": objects, "5 objects, cue protection": objects_p}
    solo_cll = 10 * np.log10(np.abs(Hs[:, 0]) ** 2 + np.abs(Hs[:, 1]) ** 2)   # S x bins: what each alone gives
    band = (fr >= 2000) & (fr <= 16000)
    print(f"{os.path.basename(out_dir)}: {len(SOURCES)} sources, {SECONDS} s, order {ORDER}; 2–16 kHz, a source's notch bands (share ≥ 0.5) "
          f"and the rest. Per source, in tiles it has alone (≥ 95 %) | shares (70–95 %): notch fill / colour, dB")
    print(f"  direction found, median error: " + "  ".join(f"{np.degrees(np.median(np.arccos(np.clip(d.reshape(*W.shape, 3)[(dom[s] >= 0.95) & band[:, None] & (P[s] > 1e-6 * P[s].max())] @ sdirs[s], -1, 1)))):.0f}°" for s in range(len(SOURCES))))
    for name, O in modes.items():
        cells = []
        for s in range(len(SOURCES)):
            r = 10 * np.log10(np.maximum(np.abs(O[0]) ** 2 + np.abs(O[1]) ** 2, 1e-12) / np.maximum(P[s], 1e-12))
            dev = r - solo_cll[s][:, None]
            for lo, hi in ((0.95, 1.01), (0.7, 0.95)):
                sel = (dom[s] >= lo) & (dom[s] < hi) & band[:, None] & (P[s] > 1e-6 * P[s].max())
                nm = sel & (ms[s][:, None] >= 0.5)
                rest = sel & (ms[s][:, None] < 0.1)
                cells.append(f"{np.median(dev[nm]):+5.1f} / {np.sqrt(np.mean(dev[rest] ** 2)):4.1f}")
        print(f"  {name:30s} " + "   ".join(f"{cells[2*i]}  | {cells[2*i+1]}" for i in range(len(SOURCES))))

    # The renders, all at the same loudness, under the ceiling.
    def write(name, O):
        x = np.stack([istft(o, FS, window="hann", nperseg=N, noverlap=N - HOP)[1] for o in O], 1)
        return x
    rendered = {k: write(k, v) for k, v in modes.items()}
    for s, (label, _, _) in enumerate(SOURCES):
        rendered[f"solo {s + 1} {label}"] = write("", np.einsum("ek,kt->ekt", Hs[s], Ssrc[s]))
    ref = np.sqrt(np.mean(rendered["1 linear"] ** 2))
    for k, x in rendered.items():
        g_ = 10 ** (-30 / 20) / ref
        y = np.clip(x * g_, -10 ** (-12 / 20), 10 ** (-12 / 20)).astype(np.float32)
        wavfile.write(os.path.join(out_dir, k.split(" ")[0] + ("-" + k.split(" ")[1] if k.startswith("solo") else "") + ".wav"), FS, y)
    print(f"wrote {len(rendered)} files to {out_dir} (the mix's loudness at −30 dBFS RMS; each solo at its own level within it)")


if __name__ == "__main__":
    main(*sys.argv[1:])
