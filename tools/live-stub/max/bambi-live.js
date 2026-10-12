// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The bambi Live device's Live side, in Max for Live's v8: what the host is told about the set, and the
//  routing the host asks for. It decides nothing. Messages to and from the host are JSON lines carried by
//  the Node script; `send` goes out, `fromhost` comes in.
//
//  On: the tree goes to the host, observers report every change, and the host's `route` is applied -- each
//  track's routing remembered first. Off: every remembered routing is put back. A pair p is the loop device's
//  channels 2p+1/2p+2, which on the "bambi bridge" aggregate are Live's outputs 2p+3/2p+4, the speakers being 1/2.
//
//  Everything that must outlive the session -- each track's key with its place and name, the remembered routings,
//  the host's state -- lives in the device's unnamed dict, a parameter, whose contents Live saves with the set and
//  gives back when the device loads. (Live 12 offers a device no set_data; an embedded dict is saved with the
//  device's patcher, not with the set.) A track is known to the host by its key, never by Live's id, which lasts only while the set
//  is open; after a reopening it is found again by its place and name.
//
//  When the host goes away while bambi is on, the device waits five seconds for it (the Node script starts it
//  again); then it turns bambi off, so the tracks play in Live again.

autowatch = 1;
inlets = 1;
outlets = 2;  // 0: "send <json>" and "status <text>", 1: the button, set without output

const PROTOCOL = 1;
const MAIN = ["Main", "Master"];  // Live 12 says Main, earlier versions Master
const GRACE_MS = 5000;

let store = null;      // the dict the set keeps
let ready = false;     // loaded; a press before that is a parameter being restored
let linked = false;    // the host is on the other end
let busy = false;      // the host answers another device
let on = false;
let observers = [];
let lastTree = "";     // the tree as last sent; one is sent only when it differs
let lastValue = {};    // "<key> <what>" -> the value as last sent, likewise
let set_ = {};         // key -> the routing this device set; a routing seen that is not it is the user's
let routing = false;   // the device is changing a routing: half of a change is not a change to report
let keyOfLive = {};    // Live id -> key, learned in this session only
let liveOfKey = {};    // key -> Live id, likewise
const grace = new Task(() => {
    if (!on || linked) return;
    stop();
    status("the host did not come back: bambi off, everything back in Live");
});

function dictionary(name) {
    store = new Dict(name);
}

function bang() {
    ready = true;
    post("bambi: loaded; the set kept " + (store === null ? "nothing (no dict)" : store.getkeys()) + "\n");
    identify();
    on = remembered().length > 0;
    outlet(1, "set", on ? 1 : 0);
    status(linked ? "connected" : "waiting for the host");
}

// ---- what the set keeps -------------------------------------------------------------------------------------

function kept(name, fallback) {
    if (store === null || !store.contains(name)) return fallback;
    const s = String(store.get(name));
    return s.length > 0 ? JSON.parse(s) : fallback;
}

function keep(name, value) {
    if (store === null) return;
    if (value === null) store.remove(name);
    else store.set(name, JSON.stringify(value));
}

function remembered() {
    return kept("routings", []);
}

function remember(list) {
    keep("routings", list.length > 0 ? list : null);
}

// ---- the host ------------------------------------------------------------------------------------------------

function send(message) {
    if (linked) outlet(0, "send", JSON.stringify(message));
}

function status(text) {
    outlet(0, "status", text);
}

function connected(flag) {
    linked = Number(flag) === 1;
    busy = false;
    if (linked) {
        grace.cancel();
        const app = new LiveAPI("live_app");
        send({type: "hello", protocol: PROTOCOL,
              live: [app.call("get_major_version"), app.call("get_minor_version")].join("."),
              rate: 0});  // the set does not say its rate; the host knows it from the device it opens
        const state = kept("host", null);
        if (state !== null) send({type: "restore", state: state});
        status("connected");
        if (on) start();
    } else {
        stopObserving();
        if (on) {
            grace.schedule(GRACE_MS);
            status("the host is gone: waiting for it");
        } else {
            status("waiting for the host");
        }
    }
}

