"""Step 1 of the model: separate where the cues sit from how strong they are, by aligning heads in frequency.

The KU100 gives the timbre — its CTF, on which every head of the data set already sits — and the direction cues
come from a model of many heads on top of it. A plain average of heads is no template for that: notches at 7 kHz in
one head and 8 kHz in another average to two shallow dips. So the template is built by alignment: every head is
warped in log-frequency onto the current template, the aligned heads are averaged into the next one, and so on.

Here on the CLL's direction-dependent part (the DTF: the CLL less its average over directions, which is the CTF)
in the median plane, front and back, on a log-frequency axis 1–18 kHz. A head's warp is a shift in octaves that
depends on the polar angle θ and on frequency:
    τ(θ, f) = Σ_k Σ_j a_kj · B_k(θ) · S_j(log f)
B_k: 1, cos(πu), cos(2πu), … with u the polar angle scaled to 0…1; S_j: hat functions at J anchor points spread
evenly over the log-frequency axis (J = 1 is one shift for all frequencies). A small ridge penalty keeps the warp
from chasing noise; each coefficient is held within ±0.5 octaves, and a warp that folds the frequency axis back is counted.

Measured for each size of warp: how much of the variation between heads it takes out — on a held-out half, aligned
to a template built from the other half, so more parameters cannot simply fit noise — and how deep the template's
notches come out against a single head's.

  python align.py <data set .npz>
"""
import os, sys
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np
from scipy.optimize import least_squares
import plotly.graph_objects as go
from plotly.subplots import make_subplots

_d = os.path.dirname(os.path.abspath(__file__))
_s = spec_from_file_location("ge", os.path.join(_d, "grid-experiment.py")); ge = module_from_spec(_s); _s.loader.exec_module(ge)

F_LO, F_HI, NF = 1000.0, 18000.0, 120
LOGF = np.linspace(np.log2(F_LO), np.log2(F_HI), NF)          # octaves
POLAR = np.arange(-45, 226, 5.0)                               # 0 front, 90 up, 180 back
LIMIT, ITERATIONS, RIDGE = 0.5, 5, 0.02
BAND = (LOGF >= np.log2(2000)) & (LOGF <= np.log2(16000))
NOTCH = (LOGF >= np.log2(5000)) & (LOGF <= np.log2(12000))
CONFIGS = [(1, 1), (4, 1), (4, 2), (4, 3), (4, 4), (4, 6), (6, 6), (6, 8)]   # (elevation terms K, frequency anchors J)


def median_plane(coefs, order, nfft, fs):
    """The CLL's DTF (dB) on the median plane at POLAR, on the log-frequency axis: heads x angles x NF."""
    az = np.where(POLAR <= 90, 0.0, np.pi)
    el = np.radians(np.where(POLAR <= 90, POLAR, 180 - POLAR))
    y = ge.sh_orth(order, az, el)
    nb = nfft // 2 + 1
    fr = np.fft.rfftfreq(nfft, 1 / fs)
    cll = np.einsum("dc,hcb->hdb", y, coefs[:, :, :nb])
    dtf = cll - (coefs[:, 0, :nb] / np.sqrt(4 * np.pi))[:, None, :]     # less the average over all directions
    return np.stack([[np.interp(2 ** LOGF, fr, row) for row in head] for head in dtf])


def angle_basis(k):
    u = (POLAR - POLAR[0]) / (POLAR[-1] - POLAR[0])
    return np.stack([np.ones_like(u)] + [np.cos(np.pi * i * u) for i in range(1, k)], 1)       # angles x K


def freq_basis(j):
    if j == 1:
        return np.ones((NF, 1))
    knots = np.linspace(LOGF[0], LOGF[-1], j)
    return np.stack([np.interp(LOGF, knots, np.eye(j)[i]) for i in range(j)], 1)               # NF x J


def warp_field(a, bk, sj):
    return bk @ a.reshape(bk.shape[1], sj.shape[1]) @ sj.T                                     # angles x NF


def read(x, tau):
    """x read at log-frequency LOGF + τ, row by row: a feature at f in x appears at f·2^−τ."""
    return np.stack([np.interp(LOGF + t, LOGF, row) for row, t in zip(x, tau)])


