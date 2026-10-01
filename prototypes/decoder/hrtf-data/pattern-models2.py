"""Deeper notches and two free inputs: three variants of the pattern model, benchmarked on the same held-out heads.

As pattern-models.py — CLL DTF split at 3 ERB, the population baseline fixed, the notch layer generated — with:
  V0  one free input and the shift, as before: shift-free training patterns, β = 0.05            (the reference)
  V1  one free input and the shift, improved: the training heads also mirrored left–right (a mirrored head is a
      plausible head; in SH exact: the sine-type coefficients change sign), a loss that weights the dips — where the
      head's notch layer is below 0 an error counts up to four times, and a generated notch shallower than the head's
      costs extra — β = 0.005, a wider network, more steps
  V2  two free inputs and no shift: the notch layers as they are (shift included), mirrored, each step shifted at
      random by up to ±1.5 ERB so that position is part of what the inputs carry; V1's loss and settings
Each trains with a tenth of its training heads held back for validation, and keeps the step where that was best.
On the held-out heads: the distance at the best place on the pad (auditory measure), how deep the notches come out
there against the head's own, and how even the pad is (the largest step between neighbouring grid points against
the median one).

  python pattern-models2.py <data set .npz>
"""
import os, sys, time, copy
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np
import torch

_d = os.path.dirname(os.path.abspath(__file__))
_s = spec_from_file_location("pm", os.path.join(_d, "pattern-models.py")); pm = module_from_spec(_s); _s.loader.exec_module(pm)
ex, ge, DEVICE, log = pm.ex, pm.ge, pm.DEVICE, pm.log
torch.manual_seed(0)
SCALE = 5.0
NOTCH_K = (ex.FREQ >= 5000) & (ex.FREQ <= 12000)


class Net(torch.nn.Module):
    """Encoder to a latent of `latent` dimensions, decoder back to the notch layer (directions x ERB)."""
    def __init__(self, channels, length, latent, hidden):
        super().__init__()
        self.length, self.hidden, self.l0 = length, hidden, (length + 3) // 4
        self.enc = torch.nn.Sequential(
            torch.nn.Conv1d(channels, hidden, 5, stride=2, padding=2), torch.nn.GELU(),
            torch.nn.Conv1d(hidden, hidden, 5, stride=2, padding=2), torch.nn.GELU(), torch.nn.Flatten())
        n = hidden * self.l0
        self.mu, self.logvar = torch.nn.Linear(n, latent), torch.nn.Linear(n, latent)
        self.dec_in = torch.nn.Linear(latent, hidden * self.l0)
        self.dec = torch.nn.Sequential(
            torch.nn.GELU(),
            torch.nn.ConvTranspose1d(hidden, hidden, 4, stride=2, padding=1), torch.nn.GELU(),
            torch.nn.Conv1d(hidden, hidden, 5, padding=2), torch.nn.GELU(),
            torch.nn.ConvTranspose1d(hidden, hidden, 4, stride=2, padding=1), torch.nn.GELU(),
            torch.nn.Conv1d(hidden, channels, 5, padding=2))

    def decode(self, z):
        return self.dec(self.dec_in(z).view(len(z), self.hidden, self.l0))[..., :self.length]

    def forward(self, x):
        h = self.enc(x)
        mu, logvar = self.mu(h), self.logvar(h).clamp(-8, 4)
        return self.decode(mu + torch.randn_like(mu) * torch.exp(0.5 * logvar)), mu, logvar


def torch_shift(x, s):
    """x (batch x dirs x ERB) read at ERB + s per batch row (ERB): a feature at e moves to e − s."""
    n = x.shape[-1]
    pos = (torch.arange(n, device=x.device)[None, :] + s[:, None] / ex.STEP).clamp(0, n - 1)
    i = pos.floor().long().clamp(max=n - 2)
    f = (pos - i)[:, None, :]
    i = i[:, None, :].expand(-1, x.shape[1], -1)
    return torch.gather(x, 2, i) * (1 - f) + torch.gather(x, 2, i + 1) * f


def loss_fn(rec, x, b, smooth, band, k_mask, depth_weight):
    pr, pt = pm.torch_profile(b + rec, smooth, band), pm.torch_profile(b + x, smooth, band)
    loss = torch.mean((pr - pt) ** 2) * SCALE ** 2
    err = (rec - x) * SCALE
    if depth_weight:
        dip = torch.relu(-x * SCALE) / 3.0                                # dB below 0, in 3-dB units
        w = 1 + torch.clamp(dip, max=3.0)                                 # up to four times where the head dips
        loss = loss + 0.05 * torch.mean(w * err ** 2)
        d_rec = (rec * SCALE)[..., k_mask]; d_x = (x * SCALE)[..., k_mask]
        depth_rec = d_rec.max(-1).values - d_rec.min(-1).values
        depth_x = d_x.max(-1).values - d_x.min(-1).values
        loss = loss + 0.02 * torch.mean(torch.relu(depth_x - depth_rec) ** 2)  # a shallower notch than the head's costs
    else:
        loss = loss + 0.05 * torch.mean(err ** 2)
    return loss


