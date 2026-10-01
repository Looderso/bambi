"""The horizontal cues: how much of the difference between heads does head size alone explain?

The match prototype's head size stretches the KU100's ILD in frequency (a larger head shadows from a lower
frequency up) and scales its ITD. Measured on all 274 heads of the data set, 400 directions over the sphere:
  1. head size: each head's best scale — for the ILD, for the ITD, and one for both — and how much closer that brings
     the KU100 to the head; against the population average (of the other 273 heads)
  2. the ILD split at 3 ERB, as the CLL was (extract.py): a smooth baseline (the shadow) and a notch layer (narrow
     features, mostly the far ear's notches) — the share of the variance between heads in each, and how far a head
     moves when one layer is replaced by the population's
  3. the ITD: how much of a head's difference from the population average one scale factor takes out, and what is
     left — shape, e.g. how the ITD changes with elevation
The ILD measure: the ILD smoothed to about one ERB, RMS difference over directions and 1.5–16 kHz, in dB. The ITD
measure: RMS difference over directions, in µs.

  python horizontal.py <data set .npz>
"""
import os, sys
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np
from scipy.ndimage import gaussian_filter1d
import plotly.graph_objects as go
from plotly.subplots import make_subplots

_d = os.path.dirname(os.path.abspath(__file__))
_s = spec_from_file_location("ex", os.path.join(_d, "extract.py")); ex = module_from_spec(_s); _s.loader.exec_module(ex)
ge = ex.ge
SCALES = np.round(np.arange(0.80, 1.251, 0.01), 3)
BAND = (ex.FREQ >= 1500) & (ex.FREQ <= 16000)


def ild_on(coefs, order, nfft, fs, az, el, scale=1.0):
    """ILD (dB) on the ERB axis, the frequency read at f·scale: heads x dirs x len(ERB)."""
    nb = nfft // 2 + 1
    fr = np.fft.rfftfreq(nfft, 1 / fs)
    lin = np.einsum("dc,hcb->hdb", ge.sh_orth(order, az, el), coefs[:, :, nb:])
    f = np.minimum(ex.FREQ * scale, fr[-1])
    idx = np.clip(np.searchsorted(fr, f), 1, nb - 1)
    w = (f - fr[idx - 1]) / (fr[idx] - fr[idx - 1])
    return lin[..., idx - 1] * (1 - w) + lin[..., idx] * w


def smooth1(x):
    return gaussian_filter1d(x, 1 / 2.355 / ex.STEP, axis=-1, mode="nearest")


def d_ild(a, b):
    return np.sqrt(np.mean((smooth1(a) - smooth1(b))[..., BAND] ** 2, axis=(-2, -1)))


def d_itd(a, b):
    return np.sqrt(np.mean((a - b) ** 2, axis=-1)) * 1e6


