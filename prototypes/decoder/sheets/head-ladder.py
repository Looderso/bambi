"""What does one number a head buy for everything but the notches — the CLL's average, its smooth pattern, the ILD and
the ITD? From run-sheets.js's run: every head's parts, each database's mean taken out and the mean of the databases put
in its place. Half of the people give what is learned (the mean head, the components); the rungs are scored on the
other half, each part by how far it is from the head's own, RMS over the sphere (and over 0.3–16 kHz; the ILD 1–16 kHz):

  the KU100 for everyone
  nobody's head                 the mean of the heads
  + the ITD scaled              one factor
  + the ILD, the pattern scaled one factor on the ILD's, or on the pattern's, strength
  + a stretch along frequency   average, pattern and ILD shifted along the ERB axis by one amount
  + size                        one number: the ITD scaled by it and the spectra stretched the other way, as a larger
                                or smaller head would
  + components 1, 2, 3          the directions in which the heads vary most, all parts together (each part weighed by
                                its own spread), one number each

  python head-ladder.py <run folder>
"""
import glob, json, os, sys
import numpy as np

SETS = ["HUTUBS", "3D3A", "SONICOM"]


def main(folder):
    heads, rows = [], []
    for p in sorted(glob.glob(os.path.join(folder, "part-*.json"))):
        m = json.load(open(p)); nf, ki, kl = m["nf"], (m["itd"] + 1) ** 2, (m["ild"] + 1) ** 2
        x = np.fromfile(p.replace("part-", "parts-").replace(".json", ".f32"), "<f4").reshape(len(m["heads"]), -1)
        heads += m["heads"]; rows.append(x)
    x = np.concatenate(rows).astype(float)
    e = m["nf"]; cut = np.cumsum([nf, 25 * nf, ki, kl * nf])
    freq = (10 ** ((21.4 * np.log10(1 + 0.00437 * 300) + 0.1 * np.arange(nf)) / 21.4) - 1) / 0.00437
    wide, ildband = (freq <= 16000), (freq >= 1000) & (freq <= 16000)
    sn = lambda k: np.concatenate([np.full(2 * n + 1, 1 / np.sqrt(2 * n + 1)) for n in range(int(np.sqrt(k)))])   # SN3D: RMS over the sphere
    parts = {"average": x[:, :cut[0]] - x[:, :cut[0]][:, wide].mean(1, keepdims=True),
             "pattern": x[:, cut[0]:cut[1]].reshape(-1, 25, nf) * sn(25)[:, None],
             "ITD": x[:, cut[1]:cut[2]] * sn(ki),
             "ILD": x[:, cut[2]:cut[3]].reshape(-1, kl, nf) * sn(kl)[:, None]}
    band = {"average": wide, "pattern": wide, "ILD": ildband}
    # How far two heads' parts are apart: RMS over the sphere and the band.
    far = {"average": lambda a, b: np.sqrt(np.mean((a - b)[..., wide] ** 2, -1)),
           "pattern": lambda a, b: np.sqrt(np.mean(np.sum((a - b)[..., wide] ** 2, -2), -1)),
           "ILD": lambda a, b: np.sqrt(np.mean(np.sum((a - b)[..., ildband] ** 2, -2), -1)),
           "ITD": lambda a, b: np.sqrt(np.sum((a - b) ** 2, -1))}
    sets = np.array([h["set"] for h in heads]); people = sorted(set((h["set"], h["name"]) for h in heads if h["set"] in SETS))
    learnt = set(people[::2]); is_db = np.isin(sets, SETS)
    learn = np.array([(h["set"], h["name"]) in learnt for h in heads]); test = is_db & ~learn
    for k in parts:                                                        # each database's mean out, the databases' mean in
        means = {s: parts[k][sets == s].mean(0) for s in SETS}; common = np.mean([means[s] for s in SETS], 0)
        for s in SETS: parts[k][sets == s] += common - means[s]
    mean = {k: parts[k][learn].mean(0) for k in parts}
    ku = next(i for i, h in enumerate(heads) if h["set"] == "KU100" and h["ear"] == "left")

    def shifted(a, delta):                                                # a curve (… x nf) moved along the ERB axis by delta ERB
        pos = np.arange(nf) - delta / 0.1
        lo = np.clip(np.floor(pos).astype(int), 0, nf - 2); t = np.clip(pos - lo, 0, 1)
        return a[..., lo] * (1 - t) + a[..., lo + 1] * t

    idx = np.where(test)[0]; out = []
    def score(name, numbers, model):                                       # model(i) -> dict of the parts for head i
        errs = {k: [] for k in parts}
        for i in idx:
            mdl = model(i)
            for k in parts: errs[k].append(far[k](mdl[k], parts[k][i]))
        out.append((name, numbers, {k: np.median(errs[k]) for k in parts}))

    score("the KU100 for everyone", 0, lambda i: {k: parts[k][ku] if k != "average" else parts[k][ku] for k in parts})
    score("nobody's head", 0, lambda i: mean)
    scales = np.linspace(0.75, 1.3, 23)
    def itd_scaled(i):
        s = scales[np.argmin([far["ITD"](mean["ITD"] * q, parts["ITD"][i]) for q in scales])]
        return {**mean, "ITD": mean["ITD"] * s}
    score("+ the ITD scaled", 1, itd_scaled)
    def louder(key):                                                      # one part's strength scaled: a head that shadows more, or less
        def model(i):
            q = scales[np.argmin([far[key](mean[key] * v, parts[key][i]) for v in scales])]
            return {**mean, key: mean[key] * q}
        return model
    score("+ the ILD scaled", 1, louder("ILD"))
    score("+ the pattern scaled", 1, louder("pattern"))
    deltas = np.linspace(-2, 2, 33)
    spectra = lambda d: {k: shifted(mean[k], d) for k in ("average", "pattern", "ILD")}
    typical = {k: np.median([far[k](mean[k], parts[k][i]) for i in np.where(learn)[0]]) for k in parts}
    total = lambda mdl, i, keys: sum(far[k](mdl[k], parts[k][i]) / typical[k] for k in keys)
    def stretched(i):
        d = deltas[np.argmin([total(spectra(q), i, ("average", "pattern", "ILD")) for q in deltas])]
        return {**mean, **spectra(d)}
    score("+ a stretch along frequency", 1, stretched)
    def sized(i):
        best = min(scales, key=lambda q: total({**spectra(-21.4 * np.log10(q)), "ITD": mean["ITD"] * q}, i, parts))
        return {**spectra(-21.4 * np.log10(best)), "ITD": mean["ITD"] * best}
    score("+ size: ITD scaled, spectra stretched the other way", 1, sized)
    flat = lambda i: np.concatenate([(parts[k][i] - mean[k]).ravel()[np.broadcast_to(band.get(k, True), parts[k][i].shape).ravel()] / typical[k] / np.sqrt(parts[k][i].size) * (1 if k == "ITD" else 1) for k in parts])
    L = np.array([flat(i) for i in np.where(learn)[0]])
    _, sv, vt = np.linalg.svd(L - L.mean(0), full_matrices=False)
    def components(n):
        def model(i):
            v = flat(i); rec = (v @ vt[:n].T) @ vt[:n]; mdl, at = {}, 0
            for k in parts:
                mask = np.broadcast_to(band.get(k, True), parts[k][i].shape); size = int(mask.sum())
                full = np.zeros(parts[k][i].shape); full[mask] = rec[at:at + size] * typical[k] * np.sqrt(parts[k][i].size); at += size
                mdl[k] = mean[k] + full
            return mdl
        return model
    for n in (1, 2, 3, 6): score(f"+ component{'s 1–' + str(n) if n > 1 else ' 1'}", n, components(n))
    print(f"{int(test.sum())} heads of the people not learned from; how far each part is from the head's own, median")
    print("numbers".rjust(8) + "ITD µs".rjust(9) + "ILD dB".rjust(9) + "pattern dB".rjust(12) + "average dB".rjust(12))
    for name, numbers, e in out:
        print(f"{numbers:8d}{e['ITD']:9.1f}{e['ILD']:9.2f}{e['pattern']:12.2f}{e['average']:12.2f}   {name}")
    print("the heads' variation the first components hold (learned half): " + " ".join(f"{i + 1}: {100 * (sv[:i + 1] ** 2).sum() / (sv ** 2).sum():.0f} %" for i in (0, 1, 2, 5)))


if __name__ == "__main__":
    main(*sys.argv[1:])
