// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The bambi routing device's logic, in Max for Live's v8. It sits on the Main track. On: every track that
//  plays to Main goes to its own channel of the loop device, its routing remembered, and a track called
//  bambi is made to play the binaural return. Off: every remembered track gets its routing back and the
//  bambi track is deleted. Everything goes through the Live API; no audio passes through Max.
//
//  Channels as Live numbers them on the "bambi bridge" aggregate: outputs 1/2 are the speakers, so the
//  loop device's channel n is Live's output n + 2; its inputs keep their numbers. The remembered routing
//  lives in the device's embedded dict, so it is saved with the set and survives reopening it.

autowatch = 1;
inlets = 1;
outlets = 2;  // 0: the status line, 1: the button, set without output

const FIRST_OUT = 3;  // Live's output for the loop device's channel 1
const SOURCES = 12;   // the loop device's channels 1..12 carry sources
const RETURN_IN = "15/16";
const RETURN_NAME = "bambi";
const MAIN = ["Main", "Master"];  // Live 12 says Main, earlier versions Master
const MONITOR_IN = 0;

let store = null;    // the embedded dict
let ready = false;   // the device has loaded; a press before that is a parameter being restored, not a press

function dictionary(name) {
    store = new Dict(name);
}

function bang() {
    ready = true;
    const state = remembered();
    outlet(1, "set", state ? 1 : 0);
    if (state) report(state.tracks.length + " on bambi");
    else report(candidates().length + " track" + (candidates().length === 1 ? "" : "s") + " can go to bambi");
}

//  Every press flips: the remembered routing, not the button's value, says whether bambi is on.
function enable() {
    if (!ready) return;
    if (remembered()) stop();
    else start();
}

// ---- the Live API's dictionaries ---------------------------------------------------------------------------

//  A routing property comes back as JSON text, {"<property>": value}, sometimes wrapped in an array.
function read(api, prop) {
    let v = api.get(prop);
    if (Array.isArray(v) && v.length === 1) v = v[0];
    if (typeof v !== "string") return v;
    const o = JSON.parse(v);
    return o[prop] !== undefined ? o[prop] : o;
}

function chooseNamed(api, prop, names) {
    const wanted = Array.isArray(names) ? names : [names];
    const entry = read(api, "available_" + prop + "s").find(e => wanted.indexOf(e.display_name) >= 0);
    if (entry === undefined) return false;
    api.set(prop, {identifier: entry.identifier});
    if (wanted.indexOf(read(api, prop).display_name) < 0) api.set(prop, JSON.stringify({identifier: entry.identifier}));
    return wanted.indexOf(read(api, prop).display_name) >= 0;
}

function text(api, prop) {
    return String(api.get(prop));
}

// ---- tracks ------------------------------------------------------------------------------------------------

function tracks() {
    const set = new LiveAPI("live_set");
    const out = [];
    for (let i = 0; i < set.getcount("tracks"); ++i) out.push(new LiveAPI("live_set tracks " + i));
    return out;
}

function playsToMain(t) {
    return MAIN.indexOf(read(t, "output_routing_type").display_name) >= 0;
}

//  Tracks that play to Main: not one called probe or bambi, and only ones with audio to give.
function candidates() {
    return tracks().filter(t => {
        const name = text(t, "name").toLowerCase();
        if (name === "probe" || name === RETURN_NAME) return false;
        return Number(t.get("has_audio_output")) === 1 && playsToMain(t);
    });
}

//  A remembered track found again: at its index when the name still matches there, else by name.
function find(entry) {
    const all = tracks();
    if (entry.index < all.length && text(all[entry.index], "name") === entry.name) return all[entry.index];
    return all.find(t => text(t, "name") === entry.name);
}

// ---- remembering -------------------------------------------------------------------------------------------

function remembered() {
    if (store === null || !store.contains("state")) return null;
    const s = String(store.get("state"));
    return s.length > 0 ? JSON.parse(s) : null;
}

function remember(state) {
    if (store !== null) store.set("state", state ? JSON.stringify(state) : "");
}

// ---- on and off --------------------------------------------------------------------------------------------

function start() {
    const all = tracks();
    const state = {tracks: []};
    let failed = 0;
    candidates().slice(0, SOURCES).forEach((t, i) => {
        const index = all.findIndex(a => a.id === t.id);
        const entry = {index: index, name: text(t, "name"),
                       type: read(t, "output_routing_type").display_name,
                       channel: read(t, "output_routing_channel").display_name};
        const channel = String(FIRST_OUT + i);
        if (chooseNamed(t, "output_routing_type", "Ext. Out") && chooseNamed(t, "output_routing_channel", channel)) {
            state.tracks.push(entry);
            post("bambi: " + entry.name + " -> Ext. Out " + channel + "\n");
        } else {
            //  half-routed is worse than not routed: put it back at once
            chooseNamed(t, "output_routing_type", entry.type);
            chooseNamed(t, "output_routing_channel", entry.channel);
            post("bambi: " + entry.name + " could not go to Ext. Out " + channel +
                 " (enabled as mono in Output Config?)\n");
            ++failed;
        }
    });

    const set = new LiveAPI("live_set");
    const view = new LiveAPI("live_set view");
    const selected = view.get("selected_track");  // a new track takes the selection; it goes back after
    set.call("create_audio_track", -1);
    const ret = new LiveAPI("live_set tracks " + (set.getcount("tracks") - 1));
    ret.set("name", RETURN_NAME);
    const listening = chooseNamed(ret, "input_routing_type", "Ext. In") &&
                      chooseNamed(ret, "input_routing_channel", RETURN_IN);
    ret.set("current_monitoring_state", MONITOR_IN);
    chooseNamed(ret, "output_routing_type", MAIN);
    view.set("selected_track", "id", Number(selected[1]));
    remember(state);
    outlet(1, "set", 1);

    let status = state.tracks.length + " on bambi";
    const left = candidates().length;  // what still plays to Main: past the twelfth, or failed
    if (left > failed) status += ", " + (left - failed) + " left on Main (12 at most)";
    if (failed > 0) status += ", " + failed + " failed (see the Max window)";
    if (!listening) status += "; the bambi track cannot hear Ext. In " + RETURN_IN + " (enable it in Input Config)";
    report(status);
}

function stop() {
    const state = remembered();
    let restored = 0, missing = 0;
    state.tracks.forEach(entry => {
        const t = find(entry);
        if (t === undefined) {
            ++missing;
            return;
        }
        const ok = chooseNamed(t, "output_routing_type", entry.type);
        chooseNamed(t, "output_routing_channel", entry.channel);
        ok ? ++restored : ++missing;
    });
    const set = new LiveAPI("live_set");
    const all = tracks();
    for (let i = all.length - 1; i >= 0; --i)
        if (text(all[i], "name") === RETURN_NAME) set.call("delete_track", i);
    remember(null);
    outlet(1, "set", 0);
    report(restored + " back as they were" + (missing > 0 ? ", " + missing + " not found (renamed or deleted?)" : ""));
}

function report(status) {
    outlet(0, "set", status);
    post("bambi: " + status + "\n");
}
