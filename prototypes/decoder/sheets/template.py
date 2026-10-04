"""Do the heads share their notches? The surfaces of all heads (run-sheets.js) laid over each other.

A head here is symmetric, so its CLL is the same left and right, and a surface is folded onto the left half of the
sphere: at each direction the deeper of the surface there and at the mirror image. Folded surfaces are compared by

    distance = how far their centres are apart where both have a notch (ERB, weighted by depth)
               + 2 ERB x the share of their notch they do not share

and grouped around K typical surfaces (medoids). Prints
  - how a head's surfaces sit left and right: one over both halves, a mirrored pair, or on one side only
  - for several K: how close the surfaces are to their group's typical one, and how much closer than to the next
  - for one K (chosen by the best separation, or given): each group's frequency, its cover, the share of heads that
    have it, and how far a head's surface is from the typical one
  - a head's offsets — how much higher or lower its notches sit than the typical ones: their spread, whether the two
    ears of a person agree, whether the groups move together (one axis: the ear's size?), and, for HUTUBS, what they
    have to do with the measured ears
Writes <run folder>/template.json: the typical surfaces on the half sphere, for a page to draw.

  python template.py <run folder> [<HUTUBS AntrhopometricMeasures.csv>] [K]
"""
import glob, json, os, sys
import numpy as np

f_of = lambda e: (10 ** (e / 21.4) - 1) / 0.00437


def load(folder):
    heads, maps, grid = [], [], None
    for p in sorted(glob.glob(os.path.join(folder, "part-*.json"))):
        m = json.load(open(p)); grid = np.array(m["grid"]); heads += m["heads"]
        maps.append(np.fromfile(p[:-5] + ".f32", "<f4").reshape(len(m["heads"]), -1, len(m["grid"]), 3))
    return heads, np.concatenate(maps), grid


def distances(c, w, rows):
    """Rows of the distance matrix between folded surfaces: centres c and weights w (surfaces x points)."""
    lo = np.minimum(w[rows, None, :], w[None, :, :]).sum(-1)
    hi = np.maximum(w[rows, None, :], w[None, :, :]).sum(-1)
    apart = (np.minimum(w[rows, None, :], w[None, :, :]) * np.abs(c[rows, None, :] - c[None, :, :])).sum(-1) / np.maximum(lo, 1e-9)
    return np.where(lo > 0, apart, 5.0) + 2 * (1 - lo / np.maximum(hi, 1e-9))


def medoids(D, k, rng, turns=20):
    n = len(D); centre = [int(rng.integers(n))]
    while len(centre) < k:                                                  # spread out: far from those chosen
        d = D[:, centre].min(1) ** 2; centre.append(int(rng.choice(n, p=d / d.sum())))
    for _ in range(turns):
        label = np.argmin(D[:, centre], 1)
        new = [int(np.where(label == j)[0][np.argmin(D[np.ix_(label == j, label == j)].sum(1))]) if np.any(label == j) else centre[j] for j in range(k)]
        if new == centre: break
        centre = new
    return centre, np.argmin(D[:, centre], 1)


