"""Make two databases comparable: equalise each head onto the KU100's CTF, unify the grids, measure what is left.

Each head, per direction: resampled to 48 kHz, a 256-sample window from the earlier ear's onset (less 0.5 ms), then
the left and right magnitude in dB on a 256-point FFT.

Equalisation, as the thesis did it: a head's CTF per ear is its magnitude in dB averaged over directions — weighted
by the area each direction covers, since grids are not uniform — and its DTF the rest. The equalised CLL is the CLL
of the two DTFs plus the KU100's CTF averaged over both ears. The ILD is taken from the DTFs too, so a fixed left/right
difference of the measurement chain leaves it; how large that was is printed.

Grids are unified as prior-experiment.py found best: each head's deviation from its database's average (HUTUBS,
whole sphere) — or, where a database leaves gaps (3D3A, below −57° and above 75°), from the HUTUBS average — fitted
at order 20 with the roughness penalty, and evaluated on one common set of directions.

Measured, before and after equalisation, over 2–16 kHz on the directions both databases cover:
  - how far the two databases' averages sit apart, against how far heads sit from their own database's average
  - how well a classifier tells which database a head came from (5-fold, balanced accuracy)

  python equalise.py <KU100 sofa> <HUTUBS directory> <3D3A directory>
"""
import glob, os, sys
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np
from scipy.signal import resample_poly
from sklearn.decomposition import PCA
from sklearn.linear_model import LogisticRegression
from sklearn.model_selection import StratifiedKFold, cross_val_predict
from sklearn.metrics import balanced_accuracy_score
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler
import plotly.graph_objects as go
from plotly.subplots import make_subplots

_d = os.path.dirname(os.path.abspath(__file__))
_s = spec_from_file_location("ge", os.path.join(_d, "grid-experiment.py")); ge = module_from_spec(_s); _s.loader.exec_module(ge)

FS, NFFT, ORDER = 48000, 256, 20
LAM_FULL, LAM_GAPS = 1e-3, 3e-4
nb = NFFT // 2 + 1
BAND = ge.BAND


def windowed(ir, fs):
    """48 kHz, 256 samples from the earlier ear's onset (first sample above 10 % of the peak) less 0.5 ms."""
    if round(fs) != FS:
        up, down = {44100: (160, 147), 96000: (1, 2)}[round(fs)]
        ir = resample_poly(ir, up, down, axis=-1)
    out = np.zeros(ir.shape[:2] + (NFFT,))
    fade = 0.5 + 0.5 * np.cos(np.linspace(0, np.pi, 32))
    for i in range(ir.shape[0]):
        on = min((np.abs(ir[i, e]) > 0.1 * np.abs(ir[i, e]).max()).argmax() for e in range(2))
        s = max(0, on - 24)
        seg = ir[i, :, s:s + NFFT]
        out[i, :, :seg.shape[-1]] = seg
    out[..., -32:] *= fade
    return out


def ears_db(ir):
    return 10 * np.log10(np.maximum(np.abs(np.fft.rfft(ir, NFFT, axis=-1)) ** 2, ge.FLOOR))     # dirs x 2 x bins


def cll_ild_of(db):
    pl, pr = 10 ** (db[:, 0] / 10), 10 ** (db[:, 1] / 10)
    return np.concatenate([10 * np.log10(pl + pr), np.clip(db[:, 0] - db[:, 1], -ge.ILD_LIMIT, ge.ILD_LIMIT)], 1)


def load_db(directory, pattern):
    heads, az, el = [], None, None
    for p in sorted(glob.glob(os.path.join(directory, pattern))):
        a, e, ir, fs = ge.load(p)
        if az is None:
            az, el = a, e
        heads.append(ears_db(windowed(ir, fs)))
    return az, el, np.stack(heads)                                       # heads x dirs x 2 x bins


