// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The bambi Live device's connection to the host, in Node for Max. One TCP connection, one JSON object per
//  line, kept open and reopened every second while the host is not there. It carries messages only -- the Live
//  API is the v8 script's -- with two exceptions that are about the connection itself:
//    - nobody answering: the host is started, and again every RELAUNCH_MS while still nobody answers -- a launch can fail,
//      or race with a host that is quitting, and a device that gave up after one would wait for ever;
//    - the host answering `busy` (another device has it): the v8 script is told once, and this one waits
//      five seconds between tries rather than flickering connected and gone.
//
//  The host started here is the one built in this folder, build-plugin's bambi live host app; without it, the
//  stand-in, build/bambi-live-stub, logging to ~/Library/Logs/bambi-live-stub.log and quitting ten seconds after
//  its last device has gone.

const maxApi = require("max-api");
const net = require("net");
const path = require("path");
const fs = require("fs");
const os = require("os");
const {spawn} = require("child_process");

const PORT = 9461;
const APP = path.resolve(__dirname,
                         "../../../build-plugin/apps/live-host/bambi-live-host_artefacts/Release/bambi live host.app");
const STUB = path.resolve(__dirname, "../../../build/bambi-live-stub");
const LOG = path.join(os.homedir(), "Library/Logs/bambi-live-stub.log");

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

let socket = null;
let open = false;
let refused = false;   // the host said busy on this connection
let startedAt = 0;     // when the host was last started since a connection was made; 0 when it was not
let pending = "";

const RELAUNCH_MS = 10000;  // how long a started host is given to answer before it is started again

//  Whether a refused connection should start the host now: never started since a connection, or not for RELAUNCH_MS.
function launchDue(now, lastStart) {
    return lastStart === 0 || now - lastStart >= RELAUNCH_MS;
}

function startHost() {
    const now = Date.now();
    if (!launchDue(now, startedAt)) return;
    if (fs.existsSync(APP)) {
        startedAt = now;
        spawn("open", [APP], {detached: true, stdio: "ignore"}).unref();
        maxApi.post("bambi: started " + APP);
    } else if (fs.existsSync(STUB)) {
        startedAt = now;
        const log = fs.openSync(LOG, "a");
        spawn(STUB, ["--quit-when-alone", "10"], {detached: true, stdio: ["ignore", log, log]}).unref();
        maxApi.post("bambi: no host built; started the stand-in " + STUB + ", log in " + LOG);
    }
}

function connect() {
    socket = net.createConnection({host: "127.0.0.1", port: PORT}, () => {
        open = true;
        startedAt = 0;
        //  a busy host says so at once: give it that moment before telling the script it is connected
        setTimeout(() => {
            if (open && !refused) maxApi.outlet("connected", 1);
        }, 150);
    });
    socket.setEncoding("utf8");
    socket.on("data", data => {
        pending += data;
        for (let at = pending.indexOf("\n"); at >= 0; at = pending.indexOf("\n")) {
            const line = pending.slice(0, at);
            pending = pending.slice(at + 1);
            if (line.length === 0) continue;
            if (line.indexOf('"busy"') >= 0) {
                refused = true;
                maxApi.outlet("refused");
                continue;
            }
            if (line.length <= PIECE) {
                maxApi.outlet("fromhost", line);
                continue;
            }
            for (const piece of pieces(line)) maxApi.outlet("fromhost", "+" + piece);
            maxApi.outlet("fromhost", ".");
        }
    });
    socket.on("error", e => {
        if (e.code === "ECONNREFUSED") startHost();
    });
    socket.on("close", () => {
        if (open && !refused) maxApi.outlet("connected", 0);
        const wait = refused ? 5000 : 1000;
        open = false;
        refused = false;
        socket = null;
        pending = "";
        fromDevice = "";
        setTimeout(connect, wait);
    });
}

let fromDevice = "";  // the pieces of a long line from the device, until its "."
maxApi.addHandler("send", sent => {
    let line = String(sent);
    if (line[0] === "+") {
        fromDevice += line.slice(1);
        return;
    }
    if (line === ".") {
        line = fromDevice;
        fromDevice = "";
    }
    if (open && !refused) socket.write(line + "\n");
});

connect();
