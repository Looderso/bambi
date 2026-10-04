// From a measured head to a few sheets of notches over the sphere — what the sheets page's worker runs: the head's
// analysis (CLL, ILD, ITD), the CLL prepared, split into a smooth part and DIPS dips a direction (fitted together, blur
// WIDTH ERB, dips between FROM and TOP Hz), the dips linked into tracks, gathered into surfaces, and fitted to the
// measured CLL again. Needs common.js (shN, dirOf, D2R).
let FS = 48000, TAPS = 256;
const WIDTH = 3, DIPS = 4, FROM = 4000, TOP = 18000;

// ---------- the auditory axis: ERB number, 0.3–20 kHz in steps of 0.1 ERB ----------
const erbOf = f => 21.4 * Math.log10(1 + 0.00437 * f), fOf = e => (10 ** (e / 21.4) - 1) / 0.00437;
const STEP = 0.1, E0 = erbOf(300), NF = Math.floor((erbOf(20000) - E0) / STEP) + 1, E1 = E0 + (NF - 1) * STEP;
const FREQ = Float32Array.from({ length: NF }, (_, k) => fOf(E0 + k * STEP));
const inBand = (lo, hi) => { const k = []; FREQ.forEach((f, i) => { if (f >= lo && f <= hi) k.push(i); }); return k; };
const NOTCHES = inBand(4000, 16000), WIDE = inBand(1000, 16000);
const LO = Math.round((erbOf(FROM) - E0) / STEP), HI = Math.round((erbOf(TOP) - E0) / STEP);   // the dips' band on the axis

// ---------- transforms ----------
function fft(re, im, inverse) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) { let bit = n >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit;
    if (i < j) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; } }
  for (let len = 2; len <= n; len <<= 1) { const a = 2 * Math.PI / len * (inverse ? 1 : -1), wr = Math.cos(a), wi = Math.sin(a);
    for (let i = 0; i < n; i += len) { let cr = 1, ci = 0;
      for (let k = 0; k < len / 2; k++) { const p = i + k, q = p + len / 2;
        const xr = re[q] * cr - im[q] * ci, xi = re[q] * ci + im[q] * cr;
        re[q] = re[p] - xr; im[q] = im[p] - xi; re[p] += xr; im[p] += xi;
        const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t; } } }
  if (inverse) for (let i = 0; i < n; i++) { re[i] /= n; im[i] /= n; }
}
function spectrumOf(x, n) { const re = new Float64Array(n), im = new Float64Array(n); re.set(x.subarray(0, Math.min(n, x.length))); fft(re, im); return [re, im]; }
// A response's power in dB on the axis, from its n-point spectrum.
function onAxis(re, im, n, out, o) {
  for (let k = 0; k < NF; k++) { const b = FREQ[k] * n / FS, i = Math.floor(b), t = b - i;
    const p0 = 10 * Math.log10(Math.max(re[i] * re[i] + im[i] * im[i], 1e-12)), p1 = 10 * Math.log10(Math.max(re[i + 1] * re[i + 1] + im[i + 1] * im[i + 1], 1e-12));
    out[o + k] = p0 + (p1 - p0) * t; }
}
// tL − tR in seconds: where the two ears' cross-correlation, low-passed at 1.5 kHz, peaks; to an eighth of a sample.
const ITD_N = 512, ITD_UP = 8, ITD_W = Float64Array.from({ length: ITD_N / 2 + 1 }, (_, k) => 1 / (1 + (k * FS / ITD_N / 1500) ** 8) ** 2);
function itdOf(lr, li, rr, ri) {
  const m = ITD_N * ITD_UP, re = new Float64Array(m), im = new Float64Array(m);
  for (let k = 0; k <= ITD_N / 2; k++) { const cr = (lr[k] * rr[k] + li[k] * ri[k]) * ITD_W[k], ci = (li[k] * rr[k] - lr[k] * ri[k]) * ITD_W[k];
    re[k] = cr; im[k] = ci; if (k > 0 && k < ITD_N / 2) { re[m - k] = cr; im[m - k] = -ci; } }
  fft(re, im, true);
  let best = 0, lag = 0;
  for (let l = -80 * ITD_UP; l <= 80 * ITD_UP; l++) { const v = re[(l + m) % m]; if (v > best) { best = v; lag = l; } }
  return lag / ITD_UP / FS;
}

