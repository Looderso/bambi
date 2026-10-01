"""Remove the database fingerprint by normalising each direction: a head minus its database's average, plus the
average of the databases (each database weighted equally).

Measured on the databases after equalisation (equalise.py), 2–16 kHz, on the directions all of them measured:
  - the classifier that names the database, before and after. Within the cross-validation, a test head is normalised
    with the average of its database's *training* heads only, so the test never sees its own answer. In use the
    database a head came from is known, so normalising by it is fair.
  - how widely heads spread: around their own database's average (untouched by a shift, measured to show it), and
    pooled around the common average (where the databases' offsets are what goes)
  - how differently the databases spread, per frequency — a fingerprint the average cannot hold
  - whether the databases' averages differ by measurement or by people: random groups of heads from within one
    database, of the real databases' sizes, show how far two averages differ by chance of who was measured. A gap
    between databases well beyond that is the measurement's.

  python normalise.py <KU100 sofa> <HUTUBS directory> <3D3A directory> [<SONICOM directory>]
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


def chance_gap(v, sizes, cols, draws=300, rng=np.random.default_rng(0)):
    """How far the averages of two random groups of heads from one database differ — the 95th percentile over
    draws: a gap this large comes from who was measured, not from how."""
    out = []
    for _ in range(draws):
        idx = rng.permutation(len(v))
        a, b = idx[:sizes[0]], idx[sizes[0]:sizes[0] + sizes[1]]
        out.append(np.sqrt(np.mean((v[a].mean(0) - v[b].mean(0))[:, cols] ** 2)))
    return float(np.percentile(out, 95))


def main(ku_path, *dirs):
    ku_ctf, dbs = eq.load_all(ku_path, dict(zip(("HUTUBS", "3D3A", "SONICOM"), dirs)))
    caz, cel, (lo, hi) = eq.common_directions(dbs)
    ev = eq.unified(dbs, ku_ctf, "equalised", caz, cel)
    names = list(ev)
    means = {n: v.mean(0) for n, v in ev.items()}
    nv = normalised(ev, means)
    nmeans = {n: v.mean(0) for n, v in nv.items()}
    print(f"{', '.join(f'{n} {len(v)}' for n, v in ev.items())} heads; {len(caz)} directions, elevation {lo:.0f}…{hi:.0f}°")

    for label, e, m in (("equalised", ev, means), ("normalised", nv, nmeans)):
        pooled = np.concatenate(list(e.values()))
        grand = np.mean(list(m.values()), axis=0)
        print(f"\n{label}:")
        for q, cols in (("CLL", eq.COLS_CLL), ("ILD", eq.COLS_ILD)):
            own = ", ".join(f"{n} {spread(e[n], m[n], cols):.2f}" for n in names)
            print(f"  {q}: around its own database's average — {own} dB; all heads around the common average "
                  f"{spread(pooled, grand, cols):.2f} dB")
    chance = 100 / len(names)
    for kernel, name in ((False, "linear"), (True, "kernel SVM")):
        print(f"\n{name} classifier naming the database: equalised {100 * accuracy(ev, False, kernel):.0f} %, "
              f"normalised (in-fold) {100 * accuracy(ev, True, kernel):.0f} % — chance {chance:.0f} %")

    print("\nmeasurement or people — the averages' gap against the 95th percentile of random groups' gap (dB):")
    big = max(names, key=lambda n: len(ev[n]))
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            row = []
            for q, cols in (("CLL", eq.COLS_CLL), ("ILD", eq.COLS_ILD)):
                gap = np.sqrt(np.mean((means[a] - means[b])[:, cols] ** 2))
                sizes = sorted((len(ev[a]), len(ev[b])))
                pool = big if len(ev[big]) >= sum(sizes) else None
                ref = chance_gap(ev[pool], sizes, cols) if pool else chance_gap(ev[big], (len(ev[big]) // 2, len(ev[big]) // 2), cols)
                row.append(f"{q} {gap:.2f} against {ref:.2f}")
            print(f"  {a} – {b}: " + ", ".join(row) + (f"  (random groups from {big})"))

    fr = np.fft.rfftfreq(eq.NFFT, 1 / eq.FS)
    sd = {n: v.std(0) for n, v in ev.items()}
    for n in names[1:]:
        ratio = np.median(sd[n] / np.maximum(sd[names[0]], 1e-6), axis=0)
        print(f"  {n} spreads {np.median(ratio[:nb][eq.BAND]):.2f}× (CLL) and {np.median(ratio[nb:][eq.BAND]):.2f}× (ILD) "
              f"as widely as {names[0]} (median over directions, 2–16 kHz)")


if __name__ == "__main__":
    main(*sys.argv[1:])
