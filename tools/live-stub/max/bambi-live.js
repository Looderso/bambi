// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The bambi Live device's Live side, in Max for Live's v8: what the host is told about the set, and the
//  routing the host asks for. It decides nothing. Messages to and from the host are JSON lines carried by
//  the Node script; `send` goes out, `fromhost` comes in.
//
//  On while the host is connected: the tree goes to the host, observers report every change, and the host's `route`
//  is applied -- each track's routing remembered first. The host decides which tracks go to bambi, none until the
//  user takes one there, so connecting moves nothing; the bambi track is made with the first. Every remembered
//  routing is put back when the host takes every track out, when it goes away -- or, the set opened with tracks on
//  bambi, never comes -- and when the device is removed. A pair p is the loop device's
//  channels 2p+3/2p+4: 1/2 carry Live's Main, where Live sends its Master, and the bambi track's input -- the
//  host's copy back to Live -- comes in on them too.
//
//  Everything that must outlive the session -- each track's key with its place and name, the remembered routings,
//  the host's state -- lives in the device's unnamed dict, a parameter, whose contents Live saves with the set and
//  gives back when the device loads. (Live 12 offers a device no set_data; an embedded dict is saved with the
//  device's patcher, not with the set.) A track is known to the host by its key, never by Live's id, which lasts only while the set
//  is open; after a reopening it is found again by its place and name.
//
//  When the host goes away while bambi is on, or does not come to a set that has tracks on bambi, the device waits
//  five seconds for it (the Node script starts it again); then it puts every track back, so they play in Live.
//
//  It changes nothing on its own besides that. When the set opens it saves every track's routing -- for a track
//  it routed, the routing it remembers -- which changes nothing; restoring that is a press, confirmed by a second.
//  Tracks found on the loop device's channels that it did not route are shown, never moved.

autowatch = 1;
inlets = 1;
outlets = 4;  // 0: "send", "status", "detail", "meta", "saved", "notice" with their text; 1: the list's rows;
              //    2: the restore button's label; 3: the connection light

//  The connection light, a square as Live draws its own: green connected (the suite's green), yellow waiting --
//  for a host that is starting, or one that went away -- red when it failed, grey when another device has it.
const LIGHT = {connected: [0.494, 0.643, 0.494, 1], waiting: [0.98, 0.69, 0.23, 1],
               failed: [0.86, 0.27, 0.23, 1], busy: [0.45, 0.45, 0.45, 1]};
let protocolNote = "";  // the host speaks another protocol: what the device says until it is replaced
let failed = false;  // the host did not come back, or has no loop device: red until it connects again

function light(state) {
    outlet(3, "bgfillcolor", LIGHT[state]);
}

const PROTOCOL = 3;  // with the host's (core/live/protocol.hpp): bumped when a message or its meaning changes
const MAIN = ["Main", "Master"];  // Live 12 says Main, earlier versions Master
const GRACE_MS = 5000;
const RETURN_IN = "1/2";     // the host's copy back to Live: the loop device's first pair, as Main's goes out
const RETURN_NAME = "bambi";
const MONITOR_IN = 0;
const RETURN_START_DB = -12;  // the bambi track starts low: the decoded mix of every track bambi plays can reach full scale

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
let info = {};         // what the host last said about the mix: latency, rate, a warning per track key
let confirming = false;  // restore pressed once, waiting for the second press
const unconfirm = new Task(() => {
    confirming = false;
    outlet(2, "text", "restore saved");
});
const grace = new Task(() => {
    if (!on || linked) return;
    stop();
    failed = true;
    light("failed");
    status("off", "the host did not come back; every routing is back as it was");
});

//  The device is being removed: every observer and timer stopped first, so nothing reports into a device that
//  is going.
function notifydeleted() {
    for (const o of lightObservers) o.property = "";
    lightObservers = [];
    stopObserving();
    refresh.cancel();
    relight.cancel();
    grace.cancel();
    unconfirm.cancel();
    if (on) {  // removed while it routes: everything back to Live, as it was
        restoreAll();
        silence([]);
    }
}

function dictionary(name) {
    store = new Dict(name);
}

function bang() {
    ready = true;
    observeLightly();
    post("bambi: loaded; the set kept " + (store === null ? "nothing (no dict)" : store.getkeys()) + "\n");
    identify();
    on = remembered().length > 0;
    if (on) grace.schedule(GRACE_MS);  // a set saved with tracks on bambi: back to Live unless the host comes
    takeSnapshot(true);
    idle();
    showList();
}

//  The status when nothing is happening.
function idle() {
    if (protocolNote !== "") {  // not a state to leave by itself: the host is deaf to this device until it is replaced
        light("failed");
        status("old device", protocolNote);
        return;
    }
    light(busy ? "busy" : failed ? "failed" : linked ? "connected" : "waiting");
    if (busy) status("busy", "another bambi device in this set has the host");
    else if (!linked) status("no host", "starting it… if it does not come, is bambi installed?");
    else if (on) status("on · " + routedCount() + " tracks",
                        routedCount() === 0 ? "add a Path Encoder to a track in the host to play it in the field"
                                            : "in the field; binaural back on the bambi track");
    else status("connected", "everything plays in Live");
    const found = strays(identify());
    notice(found.length > 0 && !on
        ? found.length + " track" + (found.length === 1 ? "" : "s") + " still play to bambi's channels, not routed by " +
          "this device. Restore saved puts them back."
        : "");
}