// ---------- a head ----------
function parseHead(buf, entry) {                                       // buf: a head's file as build-heads.py wrote it
  const n = new Int32Array(buf, 0, 1)[0];
  const ang = new Float32Array(buf, 4, 2 * n), H = { entry, n, ir: new Float32Array(buf, 4 + 8 * n, n * 2 * TAPS), xyz: [], an: {} };
  for (let d = 0; d < n; d++) H.xyz.push(dirOf(ang[2 * d] * D2R, ang[2 * d + 1] * D2R));
  H.mirror = new Int32Array(n); H.mirrorOff = 0;                    // each direction's mirror image, left to right
  for (let d = 0; d < n; d++) { const [x, y, z] = H.xyz[d]; const [m, off] = nearest(H, [x, -y, z]); H.mirror[d] = m; H.mirrorOff = Math.max(H.mirrorOff, off); }
  return H;
}
function nearest(H, v) {
  let best = -2, at = 0;
  for (let d = 0; d < H.n; d++) { const u = H.xyz[d], dot = u[0] * v[0] + u[1] * v[1] + u[2] * v[2]; if (dot > best) { best = dot; at = d; } }
  return [at, Math.acos(Math.min(1, best)) / D2R];
}
// One ear's response in a variant: direction d, ear e (0 left, 1 right).
function ear(H, variant, d, e) {
  const from = variant === 0 ? e : variant - 1, dir = variant === 0 || e === from ? d : H.mirror[d];
  return H.ir.subarray((dir * 2 + from) * TAPS, (dir * 2 + from + 1) * TAPS);
}
// A variant's CLL and ILD (dB on the axis) and ITD (µs) in every direction, and the CLL's average over the directions.
function analyse(H, variant) {
  if (H.an[variant]) return H.an[variant];
  const n = H.n, A = { cll: new Float32Array(n * NF), ild: new Float32Array(n * NF), itd: new Float32Array(n), avg: new Float32Array(NF) };
  const L = new Float32Array(NF), R = new Float32Array(NF);
  for (let d = 0; d < n; d++) {
    const [lr, li] = spectrumOf(ear(H, variant, d, 0), ITD_N), [rr, ri] = spectrumOf(ear(H, variant, d, 1), ITD_N);
    onAxis(lr, li, ITD_N, L, 0); onAxis(rr, ri, ITD_N, R, 0);
    for (let k = 0; k < NF; k++) { const c = 10 * Math.log10(10 ** (L[k] / 10) + 10 ** (R[k] / 10));
      A.cll[d * NF + k] = c; A.ild[d * NF + k] = L[k] - R[k]; A.avg[k] += c / n; }
    A.itd[d] = itdOf(lr, li, rr, ri) * 1e6;
  }
  return H.an[variant] = A;
}

// ---------- curves on the axis ----------
const kernels = new Map();
function blur(x, fwhm) {
  let g = kernels.get(fwhm);
  if (!g) { const s = fwhm / 2.355 / STEP, r = Math.ceil(3 * s); g = Float32Array.from({ length: 2 * r + 1 }, (_, i) => Math.exp(-0.5 * ((i - r) / s) ** 2));
    const sum = g.reduce((a, b) => a + b); for (let i = 0; i < g.length; i++) g[i] /= sum; kernels.set(fwhm, g); }
  const n = x.length, r = (g.length - 1) / 2, out = new Float32Array(n);
  for (let k = 0; k < n; k++) { let s = 0; for (let i = -r; i <= r; i++) s += g[i + r] * x[Math.max(0, Math.min(n - 1, k + i))]; out[k] = s; }
  return out;
}
// Dips: Gaussians on the axis, each [centre (ERB), depth (dB, negative), sigma (ERB)].
function bumps(par) {
  const out = new Float32Array(NF);
  for (const [c, a, s] of par) for (let k = 0; k < NF; k++) { const u = (E0 + k * STEP - c) / s; out[k] += a * Math.exp(-0.5 * u * u); }
  return out;
}
function solve(M, b, n) {                                              // Gaussian elimination with pivoting; M and b are used up
  for (let p = 0; p < n; p++) {
    let best = p; for (let r = p + 1; r < n; r++) if (Math.abs(M[r * n + p]) > Math.abs(M[best * n + p])) best = r;
    if (best !== p) { for (let c = 0; c < n; c++) { const t = M[p * n + c]; M[p * n + c] = M[best * n + c]; M[best * n + c] = t; } const t = b[p]; b[p] = b[best]; b[best] = t; }
    const piv = M[p * n + p] || 1e-12;
    for (let r = p + 1; r < n; r++) { const f = M[r * n + p] / piv; if (!f) continue; for (let c = p; c < n; c++) M[r * n + c] -= f * M[p * n + c]; b[r] -= f * b[p]; }
  }
  for (let p = n - 1; p >= 0; p--) { let v = b[p]; for (let c = p + 1; c < n; c++) v -= M[p * n + c] * b[c]; b[p] = v / (M[p * n + p] || 1e-12); }
  return b;
}
// Curves of many directions as spherical harmonics of the given order, one fit a point of the curves (their first
// `width` points): least squares with a penalty on roughness. Returns the coefficients, (order + 1)² x width.
function fitSphere(H, dirs, curves, order, width = NF) {
  const K = (order + 1) ** 2, Y = dirs.map(d => shN(order, ...H.xyz[d])), M = new Float64Array(K * K), B = new Float64Array(K * width);
  for (let i = 0; i < dirs.length; i++) { const y = Y[i], b = curves[i];
    for (let p = 0; p < K; p++) { for (let q = 0; q <= p; q++) M[p * K + q] += y[p] * y[q]; for (let k = 0; k < width; k++) B[p * width + k] += y[p] * b[k]; } }
  let trace = 0; for (let p = 0; p < K; p++) trace += M[p * K + p];
  for (let p = 0; p < K; p++) { const n = Math.floor(Math.sqrt(p)); M[p * K + p] += 1e-3 * trace / K * (0.01 + n * (n + 1)); }
  for (let p = 0; p < K; p++) for (let q = 0; q <= p; q++) {          // Cholesky, in place, lower triangle
    let t = M[p * K + q]; for (let j = 0; j < q; j++) t -= M[p * K + j] * M[q * K + j];
    M[p * K + q] = p === q ? Math.sqrt(t) : t / M[q * K + q]; }
  for (let k = 0; k < width; k++) {
    for (let p = 0; p < K; p++) { let t = B[p * width + k]; for (let j = 0; j < p; j++) t -= M[p * K + j] * B[j * width + k]; B[p * width + k] = t / M[p * K + p]; }
    for (let p = K - 1; p >= 0; p--) { let t = B[p * width + k]; for (let j = p + 1; j < K; j++) t -= M[j * K + p] * B[j * width + k]; B[p * width + k] = t / M[p * K + p]; } }
  return B;
}
// Those coefficients' curve in direction d.
function onSphere(H, coef, order, d, width = NF) {
  const y = shN(order, ...H.xyz[d]), out = new Float32Array(width);
  for (let p = 0; p < y.length; p++) for (let k = 0; k < width; k++) out[k] += y[p] * coef[p * width + k];
  return out;
}

