"""Does an average head fill a grid's gaps better than smoothness alone?

grid-experiment.py filled the KU100's gaps from one other head, and lost to plain smoothing: a single head is the
wrong head. Here the prior is a population average. HUTUBS (96 measured heads, the whole sphere, 440 directions) is
the population: each of N_TRUTH heads in turn is the truth, the average of the other 95 the prior. A database's
coverage is simulated by keeping only the truth's directions inside its elevation range; the error is measured on
the directions removed (the gaps) and on a tenth of the kept ones held back from the fit (inside the coverage).

Compared, at order 20 with the roughness penalty λ·n(n+1) swept:
  smooth          no prior
  prior: average  the deviation from the average of the other heads
  prior: KU100    the deviation from the KU100 — a different head, for contrast

  python prior-experiment.py <KU100 sofa> <directory of HUTUBS pp*_HRIRs_measured.sofa>
"""
import glob, os, sys
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np
from scipy.signal import resample_poly
import plotly.graph_objects as go
from plotly.subplots import make_subplots

_s = spec_from_file_location("ge", os.path.join(os.path.dirname(os.path.abspath(__file__)), "grid-experiment.py"))
ge = module_from_spec(_s); _s.loader.exec_module(ge)

ORDER, N_TRUTH = 20, 20
COVERAGE = {"ARI −30…80°": (-30, 80), "3D3A −57…75°": (-57, 75), "SONICOM, LISTEN −45…90°": (-45, 90)}
LAMBDAS = np.logspace(-6, 0, 13)
nb = ge.NFFT // 2 + 1
CLL, ILD = np.r_[ge.BAND, np.zeros(nb, bool)], np.r_[np.zeros(nb, bool), ge.BAND]


def load_48k(path):
    az, el, ir, fs = ge.load(path)
    if fs != ge.FS:
        ir = resample_poly(ir, 160, 147, axis=-1) if round(fs) == 44100 else resample_poly(ir, int(ge.FS), int(fs), axis=-1)
    return az, el, ir


def rms(err, w, cols):
    return float(np.sqrt(np.sum(w[:, None] * err[:, cols] ** 2) / (w.sum() * cols.sum())))


def main(ku_path, hutubs_dir):
    paths = sorted(glob.glob(os.path.join(hutubs_dir, "pp*_HRIRs_measured.sofa")), key=lambda p: int(os.path.basename(p)[2:].split("_")[0]))
    heads = []
    for p in paths:
        az, el, ir = load_48k(p)
        heads.append(ge.cll_ild(ir))
    heads = np.stack(heads)                                              # heads x 440 x (2·bins)
    w_all = ge.areas(az, el)
    y_all = ge.sh_orth(ORDER, az, el)
    e_deg = np.degrees(el)
    print(f"HUTUBS: {len(heads)} heads, {len(az)} directions, elevation {e_deg.min():.0f}…{e_deg.max():.0f}°")

    kaz, kel, kir = load_48k(ku_path)
    ku_c = ge.solve(ge.sh_orth(ORDER, kaz, kel), ge.areas(kaz, kel), ge.cll_ild(kir), 1e-5, ORDER)
    ku_at = y_all @ ku_c

    rng = np.random.default_rng(0)
    truths = rng.choice(len(heads), N_TRUTH, replace=False)
    holdout = rng.random(len(az)) < 0.1
    # results[coverage][method][λ] = list over heads of (region → (CLL, ILD))
    results = {c: {m: {lam: [] for lam in LAMBDAS} for m in ("smooth", "prior: average", "prior: KU100")} for c in COVERAGE}
    spread = []
    for t in truths:
        truth = heads[t]
        avg = np.delete(heads, t, axis=0).mean(0)                         # the other heads, direction by direction
        spread.append((rms(avg - truth, w_all, CLL), rms(avg - truth, w_all, ILD), rms(ku_at - truth, w_all, CLL), rms(ku_at - truth, w_all, ILD)))
        for cname, (lo, hi) in COVERAGE.items():
            inside = (e_deg >= lo - 0.5) & (e_deg <= hi + 0.5)
            fit = inside & ~holdout
            regions = {"inside, held back": inside & holdout, "above": e_deg > hi + 0.5, "below": e_deg < lo - 0.5}
            y, w = y_all[fit], w_all[fit]
            for lam in LAMBDAS:
                for m, prior in (("smooth", None), ("prior: average", avg), ("prior: KU100", ku_at)):
                    if prior is None:
                        rec = y_all @ ge.solve(y, w, truth[fit], lam, ORDER)
                    else:
                        rec = prior + y_all @ ge.solve(y, w, truth[fit] - prior[fit], lam, ORDER)
                    err = rec - truth
                    results[cname][m][lam].append({r: (rms(err[mk], w_all[mk], CLL), rms(err[mk], w_all[mk], ILD))
                                                    for r, mk in regions.items() if mk.any()})
    s = np.array(spread).mean(0)
    print(f"over the whole sphere, against a held-out head: the average of the others is {s[0]:.2f} dB off in CLL, "
          f"{s[1]:.2f} in ILD; the KU100 {s[2]:.2f} and {s[3]:.2f}")

    for cname in COVERAGE:
        print(f"\n{cname} — mean over {N_TRUTH} heads, at each method's best λ for the gaps (CLL / ILD dB, 2–16 kHz)")
        for m in results[cname]:
            def score(lam):
                rows = results[cname][m][lam]
                return np.mean([sum(v[0] for k, v in r.items() if k != "inside, held back") for r in rows])
            lam = min(LAMBDAS, key=score)
            rows = results[cname][m][lam]
            regs = rows[0].keys()
            print(f"  {m:15s} λ={lam:.0e}  " + "  ".join(
                f"{r} {np.mean([x[r][0] for x in rows]):.2f} / {np.mean([x[r][1] for x in rows]):.2f}" for r in regs))
    plot(results)


def plot(results):
    names = list(COVERAGE)
    colours = {"smooth": "#6f6b62", "prior: average": "#d4873a", "prior: KU100": "#5e8bae"}
    dashes = {"inside, held back": "dot", "above": "dash", "below": "solid"}
    for q, qi in (("CLL", 0), ("ILD", 1)):
        fig = make_subplots(rows=1, cols=len(names), subplot_titles=names, shared_yaxes=True)
        for i, c in enumerate(names):
            for m in results[c]:
                regs = results[c][m][LAMBDAS[0]][0].keys()
                for r in regs:
                    mean = [np.mean([x[r][qi] for x in results[c][m][lam]]) for lam in LAMBDAS]
                    sd = [np.std([x[r][qi] for x in results[c][m][lam]]) for lam in LAMBDAS]
                    fig.add_trace(go.Scatter(x=LAMBDAS, y=mean, error_y=dict(type="data", array=sd, thickness=1, width=2),
                                             mode="lines+markers", line=dict(color=colours[m], dash=dashes[r]), marker=dict(size=4),
                                             name=f"{m} · {r}", legendgroup=f"{m}{r}", showlegend=i == 0), row=1, col=i + 1)
        fig.update_xaxes(type="log", title_text="λ")
        fig.update_yaxes(title_text=f"{q} RMS error dB (2–16 kHz), mean ± sd over heads", row=1, col=1)
        fig.update_layout(title=f"{q}: filling a grid's gaps — grey smooth, orange average of other heads, blue KU100; "
                                f"dotted inside (held back), dashed above, solid below", height=560, template="plotly_white")
        fig.write_html(os.path.join(ge.OUT, f"prior-{q.lower()}.html"), include_plotlyjs="cdn")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