// ---- what the set keeps -------------------------------------------------------------------------------------

//  A value kept long is kept in pieces, each a symbol of its own, joined when read.
function kept(name, fallback) {
    if (store === null || !store.contains(name)) return fallback;
    const v = store.get(name);
    const s = Array.isArray(v) ? v.join("") : String(v);
    return s.length > 0 ? JSON.parse(s) : fallback;
}

function keep(name, value) {
    if (store === null) return;
    if (value === null) {
        if (store.contains(name)) store.remove(name);
        return;
    }
    const s = JSON.stringify(value);
    if (s.length <= PIECE) store.set(name, s);
    else store.set(name, pieces(s));
}

function remembered() {
    return kept("routings", []);
}

function remember(list) {
    keep("routings", list.length > 0 ? list : null);
}

// ---- the host ------------------------------------------------------------------------------------------------

//  A line too long for one Max message -- a symbol holds at most 32767 bytes -- goes as pieces: each "+" and at most
//  PIECE characters, never splitting a character in two, then "." to join them. A JSON line never starts with either.
const PIECE = 8000;
function pieces(line) {
    const out = [];
    for (let at = 0; at < line.length;) {
        let end = Math.min(line.length, at + PIECE);
        const c = line.charCodeAt(end - 1);
        if (end < line.length && c >= 0xd800 && c <= 0xdbff) --end;  // the first half of a pair: keep it with its second
        out.push(line.slice(at, end));
        at = end;
    }
    return out;
}

function send(message) {
    if (!linked) return;
    const line = JSON.stringify(message);
    if (line.length <= PIECE) {
        outlet(0, "send", line);
        return;
    }
    for (const piece of pieces(line)) outlet(0, "send", "+" + piece);
    outlet(0, "send", ".");
}

let fromHost = "";  // the pieces of a long line from the host, until its "."

function status(what, why) {
    outlet(0, "status", what);
    outlet(0, "detail", why || "");
}

function notice(text) {
    outlet(0, "notice", text);
}

//  A row of the list pressed: its warning in full, or nothing.
function warning(text) {
    notice(String(text || ""));
}

function connected(flag) {
    linked = Number(flag) === 1;
    busy = false;
    if (linked) {
        grace.cancel();
        failed = false;
        const app = new LiveAPI("live_app");
        send({type: "hello", protocol: PROTOCOL,
              live: [app.call("get_major_version"), app.call("get_minor_version")].join("."),
              rate: 0});  // the set does not say its rate; the host knows it from the device it opens
        const state = kept("host", null);
        if (state !== null) send({type: "restore", state: state});
        idle();
        start();  // nothing moves until the host asks for it
    } else {
        protocolNote = "";
        stopObserving();
        info = {};
        outlet(0, "meta", "");
        if (on) {
            grace.schedule(GRACE_MS);
            light("waiting");
            status("host gone", "waiting for it five seconds, then everything goes back to Live");
        } else {
            idle();
        }
    }
}

//  The Node script heard the host answer another device: wait quietly, nothing here is lost.
function refused() {
    linked = false;
    busy = true;
    idle();
}

function fromhost(sent) {
    let line = String(sent);
    if (line[0] === "+") {
        fromHost += line.slice(1);
        return;
    }
    if (line === ".") {
        line = fromHost;
        fromHost = "";
    }
    let m;
    try {
        m = JSON.parse(line);
    } catch (e) {
        post("bambi: not JSON from the host: " + line + "\n");
        return;
    }
    if (m.type === "welcome") {
        if (m.protocol !== PROTOCOL) {
            //  a set keeps its device's own scripts, so this is an old device in a new host's session, or the other way about
            protocolNote = "the host speaks protocol " + m.protocol + ", this device " + PROTOCOL +
                           ": remove this device and add bambi again";
            idle();
        } else if (!m.loop) {
            failed = true;
            light("failed");
            status("no loop device", "the host found no bambi loop device; is it installed?");
        }
    } else if (m.type === "route") {
        route(m);
    } else if (m.type === "save") {
        keep("host", m.state);
    } else if (m.type === "info") {
        info = m;
        outlet(0, "meta", "latency " + Number(m.latency).toFixed(1) + " ms · " + (Number(m.rate) / 1000) + " kHz");
        showList();
    } else if (m.type === "status") {
        status(m.text);
    } else if (m.type === "command") {
        command(m);
    } else if (m.type === "refresh") {
        //  playing or not, but only as often as the last read's cost allows
        if (on && mayRead()) sendArrangement();
    }
}