def equalise(heads, w, ref_ctf):
    """DTF per ear (minus the area-weighted directional mean) plus the reference CTF, averaged over both ears."""
    ctf = np.einsum("d,hdeb->heb", w, heads)                             # heads x ears x bins
    dtf = heads - ctf[:, None]
    return dtf + ref_ctf.mean(0)[None, None, None, :], ctf


def main(ku_path, hutubs_dir, d3a_dir):
    kaz, kel, kir, kfs = ge.load(ku_path)
    ku = ears_db(windowed(kir, kfs))
    ku_ctf = np.einsum("d,deb->eb", ge.areas(kaz, kel), ku)

    dbs = {"HUTUBS": load_db(hutubs_dir, "pp*_HRIRs_measured.sofa"), "3D3A": load_db(d3a_dir, "Subject*_HRIRs.sofa")}
    for n, (az, el, h) in dbs.items():
        print(f"{n}: {h.shape[0]} heads, {h.shape[1]} directions, elevation {np.degrees(el).min():.0f}…{np.degrees(el).max():.0f}°")

    # Common directions: a Fibonacci set, evaluated in both, restricted to where both measured.
    k = 2000
    i = np.arange(k) + 0.5
    z = 1 - 2 * i / k
    caz, cel = np.pi * (3 - np.sqrt(5)) * i, np.arcsin(z)
    common = (np.degrees(cel) >= -57) & (np.degrees(cel) <= 75)
    y_common = ge.sh_orth(ORDER, caz, cel)
    wc = np.full(k, 1 / k)

    out = {}
    for stage in ("raw", "equalised"):
        coefs, fit_err = {}, {}
        hut_avg_c = None
        for n in ("HUTUBS", "3D3A"):
            az, el, h = dbs[n]
            w = ge.areas(az, el)
            if stage == "equalised":
                h, ctf = equalise(h, w, ku_ctf)
                lr = ctf[:, 0, :] - ctf[:, 1, :]
                print(f"{n}: the left/right CTF difference removed from the ILD — mean {np.mean(np.abs(lr[:, BAND])):.2f} dB, "
                      f"largest {np.max(np.abs(lr[:, BAND])):.2f} dB over 2–16 kHz")
            q = np.stack([cll_ild_of(x) for x in h])                      # heads x dirs x (2·bins)
            y = ge.sh_orth(ORDER, az, el)
            avg = q.mean(0)
            if n == "HUTUBS":
                avg_c = ge.solve(y, w, avg, LAM_FULL, ORDER)
                hut_avg_c = avg_c
                lam = LAM_FULL
            else:
                avg_c = hut_avg_c + ge.solve(y, w, avg - y @ hut_avg_c, LAM_GAPS, ORDER)
                lam = LAM_GAPS
            prior = y @ avg_c
            coefs[n] = np.stack([avg_c + ge.solve(y, w, x - prior, lam, ORDER) for x in q])
        cols_cll = np.r_[BAND, np.zeros(nb, bool)]
        cols_ild = np.r_[np.zeros(nb, bool), BAND]
        ev = {n: np.einsum("dc,hcf->hdf", y_common[common], c) for n, c in coefs.items()}   # heads x common dirs x cols
        means = {n: v.mean(0) for n, v in ev.items()}
        d = means["HUTUBS"] - means["3D3A"]
        between = [np.sqrt(np.mean(d[:, cols] ** 2)) for cols in (cols_cll, cols_ild)]
        within = {n: [np.mean(np.sqrt(np.mean((v - means[n])[:, :, cols] ** 2, axis=(1, 2)))) for cols in (cols_cll, cols_ild)]
                  for n, v in ev.items()}
        # Classifier: which database? Features: CLL and ILD at the common directions, 2–16 kHz.
        X = np.concatenate([ev[n][:, :, np.r_[cols_cll | cols_ild]].reshape(len(ev[n]), -1) for n in ev])
        yl = np.r_[np.zeros(len(ev["HUTUBS"])), np.ones(len(ev["3D3A"]))]
        clf = make_pipeline(StandardScaler(), PCA(n_components=20), LogisticRegression(max_iter=2000, class_weight="balanced"))
        pred = cross_val_predict(clf, X, yl, cv=StratifiedKFold(5, shuffle=True, random_state=0))
        acc = balanced_accuracy_score(yl, pred)
        print(f"\n{stage}: the databases' averages differ by {between[0]:.2f} dB (CLL), {between[1]:.2f} dB (ILD); "
              f"a head differs from its own database's average by {within['HUTUBS'][0]:.2f} / {within['HUTUBS'][1]:.2f} (HUTUBS), "
              f"{within['3D3A'][0]:.2f} / {within['3D3A'][1]:.2f} (3D3A); a classifier tells the database apart at "
              f"{100 * acc:.0f} % balanced accuracy (chance 50 %)")
        out[stage] = (means, d)
    plot(out, caz[common], cel[common])


