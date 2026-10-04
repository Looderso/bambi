// A head's analysis and the split of its CLL, shared by index.html and run-split.js. Needs common.js (shN, dirOf, D2R)
// and the settings S: variant-independent — flat (kHz, 20 is off), method, width (ERB), passes, notch (ERB), dips,
// from (kHz), average ("removed" or "kept").
let FS = 48000, TAPS = 256;

// ---------- the auditory axis: ERB number, 0.3–20 kHz in steps of 0.1 ERB ----------
const erbOf = f => 21.4 * Math.log10(1 + 0.00437 * f), fOf = e => (10 ** (e / 21.4) - 1) / 0.00437;
const STEP = 0.1, E0 = erbOf(300), NF = Math.floor((erbOf(20000) - E0) / STEP) + 1, E1 = E0 + (NF - 1) * STEP;
const FREQ = Float32Array.from({ length: NF }, (_, k) => fOf(E0 + k * STEP));
const inBand = (lo, hi) => { const k = []; FREQ.forEach((f, i) => { if (f >= lo && f <= hi) k.push(i); }); return k; };
const NOTCHES = inBand(4000, 16000), WIDE = inBand(1000, 16000);

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
// Above S.flat kHz an ear's spectrum (dB on the axis) fades over two ERB to its mean level over the three ERB below.
function flatten(v) {
  if (S.flat >= 20) return v;
  const k0 = Math.round((erbOf(S.flat * 1000) - E0) / STEP), from = Math.max(0, k0 - Math.round(3 / STEP));
  let level = 0; for (let k = from; k <= k0; k++) level += v[k] / (k0 - from + 1);
  for (let k = k0; k < NF; k++) { const t = Math.min(1, (k - k0) * STEP / 2); v[k] += (level - v[k]) * t * t * (3 - 2 * t); }
  return v;
}
// A variant's CLL and ILD (dB on the axis) and ITD (µs) in every direction, and the CLL's average over the directions.
function analyse(H, variant) {
  const key = variant + "," + S.flat;
  if (H.an[key]) return H.an[key];
  const n = H.n, A = { cll: new Float32Array(n * NF), ild: new Float32Array(n * NF), itd: new Float32Array(n), avg: new Float32Array(NF) };
  const L = new Float32Array(NF), R = new Float32Array(NF);
  for (let d = 0; d < n; d++) {
    const [lr, li] = spectrumOf(ear(H, variant, d, 0), ITD_N), [rr, ri] = spectrumOf(ear(H, variant, d, 1), ITD_N);
    onAxis(lr, li, ITD_N, L, 0); onAxis(rr, ri, ITD_N, R, 0); flatten(L); flatten(R);
    for (let k = 0; k < NF; k++) { const c = 10 * Math.log10(10 ** (L[k] / 10) + 10 ** (R[k] / 10));
      A.cll[d * NF + k] = c; A.ild[d * NF + k] = L[k] - R[k]; A.avg[k] += c / n; }
    A.itd[d] = itdOf(lr, li, rr, ri) * 1e6;
  }
  return H.an[key] = A;
}
// How far a variant is from the head as measured: RMS over all directions, CLL and ILD at 1–16 kHz.
function distance(H, variant) {
  const A = analyse(H, variant), O = analyse(H, 0); let c = 0, i = 0, t = 0;
  for (let d = 0; d < H.n; d++) { for (const k of WIDE) { c += (A.cll[d * NF + k] - O.cll[d * NF + k]) ** 2; i += (A.ild[d * NF + k] - O.ild[d * NF + k]) ** 2; }
    t += (A.itd[d] - O.itd[d]) ** 2; }
  return [Math.sqrt(c / H.n / WIDE.length), Math.sqrt(i / H.n / WIDE.length), Math.sqrt(t / H.n)];
}