// ---------- dips ----------
// The dips moved to fit r on the dips' band: damped least squares, each dip kept a dip inside the band. With a prior —
// per dip a [centre, depth, sigma] to stay near, or none — being off it by TIE costs, at pull 1, as much as 1 dB RMS
// over the band; pull is one number or one a parameter, so [1, 0, 1] holds centre and width and leaves the depth free.
// Returns the dips, with the cost reached as their .cost.
const TIE = [0.5, 6, 0.3];                                            // ERB, dB, ERB
function refine(r, par, steps, prior = null, pull = 0) {
  if (!par.length) { const none = []; none.cost = 0; for (let k = LO; k <= HI; k++) none.cost += r[k] * r[k]; return none; }
  const K = par.length, P = 3 * K, cLo = E0 + LO * STEP, cHi = E0 + HI * STEP;
  const tie = TIE.map((t, i) => (Array.isArray(pull) ? pull[i] : pull) * (HI - LO + 1) / (t * t)), held = p => prior && prior[(p - p % 3) / 3];
  const hold = q => { for (let j = 0; j < K; j++) { q[3 * j] = Math.max(cLo, Math.min(cHi, q[3 * j]));
    q[3 * j + 1] = Math.max(-60, Math.min(-0.01, q[3 * j + 1])); q[3 * j + 2] = Math.max(0.1, Math.min(1.5, q[3 * j + 2])); } return q; };
  const left = q => { const out = new Float64Array(HI - LO + 1);
    for (let k = LO; k <= HI; k++) { let m = 0; for (let j = 0; j < K; j++) { const u = (E0 + k * STEP - q[3 * j]) / q[3 * j + 2]; m += q[3 * j + 1] * Math.exp(-0.5 * u * u); } out[k - LO] = r[k] - m; }
    return out; };
  const costOf = (q, res) => { let t = 0; for (const x of res) t += x * x;
    for (let p = 0; p < P; p++) if (held(p)) t += tie[p % 3] * (q[p] - held(p)[p % 3]) ** 2; return t; };
  let q = hold(par.flat()), res = left(q), cost = costOf(q, res), damp = 0.01;
  for (let it = 0; it < steps; it++) {
    const JJ = new Float64Array(P * P), Jr = new Float64Array(P), row = new Float64Array(P);
    for (let k = LO; k <= HI; k++) {
      for (let j = 0; j < K; j++) { const a = q[3 * j + 1], s = q[3 * j + 2], u = (E0 + k * STEP - q[3 * j]) / s, g = Math.exp(-0.5 * u * u);
        row[3 * j] = a * g * u / s; row[3 * j + 1] = g; row[3 * j + 2] = a * g * u * u / s; }
      for (let p = 0; p < P; p++) { Jr[p] += row[p] * res[k - LO]; for (let t = 0; t < P; t++) JJ[p * P + t] += row[p] * row[t]; } }
    for (let p = 0; p < P; p++) if (held(p)) { JJ[p * P + p] += tie[p % 3]; Jr[p] += tie[p % 3] * (held(p)[p % 3] - q[p]); }
    let better = false;
    for (let tries = 0; tries < 6 && !better; tries++) {
      const M = Float64Array.from(JJ); for (let p = 0; p < P; p++) M[p * P + p] += damp * (JJ[p * P + p] + 1e-9);
      const step = solve(M, Float64Array.from(Jr), P), q2 = hold(q.map((v, i) => v + step[i])), res2 = left(q2), cost2 = costOf(q2, res2);
      if (cost2 < cost) { q = q2; res = res2; cost = cost2; damp *= 0.3; better = true; } else damp *= 10;
    }
    if (!better) break;
  }
  const out = Array.from({ length: K }, (_, j) => [q[3 * j], q[3 * j + 1], q[3 * j + 2]]); out.cost = cost; return out;
}
// What dips are fitted to: a CLL (less its average) less its smooth part — the blur of the CLL with the dips taken
// out; below FROM the smooth part is the CLL itself, over one ERB.
function targetOf(x, dips) {
  const m = bumps(dips), e = erbOf(FROM);
  for (let k = 0; k < NF; k++) m[k] = x[k] - m[k];
  const b = blur(m, WIDTH), r = new Float32Array(NF);
  for (let k = 0; k < NF; k++) { const t = Math.max(0, Math.min(1, E0 + k * STEP - e + 0.5)); r[k] = (x[k] - b[k]) * t * t * (3 - 2 * t); }
  return r;
}
// The smooth part and DIPS dips of a CLL fitted in turn until they agree: the dips to what the smooth part leaves, the
// smooth part the blur of the CLL with the dips taken out. Each dip starts at the deepest point still left.
function splitOf(x) {
  let r = targetOf(x, []), par = [];
  for (let j = 0; j < DIPS; j++) { const m = bumps(par); let at = LO, deep = Infinity;
    for (let k = LO; k <= HI; k++) if (r[k] - m[k] < deep) { deep = r[k] - m[k]; at = k; }
    par = [...refine(r, [...par, [E0 + at * STEP, Math.min(deep, -0.1), 0.3]], 12)]; }
  for (let turn = 0; turn < 4; turn++) { r = targetOf(x, par); par = [...refine(r, par, 8)]; }
  return par;
}
// The CLLs of all directions prepared for the fit: blurred along the axis (ERB, 0 for not), then held to a
// spherical-harmonic order over the sphere (0 for not) — a notch then has to agree with its neighbours to be seen.
function prepared(H, xs, width, order) {
  let out = width > 0 ? xs.map(x => blur(x, width)) : xs;
  if (order > 0) { const dirs = xs.map((_, d) => d), coef = fitSphere(H, dirs, out, order); out = dirs.map(d => onSphere(H, coef, order, d)); }
  return out;
}

