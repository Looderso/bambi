// The head model as one file, for the plugin: nobody's head and the template of 0168/0169 in one JSON — every part a
// table of spherical-harmonic coefficients (real, SN3D, ACN; x front, y left, z up) per point of the ERB axis, the
// template's maps, and every constant the synthesis needs, so the plugin holds no number of its own about the model.
//
//   smooth    the CLL's smooth pattern, dB: coef[p * points + k]; its order-0 row is the KU100's timbre at nobody's level
//   ild       the ILD (left less right, dB) and its change per unit of head size: ild + (size − 1) · withSize
//   itd       tL − tR, µs, times the head size
//   notches   the template: centre (ERB), depth (dB, negative), width (ERB), presence (0–1) as maps; the dip shape
//             1 / (1 + u²) in dB, u the distance from the centre in widths; depth × depthFactor × fade(presence)
//
//   node export-model.js <generic.json> <template.json> <out head.json>
const fs = require("fs"), path = require("path");
const [genericFile, templateFile, out] = process.argv.slice(2);
const G = JSON.parse(fs.readFileSync(genericFile, "utf8")), T = JSON.parse(fs.readFileSync(templateFile, "utf8"));
const lab = new Function(["../common.js", "lab.js"].map(f => fs.readFileSync(path.join(__dirname, f), "utf8")).join("\n") + "\nreturn { E0, STEP, NF, LO, HI, inBand };")();
const { E0, STEP, NF } = lab, round = v => +v.toFixed(4);
const level = lab.inBand(300, 16000).reduce((t, k) => t + G.average[k] + G.pattern[k], 0) / lab.inBand(300, 16000).length;   // nobody's level, which the KU100's timbre takes
const smooth = G.pattern.slice(); for (let k = 0; k < NF; k++) smooth[k] = G.timbreKU100[k] + level;
const model = {
  version: 1, learnedFrom: T.learnedFrom, convention: "spherical harmonics real, SN3D, ACN; x front, y left, z up; dB; the axis in ERB numbers: erb0 + k * step, f = (10^(e / 21.4) - 1) / 0.00437",
  axis: { erb0: round(E0), step: STEP, points: NF },
  smooth: { order: 4, coef: smooth.map(round) },
  ild: { order: G.ildOrder, coef: G.ild.map(round), withSize: G.ildWithSize.map(round) },
  itd: { order: G.itdOrder, coef: G.itd.map(round) },
  notches: { order: T.order, shape: T.shape, depthFactor: 2.5, fade: T.fade, band: [round(E0 + lab.LO * STEP), round(E0 + lab.HI * STEP)], widthRange: [0.1, 1.5],
             list: T.template.map(t => ({ centre: t.centre.map(round), depth: t.depth.map(round), width: t.width.map(round), presence: t.presence.map(round) })) },
};
fs.writeFileSync(out, JSON.stringify(model));
console.log(`wrote ${out}, ${(fs.statSync(out).size / 1e6).toFixed(2)} MB; learned from ${T.learnedFrom.join(" and ")}`);