//  Live's transport, as the host asks for it: Live stays the clock, and its observers report back what it did.
function command(m) {
    const set = new LiveAPI("live_set");
    if (m.action === "play") set.call("start_playing");
    else if (m.action === "stop") set.call("stop_playing");
    else if (m.action === "continue") set.call("continue_playing");
    else if (m.action === "locate") set.set("current_song_time", Math.max(0, Number(m.ppq) || 0));
}

// ---- the button ------------------------------------------------------------------------------------------------

function start() {
    on = true;
    lastTree = "";
    lastArrangement = "";
    lastValue = {};
    send({type: "bambi", on: 1});
    sendTree();
    observe();
    sendArrangement();
}

function stop() {
    on = false;
    grace.cancel();
    stopObserving();
    identify();
    post("bambi: off, restoring " + remembered().length + " tracks\n");
    const back = restoreAll();
    silence([]);
    removeReturn();
    send({type: "bambi", on: 0});
    info = {};
    outlet(0, "meta", "");
    showList();
    status("off", back + " track" + (back === 1 ? "" : "s") + " back as they were");
}

function show() {
    if (!linked) return idle();
    send({type: "show"});
    notice("the host was asked to come to the front");
}

// ---- which track is which -------------------------------------------------------------------------------------

// ---- the bambi track: the host's binaural mix back in Live -----------------------------------------------------

//  The track that plays the host's mix: it listens to the loop device's return pair. Known by that rather than by
//  its name, so a set reopened with bambi on finds the one it has.
function isReturn(path) {
    const t = new LiveAPI(path);
    try {
        const type = read(t, "input_routing_type"), channel = read(t, "input_routing_channel");
        return type !== null && type !== undefined && type.display_name === "Ext. In" && channel !== null &&
               channel !== undefined && channel.display_name === RETURN_IN;
    } catch (e) {
        return false;  // a group has no input
    }
}

function returnIndices() {
    const set = new LiveAPI("live_set");
    const out = [];
    for (let i = 0; i < set.getcount("tracks"); ++i)
        if (isReturn("live_set tracks " + i)) out.push(i);
    return out;
}

//  Makes the bambi track unless the set has it: last, listening to the return pair, monitoring In, playing to
//  Main. False when it cannot hear that pair.
function makeReturn() {
    if (returnIndices().length > 0) return true;
    const set = new LiveAPI("live_set");
    const view = new LiveAPI("live_set view");
    const selected = view.get("selected_track");  // a new track takes the selection; it goes back after
    set.call("create_audio_track", -1);
    const path = "live_set tracks " + (set.getcount("tracks") - 1);
    const t = new LiveAPI(path);
    t.set("name", RETURN_NAME);
    const listening = chooseNamed(t, "input_routing_type", "Ext. In") &&
                      chooseNamed(t, "input_routing_channel", RETURN_IN);
    t.set("current_monitoring_state", MONITOR_IN);
    chooseNamed(t, "output_routing_type", MAIN);
    setDb(path + " mixer_device volume", RETURN_START_DB);
    if (Number(selected[1]) > 0) view.set("selected_track", "id", Number(selected[1]));
    stale();
    return listening;
}

//  A fader to the value Live shows as `target` dB: found by halving, as Live says what a value is in dB but not the
//  other way round.
function setDb(path, target) {
    let lo = 0, hi = 1;
    for (let i = 0; i < 24; ++i) {
        const mid = (lo + hi) / 2, d = db(path, mid);
        if (d === null || d < target) lo = mid;
        else hi = mid;
    }
    new LiveAPI(path).set("value", (lo + hi) / 2);
}

function removeReturn() {
    const set = new LiveAPI("live_set");
    for (const i of returnIndices().reverse()) set.call("delete_track", i);
    stale();
}

function nodes() {
    const set = new LiveAPI("live_set");
    const out = [];
    for (let i = 0; i < set.getcount("tracks"); ++i)
        if (!isReturn("live_set tracks " + i)) out.push({path: "live_set tracks " + i, index: i, ret: false});
    for (let i = 0; i < set.getcount("return_tracks"); ++i)
        out.push({path: "live_set return_tracks " + i, index: i, ret: true});
    return out;
}

//  A node's features, for finding it again after a reopening: what a user changes one of at a time.
function fingerprint(n) {
    const t = n.api;
    const devices = [];
    for (let i = 0; i < t.getcount("devices"); ++i) devices.push(text(new LiveAPI(n.path + " devices " + i), "name"));
    let group = "";
    if (!n.ret && Number(t.get("is_grouped")) === 1) group = text(new LiveAPI("id " + t.get("group_track")[1]), "name");
    return {ret: n.ret, index: n.index, name: n.name, colour: hex(t.get("color")),
            kind: n.ret ? "return" : Number(t.get("is_foldable")) === 1 ? "group" : "track", group: group,
            devices: devices};
}

const MATCH = {name: 4, index: 2, colour: 1, group: 1, devices: 3, someDevices: 1, enough: 4};

