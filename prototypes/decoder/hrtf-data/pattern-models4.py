"""Round four: the decoder outputs spherical harmonics, and the deepest notch's depth is part of the loss.

stages.py found the notches widened by the export (the pattern fitted with order-8 SH on 300 directions: +50 % width)
and by the renderer (+25 %), and made shallower by the model itself (71 % of a head's depth). Here, V2b's settings
(two free inputs, mirrored, randomly shifted ±1.5 ERB, KL weight 0.02, symmetric depth term), and:
  - the decoder's last layer is SH coefficients of order 20 for every point of the ERB axis — no fit afterwards, any
    direction exact; the loss is taken on 961 directions (900 over the sphere, 61 in the median plane), more than
    the 441 coefficients, so none is left undetermined
  - a term for the depth of the deepest notch of 4–14 kHz in every direction (a soft minimum of the notch layer,
    0.5 dB wide), against the head's, as stages.py measures it
The data in SH throughout: a head's DTF coefficients on the ERB axis, the 3-ERB split taken on the coefficients (a
smoothing along frequency, so it commutes with directions), mirrored by changing the sign of the sine-type ones.

Saves the decoder, its latent range and the held-out heads' best places for export-decoder.py.

  python pattern-models4.py <data set .npz>
"""
import os, sys, time, copy
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np
import torch
from scipy.ndimage import gaussian_filter1d

_d = os.path.dirname(os.path.abspath(__file__))
_s = spec_from_file_location("p2", os.path.join(_d, "pattern-models2.py")); p2 = module_from_spec(_s); _s.loader.exec_module(p2)
pm, ex, ge, DEVICE, log = p2.pm, p2.ex, p2.ge, p2.DEVICE, p2.log
torch.manual_seed(0)
ORDER, NC = 20, 441
SCALE = 5.0
NOTCH_B = (ex.FREQ >= 4000) & (ex.FREQ <= 14000)


def directions():
    i = np.arange(900) + 0.5
    saz, sel = np.pi * (3 - np.sqrt(5)) * i, np.arcsin(1 - 2 * i / 900)
    polar = np.arange(-45, 226, 4.5)
    maz = np.where(polar <= 90, 0.0, np.pi)
    mel = np.radians(np.where(polar <= 90, polar, 180 - polar))
    return np.r_[saz, maz], np.r_[sel, mel], np.arange(900, 900 + len(polar)), polar


def dtf_coefs(coefs, order, nfft, fs):
    """The CLL's DTF as SH coefficients on the ERB axis: heads x 441 x E (the order-0 term, the average, removed)."""
    nb = nfft // 2 + 1
    fr = np.fft.rfftfreq(nfft, 1 / fs)
    c = coefs[:, :, :nb].copy()
    c[:, 0, :] = 0
    idx = np.searchsorted(fr, ex.FREQ)
    w = (ex.FREQ - fr[idx - 1]) / (fr[idx] - fr[idx - 1])
    return c[..., idx - 1] * (1 - w) + c[..., idx] * w


def split_coefs(c):
    base = gaussian_filter1d(c, 3 / 2.355 / ex.STEP, axis=-1, mode="nearest")
    return base, c - base


class SHNet(p2.Net):
    """p2.Net with the decoder's last layer giving NC SH coefficients per ERB point; encoder on directions."""
    def __init__(self, channels, length, latent, hidden):
        super().__init__(channels, length, latent, hidden)
        self.dec[-1] = torch.nn.Conv1d(hidden, NC, 5, padding=2)