// ---------- sheets: the dips of all directions, each dip one surface over the sphere ----------
// Each direction's nearest directions, nearest first: [direction, angle in degrees], within 20°, at most 24.
function neighbours(H) {
  const limit = Math.cos(20 * D2R);
  return H.xyz.map((u, d) => { const list = [];
    for (let j = 0; j < H.n; j++) { if (j === d) continue; const v = H.xyz[j], dot = u[0] * v[0] + u[1] * v[1] + u[2] * v[2]; if (dot > limit) list.push([j, Math.acos(Math.min(1, dot)) / D2R]); }
    return list.sort((a, b) => a[1] - b[1]).slice(0, 24); });
}
const ORDERS = [[0, 1, 2, 3], [0, 1, 3, 2], [0, 2, 1, 3], [0, 2, 3, 1], [0, 3, 1, 2], [0, 3, 2, 1], [1, 0, 2, 3], [1, 0, 3, 2], [1, 2, 0, 3], [1, 2, 3, 0], [1, 3, 0, 2], [1, 3, 2, 0],
                [2, 0, 1, 3], [2, 0, 3, 1], [2, 1, 0, 3], [2, 1, 3, 0], [2, 3, 0, 1], [2, 3, 1, 0], [3, 0, 1, 2], [3, 0, 2, 1], [3, 1, 0, 2], [3, 1, 2, 0], [3, 2, 0, 1], [3, 2, 1, 0]];
// The dips named: from the front outward, a direction's dips take the order in which their centres lie nearest the
// same-named dips of the directions already done — a shallow dip counting for less. Sheet 0 is the lowest in front.
// Returns the dips in that order and the order of the walk.
function named(H, near, dips) {
  const n = H.n, out = new Array(n), walk = [nearest(H, [1, 0, 0])[0]], queued = new Uint8Array(n); queued[walk[0]] = 1;
  for (let i = 0; i < walk.length; i++) for (const [j] of near[walk[i]]) if (!queued[j]) { queued[j] = 1; walk.push(j); }
  for (let d = 0; d < n; d++) if (!queued[d]) walk.push(d);                                   // a direction with no neighbour
  for (const d of walk) {
    const ref = [0, 0, 0, 0], sum = [0, 0, 0, 0];
    for (const [j, angle] of near[d]) if (out[j]) { const w = Math.exp(-0.5 * (angle / 8) ** 2);
      out[j].forEach((p, k) => { const depth = Math.min(1, -p[1] / 3); ref[k] += w * depth * p[0]; sum[k] += w * depth; }); }
    if (!sum.some(s => s > 0)) { out[d] = [...dips[d]].sort((a, b) => a[0] - b[0]); continue; }
    let best = null, least = Infinity;
    for (const o of ORDERS) { let cost = 0;
      for (let k = 0; k < 4; k++) if (sum[k] > 0) cost += Math.abs(dips[d][o[k]][0] - ref[k] / sum[k]) * (0.3 + 0.7 * Math.min(1, -dips[d][o[k]][1] / 3));
      if (cost < least) { least = cost; best = o; } }
    out[d] = best.map(i => dips[d][i]);
  }
  return [out, walk];
}
// Whether the target itself, within half a sigma of a dip's centre, is at least half as deep as the dip.
function supported(target, [c, a, sigma]) {
  let m = 0, n = 0;
  for (let k = Math.max(0, Math.round((c - 0.5 * sigma - E0) / STEP)); k <= Math.min(NF - 1, Math.round((c + 0.5 * sigma - E0) / STEP)); k++) { m += target[k]; n++; }
  return m / n <= a / 2;
}
// How a set of named dips does (a direction's dip of a name may be missing):
//   left         what is left of the targets, dB RMS at 4–16 kHz
//   gaps         of the dips deeper than 3 dB, the share whose namesake in the nearest direction is missing, shallower
//                than 1.5 dB or more than 0.5 ERB away
//   apart        how far the centres of namesakes deeper than 3 dB are apart between nearest directions, ERB, median
//   unsupported  of the dips deeper than 3 dB, the share the target does not support
//   deep         dips deeper than 3 dB, a direction
function measureOf(near, targets, dips) {
  let left = 0, count = 0, deep = 0, gap = 0, bare = 0; const apart = [];
  dips.forEach((par, d) => { const m = bumps(par.filter(Boolean)); for (const k of NOTCHES) { left += (targets[d][k] - m[k]) ** 2; count++; }
    const other = near[d].length ? dips[near[d][0][0]] : [];
    par.forEach((p, k) => { if (!p || p[1] > -3) return; deep++; const q = other[k];
      if (!supported(targets[d], p)) bare++;
      if (!q || q[1] > -1.5 || Math.abs(q[0] - p[0]) > 0.5) gap++;
      if (q && q[1] <= -3) apart.push(Math.abs(q[0] - p[0])); }); });
  apart.sort((a, b) => a - b);
  return { left: Math.sqrt(left / count), gaps: gap / deep, apart: apart[apart.length >> 1] || 0, unsupported: bare / deep, deep: deep / dips.length };
}

