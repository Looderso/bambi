"""Does aligning in frequency before PCA describe a head better than PCA alone, for the same number of numbers?

Both encode a held-out head with a few numbers and decode it; the error is against the head itself, on its own
frequency axis, 2–16 kHz, median-plane CLL DTF (align.py's representation). Half the heads train, half are tested.

  PCA        the training heads' mean plus m principal components; a head is its m weights
  aligned    the head's warp onto the template (W numbers, align.py), PCA with m components on the aligned training
             heads, the reconstruction warped back to the head's frequency axis — W + m numbers

  python compare.py <data set .npz>
"""
import os, sys
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np
import plotly.graph_objects as go

_d = os.path.dirname(os.path.abspath(__file__))
_s = spec_from_file_location("al", os.path.join(_d, "align.py")); al = module_from_spec(_s); _s.loader.exec_module(al)
LOGF, BAND = al.LOGF, al.BAND
COMPONENTS = list(range(0, 31))
WARPS = [(1, 1), (4, 1), (4, 2)]                                         # (elevation terms, frequency anchors)


def pca(train):
    mean = train.mean(0)
    u, s, vt = np.linalg.svd((train - mean).reshape(len(train), -1), full_matrices=False)
    return mean, vt                                                     # components as rows


def encode_decode(x, mean, vt, m):
    if m == 0:
        return mean
    v = vt[:m]
    w = v @ (x - mean).ravel()
    return mean + (w @ v).reshape(mean.shape)


def unwarp(aligned, tau):
    """Back to the head's own axis: aligned(f) is the head at f + τ(f), so the head at g is aligned where f + τ = g."""
    return np.stack([np.interp(LOGF, LOGF + t, row) for row, t in zip(aligned, tau)])


def err(rec, x):
    return float(np.sqrt(np.mean((rec - x)[:, BAND] ** 2)))


def main(path):
    data = np.load(path)
    dtf = al.median_plane(data["cll_ild"].astype(float), int(data["order"]), int(data["nfft"]), int(data["fs"]))
    perm = np.random.default_rng(0).permutation(len(dtf))
    train, test = dtf[perm[: len(dtf) // 2]], dtf[perm[len(dtf) // 2:]]
    print(f"{len(train)} heads train, {len(test)} test; a held-out head's distance from the training mean "
          f"{np.mean([err(train.mean(0), x) for x in test]):.2f} dB RMS (2–16 kHz)")

    curves = {}
    mean, vt = pca(train)
    curves["PCA"] = [(m, np.mean([err(encode_decode(x, mean, vt, m), x) for x in test])) for m in COMPONENTS]

    for k, j in WARPS:
        bk, sj = al.angle_basis(k), al.freq_basis(j)
        template, tr_coefs = al.build_template(train, bk, sj)
        aligned_tr = np.stack([al.read(x, al.warp_field(c, bk, sj)) for x, c in zip(train, tr_coefs)])
        amean, avt = pca(aligned_tr)
        te_coefs = [al.align(x, template, bk, sj, np.zeros(k * j)) for x in test]
        taus = [al.warp_field(c, bk, sj) for c in te_coefs]
        aligned_te = [al.read(x, t) for x, t in zip(test, taus)]
        w = k * j
        curves[f"aligned, warp {w}"] = [(w + m, np.mean([err(unwarp(encode_decode(a, amean, avt, m), t), x)
                                                        for a, t, x in zip(aligned_te, taus, test)])) for m in COMPONENTS]

    for name, c in curves.items():
        pts = dict(c)
        print(f"  {name:16s}: " + "  ".join(f"{n} → {pts[n]:.2f}" for n in (1, 4, 8, 12, 16, 24, 30) if n in pts))
    # Numbers needed for a given error.
    for target in (2.0, 1.75, 1.5):
        need = {n: next((p for p, e in c if e <= target), None) for n, c in curves.items()}
        print(f"  numbers per head to reach {target} dB: " + ", ".join(f"{n} {v if v is not None else '> 30'}" for n, v in need.items()))

    fig = go.Figure()
    colours = {"PCA": "#1b1a17", "aligned, warp 1": "#a9a59c", "aligned, warp 4": "#5e8bae", "aligned, warp 8": "#d4873a"}
    for name, c in curves.items():
        c = np.array(c)
        fig.add_trace(go.Scatter(x=c[:, 0], y=c[:, 1], mode="lines+markers", name=name, line=dict(color=colours.get(name))))
    fig.update_layout(title="Held-out heads, median-plane CLL DTF: reconstruction error against the numbers that describe a head",
                      xaxis_title="numbers per head (warp + components)", yaxis_title="RMS error dB, 2–16 kHz",
                      height=480, template="plotly_white")
    fig.write_html(os.path.join(al.ge.OUT, "compare.html"), include_plotlyjs="cdn")


if __name__ == "__main__":
    main(sys.argv[1])
