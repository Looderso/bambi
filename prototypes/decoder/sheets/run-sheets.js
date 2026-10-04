// Every symmetric head of run/index.json through the chain (lab.js), with the settings the search found for a size:
// each surface's three patterns, and — so that surfaces of different heads can be laid over each other — each surface
// on one common set of directions: POINTS directions on the left half of the sphere (a Fibonacci spread) and their
// mirror images, in that order. Writes <out>/part-<part>.json (the heads: set, name, ear, the costs, the surfaces'
// coefficients) and <out>/part-<part>.f32 (heads x surfaces x 2·POINTS x [centre ERB, depth dB, width ERB], depth 0
// where a surface has no dip) — and <out>/parts-<part>.f32, a head's other parts: the CLL's average over the
// directions (NF points), the smooth pattern (order 4: 25 x NF), the ITD's pattern (order ITD: its coefficients, µs)
// and the ILD's (order ILD, not blurred, held at ±40 dB: coefficients x NF). Run as several processes at once, each
// taking every <of>-th head:
//
//   node run-sheets.js <out folder> [<part> <of>] [surfaces, default 6] [order, default 4]
const fs = require("fs"), path = require("path");
const here = f => path.join(__dirname, f);
const [out, part = "0", of = "1", surfaces = "6", order = "4"] = process.argv.slice(2), POINTS = 200;
const lab = new Function(["../common.js", "lab.js"].map(f => fs.readFileSync(here(f), "utf8")).join("\n") + "\nreturn { parseHead, analyse, chain, shN, fitSphere, NF };")();
const preset = JSON.parse(fs.readFileSync(here("presets.json"), "utf8")).sizes.find(z => z.surfaces === +surfaces && z.order === +order);
const P = { ...preset.P, surfaces: +surfaces, surfaceOrder: +order }, K = (+order + 1) ** 2;
const half = []; for (let i = 0; half.length < POINTS; i++) { const z = 1 - 2 * (i + 0.5) / (2.2 * POINTS), r = Math.sqrt(1 - z * z), a = Math.PI * (3 - Math.sqrt(5)) * i, v = [r * Math.cos(a), r * Math.sin(a), z]; if (v[1] > 0.02 && Math.abs(z) < 1) half.push(v); }
const grid = [...half, ...half.map(([x, y, z]) => [x, -y, z])], shapes = grid.map(v => lab.shN(+order, ...v));
const index = JSON.parse(fs.readFileSync(here("run/index.json"), "utf8")), heads = [], maps = [], parts = [], ITD = 8, ILD = 10, NF = lab.NF;
index.heads.forEach((entry, i) => {
  if (i % +of !== +part) return;
  const f = fs.readFileSync(here("heads/" + entry.head)), H = lab.parseHead(f.buffer.slice(f.byteOffset, f.byteOffset + f.byteLength), entry);
  const A = lab.analyse(H, entry.ear === "left" ? 1 : 2), r = lab.chain(H, A, P, {}), at = (coef, y) => { let v = 0; for (let p = 0; p < K; p++) v += y[p] * coef[p]; return v; };
  const map = new Float32Array(+surfaces * grid.length * 3);
  for (let s = 0; s < +surfaces; s++) { const sh = r.surfaces[s]; if (!sh) continue;
    shapes.forEach((y, g) => { const depth = at(sh.depth, y); if (depth > -0.5) return; map.set([at(sh.centre, y), depth, Math.max(0.1, Math.min(1.5, at(sh.width, y)))], (s * grid.length + g) * 3); }); }
  maps.push(map);
  const some = H.xyz.map((_, d) => d).filter(d => d % Math.ceil(H.n / 1000) === 0);
  parts.push(A.avg, Float32Array.from(r.pattern), Float32Array.from(lab.fitSphere(H, some, some.map(d => [A.itd[d]]), ITD, 1)),
             Float32Array.from(lab.fitSphere(H, some, some.map(d => A.ild.subarray(d * NF, (d + 1) * NF).map(v => Math.max(-40, Math.min(40, v)))), ILD)));
  heads.push({ set: entry.set, name: entry.name, ear: entry.ear, cost: r.cost, costFree: r.costFree, costNone: r.costNone,
               surfaces: r.surfaces.map(sh => sh && { centre: Array.from(sh.centre), depth: Array.from(sh.depth), width: Array.from(sh.width) }) });
});
fs.mkdirSync(out, { recursive: true });
fs.writeFileSync(path.join(out, `part-${part}.f32`), Buffer.concat(maps.map(m => Buffer.from(m.buffer))));
fs.writeFileSync(path.join(out, `parts-${part}.f32`), Buffer.concat(parts.map(m => Buffer.from(m.buffer, m.byteOffset, m.byteLength))));
fs.writeFileSync(path.join(out, `part-${part}.json`), JSON.stringify({ settings: P, points: POINTS, grid, nf: NF, itd: ITD, ild: ILD, heads }));
console.log(`part ${part} of ${of}: ${heads.length} heads`);