def align(x, template, bk, sj, a0):
    """Warp coefficients moving head x onto the template: least squares over 2–16 kHz plus a ridge on the
    coefficients, with the analytic Jacobian."""
    grad = np.gradient(x, LOGF, axis=1)
    k, j = bk.shape[1], sj.shape[1]

    def res(a):
        tau = warp_field(a, bk, sj)
        return np.r_[(read(x, tau) - template)[:, BAND].ravel(), np.sqrt(RIDGE) * a * 10]

    def jac(a):
        tau = warp_field(a, bk, sj)
        g = read(grad, tau)[:, BAND]                                     # angles x band
        # d residual(angle, f) / d a_kj = g(angle, f) · B_k(angle) · S_j(f)
        jm = np.einsum("af,ak,fj->afkj", g, bk, sj[BAND]).reshape(-1, k * j)
        return np.r_[jm, np.sqrt(RIDGE) * 10 * np.eye(k * j)]

    return least_squares(res, np.clip(a0, -LIMIT * 0.999, LIMIT * 0.999), jac=jac, bounds=(-LIMIT, LIMIT)).x


def build_template(heads, bk, sj):
    n = bk.shape[1] * sj.shape[1]
    template, coefs = heads.mean(0), np.zeros((len(heads), n))
    for _ in range(ITERATIONS):
        coefs = np.stack([align(x, template, bk, sj, c) for x, c in zip(heads, coefs)])
        centre = coefs.mean(0)                                           # keep the template centred: mean warp zero
        coefs -= centre
        template = np.stack([read(x, warp_field(c, bk, sj)) for x, c in zip(heads, coefs)]).mean(0)
    return template, coefs


def depth(m):
    return float(np.mean(np.max(m[:, NOTCH], 1) - np.min(m[:, NOTCH], 1)))


def folds(coefs, bk, sj):
    """Warps that fold the frequency axis: log f + τ not increasing somewhere."""
    return int(sum(np.any(np.diff(LOGF[None, :] + warp_field(c, bk, sj), axis=1) <= 0) for c in coefs))