// ---------- the split: a curve on the axis into a smooth part and a layer ----------
const kernels = new Map();
function blur(x, fwhm) {
  let g = kernels.get(fwhm);
  if (!g) { const s = fwhm / 2.355 / STEP, r = Math.ceil(3 * s); g = Float32Array.from({ length: 2 * r + 1 }, (_, i) => Math.exp(-0.5 * ((i - r) / s) ** 2));
    const sum = g.reduce((a, b) => a + b); for (let i = 0; i < g.length; i++) g[i] /= sum; kernels.set(fwhm, g); }
  const n = x.length, r = (g.length - 1) / 2, out = new Float32Array(n);
  for (let k = 0; k < n; k++) { let s = 0; for (let i = -r; i <= r; i++) s += g[i + r] * x[Math.max(0, Math.min(n - 1, k + i))]; out[k] = s; }
  return out;
}
function sheet(x, fwhm, passes) {
  let b = blur(x, fwhm); const t = new Float32Array(x.length);
  for (let p = 0; p < passes; p++) { for (let k = 0; k < x.length; k++) t[k] = Math.max(x[k], b[k]); b = blur(t, fwhm); }
  return b;
}
function maxMin(x, window, fwhm) {
  const n = x.length, r = Math.max(1, Math.round(window / STEP / 2)), up = new Float32Array(n), down = new Float32Array(n);
  for (let k = 0; k < n; k++) { let m = -Infinity; for (let i = Math.max(0, k - r); i <= Math.min(n - 1, k + r); i++) m = Math.max(m, x[i]); up[k] = m; }
  for (let k = 0; k < n; k++) { let m = Infinity; for (let i = Math.max(0, k - r); i <= Math.min(n - 1, k + r); i++) m = Math.min(m, up[i]); down[k] = m; }
  return blur(down, fwhm);
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
// The dips moved to fit r between the axis points lo and hi: damped least squares, each dip kept a dip inside the band.
function refine(r, par, lo, hi, steps) {
  const K = par.length, P = 3 * K, cLo = E0 + lo * STEP, cHi = E0 + hi * STEP;
  const hold = q => { for (let j = 0; j < K; j++) { q[3 * j] = Math.max(cLo, Math.min(cHi, q[3 * j]));
    q[3 * j + 1] = Math.max(-60, Math.min(-0.01, q[3 * j + 1])); q[3 * j + 2] = Math.max(0.1, Math.min(1.5, q[3 * j + 2])); } return q; };
  const left = q => { const out = new Float64Array(hi - lo + 1);
    for (let k = lo; k <= hi; k++) { let m = 0; for (let j = 0; j < K; j++) { const u = (E0 + k * STEP - q[3 * j]) / q[3 * j + 2]; m += q[3 * j + 1] * Math.exp(-0.5 * u * u); } out[k - lo] = r[k] - m; }
    return out; };
  const sum = v => { let t = 0; for (const x of v) t += x * x; return t; };
  let q = hold(par.flat()), res = left(q), cost = sum(res), damp = 0.01;
  for (let it = 0; it < steps; it++) {
    const JJ = new Float64Array(P * P), Jr = new Float64Array(P), row = new Float64Array(P);
    for (let k = lo; k <= hi; k++) {
      for (let j = 0; j < K; j++) { const a = q[3 * j + 1], s = q[3 * j + 2], u = (E0 + k * STEP - q[3 * j]) / s, g = Math.exp(-0.5 * u * u);
        row[3 * j] = a * g * u / s; row[3 * j + 1] = g; row[3 * j + 2] = a * g * u * u / s; }
      for (let p = 0; p < P; p++) { Jr[p] += row[p] * res[k - lo]; for (let t = 0; t < P; t++) JJ[p * P + t] += row[p] * row[t]; } }
    let better = false;
    for (let tries = 0; tries < 6 && !better; tries++) {
      const M = Float64Array.from(JJ); for (let p = 0; p < P; p++) M[p * P + p] += damp * (JJ[p * P + p] + 1e-9);
      const step = solve(M, Float64Array.from(Jr), P), q2 = hold(q.map((v, i) => v + step[i])), res2 = left(q2), cost2 = sum(res2);
      if (cost2 < cost) { q = q2; res = res2; cost = cost2; damp *= 0.3; better = true; } else damp *= 10;
    }
    if (!better) break;
  }
  return Array.from({ length: K }, (_, j) => [q[3 * j], q[3 * j + 1], q[3 * j + 2]]);
}
// The smooth part and S.dips dips of x, fitted in turn until they agree: the dips to what the smooth part leaves, the
// smooth part the blur of x with the dips taken out. Dips sit between `from` and 18 kHz.
function joint(x) {
  const lo = Math.max(0, Math.round((erbOf(S.from * 1000) - E0) / STEP)), hi = Math.round((erbOf(18000) - E0) / STEP);
  let base = blur(x, S.width), par = [], dips = new Float32Array(NF);
  for (let turn = 0; turn < 5; turn++) {
    const r = x.map((v, k) => v - base[k]);
    if (!turn) for (let j = 0; j < S.dips; j++) {                      // each dip starts at the deepest point still left
      const m = bumps(par); let at = lo, deep = Infinity;
      for (let k = lo; k <= hi; k++) if (r[k] - m[k] < deep) { deep = r[k] - m[k]; at = k; }
      par = refine(r, [...par, [E0 + at * STEP, Math.min(deep, -0.1), 0.3]], lo, hi, 12);
    } else par = refine(r, par, lo, hi, 8);
    dips = bumps(par); base = blur(x.map((v, k) => v - dips[k]), S.width);
  }
  return [base, dips, par];
}
// The CLL in direction d (less its average over the directions, if that is removed), its smooth part and, from the
// joint fit, its dips as a curve and as [centre, depth, sigma] each; below `from` the smooth part is the CLL itself,
// over one ERB.
function smoothOf(A, d) {
  const x = new Float32Array(NF);
  for (let k = 0; k < NF; k++) x[k] = A.cll[d * NF + k] - (S.average === "removed" ? A.avg[k] : 0);
  let b, dips = null, par = null;
  if (S.method === "smooth + dips") [b, dips, par] = joint(x);
  else b = S.method === "blur" ? blur(x, S.width) : S.method === "sheet" ? sheet(x, S.width, S.passes) : maxMin(x, S.notch, S.width);
  const e = erbOf(S.from * 1000);
  for (let k = 0; k < NF; k++) { const t = Math.max(0, Math.min(1, E0 + k * STEP - e + 0.5)), m = t * t * (3 - 2 * t); b[k] = x[k] + (b[k] - x[k]) * m; }
  return [x, b, dips, par];
}
// The smooth parts of many directions as spherical harmonics of the given order, one fit a point of the axis:
// least squares with a penalty on roughness. Returns the coefficients, (order + 1)² x NF.
function fitSphere(H, dirs, bases, order) {
  const K = (order + 1) ** 2, Y = dirs.map(d => shN(order, ...H.xyz[d])), M = new Float64Array(K * K), B = new Float64Array(K * NF);
  for (let i = 0; i < dirs.length; i++) { const y = Y[i], b = bases[i];
    for (let p = 0; p < K; p++) { for (let q = 0; q <= p; q++) M[p * K + q] += y[p] * y[q]; for (let k = 0; k < NF; k++) B[p * NF + k] += y[p] * b[k]; } }
  let trace = 0; for (let p = 0; p < K; p++) trace += M[p * K + p];
  for (let p = 0; p < K; p++) { const n = Math.floor(Math.sqrt(p)); M[p * K + p] += 1e-3 * trace / K * (0.01 + n * (n + 1)); }
  for (let p = 0; p < K; p++) for (let q = 0; q <= p; q++) {          // Cholesky, in place, lower triangle
    let s = M[p * K + q]; for (let j = 0; j < q; j++) s -= M[p * K + j] * M[q * K + j];
    M[p * K + q] = p === q ? Math.sqrt(s) : s / M[q * K + q]; }
  for (let k = 0; k < NF; k++) {
    for (let p = 0; p < K; p++) { let s = B[p * NF + k]; for (let j = 0; j < p; j++) s -= M[p * K + j] * B[j * NF + k]; B[p * NF + k] = s / M[p * K + p]; }
    for (let p = K - 1; p >= 0; p--) { let s = B[p * NF + k]; for (let j = p + 1; j < K; j++) s -= M[j * K + p] * B[j * NF + k]; B[p * NF + k] = s / M[p * K + p]; } }
  return B;
}