//  The Node script heard the host answer another device: wait quietly, nothing here is lost.
function refused() {
    linked = false;
    busy = true;
    status("the host is busy with another bambi device");
}

function fromhost(line) {
    let m;
    try {
        m = JSON.parse(String(line));
    } catch (e) {
        post("bambi: not JSON from the host: " + line + "\n");
        return;
    }
    if (m.type === "welcome") {
        if (m.protocol !== PROTOCOL) status("the host speaks protocol " + m.protocol + ", this device " + PROTOCOL);
        else if (!m.loop) status("the host has no loop device");
    } else if (m.type === "route") {
        route(m);
    } else if (m.type === "save") {
        keep("host", m.state);
    } else if (m.type === "status") {
        status(m.text);
    }
}

// ---- the button ------------------------------------------------------------------------------------------------

function enable() {
    if (!ready) return;
    if (on) {
        stop();
    } else if (!linked) {
        outlet(1, "set", 0);
        status(busy ? "the host is busy with another bambi device" : "no host to turn on with");
    } else {
        start();
    }
}

function start() {
    on = true;
    lastTree = "";
    lastValue = {};
    outlet(1, "set", 1);
    send({type: "bambi", on: 1});
    sendTree();
    observe();
}

function stop() {
    on = false;
    grace.cancel();
    stopObserving();
    identify();
    post("bambi: off, restoring " + remembered().length + " tracks\n");
    const back = restoreAll();
    outlet(1, "set", 0);
    send({type: "bambi", on: 0});
    status(back + " back as they were");
}

// ---- which track is which -------------------------------------------------------------------------------------

function nodes() {
    const set = new LiveAPI("live_set");
    const out = [];
    for (let i = 0; i < set.getcount("tracks"); ++i)
        out.push({path: "live_set tracks " + i, index: i, ret: false});
    for (let i = 0; i < set.getcount("return_tracks"); ++i)
        out.push({path: "live_set return_tracks " + i, index: i, ret: true});
    return out;
}

//  Every track and return with its key: by Live's id where this session saw it before; after a reopening by its
//  place and name, then by its name alone; a key of its own when none matches. The set keeps every key with the
//  place and name it was last seen at, so the next opening can find it again.
function identify() {
    const known = kept("tracks", {next: 1, list: []});
    const now = nodes().map(n => {
        const t = new LiveAPI(n.path);
        return Object.assign(n, {api: t, live: Number(t.id), name: text(t, "name"), key: 0});
    });
    const taken = {};
    const claim = (n, key) => {
        n.key = key;
        taken[key] = true;
    };
    for (const n of now)
        if (keyOfLive[n.live] !== undefined) claim(n, keyOfLive[n.live]);
    for (const n of now) {
        if (n.key) continue;
        const e = known.list.find(e => !taken[e.key] && e.ret === n.ret && e.index === n.index && e.name === n.name);
        if (e) claim(n, e.key);
    }
    for (const n of now) {
        if (n.key) continue;
        const e = known.list.find(e => !taken[e.key] && e.ret === n.ret && e.name === n.name);
        if (e) claim(n, e.key);
    }
    for (const n of now)
        if (!n.key) claim(n, known.next++);

    keyOfLive = {};
    liveOfKey = {};
    for (const n of now) {
        keyOfLive[n.live] = n.key;
        liveOfKey[n.key] = n.live;
    }
    const tracks = {next: known.next, list: now.map(n => ({key: n.key, ret: n.ret, index: n.index, name: n.name}))};
    if (JSON.stringify(tracks) !== JSON.stringify(known)) keep("tracks", tracks);  // a write marks the set changed
    return now;
}

// ---- the set as the host sees it ----------------------------------------------------------------------------

