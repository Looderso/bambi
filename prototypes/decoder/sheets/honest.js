// The template learned honestly: the notches as deep and as narrow as the heads have them, present where the heads
// have them — not where a least-squares fit, hedging against a notch a little off, puts a shallow wide dip.
//
// Per head (lab.js): its dips found on the prepared CLL, tracked, and fitted to the measured CLL again with centre
// held and depth and width free — a dip's real depth and width. The template: NOTCHES surfaces at order ORDER,
// symmetric. Four turns: each head's one shift is the median of how far its dips sit from the template notches they
// lie nearest (within 0.75 ERB; a shallow dip counts for less), the shift first found by a scan; then, the shifts taken out, each notch's centre and
// width are fitted to the dips given to it, its depth to those dips alone — where a head has the notch, how deep it is
// — and its presence to every direction, 1 where a head has it and 0 where not. Learned from half of the people.
//
// Scored on the others, against the measured CLLs: the deepest notch a direction (below the 3-ERB blur) and its width
// at half depth, measured and from the template with the heads' shifts; of the heads' dips deeper than 3 dB, the share
// with a template notch within 0.5 ERB (found); of the template's notches, the share with such a dip (right); the
// costs as before, for the record. Writes <out>/honest.json: the template (centre, depth, width, presence), and every
// head's shift and depth factor.
//
//   node honest.js <out folder> <ladder folder, for the first centres> [processes, default 8] [shape: bell or peak, default peak]
//
// The shape: a dip as a bell (Gaussian) or as a peaking filter's notch (lab.js), for the dips found and for the template.
const fs = require("fs"), path = require("path"), { fork } = require("child_process");
const here = f => path.join(__dirname, f);
const lab = new Function(["../common.js", "lab.js"].map(f => fs.readFileSync(here(f), "utf8")).join("\n")
  + "\nreturn { parseHead, analyse, fitted, tracksOf, backToInput, targetOf, bumps, blur, costOf, shN, solve, setShape, NOTCHES, E0, STEP, LO, HI, NF };")();
const SHAPE = process.argv[2] === "child" ? process.argv[3] : (process.argv[5] || "peak"); lab.setShape(SHAPE);
const NOTCHES = 3, ORDER = 6, COEF = (ORDER + 1) ** 2, PRESENT = 0.4;
const at = (coef, y) => { let v = 0; for (let p = 0; p < coef.length; p++) v += y[p] * coef[p]; return v; }, mirror = ([x, y, z]) => [x, -y, z];
const median = v => { const s = [...v].sort((a, b) => a - b); return s.length ? s[s.length >> 1] : NaN; };

