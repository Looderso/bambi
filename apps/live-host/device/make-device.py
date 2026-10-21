#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Writes bambi-live.amxd, the Max for Live audio effect that talks to the host: one v8ui (bambi-live-view.js) that
# draws the whole device -- the link's state, "show host", the tracks bambi plays -- an unnamed dict parameter the set
# keeps the device's memory in, and the track's audio passed through untouched. bambi-live.js holds the Live side,
# bambi-live-node.js the connection. An .amxd is three chunks -- "ampf" with the device type, an empty "meta", and "ptch"
# holding the patcher as JSON ending in a newline and a NUL. Needs Live 12 Suite installed, whose empty
# audio effect it starts from: a patcher missing a key Max expects crashes Live while the device loads.
#
#   apps/live-host/device/make-device.py
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


#  The device's one view, Live's device height, wide enough for the link's state and two columns of tracks.
WIDTH, HEIGHT = 360, 169

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
    obj("defer", "deferlow", [120, 120, 60, 22], 1, 1),
    obj("js", "v8 bambi-live.js", [120, 160, 120, 22], 1, 3),
    obj("split", "route send status detail meta notice", [120, 200, 220, 22], 1, 6),
    obj("tohost", "prepend send", [120, 240, 80, 22], 1, 1),
    obj("node", "node.script bambi-live-node.js @autostart 1", [120, 380, 240, 22], 1, 2),
    obj("fromnode", "deferlow", [380, 380, 60, 22], 1, 1),
    box("view", "v8ui", [460, 200, WIDTH, HEIGHT], 1, 1, outlettype=[""], filename="bambi-live-view.js", border=0,
        presentation=1, presentation_rect=[0, 0, WIDTH, HEIGHT]),
] + [obj("to" + n, "prepend " + n, [160 + 70 * i, 260, 80, 22], 1, 1)
     for i, n in enumerate(["status", "detail", "meta", "notice"])]
lines = [
    line("in", 0, "out", 0),
    line("in", 1, "out", 1),
    line("device", 0, "order", 0),
    line("order", 1, "store", 0),
    line("store", 0, "js", 0),
    line("order", 0, "defer", 0),
    #  the view's presses -- "show", a marked track's warning -- to the script
    line("view", 0, "defer", 0),
    line("defer", 0, "js", 0),
    line("js", 0, "split", 0),
    line("split", 0, "tohost", 0),
    line("tohost", 0, "node", 0),
    line("js", 1, "view", 0),
    line("js", 2, "view", 0),
    #  what the host says, back to the script outside the Node process's thread
    line("node", 0, "fromnode", 0),
    line("fromnode", 0, "js", 0),
] + [l for i, n in enumerate(["status", "detail", "meta", "notice"])
     for l in (line("split", i + 1, "to" + n, 0), line("to" + n, 0, "view", 0))]

TEMPLATE = "/Applications/Ableton Live 12 Suite.app/Contents/App-Resources/Misc/Max Devices/Max Audio Effect.amxd"
raw = open(TEMPLATE, "rb").read()
patcher = json.loads(raw[raw.index(b"ptch") + 8:].rstrip(b"\0").decode())
p = patcher["patcher"]
p["boxes"] = boxes
p["lines"] = lines
p["parameters"] = {"store": ["bambi state", "state", 0]}
p["openinpresentation"] = 1
p["devicewidth"] = float(WIDTH)

body = (json.dumps(patcher, indent="\t") + "\n").encode() + b"\0"
data = b"ampf" + struct.pack("<I", 4) + b"aaaa" + b"meta" + struct.pack("<I", 4) + b"\0" * 4
data += b"ptch" + struct.pack("<I", len(body)) + body
with open(os.path.join(here, "bambi-live.amxd"), "wb") as f:
    f.write(data)
print("wrote bambi-live.amxd,", len(data), "bytes")
