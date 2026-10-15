// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The bambi Live device's connection to the host, in Node for Max. One TCP connection, one JSON object per
//  line, kept open and reopened every second while the host is not there. It carries messages only -- the Live
//  API is the v8 script's -- with two exceptions that are about the connection itself:
//    - nobody answering: the host is started, once, until a connection has been made and lost again;
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

let socket = null;
let open = false;
let refused = false;   // the host said busy on this connection
let started = false;   // the host was started since the last connection made
let pending = "";

function startHost() {
    if (started) return;
    if (fs.existsSync(APP)) {
        started = true;
        spawn("open", [APP], {detached: true, stdio: "ignore"}).unref();
        maxApi.post("bambi: started " + APP);
    } else if (fs.existsSync(STUB)) {
        started = true;
        const log = fs.openSync(LOG, "a");
        spawn(STUB, ["--quit-when-alone", "10"], {detached: true, stdio: ["ignore", log, log]}).unref();
        maxApi.post("bambi: no host built; started the stand-in " + STUB + ", log in " + LOG);
    }
}

function connect() {
    socket = net.createConnection({host: "127.0.0.1", port: PORT}, () => {
        open = true;
        started = false;
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
            maxApi.outlet("fromhost", line);
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
        setTimeout(connect, wait);
    });
}

maxApi.addHandler("send", line => {
    if (open && !refused) socket.write(String(line) + "\n");
});

connect();