def main(path):
    data = np.load(path)
    order, nfft, fs = int(data["order"]), int(data["nfft"]), int(data["fs"])
    coefs = data["cll_ild"].astype(float)
    kc = data["ku100_cll_ild"].astype(float)[None]
    io = int(data["itd_order"])
    az, el = ex.sphere()
    ild = ild_on(coefs, order, nfft, fs, az, el)
    yi = ge.sh_orth(io, az, el)
    itd = data["itd"].astype(float) @ yi.T                              # heads x dirs, seconds
    ku_itd = yi @ data["ku100_itd"].astype(float)
    n = len(ild)
    print(f"{n} heads, {len(az)} directions; ILD over 1.5–16 kHz on the ERB axis, ITD up to "
          f"{1e6 * np.abs(itd).max():.0f} µs (the KU100's up to {1e6 * np.abs(ku_itd).max():.0f})")

    # 1. Head size.
    ku_ild_s = {s: ild_on(kc, order, nfft, fs, az, el, s)[0] for s in SCALES}
    sum_ild, sum_itd = ild.sum(0), itd.sum(0)
    rows = {k: [] for k in ("ild KU100", "ild KU100 best", "ild population", "itd KU100", "itd KU100 best", "itd population",
                            "ild KU100 joint", "itd KU100 joint")}
    best = []
    for h in range(n):
        x, t = ild[h], itd[h]
        dI = np.array([d_ild(ku_ild_s[s], x) for s in SCALES])
        dT = np.array([d_itd(s * ku_itd, t) for s in SCALES])
        pop_i, pop_t = (sum_ild - x) / (n - 1), (sum_itd - t) / (n - 1)
        rows["ild KU100"].append(dI[SCALES == 1.0][0]); rows["ild KU100 best"].append(dI.min()); rows["ild population"].append(d_ild(pop_i, x))
        rows["itd KU100"].append(dT[SCALES == 1.0][0]); rows["itd KU100 best"].append(dT.min()); rows["itd population"].append(d_itd(pop_t, t))
        j = int(np.argmin(dI / dI[SCALES == 1.0][0] + dT / dT[SCALES == 1.0][0]))  # one scale for both, each relative to the KU100
        rows["ild KU100 joint"].append(dI[j]); rows["itd KU100 joint"].append(dT[j])
        best.append((SCALES[int(np.argmin(dI))], SCALES[int(np.argmin(dT))], SCALES[j]))
    r = {k: np.array(v) for k, v in rows.items()}
    best = np.array(best)
    med = lambda k: np.median(r[k])
    print("\n1. head size — distance from the head (median over heads):")
    for q, unit in (("ild", "dB"), ("itd", "µs")):
        base = med(f"{q} KU100")
        print(f"  {q.upper()}: KU100 {base:.2f} {unit}; at its best scale for this cue {med(f'{q} KU100 best'):.2f} "
              f"({100 * (1 - med(f'{q} KU100 best') / base):.0f} % closer); at one scale for both cues {med(f'{q} KU100 joint'):.2f} "
              f"({100 * (1 - med(f'{q} KU100 joint') / base):.0f} %); the population average {med(f'{q} population'):.2f} "
              f"({100 * (1 - med(f'{q} population') / base):.0f} %)")
    print(f"  best scales: ILD {np.percentile(best[:, 0], 5):.2f}…{np.percentile(best[:, 0], 95):.2f}, "
          f"ITD {np.percentile(best[:, 1], 5):.2f}…{np.percentile(best[:, 1], 95):.2f} (5th–95th percentile); "
          f"ILD and ITD scale correlate at r = {np.corrcoef(best[:, 0], best[:, 1])[0, 1]:+.2f}; "
          f"at the ends of the range: {100 * np.mean((best[:, :2] <= SCALES[0]) | (best[:, :2] >= SCALES[-1])):.0f} %")

    # 2. The ILD split.
    b, l = ex.split(ild, 3)
    vb, vl, vt = b[..., BAND].var(0).mean(), l[..., BAND].var(0).mean(), ild[..., BAND].var(0).mean()
    mb, ml = b.mean(0), l.mean(0)
    full = d_ild(ild, ild.mean(0))
    keep_l, keep_b = d_ild(mb + l, ild), d_ild(b + ml, ild)
    print(f"\n2. the ILD split at 3 ERB — variance between heads (1.5–16 kHz): baseline {100 * vb / vt:.0f} %, notch layer "
          f"{100 * vl / vt:.0f} %, covariance {100 * (vt - vb - vl) / vt:+.0f} %")
    print(f"  replacing the baseline by the population's moves a head {100 * np.median(keep_l / full):.0f} % of the way to the average, "
          f"the notch layer {100 * np.median(keep_b / full):.0f} % (median; the ILD measure)")

    # 3. The ITD: one scale against the population average, and what remains.
    pop = itd.mean(0)
    raw = d_itd(itd, pop)
    k = np.sum(itd * pop, 1) / np.sum(pop * pop)                        # least-squares scale per head
    resid = d_itd(itd, k[:, None] * pop)
    e = np.degrees(el)
    print(f"\n3. the ITD — a head from the population average: {np.median(raw):.1f} µs (median); after its best single "
          f"scale {np.median(resid):.1f} µs ({100 * (1 - np.median(resid) / np.median(raw)):.0f} % taken out); scales "
          f"{np.percentile(k, 5):.3f}…{np.percentile(k, 95):.3f}")
    rest = itd - k[:, None] * pop
    for lo, hi in ((-90, -30), (-30, 30), (30, 90)):
        m = (e >= lo) & (e < hi)
        print(f"  what is left, elevation {lo}…{hi}°: {1e6 * np.sqrt(np.mean(rest[:, m] ** 2)):.1f} µs RMS")
    plot(r, best, ild, ku_ild_s, itd, ku_itd, az, el, pop, k)


