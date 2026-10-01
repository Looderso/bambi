"""Split the direction cues into a baseline that stays and a notch layer that can move.

Each direction's DTF (the CLL less its average over all directions) on an auditory frequency axis — ERB number,
1–18 kHz in quarter-ERB steps — is smoothed with a Gaussian window W ERB wide (full width at half maximum): that is
the baseline. The notch layer is the rest: the narrow dips and peaks relative to the baseline, as a resonance
suppressor sees them against a smoothed spectrum. (Low-pass liftering of the cepstrum on this axis is the same split.)

Checked for W = 2, 3 and 5 ERB:
  - the split for the KU100 and three heads in the median plane (plots)
  - notch centres (dips in the notch layer below −3 dB, 4–16 kHz) against polar angle, for the KU100 and all heads —
    whether the notch trajectories show up (plot)
  - where the individuality sits: across all heads and 400 directions, 2–16 kHz, the share of the variance between
    heads in the baseline and in the notch layer; and with the auditory measure (positive gradients across ERB
    bands), how far a head is from itself when its baseline is replaced by the population's, and when its notch
    layer is

  python extract.py <data set .npz>
"""
import os, sys
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np
from scipy.ndimage import gaussian_filter1d
import plotly.graph_objects as go
from plotly.subplots import make_subplots

_d = os.path.dirname(os.path.abspath(__file__))
_s = spec_from_file_location("ge", os.path.join(_d, "grid-experiment.py")); ge = module_from_spec(_s); _s.loader.exec_module(ge)

erb_of = lambda f: 21.4 * np.log10(1 + 0.00437 * f)
f_of = lambda e: (10 ** (e / 21.4) - 1) / 0.00437
STEP = 0.25
ERB = np.arange(erb_of(1000), erb_of(18000), STEP)
FREQ = f_of(ERB)
WIDTHS = (2, 3, 5)
BAND = (FREQ >= 2000) & (FREQ <= 16000)
NOTCH_BAND = (FREQ >= 4000) & (FREQ <= 16000)
POLAR = np.arange(-45, 226, 3.0)


def sphere(n=400):
    i = np.arange(n) + 0.5
    return np.pi * (3 - np.sqrt(5)) * i, np.arcsin(1 - 2 * i / n)


def median_dirs():
    return np.where(POLAR <= 90, 0.0, np.pi), np.radians(np.where(POLAR <= 90, POLAR, 180 - POLAR))


def dtf(coefs, order, nfft, fs, az, el):
    """The CLL's DTF (dB) on the ERB axis: heads x dirs x len(ERB)."""
    y = ge.sh_orth(order, az, el)
    nb = nfft // 2 + 1
    fr = np.fft.rfftfreq(nfft, 1 / fs)
    lin = np.einsum("dc,hcb->hdb", y, coefs[:, :, :nb]) - (coefs[:, 0, :nb] / np.sqrt(4 * np.pi))[:, None, :]
    idx = np.searchsorted(fr, FREQ)
    w = (FREQ - fr[idx - 1]) / (fr[idx] - fr[idx - 1])
    return lin[..., idx - 1] * (1 - w) + lin[..., idx] * w


def split(x, width):
    base = gaussian_filter1d(x, width / 2.355 / STEP, axis=-1, mode="nearest")
    return base, x - base


def profile(x):
    """Positive gradient across ERB bands of the DTF smoothed to about one ERB, 2–16 kHz."""
    s = gaussian_filter1d(x, 1 / 2.355 / STEP, axis=-1, mode="nearest")[..., BAND]
    return np.maximum(np.diff(s, axis=-1), 0) / STEP                  # dB per ERB


def dist(a, b):
    """Per head: RMS over directions and bands of the profile difference."""
    return np.sqrt(np.mean((profile(a) - profile(b)) ** 2, axis=(-2, -1)))


def notches(layer, depth=-3.0):
    """(angle index, frequency, depth) of local minima below depth in NOTCH_BAND."""
    out = []
    k = np.where(NOTCH_BAND)[0]
    for a, row in enumerate(layer):
        r = row[k]
        mins = np.where((r[1:-1] < r[:-2]) & (r[1:-1] < r[2:]) & (r[1:-1] < depth))[0] + 1
        out += [(a, FREQ[k[m]], r[m]) for m in mins]
    return out