// ---------- tracks: sheets that are born and end, and hold nothing the spectrum does not ----------
// 1  A dip counts where it is at least `counts` dB deep and its own spectrum supports it; a direction has as many dips
//    as count, none if none does.
// 2  Dips of neighbouring directions (within `reach` degrees) are linked where each is the other's nearest and their
//    centres lie within `link` ERB; links join dips into sheets, the closest first, a sheet never holding two dips of
//    one direction. A sheet of fewer than `least` directions is dropped.
// 3  A hole — a direction a sheet misses, with the sheet around it — gets a dip at the centre and width its
//    neighbours suggest, the depth fitted to the direction's own spectrum; it stays if it is at least `bridge` dB
//    deep and supported. (bridge 0: no bridging.)
// 4  Every direction's dips are fitted once more, centre and width held near the sheet's neighbours by `pull`, the
//    depth by nothing but the spectrum; a dip that ends shallower than the bridge's depth, or unsupported, goes.
// Returns per direction an array with one place a sheet — the dip, or null — the sheets ordered by size.
function tracksOf(H, near, targets, dips, { counts, link, reach, least, pull, bridge }) {
  const n = H.n, floor = bridge || counts / 2;
  const cand = dips.map((par, d) => { const keep = par.filter(p => p[1] <= -counts && supported(targets[d], p));
    return (keep.length < par.length ? [...refine(targets[d], keep, 6)] : keep.map(p => [...p])).filter(p => p[1] <= -counts); });
  const base = [0]; for (let d = 0; d < n; d++) base.push(base[d] + cand[d].length);
  const edges = [], closest = (p, list) => { let at = -1, least = Infinity; list.forEach((q, i) => { const gap = Math.abs(q[0] - p[0]); if (gap < least) { least = gap; at = i; } }); return [at, least]; };
  for (let d = 0; d < n; d++) for (const [j, angle] of near[d]) { if (j < d || angle > reach) continue;
    cand[d].forEach((p, i) => { const [k, gap] = closest(p, cand[j]); if (k >= 0 && gap <= link && closest(cand[j][k], cand[d])[0] === i) edges.push([gap + 0.01 * angle, base[d] + i, base[j] + k]); }); }
  edges.sort((a, b) => a[0] - b[0]);
  const dirOf_ = new Int32Array(base[n]), group = new Int32Array(base[n]), members = [];
  for (let d = 0; d < n; d++) cand[d].forEach((_, i) => { dirOf_[base[d] + i] = d; group[base[d] + i] = base[d] + i; members[base[d] + i] = new Map([[d, base[d] + i]]); });
  for (const [, a, b] of edges) { let ga = group[a], gb = group[b]; if (ga === gb) continue;
    if (members[ga].size < members[gb].size) [ga, gb] = [gb, ga];
    let clash = false; for (const d of members[gb].keys()) if (members[ga].has(d)) { clash = true; break; }
    if (clash) continue;
    for (const [d, id] of members[gb]) { members[ga].set(d, id); group[id] = ga; } members[gb] = null; }
  const sheets = members.filter((m, id) => m && group[id] === id && m.size >= least).sort((a, b) => b.size - a.size)
    .map(m => new Map([...m].map(([d, id]) => [d, cand[d][id - base[d]]])));                       // direction -> dip
  const inSheets = d => sheets.map((s, k) => [k, s.get(d)]).filter(([, p]) => p);
  const around = (s, d) => { let w = 0, c = 0, a = 0, sg = 0, off = [0, 0, 0], far = 0, count = 0; const u = H.xyz[d];
    for (const [j, angle] of near[d]) { const p = s.get(j); if (!p || angle > reach) continue; const g = Math.exp(-0.5 * (angle / (reach / 2)) ** 2), v = H.xyz[j];
      w += g; c += g * p[0]; a += g * p[1]; sg += g * p[2]; count++; far += angle; for (let i = 0; i < 3; i++) off[i] += v[i] - u[i]; }
    return count ? { count, par: [c / w, a / w, sg / w], inside: Math.hypot(...off) / count < 0.5 * 2 * Math.sin(far / count * D2R / 2) } : null; };
  if (bridge > 0) for (let pass = 0; pass < 2; pass++) sheets.forEach(s => { const adds = [];
    for (let d = 0; d < n; d++) { if (s.has(d)) continue; const nb = around(s, d); if (!nb || nb.count < 2 || !nb.inside) continue;
      const own = inSheets(d), trial = refine(targets[d], [...own.map(([, p]) => p), nb.par], 8, [...own.map(() => null), nb.par], [1, 0, 1]), born = trial[trial.length - 1];
      if (born[1] <= -bridge && supported(targets[d], born)) adds.push([d, own, trial]); }
    for (const [d, own, trial] of adds) { own.forEach(([k], i) => sheets[k].set(d, trial[i])); s.set(d, trial[trial.length - 1]); } });
  for (let pass = 0; pass < 2; pass++) for (let d = 0; d < n; d++) { const own = inSheets(d); if (!own.length) continue;
    const priors = own.map(([k]) => { const nb = around(sheets[k], d); return nb ? nb.par : null; });
    refine(targets[d], own.map(([, p]) => p), 6, priors, [pull, 0, pull]).forEach((p, i) => sheets[own[i][0]].set(d, p)); }
  for (const s of sheets) for (const [d, p] of [...s]) if (p[1] > -floor || !supported(targets[d], p)) s.delete(d);
  const kept = sheets.filter(s => s.size >= least).sort((a, b) => b.size - a.size);
  return Array.from({ length: n }, (_, d) => kept.map(s => s.get(d) || null));
}