//  How alike a saved node and a current one are; -1 when they cannot be the same node at all.
function likeness(a, b) {
    if (a.ret !== b.ret || (a.kind !== undefined && a.kind !== b.kind)) return -1;
    let score = 0;
    if (a.name === b.name) score += MATCH.name;
    if (a.index === b.index) score += MATCH.index;
    if (a.colour !== undefined && a.colour === b.colour) score += MATCH.colour;
    if (a.group !== undefined && a.group === b.group) score += MATCH.group;
    const da = a.devices || [], db_ = b.devices || [];
    if (da.length > 0 && da.join("\u0000") === db_.join("\u0000")) score += MATCH.devices;
    else if (da.length > 0 && db_.length > 0 && da.filter(d => db_.indexOf(d) >= 0).length * 2 >= Math.max(da.length, db_.length))
        score += MATCH.someDevices;
    return score;
}

//  Which saved node each current one is: the most alike pair first, as long as it is alike enough and no other
//  free pair is as alike for either side -- a tie is left unmatched rather than guessed. Returns, per current node,
//  the saved node's key or 0. Pure: no Live, so it can be tested outside Max.
function match(saved, current) {
    const pairs = [];
    saved.forEach((a, i) => current.forEach((b, j) => {
        const score = likeness(a, b);
        if (score >= MATCH.enough) pairs.push({i: i, j: j, score: score});
    }));
    pairs.sort((x, y) => y.score - x.score);
    const savedTaken = {}, currentTaken = {}, out = current.map(() => 0);
    for (const p of pairs) {
        if (savedTaken[p.i] || currentTaken[p.j]) continue;
        const rival = pairs.some(q => q !== p && q.score === p.score && !savedTaken[q.i] && !currentTaken[q.j] &&
                                      (q.i === p.i || q.j === p.j));
        if (rival) continue;
        savedTaken[p.i] = currentTaken[p.j] = true;
        out[p.j] = saved[p.i].key;
    }
    return out;
}

//  Every track and return with its key: by Live's id where this session saw it before; after a reopening by its
//  likeness to what the set kept -- name, place, colour, group, devices -- so a renamed or moved track is still
//  itself; a key of its own when nothing kept is alike enough. The set keeps every key with those features, kept
//  current by the light observers, so the next opening can find it again.
//  The picture of the set, built once and handed out until something changes: what the light observers see
//  marks it stale. Each node's fingerprint is kept by Live's id and taken again only when that node changed, so a
//  track moved costs a reordering, not a question to every device in the set.
let picture = null;
let prints = {};   // Live id -> fingerprint without its place

function stale(live) {
    picture = null;
    if (live !== undefined) delete prints[live];
}

function identify() {
    if (picture !== null) return picture;
    const known = kept("tracks", {next: 1, list: []});
    const now = nodes().map(n => {
        const t = new LiveAPI(n.path);
        return Object.assign(n, {api: t, live: Number(t.id), name: text(t, "name"), key: 0});
    });
    for (const n of now) {
        if (prints[n.live] === undefined || prints[n.live].name !== n.name) prints[n.live] = fingerprint(n);
        n.print = Object.assign({}, prints[n.live], {index: n.index});
    }
    const taken = {};
    for (const n of now)
        if (keyOfLive[n.live] !== undefined) {
            n.key = keyOfLive[n.live];
            taken[n.key] = true;
        }
    const open = now.filter(n => !n.key);
    const found = match(known.list.filter(e => !taken[e.key]), open.map(n => n.print));
    open.forEach((n, j) => {
        if (found[j]) n.key = found[j];
    });
    for (const n of now)
        if (!n.key) n.key = known.next++;

    keyOfLive = {};
    liveOfKey = {};
    for (const n of now) {
        keyOfLive[n.live] = n.key;
        liveOfKey[n.key] = n.live;
    }
    const tracks = {next: known.next, list: now.map(n => Object.assign({key: n.key}, n.print))};
    if (JSON.stringify(tracks) !== JSON.stringify(known)) keep("tracks", tracks);  // a write marks the set changed
    picture = now;
    return now;
}

// ---- the light observers ------------------------------------------------------------------------------------

//  Always on, whether bambi is or not: what identifies a track -- the track list, names, colours, devices -- so
//  the set's memory of it is never older than the set, and the list shows the set as it is. They tell the host
//  nothing. Their reports are gathered into one refresh: an observer reports once as it is set up.
let lightObservers = [];
let reshaped = false;  // the track list changed since the last refresh: the route observers follow it
const refresh = new Task(() => {
    const started = Date.now();
    identify();
    showList();
    if (on) {
        sendTree();
        if (reshaped) observe();
    }
    reshaped = false;
    post("bambi: refreshed in " + (Date.now() - started) + " ms\n");
});

function softly() {
    refresh.cancel();
    refresh.schedule(80);
}

function watchLightly(path, prop, then) {
    const o = new LiveAPI(args => {
        if (args[0] === prop) then();
    }, path);
    o.property = prop;
    lightObservers.push(o);
}

//  The set's tracks and returns, in order, by id: what a rebuild of the observers follows.
let shape = "";
function shapeNow() {
    const s = new LiveAPI("live_set");
    return s.get("tracks").join(" ") + "|" + s.get("return_tracks").join(" ");
}

