"""How to unify HRTF measurement grids with spherical harmonics without the gaps exploding.

The truth is the KU100 (SADIE II D1, 8802 directions over the whole sphere). It is sampled at each database's real
grid — the nearest measured direction to every grid point — fitted with each method, and compared with the full
KU100 everywhere: where the grid has points, and in its gaps (above its highest, below its lowest elevation).

Quantities are those the decoder renders from: CLL = 10·log10(P_L + P_R) and ILD = 10·log10(P_L / P_R), in dB, per
bin of a 256-point FFT at 48 kHz. Errors are RMS in dB over 2–16 kHz, weighted by the area each direction covers.

Methods, all on orthonormal real SH:
  ls-N       least squares at order N, weighted by the grid's own Voronoi areas — no regularisation
  smooth     order 20, Tikhonov on roughness: λ·n(n+1) per coefficient — gaps are bridged smoothly
  prior      order 20, the same Tikhonov on the deviation from a prior head; a gap falls back to the prior.
             The prior is SADIE II human H3 (full sphere), not the KU100 — a prior equal to the truth would make
             the test trivially perfect.

  python grid-experiment.py <KU100 sofa> <prior sofa> <grid sofa>...   → out/*.html (plotly) and a table
"""
import os, sys
from importlib.util import spec_from_file_location, module_from_spec
import h5py
import numpy as np
from scipy.spatial import SphericalVoronoi, cKDTree
import plotly.graph_objects as go
from plotly.subplots import make_subplots

_s = spec_from_file_location("bf", os.path.join(os.path.dirname(__file__), "../listen/build-filters.py"))
bf = module_from_spec(_s); _s.loader.exec_module(bf)

FS, NFFT, ORDER = 48000, 256, 20
FLOOR = 10 ** (-60 / 10)
ILD_LIMIT = 40.0
FREQ = np.fft.rfftfreq(NFFT, 1 / FS)
BAND = (FREQ >= 2000) & (FREQ <= 16000)
LAMBDAS = np.logspace(-7, 0, 15)
LS_ORDERS = (4, 6, 8, 10, 12, 15, 20)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")


def xyz(az, el):
    return np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], -1)


def load(path):
    f = h5py.File(path)
    pos = f["SourcePosition"][:]
    return np.radians(pos[:, 0]), np.radians(pos[:, 1]), f["Data.IR"][:], float(f["Data.SamplingRate"][:][0])


def cll_ild(ir):
    p = np.abs(np.fft.rfft(ir, NFFT, axis=-1)) ** 2
    cll = 10 * np.log10(np.maximum(p[:, 0] + p[:, 1], FLOOR))
    ild = np.clip(10 * np.log10(np.maximum(p[:, 0], FLOOR) / np.maximum(p[:, 1], FLOOR)), -ILD_LIMIT, ILD_LIMIT)
    return np.concatenate([cll, ild], 1)                                  # dirs x (2·bins): CLL bins, then ILD bins


def sh_orth(order, az, el):
    """Real SH orthonormal on the sphere (∫ Y_i Y_j dΩ = δ_ij): SN3D · sqrt((2n+1)/4π)."""
    y = bf.sh_sn3d(order, az, el)
    n = np.array([n for n in range(order + 1) for _ in range(2 * n + 1)])
    return y * np.sqrt((2 * n + 1) / (4 * np.pi))


def areas(az, el):
    pts = xyz(az, el)
    _, keep = np.unique(np.round(pts, 9), axis=0, return_index=True)       # SphericalVoronoi needs unique points
    w = np.zeros(len(pts))
    w[keep] = SphericalVoronoi(pts[keep]).calculate_areas()
    return w / w.sum()


def solve(y, w, d, lam, order):
    """Weighted least squares with a roughness penalty lam·n(n+1); lam = 0 is plain (min-norm) least squares."""
    n = np.array([n for n in range(order + 1) for _ in range(2 * n + 1)])
    a = (y * w[:, None]).T @ y
    scale = np.trace(a) / a.shape[0]
    if lam == 0:
        return np.linalg.lstsq(y * np.sqrt(w)[:, None], d * np.sqrt(w)[:, None], rcond=1e-10)[0]
    return np.linalg.solve(a + lam * scale * np.diag(n * (n + 1.0)), (y * w[:, None]).T @ d)


