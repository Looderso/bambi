// The chain's settings found by search instead of by eye. The cost of a setting: what the surfaces, rebuilt from their
// numbers, and the smooth part leave of the measured CLLs (lab.js's costOf), averaged over twelve heads — four of
// each database, both ears among them. For each size — a number of surfaces and an order, so 3 x surfaces x
// (order + 1)² numbers a head — random settings first, then, twice, each knob moved one step either way from the
// best and the moves that helped taken together. The winners are checked on twelve other heads and written to
// presets.json, with how much each knob moved the cost.
//
//   node search.js [processes, default 6]
const fs = require("fs"), path = require("path"), { fork } = require("child_process");
const here = f => path.join(__dirname, f);
const SIZES = [[3, 4], [4, 4], [6, 4], [8, 4], [3, 6], [4, 6], [6, 6], [8, 6]];
const GRID = { blur: [0, 0.5, 0.75, 1, 1.5], sphere: [0, 6, 8, 10, 12], counts: [2, 2.5, 3, 3.5, 4, 5], link: [0.3, 0.4, 0.5, 0.6, 0.8, 1, 1.2],
               reach: [8, 10, 12, 15, 18], least: [2, 3, 4, 6, 8, 12, 20], pull: [0, 0.1, 0.3, 0.6, 1], bridge: [0, 1, 1.5, 2],
               join: [0.2, 0.3, 0.4, 0.5, 0.6, 0.8, 1, 1.2], floor: [0.5, 1, 1.5, 2, 3] };
const BY_EYE = { blur: 0.75, sphere: 8, counts: 3, link: 0.6, reach: 12, least: 4, pull: 0.3, bridge: 1.5, join: 0.5, floor: 1.5 };