function observeLightly() {
    for (const o of lightObservers) o.property = "";
    lightObservers = [];
    shape = shapeNow();
    //  The list observers report once as they are set up, the list unchanged: only a list that did change
    //  rebuilds, or every rebuild would schedule the next.
    const rebuild = () => {
        const now = shapeNow();
        if (now === shape) return;
        shape = now;
        stale();
        reshaped = true;
        softly();
        relight.cancel();
        relight.schedule(80);  // a track added, removed or moved: its own observers too
    };
    watchLightly("live_set", "tracks", rebuild);
    watchLightly("live_set", "return_tracks", rebuild);
    for (const n of nodes()) {
        const live = Number(new LiveAPI(n.path).id);
        const changed = () => {
            stale(live);
            softly();
        };
        watchLightly(n.path, "name", changed);
        watchLightly(n.path, "color", changed);
        watchLightly(n.path, "devices", changed);
        watchLightly(n.path, "is_grouped", changed);
    }
}
const relight = new Task(() => observeLightly());

// ---- the arrangement, as guidance --------------------------------------------------------------------------

//  Every track's arrangement clips, the locators and the loop, read in one go and timed: what reading them holds Live
//  up is sent along, measured rather than guessed. A group and a return have no clips of their own.
const ARRANGEMENT_SETTLE_MS = 300;  // edits come in bursts: read once they stop
const ARRANGEMENT_SHARE = 50;       // a read waits this many times its own last cost: Live's thread stays free
let lastArrangement = "";
let arrangementStale = false;
let lastReadAt = 0, lastReadMs = 0;
const arrangementLater = new Task(() => sendArrangement());

//  Whether reading now keeps Live's own thread free enough: the last read's cost, fifty times over, has passed.
function mayRead() {
    return Date.now() - lastReadAt >= Math.max(ARRANGEMENT_SETTLE_MS, ARRANGEMENT_SHARE * lastReadMs);
}

function arrangementChanged() {
    arrangementStale = true;
    arrangementLater.cancel();
    arrangementLater.schedule(Math.max(ARRANGEMENT_SETTLE_MS, ARRANGEMENT_SHARE * lastReadMs));
}

function sendArrangement() {
    arrangementStale = false;
    const began = Date.now();
    lastReadAt = began;
    const set = new LiveAPI("live_set");
    const tracks = [], session = [];
    for (const n of identify()) {
        if (n.path.startsWith("live_set return_tracks") || Number(n.api.get("is_foldable")) === 1) continue;
        const clips = [];
        for (let i = 0; i < n.api.getcount("arrangement_clips"); ++i) {
            const c = new LiveAPI(n.path + " arrangement_clips " + i);
            clips.push({start: Number(c.get("start_time")), end: Number(c.get("end_time")), name: text(c, "name"),
                        colour: hex(c.get("color")), midi: Number(c.get("is_midi_clip")) === 1});
        }
        clips.sort((a, b) => a.start - b.start);
        tracks.push({id: n.key, clips: clips});
        //  its session clips, slot by slot
        const slots = [];
        for (let s = 0; s < n.api.getcount("clip_slots"); ++s) {
            if (Number(new LiveAPI(n.path + " clip_slots " + s).get("has_clip")) !== 1) continue;
            const c = new LiveAPI(n.path + " clip_slots " + s + " clip");
            slots.push({slot: s, name: text(c, "name"), colour: hex(c.get("color")),
                        loopStart: Number(c.get("loop_start")), loopEnd: Number(c.get("loop_end")),
                        looping: Number(c.get("looping")) === 1, midi: Number(c.get("is_midi_clip")) === 1});
        }
        session.push({id: n.key, clips: slots});
    }
    const locators = [];
    for (let i = 0; i < set.getcount("cue_points"); ++i) {
        const c = new LiveAPI("live_set cue_points " + i);
        locators.push({time: Number(c.get("time")), name: text(c, "name")});
    }
    locators.sort((a, b) => a.time - b.time);
    const message = {type: "arrangement", tracks: tracks, session: session, scenes: set.getcount("scenes"),
                     locators: locators,
                     loop: {on: Number(set.get("loop")) === 1, start: Number(set.get("loop_start")),
                            length: Number(set.get("loop_length"))},
                     readMs: Date.now() - began};
    lastReadMs = message.readMs;
    const same = JSON.stringify([tracks, session, message.scenes, locators, message.loop]);
    if (same === lastArrangement) return;
    lastArrangement = same;
    post("bambi: the arrangement read in " + message.readMs + " ms\n");
    send(message);
}

// ---- session clips, as Live plays them ------------------------------------------------------------------------

//  Each track's session clip: which slot plays, and where in the clip Live is -- every report Live gives, sent on,
//  so the host can learn how often a clip's position arrives before it projects one between them.
let clipWatchers = [];
//  Each watched track's way back to the arrangement, for Back to Arrangement.
let sessionTracks = [];

