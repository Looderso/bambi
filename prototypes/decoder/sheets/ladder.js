// How far do a few numbers a head get? A template — three notch surfaces, the same for every head, each its centre,
// depth and width over the sphere as spherical harmonics of order ORDER, symmetric left to right — and per head a
// shift along frequency and a depth for each notch. The template is learned from half of the people, in turns: each
// head's numbers fitted to its measured CLLs with the template held; then, with the numbers held, each head's own
// dips (found and checked against its spectrum, lab.js) given to the template notch they lie nearest, the shifts
// taken out, and the template fitted to all of them. Then two templates, each head taking the one that fits it.
//
// The ladder, every rung scored with lab.js's cost over all heads: no notches; the KU100's notches for everyone, as
// they are and with one shift; the template as it is, with one shift, with a shift a notch, with a depth a notch too,
// with the choice of two templates; and a head's own six surfaces. Prints it for the people the template was learned
// from and for the others, and writes <out>/ladder.json: the templates, and every head's costs and numbers.
//
//   node ladder.js <out folder> <folder of run-sheets.js's six-surface run> <folder of its three-surface run> [processes, default 8]
const fs = require("fs"), path = require("path"), { fork } = require("child_process");
const here = f => path.join(__dirname, f);
const lab = new Function(["../common.js", "lab.js"].map(f => fs.readFileSync(here(f), "utf8")).join("\n")
  + "\nreturn { parseHead, analyse, fitted, tracksOf, backToInput, costOf, shN, solve, E0, STEP, LO, HI };")();
const NOTCHES = 3, ORDER = 6, COEF = (ORDER + 1) ** 2, SHIFTS = Array.from({ length: 9 }, (_, i) => (i - 4) * 0.5), DEPTHS = [0, 0.5, 0.75, 1, 1.25, 1.5, 2, 2.5];
const at = (coef, y) => { let v = 0; for (let p = 0; p < coef.length; p++) v += y[p] * coef[p]; return v; };
const mirror = ([x, y, z]) => [x, -y, z];

