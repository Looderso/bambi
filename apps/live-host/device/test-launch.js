// SPDX-License-Identifier: GPL-3.0-or-later
//
//  When the bambi Live device starts the host, tested outside Max: the Node script is loaded with stand-ins for max-api, the
//  network, the file system and the clock, and its connection refused again and again.
//
//    node apps/live-host/device/test-launch.js

const fs = require("fs");
const path = require("path");
const vm = require("vm");

let now = 1000000;
let launches = 0;
let launched = [];
let sockets = [];
const timers = [];

function load(appExists) {
    launches = 0;
    launched = [];
    sockets = [];
    timers.length = 0;
    const stand = {
        "max-api": {post() {}, outlet() {}, addHandler() {}},
        net: {
            createConnection(opts, onConnect) {
                const handlers = {};
                const socket = {setEncoding() {}, write() {}, on(name, f) { handlers[name] = f; }, handlers, onConnect};
                sockets.push(socket);
                return socket;
            }
        },
        path: require("path"),
        fs: {existsSync(p) { return appExists && String(p).endsWith(".app"); }},
        child_process: {spawn(cmd, args) { ++launches; launched = args; return {unref() {}}; }},
    };
    const context = {
        require: name => stand[name] || stand[name.replace(/^node:/, "")],
        __dirname: path.join(__dirname),
        setTimeout: (f, ms) => timers.push({f, at: now + ms}),
        Date: {now: () => now},
        console,
    };
    vm.createContext(context);
    vm.runInContext(fs.readFileSync(path.join(__dirname, "bambi-live-node.js"), "utf8"), context);
    return context;
}

//  Time passes: every timer due runs, in order; a refused connection reports ECONNREFUSED and closes.
function elapse(ms) {
    const end = now + ms;
    for (;;) {
        timers.sort((a, b) => a.at - b.at);
        if (timers.length === 0 || timers[0].at > end) break;
        const t = timers.shift();
        now = Math.max(now, t.at);
        t.f();
        const s = sockets[sockets.length - 1];
        if (s && !s.settled) {
            s.settled = true;
            s.handlers.error({code: "ECONNREFUSED"});
            s.handlers.close();
        }
    }
    now = end;
}

let failed = 0, passed = 0;
function check(name, got, want) {
    if (JSON.stringify(got) === JSON.stringify(want)) ++passed;
    else {
        ++failed;
        console.log("FAIL " + name + ": got " + JSON.stringify(got) + ", want " + JSON.stringify(want));
    }
}

{
    //  Catches: a device that starts the host once and never again, which waits for ever when that launch fails.
    const c = load(true);
    const first = sockets[0];
    first.settled = true;
    first.handlers.error({code: "ECONNREFUSED"});
    first.handlers.close();
    check("the first refusal starts the host", launches, 1);
    elapse(9000);
    check("and the retries a second apart do not start it again", launches, 1);
    elapse(2000);
    check("but one that is still refused after ten seconds does", launches, 2);
    elapse(25000);
    check("and again every ten seconds while nobody answers", launches, 4);
}
{
    //  Catches: a launch counted after the connection is made, so that a host lost later is never started again.
    const c = load(true);
    const first = sockets[0];
    first.settled = true;
    first.handlers.error({code: "ECONNREFUSED"});
    first.handlers.close();
    elapse(1100);
    const second = sockets[sockets.length - 1];
    second.settled = true;
    second.onConnect();  // it answered
    second.handlers.close();  // and went away
    elapse(1100);  // the retry after it went away is refused: the host is started at once, not after ten seconds
    check("a host that answered and was lost is started again at the next refusal", launches, 2);
}
{
    //  Catches: a device away from a source checkout -- in Live's User Library -- that starts nothing, or starts the
    //  checkout's path, which is not there.
    const c = load(false);
    const first = sockets[0];
    first.settled = true;
    first.handlers.error({code: "ECONNREFUSED"});
    first.handlers.close();
    check("with no host built beside it, the installed host is started by its bundle id", launched,
          ["-b", "org.bambi.livehost"]);
}
{
    //  Catches: a checkout's own host passed over for an installed one, which would test the wrong build.
    const c = load(true);
    const first = sockets[0];
    first.settled = true;
    first.handlers.error({code: "ECONNREFUSED"});
    first.handlers.close();
    check("in a source checkout, the host built there is started", launched.length === 1 && launched[0].endsWith(".app"),
          true);
}

console.log(failed === 0 ? "ok: " + passed + " checks" : failed + " FAILED, " + passed + " passed");
process.exit(failed === 0 ? 0 : 1);