def main(path):
    data = np.load(path)
    dtf = median_plane(data["cll_ild"].astype(float), int(data["order"]), int(data["nfft"]), int(data["fs"]))
    rng = np.random.default_rng(0)
    half = rng.permutation(len(dtf))
    train, test = dtf[half[: len(dtf) // 2]], dtf[half[len(dtf) // 2:]]
    var0 = np.mean(test[:, :, BAND].var(0))
    print(f"{len(dtf)} heads; median plane {POLAR[0]:.0f}…{POLAR[-1]:.0f}°, {F_LO / 1000:.0f}–{F_HI / 1000:.0f} kHz log-spaced. "
          f"Variation between held-out heads, 2–16 kHz: {np.sqrt(var0):.2f} dB RMS. A single head's notch depth "
          f"(5–12 kHz): {np.mean([depth(x) for x in dtf]):.1f} dB; the plain average's {depth(dtf.mean(0)):.1f} dB")
    rows, keep = [], {}
    for k, j in CONFIGS:
        bk, sj = angle_basis(k), freq_basis(j)
        template, _ = build_template(train, bk, sj)
        tc = np.stack([align(x, template, bk, sj, np.zeros(k * j)) for x in test])
        aligned = np.stack([read(x, warp_field(c, bk, sj)) for x, c in zip(test, tc)])
        var1 = np.mean(aligned[:, :, BAND].var(0))
        rows.append((k * j, k, j, 100 * (1 - var1 / var0), np.sqrt(var1), depth(template), folds(tc, bk, sj)))
        keep[(k, j)] = (template, test, aligned, tc, bk, sj)
        r = rows[-1]
        print(f"  {r[0]:3d} parameters (K {k}, J {j}): {r[3]:4.1f} % of the variation taken out on held-out heads "
              f"({np.sqrt(var0):.2f} → {r[4]:.2f} dB); template notch depth {r[5]:.1f} dB; folding warps {r[6]}")
    plot(rows, keep, dtf)


def plot(rows, keep, dtf):
    out = ge.OUT
    fx = 2 ** LOGF
    rows = np.array(rows, dtype=float)
    fig = make_subplots(rows=1, cols=2, subplot_titles=["variation taken out, held-out heads", "template notch depth"])
    fig.add_trace(go.Scatter(x=rows[:, 0], y=rows[:, 3], mode="lines+markers", line=dict(color="#1b1a17"),
                             text=[f"K {int(k)}, J {int(j)}" for k, j in rows[:, 1:3]]), row=1, col=1)
    fig.add_trace(go.Scatter(x=rows[:, 0], y=rows[:, 5], mode="lines+markers", line=dict(color="#d4873a"), name="template"), row=1, col=2)
    fig.add_hline(y=np.mean([depth(x) for x in dtf]), line=dict(color="#6f6b62", dash="dash"), row=1, col=2,
                  annotation_text="a single head")
    fig.add_hline(y=depth(dtf.mean(0)), line=dict(color="#a9a59c", dash="dot"), row=1, col=2, annotation_text="plain average")
    fig.update_xaxes(type="log", title_text="warp parameters per head")
    fig.update_yaxes(title_text="% of variance", row=1, col=1); fig.update_yaxes(title_text="dB", row=1, col=2)
    fig.update_layout(height=440, template="plotly_white", showlegend=False,
                      title="How far frequency warping explains the differences between heads (median-plane CLL DTF)")
    fig.write_html(os.path.join(out, "align-curve.html"), include_plotlyjs="cdn")

    cfgs = list(keep)
    fig = make_subplots(rows=1, cols=len(cfgs) + 1, subplot_titles=["plain average"] + [f"K {k}, J {j}" for k, j in cfgs],
                        shared_yaxes=True)
    fig.add_trace(go.Heatmap(x=fx, y=POLAR, z=dtf.mean(0), zmin=-15, zmax=10, colorscale="Greys_r", showscale=False), row=1, col=1)
    for i, c in enumerate(cfgs):
        fig.add_trace(go.Heatmap(x=fx, y=POLAR, z=keep[c][0], zmin=-15, zmax=10, colorscale="Greys_r",
                                 showscale=i == len(cfgs) - 1, colorbar=dict(title="dB")), row=1, col=i + 2)
    fig.update_xaxes(type="log", range=[np.log10(2000), np.log10(18000)])
    fig.update_yaxes(title_text="polar angle ° (0 front, 90 up, 180 back)", row=1, col=1)
    fig.update_layout(title="The template for each size of warp — built from half the heads", height=480, template="plotly_white")
    fig.write_html(os.path.join(out, "align-template.html"), include_plotlyjs="cdn")

    best = max(keep, key=lambda c: c[0] * c[1] if c[0] * c[1] <= 24 else 0)
    template, test, aligned, tc, bk, sj = keep[best]
    pick = [0, 30, 60, 100]
    fig = make_subplots(rows=3, cols=len(pick), shared_yaxes=True,
                        subplot_titles=[f"held-out head {p}" for p in pick] + ["aligned" for _ in pick] + ["its warp, octaves" for _ in pick])
    for j, p in enumerate(pick):
        fig.add_trace(go.Heatmap(x=fx, y=POLAR, z=test[p], zmin=-15, zmax=10, colorscale="Greys_r", showscale=False), row=1, col=j + 1)
        fig.add_trace(go.Heatmap(x=fx, y=POLAR, z=aligned[p], zmin=-15, zmax=10, colorscale="Greys_r", showscale=False), row=2, col=j + 1)
        fig.add_trace(go.Heatmap(x=fx, y=POLAR, z=warp_field(tc[p], bk, sj), zmin=-0.5, zmax=0.5, colorscale="RdBu",
                                 showscale=j == len(pick) - 1, colorbar=dict(title="oct")), row=3, col=j + 1)
    fig.update_xaxes(type="log", range=[np.log10(2000), np.log10(18000)])
    fig.update_layout(title=f"Held-out heads before and after alignment, and their warps (K {best[0]}, J {best[1]})",
                      height=860, template="plotly_white")
    fig.write_html(os.path.join(out, "align-heads.html"), include_plotlyjs="cdn")


if __name__ == "__main__":
    main(sys.argv[1])
