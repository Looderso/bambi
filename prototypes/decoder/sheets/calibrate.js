// How deep and how sharp should the template's notches be? Two answers from the heads, for one depth factor and one
// sharpness (narrower and deeper by a factor, the same area) for all notches:
//   as the heads' own   where a head has a dip at a template notch (within 0.75 ERB of it, the head's one shift in), how
//                       much narrower and how much deeper that dip is than the template's there — the median over all
//                       such dips. A real notch's shape, not a bet on where it is.
//   as the curves       the deepest notch of every direction's CLL (below its own 3-ERB blur, 4–16 kHz) and that notch's
//                       width at half its depth — measured, and of the template's notches for every depth and
//                       sharpness: the factors with which the template's notches are as deep and as narrow as the
//                       measured curves' (medians). The same reading of both, no dip fitted.
//   closest             the factors that leave the least of the measured CLLs, point for point, with the cost as it
//                       is (blurred to one ERB) and without the blur. A notch a little off costs more the deeper and
//                       narrower it is, so this answer hedges: it is the shallow, wide one.
// On every STEP-th head of the ladder's (ladder.json), each with the one shift the ladder fitted. One process.
//
//   node calibrate.js <ladder folder> [STEP, default 16]
const fs = require("fs"), path = require("path");
const here = f => path.join(__dirname, f);
const lab = new Function(["../common.js", "lab.js"].map(f => fs.readFileSync(here(f), "utf8")).join("\n")
  + "\nreturn { parseHead, analyse, fitted, tracksOf, backToInput, targetOf, bumps, blur, shN, NOTCHES, E0, STEP, LO, HI };")();
const [folder, every = "16"] = process.argv.slice(2), L = JSON.parse(fs.readFileSync(path.join(folder, "ladder.json"), "utf8"));
const preset = JSON.parse(fs.readFileSync(here("presets.json"), "utf8")).sizes.find(z => z.surfaces === 6 && z.order === 4).P;
const index = JSON.parse(fs.readFileSync(here("run/index.json"), "utf8")).heads, at = (c, y) => c.reduce((t, v, p) => t + v * y[p], 0);
const lo = lab.E0 + lab.LO * lab.STEP, hi = lab.E0 + lab.HI * lab.STEP, median = v => [...v].sort((a, b) => a - b)[v.length >> 1];
const DEPTHS = [0.5, 0.75, 1, 1.5, 2, 3], SHARPS = [1, 1.5, 2, 2.5, 3, 4], narrower = [], deeper = [], sums = {}; let count = 0, claimed = 0, had = 0;
// A curve's deepest notch at 4–16 kHz below its own 3-ERB blur: [depth dB, width at half depth ERB].
const notchOf = x => { const b = lab.blur(x, 3); let at = lab.NOTCHES[0], deep = 0; for (const k of lab.NOTCHES) if (x[k] - b[k] < deep) { deep = x[k] - b[k]; at = k; }
  let l = at, r = at; while (l > 0 && x[l] - b[l] < deep / 2) l--; while (r < x.length - 1 && x[r] - b[r] < deep / 2) r++; return [deep, (r - l) * lab.STEP]; };