function sendTree() {
    const t = tree();
    const text = JSON.stringify(t);
    if (text === lastTree) return;
    lastTree = text;
    send(t);
}

function sendValue(key, name, what, value) {
    const k = key + " " + what, text = JSON.stringify(value);
    if (lastValue[k] === text) return;
    lastValue[k] = text;
    send({type: "value", id: key, name: name, what: what, value: value});
}

//  A routing property comes back as JSON text, {"<property>": value}, sometimes wrapped in an array.
function read(api, prop) {
    let v = api.get(prop);
    if (Array.isArray(v) && v.length === 1) v = v[0];
    if (typeof v !== "string") return v;
    const o = JSON.parse(v);
    return o[prop] !== undefined ? o[prop] : o;
}

function text(api, prop) {
    return String(api.get(prop));
}

function hex(colour) {
    return "#" + ("000000" + Number(colour).toString(16)).slice(-6);
}

//  A fader's or a send's value as Live shows it, in dB; silence is null, JSON having no infinity.
function db(path, value) {
    const shown = String(new LiveAPI(path).call("str_for_value", value));
    const n = parseFloat(shown);
    return /inf/i.test(shown) || isNaN(n) ? null : n;
}

//  Where a node's audio goes, as the user set it: for a track this device routed, the routing it remembers.
function outputOf(n) {
    const k = remembered().find(e => e.key === n.key);
    return k ? k.type : read(n.api, "output_routing_type").display_name;
}

//  A tree of where the audio goes, not of how the tracks are filed: a track in a group belongs to it only
//  while it plays into it. Played to Main it is a path of its own; played anywhere else it is not on a path
//  to Main at all, and the host leaves it alone.
function tree() {
    const list = identify().map(n => {
        const t = n.api;
        const output = outputOf(n);
        let parent = null, path = "main";
        if (Number(t.get("is_grouped")) === 1) {
            const group = new LiveAPI("id " + t.get("group_track")[1]);
            if (output === text(group, "name") || output === "Group") parent = keyOfLive[Number(group.id)];
        }
        if (parent === null && MAIN.indexOf(output) < 0 && !n.ret) path = "elsewhere";
        const mixer = new LiveAPI(n.path + " mixer_device");
        const sends = [];
        for (let j = 0; j < mixer.getcount("sends"); ++j) {
            const send = n.path + " mixer_device sends " + j;
            sends.push(db(send, Number(new LiveAPI(send).get("value"))));
        }
        const volume = n.path + " mixer_device volume";
        return {
            id: n.key,
            kind: n.ret ? "return" : Number(t.get("is_foldable")) === 1 ? "group" : "track",
            parent: parent,
            path: path,
            name: n.name,
            colour: hex(t.get("color")),
            audio: n.ret ? 1 : Number(t.get("has_audio_output")),
            sends: sends,
            fader: db(volume, Number(new LiveAPI(volume).get("value"))),
            mute: Number(t.get("mute")),
            solo: Number(t.get("solo")),
            pan: Number(new LiveAPI(n.path + " mixer_device panning").get("value")),
        };
    });
    return {type: "tree", nodes: list};
}

// ---- observing --------------------------------------------------------------------------------------------

function watch(path, prop, report) {
    const o = new LiveAPI(args => {
        if (args[0] === prop) report(args.slice(1));
    }, path);
    o.property = prop;
    observers.push(o);
}