//  Whether the session has a track: from a clip launched on it, or its stop button pressed, until Back to
//  Arrangement -- Live keeps the arrangement silent on it till then, whether a clip still plays or not.
function watchSessionClip(track, key) {
    let slot = -1, clip = null, taken = false;
    const sendSlot = position => {
        if (slot < 0 || clip === null) {
            send({type: "slot", id: key, slot: -1, overrides: taken});
            return;
        }
        send({type: "slot", id: key, slot: slot, position: position, loopStart: Number(clip.get("loop_start")),
              loopEnd: Number(clip.get("loop_end")), looping: Number(clip.get("looping")) === 1, overrides: taken});
    };
    sessionTracks.push(() => {
        if (!taken) return;
        taken = false;
        sendSlot(clip !== null ? Number(clip.get("playing_position")) : 0);
    });
    const position = new LiveAPI(args => {
        if (args[0] === "playing_position" && slot >= 0) sendSlot(Number(args[1]));
    }, track);
    const playing = new LiveAPI(args => {
        if (args[0] !== "playing_slot_index") return;
        slot = Number(args[1]);
        if (slot >= 0 || slot === -2) taken = true;  // a clip launched, or the track's stop button
        clip = null;
        position.property = "";
        if (slot >= 0 && Number(new LiveAPI(track + " clip_slots " + slot).get("has_clip")) === 1) {
            clip = new LiveAPI(track + " clip_slots " + slot + " clip");
            position.path = track + " clip_slots " + slot + " clip";
            position.property = "playing_position";
        } else {
            slot = -1;
        }
        sendSlot(clip !== null ? Number(clip.get("playing_position")) : 0);
        arrangementChanged();  // a clip launched or stopped: the session's clips read again
    }, track);
    playing.property = "playing_slot_index";
    clipWatchers.push(position, playing);
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

//  Where a node's audio goes, as the user set it: for a track this device routed, the routing it remembers; for
//  one on the loop device's channels that it did not route, the routing the snapshot saved, else its group or Main
//  -- bambi's channel is never where the user meant it to go.
//  The routing Live gives a track by default -- into its group if it is in one, else to Main -- kept as this marker
//  rather than by name, so a track grouped or ungrouped while bambi plays it belongs to where it now is, and goes
//  back there. A routing the user chose -- out of a group straight to Main, or anywhere else -- is kept by name.
const DEFAULT_ROUTING = "@default";

function normalised(type, n) {
    const group = groupOf(n);
    if (group !== null && (type === text(group, "name") || type === "Group")) return DEFAULT_ROUTING;
    if (group === null && MAIN.indexOf(type) >= 0) return DEFAULT_ROUTING;
    return type;
}

function resolved(type, n) {
    if (type !== DEFAULT_ROUTING) return type;
    const group = groupOf(n);
    return group !== null ? text(group, "name") : MAIN[0];
}

function outputOf(n) {
    const k = remembered().find(e => e.key === n.key);
    if (k) return resolved(k.type, n);
    const now = read(n.api, "output_routing_type").display_name;
    if (n.ret || !isStereoOut(n.api)) return now;
    const s = kept("snapshot", null);
    const saved = s === null ? null : s.list.find(e => e.key === n.key);
    if (saved) return saved.type;
    const group = groupOf(n);
    return group !== null ? text(group, "name") : MAIN[0];
}

//  A tree of where the audio goes, and of how the tracks are filed. A track's `parent` is the group it plays into, by its
//  routing as the user has it, not as bambi made it, so a track grouped while bambi plays it plays into the group; played
//  to Main it is a path of its own; played anywhere else it is not on a path to Main at all, and the host leaves it alone.
//  Its `group` is the group Live files it under, whatever its routing: where the host lists it, as Live does.
function tree() {
    const list = identify().map(n => {
        const t = n.api;
        const output = outputOf(n);
        let parent = null, path = "main", filed = null;
        if (Number(t.get("is_grouped")) === 1) {
            const group = new LiveAPI("id " + t.get("group_track")[1]);
            filed = keyOfLive[Number(group.id)] || null;  // where Live files it, whatever its routing
            if (output === text(group, "name") || output === "Group") parent = filed;
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
            group: filed,
            path: path,
            output: output,  // what the parent and the path were read from; the host logs it, and reads nothing else of it
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
    watch(song, "is_playing", v => {
        transport();
        if (!Number(v[0]) && arrangementStale) arrangementChanged();
    });
    watch(song, "tempo", transport);
    //  the arrangement, sent again when it changes -- never while Live plays, where reading it could stall
    for (const prop of ["cue_points", "loop", "loop_start", "loop_length", "scenes"]) watch(song, prop, arrangementChanged);
    //  Back to Arrangement: every track the session had goes back to the arrangement
    watch(song, "back_to_arranger", v => {
        if (!Number(v[0])) for (const back of sessionTracks) back();
    });
    //  names, colours and the track list come with the tree, which the light observers' refresh sends

    for (const n of all) {
        const t = n.api, key = n.key, name = n.name;
        const value = (what, v) => sendValue(key, name, what, v);
        if (!n.path.startsWith("live_set return_tracks") && Number(t.get("is_foldable")) !== 1) {
            watch(n.path, "arrangement_clips", arrangementChanged);
            watchSessionClip(n.path, key);
        }
        watch(n.path, "mute", v => value("mute", Number(v[0])));  // also the track's on/off button
        watch(n.path, "solo", v => value("solo", Number(v[0])));
        watch(n.path, "output_routing_type", () => {
            if (routing) return;  // the device's own change: half of a change is not a change to report
            const now = routingOf(t);
            if (set_[key] !== now) value("routing", now);
            sendTree();  // where its audio goes may have moved with it: the host's tree follows, sent only if it differs
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
    for (const o of clipWatchers) o.property = "";
    clipWatchers = [];
    sessionTracks = [];
}

// ---- routing ------------------------------------------------------------------------------------------------

function routingOf(t) {
    return read(t, "output_routing_type").display_name + " " + read(t, "output_routing_channel").display_name;
}

//  `names`: one display name, or several of which the first offered is taken.
function chooseNamed(api, prop, names) {
    const wanted = Array.isArray(names) ? names : [names];
    const entry = read(api, "available_" + prop + "s").find(e => wanted.indexOf(e.display_name) >= 0);
    if (entry === undefined) return false;
    api.set(prop, {identifier: entry.identifier});
    if (wanted.indexOf(read(api, prop).display_name) < 0)
        api.set(prop, JSON.stringify({identifier: entry.identifier}));
    return wanted.indexOf(read(api, prop).display_name) >= 0;
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
        if (!list.some(e => e.key === key)) list.push(Object.assign({key: key, name: name}, originalOf(key, t)));
        const to = (2 * pair + 3) + "/" + (2 * pair + 4);
        const ok = chooseNamed(t, "output_routing_type", "Ext. Out") && chooseNamed(t, "output_routing_channel", to);
        set_[key] = routingOf(t);
        results.push(ok ? {id: key, name: name, ok: true, to: "Ext. Out " + to}
                        : {id: key, name: name, ok: false, error: "no Ext. Out " + to + " (enabled as a stereo pair?)"});
    }
    remember(list);
    silence(m.silent || []);
    //  the bambi track, with the first track on bambi
    if (wanted.size > 0 && !makeReturn())
        notice("the bambi track cannot hear Ext. In " + RETURN_IN + ": enable it as a stereo pair in Live's " +
               "Input Config, with the bambi bridge as the input device");
    routing = false;
    send({type: "routed", tracks: results});
    idle();
    showList();
}

//  The routing to come back to: the track's own, unless it is on the loop device's channels without this device
//  having put it there -- then what the snapshot saved, else its group or Main.
function originalOf(key, t) {
    const n = {key: key, api: t, ret: false};
    if (!isStereoOut(t)) {
        const type = read(t, "output_routing_type").display_name;
        return {type: normalised(type, n), channel: read(t, "output_routing_channel").display_name};
    }
    const s = kept("snapshot", null);
    const saved = s === null ? null : s.list.find(e => e.key === key);
    if (saved) return {type: normalised(saved.type, n), channel: saved.channel};
    return {type: DEFAULT_ROUTING, channel: ""};
}

//  Where a track goes back to when bambi gives it to Live: what it was remembered playing to, except Main for a track
//  filed in a group -- a routing remembered from before it was grouped, or one bambi's own channels hid when the set was
//  saved -- which goes back into its group. Pure: no Live, so it can be tested outside Max.
function givenBackTo(type, grouped) {
    return grouped && MAIN.indexOf(type) >= 0 ? DEFAULT_ROUTING : type;
}

function restore(entry) {
    post("bambi: " + entry.name + " back to " + entry.type + " " + entry.channel + "\n");  // @default: its group or Main
    const t = trackOf(entry.key);
    if (t === null) return false;  // gone
    const type = givenBackTo(entry.type, groupOf({api: t}) !== null);
    if (!chooseNamed(t, "output_routing_type", resolved(type, {api: t}))) return false;  // refused
    chooseNamed(t, "output_routing_channel", entry.channel);  // Main has no channel to choose
    set_[entry.key] = routingOf(t);
    return true;
}

//  Live's returns the field plays are muted in Live, or they would be heard twice; each one's own mute is kept with
//  the set, so it is given back as it was -- one the user had muted stays muted.
function silence(keys) {
    const was = kept("silenced", []);
    const now = [];
    for (const entry of was) {
        if (keys.indexOf(entry.key) >= 0) {
            now.push(entry);
            continue;
        }
        const t = trackOf(entry.key);
        if (t !== null) t.set("mute", entry.mute);
    }
    for (const key of keys) {
        if (now.some(e => e.key === key)) continue;
        const t = trackOf(key);
        if (t === null) continue;
        now.push({key: key, mute: Number(t.get("mute"))});
        t.set("mute", 1);
    }
    keep("silenced", now.length > 0 ? now : null);
}

function restoreAll() {
    let back = 0;
    for (const entry of remembered())
        if (restore(entry)) ++back;
    remember([]);
    return back;
}

// ---- what the device shows --------------------------------------------------------------------------------

function routedCount() {
    return remembered().length;
}

function isStereoOut(t) {
    return read(t, "output_routing_type").display_name === "Ext. Out" &&
           /^\d+\/\d+$/.test(read(t, "output_routing_channel").display_name) &&
           parseInt(read(t, "output_routing_channel").display_name, 10) >= 3;
}

//  Tracks on the loop device's channels that this device does not remember routing: what it would not move.
function strays(all) {
    const kept_ = remembered();
    return all.filter(n => !n.ret && !kept_.some(e => e.key === n.key) && isStereoOut(n.api));
}

function groupOf(n) {
    if (Number(n.api.get("is_grouped")) !== 1) return null;
    return new LiveAPI("id " + n.api.get("group_track")[1]);
}

//  Every track and return, with what bambi does with it, to the list.
function showList() {
    const all = identify();
    const kept_ = remembered();
    const stray = strays(all).map(n => n.key);  // the same picture: identify() hands out what it built
    const rows = all.map(n => {
        const group = n.ret ? null : groupOf(n);
        const output = outputOf(n);
        const intoGroup = group !== null && (output === text(group, "name") || output === "Group");
        let to = n.ret ? "return" : intoGroup ? "into " + text(group, "name") : "in Live", kind = "";
        if (on && kept_.some(e => e.key === n.key)) {
            to = read(n.api, "output_routing_channel").display_name;
            kind = "pair";
        } else if (stray.indexOf(n.key) >= 0) {
            to = "on " + read(n.api, "output_routing_channel").display_name + " · not by this device";
            kind = "stray";
        }
        return {name: n.name, colour: hex(n.api.get("color")), child: intoGroup, to: to, kind: kind,
                warn: info.warnings && info.warnings[n.key] ? info.warnings[n.key] : ""};
    });
    const count = on ? kept_.length : stray.length;
    const head = count === 0 ? "no tracks on bambi"
               : count + " track" + (count === 1 ? "" : "s") + (on ? " on bambi" : " on bambi's channels");
    outlet(1, "rows", JSON.stringify({head: head, sub: all.filter(n => !n.ret).length + " in the set", rows: rows}));
}

// ---- the routing snapshot ----------------------------------------------------------------------------------

function clock() {
    const d = new Date();
    return d.getHours() + ":" + ("0" + d.getMinutes()).slice(-2);
}

//  Every track's routing as the user set it: for a track this device routed, the routing it remembers; a track
//  on the loop device's channels that it did not route is left out, its own routing being unknown.
function takeSnapshot(auto) {
    const all = identify();
    const kept_ = remembered();
    const stray = strays(all).map(n => n.key);
    const list = [];
    for (const n of all) {
        const r = kept_.find(e => e.key === n.key);
        if (r) list.push({key: n.key, name: n.name, type: r.type, channel: r.channel});
        else if (stray.indexOf(n.key) < 0)
            list.push({key: n.key, name: n.name, type: read(n.api, "output_routing_type").display_name,
                       channel: read(n.api, "output_routing_channel").display_name});
    }
    keep("snapshot", {at: clock(), auto: auto, list: list});
    showSnapshot();
}

function showSnapshot() {
    const s = kept("snapshot", null);
    outlet(0, "saved", s === null ? "nothing saved yet" : "saved " + s.at + (s.auto ? ", when the set opened" : ""));
}

function save() {
    if (!ready) return;
    takeSnapshot(false);
    notice(on ? "saved each track's routing from before bambi, not bambi's channels" : "saved every track's routing");
}

//  Every track back as saved; a track on the loop device's channels with nothing saved goes to its group, or Main.
//  Two presses: the first asks, the second does it.
function restoreSaved() {
    if (!ready) return;
    if (on && routedCount() > 0) {
        notice("take every track out of bambi in the host first: all to Live");
        return;
    }
    if (!confirming) {
        confirming = true;
        outlet(2, "text", "restore? press again");
        unconfirm.schedule(4000);
        return;
    }
    unconfirm.cancel();
    confirming = false;
    outlet(2, "text", "restore saved");
    const s = kept("snapshot", null);
    const all = identify();
    const stray = strays(all).map(n => n.key);
    let moved = 0;
    routing = true;
    for (const n of all) {
        const saved = s === null ? null : s.list.find(e => e.key === n.key);
        if (saved) {
            if (routingOf(n.api) === saved.type + " " + saved.channel) continue;
            if (chooseNamed(n.api, "output_routing_type", saved.type)) {
                chooseNamed(n.api, "output_routing_channel", saved.channel);
                ++moved;
            }
        } else if (stray.indexOf(n.key) >= 0) {
            const group = groupOf(n);
            if (chooseNamed(n.api, "output_routing_type", group !== null ? [text(group, "name"), "Group"] : MAIN))
                ++moved;
        }
        set_[n.key] = routingOf(n.api);
    }
    routing = false;
    notice(moved + " track" + (moved === 1 ? "" : "s") + " back as saved" + (s === null ? "" : " at " + s.at));
    idle();
    showList();
}
