// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The bambi Live device's connection to the host, in Node for Max. One TCP connection, one JSON object per
//  line, kept open and reopened every second while the host is not there. It carries messages only: the Live
//  API is the v8 script's, and every line goes to it unread.

const maxApi = require("max-api");
const net = require("net");

const PORT = 9461;
let socket = null;
let open = false;
let pending = "";

function connect() {
    socket = net.createConnection({host: "127.0.0.1", port: PORT}, () => {
        open = true;
        maxApi.outlet("connected", 1);
    });
    socket.setEncoding("utf8");
    socket.on("data", data => {
        pending += data;
        for (let at = pending.indexOf("\n"); at >= 0; at = pending.indexOf("\n")) {
            const line = pending.slice(0, at);
            pending = pending.slice(at + 1);
            if (line.length > 0) maxApi.outlet("fromhost", line);
        }
    });
    socket.on("error", () => {});  // a refused connection closes too; the close retries
    socket.on("close", () => {
        if (open) maxApi.outlet("connected", 0);
        open = false;
        socket = null;
        pending = "";
        setTimeout(connect, 1000);
    });
}

maxApi.addHandler("send", line => {
    if (open) socket.write(String(line) + "\n");
});

connect();
