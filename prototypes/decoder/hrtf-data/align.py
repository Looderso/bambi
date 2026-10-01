"""Step 1 of the model: separate where the cues sit from how strong they are, by aligning heads in frequency.

The KU100 gives the timbre — its CTF, on which every head of the data set already sits — and the direction cues
come from a model of many heads on top of it. A plain average of heads is no template for that: notches at 7 kHz in
one head and 8 kHz in another average to two shallow dips. So the template is built by alignment: every head is
shifted in log-frequency onto the current template, the aligned heads are averaged into the next one, and so on.

Here on the CLL's direction-dependent part (the DTF: the CLL less its average over directions, which is the CTF)
in the median plane, front and back, on a log-frequency axis 1–18 kHz. A head's shift is a function of the polar
angle θ: τ(θ) = a0 + a1·cos(πu) + a2·cos(2πu) + a3·cos(3πu), u the polar angle scaled to 0…1 — a constant shift (what
"ear size" is) plus three smooth ones, in octaves, held within ±0.5.

Measured: how much of the variation between heads the shifts alone take out, how much deeper the template's notches
are than the plain average's, and how the shifts spread.

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
N_BASIS, LIMIT, ITERATIONS = 4, 0.5, 6


def median_plane(coefs, order, nfft, fs):
    """CLL (dB) on the median plane at POLAR, on the log-frequency axis: heads x angles x NF."""
    az = np.where(POLAR <= 90, 0.0, np.pi)
    el = np.radians(np.where(POLAR <= 90, POLAR, 180 - POLAR))
    y = ge.sh_orth(order, az, el)
    nb = nfft // 2 + 1
    lin = np.einsum("dc,hcb->hdb", y, coefs[:, :, :nb])                 # CLL columns first
    fr = np.fft.rfftfreq(nfft, 1 / fs)
    return np.stack([[np.interp(2 ** LOGF, fr, row) for row in head] for head in lin])


def basis():
    u = (POLAR - POLAR[0]) / (POLAR[-1] - POLAR[0])
    return np.stack([np.ones_like(u)] + [np.cos(np.pi * k * u) for k in range(1, N_BASIS)], 1)   # angles x N_BASIS


def shifted(x, tau):
    """x (angles x NF) read at log-frequency LOGF + τ(angle): a feature at f in x appears at f·2^−τ."""
    return np.stack([np.interp(LOGF + t, LOGF, row) for row, t in zip(x, tau)])


def align(x, template, b, a0):
    """The shift coefficients that best move head x onto the template, over the band where both are defined."""
    def res(a):
        tau = b @ a
        return (shifted(x, tau) - template).ravel()
    r = least_squares(res, a0, bounds=(-LIMIT, LIMIT))
    return r.x


def main(path):
    data = np.load(path)
    coefs = data["cll_ild"].astype(float)
    order, nfft, fs = int(data["order"]), int(data["nfft"]), int(data["fs"])
    cll = median_plane(coefs, order, nfft, fs)
    # The DTF: less each head's own average over all directions — the CLL's order-0 coefficient times Y00.
    y00 = 1 / np.sqrt(4 * np.pi)
    nb = nfft // 2 + 1
    fr = np.fft.rfftfreq(nfft, 1 / fs)
    c0 = np.stack([np.interp(2 ** LOGF, fr, coefs[h, 0, :nb] * y00) for h in range(len(coefs))])   # heads x NF
    dtf = cll - c0[:, None, :]
    names = list(data["database_names"])
    print(f"{len(dtf)} heads; median plane {POLAR[0]:.0f}…{POLAR[-1]:.0f}° in {len(POLAR)} angles, "
          f"{F_LO / 1000:.0f}–{F_HI / 1000:.0f} kHz in {NF} log-spaced points")

    b = basis()
    mean = dtf.mean(0)
    template, shifts = mean.copy(), np.zeros((len(dtf), N_BASIS))
    band = (LOGF >= np.log2(2000)) & (LOGF <= np.log2(16000))
    for it in range(ITERATIONS):
        shifts = np.stack([align(x, template, b, s) for x, s in zip(dtf, shifts)])
        aligned = np.stack([shifted(x, b @ s) for x, s in zip(dtf, shifts)])
        # The template moves with the heads; keep it centred, so the average shift is zero.
        centre = shifts.mean(0)
        template = shifted(aligned.mean(0), -(b @ centre))
        shifts -= centre
        aligned = np.stack([shifted(x, b @ s) for x, s in zip(dtf, shifts)])
        var_before = np.mean(dtf[:, :, band].var(0))
        var_after = np.mean(aligned[:, :, band].var(0))
        print(f"  iteration {it + 1}: variation between heads 2–16 kHz {np.sqrt(var_before):.2f} → {np.sqrt(var_after):.2f} dB RMS "
              f"({100 * (1 - var_after / var_before):.0f} % of the variance taken out by the shifts)")

    # How deep the notches are: the mean depth of the template's deepest dip per angle, 5–12 kHz, against the plain mean's.
    nk = (LOGF >= np.log2(5000)) & (LOGF <= np.log2(12000))
    depth = lambda m: np.mean(np.max(m[:, nk], 1) - np.min(m[:, nk], 1))
    print(f"notch depth (max − min, 5–12 kHz, mean over angles): plain average {depth(mean):.1f} dB, aligned template {depth(template):.1f} dB, "
          f"a single head {np.mean([depth(x) for x in dtf]):.1f} dB on average")
    for k in range(N_BASIS):
        print(f"  shift coefficient a{k}: spread (sd) {np.std(shifts[:, k]):.3f} octaves "
              f"= ×{2 ** np.std(shifts[:, k]):.3f} in frequency" + ("  — the constant part, 'ear size'" if k == 0 else ""))
    for i, n in enumerate(names):
        sel = data["database"] == i
        print(f"  {n}: mean a0 {np.mean(shifts[sel, 0]):+.3f} octaves")
    plot(mean, template, dtf, aligned, shifts, b)


def plot(mean, template, dtf, aligned, shifts, b):
    out = ge.OUT
    fx = 2 ** LOGF
    fig = make_subplots(rows=1, cols=2, subplot_titles=["plain average of the heads", "aligned template"], shared_yaxes=True)
    for i, z in enumerate((mean, template)):
        fig.add_trace(go.Heatmap(x=fx, y=POLAR, z=z, zmin=-15, zmax=10, colorscale="Greys_r", showscale=i == 1,
                                 colorbar=dict(title="DTF dB")), row=1, col=i + 1)
    fig.update_xaxes(type="log", title_text="Hz"); fig.update_yaxes(title_text="polar angle ° (0 front, 90 up, 180 back)", row=1, col=1)
    fig.update_layout(title="Median-plane DTF of the CLL: the plain average smears the notches, the aligned template keeps them",
                      height=520, template="plotly_white")
    fig.write_html(os.path.join(out, "align-template.html"), include_plotlyjs="cdn")

    pick = [0, 100, 150, 250]
    fig = make_subplots(rows=2, cols=len(pick), subplot_titles=[f"head {p} as measured" for p in pick] + [f"head {p} aligned" for p in pick],
                        shared_yaxes=True)
    for j, p in enumerate(pick):
        for r, z in enumerate((dtf[p], aligned[p])):
            fig.add_trace(go.Heatmap(x=fx, y=POLAR, z=z, zmin=-15, zmax=10, colorscale="Greys_r", showscale=False), row=r + 1, col=j + 1)
    fig.update_xaxes(type="log")
    fig.update_layout(title="Four heads, before and after alignment to the template", height=700, template="plotly_white")
    fig.write_html(os.path.join(out, "align-heads.html"), include_plotlyjs="cdn")

    fig = make_subplots(rows=1, cols=2, subplot_titles=["the constant shift a0 over all heads — 'ear size'", "each head's shift against polar angle"])
    fig.add_trace(go.Histogram(x=shifts[:, 0], nbinsx=30, marker_color="#1b1a17", name="a0"), row=1, col=1)
    for s in shifts[::4]:
        fig.add_trace(go.Scatter(x=POLAR, y=b @ s, mode="lines", line=dict(color="rgba(27,26,23,0.25)", width=1), showlegend=False), row=1, col=2)
    fig.update_xaxes(title_text="octaves", row=1, col=1); fig.update_xaxes(title_text="polar angle °", row=1, col=2)
    fig.update_yaxes(title_text="shift, octaves", row=1, col=2)
    fig.update_layout(height=440, template="plotly_white", showlegend=False)
    fig.write_html(os.path.join(out, "align-shifts.html"), include_plotlyjs="cdn")


if __name__ == "__main__":
    main(sys.argv[1])
