// SPDX-License-Identifier: GPL-3.0-or-later
//
//  How the bambi Live device finds a track again after a reopening, tested outside Max: the device's script is
//  loaded with stand-ins for Max's globals and its pure match() is asked about the cases that matter.
//
//    node tools/live-stub/max/test-match.js

const fs = require("fs");
const path = require("path");
const vm = require("vm");

class Task {
    constructor(f) { this.f = f; }
    schedule() {}
    cancel() {}
}
const context = {Task, LiveAPI: function () {}, Dict: function () {}, post() {}, outlet() {}, autowatch: 0,
                 inlets: 0, outlets: 0};
vm.createContext(context);
vm.runInContext(fs.readFileSync(path.join(__dirname, "bambi-live.js"), "utf8"), context);
const match = context.match;

let failed = 0, passed = 0;
function check(name, got, want) {
    if (JSON.stringify(got) === JSON.stringify(want)) ++passed;
    else {
        ++failed;
        console.log("FAIL " + name + ": got " + JSON.stringify(got) + ", want " + JSON.stringify(want));
    }
}

const track = (key, index, name, more) =>
    Object.assign({key, ret: false, index, name, colour: "#aa0000", kind: "track", group: "", devices: []}, more);
const now = t => Object.assign({}, t, {key: undefined});

//  the set as it was saved
const vocal = track(1, 0, "Vox", {colour: "#5e9bb5", devices: ["EQ Eight", "Compressor", "De-Esser"]});
const bass = track(2, 1, "Bass", {colour: "#8f6fb0", devices: ["Saturator"]});
const pad = track(3, 2, "Pad", {colour: "#7fae7a"});
const saved = [vocal, bass, pad];

// Catches: reassigning keys from scratch, or by position alone.
check("unchanged", match(saved, saved.map(now)), [1, 2, 3]);

// Catches: matching on the name before anything else -- a rename would make a track new and lose its state.
check("renamed", match(saved, [now(Object.assign({}, vocal, {name: "Lead Vocal"})), now(bass), now(pad)]), [1, 2, 3]);

// Catches: requiring devices -- a renamed track without any must still be found by its place and colour.
check("renamed, no devices", match(saved, [now(vocal), now(bass), now(Object.assign({}, pad, {name: "Strings"}))]),
      [1, 2, 3]);

// Catches: matching on the place -- a moved track must follow its name, not stay with its old place.
check("moved", match(saved, [now(Object.assign({}, pad, {index: 0})), now(Object.assign({}, vocal, {index: 1})),
                             now(Object.assign({}, bass, {index: 2}))]), [3, 1, 2]);

// Catches: too high a bar -- renamed and moved, a track is still found by its colour, group and devices.
check("renamed and moved", match(saved, [now(Object.assign({}, bass, {index: 0})),
                                         now(Object.assign({}, vocal, {index: 1, name: "Lead Vocal"})),
                                         now(pad)]), [2, 1, 3]);

// Catches: too low a bar -- a track that shares nothing with any saved one is new, not given a stranger's key.
check("a stranger", match([vocal], [track(undefined, 5, "Choir", {colour: "#123456", devices: ["Reverb"]})]), [0]);

// Catches: guessing in a tie -- two saved tracks exactly as alike to one current track leave it unmatched.
const twinA = track(7, 4, "Pad", {colour: "#7fae7a"}), twinB = track(8, 5, "Pad", {colour: "#7fae7a"});
check("a tie", match([twinA, twinB], [track(undefined, 9, "Pad", {colour: "#7fae7a"})]), [0]);

// Catches: ignoring the track/return divide -- a return with a track's name and place is not that track.
check("a return is no track", match(saved, [Object.assign(now(vocal), {ret: true, kind: "return"})]), [0]);

// Catches: ignoring the track/return divide where the kind cannot tell -- an older memory knows no kinds.
check("a return is no track, to an older memory",
      match([{key: 4, ret: false, index: 0, name: "Vox"}], [Object.assign(now(vocal), {ret: true, kind: "return"})]),
      [0]);

// Catches: ignoring the kind -- a group in a track's place, with its name, is not that track.
check("a group is no track", match(saved, [Object.assign(now(vocal), {kind: "group"})]), [0]);

// Catches: refusing what an older device saved -- key, place and name only -- which must still match by those.
check("an older memory", match([{key: 4, ret: false, index: 0, name: "Vox"}], [now(vocal)]), [4]);

// Catches: two duplicates of one track swapping keys -- each keeps the key of its own place.
const kickA = track(5, 0, "Kick", {devices: ["Drum Buss"]}), kickB = track(6, 1, "Kick", {devices: ["Drum Buss"]});
check("duplicates", match([kickA, kickB], [now(kickA), now(kickB)]), [5, 6]);

console.log(failed === 0 ? "match: " + passed + " cases pass" : failed + " of " + (passed + failed) + " fail");
process.exitCode = failed === 0 ? 0 : 1;