// ---------- surfaces: a few sheets, each smooth over the sphere ----------
// The `count` largest sheets grow into surfaces: a surface's centre over the sphere is the spherical-harmonic fit
// (order `order`) of its dips' centres; a dip of another sheet in a direction next to the surface (within 15°), within
// `join` ERB of where the surface passes, joins it; fitted again, eight times. What joins no surface is left out.
// Returns per direction the surfaces' dips (or null), and each surface's centre coefficients.
function surfacesOf(H, near, set, count, order, join) {
  const n = H.n, sizes = (set[0] || []).map((_, k) => set.filter(p => p[k]).length);
  const largest = sizes.map((size, k) => [size, k]).sort((a, b) => b[0] - a[0]).slice(0, count).map(q => q[1]);
  const owner = set.map(p => p.map(q => q ? -1 : -2));                 // a dip's surface; -1 free, -2 no dip
  largest.forEach((k, s) => { for (let d = 0; d < n; d++) if (set[d][k]) owner[d][k] = s; });
  const coefs = largest.map(() => null);
  for (let turn = 0; turn < 8; turn++) largest.forEach((_, s) => {
    const mem = []; for (let d = 0; d < n; d++) { const k = owner[d].indexOf(s); if (k >= 0) mem.push([d, set[d][k][0]]); }
    if (mem.length < 2 * (order + 1) ** 2) return;                     // too few dips to carry a surface of that order
    coefs[s] = fitSphere(H, mem.map(m => m[0]), mem.map(m => [m[1]]), order, 1);
    const has = new Set(mem.map(m => m[0]));
    for (const [d] of mem) for (const [j, angle] of near[d]) { if (has.has(j) || angle > 15) continue; has.add(j);
      const c = onSphere(H, coefs[s], order, j, 1)[0]; let best = -1, gap = join;
      set[j].forEach((q, k) => { if (q && owner[j][k] === -1 && Math.abs(q[0] - c) < gap) { gap = Math.abs(q[0] - c); best = k; } });
      if (best >= 0) owner[j][best] = s; } });
  return [set.map((p, d) => largest.map((_, s) => { const k = owner[d].indexOf(s); return k >= 0 ? p[k] : null; })), coefs];
}
// Back to the input: the dips, found on the prepared CLLs, fitted to the measured CLL of their own direction — the
// centre held near where it was found, the width loosely, the depth by nothing but the spectrum. Where a surface
// passes a direction without a dip in it (the surface around it), a dip is tried at the surface's centre. A dip that
// ends shallower than `floor` dB, or that the measured spectrum does not support, goes.
// Returns per direction the dips (or null) and the measured targets they are fitted to.
function backToInput(H, near, xs, set, coefs, order, floor) {
  const count = (set[0] || []).length, widths = Array.from({ length: count }, (_, s) => { const w = set.map(p => p[s]).filter(Boolean).map(p => p[2]).sort((a, b) => a - b); return w[w.length >> 1] || 0.4; });
  const inside = (s, d) => { let found = 0, far = 0, off = [0, 0, 0]; const u = H.xyz[d];
    for (const [j, angle] of near[d]) { if (angle > 15 || !set[j][s]) continue; found++; far += angle; for (let i = 0; i < 3; i++) off[i] += H.xyz[j][i] - u[i]; }
    return found >= 2 && Math.hypot(...off) / found < 0.5 * 2 * Math.sin(far / found * D2R / 2); };
  const targets = [], out = set.map((places, d) => {
    const start = places.map((p, s) => p ? [...p] : coefs && coefs[s] && inside(s, d) ? [onSphere(H, coefs[s], order, d, 1)[0], -2, widths[s]] : null);
    const at = start.map((p, s) => p ? s : -1).filter(s => s >= 0); let par = at.map(s => start[s]);
    const prior = par.map(p => [...p]);
    for (let turn = 0; turn < 2; turn++) par = [...refine(targetOf(xs[d], par), par, 8, prior, [2, 0, 0.5])];
    const r = targetOf(xs[d], par), kept = places.map(() => null);
    par.forEach((p, i) => { if (p[1] <= -floor && supported(r, p)) kept[at[i]] = p; });
    targets.push(targetOf(xs[d], kept.filter(Boolean)));
    return kept; });
  return [out, targets];
}

