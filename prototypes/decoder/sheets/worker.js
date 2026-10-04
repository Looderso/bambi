// The chain of lab.js off the page's thread: a head's file and the settings in, what the page draws out. A head's
// analysis and the slow part of its fit are kept for its next runs; of several requests waiting, only the last runs.
importScripts("../common.js", "lab.js?10");                         // the number: a changed file is not read from the browser's cache
const heads = new Map();
let waiting = null, busy = false;
const pack = (set, places) => { const out = new Float32Array(set.length * places * 3).fill(NaN);
  set.forEach((p, d) => p.forEach((q, k) => { if (q) out.set(q, (d * places + k) * 3); })); return out; };
const rows = list => { const out = new Float32Array(list.length * NF); list.forEach((v, d) => out.set(v, d * NF)); return out; };
onmessage = async e => {
  waiting = e.data; if (busy) return; busy = true;
  while (waiting) {
    const { id, file, ear, P } = waiting, key = file + "|" + ear; waiting = null;
    if (!heads.has(key)) { if (heads.size > 3) heads.delete(heads.keys().next().value);
      const H = parseHead(await (await fetch("heads/" + file)).arrayBuffer(), { file });
      heads.set(key, { H, A: analyse(H, ear === "left" ? 1 : 2), keep: {} }); }
    const { H, A, keep } = heads.get(key), t = performance.now(), r = chain(H, A, P, keep);
    const out = { id, n: r.n, places: r.places, sheets: r.sheets, final: pack(r.final, r.places), rebuilt: pack(r.rebuilt, r.places), prep: rows(r.prep),
                  pattern: Float32Array.from(r.pattern), tracks: r.tracks, found: r.found, tracked: r.tracked, gathered: r.gathered, kept: r.kept, numbers: r.numbers,
                  cost: r.cost, costFree: r.costFree, costNone: r.costNone, measure: r.measure, ms: performance.now() - t };
    postMessage(out, [out.final.buffer, out.rebuilt.buffer, out.prep.buffer, out.pattern.buffer]);
    await new Promise(done => setTimeout(done));                       // lets a request that came meanwhile in
  }
  busy = false;
};