def train(patterns, base, latent, hidden, steps, beta, depth_weight, shift_aug, name):
    """Returns the decoder at the step where the validation loss was lowest, and the training heads' latents."""
    rng = np.random.default_rng(1)
    idx = rng.permutation(len(patterns))
    n_val = max(8, len(patterns) // 10)
    val, tr = idx[:n_val], idx[n_val:]
    x = torch.tensor(patterns / SCALE, dtype=torch.float32, device=DEVICE)
    b = torch.tensor(base / SCALE, dtype=torch.float32, device=DEVICE)
    smooth, band = pm.smoothing_kernel(), torch.tensor(np.where(ex.BAND)[0], device=DEVICE)
    k_mask = torch.tensor(NOTCH_K, device=DEVICE)
    model = Net(x.shape[1], x.shape[2], latent, hidden).to(DEVICE)
    opt = torch.optim.Adam(model.parameters(), lr=1e-3)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, steps)
    best, best_state = np.inf, None
    xtr, xval = x[tr], x[val]
    for step in range(steps):
        xb = xtr
        if shift_aug:
            xb = torch_shift(xb, (torch.rand(len(xb), device=DEVICE) * 2 - 1) * shift_aug)
        rec, mu, logvar = model(xb)
        kl = -0.5 * torch.mean(1 + logvar - mu ** 2 - logvar.exp())
        loss = loss_fn(rec, xb, b, smooth, band, k_mask, depth_weight) + beta * min(1.0, step / 1000) * kl
        opt.zero_grad(); loss.backward(); opt.step(); sched.step()
        if step % 100 == 0 or step == steps - 1:
            model.eval()
            with torch.no_grad():
                h = model.enc(xval)
                vloss = loss_fn(model.decode(model.mu(h)), xval, b, smooth, band, k_mask, depth_weight).item()
            model.train()
            if vloss < best:
                best, best_state, best_step = vloss, copy.deepcopy(model.state_dict()), step
            if step % 1000 == 0 or step == steps - 1:
                log(f"    {name} step {step:5d}: training {loss.item():.3f}, validation {vloss:.3f} (best {best:.3f} at {best_step})")
    model.load_state_dict(best_state); model.eval()
    with torch.no_grad():
        z = model.mu(model.enc(x)).cpu().numpy()
    return model, z


def decode(model, zs):
    with torch.no_grad():
        return model.decode(torch.tensor(np.asarray(zs, float), dtype=torch.float32, device=DEVICE)).cpu().numpy() * SCALE


def mirrored_layers(coefs, order, nfft, fs, az, el):
    """The notch layers of the heads mirrored left–right: sine-type SH coefficients (m < 0) change sign."""
    sign = np.array([1.0 if m >= 0 else -1.0 for n in range(order + 1) for m in range(-n, n + 1)])
    full = ex.dtf(coefs * sign[None, :, None], order, nfft, fs, az, el)
    return ex.split(full, pm.W)[1]


def depth_of(layer, med):
    x = layer[..., med, :][..., NOTCH_K]
    return x.max(-1) - x.min(-1)                                         # per median-plane direction


def profiles_t(cand):
    smooth, band = pm.smoothing_kernel(), torch.tensor(np.where(ex.BAND)[0], device=DEVICE)
    with torch.no_grad():
        return pm.torch_profile(torch.tensor(cand, dtype=torch.float32, device=DEVICE), smooth, band)