def main(path):
    data = np.load(path)
    coefs = data["cll_ild"].astype(float)
    ku_c = data["ku100_cll_ild"].astype(float)[None]
    order, nfft, fs = int(data["order"]), int(data["nfft"]), int(data["fs"])
    saz, sel = sphere()
    heads = dtf(coefs, order, nfft, fs, saz, sel)
    print(f"{len(heads)} heads, {heads.shape[1]} directions, {len(ERB)} ERB-axis points "
          f"({ERB[0]:.1f}–{ERB[-1]:.1f} ERB = 1–18 kHz)")
    mean_head = heads.mean(0)
    for w in WIDTHS:
        base, layer = split(heads, w)
        vb = base[..., BAND].var(0).mean(); vn = layer[..., BAND].var(0).mean(); vt = heads[..., BAND].var(0).mean()
        cov = vt - vb - vn
        d_full = dist(heads, mean_head)
        mb, ml = base.mean(0), layer.mean(0)
        keep_notch = dist(mb + layer, heads)                            # population baseline, own notch layer
        keep_base = dist(base + ml, heads)                              # own baseline, population notch layer
        print(f"\nW = {w} ERB: variance between heads (2–16 kHz) — baseline {100 * vb / vt:.0f} %, notch layer "
              f"{100 * vn / vt:.0f} %, their covariance {100 * cov / vt:+.0f} %")
        print(f"  auditory measure, median over heads: a head from the population average {np.median(d_full):.2f} dB/ERB; "
              f"from itself with the population's baseline {np.median(keep_notch):.2f}, with the population's notch layer "
              f"{np.median(keep_base):.2f}")
        # Replacing a layer by the population's moves a head away from itself by what that layer carried.
        print(f"  replacing the notch layer moves a head {100 * np.median(keep_base / d_full):.0f} % of the way to the average; "
              f"replacing the baseline {100 * np.median(keep_notch / d_full):.0f} %")

    maz, mel = median_dirs()
    med = dtf(coefs, order, nfft, fs, maz, mel)
    kum = dtf(ku_c, order, nfft, fs, maz, mel)[0]
    pick = [0, 120, 200]
    for w in WIDTHS:
        rows = [("KU100", kum)] + [(f"head {p}", med[p]) for p in pick]
        fig = make_subplots(rows=len(rows), cols=3, shared_xaxes=True, shared_yaxes=True, vertical_spacing=0.03,
                            subplot_titles=sum(([f"{n} — DTF", "baseline", "notch layer"] for n, _ in rows), []))
        for r, (n, x) in enumerate(rows):
            b, l = split(x, w)
            for c, (z, lim) in enumerate(((x, (-15, 10)), (b, (-15, 10)), (l, (-10, 10)))):
                fig.add_trace(go.Heatmap(x=FREQ, y=POLAR, z=z, zmin=lim[0], zmax=lim[1],
                                         colorscale="Greys_r" if c < 2 else "RdBu", showscale=r == 0 and c in (1, 2),
                                         colorbar=dict(x=0.64 if c == 1 else 1.0, len=0.25, y=0.88, title="dB")), row=r + 1, col=c + 1)
        fig.update_xaxes(type="log", range=[np.log10(1500), np.log10(18000)])
        fig.update_yaxes(title_text="polar °", col=1)
        fig.update_layout(title=f"Median plane, split with a {w}-ERB baseline: polar angle up (0 front, 90 up, 180 back), frequency right",
                          height=1100, template="plotly_white")
        fig.write_html(os.path.join(ge.OUT, f"extract-split-w{w}.html"), include_plotlyjs="cdn")

    w = 3
    pop = []
    for x in med:
        pop += notches(split(x, w)[1])
    pop = np.array(pop)
    ku = np.array(notches(split(kum, w)[1]))
    fig = go.Figure()
    fig.add_trace(go.Histogram2d(x=pop[:, 1], y=POLAR[pop[:, 0].astype(int)], xbins=dict(size=250), ybins=dict(size=6),
                                 colorscale="Greys", showscale=False, name="all heads"))
    fig.add_trace(go.Scatter(x=ku[:, 1], y=POLAR[ku[:, 0].astype(int)], mode="markers", marker=dict(color="#d4873a", size=6),
                             name="KU100"))
    fig.update_xaxes(title_text="notch centre, Hz", range=[4000, 16000])
    fig.update_yaxes(title_text="polar angle ° (0 front, 90 up, 180 back)")
    fig.update_layout(title=f"Notches in the notch layer ({w}-ERB baseline, below −3 dB): all {len(med)} heads (grey density) and the KU100 (orange)",
                      height=620, template="plotly_white")
    fig.write_html(os.path.join(ge.OUT, "extract-notches.html"), include_plotlyjs="cdn")
    print(f"\nnotches found (W = {w}): {len(pop) / len(med):.0f} per head in the median plane on average, {len(ku)} for the KU100")


if __name__ == "__main__":
    main(sys.argv[1])