def train(x_dirs, x_coefs, y, base_dirs, steps, beta, aug, name):
    """x_dirs: heads x dirs x E (encoder input and loss target, notch layer); y: dirs x NC."""
    rng = np.random.default_rng(1)
    idx = rng.permutation(len(x_dirs))
    n_val = max(8, len(x_dirs) // 10)
    val, tr = idx[:n_val], idx[n_val:]
    x = torch.tensor(x_dirs / SCALE, dtype=torch.float32, device=DEVICE)
    Y = torch.tensor(y, dtype=torch.float32, device=DEVICE)
    b = torch.tensor(base_dirs / SCALE, dtype=torch.float32, device=DEVICE)
    smooth, band = pm.smoothing_kernel(), torch.tensor(np.where(ex.BAND)[0], device=DEVICE)
    k_mask = torch.tensor(p2.NOTCH_K, device=DEVICE)
    nb_mask = torch.tensor(NOTCH_B, device=DEVICE)
    model = SHNet(x.shape[1], x.shape[2], 2, 128).to(DEVICE)
    opt = torch.optim.Adam(model.parameters(), lr=1e-3)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, steps)

    def run(xb, sample=True):
        h = model.enc(xb)
        mu, logvar = model.mu(h), model.logvar(h).clamp(-8, 4)
        z = mu + torch.randn_like(mu) * torch.exp(0.5 * logvar) if sample else mu
        c = model.decode(z)                                                   # batch x NC x E
        return torch.einsum("dc,bce->bde", Y, c), mu, logvar

    def loss_of(rec, xb):
        l = p2.loss_fn(rec, xb, b, smooth, band, k_mask, "both")
        tau = 0.5 / SCALE                                                     # soft minimum, 0.5 dB wide
        dr = tau * torch.logsumexp(-rec[..., nb_mask] / tau, -1)
        dx = tau * torch.logsumexp(-xb[..., nb_mask] / tau, -1)
        return l + 0.05 * torch.mean(((dr - dx) * SCALE) ** 2)

    best, best_state, best_step = np.inf, None, 0
    xtr, xval = x[tr], x[val]
    for step in range(steps):
        xb = xtr
        if aug:
            xb = p2.torch_shift(xb, (torch.rand(len(xb), device=DEVICE) * 2 - 1) * aug)
        rec, mu, logvar = run(xb)
        kl = -0.5 * torch.mean(1 + logvar - mu ** 2 - logvar.exp())
        loss = loss_of(rec, xb) + beta * min(1.0, step / 1000) * kl
        opt.zero_grad(); loss.backward(); opt.step(); sched.step()
        if step % 100 == 0 or step == steps - 1:
            model.eval()
            with torch.no_grad():
                vloss = loss_of(run(xval, False)[0], xval).item()
            model.train()
            if vloss < best:
                best, best_state, best_step = vloss, copy.deepcopy(model.state_dict()), step
            if step % 500 == 0 or step == steps - 1:
                log(f"    {name} step {step:5d}: training {loss.item():.3f}, validation {vloss:.3f} (best {best:.3f} at {best_step})")
    model.load_state_dict(best_state); model.eval()
    with torch.no_grad():
        z = model.mu(model.enc(x)).cpu().numpy()
    return model, z


def decode_coefs(model, zs):
    with torch.no_grad():
        return model.decode(torch.tensor(np.asarray(zs, float), dtype=torch.float32, device=DEVICE)).cpu().numpy() * SCALE


def main(path):
    t0 = time.time()
    data = np.load(path)
    order, nfft, fs = int(data["order"]), int(data["nfft"]), int(data["fs"])
    az, el, med, polar = directions()
    y = ge.sh_orth(ORDER, az, el)
    dc = dtf_coefs(data["cll_ild"].astype(float), order, nfft, fs)            # heads x 441 x E
    bc, lc = split_coefs(dc)
    sign = np.array([1.0 if m >= 0 else -1.0 for n in range(ORDER + 1) for m in range(-n, n + 1)])
    perm = np.random.default_rng(0).permutation(len(dc))
    n_test = int(round(pm.TEST_SHARE * len(dc)))
    test_i, train_i = perm[:n_test], perm[n_test:]
    base_c = bc[train_i].mean(0)                                              # 441 x E
    base = y @ base_c
    lc_train = np.concatenate([lc[train_i], lc[train_i] * sign[None, :, None]])
    x_train = np.einsum("dc,hce->hde", y, lc_train)
    full_test = np.einsum("dc,hce->hde", y, dc[test_i])
    own_test = np.einsum("dc,hce->hde", y, lc[test_i])
    log(f"{len(train_i)} heads train (and mirrored), {len(test_i)} test; {len(az)} directions, order {ORDER}; device {DEVICE}")

    log("V4: two free inputs, SH output, notch-depth loss")
    model, z = train(x_train, lc_train, y, base, 2000, 0.02, 1.5, "V4")
    g = [np.linspace(np.percentile(z[:, k], 1) - 0.5, np.percentile(z[:, k], 99) + 0.5, 25) for k in range(2)]
    zz = np.stack(np.meshgrid(g[0], g[1], indexing="ij"), -1).reshape(-1, 2)
    grid = np.einsum("dc,gce->gde", y, decode_coefs(model, zz))
    r = p2.evaluate("V4", grid, (25, 25), np.array([0.0]), base, full_test, own_test, med)
    ku = np.einsum("dc,ce->de", y, dtf_coefs(data["ku100_cll_ild"].astype(float)[None], order, nfft, fs)[0])
    ref = np.array([pm.dist(ku, x) for x in full_test])
    log(f"\nheld-out heads ({len(test_i)}): the KU100 {np.median(ref):.3f}; V4 {np.median(r['dist']):.3f} "
        f"({100 * (1 - np.median(r['dist']) / np.median(ref)):+.0f} %); notch depth at the best place "
        f"{100 * np.median(r['depth']):.0f} % of the head's own (5–12 kHz range); largest step {r['even']:.1f}× the median")
    fitted = np.array([zz[p[0]] for p in r["picks"]])
    torch.save({"state": model.state_dict(), "lo": zz[0], "hi": zz[-1], "fitted": fitted, "test": test_i,
                "base_c": base_c.astype(np.float32)}, os.path.splitext(path)[0] + "-model4.pt")
    log(f"saved ({time.time() - t0:.0f} s)")


if __name__ == "__main__":
    main(sys.argv[1])
