"""The KU100 as the matching prototype renders it: a magnitude and an interaural delay for any direction.

Writes match.js:
  - the magnitude of each ear in dB, as spherical harmonics of order MAG_ORDER per frequency bin (256-point FFT);
    order 0 is the average over all directions — the timbre — and the rest is the direction-dependent part,
    which is what "ear size" stretches in frequency
  - the interaural time difference tL − tR (seconds, negative when the left ear hears first), as spherical
    harmonics of order ITD_ORDER — measured, so it keeps the head's own elevation dependence; "head size" scales it
  - the ITD the two minimum-phase filters already carry by themselves, the same way: the page delays the ears by
    scale × ITD − that, so the delay is not counted twice

The page turns a direction into two minimum-phase filters and a delay. This script runs that chain here too and
compares it with the measured HRIRs, so what is rendered is known to be the KU100 before anything is changed.

  python build-match.py path/to/D1_48K_24bit_256tap_FIR_SOFA.sofa
"""
import base64, sys
from importlib.util import spec_from_file_location, module_from_spec
import h5py
import numpy as np
from scipy.signal import butter, sosfiltfilt
from scipy.spatial import SphericalVoronoi

_s = spec_from_file_location("bf", "../listen/build-filters.py"); bf = module_from_spec(_s); _s.loader.exec_module(bf)

FS, NFFT = 48000, 256
MAG_ORDER, ITD_ORDER = 20, 10
FLOOR_DB = -60.0
UP = 8                          # cross-correlation resolution: 1/UP of a sample


def lowpass(x):
    """Below 1.5 kHz, zero-phase, padded with silence on both sides: a filtfilt's own edge padding distorts a
    response that starts at its first sample, as a minimum-phase one does."""
    pad = 256
    return sosfiltfilt(butter(4, 1500, fs=FS, output="sos"), np.pad(x, (pad, pad)), axis=-1)


def itd_of(l, r):
    """tL − tR in seconds, from the cross-correlation of the two ears (already low-passed)."""
    n = 2 * len(l)
    cc = np.fft.irfft(np.fft.rfft(l, n) * np.conj(np.fft.rfft(r, n)), n * UP)
    cc = np.concatenate([cc[-len(l) * UP:], cc[:len(l) * UP]])
    return (np.argmax(cc) - len(l) * UP) / UP / FS


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


def main(path):
    f = h5py.File(path)
    pos, h = f["SourcePosition"][:], f["Data.IR"][:]
    az, el = np.radians(pos[:, 0]), np.radians(pos[:, 1])
    xyz = np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], 1)
    w = SphericalVoronoi(xyz).calculate_areas(); w /= w.sum()
    sw = np.sqrt(w)[:, None]

    db = 20 * np.log10(np.maximum(np.abs(np.fft.rfft(h, NFFT, axis=-1)), 10 ** (FLOOR_DB / 20)))
    y = bf.sh_sn3d(MAG_ORDER, az, el)
    p = np.linalg.pinv(y * sw)
    mag = np.stack([p @ (db[:, e, :] * sw) for e in range(2)])            # ears x coefs x bins

    itd = np.array([itd_of(lowpass(h[i, 0]), lowpass(h[i, 1])) for i in range(len(h))])
    yi = bf.sh_sn3d(ITD_ORDER, az, el)
    itd_c = np.linalg.pinv(yi * sw) @ (itd * sw[:, 0])
    res = yi @ itd_c - itd
    itd_mp = np.array([itd_of(lowpass(min_phase(y[i] @ mag[0])), lowpass(min_phase(y[i] @ mag[1])))
                       for i in range(len(h))])
    mp_c = np.linalg.pinv(yi * sw) @ (itd_mp * sw[:, 0])
    print(f"ITD of the minimum-phase pair alone: up to {1e6 * np.abs(itd_mp).max():.0f} µs")
    print(f"ITD: largest {1e6 * np.abs(itd).max():.0f} µs; order-{ITD_ORDER} fit residual RMS "
          f"{1e6 * np.sqrt(np.sum(w * res ** 2)):.1f} µs, worst {1e6 * np.abs(res).max():.0f} µs")

    # The page's chain, here: magnitude from the coefficients, minimum phase, the delay from the ITD coefficients.
    fr = np.fft.rfftfreq(NFFT, 1 / FS)
    rng = np.random.default_rng(1)
    test = rng.choice(len(h), 300, replace=False)
    mag_err, itd_err = [], []
    for i in test:
        for e in range(2):
            m = y[i] @ mag[e]
            hm = min_phase(m)
            got = 20 * np.log10(np.maximum(np.abs(np.fft.rfft(hm, NFFT)), 10 ** (FLOOR_DB / 20)))
            k = (fr >= 200) & (fr < 16000) & (db[i, e] > -30)
            mag_err.append(np.sqrt(np.mean((got[k] - db[i, e, k]) ** 2)))
        d = yi[i] @ itd_c - yi[i] @ mp_c
        hl, hr = min_phase(y[i] @ mag[0]), min_phase(y[i] @ mag[1])
        shift = d * FS                                                    # tL − tR in samples
        n = 1024
        ph = np.exp(-2j * np.pi * np.fft.rfftfreq(n) * np.array([max(shift, 0), max(-shift, 0)])[:, None])
        l, r = (np.fft.irfft(np.fft.rfft(x, n) * p_, n) for x, p_ in zip((hl, hr), ph))
        itd_err.append(itd_of(lowpass(l), lowpass(r)) - itd[i])
    print(f"the page's chain against the measured HRIRs, 300 directions: magnitude RMS {np.median(mag_err):.2f} dB "
          f"(median over ears and directions, 0.2–16 kHz, where above −30 dB), ITD error median "
          f"{1e6 * np.median(np.abs(itd_err)):.0f} µs, worst {1e6 * np.max(np.abs(itd_err)):.0f} µs")

    blob = lambda a: base64.b64encode(np.ascontiguousarray(a, dtype="<f4").tobytes()).decode()
    with open("match.js", "w") as js:
        js.write("// Generated by build-match.py from the SADIE II KU100 HRIRs (subject D1),\n")
        js.write("// Copyright 2018 University of York, Apache License 2.0 (https://www.apache.org/licenses/LICENSE-2.0).\n")
        js.write("// MAG: dB per ear, [ear][coef][bin], SN3D ACN, 129 bins of a 256-point FFT at 48 kHz.\n")
        js.write("// ITD: tL − tR in seconds, [coef]; itdMinPhase: the same, of the minimum-phase pair alone.\n")
        js.write(f"const MATCH = {{ magOrder: {MAG_ORDER}, itdOrder: {ITD_ORDER}, bins: {NFFT // 2 + 1}, "
                 f"floorDb: {FLOOR_DB},\n  mag: \"{blob(mag)}\",\n  itd: \"{blob(itd_c)}\",\n  itdMinPhase: \"{blob(mp_c)}\" }};\n")


if __name__ == "__main__":
    main(sys.argv[1])