if (process.argv[2] === "child") {
  const lab = new Function(["../common.js", "lab.js"].map(f => fs.readFileSync(here(f), "utf8")).join("\n") + "\nreturn { parseHead, analyse, scores };")();
  const heads = new Map();
  process.on("message", ({ entries, jobs }) => {
    process.send(entries.map(e => { const key = e.head + e.ear;
      if (!heads.has(key)) { const f = fs.readFileSync(here("heads/" + e.head)), H = lab.parseHead(f.buffer.slice(f.byteOffset, f.byteOffset + f.byteLength), e);
        heads.set(key, { H, A: lab.analyse(H, e.ear === "left" ? 1 : 2), keep: {} }); }
      const h = heads.get(key);
      return jobs.map(j => lab.scores(h.H, h.A, j.P, h.keep, j.sizes)); }));
  });
} else (async () => {
  const count = +process.argv[2] || 6, index = JSON.parse(fs.readFileSync(here("run/index.json"), "utf8"));
  const pick = offset => ["HUTUBS", "3D3A", "SONICOM"].flatMap(set => { const all = index.heads.filter(h => h.set === set);
    return [0, 1, 2, 3].map(i => all[(Math.floor(all.length * (2 * i + offset) / 8) | 1) - (i % 2)]); });   // spread over the set, ears alternating
  const tune = pick(0), check = pick(1), children = Array.from({ length: count }, () => fork(__filename, ["child"]));
  // The mean cost over the heads of every job [{ P, sizes }]: an array per job, a cost per size.
  const ask = (entries, jobs) => Promise.all(children.map((c, i) => new Promise(done => { const mine = entries.filter((_, k) => k % count === i);
      c.once("message", r => done(r)); c.send({ entries: mine, jobs }); })))
    .then(parts => { const per = parts.flat(); return jobs.map((j, k) => j.sizes.map((_, s) => per.reduce((t, h) => t + h[k][s], 0) / per.length)); });
  let seed = 7; const random = () => { seed = seed + 0x6D2B79F5 | 0; let t = Math.imul(seed ^ seed >>> 15, 1 | seed); t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; };
  const name = P => Object.keys(GRID).map(k => P[k]).join(" "), started = Date.now(), minutes = () => ((Date.now() - started) / 60000).toFixed(1);

  const first = [BY_EYE, ...Array.from({ length: 63 }, () => Object.fromEntries(Object.entries(GRID).map(([k, v]) => [k, v[Math.floor(random() * v.length)]])))];
  const costs = await ask(tune, first.map(P => ({ P, sizes: SIZES })));
  console.log(`64 settings on ${tune.length} heads, ${minutes()} min`);
  const best = SIZES.map((_, s) => { const i = costs.reduce((b, c, k) => c[s] < costs[b][s] ? k : b, 0); return { P: first[i], cost: costs[i][s], byEye: costs[0][s], matters: {} }; });
  for (let pass = 0; pass < 2; pass++) {
    const jobs = []; SIZES.forEach((size, s) => { for (const [k, values] of Object.entries(GRID)) { const at = values.indexOf(best[s].P[k]);
      for (const to of [at - 1, at + 1]) if (values[to] !== undefined) jobs.push({ s, k, P: { ...best[s].P, [k]: values[to] }, sizes: [size] }); } });
    const moved = await ask(tune, jobs), together = SIZES.map((_, s) => ({ ...best[s].P }));
    SIZES.forEach((_, s) => { for (const k of Object.keys(GRID)) { const tries = jobs.map((j, i) => [j, moved[i][0]]).filter(([j]) => j.s === s && j.k === k);
      const all = [best[s].cost, ...tries.map(t => t[1])]; best[s].matters[k] = +(Math.max(...all) - Math.min(...all)).toFixed(3);
      const good = tries.reduce((b, t) => t[1] < b[1] ? t : b, [null, best[s].cost]); if (good[0]) together[s][k] = good[0].P[k]; } });
    const joint = await ask(tune, together.map((P, s) => ({ P, sizes: [SIZES[s]] })));
    SIZES.forEach((_, s) => { const single = jobs.map((j, i) => [j.P, moved[i][0]]).filter((_, i) => jobs[i].s === s);
      for (const [P, cost] of [...single, [together[s], joint[s][0]]]) if (cost < best[s].cost) { best[s].P = P; best[s].cost = cost; } });
    console.log(`pass ${pass + 1}: ${jobs.length + SIZES.length} settings, ${minutes()} min`);
  }
  const checked = await ask(check, best.map((b, s) => ({ P: b.P, sizes: [SIZES[s]] })));
  const none = (await ask(tune, [{ P: { ...BY_EYE, counts: 99 }, sizes: [SIZES[0]] }]))[0][0];             // no dip counts: the smooth part alone
  children.forEach(c => c.kill());
  console.log(`\nthe smooth part alone leaves ${none.toFixed(2)} dB. Per size: numbers a head, cost by eye → found (on the other twelve heads), and the settings (${Object.keys(GRID).join(" ")})`);
  const sizes = SIZES.map(([surfaces, order], s) => { const b = best[s], numbers = 3 * surfaces * (order + 1) ** 2;
    console.log(`  ${surfaces} surfaces, order ${order}: ${String(numbers).padStart(4)} numbers   ${b.byEye.toFixed(3)} → ${b.cost.toFixed(3)} (${checked[s][0].toFixed(3)})   ${name(b.P)}`);
    return { surfaces, order, numbers, cost: +b.cost.toFixed(3), checked: +checked[s][0].toFixed(3), byEye: +b.byEye.toFixed(3), P: b.P, matters: b.matters }; });
  console.log("\nhow far one step of a knob moved the cost, dB (mean over the sizes):");
  for (const k of Object.keys(GRID)) console.log(`  ${k.padEnd(8)} ${(best.reduce((t, b) => t + b.matters[k], 0) / best.length).toFixed(3)}`);
  fs.writeFileSync(here("presets.json"), JSON.stringify({ none: +none.toFixed(3), tunedOn: tune.map(h => `${h.set} ${h.name} ${h.ear}`), checkedOn: check.map(h => `${h.set} ${h.name} ${h.ear}`), sizes }, null, 1));
  console.log(`\nwrote presets.json, ${minutes()} min`);
})();
