"""Two inputs that generate a whole set of vertical cues: a free pattern input and a shift.

The CLL's DTF of every direction is split at 3 ERB (extract.py) into a baseline and a notch layer. The baseline is
fixed: the population's average. The notch layer comes from the two inputs:
  shift s    moves the notch pattern along the ERB axis — a fixed operation, not learned, so it can only shift
  pattern    which notch pattern, out of what the heads have: a free axis, learned from the heads' notch layers with
             their own shift taken out (each aligned to a template), so it can only be shape
Two ways to make the pattern axis:
  A    spectral ordering: the heads' shift-free patterns put on a line by perceptual similarity (the second
       eigenvector of their similarity graph), spaced by the distance between neighbours, and a position between two
       neighbours made by aligning the pair and blending
  VAE  a variational autoencoder with a one-dimensional latent, trained on the shift-free patterns with the auditory
       measure as its loss; its axis respaced afterwards by perceptual distance, so equal steps are equal changes
Both are fitted to held-out heads: each head's best (pattern, shift), and its distance from the head's own whole DTF,
against the KU100, the KU100's notch layer on the population baseline (unshifted, and at its best shift), and the
best any pattern can do here — the head's own shift-free pattern at its best shift.

The measure: positive gradients across ERB bands of the DTF smoothed to about one ERB, 2–16 kHz (extract.py), as
RMS over directions and bands, dB per ERB.

  python pattern-models.py <data set .npz>
"""
import os, sys, time
from importlib.util import spec_from_file_location, module_from_spec
import numpy as np
import torch
import plotly.graph_objects as go
from plotly.subplots import make_subplots

_d = os.path.dirname(os.path.abspath(__file__))
_s = spec_from_file_location("ex", os.path.join(_d, "extract.py")); ex = module_from_spec(_s); _s.loader.exec_module(ex)
ge = ex.ge
W = 3
SHIFTS = np.arange(-3, 3.001, 0.25)                                    # ERB
TEST_SHARE = 0.25
torch.manual_seed(0)
# The Apple GPU when there is one: a training step there takes 6 ms against 2.3 s on six CPU cores.
DEVICE = "mps" if torch.backends.mps.is_available() else "cpu"


def log(*a):
    print(*a, flush=True)


# ---------- directions, data ----------
def directions():
    i = np.arange(300) + 0.5
    saz, sel = np.pi * (3 - np.sqrt(5)) * i, np.arcsin(1 - 2 * i / 300)
    polar = np.arange(-45, 226, 4.5)
    maz = np.where(polar <= 90, 0.0, np.pi)
    mel = np.radians(np.where(polar <= 90, polar, 180 - polar))
    return np.r_[saz, maz], np.r_[sel, mel], np.arange(300, 300 + len(polar)), polar


def shift(x, s):
    """x read at ERB + s along its last axis: a feature at e moves to e − s. s a number or one per leading row."""
    s = np.broadcast_to(np.asarray(s, float), x.shape[:-1]).reshape(-1, 1)
    n = x.shape[-1]
    pos = np.clip(np.arange(n)[None, :] + s / ex.STEP, 0, n - 1)
    i = np.minimum(pos.astype(int), n - 2)
    f = pos - i
    xf = x.reshape(-1, n)
    r = np.arange(len(xf))[:, None]
    return (xf[r, i] * (1 - f) + xf[r, i + 1] * f).reshape(x.shape)


def profile(x):
    return ex.profile(x)


def dist(a, b):
    """RMS over directions and bands of the profile difference; b may have leading dims (broadcast)."""
    return np.sqrt(np.mean((profile(a) - profile(b)) ** 2, axis=(-2, -1)))


def estimate_shifts(layers, template):
    out = []
    for x in layers:
        e = [np.mean((shift(x, s) - template)[:, ex.BAND] ** 2) for s in SHIFTS]
        out.append(SHIFTS[int(np.argmin(e))])
    return np.array(out)


def align_all(layers, iterations=5):
    template, s = layers.mean(0), np.zeros(len(layers))
    for _ in range(iterations):
        s = estimate_shifts(layers, template)
        s -= np.round(s.mean() / 0.25) * 0.25
        template = np.stack([shift(x, v) for x, v in zip(layers, s)]).mean(0)
    return template, s


