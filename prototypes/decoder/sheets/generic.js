// Nobody's head: the population's average of every part but the notches — the CLL's average over the directions
// (each head's brought to one level first: a measurement's level is arbitrary), the smooth pattern, the ITD's pattern
// and the ILD's — from run-sheets.js's run. Each database's mean first, then the mean of those, so no database's
// way of measuring weighs more than another's. And what goes with a head's size: a head's ITD is the mean ITD times a
// scale, and the ILD's change a unit of that scale is learned over the heads (a database's own mean taken out first)
// — kept below 1.5 kHz, faded out by 3 kHz: above, the ILD does not follow the head's size. Writes
// <run folder>/generic.json — with the KU100's timbre beside it: its CLL's mean over the sphere (the average over the
// directions plus the smooth pattern's order 0), at the same level, for a head to take in place of its own.
//
//   node generic.js <run folder>            WITHOUT=SONICOM leaves that database out
const fs = require("fs"), path = require("path");
const skip = (process.env.WITHOUT || "").split(",").filter(Boolean), tag = skip.length ? "-without-" + skip.join("-").toLowerCase() : "";   // WITHOUT=SONICOM: that database left out
const folder = process.argv[2], SETS = ["HUTUBS", "3D3A", "SONICOM"].filter(s => !skip.includes(s));
const lab = new Function(["../common.js", "lab.js"].map(f => fs.readFileSync(path.join(__dirname, f), "utf8")).join("\n") + "\nreturn { inBand, FREQ, erbOf };")();
const all = [];                                                            // every head's parts, for the second pass
const sums = {}, counts = {};
let sizes = null, dummy = null;
for (const f of fs.readdirSync(folder).filter(f => /^part-\d+\.json$/.test(f))) {
  const m = JSON.parse(fs.readFileSync(path.join(folder, f), "utf8")), raw = fs.readFileSync(path.join(folder, f.replace("part-", "parts-").replace(".json", ".f32")));
  const x = new Float32Array(raw.buffer, raw.byteOffset, raw.byteLength / 4), nf = m.nf;
  sizes = { average: nf, pattern: 25 * nf, itd: (m.itd + 1) ** 2, ild: (m.ild + 1) ** 2 * nf, nf, itdOrder: m.itd, ildOrder: m.ild };
  const each = sizes.average + sizes.pattern + sizes.itd + sizes.ild, level = lab.inBand(300, 16000);
  m.heads.forEach((h, i) => {
    if (h.set === "KU100" && h.ear === "left") { const t = Float64Array.from({ length: nf }, (_, k) => x[i * each + k] + x[i * each + nf + k]), at = level.reduce((a, k) => a + t[k], 0) / level.length; dummy = Array.from(t, v => +(v - at).toFixed(4)); }
    if (!SETS.includes(h.set)) return;
    const v = Float64Array.from(x.subarray(i * each, (i + 1) * each)), mean = level.reduce((t, k) => t + v[k], 0) / level.length;
    for (let k = 0; k < nf; k++) v[k] -= mean;
    if (!sums[h.set]) { sums[h.set] = new Float64Array(each); counts[h.set] = 0; }
    for (let k = 0; k < each; k++) sums[h.set][k] += v[k]; counts[h.set]++; all.push([h.set, v]); });
}
const each = sizes.average + sizes.pattern + sizes.itd + sizes.ild, mean = new Float64Array(each);
for (const s of SETS) for (let k = 0; k < each; k++) mean[k] += sums[s][k] / counts[s] / SETS.length;
const cut = (from, n) => Array.from(mean.subarray(from, from + n), v => +v.toFixed(4));
// The ILD's change a unit of ITD scale: least squares through nobody's head, every database weighing the same.
const itdAt = sizes.average + sizes.pattern, ildAt = itdAt + sizes.itd, slope = new Float64Array(sizes.ild), sphere = p => 1 / (2 * Math.floor(Math.sqrt(p)) + 1);
let norm = 0, spread = 0; for (let p = 0; p < sizes.itd; p++) norm += mean[itdAt + p] ** 2 * sphere(p);
for (const [set, v] of all) { const w = 1 / counts[set]; let dot = 0;
  for (let p = 0; p < sizes.itd; p++) dot += (v[itdAt + p] - sums[set][itdAt + p] / counts[set] + mean[itdAt + p]) * mean[itdAt + p] * sphere(p);
  const ds = dot / norm - 1; spread += w * ds * ds;
  for (let k = 0; k < sizes.ild; k++) slope[k] += w * ds * (v[ildAt + k] - sums[set][ildAt + k] / counts[set]); }
const fade = Array.from(lab.FREQ, f => { const t = Math.max(0, Math.min(1, (lab.erbOf(3000) - lab.erbOf(f)) / (lab.erbOf(3000) - lab.erbOf(1500)))); return t * t * (3 - 2 * t); });
for (let k = 0; k < sizes.ild; k++) slope[k] = slope[k] / spread * fade[k % sizes.nf];
fs.writeFileSync(path.join(folder, `generic${tag}.json`), JSON.stringify({ heads: counts, itdOrder: sizes.itdOrder, ildOrder: sizes.ildOrder,
  average: cut(0, sizes.average), pattern: cut(sizes.average, sizes.pattern), itd: cut(sizes.average + sizes.pattern, sizes.itd), ild: cut(sizes.average + sizes.pattern + sizes.itd, sizes.ild),
  timbreKU100: dummy, ildWithSize: Array.from(slope, v => +v.toFixed(4)), sizeSpread: +Math.sqrt(spread / SETS.length).toFixed(4) }));
console.log(`a head's ITD scale spreads by ${Math.sqrt(spread / SETS.length).toFixed(3)}; the ILD's change with it kept below 1.5 kHz, gone by 3 kHz`);
console.log(`nobody's head from ${SETS.map(s => s + " " + counts[s]).join(", ")} heads; wrote ${path.join(folder, `generic${tag}.json`)}, ${(fs.statSync(path.join(folder, `generic${tag}.json`)).size / 1e6).toFixed(1)} MB`);
