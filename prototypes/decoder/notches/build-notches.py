"""A few synthesised notches in place of a head's notch layer — the filters and numbers for index.html.

A head's own HRTF on the median plane: each ear's magnitude in dB on an auditory axis (ERB number, 0.2–20 kHz in steps
of 0.05 ERB), split with a 3-ERB Gaussian into a smooth part and a notch layer. The smooth part stays; the notch layer
is replaced by a few Gaussian features (centre, depth, width in ERB), fitted per ear between 3 and 18 kHz through the
same split. Six versions of every median-plane direction from polar −45° (front-below) over 90° (overhead) to 225°
(behind-below):
  real                  the measured magnitudes
  smooth                the smooth part alone, no notch layer — what is left without any notch
  2 notches             two dips, free in every direction
  2 notches + peak      and one peak
  3 notches + 2 peaks
  curves                two dips and a peak that each follow one curve over the polar angle: centre and depth a Fourier
                        series of order 5, one width — 23 numbers a feature and ear for the whole plane
Every version is built the same way: minimum phase from its magnitude, the pair delayed to the measured ITD. A head's
responses share one scale: pink noise through its real ones comes out, on average, as loud as it went in.

Prints per head, over its median-plane directions and both ears, how much of the notch layer each version keeps, how
far the real version is from the measured magnitudes, and the loudest peak the page's signals reach. Writes data.js:
per head the polar angles, the responses (directions x versions x ears x TAPS, float32, base64) and the numbers.

  python build-notches.py <name>=<sofa, 48 kHz> [<name>=<sofa> ...]
"""
import base64, json, os, sys
import h5py
import numpy as np
from scipy.ndimage import gaussian_filter1d
from scipy.optimize import least_squares
from scipy.signal import fftconvolve, lfilter

FS = 48000
NFFT, TAPS, FADE, LEAD = 4096, 512, 32, 32      # LEAD: samples ahead of a response, for a fractional delay's ringing
ITD_FC, ITD_UP = 1500.0, 8                        # the ITD: below this, Hz; at this share of a sample
STEP, WIDTH, ORDER = 0.05, 3.0, 5                 # the axis' step and the smooth part's width, ERB; the curves' order
BEND, SLOPE = 7.2, 0.12                           # a curve's cost of bending, dB per ERB/degree; its limit, ERB/degree
erb_of = lambda f: 21.4 * np.log10(1 + 0.00437 * f)
f_of = lambda e: (10 ** (e / 21.4) - 1) / 0.00437
ERB = np.arange(erb_of(200), erb_of(20000), STEP)
FREQ = f_of(ERB)
FIT = (FREQ >= 3000) & (FREQ <= 18000)
BAND = (FREQ >= 4000) & (FREQ <= 16000)
FEATURES = {"2 notches": (2, 0), "2 notches + peak": (2, 1), "3 notches + 2 peaks": (3, 2)}
CURVES = "curves"

lowpass = lambda x: gaussian_filter1d(x, WIDTH / 2.355 / STEP, axis=-1, mode="nearest")
split = lambda x: x - lowpass(x)                  # what the split leaves of x in the notch layer
rms = lambda x: np.sqrt(np.mean(x ** 2))


def load(sofa):
    """The SOFA set's directions (unit vectors; x front, y left, z up) and HRIRs (directions x ears x taps)."""
    f = h5py.File(sofa)
    assert round(float(f["Data.SamplingRate"][0])) == FS, f"{sofa} is not at {FS} Hz"
    pos = f["SourcePosition"][:]
    az, el = np.radians(pos[:, 0]), np.radians(pos[:, 1])
    return np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], -1), f["Data.IR"][:]


def median_plane(dirs):
    """Indices of the median-plane directions from polar −45° to 225°, and their polar angles, ascending."""
    i = np.where(np.abs(dirs[:, 1]) < 0.01)[0]
    p = np.degrees(np.arctan2(dirs[i, 2], dirs[i, 0])); p = np.where(p < -90, p + 360, p)
    keep = (p > -45.5) & (p < 225.5)
    p, first = np.unique(np.round(p[keep], 1), return_index=True)
    return i[keep][first], p