# ---------- A: spectral ordering ----------
class Ordering:
    def __init__(self, patterns, base):
        self.p = patterns
        n = len(patterns)
        full = base[None] + patterns
        D = np.stack([dist(full[i], full) for i in range(n)])
        sigma = np.median(D[D > 0])
        Wm = np.exp(-(D / sigma) ** 2)
        L = np.diag(Wm.sum(1)) - Wm
        vals, vecs = np.linalg.eigh(L)
        self.order = np.argsort(vecs[:, 1])
        steps = np.array([D[a, b] for a, b in zip(self.order[:-1], self.order[1:])])
        self.t = np.r_[0, np.cumsum(steps)]                             # positions, spaced by neighbour distance
        self.t /= self.t[-1]
        # Each neighbour pair's own alignment, so the blend between them does not smear.
        self.pair_shift = np.array([estimate_shifts(patterns[b][None], patterns[a])[0] for a, b in zip(self.order[:-1], self.order[1:])])

    def __call__(self, t):
        t = float(np.clip(t, 0, 1))
        k = min(int(np.searchsorted(self.t, t, side="right")) - 1, len(self.order) - 2)
        a = (t - self.t[k]) / max(self.t[k + 1] - self.t[k], 1e-9)
        pa, pb, d = self.p[self.order[k]], self.p[self.order[k + 1]], self.pair_shift[k]
        # pb aligned onto pa is shift(pb, d); move both a fraction of the way towards each other, then blend.
        return (1 - a) * shift(pa, -a * d) + a * shift(pb, (1 - a) * d)


