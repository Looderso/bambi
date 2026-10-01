"""Make two databases comparable: equalise each head onto the KU100's CTF, unify the grids, measure what is left.

Each head, per direction: resampled to 48 kHz, a 256-sample window from the earlier ear's onset (less 0.5 ms), then
the left and right magnitude in dB on a 256-point FFT.

Equalisation, as the thesis did it: a head's CTF per ear is its magnitude in dB averaged over directions — weighted
by the area each direction covers, since grids are not uniform — and its DTF the rest. The equalised CLL is the CLL
of the two DTFs plus the KU100's CTF averaged over both ears. The ILD is taken from the DTFs too, so a fixed left/right
difference of the measurement chain leaves it; how large that was is printed.

Grids are unified as prior-experiment.py found best: each head's deviation from its database's average, fitted at
order 20 with the roughness penalty; HUTUBS covers the whole sphere and is fitted first, and a database that leaves
gaps (3D3A below −57° and above 75°, SONICOM below −45°) has its average filled there from the HUTUBS average. All
are evaluated on one common set of directions, where every database measured.

Measured, before and after equalisation, over 2–16 kHz on those directions:
  - how far the databases' averages sit apart, against how far heads sit from their own database's average
  - how well a classifier tells which database a head came from (5-fold, balanced accuracy)

  python equalise.py <KU100 sofa> <HUTUBS directory> <3D3A directory> [<SONICOM directory>]
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


PATTERNS = {"HUTUBS": "pp*_HRIRs_measured.sofa", "3D3A": "Subject*_HRIRs.sofa", "SONICOM": "p[0-9]*.sofa"}


def common_directions(dbs, k=2000):
    """A Fibonacci set, restricted to the elevations every database measured."""
    lo = max(np.degrees(el).min() for _, el, _ in dbs.values())
    hi = min(np.degrees(el).max() for _, el, _ in dbs.values())
    i = np.arange(k) + 0.5
    caz, cel = np.pi * (3 - np.sqrt(5)) * i, np.arcsin(1 - 2 * i / k)
    keep = (np.degrees(cel) >= lo) & (np.degrees(cel) <= hi)
    return caz[keep], cel[keep], (lo, hi)


def load_all(ku_path, dirs):
    """dirs: database name → directory; HUTUBS first, as it covers the whole sphere."""
    kaz, kel, kir, kfs = ge.load(ku_path)
    ku_ctf = np.einsum("d,deb->eb", ge.areas(kaz, kel), ears_db(windowed(kir, kfs)))
    return ku_ctf, {n: load_db(d, PATTERNS[n]) for n, d in dirs.items()}


def unified(dbs, ku_ctf, stage, caz, cel, report=False, max_order=ORDER):
    """Every head's CLL and ILD on the common directions: heads x directions x (2·bins), per database. max_order
    keeps only the SH orders up to it — the resolution every database's grid can carry alike."""
    coefs, base_c = {}, None
    for n, (az, el, h) in dbs.items():
        w = ge.areas(az, el)
        if stage == "equalised":
            h, ctf = equalise(h, w, ku_ctf)
            if report:
                lr = ctf[:, 0, :] - ctf[:, 1, :]
                print(f"{n}: the left/right CTF difference removed from the ILD — mean {np.mean(np.abs(lr[:, BAND])):.2f} dB, "
                      f"largest {np.max(np.abs(lr[:, BAND])):.2f} dB over 2–16 kHz")
        q = np.stack([cll_ild_of(x) for x in h])                          # heads x dirs x (2·bins)
        y = ge.sh_orth(ORDER, az, el)
        avg = q.mean(0)
        if base_c is None:
            avg_c = base_c = ge.solve(y, w, avg, LAM_FULL, ORDER)
            lam = LAM_FULL
        else:
            avg_c = base_c + ge.solve(y, w, avg - y @ base_c, LAM_GAPS, ORDER)
            lam = LAM_GAPS
        prior = y @ avg_c
        coefs[n] = np.stack([avg_c + ge.solve(y, w, x - prior, lam, ORDER) for x in q])
    keep = (max_order + 1) ** 2
    y_common = ge.sh_orth(ORDER, caz, cel)[:, :keep]
    return {n: np.einsum("dc,hcf->hdf", y_common, c[:, :keep]) for n, c in coefs.items()}


COLS_CLL = np.r_[BAND, np.zeros(nb, bool)]
COLS_ILD = np.r_[np.zeros(nb, bool), BAND]


def database_accuracy(ev):
    """Balanced accuracy of telling the database from a head's CLL and ILD (2–16 kHz): standardised, 20 principal
    components, logistic regression, 5-fold cross-validation. Chance is 1 / the number of databases."""
    X = np.concatenate([v[:, :, COLS_CLL | COLS_ILD].reshape(len(v), -1) for v in ev.values()])
    yl = np.concatenate([np.full(len(v), i) for i, v in enumerate(ev.values())])
    clf = make_pipeline(StandardScaler(), PCA(n_components=20), LogisticRegression(max_iter=2000, class_weight="balanced"))
    return balanced_accuracy_score(yl, cross_val_predict(clf, X, yl, cv=StratifiedKFold(5, shuffle=True, random_state=0)))