function observe() {
    stopObserving();
    const song = "live_set";
    const transport = () => {
        const s = new LiveAPI(song);
        send({type: "transport", playing: Number(s.get("is_playing")), ppq: Number(s.get("current_song_time")),
              bpm: Number(s.get("tempo"))});
    };
    const all = identify();
    for (const n of all) set_[n.key] = routingOf(n.api);  // what the routing is now is the device's own
    watch(song, "current_song_time", transport);
    watch(song, "is_playing", transport);
    watch(song, "tempo", transport);
    watch(song, "tracks", sendTree);
    watch(song, "return_tracks", sendTree);

    for (const n of all) {
        const t = n.api, key = n.key, name = n.name;
        const value = (what, v) => sendValue(key, name, what, v);
        watch(n.path, "name", v => value("name", String(v[0])));
        watch(n.path, "color", v => value("colour", hex(v[0])));
        watch(n.path, "mute", v => value("mute", Number(v[0])));  // also the track's on/off button
        watch(n.path, "solo", v => value("solo", Number(v[0])));
        watch(n.path, "output_routing_type", () => {
            const now = routingOf(t);
            if (!routing && set_[key] !== now) value("routing", now);
        });
        const volume = n.path + " mixer_device volume";
        watch(volume, "value", v => value("fader", db(volume, Number(v[0]))));
        watch(n.path + " mixer_device panning", "value", v => value("pan", Number(v[0])));
        const mixer = new LiveAPI(n.path + " mixer_device");
        for (let j = 0; j < mixer.getcount("sends"); ++j) {
            const send = n.path + " mixer_device sends " + j;
            watch(send, "value", v => value("send:" + j, db(send, Number(v[0]))));
        }
    }
}

function stopObserving() {
    for (const o of observers) o.property = "";
    observers = [];
}

// ---- routing ------------------------------------------------------------------------------------------------

function routingOf(t) {
    return read(t, "output_routing_type").display_name + " " + read(t, "output_routing_channel").display_name;
}

function chooseNamed(api, prop, name) {
    const entry = read(api, "available_" + prop + "s").find(e => e.display_name === name);
    if (entry === undefined) return false;
    api.set(prop, {identifier: entry.identifier});
    if (read(api, prop).display_name !== name) api.set(prop, JSON.stringify({identifier: entry.identifier}));
    return read(api, prop).display_name === name;
}

function trackOf(key) {
    if (liveOfKey[key] === undefined) identify();
    return liveOfKey[key] === undefined ? null : new LiveAPI("id " + liveOfKey[key]);
}

function route(m) {
    if (!on) return;
    routing = true;
    const list = remembered();
    const wanted = new Map(m.tracks.map(t => [t.id, t.pair]));
    const results = [];

    //  a track the host no longer wants goes back to what it was
    for (const entry of list.slice()) {
        if (wanted.has(entry.key)) continue;
        restore(entry);
        list.splice(list.indexOf(entry), 1);
    }
    for (const [key, pair] of wanted) {
        const t = trackOf(key);
        if (t === null) {
            results.push({id: key, ok: false, error: "no such track"});
            continue;
        }
        const name = text(t, "name");
        if (!list.some(e => e.key === key))
            list.push({key: key, name: name, type: read(t, "output_routing_type").display_name,
                       channel: read(t, "output_routing_channel").display_name});
        const to = (2 * pair + 3) + "/" + (2 * pair + 4);
        const ok = chooseNamed(t, "output_routing_type", "Ext. Out") && chooseNamed(t, "output_routing_channel", to);
        set_[key] = routingOf(t);
        results.push(ok ? {id: key, name: name, ok: true, to: "Ext. Out " + to}
                        : {id: key, name: name, ok: false, error: "no Ext. Out " + to + " (enabled as a stereo pair?)"});
    }
    remember(list);
    routing = false;
    send({type: "routed", tracks: results});
    status(results.filter(r => r.ok).length + " on bambi");
}

function restore(entry) {
    post("bambi: " + entry.name + " back to " + entry.type + " " + entry.channel + "\n");
    const t = trackOf(entry.key);
    if (t === null || !chooseNamed(t, "output_routing_type", entry.type)) return false;  // gone, or refused
    chooseNamed(t, "output_routing_channel", entry.channel);  // Main has no channel to choose
    set_[entry.key] = routingOf(t);
    return true;
}

function restoreAll() {
    let back = 0;
    for (const entry of remembered())
        if (restore(entry)) ++back;
    remember([]);
    return back;
}