def plot(out, az, el):
    fr = np.fft.rfftfreq(NFFT, 1 / FS)
    k = (fr >= 200) & (fr <= 20000)
    fig = make_subplots(rows=1, cols=2, subplot_titles=["CLL: HUTUBS average − 3D3A average", "ILD: HUTUBS average − 3D3A average"])
    for stage, col in (("raw", "#a9a59c"), ("equalised", "#1b1a17")):
        _, d = out[stage]
        for qi in range(2):
            dd = d[:, qi * nb:(qi + 1) * nb]
            fig.add_trace(go.Scatter(x=fr[k], y=np.sqrt(np.mean(dd ** 2, 0))[k], name=f"{stage}, RMS over directions",
                                     line=dict(color=col), showlegend=qi == 0, legendgroup=stage), row=1, col=qi + 1)
            fig.add_trace(go.Scatter(x=fr[k], y=np.mean(dd, 0)[k], name=f"{stage}, mean over directions",
                                     line=dict(color=col, dash="dot"), showlegend=qi == 0, legendgroup=stage + "m"), row=1, col=qi + 1)
    fig.update_xaxes(type="log", title_text="Hz"); fig.update_yaxes(title_text="dB")
    fig.update_layout(title="How far the two databases' averages sit apart, on the directions both measured (−57…75°)",
                      height=480, template="plotly_white")
    fig.write_html(os.path.join(ge.OUT, "equalise-difference.html"), include_plotlyjs="cdn")

    # The averages themselves in the median plane — front and back — after equalisation.
    means, _ = out["equalised"]
    front = np.abs(((np.degrees(az) + 180) % 360) - 180) < 8
    back = np.abs(np.abs(((np.degrees(az) + 180) % 360) - 180) - 180) < 8
    ang = np.r_[np.degrees(el[front]), 180 - np.degrees(el[back])]
    o = np.argsort(ang)
    fig = make_subplots(rows=1, cols=3, subplot_titles=["HUTUBS average", "3D3A average", "difference"], shared_yaxes=True)
    for i, z in enumerate([means["HUTUBS"], means["3D3A"], means["HUTUBS"] - means["3D3A"]]):
        zz = np.r_[z[front], z[back]][o][:, :nb]
        fig.add_trace(go.Heatmap(x=fr, y=ang[o], z=zz, zmin=-12 if i == 2 else -25, zmax=12 if i == 2 else 15,
                                 colorscale="RdBu" if i == 2 else "Greys_r", showscale=i != 0,
                                 colorbar=dict(x=1.0 if i == 2 else 0.64, title="dB")), row=1, col=i + 1)
    fig.update_xaxes(type="log", range=[np.log10(1500), np.log10(20000)], title_text="Hz")
    fig.update_yaxes(title_text="polar angle ° (0 front, 90 up, 180 back)", row=1, col=1)
    fig.update_layout(title="Median-plane CLL after equalisation onto the KU100's CTF", height=520, template="plotly_white")
    fig.write_html(os.path.join(ge.OUT, "equalise-median.html"), include_plotlyjs="cdn")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