// ---------- surfaces as numbers ----------
// Each surface as three patterns over the sphere — its dips' centre, depth and width as spherical harmonics of the
// order: 3 x (order + 1)² numbers a surface. Centre and width are fitted to the surface's dips; with those, each
// direction's depths are the ones that fit its measured CLL best (never above 0, and 0 further than 15° from the
// surface's dips), and the depth's pattern is fitted to them. A surface of too few dips for the order is null.
function asNumbers(H, near, xs, final, order) {
  const n = H.n, all = Array.from({ length: n }, (_, d) => d), K = (order + 1) ** 2, bandLo = E0 + LO * STEP, bandHi = E0 + HI * STEP;
  const sheets = ((final[0] || []).map((_, s) => { const mem = all.filter(d => final[d][s]); if (mem.length < 1.5 * K) return null;
    const reach = new Uint8Array(n); for (const d of mem) { reach[d] = 1; for (const [j, angle] of near[d]) if (angle <= 15) reach[j] = 1; }
    return { centre: fitSphere(H, mem, mem.map(d => [final[d][s][0]]), order, 1), width: fitSphere(H, mem, mem.map(d => [final[d][s][2]]), order, 1), reach, depths: new Float32Array(n) }; }));
  const at = (coef, y) => { let v = 0; for (let p = 0; p < K; p++) v += y[p] * coef[p]; return v; };
  for (const d of all) { const y = shN(order, ...H.xyz[d]), r = targetOf(xs[d], final[d].filter(Boolean));
    let live = sheets.filter(sh => sh && sh.reach[d]);
    const shape = new Map(live.map(sh => { const c = Math.max(bandLo, Math.min(bandHi, at(sh.centre, y))), w = Math.max(0.1, Math.min(1.5, at(sh.width, y)));
      return [sh, Float32Array.from({ length: HI - LO + 1 }, (_, k) => Math.exp(-0.5 * ((E0 + (LO + k) * STEP - c) / w) ** 2))]; }));
    while (live.length) { const m = live.length, M = new Float64Array(m * m), b = new Float64Array(m);    // least squares, depths above 0 dropped
      live.forEach((p, i) => { const g = shape.get(p); for (let k = 0; k <= HI - LO; k++) b[i] += g[k] * r[LO + k];
        live.forEach((q, j) => { const h = shape.get(q); let t = 0; for (let k = 0; k <= HI - LO; k++) t += g[k] * h[k]; M[i * m + j] = t + (i === j ? 1e-6 : 0); }); });
      const a = solve(M, b, m), worst = a.reduce((w, v, i) => v > a[w] ? i : w, 0);
      if (a[worst] > 0) { live = live.filter((_, i) => i !== worst); continue; }
      live.forEach((sh, i) => { sh.depths[d] = a[i]; }); break; } }
  for (const sh of sheets) if (sh) { sh.depth = fitSphere(H, all, all.map(d => [sh.depths[d]]), order, 1); delete sh.depths; delete sh.reach; }
  return sheets;
}
// Per direction the dips those numbers give, a place a surface (null where it is shallower than half a dB, or left out).
function rebuilt(H, sheets, order) {
  const K = (order + 1) ** 2, bandLo = E0 + LO * STEP, bandHi = E0 + HI * STEP, at = (coef, y) => { let v = 0; for (let p = 0; p < K; p++) v += y[p] * coef[p]; return v; };
  return H.xyz.map(u => { const y = shN(order, ...u);
    return sheets.map(sh => { if (!sh) return null; const a = at(sh.depth, y); return a > -0.5 ? null : [Math.max(bandLo, Math.min(bandHi, at(sh.centre, y))), Math.max(-60, a), Math.max(0.1, Math.min(1.5, at(sh.width, y)))]; }); });
}
// The cost of a set of dips: what they and the smooth part leave of the measured CLLs, blurred to one ERB — finer than
// that is not heard — dB RMS over the directions and 4–16 kHz.
function costOf(xs, set) {
  let sum = 0;
  set.forEach((p, d) => { const dips = p.filter(Boolean), r = targetOf(xs[d], dips), m = bumps(dips), e = blur(r.map((v, k) => v - m[k]), 1); for (const k of NOTCHES) sum += e[k] * e[k]; });
  return Math.sqrt(sum / set.length / NOTCHES.length);
}