const seen = { measured: [] }; for (const d of DEPTHS) for (const sh of SHARPS) seen[d + "," + sh] = [];
L.heads.forEach((h, i) => { if (i % +every) return;
  const e = index.find(q => q.set === h.set && q.name === h.name && q.ear === h.ear), f = fs.readFileSync(here("heads/" + e.head));
  const H = lab.parseHead(f.buffer.slice(f.byteOffset, f.byteOffset + f.byteLength), e), keep = {}, fit = lab.fitted(H, lab.analyse(H, e.ear === "left" ? 1 : 2), preset, keep);
  const [final] = lab.backToInput(H, keep.near, keep.xs, lab.tracksOf(H, keep.near, fit.targets, fit.dips, preset), null, 4, preset.floor), shift = h.fit.one[0];
  const maps = H.xyz.map(v => { const y = lab.shN(L.order, ...v); return L.template.map(t => [at(t.centre, y) + shift, Math.min(0, at(t.depth, y)), Math.max(0.1, Math.min(1.5, at(t.width, y)))]); });
  maps.forEach((notches, d) => { const own = final[d].filter(p => p && p[1] <= -1.5), used = new Set();
    notches.forEach(([c, a, w]) => { if (a > -1) return; claimed++; let best = -1, gap = 0.75;
      own.forEach((p, j) => { if (!used.has(j) && Math.abs(p[0] - c) < gap) { gap = Math.abs(p[0] - c); best = j; } });
      if (best < 0) return; used.add(best); had++; narrower.push(w / own[best][2]); deeper.push(own[best][1] / a); }); });
  for (const depth of DEPTHS) for (const sharp of SHARPS) { let blurred = 0, plain = 0;
    for (let d = 0; d < H.n; d += 2) { const dips = maps[d].filter(([, a]) => a * depth <= -0.5).map(([c, a, w]) => [Math.max(lo, Math.min(hi, c)), Math.max(-60, a * depth * sharp), Math.max(0.08, w / sharp)]);
      const r = lab.targetOf(keep.xs[d], dips), m = lab.bumps(dips), err = r.map((v, k) => v - m[k]), soft = lab.blur(err, 1);
      seen[depth + "," + sharp].push(notchOf(m)); if (depth === DEPTHS[0] && sharp === SHARPS[0]) seen.measured.push(notchOf(keep.xs[d]));
      for (const k of lab.NOTCHES) { plain += err[k] ** 2; blurred += soft[k] ** 2; } }
    const key = depth + "," + sharp, n = Math.ceil(H.n / 2) * lab.NOTCHES.length; sums[key] = sums[key] || [0, 0]; sums[key][0] += Math.sqrt(blurred / n); sums[key][1] += Math.sqrt(plain / n); }
  count++; });
const sharp = median(narrower), depth = median(deeper) / sharp;
console.log(`${count} heads. The template claims a notch (deeper than 1 dB) in ${claimed} places; the head has a dip there in ${Math.round(100 * had / claimed)} % of them.`);
console.log(`As the heads' own: their dips are ${sharp.toFixed(2)} times narrower than the template's (median) and ${median(deeper).toFixed(2)} times deeper  →  sharpness × ${sharp.toFixed(1)}, depth × ${depth.toFixed(2)}`);
const read = list => { const deep = list.map(q => q[0]), wide = list.filter(q => q[0] < -3).map(q => q[1]); return [median(deep), median(wide), list.filter(q => q[0] < -6).length / list.length]; };
const real = read(seen.measured);
console.log(`\nAs the curves: a direction's deepest notch, measured: ${real[0].toFixed(1)} dB deep (median), ${real[1].toFixed(2)} ERB wide at half depth, deeper than 6 dB in ${Math.round(100 * real[2])} % of the directions.`);
console.log(`The template's, depth [dB] / width [ERB] / deeper than 6 dB [%]; columns: sharpness ${SHARPS.join("  ")}`);
let like = null;
for (const d of DEPTHS) console.log(`  depth × ${String(d).padEnd(4)} ` + SHARPS.map(sh => { const t = read(seen[d + "," + sh]), off = Math.abs(Math.log(t[0] / real[0])) + Math.abs(Math.log(t[1] / real[1]));
  if (!like || off < like[0]) like = [off, d, sh]; return `${t[0].toFixed(1)}/${t[1].toFixed(2)}/${Math.round(100 * t[2])}`; }).join("  "));
console.log(`  nearest the measured in depth and width: depth × ${like[1]}, sharpness × ${like[2]}`);
for (const [which, name] of [[0, "the cost as it is, blurred to one ERB"], [1, "without the blur"]]) {
  console.log(`\nClosest, ${name} — what is left, dB (mean over the heads); columns: sharpness ${SHARPS.join("  ")}`);
  let best = null;
  for (const d of DEPTHS) { console.log(`  depth × ${String(d).padEnd(4)} ` + SHARPS.map(s => { const v = sums[d + "," + s][which] / count; if (!best || v < best[0]) best = [v, d, s]; return v.toFixed(3); }).join("  ")); }
  console.log(`  least: depth × ${best[1]}, sharpness × ${best[2]} (${best[0].toFixed(3)} dB)`);
}
