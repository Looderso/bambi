"""How much do the baselines vary between heads — before deciding which one the model keeps fixed?

The baseline is extract.py's split at 3 ERB: each direction's CLL DTF smoothed with a 3-ERB window; the notch layer is
the rest. Over all 274 heads:
  - the spread between heads (standard deviation, dB) of the baseline, the notch layer and the whole DTF, per
    frequency over the sphere and as maps of the median plane
  - per head, how far its baseline is from the population's (RMS dB over directions, 2–16 kHz) — the whole
    distribution, so a few very different heads would show — and where the KU100's lies in it
  - how simple the variation is: the share of the baseline's variance between heads that its first principal
    components hold, and what the first three look like in the median plane

  python baseline.py <data set .npz>
"""
import os, sys
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np
import plotly.graph_objects as go
from plotly.subplots import make_subplots

_d = os.path.dirname(os.path.abspath(__file__))
_s = spec_from_file_location("ex", os.path.join(_d, "extract.py")); ex = module_from_spec(_s); _s.loader.exec_module(ex)
ge = ex.ge
W = 3


def main(path):
    data = np.load(path)
    coefs = data["cll_ild"].astype(float)
    ku_c = data["ku100_cll_ild"].astype(float)[None]
    order, nfft, fs = int(data["order"]), int(data["nfft"]), int(data["fs"])
    saz, sel = ex.sphere()
    heads = ex.dtf(coefs, order, nfft, fs, saz, sel)
    base, layer = ex.split(heads, W)
    ku_base = ex.split(ex.dtf(ku_c, order, nfft, fs, saz, sel)[0], W)[0]
    B = ex.BAND
    sd = {n: x.std(0) for n, x in (("baseline", base), ("notch layer", layer), ("whole DTF", heads))}   # dirs x freq
    print(f"spread between heads, 2–16 kHz, mean over directions (dB): " +
          ", ".join(f"{n} {np.mean(v[:, B]):.2f}" for n, v in sd.items()))
    for lo, hi in ((2000, 4000), (4000, 8000), (8000, 16000)):
        k = (ex.FREQ >= lo) & (ex.FREQ < hi)
        print(f"  {lo / 1000:.0f}–{hi / 1000:.0f} kHz: " + ", ".join(f"{n} {np.mean(v[:, k]):.2f}" for n, v in sd.items()))

    mean_base = base.mean(0)
    d = np.sqrt(np.mean((base - mean_base)[..., B] ** 2, axis=(1, 2)))
    dk = float(np.sqrt(np.mean((ku_base - mean_base)[..., B] ** 2)))
    pct = lambda q: np.percentile(d, q)
    print(f"\na head's baseline from the population's (RMS dB, 2–16 kHz): median {pct(50):.2f}, 5th {pct(5):.2f}, 95th {pct(95):.2f}, "
          f"largest {d.max():.2f}; the KU100's {dk:.2f} — further than {100 * np.mean(d < dk):.0f} % of the heads")
    names = list(data["database_names"])
    for i, n in enumerate(names):
        print(f"  {n}: median {np.median(d[data['database'] == i]):.2f} dB")

    X = (base - mean_base)[..., B].reshape(len(base), -1)
    u, s, vt = np.linalg.svd(X, full_matrices=False)
    share = np.cumsum(s ** 2) / np.sum(s ** 2)
    print("\nshare of the baseline's variance between heads held by the first principal components: " +
          ", ".join(f"{k} → {100 * share[k - 1]:.0f} %" for k in (1, 2, 3, 5, 10, 20)))
    Xn = layer[..., B].reshape(len(layer), -1); Xn = Xn - Xn.mean(0)
    sn = np.linalg.svd(Xn, compute_uv=False)
    sharen = np.cumsum(sn ** 2) / np.sum(sn ** 2)
    print("  the notch layer's, for comparison: " + ", ".join(f"{k} → {100 * sharen[k - 1]:.0f} %" for k in (1, 2, 3, 5, 10, 20)))

    plot(data, coefs, ku_c, order, nfft, fs, sd, d, dk, share, sharen, vt, s, len(base))


