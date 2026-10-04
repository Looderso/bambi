"""The measured heads as index.html reads them: one small file a head, loaded when it is chosen.

Every SOFA set under the HRTF folder — hutubs/pp*_HRIRs_measured.sofa, 3d3a/Subject*_HRIRs.sofa, sonicom/p*.sofa,
ku100/*.sofa, and any .sofa beside those folders — is resampled to 48 kHz, cut to TAPS samples from LEAD samples
ahead of the set's earliest onset (the same cut for every direction and ear, so the delays between them stay), faded
out over the last FADE samples, and scaled so that pink noise through its loudest direction and ear comes out as loud
as it went in.

Writes heads/<database>/<name>.bin — int32 the number of directions; float32 azimuth and elevation of each, degrees
(azimuth counter-clockwise from the front, elevation up); float32 the responses, directions x ears x TAPS, left ear
first — and heads/index.json. Prints, per database, the heads, the directions and the loudest peak the page's signals
reach through any response.

  python build-heads.py <HRTF folder>
"""
import glob, json, os, sys
from math import gcd
import h5py
import numpy as np
from scipy.signal import fftconvolve, lfilter, resample_poly

FS, TAPS, LEAD, FADE = 48000, 256, 24, 32
SETS = [("HUTUBS", "hutubs/pp*_HRIRs_measured.sofa"), ("3D3A", "3d3a/Subject*_HRIRs.sofa"), ("SONICOM", "sonicom/p[0-9]*.sofa"),
        ("KU100", "ku100/*48K*.sofa"), ("other", "*.sofa")]
rms = lambda x: np.sqrt(np.mean(x ** 2))


def read(path):
    """A set's directions (azimuth, elevation, degrees) and responses (directions x ears x TAPS) at FS."""
    f = h5py.File(path)
    pos, ir, fs = f["SourcePosition"][:], f["Data.IR"][:].astype(float), int(round(float(f["Data.SamplingRate"][0])))
    if fs != FS:
        ir = resample_poly(ir, FS // gcd(FS, fs), fs // gcd(FS, fs), axis=-1)
    onset = np.argmax(np.abs(ir) > 0.1 * np.abs(ir).max(-1, keepdims=True), -1).min()
    ir = ir[..., max(0, onset - LEAD):][..., :TAPS]
    ir = np.pad(ir, [(0, 0), (0, 0), (0, TAPS - ir.shape[-1])])
    ir[..., -FADE:] *= np.cos(np.linspace(0, np.pi / 2, FADE)) ** 2
    fr = np.fft.rfftfreq(1024, 1 / FS); heard = (fr >= 50) & (fr <= 20000)
    gain = np.sum(np.abs(np.fft.rfft(ir, 1024, axis=-1))[..., heard] ** 2 / fr[heard], -1) / np.sum(1 / fr[heard])
    return (pos[:, 0] + 180) % 360 - 180, pos[:, 1], ir / np.sqrt(gain.max())


def loudest(ir, rng):
    """The largest sample of the page's noise and clicks (−30 dBFS RMS, peaks at most −18 dBFS) through any response."""
    noise = lfilter([0.049922035, -0.095993537, 0.050612699, -0.004408786], [1, -2.494956002, 2.017265875, -0.522189400],
                    rng.standard_normal(FS // 2))
    click = np.zeros(FS // 4); click[:96] = rng.uniform(-1, 1, 96) * np.exp(-np.arange(96) / FS / 0.0004)
    peak = 0
    for x in (noise, click):
        x = x * min(10 ** (-30 / 20) / rms(x), 10 ** (-18 / 20) / np.abs(x).max())
        peak = max(peak, max(np.abs(fftconvolve(x, h)).max() for h in ir.reshape(-1, TAPS)[::7]))
    return 20 * np.log10(peak)


def main(folder):
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "heads")
    index, rng = [], np.random.default_rng(1)
    for name, pattern in SETS:
        os.makedirs(os.path.join(out, name), exist_ok=True)
        peaks, dirs = [], set()
        for path in sorted(glob.glob(os.path.join(folder, pattern))):
            az, el, ir = read(path)
            head = os.path.splitext(os.path.basename(path))[0]
            file = f"{name}/{head}.bin"
            with open(os.path.join(out, file), "wb") as b:
                b.write(np.int32(len(az)).tobytes())
                b.write(np.stack([az, el], -1).astype("<f4").tobytes())
                b.write(ir.astype("<f4").tobytes())
            index.append({"set": name, "name": head, "file": file})
            dirs.add(len(az))
            if len(peaks) < 6:
                peaks.append(loudest(ir, rng))
        n = sum(1 for h in index if h["set"] == name)
        if n:
            print(f"{name}: {n} heads, {sorted(dirs)} directions; loudest peak through {len(peaks)} of them "
                  f"{max(peaks):.1f} dBFS (the page's ceiling: −12)")
    with open(os.path.join(out, "index.json"), "w") as js:
        json.dump({"fs": FS, "taps": TAPS, "heads": index}, js)
    size = sum(os.path.getsize(os.path.join(out, h["file"])) for h in index)
    print(f"wrote {len(index)} heads to {out}, {size / 1e6:.0f} MB")


if __name__ == "__main__":
    main(*sys.argv[1:])
