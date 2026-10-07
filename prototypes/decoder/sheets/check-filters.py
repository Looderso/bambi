"""The decoder's binaural filters, designed here from head.json in numpy, for the core to be checked against.

The same chain as core/src/decode/binaural.cpp, written again with numpy's own linear algebra: the head synthesised
on a Gauss-Legendre x azimuth grid at the FFT bins, a complex least-squares fit up to the cutoff (magnitude with a
pure interaural delay of half the ITD per ear), the magnitudes alone above it with the phase carried from the bin
below, the diffuse-field covariance constraint, the bulk delay, the inverse FFT, the taps and the fade. Writes a few
filters tap for tap and the design's errors.

  uv run --with numpy --with scipy check-filters.py model/head.json ../../../core/tests/decode/filters-fixture.json
"""
import json, sys
from math import factorial
import numpy as np
from scipy.special import lpmv

C_SOUND, R_HEAD, FS, NFFT, TAPS, DELAY, FADE = 343.0, 0.0875, 48000.0, 512, 256, 64, 32
RINGS, AZIMUTHS = 12, 20


def sh_sn3d(order, xyz):
    az, el = np.arctan2(xyz[:, 1], xyz[:, 0]), np.arcsin(np.clip(xyz[:, 2], -1, 1))
    out = np.zeros((len(az), (order + 1) ** 2))
    for n in range(order + 1):
        for m in range(-n, n + 1):
            am = abs(m)
            p = lpmv(am, n, np.sin(el)) * (-1) ** am
            norm = np.sqrt((2 - (m == 0)) * factorial(n - am) / factorial(n + am))
            out[:, n * n + n + m] = norm * p * (np.cos(am * az) if m >= 0 else np.sin(am * az))
    return out


def grid():
    z, wz = np.polynomial.legendre.leggauss(RINGS)
    phi = 2 * np.pi * np.arange(AZIMUTHS) / AZIMUTHS
    r = np.sqrt(1 - z ** 2)
    xyz = np.stack([np.outer(r, np.cos(phi)), np.outer(r, np.sin(phi)), np.repeat(z[:, None], AZIMUTHS, 1)], -1).reshape(-1, 3)
    return xyz, np.repeat(wz / 2 / AZIMUTHS, AZIMUTHS)


def head(M, xyz, erbs, shift, size):
    """Both ears (dB, dirs x bins) and the ITD (us, dirs) as core/src/decode/headmodel.cpp synthesises them."""
    ax = M["axis"]; P = ax["points"]
    p = np.clip((erbs - ax["erb0"]) / ax["step"], 0, P - 1); i = np.minimum(p.astype(int), P - 2); f = p - i
    def table(t):
        rows = np.array(t["coef"]).reshape(-1, P); rows = rows[:, i] + (rows[:, i + 1] - rows[:, i]) * f
        return sh_sn3d(t["order"], xyz) @ rows
    smooth, ild = table(M["smooth"]), table(M["ild"]) + (size - 1) * table({"order": M["ild"]["order"], "coef": M["ild"]["withSize"]})
    itd = sh_sn3d(M["itd"]["order"], xyz) @ np.array(M["itd"]["coef"]) * size
    N = M["notches"]; y = sh_sn3d(N["order"], xyz); cll = smooth.copy()
    for n in N["list"]:
        t = np.clip((y @ n["presence"] - N["fade"][0]) / (N["fade"][1] - N["fade"][0]), 0, 1)
        a = np.maximum(-60, np.minimum(0, y @ n["depth"]) * N["depthFactor"] * t * t * (3 - 2 * t))
        c = np.clip(y @ n["centre"] + shift, *N["band"]); w = np.clip(y @ n["width"], *N["widthRange"])
        u = (erbs[None, :] - c[:, None]) / w[:, None]; cll += a[:, None] / (1 + u ** 2)
    both = 10 * np.log10(1 + 10 ** (ild / 10))
    return cll + ild - both, cll - both, itd


def sqrtm_psd(a):
    w, v = np.linalg.eigh(a); return (v * np.sqrt(np.maximum(w, 1e-12))) @ v.conj().T


def constraint(b, h, wq, dn):
    target = (h.T * wq) @ h.conj(); decoded = b.T @ (dn[:, None] * b.conj())
    kx, ky = sqrtm_psd(decoded), sqrtm_psd(target)
    u, _, vh = np.linalg.svd(kx.conj().T @ ky); p = vh.conj().T @ u.conj().T
    return b @ (ky @ p @ np.linalg.inv(kx)).T


