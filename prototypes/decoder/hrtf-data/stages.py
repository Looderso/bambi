"""Where do the CLL's notches get wider? Their width and depth, stage by stage, from a real head to what is played.

For each held-out head at its best place on the two-input pad (V2b, pattern-models3.py), on 19 directions of the frontal
median plane (−30…60°, where the notches are clearest), the deepest notch of 4–14 kHz in the notch layer (the 3-ERB
split of extract.py, taken alike at every stage): its depth, and its width at half that depth, in ERB.
  real head          the head's own DTF (the data set's order-20 SH)
  VAE                the model's pattern at the head's best place, on the 361 directions it generates
  export, order N    that pattern fitted with order-N SH on the 300 sphere directions, as export-model.py does
  grid node          the 13 x 13 grid the page interpolates on, at a node (order 8)
  grid cell middle   halfway between four nodes — the blend the page makes there (order 8)
  renderer           the grid cell middle through the page's renderer: to the 129 linear bins, the two ears from the
                     KU100's timbre and the population ILD, minimum phase, 256 taps, back to the ERB axis
  real head, rendered  the real head through the same renderer — what the renderer alone costs

  python stages.py <data set .npz> <...-models3.npz>
"""
import os, sys
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np

_d = os.path.dirname(os.path.abspath(__file__))
def _load(name, path):
    s = spec_from_file_location(name, path); m = module_from_spec(s); s.loader.exec_module(m); return m
ex = _load("ex", os.path.join(_d, "extract.py"))
bm = _load("bm", os.path.join(_d, "..", "match", "build-match.py"))
pm = _load("pm", os.path.join(_d, "pattern-models.py"))
ge = ex.ge
NOTCH = (ex.FREQ >= 4000) & (ex.FREQ <= 14000)


def notch(layer_row):
    """Depth (dB below 0) and width at half depth (ERB) of the deepest dip in NOTCH."""
    k = np.where(NOTCH)[0]
    i = k[int(np.argmin(layer_row[k]))]
    depth = -layer_row[i]
    if depth <= 0:
        return 0.0, np.nan
    half = -depth / 2
    lo = i
    while lo > 0 and layer_row[lo] < half:
        lo -= 1
    hi = i
    while hi < len(layer_row) - 1 and layer_row[hi] < half:
        hi += 1
    return float(depth), float((hi - lo) * ex.STEP)


def measure(dtf_rows):
    _, layer = ex.split(dtf_rows, 3)
    return np.array([notch(r) for r in layer])                          # dirs x (depth, width)


def sh_fit_eval(pattern, az, el, sphere, out_idx, order):
    y = ge.sh_orth(order, az[sphere], el[sphere])
    c = ge.solve(y, np.full(len(sphere), 1 / len(sphere)), pattern[sphere], 1e-4, order)
    return ge.sh_orth(order, az[out_idx], el[out_idx]) @ c


def render(dtf_erb, timbre_lin, ild_lin, fr):
    """The page's chain for directions: ERB-axis DTF → linear bins (+ timbre) → ears with the ILD → minimum phase →
    magnitude → CLL → ERB axis DTF (less the timbre)."""
    out = []
    e_of_f = 21.4 * np.log10(1 + 0.00437 * fr)
    for d, ild in zip(dtf_erb, ild_lin):
        cll = np.interp(e_of_f, ex.ERB, d) + timbre_lin
        r = 10 ** (np.clip(ild, -40, 40) / 10)
        l_db, r_db = cll + 10 * np.log10(r / (1 + r)), cll - 10 * np.log10(1 + r)
        hl, hr = bm.min_phase(l_db), bm.min_phase(r_db)
        pl, pr = (np.abs(np.fft.rfft(h, 256)) ** 2 for h in (hl, hr))
        cll_out = 10 * np.log10(np.maximum(pl + pr, 1e-6)) - timbre_lin
        out.append(np.interp(ex.FREQ, fr, cll_out))
    return np.array(out)


def main(data_path, models_path):
    data, m = np.load(data_path), np.load(models_path)
    az, el, sphere = m["az"], m["el"], m["sphere"]
    med = np.setdiff1d(np.arange(len(az)), sphere)
    polar = np.arange(-45, 226, 4.5)
    front = med[(polar >= -30) & (polar <= 60)]
    order, nfft, fs = int(data["order"]), int(data["nfft"]), int(data["fs"])
    nb, fr = nfft // 2 + 1, np.fft.rfftfreq(nfft, 1 / fs)
    full = ex.dtf(data["cll_ild"].astype(float), order, nfft, fs, az, el)
    perm = np.random.default_rng(0).permutation(len(full))
    test = perm[:int(round(pm.TEST_SHARE * len(full)))]
    grid = m["V2b_grid"].astype(float)                                  # 625 x dirs x E
    zz = m["V2b_z"]
    fitted = m["V2b_fitted"]
    base = m["base"].astype(float)
    g25 = grid.reshape(25, 25, len(az), -1)
    timbre = data["ku100_cll_ild"].astype(float)[0, :nb] / np.sqrt(4 * np.pi)
    ild = ge.sh_orth(order, az[front], el[front]) @ data["grand_mean"].astype(float)[:, nb:]

    stages = {k: [] for k in ("real head", "VAE", "export, order 8", "export, order 15", "export, order 20",
                              "grid node (order 8)", "grid cell middle (order 8)", "renderer", "real head, rendered")}
    for h, z in zip(test, fitted):
        gi = int(np.argmin(np.sum((zz - z) ** 2, 1)))
        i, j = divmod(gi, 25)
        pat = grid[gi]
        stages["real head"].append(measure(full[h][front]))
        stages["VAE"].append(measure(base[front] + pat[front]))
        for o in (8, 15, 20):
            stages[f"export, order {o}"].append(measure(base[front] + sh_fit_eval(pat, az, el, sphere, front, o)))
        # The 13 x 13 grid: nodes are the 25-grid's even indices. Node nearest to the best place, and the middle of
        # the cell around it (the blend of its four corners).
        ni, nj = min(12, int(round(i / 2))), min(12, int(round(j / 2)))
        node = g25[2 * ni, 2 * nj]
        stages["grid node (order 8)"].append(measure(base[front] + sh_fit_eval(node, az, el, sphere, front, 8)))
        a, b = min(ni, 11), min(nj, 11)
        mid = 0.25 * (g25[2 * a, 2 * b] + g25[2 * a + 2, 2 * b] + g25[2 * a, 2 * b + 2] + g25[2 * a + 2, 2 * b + 2])
        mid_d = base[front] + sh_fit_eval(mid, az, el, sphere, front, 8)
        stages["grid cell middle (order 8)"].append(measure(mid_d))
        stages["renderer"].append(measure(render(mid_d, timbre, ild, fr)))
        stages["real head, rendered"].append(measure(render(full[h][front], timbre, ild, fr)))

    print(f"{len(test)} held-out heads × {len(front)} directions of the frontal median plane; the deepest notch 4–14 kHz "
          f"of each, in the 3-ERB notch layer. Median over heads and directions:")
    ref_d = np.nanmedian(np.concatenate(stages["real head"])[:, 0])
    ref_w = np.nanmedian(np.concatenate(stages["real head"])[:, 1])
    for k, v in stages.items():
        a = np.concatenate(v)
        d, w = np.nanmedian(a[:, 0]), np.nanmedian(a[:, 1])
        print(f"  {k:28s} depth {d:5.1f} dB ({100 * d / ref_d:4.0f} %)   width at half depth {w:4.2f} ERB ({100 * w / ref_w:4.0f} %)")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