def rms_db(d, cols):
    return float(np.sqrt(np.mean(d[:, cols] ** 2)))


def main(ku_path, *dirs):
    ku_ctf, dbs = load_all(ku_path, dict(zip(("HUTUBS", "3D3A", "SONICOM"), dirs)))
    for n, (az, el, h) in dbs.items():
        print(f"{n}: {h.shape[0]} heads, {h.shape[1]} directions, elevation {np.degrees(el).min():.0f}…{np.degrees(el).max():.0f}°")
    caz, cel, (lo, hi) = common_directions(dbs)
    print(f"compared on {len(caz)} directions, elevation {lo:.0f}…{hi:.0f}°")
    names = list(dbs)
    pairs = [(a, b) for i, a in enumerate(names) for b in names[i + 1:]]
    out = {}
    for stage in ("raw", "equalised"):
        ev = unified(dbs, ku_ctf, stage, caz, cel, report=True)
        means = {n: v.mean(0) for n, v in ev.items()}
        print(f"\n{stage}:")
        for a, b in pairs:
            d = means[a] - means[b]
            print(f"  {a} and {b} averages apart: {rms_db(d, COLS_CLL):.2f} dB CLL, {rms_db(d, COLS_ILD):.2f} dB ILD")
        for n, v in ev.items():
            w_ = [np.mean(np.sqrt(np.mean((v - means[n])[:, :, c] ** 2, axis=(1, 2)))) for c in (COLS_CLL, COLS_ILD)]
            print(f"  a {n} head from its database's average: {w_[0]:.2f} dB CLL, {w_[1]:.2f} dB ILD")
        print(f"  a classifier names the database at {100 * database_accuracy(ev):.0f} % (chance {100 / len(names):.0f} %)")
        out[stage] = {f"{a} − {b}": means[a] - means[b] for a, b in pairs}, means
    plot(out, caz, cel)


def plot(out, az, el):
    fr = np.fft.rfftfreq(NFFT, 1 / FS)
    k = (fr >= 200) & (fr <= 20000)
    fig = make_subplots(rows=1, cols=2, subplot_titles=["CLL: differences between database averages", "ILD"])
    palette = ["#1b1a17", "#d4873a", "#5e8bae"]
    for stage, dash in (("raw", "dot"), ("equalised", "solid")):
        for c, (pair, d) in enumerate(out[stage][0].items()):
            for qi in range(2):
                dd = d[:, qi * nb:(qi + 1) * nb]
                fig.add_trace(go.Scatter(x=fr[k], y=np.sqrt(np.mean(dd ** 2, 0))[k], name=f"{pair}, {stage}",
                                         line=dict(color=palette[c % 3], dash=dash), showlegend=qi == 0,
                                         legendgroup=pair + stage), row=1, col=qi + 1)
    fig.update_xaxes(type="log", title_text="Hz"); fig.update_yaxes(title_text="dB, RMS over the common directions")
    fig.update_layout(title="How far the databases' averages sit apart — dotted raw, solid equalised", height=480, template="plotly_white")
    fig.write_html(os.path.join(ge.OUT, "equalise-difference.html"), include_plotlyjs="cdn")

    means = out["equalised"][1]
    front = np.abs(((np.degrees(az) + 180) % 360) - 180) < 8
    back = np.abs(np.abs(((np.degrees(az) + 180) % 360) - 180) - 180) < 8
    ang = np.r_[np.degrees(el[front]), 180 - np.degrees(el[back])]
    o = np.argsort(ang)
    names = list(means)
    fig = make_subplots(rows=1, cols=len(names), subplot_titles=[f"{n} average" for n in names], shared_yaxes=True)
    for i, n in enumerate(names):
        zz = np.r_[means[n][front], means[n][back]][o][:, :nb]
        fig.add_trace(go.Heatmap(x=fr, y=ang[o], z=zz, zmin=-25, zmax=15, colorscale="Greys_r", showscale=i == len(names) - 1,
                                 colorbar=dict(title="dB")), row=1, col=i + 1)
    fig.update_xaxes(type="log", range=[np.log10(1500), np.log10(20000)], title_text="Hz")
    fig.update_yaxes(title_text="polar angle ° (0 front, 90 up, 180 back)", row=1, col=1)
    fig.update_layout(title="Median-plane CLL after equalisation onto the KU100's CTF", height=520, template="plotly_white")
    fig.write_html(os.path.join(ge.OUT, "equalise-median.html"), include_plotlyjs="cdn")


if __name__ == "__main__":
    main(*sys.argv[1:])