def on_axis(hrir):
    """Each ear's magnitude in dB on the ERB axis: ... x len(ERB)."""
    fr = np.fft.rfftfreq(NFFT, 1 / FS)
    db = 20 * np.log10(np.maximum(np.abs(np.fft.rfft(hrir, NFFT, axis=-1)), 1e-6))
    return np.apply_along_axis(lambda r: np.interp(FREQ, fr, r), -1, db)


def bumps(par):
    """The sum of Gaussian features on the ERB axis, dB; par's rows: centre (ERB), height (dB, a dip negative), sigma (ERB)."""
    return sum((h * np.exp(-0.5 * ((ERB - c) / w) ** 2) for c, h, w in par), np.zeros(len(ERB)))


def fit(layer, dips, peaks):
    """The notch layer of that many dips and peaks closest to layer between 3 and 18 kHz: each placed at the largest
    remaining dip or peak, then all refined together."""
    signs, par, rest = [-1] * dips + [1] * peaks, np.zeros((0, 3)), layer
    k = np.where(FIT)[0]
    while signs:
        s = max(set(signs), key=lambda q: np.max(q * rest[k])); signs.remove(s)
        i = k[np.argmax(s * rest[k])]
        par = np.vstack([par, [ERB[i], s * np.clip(s * rest[i], 0.1, 24), 0.4]])
        lo = np.array([[ERB[k[0]], -60 if h < 0 else 0, 0.1] for _, h, _ in par]).ravel()
        hi = np.array([[ERB[k[-1]], 0 if h < 0 else 25, 1.5] for _, h, _ in par]).ravel()
        par = least_squares(lambda q: (split(bumps(q.reshape(-1, 3))) - layer)[k], np.clip(par.ravel(), lo + 1e-3, hi - 1e-3),
                            bounds=(lo, hi)).x.reshape(-1, 3)
        rest = layer - split(bumps(par))
    return split(bumps(par))


def track(cost, polar):
    """The bin per direction of the path with the lowest total cost, bending at most SLOPE."""
    na, nb = cost.shape
    acc, back, j = cost[0].copy(), np.zeros((na, nb), int), np.arange(nb)
    for a in range(1, na):
        turn = polar[a] - polar[a - 1]
        best = np.full(nb, np.inf)
        for o in range(-max(1, int(SLOPE * turn / STEP)), max(1, int(SLOPE * turn / STEP)) + 1):
            src = np.clip(j + o, 0, nb - 1)
            v = acc[src] + BEND * abs(o) * STEP / turn
            better = v < best; best[better] = v[better]; back[a, better] = src[better]
        acc = best + cost[a]
    path = np.zeros(na, int); path[-1] = np.argmin(acc)
    for a in range(na - 1, 0, -1):
        path[a - 1] = back[a, path[a]]
    return path


def fourier(polar, order):
    t = np.radians(polar)
    return np.stack([np.ones_like(t)] + [g(i * t) for i in range(1, order + 1) for g in (np.cos, np.sin)], -1)


def on_curves(q, basis, signs):
    """The notch layer (directions x len(ERB)) of features on curves; q, per feature: the centre's coefficients (ERB),
    the height's (dB), sigma (ERB)."""
    m, model = basis.shape[1], 0
    for sign, f in zip(signs, q.reshape(len(signs), -1)):
        centre, height = basis @ f[:m], sign * np.maximum(sign * (basis @ f[m:2 * m]), 0)
        model = model + height[:, None] * np.exp(-0.5 * ((ERB[None] - centre[:, None]) / f[-1]) ** 2)
    return split(model)


