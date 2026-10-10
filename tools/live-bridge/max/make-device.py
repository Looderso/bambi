#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Writes bambi-routing.amxd, the Max for Live audio effect around bambi-routing.js: one toggle, one line of
# status, the track's audio passed through untouched. An .amxd is three chunks -- "ampf" with the device
# type, an empty "meta", and "ptch" holding the patcher as JSON ending in a newline and a NUL. Needs Live 12
# Suite installed, whose empty audio effect it starts from.
#
#   tools/live-bridge/max/make-device.py
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


boxes = [
    obj("in", "plugin~", [20, 20, 60, 22], 2, 2, ["signal", "signal"]),
    obj("out", "plugout~", [20, 260, 60, 22], 2, 0),
    obj("device", "live.thisdevice", [120, 20, 90, 22], 1, 3, ["bang", "int", "int"]),
    obj("order", "t b b", [120, 50, 40, 22], 1, 2, ["bang", "bang"]),
    #  the remembered routing, saved with the set; "---" makes the name this device's own
    obj("store", "dict ---bambi @embed 1", [200, 80, 130, 22], 2, 4, ["dictionary", "", "", ""]),
    #  a parameter, so it lights as Live's toggles do; hidden, so it is neither automated nor saved -- the
    #  remembered routing says whether bambi is on, and the script sets the button to match
    box("toggle", "live.text", [240, 20, 80, 20], 1, 2, outlettype=["", ""], mode=1, text="bambi", texton="bambi",
        parameter_enable=1, presentation=1, presentation_rect=[8, 8, 70, 20],
        saved_attribute_attributes={"valueof": {
            "parameter_longname": "bambi routing", "parameter_shortname": "bambi", "parameter_type": 2,
            "parameter_enum": ["off", "on"], "parameter_mmax": 1, "parameter_invisible": 2}}),
    obj("prepend", "prepend enable", [240, 60, 90, 22], 1, 1),
    obj("defer", "deferlow", [120, 100, 60, 22], 1, 1),
    obj("js", "v8 bambi-routing.js", [120, 140, 130, 22], 1, 2),
    box("status", "comment", [120, 180, 220, 20], 1, 0, text="bambi routing", presentation=1,
        presentation_rect=[8, 34, 220, 60], linecount=1, presentation_linecount=4),
]
lines = [
    line("in", 0, "out", 0),
    line("in", 1, "out", 1),
    line("device", 0, "order", 0),
    line("order", 1, "store", 0),
    line("store", 0, "js", 0),
    line("order", 0, "defer", 0),
    line("toggle", 0, "prepend", 0),
    line("prepend", 0, "defer", 0),
    line("defer", 0, "js", 0),
    line("js", 0, "status", 0),
    line("js", 1, "toggle", 0),
]
#  Ableton's own empty audio effect is the base, so every key Max expects is there -- a missing "project" key
#  (its search path) crashes Live while the device loads. Only the boxes, the lines and the view change.
TEMPLATE = "/Applications/Ableton Live 12 Suite.app/Contents/App-Resources/Misc/Max Devices/Max Audio Effect.amxd"
raw = open(TEMPLATE, "rb").read()
at = raw.index(b"ptch") + 8
patcher = json.loads(raw[at:].rstrip(b"\0").decode())
p = patcher["patcher"]
p["boxes"] = boxes
p["lines"] = lines
p["openinpresentation"] = 1
p["devicewidth"] = 240.0

body = (json.dumps(patcher, indent="\t") + "\n").encode() + b"\0"
data = b"ampf" + struct.pack("<I", 4) + b"aaaa" + b"meta" + struct.pack("<I", 4) + b"\0" * 4
data += b"ptch" + struct.pack("<I", len(body)) + body
with open(os.path.join(here, "bambi-routing.amxd"), "wb") as f:
    f.write(data)
print("wrote bambi-routing.amxd,", len(data), "bytes")
