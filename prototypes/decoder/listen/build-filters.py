"""Binaural decode filters for the prototype, from the SADIE II KU100 (subject D1, 48 kHz, 8802 directions).

Writes filters.js: for orders 1-3 and two methods, and order 7 MagLS for rendering loudspeakers as points, one FIR per ACN channel and ear, for AmbiX (SN3D) input.

  ls     complex least squares at every frequency
  magls  least squares below the cutoff, magnitude least squares above (the phase carried from the bin below),
         then the diffuse-field covariance constraint: a 2x2 mix per bin that gives the decode the HRTF set's own
         diffuse-field ear spectra and interaural coherence

Also prints how far each decode is from the measured HRTFs, so the choice is measured, not assumed.

  python build-filters.py path/to/D1_48K_24bit_256tap_FIR_SOFA.sofa
"""
import base64, sys
import h5py
import numpy as np
from scipy.special import lpmv
from scipy.spatial import SphericalVoronoi
from math import factorial

NFFT = 512          # the 256-tap HRIRs zero-padded; the filters are cut to TAPS
TAPS = 256
FADE = 32           # half-Hann fade over the last FADE taps
DELAY = 64          # samples: the filters' bulk delay, so the part of a response ahead of the head's centre is causal
C, R_HEAD = 343.0, 0.0875


def sh_sn3d(order, az, el):
    """Real SN3D spherical harmonics in ACN order, no Condon-Shortley phase. az, el in radians. Rows: directions."""
    x = np.sin(el)
    out = np.zeros((len(az), (order + 1) ** 2))
    for n in range(order + 1):
        for m in range(-n, n + 1):
            am = abs(m)
            p = lpmv(am, n, x) * (-1) ** am            # scipy includes Condon-Shortley; remove it
            norm = np.sqrt((2 - (m == 0)) * factorial(n - am) / factorial(n + am))
            trig = np.cos(am * az) if m >= 0 else np.sin(am * az)
            out[:, n * n + n + m] = norm * p * trig
    return out


def sqrtm_psd(a):
    w, v = np.linalg.eigh(a)
    return (v * np.sqrt(np.maximum(w, 1e-12))) @ v.conj().T


def covariance_constraint(b, h, wq, dn):
    """b: (ch, 2) decode at one bin; h: (dirs, 2) HRTFs; wq: quadrature weights summing to 1; dn: diffuse-field
    power per SH channel. Returns b mixed so its diffuse-field ear covariance equals the HRTF set's (optimal mixing
    with the least change to the signals)."""
    target = (h.T * wq) @ h.conj()                      # E[y yᴴ] of the ear signals, 2x2
    decoded = b.T @ (dn[:, None] * b.conj())
    kx, ky = sqrtm_psd(decoded), sqrtm_psd(target)
    u, _, vh = np.linalg.svd(kx.conj().T @ ky)
    p = vh.conj().T @ u.conj().T
    m = ky @ p @ np.linalg.inv(kx)
    return b @ m.T


def design(order, y, hf, wq, freqs, method):
    sw = np.sqrt(wq)[:, None]
    yw_pinv = np.linalg.pinv(y * sw)
    n_sh = y.shape[1]
    dn = np.array([1.0 / (2 * n + 1) for n in range(order + 1) for _ in range(2 * n + 1)])
    fc = order * C / (2 * np.pi * R_HEAD)
    b = np.zeros((len(freqs), n_sh, 2), complex)
    for k, f in enumerate(freqs):
        if method == "ls" or f <= fc or k == 0:
            b[k] = yw_pinv @ (hf[k] * sw)
        else:
            phase = np.angle(y @ b[k - 1])
            b[k] = yw_pinv @ (np.abs(hf[k]) * np.exp(1j * phase) * sw)
    if method == "magls":
        for k in range(len(freqs)):
            b[k] = covariance_constraint(b[k], hf[k], wq, dn)
    ir = np.fft.irfft(b * np.exp(-2j * np.pi * freqs * DELAY / 48000)[:, None, None], NFFT, axis=0)[:TAPS]
    ir[TAPS - FADE:] *= (0.5 + 0.5 * np.cos(np.linspace(0, np.pi, FADE)))[:, None, None]
    return b, ir, fc