def rms(err, w, mask):
    if not mask.any():
        return np.nan
    e = err[mask][:, :]
    return float(np.sqrt(np.sum(w[mask, None] * e ** 2) / (w[mask].sum() * e.shape[1])))


def main(ku_path, prior_path, grid_paths):
    os.makedirs(OUT, exist_ok=True)
    az, el, ir, fs = load(ku_path)
    truth = cll_ild(ir)                                                  # the KU100 at its 8802 directions
    w_eval = areas(az, el)
    y_eval = sh_orth(ORDER, az, el)
    pts = xyz(az, el)
    tree = cKDTree(pts)
    nb = NFFT // 2 + 1
    cols = np.concatenate([BAND, BAND])
    cll_cols, ild_cols = np.r_[BAND, np.zeros(nb, bool)], np.r_[np.zeros(nb, bool), BAND]

    paz, pel, pir, _ = load(prior_path)
    prior_d = cll_ild(pir)
    prior_c = solve(sh_orth(ORDER, paz, pel), areas(paz, pel), prior_d, 1e-5, ORDER)
    prior_eval = y_eval @ prior_c
    print(f"prior H3 against the KU100 (2–16 kHz): CLL {rms((prior_eval - truth)[:, cll_cols], w_eval, np.ones(len(az), bool)):.2f} dB, "
          f"ILD {rms((prior_eval - truth)[:, ild_cols], w_eval, np.ones(len(az), bool)):.2f} dB")

    results, recon_keep, grids = {}, {}, {}
    for path in grid_paths:
        name = os.path.basename(path).split(".")[0]
        gaz, gel, gir, gfs = load(path)
        gpts = xyz(gaz, gel)
        dist, idx = tree.query(gpts)
        data = truth[idx]                                                # the KU100 at this grid
        w = areas(gaz, gel)
        lo, hi = np.degrees(gel.min()), np.degrees(gel.max())
        e_deg = np.degrees(el)
        regions = {"covered": (e_deg >= lo - 1) & (e_deg <= hi + 1), "above": e_deg > hi + 1, "below": e_deg < lo - 1}
        grids[name] = (gaz, gel, lo, hi, len(gaz), np.degrees(dist).max())
        res = {}
        for order in LS_ORDERS:
            if (order + 1) ** 2 > 3 * len(gaz):
                continue
            c = solve(sh_orth(order, gaz, gel), w, data, 0, order)
            res[f"ls-{order}"] = sh_orth(order, az, el) @ c
        y = sh_orth(ORDER, gaz, gel)
        prior_at = y @ prior_c
        for lam in LAMBDAS:
            res[f"smooth λ={lam:.0e}"] = y_eval @ solve(y, w, data, lam, ORDER)
            res[f"prior λ={lam:.0e}"] = prior_eval + y_eval @ solve(y, w, data - prior_at, lam, ORDER)
        table = {}
        for k, r in res.items():
            err = r - truth
            table[k] = {reg: (rms(err[:, cll_cols], w_eval, m), rms(err[:, ild_cols], w_eval, m)) for reg, m in regions.items()}
        results[name] = (table, regions)
        recon_keep[name] = res
        print(f"\n{name}: {len(gaz)} points, elevation {lo:.0f}…{hi:.0f}°, KU100 within {np.degrees(dist).max():.1f}° of each")
        best = lambda pre: min((k for k in table if k.startswith(pre)), key=lambda k: np.nansum([table[k][r][0] for r in regions]))
        top_ls = max((k for k in table if k.startswith("ls")), key=lambda k: int(k.split("-")[1]))
        for k in sorted({best("ls"), top_ls, best("smooth"), best("prior")}):
            print(f"  {k:18s} " + "  ".join(f"{reg} CLL {table[k][reg][0]:5.2f} ILD {table[k][reg][1]:5.2f}" for reg in regions))

    plots(az, el, truth, prior_eval, grids, results, recon_keep)


