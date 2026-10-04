"""A first look at run-split.js's three parts over all symmetric heads, and the database's offset taken out of each.

Per symmetric head (one ear of a measured head, mirrored):
  the average   the CLL's mean over the directions plus the pattern's order 0: one curve on the axis; every head's
                is brought to the same level (its mean over 0.3–16 kHz)
  the pattern   the smooth part's orders 1…ORDER, spherical harmonics (SN3D) a point of the axis
  the dips      four a direction: centre (ERB), depth (dB), sigma (ERB)
The database's offset: from the average and from the pattern a database's own mean over its heads is taken and the
mean of the databases' means put in its place, so a head keeps how it differs from its own database.

Prints
  - what the split left: neither smooth nor dip, and how far the pattern is from the free smooth part
  - how far the databases' means are apart, against how far heads are from their own database's mean
  - whether a linear classifier names the database from each part, before and after — both ears of a head held out
    together, the offsets taken from the training heads only; balanced accuracy, chance 33 %
  - how many components hold the heads' variation in the average and in the pattern, after the offsets are out
  - the dips: how many a direction, how deep, and whether a dip has a neighbour in the next direction
Writes <run folder>/parts.npz: the level-free averages and the patterns, before the offsets are out, with each head's
database, name and ear, and the databases' means.

  python analyse-run.py <run folder>
"""
import glob, json, os, sys
import numpy as np

SETS = ["HUTUBS", "3D3A", "SONICOM"]
rms = lambda x, axis=None: np.sqrt(np.mean(np.square(x), axis=axis))


def load(folder):
    here = os.path.dirname(os.path.abspath(__file__))
    heads, meta = [], None
    for path in sorted(glob.glob(os.path.join(folder, "manifest-*.json"))):
        m = json.load(open(path)); meta = m
        heads += m["heads"]
    nf, k, dips = meta["nf"], (meta["order"] + 1) ** 2, meta["settings"]["dips"]
    grids = {}
    for h in heads:
        n = h["directions"]
        x = np.fromfile(os.path.join(folder, h["file"]), "<f4")
        cut = np.cumsum([nf, k * nf, n * dips * 3, n, n, n])
        avg, coef, par, over, off, itd = np.split(x, cut)[:6]
        coef = coef.reshape(k, nf)
        h.update(average=avg + coef[0], pattern=coef[1:], dips=par.reshape(n, dips, 3), over=over, off=off, itd=itd)
        if h["set"] not in grids:
            raw = np.fromfile(os.path.join(here, "heads", h["head"]), "<f4", count=2 * n, offset=4).reshape(n, 2)
            az, el = np.radians(raw[:, 0]), np.radians(raw[:, 1])
            grids[h["set"]] = np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], -1)
    freq = (10 ** ((meta["e0"] + meta["step"] * np.arange(nf)) / 21.4) - 1) / 0.00437
    return heads, grids, freq, meta


def classify(x, label, group, offsets, rng):
    """Balanced accuracy of a linear classifier (20 principal components, one-against-all least squares) naming the
    database, five folds over the measured heads; offsets(x, label, train) takes the databases' means out, or not."""
    groups = np.unique(group); rng.shuffle(groups)
    hit, count = np.zeros(len(SETS)), np.zeros(len(SETS))
    for fold in range(5):
        test = np.isin(group, groups[fold::5]); train = ~test
        z = offsets(x, label, train)
        mean = z[train].mean(0)
        u, s, vt = np.linalg.svd(z[train] - mean, full_matrices=False)
        basis = vt[:20].T / s[:20]
        a, b = (z[train] - mean) @ basis, (z[test] - mean) @ basis
        a, b = np.c_[a, np.ones(len(a))], np.c_[b, np.ones(len(b))]
        even = np.sqrt(1 / np.bincount(label[train])[label[train]])[:, None]          # every database weighs the same
        w = np.linalg.lstsq(a * even, np.eye(len(SETS))[label[train]] * even, rcond=None)[0]
        said = np.argmax(b @ w, 1)
        for c in range(len(SETS)):
            hit[c] += np.sum(said[label[test] == c] == c); count[c] += np.sum(label[test] == c)
    return np.mean(hit / count)


def without_offsets(x, label, train):
    means = np.array([x[train & (label == c)].mean(0) for c in range(len(SETS))])
    return x - means[label] + means.mean(0)


def components(x, counts=(1, 2, 3, 5, 10)):
    s = np.linalg.svd(x - x.mean(0), compute_uv=False) ** 2
    return " ".join(f"{c}: {100 * s[:c].sum() / s.sum():.0f} %" for c in counts)


