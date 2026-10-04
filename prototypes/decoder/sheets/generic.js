// Nobody's head: the population's average of every part but the notches — the CLL's average over the directions
// (each head's brought to one level first: a measurement's level is arbitrary), the smooth pattern, the ITD's pattern
// and the ILD's — from run-sheets.js's run. Each database's mean first, then the mean of those, so no database's
// way of measuring weighs more than another's. Writes <run folder>/generic.json.
//
//   node generic.js <run folder>
const fs = require("fs"), path = require("path");
const folder = process.argv[2], SETS = ["HUTUBS", "3D3A", "SONICOM"];
const lab = new Function(["../common.js", "lab.js"].map(f => fs.readFileSync(path.join(__dirname, f), "utf8")).join("\n") + "\nreturn { inBand };")();
const sums = {}, counts = {};
let sizes = null;
for (const f of fs.readdirSync(folder).filter(f => /^part-\d+\.json$/.test(f))) {
  const m = JSON.parse(fs.readFileSync(path.join(folder, f), "utf8")), raw = fs.readFileSync(path.join(folder, f.replace("part-", "parts-").replace(".json", ".f32")));
  const x = new Float32Array(raw.buffer, raw.byteOffset, raw.byteLength / 4), nf = m.nf;
  sizes = { average: nf, pattern: 25 * nf, itd: (m.itd + 1) ** 2, ild: (m.ild + 1) ** 2 * nf, nf, itdOrder: m.itd, ildOrder: m.ild };
  const each = sizes.average + sizes.pattern + sizes.itd + sizes.ild, level = lab.inBand(300, 16000);
  m.heads.forEach((h, i) => { if (!SETS.includes(h.set)) return;
    const v = Float64Array.from(x.subarray(i * each, (i + 1) * each)), mean = level.reduce((t, k) => t + v[k], 0) / level.length;
    for (let k = 0; k < nf; k++) v[k] -= mean;
    if (!sums[h.set]) { sums[h.set] = new Float64Array(each); counts[h.set] = 0; }
    for (let k = 0; k < each; k++) sums[h.set][k] += v[k]; counts[h.set]++; });
}
const each = sizes.average + sizes.pattern + sizes.itd + sizes.ild, mean = new Float64Array(each);
for (const s of SETS) for (let k = 0; k < each; k++) mean[k] += sums[s][k] / counts[s] / SETS.length;
const cut = (from, n) => Array.from(mean.subarray(from, from + n), v => +v.toFixed(4));
fs.writeFileSync(path.join(folder, "generic.json"), JSON.stringify({ heads: counts, itdOrder: sizes.itdOrder, ildOrder: sizes.ildOrder,
  average: cut(0, sizes.average), pattern: cut(sizes.average, sizes.pattern), itd: cut(sizes.average + sizes.pattern, sizes.itd), ild: cut(sizes.average + sizes.pattern + sizes.itd, sizes.ild) }));
console.log(`nobody's head from ${SETS.map(s => s + " " + counts[s]).join(", ")} heads; wrote ${path.join(folder, "generic.json")}, ${(fs.statSync(path.join(folder, "generic.json")).size / 1e6).toFixed(1)} MB`);