if (process.argv[2] === "child") {
  const preset = JSON.parse(fs.readFileSync(here("presets.json"), "utf8")).sizes.find(z => z.surfaces === 6 && z.order === 4).P, heads = [];
  const lo = lab.E0 + lab.LO * lab.STEP, hi = lab.E0 + lab.HI * lab.STEP;
  const onHead = (h, T) => { const ys = h.H.xyz.map(v => lab.shN(ORDER, ...v));
    return T.map(t => ({ c: ys.map(y => at(t.centre, y)), a: ys.map(y => Math.min(0, at(t.depth, y))), w: ys.map(y => Math.max(0.1, Math.min(1.5, at(t.width, y)))), p: ys.map(y => t.presence ? at(t.presence, y) : 1) })); };
  // Each direction's dips given to the template notches they lie nearest, the shift in: per notch the dip or null.
  const given = (h, maps, shift) => h.dips.map((dips, d) => { const pairs = [], got = maps.map(() => null), used = new Set();
    dips.forEach((p, i) => maps.forEach((m, k) => { const gap = Math.abs(p[0] - m.c[d] - shift); if (gap <= 0.75) pairs.push([gap, i, k]); }));
    for (const [, i, k] of pairs.sort((a, b) => a[0] - b[0])) if (!got[k] && !used.has(i)) { got[k] = dips[i]; used.add(i); }
    return got; });
  // A head's shift: of −2…2 ERB in quarter steps, the one under which its dips meet the template notches best — each
  // given dip counting by its depth and by how closely it meets — then the median of how far the given dips still sit.
  function fitShift(h, maps) {
    const score = shift => given(h, maps, shift).reduce((t, got, d) => t + got.reduce((u, p, k) => u + (p ? Math.min(1, -p[1] / 6) * (1 - Math.abs(p[0] - maps[k].c[d] - shift) / 0.75) : 0), 0), 0);
    let best = 0, most = -1; for (let s = -2; s <= 2.001; s += 0.25) { const v = score(s); if (v > most) { most = v; best = s; } }
    const offs = []; given(h, maps, best).forEach((got, d) => got.forEach((p, k) => { if (!p) return; const n = Math.round(Math.min(3, -p[1] / 3)); for (let i = 0; i < n; i++) offs.push(p[0] - maps[k].c[d]); }));
    return offs.length ? median(offs) : best; }
  // The template's dips on a head, with its shift and depth factor, where the template has the notch.
  const dipsOf = (maps, shift, gain, sharp, d) => maps.map(m => m.p[d] < PRESENT || m.a[d] * gain > -0.5 ? null : [Math.max(lo, Math.min(hi, m.c[d] + shift)), Math.max(-60, m.a[d] * gain * sharp), Math.max(0.08, m.w[d] / sharp)]).filter(Boolean);
  const notchOf = x => { const b = lab.blur(x, 3); let at = lab.NOTCHES[0], deep = 0; for (const k of lab.NOTCHES) if (x[k] - b[k] < deep) { deep = x[k] - b[k]; at = k; }
    let l = at, r = at; while (l > 0 && x[l] - b[l] < deep / 2) l--; while (r < x.length - 1 && x[r] - b[r] < deep / 2) r++; return [deep, (r - l) * lab.STEP]; };
  process.on("message", msg => {
    if (msg.load) { for (const e of msg.load) { const f = fs.readFileSync(here("heads/" + e.head)), H = lab.parseHead(f.buffer.slice(f.byteOffset, f.byteOffset + f.byteLength), e);
        const A = lab.analyse(H, e.ear === "left" ? 1 : 2), keep = {}, fit = lab.fitted(H, A, preset, keep);
        const [final] = lab.backToInput(H, keep.near, keep.xs, lab.tracksOf(H, keep.near, fit.targets, fit.dips, preset), null, 4, preset.floor, [2, 0, 0]);   // depth and width from the spectrum
        heads.push({ e, H, xs: keep.xs, dips: final.map(p => p.filter(q => q && q[1] <= -1.5)), measured: keep.xs.map(notchOf) }); }
      return process.send({ loaded: heads.length }); }
    const T = msg.template;
    return process.send(heads.map(h => { const maps = onHead(h, T), shift = fitShift(h, maps), got = given(h, maps, shift);
      const ratios = []; got.forEach((g, d) => g.forEach((p, k) => { if (p && maps[k].a[d] < -1) ratios.push(p[1] / maps[k].a[d]); }));
      const gain = ratios.length ? median(ratios) : 1, out = { shift, gain };
      if (msg.sums) {                                                     // what this head gives the template: least-squares sums, the mirror image with it
        const s = T.map(() => ({ Mc: new Float64Array(COEF * COEF), bc: new Float64Array(COEF), bw: new Float64Array(COEF), Ma: new Float64Array(COEF * COEF), ba: new Float64Array(COEF), Mp: new Float64Array(COEF * COEF), bp: new Float64Array(COEF) }));
        h.H.xyz.forEach((v, d) => { for (const u of [v, mirror(v)]) { const y = lab.shN(ORDER, ...u);
          T.forEach((_, k) => { const q = s[k], p = got[d][k], weight = p ? Math.min(1, -p[1] / 6) : 0;
            for (let a = 0; a < COEF; a++) { q.bp[a] += y[a] * (p ? 1 : 0); if (p) { q.bc[a] += weight * y[a] * (p[0] - shift); q.bw[a] += weight * y[a] * p[2]; q.ba[a] += y[a] * p[1]; }
              for (let b = 0; b <= a; b++) { q.Mp[a * COEF + b] += y[a] * y[b]; if (p) { q.Mc[a * COEF + b] += weight * y[a] * y[b]; q.Ma[a * COEF + b] += y[a] * y[b]; } } } }); } });
        out.sums = s.map(q => Object.fromEntries(Object.entries(q).map(([k, v]) => [k, Array.from(v)]))); }
      if (msg.score) { const dirs = h.xs.map((_, d) => d), deep = [], blurred = [], plain = [], read = []; let own = 0, found = 0, claimed = 0, right = 0, sharpOk = 0;
        for (const d of dirs) { const dips = dipsOf(maps, shift, 1, 1, d), r = lab.targetOf(h.xs[d], dips), m = lab.bumps(dips), err = r.map((v, k) => v - m[k]), soft = lab.blur(err, 1);
          let e1 = 0, e2 = 0; for (const k of lab.NOTCHES) { e1 += err[k] ** 2; e2 += soft[k] ** 2; } plain.push(e1); blurred.push(e2); read.push(notchOf(m));
          for (const p of h.dips[d]) if (p[1] <= -3) { own++; if (dips.some(q => Math.abs(q[0] - p[0]) <= 0.5)) found++; }
          for (const q of dips) { claimed++; if (h.dips[d].some(p => Math.abs(q[0] - p[0]) <= 0.5)) right++; } }
        const n = dirs.length * lab.NOTCHES.length;
        out.score = { costPlain: Math.sqrt(plain.reduce((a, b) => a + b) / n), costBlurred: Math.sqrt(blurred.reduce((a, b) => a + b) / n), found: found / own, right: right / Math.max(1, claimed), claimedPerDirection: claimed / dirs.length,
          depth: median(read.map(q => q[0])), width: median(read.filter(q => q[0] < -3).map(q => q[1])), deep6: read.filter(q => q[0] < -6).length / read.length,
          measuredDepth: median(h.measured.map(q => q[0])), measuredWidth: median(h.measured.filter(q => q[0] < -3).map(q => q[1])), measuredDeep6: h.measured.filter(q => q[0] < -6).length / h.measured.length }; }
      return out; }));
  });
} else (async () => {
  const [out, ladderFolder, procs = "8"] = process.argv.slice(2), count = +procs, started = Date.now(), minutes = () => ((Date.now() - started) / 60000).toFixed(1);
  const index = JSON.parse(fs.readFileSync(here("run/index.json"), "utf8")).heads.filter(h => ["HUTUBS", "3D3A", "SONICOM"].includes(h.set));
  const people = [...new Set(index.map(h => h.set + " " + h.name))], learnt = new Set(people.filter((_, i) => i % 2 === 0)), isLearnt = h => learnt.has(h.set + " " + h.name);
  const children = Array.from({ length: count }, () => fork(__filename, ["child", SHAPE]));
  for (const c of children) c.on("exit", code => { if (code) { console.error("a worker stopped with an error; stopping"); children.forEach(o => o.kill()); process.exit(1); } });
  const mine = i => index.map((h, k) => k).filter(k => k % count === i);
  const ask = msgOf => Promise.all(children.map((c, i) => new Promise(done => { c.once("message", done); c.send(msgOf(i)); })));
  const spread = parts => { const all = new Array(index.length); parts.forEach((p, i) => mine(i).forEach((k, j) => { all[k] = p[j]; })); return all; };
  await ask(i => ({ load: mine(i).map(k => index[k]) }));
  console.log(`${index.length} heads loaded, their dips found with depth and width free, ${minutes()} min`);
  const solved = (M, b) => { const A = Float64Array.from(M); let trace = 0; for (let a = 0; a < COEF; a++) { trace += A[a * COEF + a]; for (let c = a + 1; c < COEF; c++) A[a * COEF + c] = A[c * COEF + a]; }
    for (let a = 0; a < COEF; a++) { const n = Math.floor(Math.sqrt(a)); A[a * COEF + a] += 1e-3 * Math.max(trace, 1e-9) / COEF * (0.01 + n * (n + 1)); }
    return Array.from(lab.solve(A, Float64Array.from(b), COEF)); };
  // The first template: the ladder's centres, a width of 0.3 ERB, a depth of 8 dB, present everywhere.
  const first = JSON.parse(fs.readFileSync(path.join(ladderFolder, "ladder.json"), "utf8")).template.slice(0, NOTCHES)
    .map(t => ({ centre: t.centre, width: [0.3, ...Array(COEF - 1).fill(0)], depth: [-8, ...Array(COEF - 1).fill(0)], presence: null }));
  let template = first, result;
  for (let turn = 0; turn < 5; turn++) {
    const last = turn === 4; result = spread(await ask(() => ({ template, sums: !last, score: last })));
    if (last) break;
    const total = template.map(() => null);
    result.forEach((r, k) => { if (!isLearnt(index[k])) return; r.sums.forEach((q, j) => { if (!total[j]) { total[j] = q; return; } for (const key of Object.keys(q)) for (let a = 0; a < q[key].length; a++) total[j][key][a] += q[key][a]; }); });
    template = total.map(q => ({ centre: solved(q.Mc, q.bc), width: solved(q.Mc, q.bw), depth: solved(q.Ma, q.ba), presence: solved(q.Mp, q.bp) }));
    console.log(`turn ${turn + 1}: the heads' shifts spread by ${Math.sqrt(result.filter((_, k) => isLearnt(index[k])).reduce((t, r) => t + r.shift ** 2, 0) / result.length).toFixed(2)} ERB, ${minutes()} min`);
  }
  children.forEach(c => c.kill());
  const others = result.filter((r, k) => !isLearnt(index[k])), med = key => median(others.map(r => r.score[key]));
  console.log(`\nOn the ${others.length} heads of the people not learned from:`);
  console.log(`  a direction's deepest notch: measured ${med("measuredDepth").toFixed(1)} dB deep, ${med("measuredWidth").toFixed(2)} ERB wide at half depth, deeper than 6 dB in ${Math.round(100 * med("measuredDeep6"))} % of the directions`);
  console.log(`  the template's, each head with its shift:  ${med("depth").toFixed(1)} dB, ${med("width").toFixed(2)} ERB, ${Math.round(100 * med("deep6"))} %`);
  console.log(`  of the heads' dips deeper than 3 dB, found by a template notch within 0.5 ERB: ${Math.round(100 * med("found"))} %; of the template's notches, with such a dip: ${Math.round(100 * med("right"))} % (it plays ${med("claimedPerDirection").toFixed(2)} notches a direction)`);
  console.log(`  cost, for the record: without the blur ${med("costPlain").toFixed(2)} dB, blurred ${med("costBlurred").toFixed(2)} dB`);
  const front = lab.shN(ORDER, 1, 0, 0);
  console.log("  the template in front: " + template.map(t => `${((10 ** (at(t.centre, front) / 21.4) - 1) / 0.00437 / 1000).toFixed(1)} kHz, ${at(t.depth, front).toFixed(1)} dB, ${at(t.width, front).toFixed(2)} ERB, present in ${Math.round(100 * at(t.presence, front))} %`).join(" · "));
  fs.mkdirSync(out, { recursive: true });
  fs.writeFileSync(path.join(out, `honest${SHAPE === "peak" ? "" : "-" + SHAPE}.json`), JSON.stringify({ order: ORDER, present: PRESENT, shape: SHAPE, template, heads: index.map((h, k) => ({ set: h.set, name: h.name, ear: h.ear, learned: isLearnt(h), shift: result[k].shift, gain: result[k].gain })) }));
  console.log(`\nwrote ${path.join(out, `honest${SHAPE === "peak" ? "" : "-" + SHAPE}.json`)}, ${minutes()} min`);
})();