def curves(layer, polar, signs=(-1, -1, 1)):
    """One ear's notch layer (directions x len(ERB)) from two dips and a peak, each on one curve over the polar angle:
    each started on the path tracked through the layer, then all refined together."""
    basis, rows = fourier(polar, ORDER), np.arange(len(layer))
    q, rest, reach = [], layer.copy(), int(1.25 / STEP)
    for n, sign in enumerate(signs):
        path = track(np.where(FIT, -sign * rest, 50.0), polar)
        weight = np.sqrt(np.maximum(sign * rest[rows, path], 0.2))
        centre = np.linalg.lstsq(basis * weight[:, None], ERB[path] * weight, rcond=None)[0]
        left, best = layer - (on_curves(np.concatenate(q), basis, signs[:n]) if q else 0), None
        for width in (0.15, 0.2, 0.3, 0.4, 0.55, 0.75, 1.0):
            g = split(np.exp(-0.5 * ((ERB[None] - (basis @ centre)[:, None]) / width) ** 2))
            height = np.linalg.lstsq(basis, np.sum((g * left)[:, FIT], 1) / np.sum((g * g)[:, FIT], 1), rcond=None)[0]
            f = np.concatenate([centre, height, [width]])
            err = rms((left - on_curves(f, basis, [sign]))[:, FIT])
            if best is None or err < best[0]:
                best = (err, f)
        q.append(best[1])
        for a, b in enumerate(path):                                  # the tracked feature is not tracked again
            w = slice(max(0, b - reach), b + reach + 1)
            rest[a, w] = -sign * np.maximum(-sign * rest[a, w], 0)
    q = np.concatenate(q)
    lo, hi = np.full(len(q), -np.inf), np.full(len(q), np.inf)
    lo[len(q) // len(signs) - 1::len(q) // len(signs)], hi[len(q) // len(signs) - 1::len(q) // len(signs)] = 0.1, 1.5
    q = least_squares(lambda x: (on_curves(x, basis, signs) - layer)[:, FIT].ravel(), q, bounds=(lo, hi), max_nfev=80).x
    return on_curves(q, basis, signs)


def profile(db):
    """Positive gradient across ERB bands of the magnitude smoothed to about one ERB, 2–16 kHz, dB per ERB."""
    s = gaussian_filter1d(db, 1 / 2.355 / STEP, axis=-1, mode="nearest")[..., (FREQ >= 2000) & (FREQ <= 16000)]
    return np.maximum(np.diff(s, axis=-1), 0) / STEP


def response(db, delay):
    """TAPS samples of the minimum-phase response with the magnitude db (on the ERB axis), delayed by LEAD + delay samples."""
    fr = np.fft.rfftfreq(NFFT, 1 / FS)
    c = np.fft.irfft(np.interp(fr, FREQ, db) * np.log(10) / 20, NFFT)
    c[1:NFFT // 2] *= 2; c[NFFT // 2 + 1:] = 0
    h = np.fft.irfft(np.exp(np.fft.rfft(c)) * np.exp(-2j * np.pi * fr / FS * (LEAD + delay)), NFFT)[:TAPS]
    h[-FADE:] *= np.cos(np.linspace(0, np.pi / 2, FADE)) ** 2
    return h


def itd_of(left, right):
    """tL − tR in seconds: where the two ears' cross-correlation, low-passed at ITD_FC, peaks."""
    n = 2 * max(len(left), len(right))
    f = np.fft.rfftfreq(n, 1 / FS)
    c = np.fft.rfft(left, n) * np.conj(np.fft.rfft(right, n)) / (1 + (f / ITD_FC) ** 8) ** 2
    cc = np.fft.irfft(c, n * ITD_UP)
    reach = 80 * ITD_UP
    return (np.argmax(np.concatenate([cc[-reach:], cc[:reach + 1]])) - reach) / ITD_UP / FS


def pair(db, itd):
    """The two ears' responses for the magnitudes db, the later ear delayed until the pair's ITD (tL − tR, seconds) is
    itd — a minimum-phase pair carries some ITD of its own."""
    h = [response(d, 0) for d in db]
    more = (itd - itd_of(*h)) * FS
    return [response(db[0], max(more, 0)), response(db[1], max(-more, 0))]


def pink(rng, n):
    return lfilter([0.049922035, -0.095993537, 0.050612699, -0.004408786], [1, -2.494956002, 2.017265875, -0.522189400],
                   rng.standard_normal(n))


def head(name, sofa):
    dirs, hrir = load(sofa)
    idx, polar = median_plane(dirs)
    db = on_axis(hrir[idx])                                           # directions x ears x axis
    base = lowpass(db); layer = db - base
    layers = {"real": layer, "smooth": np.zeros_like(layer)}
    for c, (dips, peaks) in FEATURES.items():
        layers[c] = np.array([[fit(layer[a, e], dips, peaks) for e in range(2)] for a in range(len(idx))])
    layers[CURVES] = np.stack([curves(layer[:, e], polar) for e in range(2)], 1)

    print(f"{name}: {len(idx)} median-plane directions, polar {polar[0]:.0f}…{polar[-1]:.0f}°, both ears. Notch layer "
          f"{rms(layer[..., BAND]):.2f} dB RMS at 4–16 kHz.")
    print("  version               notch layer kept   deepest notch's depth   from the real head, dB per ERB")
    deep = [(a, e, np.where(BAND)[0][np.argmin(layer[a, e, BAND])]) for a in range(len(idx)) for e in range(2)
            if layer[a, e, BAND].min() < -3]
    near, numbers = int(0.5 / STEP), {}
    for c, m in layers.items():
        kept = 1 - rms((layer - m)[..., BAND]) ** 2 / rms(layer[..., BAND]) ** 2
        depth = np.mean([m[a, e, max(0, b - near):b + near + 1].min() / layer[a, e, b] for a, e, b in deep])
        far = rms(profile(base + m) - profile(db))
        numbers[c] = [round(100 * kept), round(100 * depth), round(far, 2)]
        print(f"  {c:21s} {100 * kept:5.0f} %             {100 * depth:5.0f} %                 {far:5.2f}")

    itd = [itd_of(*hrir[i]) for i in idx]
    ir = np.array([[pair(base[a] + layers[c][a], itd[a]) for c in layers] for a in range(len(idx))])   # directions x versions x ears x taps
    fr = np.fft.rfftfreq(NFFT, 1 / FS); audible = (fr >= 2000) & (fr <= 16000); heard = (fr >= 50) & (fr <= 20000)
    real = np.abs(np.fft.rfft(ir[:, 0], NFFT, axis=-1))               # directions x ears x bins
    measured = 20 * np.log10(np.maximum(np.abs(np.fft.rfft(hrir[idx], NFFT, axis=-1)), 1e-6))
    off = np.sqrt(np.mean((20 * np.log10(np.maximum(real, 1e-6)) - measured)[..., audible] ** 2, axis=(1, 2)))
    ir /= np.sqrt(np.mean(np.sum(real[..., heard] ** 2 / fr[heard], -1) / np.sum(1 / fr[heard])))   # pink in, as loud out
    print(f"  the real version against the measured magnitudes, 2–16 kHz: {np.median(off):.2f} dB RMS (median of the "
          f"directions, worst {off.max():.2f}); the ITD kept, {min(itd) * 1e6:+.0f}…{max(itd) * 1e6:+.0f} µs")

    # The loudest the page can play: its signals are at −30 dBFS RMS and never peak above −18 dBFS; volume at most 0 dB.
    rng = np.random.default_rng(1)
    noise = pink(rng, FS); noise *= min(10 ** (-30 / 20) / rms(noise), 10 ** (-18 / 20) / np.abs(noise).max())
    click = np.zeros(FS // 4); click[:96] = rng.uniform(-1, 1, 96) * np.exp(-np.arange(96) / FS / 0.0004)
    click *= min(10 ** (-30 / 20) / rms(click), 10 ** (-18 / 20) / np.abs(click).max())
    peak = max(np.abs(fftconvolve(x, h)).max() for x in (noise, click) for h in ir.reshape(-1, TAPS))
    print(f"  loudest peak of noise and clicks through any response: {20 * np.log10(peak):.1f} dBFS (the page's ceiling: −12)")
    return {"name": name, "polar": [float(p) for p in polar], "numbers": numbers,
            "ir": base64.b64encode(ir.astype("<f4").tobytes()).decode()}, list(layers)


def main(*heads):
    out = [head(*h.split("=", 1)) for h in heads]
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data.js")
    with open(path, "w") as js:
        js.write("// Written by build-notches.py.\nconst NOTCHES = " + json.dumps(
            {"fs": FS, "taps": TAPS, "lead": LEAD, "versions": out[0][1], "heads": [h for h, _ in out]}) + ";\n")
    print(f"wrote {path}, {os.path.getsize(path) / 1e6:.1f} MB")


if __name__ == "__main__":
    main(*sys.argv[1:])