# ---------- the VAE ----------
class VAE(torch.nn.Module):
    def __init__(self, channels, length, hidden=64):
        super().__init__()
        self.length = length
        self.enc = torch.nn.Sequential(
            torch.nn.Conv1d(channels, hidden, 5, stride=2, padding=2), torch.nn.GELU(),
            torch.nn.Conv1d(hidden, hidden, 5, stride=2, padding=2), torch.nn.GELU(), torch.nn.Flatten())
        n = hidden * ((length + 3) // 4)
        self.mu, self.logvar = torch.nn.Linear(n, 1), torch.nn.Linear(n, 1)
        self.l0 = (length + 3) // 4
        self.dec_in = torch.nn.Linear(1, hidden * self.l0)
        self.dec = torch.nn.Sequential(
            torch.nn.GELU(),
            torch.nn.ConvTranspose1d(hidden, hidden, 4, stride=2, padding=1), torch.nn.GELU(),
            torch.nn.ConvTranspose1d(hidden, hidden, 4, stride=2, padding=1), torch.nn.GELU(),
            torch.nn.Conv1d(hidden, channels, 5, padding=2))
        self.hidden = hidden

    def decode(self, z):
        h = self.dec_in(z).view(len(z), self.hidden, self.l0)
        return self.dec(h)[..., :self.length]

    def forward(self, x):
        h = self.enc(x)
        mu, logvar = self.mu(h), self.logvar(h).clamp(-8, 4)
        z = mu + torch.randn_like(mu) * torch.exp(0.5 * logvar)
        return self.decode(z), mu, logvar


def torch_profile(x, smooth, band):
    s = torch.nn.functional.conv1d(x.reshape(-1, 1, x.shape[-1]), smooth, padding=smooth.shape[-1] // 2).reshape(x.shape)
    s = s[..., band]
    return torch.relu(s[..., 1:] - s[..., :-1]) / ex.STEP


def smoothing_kernel():
    sig = 1 / 2.355 / ex.STEP
    k = np.arange(-int(4 * sig), int(4 * sig) + 1)
    g = np.exp(-0.5 * (k / sig) ** 2)
    return torch.tensor(g / g.sum(), dtype=torch.float32, device=DEVICE).view(1, 1, -1)


def train_vae(patterns, base, steps=3000, scale=5.0):
    x = torch.tensor(patterns / scale, dtype=torch.float32, device=DEVICE)
    b = torch.tensor(base / scale, dtype=torch.float32, device=DEVICE)
    smooth = smoothing_kernel()
    band = torch.tensor(np.where(ex.BAND)[0], device=DEVICE)
    model = VAE(x.shape[1], x.shape[2]).to(DEVICE)
    opt = torch.optim.Adam(model.parameters(), lr=1e-3)
    pt = torch_profile(b + x, smooth, band)                             # the targets' profiles do not change
    for step in range(steps):
        rec, mu, logvar = model(x)
        pr = torch_profile(b + rec, smooth, band)
        loss_p = torch.mean((pr - pt) ** 2) * scale ** 2
        loss_m = torch.mean((rec - x) ** 2) * scale ** 2
        kl = -0.5 * torch.mean(1 + logvar - mu ** 2 - logvar.exp())
        beta = 0.05 * min(1.0, step / 1000)
        loss = loss_p + 0.05 * loss_m + beta * kl
        opt.zero_grad(); loss.backward(); opt.step()
        if step % 500 == 0 or step == steps - 1:
            log(f"    step {step:4d}: profile {loss_p.item():.3f}, mse {loss_m.item():.2f}, kl {kl.item():.3f}")
    model.eval()
    with torch.no_grad():
        mu = model.mu(model.enc(x)).cpu().numpy()[:, 0]
    return model, mu, scale


class VAEAxis:
    """The VAE's decoder on a respaced axis: t in 0…1, equal steps of t equal steps of perceptual distance."""
    def __init__(self, model, mu, scale, base):
        self.model, self.scale = model, scale
        zs = np.linspace(np.percentile(mu, 1) - 0.5, np.percentile(mu, 99) + 0.5, 201)
        pats = self.decode_z(zs)
        steps = np.array([dist(base + pats[i], base + pats[i + 1]) for i in range(len(zs) - 1)])
        self.zs, self.t = zs, np.r_[0, np.cumsum(steps)] / np.sum(steps)
        self.steps = steps
        self.train_t = np.interp(mu, zs, self.t)

    def decode_z(self, zs):
        with torch.no_grad():
            z = torch.tensor(np.asarray(zs, float)[:, None], dtype=torch.float32, device=DEVICE)
            return self.model.decode(z).cpu().numpy() * self.scale

    def __call__(self, t):
        return self.decode_z([np.interp(t, self.t, self.zs)])[0]


# ---------- fitting ----------
def candidates(generator, ts, base):
    """Every (t, s) of the grid as a whole DTF, profiled on the GPU once: S x T x dirs x bands−1."""
    pats = np.stack([generator(t) for t in ts])                         # T x dirs x ERB
    cand = np.stack([base[None] + shift(pats, s) for s in SHIFTS])      # S x T x dirs x ERB
    smooth, band = smoothing_kernel(), torch.tensor(np.where(ex.BAND)[0], device=DEVICE)
    with torch.no_grad():
        return torch_profile(torch.tensor(cand, dtype=torch.float32, device=DEVICE), smooth, band)


def fit(head, cand_profiles, ts):
    """Best (t, s) for a head's whole DTF against the profiled grid; returns (distance, t, s)."""
    smooth, band = smoothing_kernel(), torch.tensor(np.where(ex.BAND)[0], device=DEVICE)
    with torch.no_grad():
        ph = torch_profile(torch.tensor(head[None], dtype=torch.float32, device=DEVICE), smooth, band)[0]
        d = torch.sqrt(torch.mean((cand_profiles - ph) ** 2, dim=(-2, -1))).cpu().numpy()      # S x T
    si, ti = np.unravel_index(int(np.argmin(d)), d.shape)
    return float(d[si, ti]), float(ts[ti]), float(SHIFTS[si])


def depth(layers, med):
    """Mean notch depth (max − min of the notch layer, 5–12 kHz) over the median-plane directions."""
    k = (ex.FREQ >= 5000) & (ex.FREQ <= 12000)
    x = layers[..., med, :][..., k]
    return float(np.mean(x.max(-1) - x.min(-1)))


def main(path):
    t0 = time.time()
    data = np.load(path)
    az, el, med, polar = directions()
    full = ex.dtf(data["cll_ild"].astype(float), int(data["order"]), int(data["nfft"]), int(data["fs"]), az, el)
    ku = ex.dtf(data["ku100_cll_ild"].astype(float)[None], int(data["order"]), int(data["nfft"]), int(data["fs"]), az, el)[0]
    base_all, layer_all = ex.split(full, W)
    ku_base, ku_layer = ex.split(ku, W)
    perm = np.random.default_rng(0).permutation(len(full))
    n_test = int(round(TEST_SHARE * len(full)))
    test, train = perm[:n_test], perm[n_test:]
    base = base_all[train].mean(0)                                      # the fixed population baseline
    template, s_train = align_all(layer_all[train])
    patterns = np.stack([shift(x, s) for x, s in zip(layer_all[train], s_train)])
    print(f"{len(train)} heads train, {len(test)} test; {len(az)} directions, {len(ex.ERB)} ERB points. "
          f"Training heads' shift: sd {np.std(s_train):.2f} ERB")

    log("A: spectral ordering")
    A = Ordering(patterns, base)
    log("VAE: training")
    model, mu, scale = train_vae(patterns, base)
    V = VAEAxis(model, mu, scale, base)

    ts = np.linspace(0, 1, 61)
    rows = {k: [] for k in ("KU100 as it is", "KU100 notch layer on population baseline", "  … at its best shift",
                             "A (best pattern, shift)", "VAE (best pattern, shift)", "own pattern at its best shift (the limit)")}
    fits = {"A": [], "VAE": []}
    log("profiling the candidates")
    cA, cV = candidates(A, ts, base), candidates(V, ts, base)
    log("fitting the held-out heads")
    for h in test:
        x = full[h]
        rows["KU100 as it is"].append(float(dist(ku, x)))
        rows["KU100 notch layer on population baseline"].append(float(dist(base + ku_layer, x)))
        rows["  … at its best shift"].append(min(float(dist(base + shift(ku_layer, s), x)) for s in SHIFTS))
        dA = fit(x, cA, ts); dV = fit(x, cV, ts)
        rows["A (best pattern, shift)"].append(dA[0]); rows["VAE (best pattern, shift)"].append(dV[0])
        fits["A"].append(dA[1:]); fits["VAE"].append(dV[1:])
        own = layer_all[h]
        rows["own pattern at its best shift (the limit)"].append(min(float(dist(base + shift(own, s), x)) for s in SHIFTS))
    ref = np.array(rows["KU100 as it is"])
    print(f"\nheld-out heads, distance from the head's own DTF (auditory measure, dB/ERB; median over {len(test)} heads):")
    for k, v in rows.items():
        v = np.array(v)
        print(f"  {k:44s} {np.median(v):.3f}   {100 * (1 - np.median(v) / np.median(ref)):+4.0f} % against the KU100; "
              f"closer than the KU100 for {100 * np.mean(v < ref):.0f} % of heads")

    print("\nsharpness — notch depth, median plane 5–12 kHz (dB):")
    print(f"  real heads {depth(layer_all, med):.1f}; the KU100 {depth(ku_layer[None], med):.1f}; "
          f"A along its axis {depth(np.stack([A(t) for t in ts]), med):.1f}; VAE along its axis {depth(np.stack([V(t) for t in ts]), med):.1f}")
    print("smoothness — perceptual step between neighbouring points of the axis, 61 even steps (dB/ERB):")
    for name, g in (("A", A), ("VAE", V)):
        pats = np.stack([g(t) for t in ts])
        st = np.array([dist(base + pats[i], base + pats[i + 1]) for i in range(len(ts) - 1)])
        print(f"  {name}: median {np.median(st):.3f}, largest {st.max():.3f} ({st.max() / np.median(st):.1f}× the median); "
              f"whole axis end to end {dist(base + pats[0], base + pats[-1]):.2f}")
    for name in fits:
        f = np.array(fits[name])
        print(f"  {name}: held-out heads' best pattern spread over t {np.percentile(f[:, 0], 5):.2f}…{np.percentile(f[:, 0], 95):.2f}; "
              f"pattern and shift correlate at r = {np.corrcoef(f[:, 0], f[:, 1])[0, 1]:+.2f}")
    print(f"\n({time.time() - t0:.0f} s)")
    plot(A, V, base, polar, med, rows, fits)


def plot(A, V, base, polar, med, rows, fits):
    out = ge.OUT
    ts = [0, 1 / 6, 2 / 6, 3 / 6, 4 / 6, 5 / 6, 1]
    fig = make_subplots(rows=2, cols=len(ts), shared_xaxes=True, shared_yaxes=True, horizontal_spacing=0.01, vertical_spacing=0.06,
                        subplot_titles=[f"A t={t:.2f}" for t in ts] + [f"VAE t={t:.2f}" for t in ts])
    for r, g in enumerate((A, V)):
        for c, t in enumerate(ts):
            fig.add_trace(go.Heatmap(x=ex.FREQ, y=polar, z=g(t)[med], zmin=-10, zmax=10, colorscale="RdBu", showscale=False),
                          row=r + 1, col=c + 1)
    fig.update_xaxes(type="log", range=[np.log10(2000), np.log10(18000)], showticklabels=False)
    fig.update_yaxes(showticklabels=False)
    fig.update_layout(title="The pattern axis: median-plane notch layer (polar angle up, frequency right) at seven points — A above, VAE below",
                      height=620, template="plotly_white")
    fig.write_html(os.path.join(out, "patterns-axis.html"), include_plotlyjs="cdn")

    fig = make_subplots(rows=1, cols=2, subplot_titles=["distance from a held-out head", "held-out heads' best (pattern, shift)"])
    names = list(rows)
    fig.add_trace(go.Box(y=sum((rows[n] for n in names), []), x=sum(([n.strip()] * len(rows[n]) for n in names), []),
                         marker_color="#1b1a17", boxpoints=False), row=1, col=1)
    for name, col in (("A", "#5e8bae"), ("VAE", "#d4873a")):
        f = np.array(fits[name])
        fig.add_trace(go.Scatter(x=f[:, 0], y=f[:, 1], mode="markers", name=name, marker=dict(color=col, size=6, opacity=0.7)), row=1, col=2)
    fig.update_yaxes(title_text="dB per ERB", row=1, col=1)
    fig.update_xaxes(title_text="pattern t", row=1, col=2); fig.update_yaxes(title_text="shift, ERB", row=1, col=2)
    fig.update_layout(height=520, template="plotly_white")
    fig.write_html(os.path.join(out, "patterns-fit.html"), include_plotlyjs="cdn")


if __name__ == "__main__":
    main(sys.argv[1])