def plots(az, el, truth, prior_eval, grids, results, recon):
    names = list(grids)
    # 1. The grids.
    fig = make_subplots(rows=2, cols=3, subplot_titles=[f"{n} — {grids[n][4]} points, {grids[n][2]:.0f}…{grids[n][3]:.0f}°" for n in names])
    for i, n in enumerate(names):
        gaz, gel = grids[n][:2]
        fig.add_trace(go.Scattergl(x=((np.degrees(gaz) + 180) % 360) - 180, y=np.degrees(gel), mode="markers",
                                   marker=dict(size=3, color="#1b1a17"), name=n, showlegend=False), row=i // 3 + 1, col=i % 3 + 1)
    fig.update_xaxes(range=[180, -180], title_text="azimuth °"); fig.update_yaxes(range=[-90, 90], title_text="elevation °")
    fig.update_layout(title="Measurement grids", height=700, template="plotly_white")
    fig.write_html(os.path.join(OUT, "grids.html"), include_plotlyjs="cdn")

    # 2. Error against λ, per grid and region, CLL and ILD.
    for q, qi in (("CLL", 0), ("ILD", 1)):
        fig = make_subplots(rows=2, cols=3, subplot_titles=names, shared_yaxes=True)
        colours = {"covered": "#1b1a17", "above": "#d4873a", "below": "#5e8bae"}
        for i, n in enumerate(names):
            table, regions = results[n]
            for reg in regions:
                if not regions[reg].any():
                    continue
                for method, dash in (("smooth", "dot"), ("prior", "solid")):
                    ks = [k for k in table if k.startswith(method)]
                    fig.add_trace(go.Scatter(x=LAMBDAS, y=[table[k][reg][qi] for k in ks], mode="lines+markers",
                                             line=dict(color=colours[reg], dash=dash), marker=dict(size=4),
                                             name=f"{method} · {reg}", legendgroup=f"{method}{reg}", showlegend=i == 0),
                                  row=i // 3 + 1, col=i % 3 + 1)
        fig.update_xaxes(type="log", title_text="λ"); fig.update_yaxes(type="log", title_text=f"{q} RMS error dB (2–16 kHz)")
        fig.update_layout(title=f"{q}: error against regularisation — dotted smooth, solid prior; ink covered, orange above, blue below",
                          height=760, template="plotly_white")
        fig.write_html(os.path.join(OUT, f"lambda-{q.lower()}.html"), include_plotlyjs="cdn")

    # 3. The median plane, front and back, elevation against frequency: truth and each method's best, per grid.
    med = (np.abs(np.degrees(az)) < 1) | (np.abs(np.abs(np.degrees(az)) - 180) < 1)
    front = np.abs(np.degrees(az)) < 1
    ang = np.where(front, np.degrees(el), 180 - np.degrees(el))          # polar angle: −90 below, 90 up, 270 below behind
    order_ = np.argsort(ang[med])
    nb = NFFT // 2 + 1
    for n in names:
        table, regions = results[n]
        pick = lambda pre: min((k for k in table if k.startswith(pre)), key=lambda k: np.nansum([table[k][r][0] for r in regions]))
        rows = [("KU100 (truth)", truth), ("prior H3 alone", prior_eval)] + [(pick(p), recon[n][pick(p)]) for p in ("ls", "smooth", "prior")]
        fig = make_subplots(rows=1, cols=len(rows), subplot_titles=[r[0] for r in rows], shared_yaxes=True)
        for i, (lab, r) in enumerate(rows):
            z = r[med][order_][:, :nb]
            fig.add_trace(go.Heatmap(x=FREQ, y=ang[med][order_], z=z, zmin=-25, zmax=15, colorscale="Greys_r",
                                     showscale=i == len(rows) - 1, colorbar=dict(title="CLL dB")), row=1, col=i + 1)
        lo, hi = grids[n][2], grids[n][3]
        for i in range(len(rows)):
            for v in (hi, 180 - hi, lo, 180 - lo):
                fig.add_hline(y=v, line=dict(color="#d4873a", width=1, dash="dash"), row=1, col=i + 1)
        fig.update_xaxes(type="log", range=[np.log10(1500), np.log10(20000)], title_text="Hz")
        fig.update_yaxes(title_text="polar angle ° (0 front, 90 up, 180 back)", row=1, col=1)
        fig.update_layout(title=f"{n}: median-plane CLL — orange dashes mark the grid's highest and lowest elevation",
                          height=520, template="plotly_white")
        fig.write_html(os.path.join(OUT, f"median-{n}.html"), include_plotlyjs="cdn")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3:])