def errors(b, y, hf, wq, freqs, dn):
    """Weighted-over-directions magnitude error (dB) in bands, and the diffuse-field spectrum error (dB)."""
    rows = []
    for lo, hi in [(200, 1000), (1000, 4000), (4000, 8000), (8000, 16000)]:
        ks = np.where((freqs >= lo) & (freqs < hi))[0]
        e = []
        for k in ks:
            d = 20 * np.log10(np.abs(y @ b[k]) + 1e-9) - 20 * np.log10(np.abs(hf[k]) + 1e-9)
            e.append(np.sqrt(np.sum(wq[:, None] * d ** 2) / 2))
        rows.append(np.mean(e))
    df = []
    for k in np.where((freqs >= 200) & (freqs < 16000))[0]:
        dec = np.real(np.einsum("ce,c,ce->e", b[k].conj(), dn, b[k]))
        tgt = np.real(np.sum(wq[:, None] * np.abs(hf[k]) ** 2, axis=0))
        df.append(np.max(np.abs(10 * np.log10(dec / tgt))))
    return rows, np.max(df)


def main(path):
    f = h5py.File(path)
    pos = f["SourcePosition"][:]
    hrir = f["Data.IR"][:]                               # (dirs, ears, taps); ear 0 is left
    az, el = np.radians(pos[:, 0]), np.radians(pos[:, 1])
    xyz = np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], 1)
    wq = SphericalVoronoi(xyz).calculate_areas()
    wq /= wq.sum()
    freqs = np.fft.rfftfreq(NFFT, 1 / 48000)
    # The set's common delay is removed before the design: magnitude LS carries the phase over from the bin below and
    # so drops any bulk delay above the cutoff, which would put the highs ahead of the lows. DELAY restores one.
    bulk = int(np.round(np.median(np.abs(hrir).argmax(-1).mean(-1))))
    hf = np.fft.rfft(hrir, NFFT, axis=-1).transpose(2, 0, 1) * np.exp(2j * np.pi * freqs * bulk / 48000)[:, None, None]
    print(f"bulk delay of the set {bulk} samples, filters delayed {DELAY}")

    left90 = np.argmin(np.abs(pos[:, 0] - 90) + np.abs(pos[:, 1]))
    assert np.sum(hrir[left90, 0] ** 2) > np.sum(hrir[left90, 1] ** 2), "ear 0 must be the left ear"

    out = {}
    front = np.argmin(np.abs(pos[:, 0]) + np.abs(pos[:, 1]))
    e = np.sum(hrir[front] ** 2, -1)
    print(f"front source, left minus right, measured: {10 * np.log10(e[0] / e[1]):.2f} dB")
    print("order method   fc    |mag err dB  0.2-1k  1-4k  4-8k  8-16k|  diffuse max dB | front L-R | outside 0-192")
    for order, methods in ((1, ("ls", "magls")), (2, ("ls", "magls")), (3, ("ls", "magls")), (7, ("magls",))):
        y = sh_sn3d(order, az, el)
        dn = np.array([1.0 / (2 * n + 1) for n in range(order + 1) for _ in range(2 * n + 1)])
        for method in methods:
            b, ir, fc = design(order, y, hf, wq, freqs, method)
            bands, df = errors(b, y, hf, wq, freqs, dn)
            outside = np.sum(ir[192:] ** 2) / np.sum(ir ** 2)
            o = np.einsum("tce,c->te", ir, y[front])
            lr = 10 * np.log10(np.sum(o[:, 0] ** 2) / np.sum(o[:, 1] ** 2))
            print(f"  {order}   {method:6s} {fc:5.0f}  |  " + "  ".join(f"{e:5.1f}" for e in bands)
                  + f"  |  {df:5.1f}         | {lr:6.2f}    | {100 * outside:.2f} %")
            # layout: [ear][ch][tap], float32
            out[f"{method}{order}"] = base64.b64encode(
                np.ascontiguousarray(ir.transpose(2, 1, 0), dtype="<f4").tobytes()).decode()

    with open("filters.js", "w") as js:
        js.write("// Generated by build-filters.py from the SADIE II KU100 HRIRs (subject D1),\n")
        js.write("// Copyright 2018 University of York, Apache License 2.0 (https://www.apache.org/licenses/LICENSE-2.0).\n// Layout per set: [ear][acn][tap], float32.\n")
        js.write(f"const FILTER_TAPS = {TAPS};\nconst FILTERS = {{\n")
        for k, v in out.items():
            js.write(f'  {k}: "{v}",\n')
        js.write("};\n")


if __name__ == "__main__":
    main(sys.argv[1])