def main(folder, measures=None, k=None):
    heads, maps, grid = load(folder)
    H, S, G, _ = maps.shape; half = G // 2
    there = maps[..., 1] < -1                                               # heads x surfaces x points
    L, R = maps[:, :, :half], maps[:, :, half:]
    print(f"{H} symmetric heads, {S} surfaces each ({int(there.any(-1).sum())} carried), on {half} directions of the left half and their mirror images")
    cost = np.array([h["cost"] for h in heads]); none = np.array([h["costNone"] for h in heads])
    print(f"cost over all heads: the smooth part alone {np.median(none):.2f} dB, rebuilt from the surfaces {np.median(cost):.2f} dB "
          f"(5th–95th percentile {np.percentile(cost, 5):.2f}–{np.percentile(cost, 95):.2f})")

    # Left and right: a surface over both halves, or one of a mirrored pair, or on one side.
    tl, tr = there[:, :, :half], there[:, :, half:]
    both = (tl & tr).sum(-1) / np.maximum((tl | tr).sum(-1), 1)
    side = np.where(there.any(-1), np.where(both > 0.5, 0, 1), -1)
    pair = np.zeros((H, S), bool)
    for s in range(S):
        for t in range(S):
            if s == t: continue
            share = ((tl[:, s] & tr[:, t]).sum(-1) + (tr[:, s] & tl[:, t]).sum(-1)) / np.maximum((tl[:, s] | tr[:, t]).sum(-1) + (tr[:, s] | tl[:, t]).sum(-1), 1)
            near = np.abs(np.where(tl[:, s] & tr[:, t], L[:, s, :, 0] - R[:, t, :, 0], 0)).sum(-1) / np.maximum((tl[:, s] & tr[:, t]).sum(-1), 1)
            pair[:, s] |= (side[:, s] == 1) & (share > 0.4) & (near < 0.5)
    carried = side >= 0
    print(f"left and right: {100 * np.mean(side[carried] == 0):.0f} % of the surfaces lie over both halves, {100 * np.mean(pair[carried]):.0f} % are one of a mirrored pair, "
          f"{100 * np.mean((side[carried] == 1) & ~pair[carried]):.0f} % sit on one side only")

    # Folded onto the left half: the deeper of the two sides.
    deeper = L[..., 1] <= R[..., 1]
    fold = np.where(deeper[..., None], L, R).reshape(H * S, half, 3)
    keep = (fold[..., 1] < -1).sum(-1) >= 0.05 * half                       # a surface on less than a twentieth of the half is left out
    owner = np.repeat(np.arange(H), S)[keep]; fold = fold[keep]
    c, w = fold[..., 0], np.clip(-fold[..., 1] / 12, 0, 1) * (fold[..., 1] < -1)
    n = len(fold)
    D = np.concatenate([distances(c, w, np.arange(i, min(i + 64, n))) for i in range(0, n, 64)])
    print(f"\n{n} folded surfaces. Grouped around K typical ones — distance to the own typical surface, to the next one, and the separation (1: apart, 0: not):")
    rng = np.random.default_rng(3); tries = {}
    for K in (3, 4, 5, 6, 8, 10):
        best = None
        for _ in range(4):
            centre, label = medoids(D, K, rng); a = D[np.arange(n), [centre[j] for j in label]]
            if best is None or a.mean() < best[2].mean(): best = (centre, label, a)
        centre, label, a = best
        b = np.sort(D[:, centre], 1)[:, 1]; sil = np.mean((b - a) / np.maximum(np.maximum(a, b), 1e-9))
        twice = np.mean([len(set(label[owner == h])) < np.sum(owner == h) for h in range(H) if np.sum(owner == h)])
        tries[K] = (centre, label, a, sil)
        print(f"  K = {K:2d}: own {np.median(a):.2f} ERB, next {np.median(b):.2f}, separation {sil:.2f}; heads with two surfaces in one group {100 * twice:.0f} %")
    K = int(k) if k else max(tries, key=lambda q: tries[q][3])
    centre, label, a, sil = tries[K]
    print(f"\nK = {K}. Each group: its typical surface's frequency (5th–95th percentile of its directions) and cover of the half sphere; the heads that have it; "
          f"a surface's centre against the typical one where both have the notch, and their overlap (medians):")
    offsets = np.full((H, K), np.nan); order = np.argsort([np.average(c[m], weights=w[m]) for m in centre])
    for rank, j in enumerate(order):
        m = centre[j]; mine = np.where(label == j)[0]; on = w[m] > 0
        lo = np.minimum(w[mine], w[m]).sum(-1); shift = (np.minimum(w[mine], w[m]) * (c[mine] - c[m])).sum(-1) / np.maximum(lo, 1e-9)
        apart = (np.minimum(w[mine], w[m]) * np.abs(c[mine] - c[m] - shift[:, None])).sum(-1) / np.maximum(lo, 1e-9)
        overlap = lo / np.maximum(np.maximum(w[mine], w[m]).sum(-1), 1e-9)
        for h in np.unique(owner[mine]):                                    # a head's offset: of its surface with the most overlap
            q = np.where(owner[mine] == h)[0]; offsets[h, rank] = shift[q[np.argmax(lo[q])]]
        f = f_of(np.sort(c[m][on]))
        print(f"  {rank + 1}: {f[int(0.05 * len(f))] / 1000:5.1f}–{f[int(0.95 * len(f)) - 1] / 1000:4.1f} kHz, {100 * on.mean():3.0f} % of the half sphere · "
              f"{100 * len(np.unique(owner[mine])) / H:3.0f} % of the heads · offset {np.median(np.abs(shift)):.2f} ERB, then {np.median(apart):.2f} ERB apart · overlap {100 * np.median(overlap):.0f} %")

    print("\nA head's offsets (its notch higher or lower than the typical one, ERB):")
    print("  spread between heads, a group: " + " ".join(f"{np.nanstd(offsets[:, j]):.2f}" for j in range(K)))
    person = {}
    for i, h in enumerate(heads): person.setdefault((h["set"], h["name"]), {})[h["ear"]] = i
    two = np.array([[p["left"], p["right"]] for p in person.values() if len(p) == 2])
    ears = [np.corrcoef(*offsets[two][:, :, j][np.all(np.isfinite(offsets[two][:, :, j]), 1)].T)[0, 1] for j in range(K)]
    print("  a person's left ear against the right, correlation a group: " + " ".join(f"{r:+.2f}" for r in ears))
    corr = np.ma.corrcoef(np.ma.masked_invalid(offsets), rowvar=False).filled(0)
    off = corr[~np.eye(K, dtype=bool)]
    print(f"  between the groups, over the heads: correlations {off.min():+.2f} … {off.max():+.2f}, mean {off.mean():+.2f}; "
          f"one axis holds {100 * np.linalg.eigvalsh(corr)[-1] / K:.0f} % of them (apart: {100 / K:.0f} %)")
    mean = np.nanmean(offsets, 1)
    print(f"  a head's mean offset over its groups: spread {np.nanstd(mean):.2f} ERB; left ear against right "
          f"{np.corrcoef(*mean[two][np.all(np.isfinite(mean[two]), 1)].T)[0, 1]:+.2f}")
    sets = sorted(set(h["set"] for h in heads))
    print("  by database, mean offset: " + ", ".join(f"{s} {np.nanmean(mean[[h['set'] == s for h in heads]]):+.2f}" for s in sets))

    if measures:
        table = np.genfromtxt(measures, delimiter=",", names=True)
        names = {"d1": "cavum concha height", "d2": "cymba concha height", "d3": "cavum concha width", "d4": "fossa height", "d5": "pinna height",
                 "d6": "pinna width", "d7": "intertragal incisure width", "d8": "cavum concha depth"}
        rows = [(mean[i], {k: table[f"{h['ear'][0].upper()}_{k}"][table["SubjectID"] == int(h["name"][2:].split("_")[0])] for k in names})
                for i, h in enumerate(heads) if h["set"] == "HUTUBS" and np.isfinite(mean[i])]
        rows = [(m, {k: v[0] for k, v in d.items()}) for m, d in rows if all(len(v) and np.isfinite(v[0]) for v in d.values())]
        print(f"  HUTUBS, {len(rows)} ears: the mean offset against the ear as measured — correlation with " +
              ", ".join(f"{names[k]} {np.corrcoef([m for m, _ in rows], [d[k] for _, d in rows])[0, 1]:+.2f}" for k in names))

    json.dump({"grid": grid[:half].tolist(), "groups": [{"centre": c[centre[j]].tolist(), "depth": fold[centre[j], :, 1].tolist(),
               "heads": float(len(np.unique(owner[label == j])) / H)} for j in order]}, open(os.path.join(folder, "template.json"), "w"))
    print(f"\nwrote {os.path.join(folder, 'template.json')}")


if __name__ == "__main__":
    main(*sys.argv[1:])
