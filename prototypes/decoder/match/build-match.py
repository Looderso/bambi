"""The KU100 as the matching prototype renders it, split the way the thesis split it.

Writes match.js, all in spherical harmonics (SN3D, ACN) per frequency bin of a 256-point FFT at 48 kHz:
  - CLL, the composite loudness level 10·log10(P_L + P_R): what both ears share, where the pinna's elevation
    cues are. Order 0 is its average over all directions, the timbre. "Ear size" stretches the rest in frequency.
  - ILD, the interaural level difference 10·log10(P_L / P_R): the head's shadow. "Head size" stretches it in
    frequency, as a larger head shadows from a lower frequency up.
  - ITD, the interaural time difference tL − tR in seconds (negative when the left ear hears first), measured, so it
    keeps the head's own elevation dependence. "Head size" scales it.
The two ears come back exactly: P_L = 10^(CLL/10)·r/(1+r), P_R = 10^(CLL/10)/(1+r), r = 10^(ILD/10).

The page makes each ear minimum-phase, measures the ITD that pair already carries with the estimator below, and
delays the ears by the rest. This script runs the same chain and compares it with the measured HRIRs.

  python build-match.py path/to/D1_48K_24bit_256tap_FIR_SOFA.sofa
"""
import base64, sys
from importlib.util import spec_from_file_location, module_from_spec
import h5py
import numpy as np
from scipy.spatial import SphericalVoronoi

_s = spec_from_file_location("bf", "../listen/build-filters.py"); bf = module_from_spec(_s); _s.loader.exec_module(bf)

FS, NFFT = 48000, 256
ORDER, ITD_ORDER = 20, 10
FLOOR_DB = -60.0
ILD_LIMIT_DB = 40.0             # an ILD beyond this is the far ear in a notch; held there
ITD_N, ITD_UP, ITD_FC = 512, 8, 1500.0


def itd_estimate(hl, hr):
    """tL − tR in seconds: the cross-correlation of the two ears weighted by a 4th-order Butterworth low-pass at
    1.5 kHz applied forward and backward to each (|B|⁴), at 1/ITD_UP of a sample. The page computes exactly this."""
    f = np.fft.rfftfreq(ITD_N, 1 / FS)
    w = 1 / (1 + (f / ITD_FC) ** 8) ** 2
    c = np.fft.rfft(hl, ITD_N) * np.conj(np.fft.rfft(hr, ITD_N)) * w
    cc = np.fft.irfft(c, ITD_N * ITD_UP)
    m = 80 * ITD_UP
    cc = np.concatenate([cc[-m:], cc[:m + 1]])
    return (np.argmax(cc) - m) / ITD_UP / FS