def evaluate(name, grid_patterns, grid_shape, shifts, base, heads, own, med):
    """grid_patterns: G x dirs x ERB over the pad's free inputs; shifts: the shift values to combine (or [0])."""
    cand_prof = torch.cat([profiles_t(base[None] + pm.shift(grid_patterns, s)) for s in shifts])     # (S·G) x dirs x bands
    smooth, band = pm.smoothing_kernel(), torch.tensor(np.where(ex.BAND)[0], device=DEVICE)
    dists, depth_ratio, picks = [], [], []
    for x, o in zip(heads, own):
        with torch.no_grad():
            ph = pm.torch_profile(torch.tensor(x[None], dtype=torch.float32, device=DEVICE), smooth, band)[0]
            d = torch.sqrt(torch.mean((cand_prof - ph) ** 2, dim=(-2, -1))).cpu().numpy()
        i = int(np.argmin(d))
        si, gi = divmod(i, len(grid_patterns))
        gen = pm.shift(grid_patterns[gi], shifts[si])
        dists.append(d[i]); picks.append((gi, si))
        depth_ratio.append(np.median(depth_of(gen, med)) / max(np.median(depth_of(o, med)), 1e-6))
    # Evenness: perceptual steps between neighbouring grid points of the free inputs.
    gp = grid_patterns.reshape(grid_shape + grid_patterns.shape[1:])
    steps = []
    if len(grid_shape) == 1:
        steps = [pm.dist(base + gp[i], base + gp[i + 1]) for i in range(grid_shape[0] - 1)]
    else:
        for i in range(grid_shape[0]):
            for j in range(grid_shape[1]):
                if i + 1 < grid_shape[0]: steps.append(pm.dist(base + gp[i, j], base + gp[i + 1, j]))
                if j + 1 < grid_shape[1]: steps.append(pm.dist(base + gp[i, j], base + gp[i, j + 1]))
    steps = np.array(steps)
    return dict(name=name, dist=np.array(dists), depth=np.array(depth_ratio), even=steps.max() / np.median(steps),
                grid_depth=float(np.median(depth_of(grid_patterns, med))), picks=picks)


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
    mir_all = mirrored_layers(coefs, order, nfft, fs, az, el)
    perm = np.random.default_rng(0).permutation(len(full))
    n_test = int(round(pm.TEST_SHARE * len(full)))
    test_i, train_i = perm[:n_test], perm[n_test:]
    base = base_all[train_i].mean(0)
    log(f"{len(train_i)} heads train (and {len(train_i)} mirrored), {len(test_i)} test; device {DEVICE}")
    log(f"real heads' notch depth (median plane, 5–12 kHz, median): {np.median(depth_of(layer_all[train_i], med)):.1f} dB")

    template, s_train = pm.align_all(layer_all[train_i])
    aligned = np.stack([pm.shift(x, s) for x, s in zip(layer_all[train_i], s_train)])
    s_mir = pm.estimate_shifts(mir_all[train_i], template)
    aligned_mir = np.stack([pm.shift(x, s) for x, s in zip(mir_all[train_i], s_mir)])
    heads, own = full[test_i], layer_all[test_i]

    results, models = [], {}
    # V0 — the reference, as pattern-models.py.
    log("V0: one free input and the shift, as before")
    m0, z0 = train(aligned, base, 1, 64, 3000, 0.05, False, 0, "V0")
    zs = np.linspace(np.percentile(z0, 1) - 0.5, np.percentile(z0, 99) + 0.5, 41)[:, None]
    results.append(evaluate("V0  1 free + shift (reference)", decode(m0, zs), (41,), pm.SHIFTS, base, heads, own, med))
    # V1 — improved, mirrored.
    log("V1: one free input and the shift, improved, mirrored")
    m1, z1 = train(np.concatenate([aligned, aligned_mir]), base, 1, 128, 8000, 0.005, True, 0, "V1")
    zs = np.linspace(np.percentile(z1, 1) - 0.5, np.percentile(z1, 99) + 0.5, 41)[:, None]
    results.append(evaluate("V1  1 free + shift, improved", decode(m1, zs), (41,), pm.SHIFTS, base, heads, own, med))
    models["V1"] = (m1, zs)
    # V2 — two free inputs, shifts left in, mirrored, randomly shifted.
    log("V2: two free inputs, no shift")
    m2, z2 = train(np.concatenate([layer_all[train_i], mir_all[train_i]]), base, 2, 128, 8000, 0.005, True, 1.5, "V2")
    g = [np.linspace(np.percentile(z2[:, k], 1) - 0.5, np.percentile(z2[:, k], 99) + 0.5, 25) for k in range(2)]
    zz = np.stack(np.meshgrid(g[0], g[1], indexing="ij"), -1).reshape(-1, 2)
    results.append(evaluate("V2  2 free inputs", decode(m2, zz), (25, 25), np.array([0.0]), base, heads, own, med))
    models["V2"] = (m2, zz)

    ref = np.array([pm.dist(ku, x) for x in heads])
    log(f"\nheld-out heads ({len(test_i)}), auditory measure dB/ERB, median; the KU100 as it is: {np.median(ref):.3f}")
    for r in results:
        log(f"  {r['name']:32s} distance {np.median(r['dist']):.3f} ({100 * (1 - np.median(r['dist']) / np.median(ref)):+.0f} % against the KU100); "
            f"notch depth at the best place {100 * np.median(r['depth']):.0f} % of the head's own; "
            f"pad evenness: largest step {r['even']:.1f}× the median; depth over the pad {r['grid_depth']:.1f} dB")
    out = os.path.splitext(path)[0] + "-models2.pt"
    torch.save({k: (v[0].state_dict(), v[1]) for k, v in models.items()}, out)
    np.savez_compressed(os.path.splitext(path)[0] + "-models2.npz", base=base.astype(np.float32), az=az, el=el, erb=ex.ERB, freq=ex.FREQ,
                        v1_grid=decode(*models["V1"]).astype(np.float32), v1_z=models["V1"][1],
                        v2_grid=decode(*models["V2"]).astype(np.float32), v2_z=models["V2"][1],
                        v2_fitted=np.array([models["V2"][1][p[0]] for p in results[2]["picks"]]))
    log(f"saved the models ({time.time() - t0:.0f} s)")


if __name__ == "__main__":
    main(sys.argv[1])
