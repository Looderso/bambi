#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Writes bambi-live.amxd, the Max for Live audio effect that talks to the host: "open host", a
# status, the track list (bambi-live-list.js, a v8ui), the routing snapshot's save and restore, an unnamed dict
# parameter the set keeps the device's memory in, and the track's audio passed through untouched. bambi-live.js holds the Live side, bambi-live-node.js
# the connection. An .amxd is three chunks -- "ampf" with the device type, an empty "meta", and "ptch"
# holding the patcher as JSON ending in a newline and a NUL. Needs Live 12 Suite installed, whose empty
# audio effect it starts from: a patcher missing a key Max expects crashes Live while the device loads.
#
#   tools/live-stub/max/make-device.py
import json, os, struct

here = os.path.dirname(os.path.abspath(__file__))


def box(id, maxclass, rect, inlets, outlets, **extra):
    b = {"id": id, "maxclass": maxclass, "numinlets": inlets, "numoutlets": outlets, "patching_rect": rect}
    b.update(extra)
    return {"box": b}


def obj(id, text, rect, inlets, outlets, types=None):
    return box(id, "newobj", rect, inlets, outlets, text=text, outlettype=types or [""] * outlets)


def line(src, out, dst, inlet):
    return {"patchline": {"source": [src, out], "destination": [dst, inlet]}}


def button(id, label, rect, presentation):
    """A press, not a state: no parameter, so nothing is stored or automated."""
    return box(id, "live.text", rect, 1, 2, outlettype=["", ""], mode=0, text=label, texton=label,
               parameter_enable=0, presentation=1, presentation_rect=presentation)


def label(id, text, rect, presentation):
    """Text in the device view, sized as Ableton's own devices size theirs: no line counts, which make Max fit the
    width to them."""
    return box(id, "comment", rect, 1, 0, text=text, presentation=1, presentation_rect=presentation,
               fontname="Arial", fontsize=10.0)


#  Three columns in Live's device height: "open host" and the status; the track list; the
#  routing snapshot.
boxes = [
    obj("in", "plugin~", [20, 20, 60, 22], 2, 2, ["signal", "signal"]),
    obj("out", "plugout~", [20, 420, 60, 22], 2, 0),
    obj("device", "live.thisdevice", [120, 20, 90, 22], 1, 3, ["bang", "int", "int"]),
    obj("order", "t b b", [120, 50, 40, 22], 1, 2, ["bang", "bang"]),
    #  what the set keeps: an unnamed dict that is a parameter -- Live saves its contents with the set and gives
    #  them back on load. Unnamed, because a name is either shared by every device in the set or, with "---",
    #  new on every load, where Live cannot find the saved data again; not embedded, which saves into the patcher
    box("store", "newobj", [200, 80, 150, 22], 2, 4, text="dict", outlettype=["dictionary", "", "", ""],
        varname="bambistate", saved_object_attributes={"parameter_enable": 1, "embed": 0},
        saved_attribute_attributes={"valueof": {
            "parameter_longname": "bambi state", "parameter_shortname": "state", "parameter_type": 3,
            "parameter_invisible": 1}}),
    button("open", "open host", [500, 20, 80, 20], [8, 8, 120, 26]),
    obj("toshow", "prepend show", [500, 50, 80, 22], 1, 1),
    button("save", "save routing", [600, 20, 80, 20], [408, 56, 104, 18]),
    obj("tosave", "prepend save", [600, 50, 80, 22], 1, 1),
    button("restore", "restore saved", [700, 20, 80, 20], [408, 78, 104, 18]),
    obj("torestore", "prepend restoreSaved", [700, 50, 120, 22], 1, 1),
    obj("defer", "deferlow", [120, 120, 60, 22], 1, 1),
    obj("js", "v8 bambi-live.js", [120, 160, 120, 22], 1, 4),
    obj("split", "route send status detail meta saved notice", [120, 200, 260, 22], 1, 7),
    obj("tohost", "prepend send", [120, 240, 80, 22], 1, 1),
    obj("node", "node.script bambi-live-node.js @autostart 1", [120, 380, 240, 22], 1, 2),
    obj("fromnode", "deferlow", [380, 380, 60, 22], 1, 1),
    box("list", "v8ui", [460, 200, 260, 158], 1, 1, outlettype=[""], filename="bambi-live-list.js", border=0,
        presentation=1, presentation_rect=[140, 6, 260, 158]),
    #  the status is one short word or two; what it means goes beneath, with room to wrap
    box("light", "panel", [140, 280, 8, 8], 1, 0, presentation=1, presentation_rect=[8, 67, 8, 8],
        border=1, bordercolor=[0.35, 0.35, 0.35, 1.0], bgfillcolor=[0.0, 0.0, 0.0, 0.0], rounded=0),
    label("status", "", [160, 280, 112, 14], [20, 64, 112, 14]),
    label("detail", "", [160, 300, 124, 66], [8, 80, 124, 66]),
    label("meta", "", [160, 320, 124, 14], [8, 150, 124, 14]),
    label("heading", "routing", [300, 280, 104, 14], [408, 6, 104, 14]),
    label("saved", "", [300, 300, 104, 28], [408, 22, 104, 28]),
    label("notice", "", [300, 320, 104, 64], [408, 100, 104, 64]),
] + [obj("set" + n, "prepend set", [160 + 60 * i, 260, 70, 22], 1, 1)
     for i, n in enumerate(["status", "detail", "meta", "saved", "notice"])]
lines = [
    line("in", 0, "out", 0),
    line("in", 1, "out", 1),
    line("device", 0, "order", 0),
    line("order", 1, "store", 0),
    line("store", 0, "js", 0),
    line("order", 0, "defer", 0),
    line("open", 0, "toshow", 0),
    line("toshow", 0, "defer", 0),
    line("save", 0, "tosave", 0),
    line("tosave", 0, "defer", 0),
    line("restore", 0, "torestore", 0),
    line("torestore", 0, "defer", 0),
    line("list", 0, "defer", 0),
    line("defer", 0, "js", 0),
    line("js", 0, "split", 0),
    line("split", 0, "tohost", 0),
    line("tohost", 0, "node", 0),
    line("js", 1, "list", 0),
    line("js", 2, "restore", 0),
    line("js", 3, "light", 0),
    #  what the host says, back to the script outside the Node process's thread
    line("node", 0, "fromnode", 0),
    line("fromnode", 0, "js", 0),
] + [l for i, n in enumerate(["status", "detail", "meta", "saved", "notice"])
     for l in (line("split", i + 1, "set" + n, 0), line("set" + n, 0, n, 0))]

TEMPLATE = "/Applications/Ableton Live 12 Suite.app/Contents/App-Resources/Misc/Max Devices/Max Audio Effect.amxd"
raw = open(TEMPLATE, "rb").read()
patcher = json.loads(raw[raw.index(b"ptch") + 8:].rstrip(b"\0").decode())
p = patcher["patcher"]
p["boxes"] = boxes
p["lines"] = lines
p["parameters"] = {"store": ["bambi state", "state", 0]}
p["openinpresentation"] = 1
p["devicewidth"] = 520.0

body = (json.dumps(patcher, indent="\t") + "\n").encode() + b"\0"
data = b"ampf" + struct.pack("<I", 4) + b"aaaa" + b"meta" + struct.pack("<I", 4) + b"\0" * 4
data += b"ptch" + struct.pack("<I", len(body)) + body
with open(os.path.join(here, "bambi-live.amxd"), "wb") as f:
    f.write(data)
print("wrote bambi-live.amxd,", len(data), "bytes")