def min_phase(mag_db, nfft=512):
    """A minimum-phase impulse response of nfft//2 taps from a magnitude in dB on the 256-point grid."""
    f_in = np.linspace(0, 1, len(mag_db))
    f_out = np.linspace(0, 1, nfft // 2 + 1)
    logmag = np.interp(f_out, f_in, mag_db) * np.log(10) / 20
    cep = np.fft.irfft(logmag, nfft)
    fold = np.zeros(nfft)
    fold[0], fold[nfft // 2] = cep[0], cep[nfft // 2]
    fold[1:nfft // 2] = 2 * cep[1:nfft // 2]
    return np.fft.irfft(np.exp(np.fft.rfft(fold)), nfft)[:nfft // 2]


def ears(cll, ild):
    """Left and right magnitude in dB from CLL and ILD in dB."""
    r = 10 ** (ild / 10)
    return cll + 10 * np.log10(r / (1 + r)), cll - 10 * np.log10(1 + r)


def warp(x, scale):
    """x(f · scale) on the same bins, held at the last bin beyond it."""
    b = np.arange(len(x))
    return np.interp(np.minimum(b * scale, len(x) - 1), b, x)


def render(y, cll_c, ild_c, itd_c, head=1.0, ear=1.0):
    """The page's chain: two minimum-phase filters and the delay to add, tL − tR in seconds."""
    cll = y[0] * cll_c[0] + warp(y[1:] @ cll_c[1:], ear)
    ild = np.clip(warp(y @ ild_c, head), -ILD_LIMIT_DB, ILD_LIMIT_DB)
    l_db, r_db = ears(cll, ild)
    hl, hr = min_phase(l_db), min_phase(r_db)
    return hl, hr, head * (y[:(ITD_ORDER + 1) ** 2] @ itd_c) - itd_estimate(hl, hr)


def main(path):
    f = h5py.File(path)
    pos, h = f["SourcePosition"][:], f["Data.IR"][:]
    az, el = np.radians(pos[:, 0]), np.radians(pos[:, 1])
    xyz = np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], 1)
    w = SphericalVoronoi(xyz).calculate_areas(); w /= w.sum()
    sw = np.sqrt(w)[:, None]

    floor = 10 ** (FLOOR_DB / 10)
    p = np.abs(np.fft.rfft(h, NFFT, axis=-1)) ** 2                          # dirs x ears x bins
    db = 10 * np.log10(np.maximum(p, floor))
    cll = 10 * np.log10(np.maximum(p[:, 0] + p[:, 1], floor))
    ild = np.clip(10 * np.log10(np.maximum(p[:, 0], floor) / np.maximum(p[:, 1], floor)), -ILD_LIMIT_DB, ILD_LIMIT_DB)

    y = bf.sh_sn3d(ORDER, az, el)
    pinv = np.linalg.pinv(y * sw)
    cll_c, ild_c = pinv @ (cll * sw), pinv @ (ild * sw)                       # coefs x bins

    fr = np.fft.rfftfreq(NFFT, 1 / FS)
    med = (np.abs(pos[:, 0]) < 1) | (np.abs(np.abs(pos[:, 0]) - 180) < 1)
    k = (fr >= 8000) & (fr < 16000)
    for name, x, c in (("CLL", cll, cll_c), ("ILD", ild, ild_c)):
        e = y @ c - x
        print(f"{name} at order {ORDER}: weighted RMS {np.sqrt(np.sum(w[:, None] * e ** 2) / e.shape[1]):.2f} dB, "
              f"median plane 8–16 kHz {np.sqrt(np.mean(e[med][:, k] ** 2)):.2f} dB")

    itd = np.array([itd_estimate(h[i, 0], h[i, 1]) for i in range(len(h))])
    yi = y[:, :(ITD_ORDER + 1) ** 2]
    itd_c = np.linalg.pinv(yi * sw) @ (itd * sw[:, 0])
    res = yi @ itd_c - itd
    print(f"ITD: largest {1e6 * np.abs(itd).max():.0f} µs; order-{ITD_ORDER} residual RMS "
          f"{1e6 * np.sqrt(np.sum(w * res ** 2)):.1f} µs, worst {1e6 * np.abs(res).max():.0f} µs")

    # The chain against the measured HRIRs, at the KU100's own size.
    rng = np.random.default_rng(1)
    mag_err, itd_err = [], []
    for i in rng.choice(len(h), 300, replace=False):
        hl, hr, d = render(y[i], cll_c, ild_c, itd_c)
        for e, he in ((0, hl), (1, hr)):
            got = 10 * np.log10(np.maximum(np.abs(np.fft.rfft(he, NFFT)) ** 2, floor))
            m = (fr >= 200) & (fr < 16000) & (db[i, e] > -30)
            mag_err.append(np.sqrt(np.mean((got[m] - db[i, e, m]) ** 2)))
        n = 1024
        sh = np.exp(-2j * np.pi * np.fft.rfftfreq(n) * np.array([max(d, 0), max(-d, 0)])[:, None] * FS)
        l, r = (np.fft.irfft(np.fft.rfft(x, n) * s, n) for x, s in zip((hl, hr), sh))
        itd_err.append(itd_estimate(l[:ITD_N], r[:ITD_N]) - itd[i])
    print(f"the chain against the measured HRIRs, 300 directions: magnitude RMS {np.median(mag_err):.2f} dB median "
          f"(0.2–16 kHz, where above −30 dB); ITD error median {1e6 * np.median(np.abs(itd_err)):.0f} µs, "
          f"worst {1e6 * np.max(np.abs(itd_err)):.0f} µs")

    # What each control moves, and what it must leave alone, at 60° left.
    i = np.argmin(np.abs(pos[:, 0] - 60) + np.abs(pos[:, 1]))
    hl0, hr0, _ = render(y[i], cll_c, ild_c, itd_c)
    ild_of = lambda hl, hr: 20 * np.log10(np.abs(np.fft.rfft(hl, NFFT)) / np.abs(np.fft.rfft(hr, NFFT)))
    cll_of = lambda hl, hr: 10 * np.log10(np.abs(np.fft.rfft(hl, NFFT)) ** 2 + np.abs(np.fft.rfft(hr, NFFT)) ** 2)
    for s in (0.9, 1.1):
        hl, hr, _ = render(y[i], cll_c, ild_c, itd_c, ear=s)
        print(f"ear size {s:.0%}: the rendered CLL moves up to {np.abs(cll_of(hl, hr) - cll_of(hl0, hr0)).max():.1f} dB, "
              f"the rendered ILD up to {np.abs(ild_of(hl, hr) - ild_of(hl0, hr0)).max():.2f} dB")
        hl, hr, _ = render(y[i], cll_c, ild_c, itd_c, head=s)
        print(f"head size {s:.0%}: ITD {1e6 * s * (yi[i] @ itd_c):.0f} µs (KU100 {1e6 * yi[i] @ itd_c:.0f}); the rendered "
              f"ILD moves up to {np.abs(ild_of(hl, hr) - ild_of(hl0, hr0)).max():.1f} dB, the rendered CLL up to "
              f"{np.abs(cll_of(hl, hr) - cll_of(hl0, hr0)).max():.2f} dB")

    blob = lambda a: base64.b64encode(np.ascontiguousarray(a, dtype="<f4").tobytes()).decode()
    with open("match.js", "w") as js:
        js.write("// Generated by build-match.py from the SADIE II KU100 HRIRs (subject D1),\n")
        js.write("// Copyright 2018 University of York, Apache License 2.0 (https://www.apache.org/licenses/LICENSE-2.0).\n")
        js.write("// CLL, ILD: dB, [coef][bin], SN3D ACN, 129 bins of a 256-point FFT at 48 kHz. ITD: tL − tR in seconds, [coef].\n")
        js.write(f"const MATCH = {{ order: {ORDER}, itdOrder: {ITD_ORDER}, bins: {NFFT // 2 + 1}, ildLimitDb: {ILD_LIMIT_DB},\n"
                 f"  itdN: {ITD_N}, itdUp: {ITD_UP}, itdFc: {ITD_FC},\n"
                 f"  cll: \"{blob(cll_c)}\",\n  ild: \"{blob(ild_c)}\",\n  itd: \"{blob(itd_c)}\" }};\n")


if __name__ == "__main__":
    main(sys.argv[1])