def plot(r, best, ild, ku_ild_s, itd, ku_itd, az, el, pop, k):
    out = ge.OUT
    fig = make_subplots(rows=1, cols=3, subplot_titles=["ILD: distance from the head (dB)", "ITD: distance from the head (µs)",
                                                         "each head's best scale: ILD against ITD"])
    for c, q in enumerate(("ild", "itd")):
        for name, key in (("KU100", "KU100"), ("KU100, best scale", "KU100 best"), ("KU100, one scale for both", "KU100 joint"),
                          ("population average", "population")):
            fig.add_trace(go.Box(y=r[f"{q} {key}"], name=name, marker_color="#1b1a17", boxpoints=False, showlegend=False), row=1, col=c + 1)
    fig.add_trace(go.Scatter(x=best[:, 0] + np.random.default_rng(0).normal(0, 0.003, len(best)),
                             y=best[:, 1] + np.random.default_rng(1).normal(0, 0.003, len(best)), mode="markers",
                             marker=dict(color="#d4873a", size=5, opacity=0.6), showlegend=False), row=1, col=3)
    fig.update_xaxes(title_text="ILD scale", row=1, col=3); fig.update_yaxes(title_text="ITD scale", row=1, col=3)
    fig.update_layout(height=460, template="plotly_white", title="How much head size explains of the horizontal cues")
    fig.write_html(os.path.join(out, "horizontal-size.html"), include_plotlyjs="cdn")

    # The horizontal plane: ILD against frequency for directions to the left, and ITD against azimuth.
    e = np.degrees(el); a = np.degrees(az) % 360
    hp = np.abs(e) < 12
    order_ = np.argsort(a[hp])
    fig = make_subplots(rows=1, cols=2, subplot_titles=["ILD near the horizontal plane, 60–120° left: heads (grey), KU100 (orange)",
                                                         "ITD near the horizontal plane against azimuth"])
    left = hp & (a > 60) & (a < 120)
    for h in range(0, len(ild), 6):
        fig.add_trace(go.Scatter(x=ex.FREQ, y=ild[h][left].mean(0), mode="lines", line=dict(color="rgba(27,26,23,0.15)", width=1),
                                 showlegend=False), row=1, col=1)
    fig.add_trace(go.Scatter(x=ex.FREQ, y=ku_ild_s[1.0][left].mean(0), mode="lines", line=dict(color="#d4873a", width=2),
                             showlegend=False), row=1, col=1)
    for h in range(0, len(itd), 6):
        fig.add_trace(go.Scatter(x=a[hp][order_], y=1e6 * itd[h][hp][order_], mode="lines", line=dict(color="rgba(27,26,23,0.15)", width=1),
                                 showlegend=False), row=1, col=2)
    fig.add_trace(go.Scatter(x=a[hp][order_], y=1e6 * ku_itd[hp][order_], mode="lines", line=dict(color="#d4873a", width=2),
                             showlegend=False), row=1, col=2)
    fig.update_xaxes(type="log", title_text="Hz", row=1, col=1); fig.update_yaxes(title_text="dB", row=1, col=1)
    fig.update_xaxes(title_text="azimuth ° (90 left)", row=1, col=2); fig.update_yaxes(title_text="µs", row=1, col=2)
    fig.update_layout(height=460, template="plotly_white")
    fig.write_html(os.path.join(out, "horizontal-curves.html"), include_plotlyjs="cdn")


if __name__ == "__main__":
    main(sys.argv[1])