def main(folder):
    heads, grids, freq, meta = load(folder)
    wide = (freq >= 300) & (freq <= 16000)
    order = meta["order"]
    weight = np.concatenate([np.full(2 * n + 1, 1 / np.sqrt(2 * n + 1)) for n in range(1, order + 1)])   # SN3D: RMS over the sphere
    db = [h for h in heads if h["set"] in SETS]
    label = np.array([SETS.index(h["set"]) for h in db])
    group = np.array([h["set"] + h["name"] for h in db])
    average = np.array([h["average"] - h["average"][wide].mean() for h in db])[:, wide]
    pattern = np.array([h["pattern"] * weight[:, None] for h in db])[:, :, wide]
    print(f"{len(heads)} symmetric heads ({', '.join(f'{s} {np.sum(label == i)}' for i, s in enumerate(SETS))}, and "
          f"{len(heads) - len(db)} others); settings {meta['settings']}, pattern order {order}")

    print("\nWhat the split left, median over heads (5th–95th percentile):")
    for key, text in (("over", "neither smooth nor dip, 4–16 kHz"), ("off", "the pattern from the free smooth part, 0.3–16 kHz")):
        v = np.array([rms(h[key]) for h in db])
        print(f"  {text:50s} {np.median(v):.2f} dB RMS ({np.percentile(v, 5):.2f}–{np.percentile(v, 95):.2f})")

    print("\nThe databases' offsets, dB RMS (the pattern: over the sphere and the axis):")
    for name, x in (("average", average), ("pattern", pattern)):
        flat = x.reshape(len(x), -1); scale = np.sqrt(flat.shape[1] / (x.shape[-1]))
        means = np.array([flat[label == c].mean(0) for c in range(len(SETS))])
        apart = [rms(means[i] - means[j]) * scale for i in range(3) for j in range(i)]
        within = [np.median(rms(flat[label == c] - means[c], 1)) * scale for c in range(3)]
        print(f"  {name}: the databases' means {min(apart):.2f}–{max(apart):.2f} apart; a head from its own database's "
              f"mean {min(within):.2f}–{max(within):.2f}")

    # The dips on directions all three databases measured: the dips' curve in 14 bands of one ERB from 4 kHz.
    common = fibonacci(240); common = common[(common[:, 2] > np.sin(np.radians(-40))) & (common[:, 2] < np.sin(np.radians(70)))]
    centres = np.arange(21.4 * np.log10(1 + 0.00437 * 4000), 21.4 * np.log10(1 + 0.00437 * 18000), 1.0)
    near = {s: np.argmax(grids[s] @ common.T, 0) for s in SETS}
    layer = np.array([curves(h["dips"][near[h["set"]]], centres) for h in db]).reshape(len(db), -1)

    print("\nA linear classifier names the database (balanced accuracy, chance 33 %):")
    for name, x in (("average", average), ("pattern", pattern.reshape(len(db), -1)), ("dips", layer)):
        raw = classify(x, label, group, lambda z, l, t: z, np.random.default_rng(1))
        out = classify(x, label, group, without_offsets, np.random.default_rng(1))
        print(f"  {name:8s} as it is {100 * raw:3.0f} %   the databases' means taken out {100 * out:3.0f} %")

    every = np.ones(len(db), bool)
    a, p = without_offsets(average, label, every), without_offsets(pattern.reshape(len(db), -1), label, every)
    print("\nThe heads' variation held by the first components, the offsets out:")
    print(f"  average  {components(a)}   (a head from the mean: {np.median(rms(a - a.mean(0), 1)):.2f} dB RMS)")
    print(f"  pattern  {components(p)}   (a head from the mean: {np.median(rms(p - p.mean(0), 1)) * np.sqrt(len(weight)):.2f} dB RMS over the sphere)")
    print(f"  both     {components(np.c_[a, p])}")

    print("\nThe dips:")
    depth = np.concatenate([h["dips"][..., 1].ravel() for h in db])
    count = np.concatenate([(h["dips"][..., 1] < -3).sum(1) for h in db])
    print(f"  deeper than 3 dB, a direction: " + ", ".join(f"{k}: {100 * np.mean(count == k):.0f} %" for k in range(5))
          + f"; depth, median of those {np.median(depth[depth < -3]):.1f} dB; sigma {np.median(np.concatenate([h['dips'][..., 2][h['dips'][..., 1] < -3] for h in db])):.2f} ERB")
    gaps = []
    for h in db[::4]:
        g = grids[h["set"]]; d = g @ g.T; np.fill_diagonal(d, -2); nb = np.argmax(d, 1)
        c, a_ = h["dips"][..., 0], h["dips"][..., 1]
        for j in range(4):
            deep = a_[:, j] < -3
            other = np.where(a_[nb] < -3, c[nb], np.inf)
            gaps.append(np.min(np.abs(other[deep] - c[deep, j][:, None]), 1))
    gaps = np.concatenate(gaps)
    step = np.median([np.degrees(np.arccos(np.clip(np.sort(grids[s] @ grids[s].T, 1)[:, -2], -1, 1))).mean() for s in SETS])
    print(f"  a dip deeper than 3 dB and the nearest such dip in the neighbouring direction (about {step:.0f}° away): "
          f"median {np.median(gaps):.2f} ERB apart, {100 * np.mean(gaps < 0.5):.0f} % within 0.5 ERB, {100 * np.mean(gaps < 1):.0f} % within 1 ERB")

    means = {s: (np.array([h["average"] - h["average"][wide].mean() for h in db if h["set"] == s]).mean(0),
                 np.array([h["pattern"] for h in db if h["set"] == s]).mean(0)) for s in SETS}
    np.savez_compressed(os.path.join(folder, "parts.npz"), freq=freq,
                        average=np.array([h["average"] - h["average"][wide].mean() for h in heads]).astype("f4"),
                        pattern=np.array([h["pattern"] for h in heads]).astype("f4"),
                        set=[h["set"] for h in heads], name=[h["name"] for h in heads], ear=[h["ear"] for h in heads],
                        **{f"mean_average_{s}": means[s][0] for s in SETS}, **{f"mean_pattern_{s}": means[s][1] for s in SETS})
    print(f"\nwrote {os.path.join(folder, 'parts.npz')}")


def fibonacci(n):
    i = np.arange(n) + 0.5
    az, el = np.pi * (3 - np.sqrt(5)) * i, np.arcsin(1 - 2 * i / n)
    return np.stack([np.cos(el) * np.cos(az), np.cos(el) * np.sin(az), np.sin(el)], -1)


def curves(dips, centres):
    """The dips' curves at the centres (ERB): directions x len(centres), dB."""
    c, a, s = dips[..., 0:1], dips[..., 1:2], dips[..., 2:3]
    return np.sum(a * np.exp(-0.5 * ((centres - c) / s) ** 2), 1)


if __name__ == "__main__":
    main(*sys.argv[1:])