// ---------- the whole chain ----------
// P: blur (ERB) and sphere (order) to prepare; counts, link, reach, least, pull, bridge for the tracks; surfaces (how
// many, 0 for the tracks as they are), surfaceOrder, join; floor for the way back. `keep` holds what a head's later
// runs can use again: its measured CLLs, its neighbours, and the split of each way of preparing (the last `room`).
function fitted(H, A, P, keep, room = 6) {
  const n = H.n, dirs = Array.from({ length: n }, (_, d) => d), key = P.blur + "," + P.sphere;
  if (!keep.xs) { keep.xs = dirs.map(d => A.cll.subarray(d * NF, (d + 1) * NF).map((c, k) => c - A.avg[k])); keep.near = neighbours(H); keep.fits = new Map(); }
  if (!keep.fits.has(key)) { if (keep.fits.size >= room) keep.fits.delete(keep.fits.keys().next().value);
    const prep = prepared(H, keep.xs, P.blur, P.sphere), dips = prep.map(splitOf);
    keep.fits.set(key, { prep, dips, targets: prep.map((x, d) => targetOf(x, dips[d])) }); }
  return keep.fits.get(key);
}
// The costs of the settings P for several sizes [surfaces, order]: of the dips rebuilt from the surfaces' numbers.
function scores(H, A, P, keep, sizes) {
  const f = fitted(H, A, P, keep, 40), tracks = tracksOf(H, keep.near, f.targets, f.dips, P);
  return sizes.map(([count, order]) => { const [places, coefs] = surfacesOf(H, keep.near, tracks, count, order, P.join);
    return costOf(keep.xs, rebuilt(H, asNumbers(H, keep.near, keep.xs, backToInput(H, keep.near, keep.xs, places, coefs, order, P.floor)[0], order), order)); });
}
// What the page draws.
function chain(H, A, P, keep) {
  const n = H.n, dirs = Array.from({ length: n }, (_, d) => d), f = fitted(H, A, P, keep);
  const tracks = tracksOf(H, keep.near, f.targets, f.dips, P);
  const [places, coefs] = P.surfaces > 0 ? surfacesOf(H, keep.near, tracks, P.surfaces, P.surfaceOrder, P.join) : [tracks, null];
  const [final] = backToInput(H, keep.near, keep.xs, places, coefs, P.surfaceOrder, P.floor);
  const sheets = asNumbers(H, keep.near, keep.xs, final, P.surfaceOrder), again = rebuilt(H, sheets, P.surfaceOrder);
  const targets = dirs.map(d => targetOf(keep.xs[d], again[d].filter(Boolean)));
  const smooth = dirs.map(d => keep.xs[d].map((v, k) => v - targets[d][k])), some = dirs.filter(d => d % Math.ceil(n / 1000) === 0);
  const count = arr => arr.reduce((t, p) => t + p.filter(Boolean).length, 0);
  return { n, final, places: (final[0] || []).length, rebuilt: again, sheets: sheets.filter(Boolean).length, numbers: sheets.filter(Boolean).length * 3 * (P.surfaceOrder + 1) ** 2, prep: f.prep,
           pattern: fitSphere(H, some, some.map(d => smooth[d]), 4), tracks: (tracks[0] || []).length,
           found: count(f.dips.map(p => p.filter(q => q[1] <= -P.counts))), tracked: count(tracks), gathered: count(places), kept: count(final),
           cost: costOf(keep.xs, again), costFree: costOf(keep.xs, final), costNone: costOf(keep.xs, dirs.map(() => [])), measure: measureOf(keep.near, targets, again) };
}

// ---------- responses: the two ears for a CLL, an ILD and an ITD ----------
function minimumPhase(db) {                                            // db: 513 points of a 1024-point spectrum
  const n = 1024, re = new Float64Array(n), im = new Float64Array(n);
  for (let k = 0; k <= n / 2; k++) { re[k] = db[k] * Math.LN10 / 20; if (k > 0 && k < n / 2) re[n - k] = re[k]; }
  fft(re, im, true); im.fill(0);
  for (let k = 1; k < n / 2; k++) re[k] *= 2; for (let k = n / 2 + 1; k < n; k++) re[k] = 0;
  fft(re, im);
  for (let k = 0; k < n; k++) { const m = Math.exp(re[k]), p = im[k]; re[k] = m * Math.cos(p); im[k] = m * Math.sin(p); }
  return [re, im];
}
const LEAD = 32, OUT = 384;                                            // a response is led by LEAD samples: a part of a sample's delay rings ahead
function delayed(re, im, delay) {
  const n = re.length, r = new Float64Array(n), i = new Float64Array(n);
  for (let k = 0; k <= n / 2; k++) { const a = -2 * Math.PI * k * (LEAD + delay) / n, c = Math.cos(a), s = Math.sin(a);
    r[k] = re[k] * c - im[k] * s; i[k] = re[k] * s + im[k] * c; if (k > 0 && k < n / 2) { r[n - k] = r[k]; i[n - k] = -i[k]; } }
  fft(r, i, true);
  const out = Float32Array.from(r.subarray(0, OUT)); for (let k = 0; k < 32; k++) out[OUT - 32 + k] *= Math.cos(k / 32 * Math.PI / 2) ** 2;
  return out;
}
// The two ears for a CLL and an ILD (dB on the axis) and an ITD (µs): minimum phase, the later ear delayed until the pair has the ITD.
function earsOf(cll, ild, itd) {
  const n = 1024, ears = [0, 1].map(e => { const db = new Float64Array(n / 2 + 1);
    for (let k = 0; k <= n / 2; k++) { const p = Math.max(0, Math.min(NF - 1.001, (erbOf(k * FS / n) - E0) / STEP)), i = Math.floor(p), t = p - i;
      const c = cll[i] + (cll[i + 1] - cll[i]) * t, r = 10 ** ((ild[i] + (ild[i + 1] - ild[i]) * t) / 10);
      db[k] = c + 10 * Math.log10((e ? 1 : r) / (1 + r)); }
    return minimumPhase(db); });
  const plain = ears.map(([re, im]) => spectrumOf(delayed(re, im, 0), ITD_N));
  const more = (itd * 1e-6 - itdOf(plain[0][0], plain[0][1], plain[1][0], plain[1][1])) * FS;
  return [delayed(...ears[0], Math.max(more, 0)), delayed(...ears[1], Math.max(-more, 0))];
}
