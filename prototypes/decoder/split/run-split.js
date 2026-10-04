// Every head of heads/index.json, made symmetric from its left and from its right ear, split into three parts with
// the page's own code (split.js):
//   the average   the CLL's mean over the measured directions, one curve on the axis
//   the pattern   the CLL's smooth part less that average, as spherical harmonics of ORDER over the sphere
//   the dips      DIPS dips a direction, each centre (ERB), depth (dB), sigma (ERB)
// The smooth part and the dips are fitted together (blur WIDTH ERB, dips above FROM kHz). Writes, per head and ear,
// <out>/<set>/<name>-<left|right>.f32 — float32: the average (NF); the pattern's coefficients ((ORDER+1)² x NF, SN3D,
// ACN); the dips (directions x DIPS x 3); per direction what is left over, neither smooth nor dip, dB RMS at 4–16 kHz;
// per direction how far the pattern is from the free smooth part, dB RMS at 0.3–16 kHz; the ITD per direction, µs —
// and <out>/manifest-<part>.json. Run as several processes at once, each taking every <of>-th head:
//
//   node run-split.js <out folder> [<part> <of>]
const fs = require("fs"), path = require("path");
const here = f => path.join(__dirname, f);
const [out, part = "0", of = "1"] = process.argv.slice(2);
const SETTINGS = { flat: 20, method: "smooth + dips", width: 3, dips: 4, from: 4, average: "removed" }, ORDER = 4;

// common.js and split.js are browser scripts: run as one function body, with the settings as their S.
const source = ["../common.js", "split.js"].map(f => fs.readFileSync(here(f), "utf8")).join("\n");
const lab = new Function("S", source + "\nreturn { parseHead, analyse, smoothOf, fitSphere, shN, inBand, NF, E0, STEP };")(SETTINGS);
const { NF } = lab, notches = lab.inBand(4000, 16000), wide = lab.inBand(300, 16000), K = (ORDER + 1) ** 2;

const index = JSON.parse(fs.readFileSync(here("heads/index.json"), "utf8")), done = [];
index.heads.forEach((entry, i) => {
  if (i % +of !== +part) return;
  const file = fs.readFileSync(here("heads/" + entry.file));
  const H = lab.parseHead(file.buffer.slice(file.byteOffset, file.byteOffset + file.byteLength), entry);
  for (const variant of [1, 2]) {
    const A = lab.analyse(H, variant), n = H.n, bases = [], dips = new Float32Array(n * SETTINGS.dips * 3), over = new Float32Array(n), off = new Float32Array(n);
    for (let d = 0; d < n; d++) {
      const [x, base, curve, par] = lab.smoothOf(A, d);
      bases.push(base); par.forEach((p, j) => dips.set(p, (d * SETTINGS.dips + j) * 3));
      let e = 0; for (const k of notches) e += (x[k] - base[k] - curve[k]) ** 2;
      over[d] = Math.sqrt(e / notches.length);
    }
    const stride = Math.ceil(n / 1000), dirs = []; for (let d = 0; d < n; d += stride) dirs.push(d);
    const C = lab.fitSphere(H, dirs, dirs.map(d => bases[d]), ORDER);
    for (let d = 0; d < n; d++) { const y = lab.shN(ORDER, ...H.xyz[d]); let e = 0;
      for (const k of wide) { let v = 0; for (let p = 0; p < K; p++) v += y[p] * C[p * NF + k]; e += (v - bases[d][k]) ** 2; }
      off[d] = Math.sqrt(e / wide.length); }
    const name = `${entry.set}/${entry.name}-${variant === 1 ? "left" : "right"}.f32`;
    fs.mkdirSync(path.join(out, entry.set), { recursive: true });
    fs.writeFileSync(path.join(out, name), Buffer.concat([A.avg, Float32Array.from(C), dips, over, off, A.itd].map(a => Buffer.from(a.buffer, a.byteOffset, a.byteLength))));
    done.push({ set: entry.set, name: entry.name, ear: variant === 1 ? "left" : "right", file: name, head: entry.file, directions: n });
  }
});
fs.writeFileSync(path.join(out, `manifest-${part}.json`), JSON.stringify({ settings: SETTINGS, order: ORDER, nf: NF, e0: lab.E0, step: lab.STEP, heads: done }));
console.log(`part ${part} of ${of}: ${done.length} symmetric heads`);
