// What the plugin's head synthesiser must reproduce: for a few directions and slider settings, the two ears'
// magnitudes (dB) at the model's axis points and the ITD (µs), computed here from head.json by the recipe itself —
// the same formulas the listening page plays. Writes a JSON the core tests read.
//
//   node export-fixtures.js <head.json> <out fixtures.json>
const fs = require("fs"), path = require("path");
const [modelFile, out] = process.argv.slice(2), M = JSON.parse(fs.readFileSync(modelFile, "utf8"));
const lab = new Function(["../common.js", "lab.js"].map(f => fs.readFileSync(path.join(__dirname, f), "utf8")).join("\n") + "\nreturn { shN };")();
const { erb0, step, points } = M.axis, at = (coef, y, k) => { let v = 0; for (let p = 0; p < y.length; p++) v += y[p] * coef[p * points + k]; return v; };
const dir = (az, el) => [Math.cos(el * Math.PI / 180) * Math.cos(az * Math.PI / 180), Math.cos(el * Math.PI / 180) * Math.sin(az * Math.PI / 180), Math.sin(el * Math.PI / 180)];
const directions = [[0, 0], [90, 0], [-30, 30], [180, 0], [0, 90], [0, -45], [135, 20]], settings = [[0, 1], [0.8, 0.9], [-0.6, 1.15]];
const cases = [];
for (const [az, el] of directions) for (const [shift, size] of settings) {
  const v = dir(az, el), ys = o => lab.shN(o, ...v), yS = ys(M.smooth.order), yI = ys(M.ild.order), yT = ys(M.itd.order), yN = ys(M.notches.order);
  const one = (coef, y) => coef.reduce((t, c, p) => t + c * y[p], 0);
  const notches = M.notches.list.map(n => { const p = one(n.presence, yN), t = Math.max(0, Math.min(1, (p - M.notches.fade[0]) / (M.notches.fade[1] - M.notches.fade[0])));
    return { c: Math.max(M.notches.band[0], Math.min(M.notches.band[1], one(n.centre, yN) + shift)), a: Math.max(-60, Math.min(0, one(n.depth, yN)) * M.notches.depthFactor * t * t * (3 - 2 * t)),
             w: Math.max(M.notches.widthRange[0], Math.min(M.notches.widthRange[1], one(n.width, yN))) }; });
  const left = [], right = [];
  for (let k = 0; k < points; k++) { const e = erb0 + k * step;
    let cll = at(M.smooth.coef, yS, k); for (const n of notches) { const u = (e - n.c) / n.w; cll += n.a / (1 + u * u); }
    const r = 10 ** ((at(M.ild.coef, yI, k) + (size - 1) * at(M.ild.withSize, yI, k)) / 10);
    left.push(+(cll + 10 * Math.log10(r / (1 + r))).toFixed(4)); right.push(+(cll - 10 * Math.log10(1 + r)).toFixed(4)); }
  cases.push({ azimuth: az, elevation: el, earShift: shift, headSize: size, left, right, itd: +(one(M.itd.coef, yT) * size).toFixed(3), notches: notches.map(n => [+n.c.toFixed(4), +n.a.toFixed(4), +n.w.toFixed(4)]) });
}
fs.writeFileSync(out, JSON.stringify({ model: path.basename(modelFile), cases }));
console.log(`wrote ${out}: ${cases.length} cases`);
