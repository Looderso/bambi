"""Remove the database fingerprint by normalising each direction: a head minus its database's average, plus the
average of the databases (each database weighted equally).

Measured on HUTUBS and 3D3A after equalisation (equalise.py), 2–16 kHz, on the directions both measured:
  - the classifier that names the database, before and after. Within the cross-validation, a test head is normalised
    with the average of its database's *training* heads only, so the test never sees its own answer. In use the
    database a head came from is known, so normalising by it is fair.
  - how widely heads spread: around their own database's average (untouched by a shift, measured to show it), and
    pooled around the common average (where the databases' offsets are what goes)
  - if the classifier stays high, where the fingerprint then lives: how differently the two databases spread, per
    frequency

  python normalise.py <KU100 sofa> <HUTUBS directory> <3D3A directory>
"""
import os, sys
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np
from sklearn.decomposition import PCA
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import balanced_accuracy_score
from sklearn.model_selection import StratifiedKFold
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler
from sklearn.svm import SVC
import plotly.graph_objects as go
from plotly.subplots import make_subplots

_d = os.path.dirname(os.path.abspath(__file__))
_s = spec_from_file_location("eq", os.path.join(_d, "equalise.py")); eq = module_from_spec(_s); _s.loader.exec_module(eq)
ge, nb = eq.ge, eq.nb
COLS = eq.COLS_CLL | eq.COLS_ILD


def normalised(ev, means):
    grand = np.mean(list(means.values()), axis=0)
    return {n: v - means[n] + grand for n, v in ev.items()}


def accuracy(ev, normalise, kernel=False):
    """Balanced accuracy of naming the database; when normalise, each fold's database averages come from its
    training heads only. kernel: an RBF support-vector machine in place of the linear classifier — the kind of
    classifier that found the database fingerprint in the literature."""
    names = list(ev)
    X = np.concatenate([ev[n] for n in names])                         # heads x dirs x cols
    yl = np.concatenate([np.full(len(ev[n]), i) for i, n in enumerate(names)])
    pred = np.zeros_like(yl)
    for tr, te in StratifiedKFold(5, shuffle=True, random_state=0).split(X[:, 0, 0], yl):
        Xtr, Xte = X[tr].copy(), X[te].copy()
        if normalise:
            means = {i: X[tr][yl[tr] == i].mean(0) for i in range(len(names))}
            grand = np.mean(list(means.values()), axis=0)
            for i in means:
                Xtr[yl[tr] == i] += grand - means[i]
                Xte[yl[te] == i] += grand - means[i]
        clf = make_pipeline(StandardScaler(), PCA(n_components=20),
                            SVC(kernel="rbf", class_weight="balanced") if kernel else LogisticRegression(max_iter=2000, class_weight="balanced"))
        clf.fit(Xtr[:, :, COLS].reshape(len(tr), -1), yl[tr])
        pred[te] = clf.predict(Xte[:, :, COLS].reshape(len(te), -1))
    return balanced_accuracy_score(yl, pred)


def spread(v, centre, cols):
    """RMS over directions and bins of each head's distance from centre, averaged over heads."""
    return float(np.mean(np.sqrt(np.mean((v - centre)[:, :, cols] ** 2, axis=(1, 2)))))


def main(ku_path, hutubs_dir, d3a_dir):
    ku_ctf, dbs = eq.load_all(ku_path, hutubs_dir, d3a_dir)
    caz, cel = eq.common_directions()
    ev = eq.unified(dbs, ku_ctf, "equalised", caz, cel)
    means = {n: v.mean(0) for n, v in ev.items()}
    nv = normalised(ev, means)
    nmeans = {n: v.mean(0) for n, v in nv.items()}

    for label, e, m in (("equalised", ev, means), ("normalised", nv, nmeans)):
        pooled = np.concatenate(list(e.values()))
        grand = np.mean(list(m.values()), axis=0)
        print(f"\n{label}:")
        for q, cols in (("CLL", eq.COLS_CLL), ("ILD", eq.COLS_ILD)):
            print(f"  {q}: around its own database's average — HUTUBS {spread(e['HUTUBS'], m['HUTUBS'], cols):.2f} dB, "
                  f"3D3A {spread(e['3D3A'], m['3D3A'], cols):.2f} dB; all heads around the common average "
                  f"{spread(pooled, grand, cols):.2f} dB")
    for kernel, name in ((False, "linear"), (True, "kernel SVM")):
        print(f"\n{name} classifier naming the database: equalised {100 * accuracy(ev, False, kernel):.0f} %, "
              f"normalised (in-fold) {100 * accuracy(ev, True, kernel):.0f} % — chance 50 %")

    # Where a remaining fingerprint would live: the ratio of the two databases' spread, per bin.
    fr = np.fft.rfftfreq(eq.NFFT, 1 / eq.FS)
    sd = {n: v.std(0) for n, v in ev.items()}                          # dirs x cols
    ratio = np.median(sd["3D3A"] / np.maximum(sd["HUTUBS"], 1e-6), axis=0)
    for q, off in (("CLL", 0), ("ILD", nb)):
        r = ratio[off:off + nb][eq.BAND]
        print(f"  {q}: 3D3A spreads {np.median(r):.2f}× as widely as HUTUBS (median over directions and 2–16 kHz; "
              f"{r.min():.2f}…{r.max():.2f} over frequency)")

    offset = means["HUTUBS"] - means["3D3A"]
    fig = make_subplots(rows=1, cols=2, subplot_titles=["CLL", "ILD"])
    k = (fr >= 200) & (fr <= 20000)
    for qi, off in enumerate((0, nb)):
        fig.add_trace(go.Scatter(x=fr[k], y=np.sqrt(np.mean(offset[:, off:off + nb] ** 2, 0))[k], name="the offset removed (RMS over directions)",
                                 line=dict(color="#1b1a17"), showlegend=qi == 0), row=1, col=qi + 1)
        fig.add_trace(go.Scatter(x=fr[k], y=ratio[off:off + nb][k], name="3D3A's spread ÷ HUTUBS's (median over directions)",
                                 line=dict(color="#d4873a"), yaxis="y2" if qi == 0 else "y4", showlegend=qi == 0), row=1, col=qi + 1)
    fig.update_xaxes(type="log", title_text="Hz"); fig.update_yaxes(title_text="dB / ratio")
    fig.update_layout(title="What the normalisation removes (ink), and how differently the two databases spread (orange)",
                      height=460, template="plotly_white")
    fig.write_html(os.path.join(ge.OUT, "normalise.html"), include_plotlyjs="cdn")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3])
