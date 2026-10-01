"""The unified data set: every head of the databases, equalised, normalised and on one SH representation.

Per head, as equalise.py and normalise.py measured it:
  - CLL and ILD in dB per bin of a 256-point FFT at 48 kHz (129 bins), equalised onto the KU100's CTF, unified by the
    deviation-from-average fit at order 20, normalised by its database's average to the average of the databases
  - ITD, tL − tR in seconds, from the cross-correlation estimator the match renderer uses, unified and normalised
    the same way at order 10
The KU100 is stored beside them in the same representation, unnormalised — the head the model modifies — and the
averages the normalisation used, so it can be undone.

Orthonormal real SH (∫ Y_i Y_j dΩ = δ_ij), ACN order — sh_orth in grid-experiment.py.

  python build-dataset.py <out.npz> <KU100 sofa> <HUTUBS directory> <3D3A directory> [<SONICOM directory>]
"""
import os, sys
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np

_d = os.path.dirname(os.path.abspath(__file__))
def _load(name, path):
    s = spec_from_file_location(name, path); m = module_from_spec(s); s.loader.exec_module(m); return m
eq = _load("eq", os.path.join(_d, "equalise.py"))
nz = _load("nz", os.path.join(_d, "normalise.py"))
bm = _load("bm", os.path.join(_d, "..", "match", "build-match.py"))
ge = eq.ge
ITD_ORDER = 10
NAMES = ("HUTUBS", "3D3A", "SONICOM")


def itds(win):
    """tL − tR per direction (s), the match renderer's estimator."""
    return np.array([bm.itd_estimate(win[i, 0], win[i, 1]) for i in range(win.shape[0])])


def main(out, ku_path, *dirs):
    kaz, kel, kir, kfs = ge.load(ku_path)
    kwin = eq.windowed(kir, kfs)
    ku_db = eq.ears_db(kwin)
    kw = ge.areas(kaz, kel)
    ku_ctf = np.einsum("d,deb->eb", kw, ku_db)
    ku_q = eq.cll_ild_of(ku_db)
    ku_c = ge.solve(ge.sh_orth(eq.ORDER, kaz, kel), kw, ku_q, 1e-5, eq.ORDER)
    ku_itd_c = ge.solve(ge.sh_orth(ITD_ORDER, kaz, kel), kw, itds(kwin)[:, None], 1e-5, ITD_ORDER)[:, 0]

    dbs, itd_raw, files = {}, {}, {}
    for n, d in zip(NAMES, dirs):
        az, el, h, it, f = eq.load_db(d, eq.PATTERNS[n], per_head=itds)
        dbs[n], itd_raw[n], files[n] = (az, el, h), it, f
        print(f"{n}: {h.shape[0]} heads, {h.shape[1]} directions; ITD up to {1e6 * np.abs(it).max():.0f} µs")

    coefs = eq.unified(dbs, ku_ctf, "equalised", None, None, coefficients=True)
    # ITD: the same unification at order 10 — deviation from the database's average, HUTUBS's filling the gaps.
    itd_c, base = {}, None
    for n in dbs:
        az, el, _ = dbs[n]
        y, w = ge.sh_orth(ITD_ORDER, az, el), ge.areas(az, el)
        avg = itd_raw[n].mean(0)
        if base is None:
            avg_c = base = ge.solve(y, w, avg[:, None], eq.LAM_FULL, ITD_ORDER)[:, 0]
            lam = eq.LAM_FULL
        else:
            avg_c = base + ge.solve(y, w, (avg - y @ base)[:, None], eq.LAM_GAPS, ITD_ORDER)[:, 0]
            lam = eq.LAM_GAPS
        itd_c[n] = np.stack([avg_c + ge.solve(y, w, (x - y @ avg_c)[:, None], lam, ITD_ORDER)[:, 0] for x in itd_raw[n]])

    means = {n: c.mean(0) for n, c in coefs.items()}
    grand = np.mean(list(means.values()), axis=0)
    itd_means = {n: c.mean(0) for n, c in itd_c.items()}
    itd_grand = np.mean(list(itd_means.values()), axis=0)
    cll_ild = np.concatenate([coefs[n] - means[n] + grand for n in dbs]).astype(np.float32)
    itd = np.concatenate([itd_c[n] - itd_means[n] + itd_grand for n in dbs]).astype(np.float32)
    database = np.concatenate([np.full(len(coefs[n]), i) for i, n in enumerate(dbs)])

    # Checks on the stored set, where all databases measured. (A cross-validated classifier is the wrong check here:
    # normalised with averages that include every head, a held-out head is pushed away from its own database's
    # training average, and the classifier lands below chance. normalise.py runs it properly, in-fold.)
    caz, cel, (lo, hi) = eq.common_directions(dbs)
    y = ge.sh_orth(eq.ORDER, caz, cel)
    print(f"\nstored: {len(cll_ild)} heads; CLL and ILD {cll_ild.shape[1]} coefficients × {cll_ild.shape[2]} columns, "
          f"ITD {itd.shape[1]} coefficients; checked on {len(caz)} directions ({lo:.0f}…{hi:.0f}°), 2–16 kHz")
    for i, n in enumerate(dbs):
        before = np.einsum("dc,hcf->hdf", y, coefs[n])
        after = np.einsum("dc,hcf->hdf", y, cll_ild[database == i].astype(float))
        gap = np.sqrt(np.mean((after.mean(0) - y @ grand)[:, eq.COLS_CLL | eq.COLS_ILD] ** 2))
        sp = [nz.spread(x, x.mean(0), eq.COLS_CLL) for x in (before, after)]
        print(f"  {n}: its average from the common one {gap:.3f} dB; heads around their average {sp[0]:.2f} → {sp[1]:.2f} dB (CLL)")
    yi = ge.sh_orth(ITD_ORDER, caz, cel)
    for i, n in enumerate(dbs):
        e = yi @ itd[database == i].T
        print(f"  {n}: ITD spread between heads {1e6 * np.mean(e.std(1)):.0f} µs (mean over directions); "
              f"its average from the common one {1e6 * np.sqrt(np.mean((yi @ (itd_means[n] - itd_grand)) ** 2)):.0f} µs before normalising")

    np.savez_compressed(out, cll_ild=cll_ild, itd=itd, database=database, database_names=np.array(list(dbs)),
                        files=np.array(sum((files[n] for n in dbs), [])),
                        ku100_cll_ild=ku_c.astype(np.float32), ku100_itd=ku_itd_c.astype(np.float32),
                        database_means=np.stack([means[n] for n in dbs]).astype(np.float32), grand_mean=grand.astype(np.float32),
                        itd_database_means=np.stack([itd_means[n] for n in dbs]).astype(np.float32), itd_grand_mean=itd_grand.astype(np.float32),
                        order=eq.ORDER, itd_order=ITD_ORDER, nfft=eq.NFFT, fs=eq.FS, ild_limit_db=ge.ILD_LIMIT)
    print(f"written {out}: {os.path.getsize(out) / 1e6:.0f} MB")


if __name__ == "__main__":
    main(*sys.argv[1:])