if (process.argv[2] === "child") {
  const preset = JSON.parse(fs.readFileSync(here("presets.json"), "utf8")).sizes.find(z => z.surfaces === 6 && z.order === 4).P;
  const heads = [], lo = lab.E0 + lab.LO * lab.STEP, hi = lab.E0 + lab.HI * lab.STEP;
  // A set of surfaces (coefficients of some order) on a head's directions: per surface centre, depth and width a direction.
  const onHead = (h, surfaces) => surfaces.map(s => { const order = Math.sqrt(s.centre.length) - 1, ys = h.H.xyz.map(v => lab.shN(order, ...v));
    return { c: ys.map(y => at(s.centre, y)), a: ys.map(y => Math.min(0, at(s.depth, y))), w: ys.map(y => Math.max(0.1, Math.min(1.5, at(s.width, y)))) }; });
  // The dips a direction of surfaces on a head, each shifted and its depth scaled; and their cost — on every third
  // direction while the numbers are sought, on all for the cost reported.
  const dipsOf = (maps, shift, gain, d) => maps.map((m, k) => m.a[d] * gain[k] > -0.5 ? null : [Math.max(lo, Math.min(hi, m.c[d] + shift[k])), Math.max(-60, m.a[d] * gain[k]), m.w[d]]);
  const costOf = (h, maps, shift, gain, every = 3) => { const dirs = h.xs.map((_, d) => d).filter(d => d % every === 0);
    return lab.costOf(dirs.map(d => h.xs[d]), dirs.map(d => dipsOf(maps, shift, gain, d))); };
  // The rungs for one set of surfaces: as it is; one shift for all; a shift a surface; a depth a surface too.
  function climb(h, maps, full) {
    const n = maps.length, ones = maps.map(() => 1), zero = maps.map(() => 0), scan = (make, values) => values.reduce((b, v) => { const c = costOf(h, maps, ...make(v)); return c < b[1] ? [v, c] : b; }, [null, Infinity]);
    const shifted = make => { const coarse = scan(make, SHIFTS), fine = scan(make, [coarse[0] - 0.25, coarse[0] + 0.25]); return fine[1] < coarse[1] ? fine[0] : coarse[0]; };   // half-ERB steps, then a quarter
    const one = shifted(v => [maps.map(() => v), ones]), out = { one: maps.map(() => one) };
    if (!full) return { ...out, costs: [costOf(h, maps, zero, ones, 1), costOf(h, maps, out.one, ones, 1)] };
    let shift = [...out.one], gain = [...ones];
    for (let pass = 0; pass < 2; pass++) for (let k = 0; k < n; k++) shift[k] = shifted(v => [shift.map((s, j) => j === k ? v : s), ones]);
    const each = [...shift];
    for (let k = 0; k < n; k++) gain[k] = scan(v => [shift, gain.map((g, j) => j === k ? v : g)], DEPTHS)[0];
    for (let k = 0; k < n; k++) shift[k] = scan(v => [shift.map((s, j) => j === k ? v : s), gain], [-0.5, -0.25, 0, 0.25, 0.5].map(v => v + shift[k]))[0];
    return { ...out, each, shift, gain, costs: [costOf(h, maps, zero, ones, 1), costOf(h, maps, out.one, ones, 1), costOf(h, maps, each, ones, 1), costOf(h, maps, shift, gain, 1)] };
  }
  // What a head gives the template it was fitted with: its own dips, each given to the notch it lies nearest (within
  // 0.75 ERB, one a notch and direction), the head's shift taken out — as the sums a least-squares fit needs, the
  // head's mirror image with it. The depth is taken as it is, 0 where the head has no such dip: the template's depth
  // is the heads' average.
  function sums(h, maps, fit) {
    const out = maps.map(() => ({ Mc: new Float64Array(COEF * COEF), bc: new Float64Array(COEF), bw: new Float64Array(COEF), Md: new Float64Array(COEF * COEF), bd: new Float64Array(COEF) }));
    h.H.xyz.forEach((v, d) => { const pairs = [];
      h.dips[d].forEach((p, i) => maps.forEach((m, k) => { const gap = Math.abs(p[0] - m.c[d] - fit.shift[k]); if (gap <= 0.75) pairs.push([gap, i, k]); }));
      const got = maps.map(() => null), used = new Set();
      for (const [, i, k] of pairs.sort((a, b) => a[0] - b[0])) if (!got[k] && !used.has(i)) { got[k] = h.dips[d][i]; used.add(i); }
      for (const u of [v, mirror(v)]) { const y = lab.shN(ORDER, ...u);
        maps.forEach((_, k) => { const s = out[k], p = got[k], depth = p ? p[1] : 0, weight = p ? Math.min(1, -p[1] / 6) : 0;
          for (let a = 0; a < COEF; a++) { s.bd[a] += y[a] * depth; if (p) { s.bc[a] += weight * y[a] * (p[0] - fit.shift[k]); s.bw[a] += weight * y[a] * p[2]; }
            for (let b = 0; b <= a; b++) { s.Md[a * COEF + b] += y[a] * y[b]; if (p) s.Mc[a * COEF + b] += weight * y[a] * y[b]; } } }); } });
    return out;
  }
  process.on("message", msg => {
    if (msg.load) { for (const e of msg.load) { const f = fs.readFileSync(here("heads/" + e.head)), H = lab.parseHead(f.buffer.slice(f.byteOffset, f.byteOffset + f.byteLength), e);
        const A = lab.analyse(H, e.ear === "left" ? 1 : 2), keep = {}, fit = lab.fitted(H, A, preset, keep);
        const [final] = lab.backToInput(H, keep.near, keep.xs, lab.tracksOf(H, keep.near, fit.targets, fit.dips, preset), null, 4, preset.floor);
        heads.push({ e, H, xs: keep.xs, dips: final.map(p => p.filter(q => q && q[1] <= -1.5)), last: null }); }
      return process.send({ loaded: heads.length }); }
    if (msg.templates) return process.send(heads.map(h => { const none = lab.costOf(h.xs, h.xs.map(() => []));
      const dummy = msg.dummy ? climb(h, onHead(h, msg.dummy), false).costs : null;
      h.last = msg.templates.map(t => { const maps = onHead(h, t); return { maps, fit: climb(h, maps, true) }; });
      return { none, dummy, fits: h.last.map(l => ({ ...l.fit })) }; }));
    if (msg.take) { const total = msg.count.map(() => null);                // take: per head the template it gives its dips to, or -1
      heads.forEach((h, i) => { const t = msg.take[i]; if (t < 0) return; const l = h.last[Math.min(t, h.last.length - 1)], s = sums(h, l.maps, l.fit);   // fitted with one template, given to two: as it was fitted
        if (!total[t]) total[t] = s; else s.forEach((q, k) => { for (const key of Object.keys(q)) for (let a = 0; a < q[key].length; a++) total[t][k][key][a] += q[key][a]; }); });
      return process.send(total.map(t => t && t.map(q => Object.fromEntries(Object.entries(q).map(([k, v]) => [k, Array.from(v)]))))); }
  });
} else (async () => {
  const [out, six, three, procs = "8"] = process.argv.slice(2), count = +procs, started = Date.now(), minutes = () => ((Date.now() - started) / 60000).toFixed(1);
  const every = +process.env.EVERY || 1;                                  // EVERY=n: every n-th head only, to try the script
  const index = JSON.parse(fs.readFileSync(here("run/index.json"), "utf8")).heads.filter((h, i) => i % every === 0 && ["HUTUBS", "3D3A", "SONICOM"].includes(h.set));
  const people = [...new Set(index.map(h => h.set + " " + h.name))];
  const learnt = new Set(people.filter((_, i) => i % 2 === 0)), isLearnt = h => learnt.has(h.set + " " + h.name), isOther = h => people.includes(h.set + " " + h.name) && !isLearnt(h);
  const children = Array.from({ length: count }, () => fork(__filename, ["child"]));
  for (const c of children) c.on("exit", code => { if (code) { console.error("a worker stopped with an error; stopping"); children.forEach(o => o.kill()); process.exit(1); } });
  const mine = i => index.map((h, k) => k).filter(k => k % count === i);
  const ask = msgOf => Promise.all(children.map((c, i) => new Promise(done => { c.once("message", done); c.send(msgOf(i)); })));
  const spread = parts => { const all = new Array(index.length); parts.forEach((p, i) => mine(i).forEach((k, j) => { all[k] = p[j]; })); return all; };
  await ask(i => ({ load: mine(i).map(k => index[k]) }));
  console.log(`${index.length} heads loaded, their dips found, ${minutes()} min`);

  // A template from sums: least squares with a penalty on roughness, a notch's centre, width and depth each.
  const solved = (M, b) => { const A = Float64Array.from(M); let trace = 0; for (let a = 0; a < COEF; a++) { trace += A[a * COEF + a]; for (let c = a + 1; c < COEF; c++) A[a * COEF + c] = A[c * COEF + a]; }
    for (let a = 0; a < COEF; a++) { const n = Math.floor(Math.sqrt(a)); A[a * COEF + a] += 1e-3 * trace / COEF * (0.01 + n * (n + 1)); }
    return Array.from(lab.solve(A, Float64Array.from(b), COEF)); };
  const templateOf = total => total.map(q => ({ centre: solved(q.Mc, q.bc), width: solved(q.Mc, q.bw), depth: solved(q.Md, q.bd) }));
  const added = parts => parts.reduce((t, p) => { p.forEach((q, f) => { if (!q) return; if (!t[f]) { t[f] = q; return; }
    q.forEach((s, k) => { for (const key of Object.keys(s)) for (let a = 0; a < s[key].length; a++) t[f][k][key][a] += s[key][a]; }); }); return t; }, []);
  const learn = async (take, families) => added(await ask(i => ({ take: mine(i).map(k => take(k)), count: Array.from({ length: families }) }))).map(templateOf);

  // The first template: the three typical surfaces of the three-surface run, on its half sphere and mirrored.
  const typical = JSON.parse(fs.readFileSync(path.join(three, "template.json"), "utf8"));
  let template = typical.groups.slice(0, NOTCHES).map(g => { const q = { Mc: new Float64Array(COEF * COEF), bc: new Float64Array(COEF), bw: new Float64Array(COEF), Md: new Float64Array(COEF * COEF), bd: new Float64Array(COEF) };
    typical.grid.forEach((v, i) => { for (const u of [v, mirror(v)]) { const y = lab.shN(ORDER, ...u), has = g.depth[i] < -1;
      for (let a = 0; a < COEF; a++) { q.bd[a] += y[a] * g.depth[i]; if (has) { q.bc[a] += y[a] * g.centre[i]; q.bw[a] += y[a] * 0.4; }
        for (let b = 0; b <= a; b++) { q.Md[a * COEF + b] += y[a] * y[b]; if (has) q.Mc[a * COEF + b] += y[a] * y[b]; } } } });
    return templateOf([q])[0]; });
  const sixRun = fs.readdirSync(six).filter(f => /^part-\d+\.json$/.test(f)).flatMap(f => JSON.parse(fs.readFileSync(path.join(six, f), "utf8")).heads);
  const dummy = sixRun.find(h => h.set === "KU100" && h.ear === "left").surfaces.filter(Boolean);
  const median = v => { const s = v.filter(Number.isFinite).sort((a, b) => a - b); return s[s.length >> 1]; };
  let result;
  for (let turn = 0; turn < 4; turn++) {
    result = spread(await ask(() => ({ templates: [template], dummy: turn === 3 ? dummy : null })));
    console.log(`turn ${turn + 1}: the template with a shift and a depth a notch leaves ${median(result.filter((_, k) => isLearnt(index[k])).map(r => r.fits[0].costs[3])).toFixed(3)} dB on the people it learns from, `
      + `${median(result.filter((_, k) => isOther(index[k])).map(r => r.fits[0].costs[3])).toFixed(3)} on the others, ${minutes()} min`);
    if (turn < 3) [template] = await learn(k => isLearnt(index[k]) ? 0 : -1, 1);
  }
  const single = result, one = template;
  // Two templates: the worse-fitting half of the heads first make their own; then each head takes the better.
  const cut = median(single.filter((_, k) => isLearnt(index[k])).map(r => r.fits[0].costs[3]));
  let pairOf = await learn(k => !isLearnt(index[k]) ? -1 : single[k].fits[0].costs[3] > cut ? 1 : 0, 2), both;
  for (let turn = 0; turn < 2; turn++) {
    both = spread(await ask(() => ({ templates: pairOf })));
    const pick = k => both[k].fits[1].costs[3] < both[k].fits[0].costs[3] ? 1 : 0;
    console.log(`two templates, turn ${turn + 1}: ${index.filter((_, k) => pick(k)).length} heads take the second; the better leaves ${median(both.filter((_, k) => isOther(index[k])).map((r, i) => Math.min(r.fits[0].costs[3], r.fits[1].costs[3]))).toFixed(3)} dB on the others, ${minutes()} min`);
    if (turn < 1) pairOf = await learn(k => isLearnt(index[k]) ? pick(k) : -1, 2);
  }
  children.forEach(c => c.kill());

  const own = {}; for (const h of sixRun) own[h.set + h.name + h.ear] = h.cost;
  const rungs = [["no notches: the smooth part alone", 0, k => single[k].none], ["the KU100's notches for everyone", 0, k => single[k].dummy[0]], ["the KU100's notches, one shift", 1, k => single[k].dummy[1]],
    ["the template as it is", 0, k => single[k].fits[0].costs[0]], ["the template, one shift for all notches", 1, k => single[k].fits[0].costs[1]], ["the template, a shift a notch", 3, k => single[k].fits[0].costs[2]],
    ["the template, a shift and a depth a notch", 6, k => single[k].fits[0].costs[3]], ["the better of two templates, a shift and a depth a notch", 7, k => Math.min(both[k].fits[0].costs[3], both[k].fits[1].costs[3])],
    ["the head's own six surfaces", 450, k => own[index[k].set + index[k].name + index[k].ear]]];
  const groups = [["the people learned from", isLearnt], ["the other people", isOther]];
  console.log(`\nThe ladder: the cost left, dB, median over the heads (${groups.map(g => g[0] + ": " + index.filter(g[1]).length + " heads").join("; ")})`);
  console.log("numbers a head".padStart(16) + "   learned from   the others   of the way from the KU100 to the head's own surfaces (the others)");
  const med = (get, is) => median(index.map((h, k) => is(h) ? get(k) : NaN)), from = med(rungs[1][2], isOther), to = med(rungs[8][2], isOther);
  for (const [name, numbers, get] of rungs) console.log(String(numbers).padStart(16) + med(get, isLearnt).toFixed(3).padStart(14) + med(get, isOther).toFixed(3).padStart(13)
    + (100 * (from - med(get, isOther)) / (from - to)).toFixed(0).padStart(8) + " %   " + name);
  const fits = index.map((h, k) => single[k].fits[0]), col = (key, j) => fits.filter((_, k) => isOther(index[k])).map(f => f[key][j]);
  const sd = v => { const m = v.reduce((a, b) => a + b) / v.length; return Math.sqrt(v.reduce((a, b) => a + (b - m) ** 2, 0) / v.length); };
  const corr = (u, v) => { const mu = u.reduce((a, b) => a + b) / u.length, mv = v.reduce((a, b) => a + b) / v.length; return u.reduce((t, x, i) => t + (x - mu) * (v[i] - mv), 0) / u.length / sd(u) / sd(v); };
  console.log("\nThe numbers, the other people: a notch's shift, spread " + [0, 1, 2].map(j => sd(col("shift", j)).toFixed(2)).join(" ") + " ERB; shifts against each other "
    + [[0, 1], [0, 2], [1, 2]].map(([a, b]) => corr(col("shift", a), col("shift", b)).toFixed(2)).join(" ") + "; a notch's depth, mean " + [0, 1, 2].map(j => (col("gain", j).reduce((a, b) => a + b) / col("gain", j).length).toFixed(2)).join(" ")
    + "; left out (depth 0) in " + [0, 1, 2].map(j => Math.round(100 * col("gain", j).filter(g => g === 0).length / col("gain", j).length) + " %").join(" "));
  const rangeOf = t => t.map(s => { const y = lab.shN(ORDER, 1, 0, 0); return ((10 ** (at(s.centre, y) / 21.4) - 1) / 0.00437 / 1000).toFixed(1) + " kHz, " + at(s.depth, y).toFixed(1) + " dB"; }).join(" · ");
  console.log("The template's notches in front: " + rangeOf(one) + "\nThe two templates' in front: " + pairOf.map(rangeOf).join("  |  "));
  fs.mkdirSync(out, { recursive: true });
  fs.writeFileSync(path.join(out, "ladder.json"), JSON.stringify({ order: ORDER, template: one, two: pairOf, dummy,
    heads: index.map((h, k) => ({ set: h.set, name: h.name, ear: h.ear, learned: isLearnt(h), none: single[k].none, dummy: single[k].dummy, fit: single[k].fits[0], two: both[k].fits.map(f => f.costs[3]) })) }));
  console.log(`\nwrote ${path.join(out, "ladder.json")}, ${minutes()} min`);
})();
