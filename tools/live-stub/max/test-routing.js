// SPDX-License-Identifier: GPL-3.0-or-later
//
//  Where the bambi Live device gives a track back to when bambi lets it go, tested outside Max: the device's script is
//  loaded with stand-ins for Max's globals and its pure givenBackTo() asked about the cases that matter.
//
//    node tools/live-stub/max/test-routing.js

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
const givenBackTo = context.givenBackTo;

let failed = 0, passed = 0;
function check(name, got, want) {
    if (got === want) ++passed;
    else {
        ++failed;
        console.log("FAIL " + name + ": got " + JSON.stringify(got) + ", want " + JSON.stringify(want));
    }
}

//  Catches: a grouped track given back to Main -- a routing remembered from before it was grouped, or hidden by bambi's
//  own channels when the set was saved -- so that it plays past its group; or a deliberate routing elsewhere overridden.
check("a grouped track remembered on Main goes back into its group", givenBackTo("Main", true), "@default");
check("as does one remembered on Master, Live's older name", givenBackTo("Master", true), "@default");
check("a track in no group remembered on Main goes back to Main", givenBackTo("Main", false), "Main");
check("a grouped track routed elsewhere on purpose keeps it", givenBackTo("Resampling", true), "Resampling");
check("its own default stays the default", givenBackTo("@default", true), "@default");

console.log(failed === 0 ? "routing: " + passed + " checks" : failed + " FAILED, " + passed + " passed");
process.exit(failed === 0 ? 0 : 1);
