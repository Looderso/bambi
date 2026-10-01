"""Round three of the pattern model: two free inputs, mirrored, notches as deep as the heads' — not deeper.

pattern-models2.py found two free inputs as good as pattern and shift, and the dip-weighted loss deepening the
notches past the heads' own (140 %) at a price in evenness (the largest step between neighbouring pad points 3.4× the
median) from a low KL weight; and every variant overfitting after a few hundred to 1300 steps. Here, all with two free
inputs and the training heads also mirrored, 2000 steps, the best validation step kept, the depth term symmetric:
  V3   the notch layers as they are, no random shift          KL weight 0.02
  V3b  the same                                              KL weight 0.05
  V2b  randomly shifted by up to ±1.5 ERB each step           KL weight 0.02
Benchmarked as before on the same held-out heads; the grids saved for the match prototype.

  python pattern-models3.py <data set .npz>
"""
import os, sys, time
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np
import torch

_d = os.path.dirname(os.path.abspath(__file__))
_s = spec_from_file_location("p2", os.path.join(_d, "pattern-models2.py")); p2 = module_from_spec(_s); _s.loader.exec_module(p2)
pm, ex, log = p2.pm, p2.ex, p2.log
VARIANTS = [("V3", 0.0, 0.02), ("V3b", 0.0, 0.05), ("V2b", 1.5, 0.02)]   # name, random shift (ERB), KL weight


def main(path):
    t0 = time.time()
    data = np.load(path)
    az, el, med, polar = pm.directions()
    coefs = data["cll_ild"].astype(float)
    order, nfft, fs = int(data["order"]), int(data["nfft"]), int(data["fs"])
    full = ex.dtf(coefs, order, nfft, fs, az, el)
    ku = ex.dtf(data["ku100_cll_ild"].astype(float)[None], order, nfft, fs, az, el)[0]
    _, layer_all = ex.split(full, pm.W)
    base_all = full - layer_all
    mir_all = p2.mirrored_layers(coefs, order, nfft, fs, az, el)
    perm = np.random.default_rng(0).permutation(len(full))
    n_test = int(round(pm.TEST_SHARE * len(full)))
    test_i, train_i = perm[:n_test], perm[n_test:]
    base = base_all[train_i].mean(0)
    heads, own = full[test_i], layer_all[test_i]
    patterns = np.concatenate([layer_all[train_i], mir_all[train_i]])
    log(f"{len(train_i)} heads train (and mirrored), {len(test_i)} test; real heads' notch depth "
        f"{np.median(p2.depth_of(layer_all[train_i], med)):.1f} dB")
    results, saved = [], {}
    for name, aug, beta in VARIANTS:
        log(f"{name}: random shift ±{aug} ERB, KL weight {beta}")
        m, z = p2.train(patterns, base, 2, 128, 2000, beta, "both", aug, name)
        g = [np.linspace(np.percentile(z[:, k], 1) - 0.5, np.percentile(z[:, k], 99) + 0.5, 25) for k in range(2)]
        zz = np.stack(np.meshgrid(g[0], g[1], indexing="ij"), -1).reshape(-1, 2)
        grid = p2.decode(m, zz)
        r = p2.evaluate(f"{name}  shift ±{aug}, KL {beta}", grid, (25, 25), np.array([0.0]), base, heads, own, med)
        results.append(r)
        saved[name] = (grid.astype(np.float32), zz, np.array([zz[p[0]] for p in r["picks"]]))
    ref = np.array([pm.dist(ku, x) for x in heads])
    log(f"\nheld-out heads ({len(test_i)}), auditory measure dB/ERB, median; the KU100 as it is: {np.median(ref):.3f}")
    for r in results:
        log(f"  {r['name']:26s} distance {np.median(r['dist']):.3f} ({100 * (1 - np.median(r['dist']) / np.median(ref)):+.0f} %); "
            f"notch depth at the best place {100 * np.median(r['depth']):.0f} % of the head's own; "
            f"largest step {r['even']:.1f}× the median; depth over the pad {r['grid_depth']:.1f} dB")
    np.savez_compressed(os.path.splitext(path)[0] + "-models3.npz", base=base.astype(np.float32), az=az, el=el,
                        erb=ex.ERB, freq=ex.FREQ, sphere=np.arange(300),
                        **{f"{k}_grid": v[0] for k, v in saved.items()}, **{f"{k}_z": v[1] for k, v in saved.items()},
                        **{f"{k}_fitted": v[2] for k, v in saved.items()})
    log(f"saved ({time.time() - t0:.0f} s)")


if __name__ == "__main__":
    main(sys.argv[1])