def plot(data, coefs, ku_c, order, nfft, fs, sd, d, dk, share, sharen, vt, s, n):
    out = ge.OUT
    F = ex.FREQ
    # 1. Spread per frequency, mean over the sphere.
    fig = make_subplots(rows=1, cols=2, subplot_titles=["spread between heads, mean over directions",
                                                       "a head's baseline from the population's"])
    for name, col in (("baseline", "#1b1a17"), ("notch layer", "#d4873a"), ("whole DTF", "#a9a59c")):
        fig.add_trace(go.Scatter(x=F, y=sd[name].mean(0), name=name, line=dict(color=col)), row=1, col=1)
    fig.add_trace(go.Histogram(x=d, nbinsx=40, marker_color="#1b1a17", name="heads", showlegend=False), row=1, col=2)
    fig.add_vline(x=dk, line=dict(color="#d4873a", width=2), annotation_text="KU100", row=1, col=2)
    fig.update_xaxes(type="log", title_text="Hz", row=1, col=1); fig.update_yaxes(title_text="sd, dB", row=1, col=1)
    fig.update_xaxes(title_text="RMS dB, 2–16 kHz", row=1, col=2)
    fig.update_layout(height=440, template="plotly_white", title="How much the 3-ERB baseline varies between the 274 heads")
    fig.write_html(os.path.join(out, "baseline-spread.html"), include_plotlyjs="cdn")

    # 2. Median plane: population baseline, KU100 baseline, spread of baseline and of notch layer.
    maz, mel = ex.median_dirs()
    med = ex.dtf(coefs, order, nfft, fs, maz, mel)
    mb, ml = ex.split(med, W)
    kub = ex.split(ex.dtf(ku_c, order, nfft, fs, maz, mel)[0], W)[0]
    panels = [("population baseline", mb.mean(0), (-15, 10), "Greys_r"), ("KU100 baseline", kub, (-15, 10), "Greys_r"),
              ("spread of the baseline", mb.std(0), (0, 6), "Viridis"), ("spread of the notch layer", ml.std(0), (0, 6), "Viridis")]
    fig = make_subplots(rows=1, cols=4, subplot_titles=[p[0] for p in panels], shared_yaxes=True)
    for i, (t, z, lim, cs) in enumerate(panels):
        fig.add_trace(go.Heatmap(x=F, y=ex.POLAR, z=z, zmin=lim[0], zmax=lim[1], colorscale=cs,
                                 showscale=i in (1, 3), colorbar=dict(x=0.47 if i == 1 else 1.0, title="dB")), row=1, col=i + 1)
    fig.update_xaxes(type="log", range=[np.log10(1500), np.log10(18000)])
    fig.update_yaxes(title_text="polar ° (0 front, 90 up, 180 back)", row=1, col=1)
    fig.update_layout(height=520, template="plotly_white", title="Median plane, 3-ERB split")
    fig.write_html(os.path.join(out, "baseline-median.html"), include_plotlyjs="cdn")

    # 3. How simple: variance held by the first components, and the first three in the median plane.
    fig = make_subplots(rows=1, cols=4, subplot_titles=["variance held by k components", "baseline component 1",
                                                         "component 2", "component 3"])
    k = np.arange(1, 31)
    fig.add_trace(go.Scatter(x=k, y=100 * share[:30], name="baseline", line=dict(color="#1b1a17")), row=1, col=1)
    fig.add_trace(go.Scatter(x=k, y=100 * sharen[:30], name="notch layer", line=dict(color="#d4873a")), row=1, col=1)
    # Components live on the sphere's directions; draw them on the median plane by the nearest sphere direction.
    saz, sel = ex.sphere()
    near = np.argmax(ge.xyz(maz, mel) @ ge.xyz(saz, sel).T, axis=1)
    nb = int(ex.BAND.sum())
    for c in range(3):
        comp = (vt[c] * s[c] / np.sqrt(n)).reshape(len(saz), nb)[near]
        fig.add_trace(go.Heatmap(x=F[ex.BAND], y=ex.POLAR, z=comp, zmid=0, colorscale="RdBu", showscale=c == 2,
                                 colorbar=dict(title="dB at 1 sd")), row=1, col=c + 2)
    fig.update_xaxes(type="log", range=[np.log10(2000), np.log10(16000)], col=2)
    fig.update_xaxes(type="log", range=[np.log10(2000), np.log10(16000)], col=3)
    fig.update_xaxes(type="log", range=[np.log10(2000), np.log10(16000)], col=4)
    fig.update_xaxes(title_text="components", col=1); fig.update_yaxes(title_text="% of variance", col=1)
    fig.update_layout(height=480, template="plotly_white", title="How many broad patterns the baseline varies along")
    fig.write_html(os.path.join(out, "baseline-components.html"), include_plotlyjs="cdn")


if __name__ == "__main__":
    main(sys.argv[1])