def design(order, y, wq, left, right, itd, freqs):
    sw = np.sqrt(wq)[:, None]; pinv = np.linalg.pinv(y * sw); n_sh = y.shape[1]
    dn = np.array([1.0 / (2 * n + 1) for n in range(order + 1) for _ in range(2 * n + 1)])
    fc = order * C_SOUND / (2 * np.pi * R_HEAD)
    amp = np.stack([10 ** (left / 20), 10 ** (right / 20)], -1)                    # dirs x bins x 2
    delay = np.exp(-1j * np.pi * freqs[None, :] * itd[:, None] * 1e-6)                # half the ITD, the left later
    h = amp * np.stack([delay, delay.conj()], -1)                                    # dirs x bins x 2
    b = np.zeros((len(freqs), n_sh, 2), complex)
    for k, f in enumerate(freqs):
        if k == 0 or f <= fc: b[k] = pinv @ (h[:, k] * sw)
        else: b[k] = pinv @ (amp[:, k] * np.exp(1j * np.angle(y @ b[k - 1])) * sw)
    for k in range(len(freqs)): b[k] = constraint(b[k], h[:, k], wq, dn)
    ir = np.fft.irfft(b * np.exp(-2j * np.pi * freqs * DELAY / FS)[:, None, None], NFFT, axis=0)[:TAPS]
    ir[TAPS - FADE:] *= (0.5 + 0.5 * np.cos(np.linspace(0, np.pi, FADE)))[:, None, None]
    bands = []
    for lo, hi in [(200, 1000), (1000, 4000), (4000, 8000), (8000, 16000)]:
        ks = np.where((freqs >= lo) & (freqs < hi))[0]
        e = [np.sqrt(np.sum(wq[:, None] * (20 * np.log10(np.abs(y @ b[k]) + 1e-9) - np.stack([left[:, k], right[:, k]], 1)) ** 2) / 2) for k in ks]
        bands.append(float(np.mean(e)))
    ks = np.where((freqs >= 200) & (freqs < 16000))[0]
    diffuse = max(float(np.max(np.abs(10 * np.log10(np.real(np.einsum("ce,c,ce->e", b[k].conj(), dn, b[k])) / np.sum(wq[:, None] * amp[:, k] ** 2, 0))))) for k in ks)
    return ir, fc, bands, diffuse


def main(model_file, out):
    M = json.load(open(model_file)); xyz, wq = grid()
    freqs = np.fft.rfftfreq(NFFT, 1 / FS); erbs = 21.4 * np.log10(1 + 0.00437 * freqs)
    cases = []
    for order, shift, size, picks in [(1, 0.3, 1.1, [(c, e) for c in range(4) for e in range(2)]), (3, -0.5, 0.95, [(0, 0), (0, 1), (5, 0), (5, 1), (15, 0), (15, 1)])]:
        left, right, itd = head(M, xyz, erbs, shift, size)
        ir, fc, bands, diffuse = design(order, sh_sn3d(order, xyz), wq, left, right, itd, freqs)
        print(f"order {order}: cutoff {fc:.0f} Hz, magnitude error 0.2-1k {bands[0]:.2f}, 1-4k {bands[1]:.2f}, 4-8k {bands[2]:.2f}, 8-16k {bands[3]:.2f} dB, diffuse {diffuse:.4f} dB, "
              f"outside 0-192: {100 * np.sum(ir[192:] ** 2) / np.sum(ir ** 2):.2f} %")
        cases.append({"order": order, "earShift": shift, "headSize": size, "cutoffHz": fc, "magnitudeError": bands, "diffuseError": diffuse,
                      "filters": [{"acn": c, "ear": e, "taps": [round(float(v), 7) for v in ir[:, c, e]]} for c, e in picks]})
    json.dump({"grid": {"rings": RINGS, "azimuths": AZIMUTHS}, "design": {"sampleRate": FS, "fftSize": NFFT, "taps": TAPS, "delay": DELAY, "fade": FADE, "headRadius": R_HEAD}, "cases": cases}, open(out, "w"))
    print(f"wrote {out}")


if __name__ == "__main__":
    main(*sys.argv[1:])
