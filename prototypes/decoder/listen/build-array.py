"""AllRAD for a loudspeaker layout read from an IEM AllRADecoder configuration file.

Writes array.js: the layout, and for orders 1-3 a decoder from AmbiX (SN3D) to the real loudspeakers, max-rE
weighted, scaled so a plane wave from any direction gives a loudspeaker energy of 1 on average.

Loudspeakers named in IMAGINARY take part in the triangulation and their signal is dropped: a layout with nothing
below the listener gets a point below, so a source down there pans to the lowest ring instead of vanishing.

Checks itself against the file's own matrix: at the file's order, N3D, no weights, the same loudspeakers real,
the design here should reproduce the IEM plug-in's.

  python build-array.py path/to/decoder.json
"""
import json, sys
import numpy as np
from scipy.spatial import ConvexHull
from scipy.special import eval_legendre

sys.path.insert(0, ".")
from importlib.util import spec_from_file_location, module_from_spec
_s = spec_from_file_location("bf", "build-filters.py"); bf = module_from_spec(_s); _s.loader.exec_module(bf)

IMAGINARY = {18}    # channels: here the point below the listener
C = 343.0
K = 5200            # design points, a Fibonacci sphere


def fibonacci(k):
    i = np.arange(k) + 0.5
    z = 1 - 2 * i / k
    r = np.sqrt(1 - z * z)
    a = np.pi * (3 - np.sqrt(5)) * i
    return np.stack([r * np.cos(a), r * np.sin(a), z], 1)


def xyz(az_deg, el_deg):
    az, el = np.radians(az_deg), np.radians(el_deg)
    return np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], -1)


def vbap(points, spk):
    """Energy-normalised VBAP gains (points x loudspeakers) over the convex hull of spk."""
    hull = ConvexHull(spk)
    g = np.zeros((len(points), len(spk)))
    done = np.zeros(len(points), bool)
    for tri in hull.simplices:
        gains = points @ np.linalg.inv(spk[tri])            # p = g · L, one row per point
        inside = np.all(gains >= -1e-9, 1) & ~done
        g[np.ix_(inside, tri)] = gains[inside]
        done |= inside
    assert done.all(), "a design point fell outside every triangle"
    return g / np.linalg.norm(g, axis=1, keepdims=True)


def max_re(order):
    return np.array([eval_legendre(n, np.cos(np.radians(137.9 / (order + 1.51)))) for n in range(order + 1)])


def allrad(order, spk, keep, weights=True, n3d=False):
    t = fibonacci(K)
    az, el = np.arctan2(t[:, 1], t[:, 0]), np.arcsin(t[:, 2])
    y = bf.sh_sn3d(order, az, el)
    per_n = np.array([n for n in range(order + 1) for _ in range(2 * n + 1)])
    w = max_re(order)[per_n] if weights else np.ones_like(per_n, float)
    col = (np.sqrt(2 * per_n + 1) if n3d else (2 * per_n + 1)) * w
    d = vbap(t, spk).T @ (y * col) / K                      # loudspeakers x SH channels
    return d[keep]


def main(path):
    cfg = json.load(open(path))
    spk = cfg["LoudspeakerLayout"]["Loudspeakers"]
    dirs = xyz(np.array([s["Azimuth"] for s in spk]), np.array([s["Elevation"] for s in spk]))
    chans = [s["Channel"] for s in spk]

    ref = np.array(cfg["Decoder"]["Matrix"])
    ref_order = int(np.sqrt(ref.shape[1])) - 1
    ours = allrad(ref_order, dirs, np.arange(len(spk)), weights=False, n3d=True)
    c = np.sum(ours * ref) / np.sum(ours * ours)
    err = np.linalg.norm(c * ours - ref) / np.linalg.norm(ref)
    print(f"against the file's order-{ref_order} matrix: relative error {100 * err:.2f} % (scale {c:.3f})")
    t = fibonacci(2000)
    y5 = bf.sh_sn3d(ref_order, np.arctan2(t[:, 1], t[:, 0]), np.arcsin(t[:, 2])) \
        * np.sqrt(2 * np.array([n for n in range(ref_order + 1) for _ in range(2 * n + 1)]) + 1)
    ga, gb = y5 @ (c * ours).T, y5 @ ref.T
    print(f"  loudspeaker gains over 2000 directions: worst difference {20 * np.log10(np.max(np.abs(ga - gb)) / np.max(np.abs(gb))):.1f} dB below the largest gain")

    keep = np.array([i for i, ch in enumerate(chans) if ch not in IMAGINARY])
    radius = np.array([spk[i]["Radius"] for i in keep])
    out = {"speakers": [{"az": spk[i]["Azimuth"], "el": spk[i]["Elevation"], "r": spk[i]["Radius"],
                         "ch": spk[i]["Channel"]} for i in keep],
           "imaginary": [{"az": spk[i]["Azimuth"], "el": spk[i]["Elevation"], "ch": spk[i]["Channel"]}
                         for i, ch in enumerate(chans) if ch in IMAGINARY]}
    tt = fibonacci(4000)
    for order in (1, 2, 3):
        d = allrad(order, dirs, keep)
        y = bf.sh_sn3d(order, np.arctan2(tt[:, 1], tt[:, 0]), np.arcsin(tt[:, 2]))
        e = np.sum((y @ d.T) ** 2, 1)
        d /= np.sqrt(e.mean())
        e /= e.mean()
        rv = ((y @ d.T) ** 2) @ dirs[keep] / np.sum((y @ d.T) ** 2, 1, keepdims=True)
        re = np.linalg.norm(rv, axis=1)
        ang = np.degrees(np.arccos(np.clip(np.sum(rv / re[:, None] * tt, 1), -1, 1)))
        up, down = tt[:, 2] > -0.5, tt[:, 2] <= -0.5
        db = 10 * np.log10(e)
        print(f"order {order}: energy {db[up].min():5.1f} … {db[up].max():4.1f} dB (below −30°: {db[down].min():5.1f} … {db[down].max():4.1f}), "
              f"rE {re[up].mean():.2f} (below −30°: {re[down].mean():.2f}), "
              f"direction error median {np.median(ang[up]):.1f}° (below −30°: {np.median(ang[down]):.1f}°)")
        out[f"order{order}"] = d.round(7).tolist()

    with open("array.js", "w") as js:
        js.write(f"// Generated by build-array.py from {cfg['Description'].split('.')[0].strip()}.\n")
        js.write("// Loudspeakers: azimuth, elevation in degrees, radius in metres. orderN: loudspeakers x (N+1)² for SN3D input.\n")
        js.write("const ARRAY = " + json.dumps(out) + ";\n")


if __name__ == "__main__":
    main(sys.argv[1])
