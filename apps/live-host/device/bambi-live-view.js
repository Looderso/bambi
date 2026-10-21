// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The bambi Live device, drawn whole in one v8ui on the suite's paper. On the left: the wordmark, the link's state in
//  a word beside the header's square (ink while linked, light grey while waiting, grey when it failed or another
//  device has the host), why in a line or three, and at the foot the one button -- "show host" while linked, "open host" while
//  not -- over the host's latency and rate. On the right: how many of the set's tracks bambi plays, and the tracks
//  in two columns, each in its colour while bambi plays it and greyed while it stays in Live; a track on bambi's
//  channels that this device did not route is marked "!" in ink, as is one the host warns about. More tracks than fit
//  scroll when dragged.
//
//  Messages in: `status <word>`, `detail <why>`, `notice <text>` (shown in place of why until the next status),
//  `meta <text>`, `light <connected|waiting|failed|busy>`, and `rows <json>`: {head, rows: [{name, colour, kind,
//  warn}]}, `kind` one of "pair", "stray" or "". Out: `show` for the button, `warning <text>` for a marked track.

outlets = 1;
mgraphics.init();
mgraphics.relative_coords = 0;
mgraphics.autofill = 0;

//  the suite's paper and inks (ui/include/bambi/ui/Theme.h), and its sizes at 1x
const PAPER = [0.937, 0.929, 0.906, 1], INK = [0.106, 0.102, 0.090, 1], LABEL = [0.435, 0.420, 0.384, 1];
const RULE = [0.784, 0.769, 0.729, 1], DIM = [0.663, 0.647, 0.612, 1], WARN = INK;
const FACE = "Helvetica Neue";
const PAD = 12, LEFT = 166, SPLIT = 190, ROW = 13, BUTTON_H = 18, TOP = PAD + 16;

let word = "", why = "", note = "", info = "", lit = "waiting";
let data = {head: "", rows: []};
let scroll = 0, grabbedAt = 0, grabbedScroll = 0;
let buttonAt = null;  // where the button was last drawn, [x, y, w, h]; none while another device has the host

const joined = (args) => Array.prototype.slice.call(args).join(" ");
function status() { word = joined(arguments); note = ""; mgraphics.redraw(); }
function detail() { why = joined(arguments); mgraphics.redraw(); }
function notice() { note = joined(arguments); mgraphics.redraw(); }
function meta() { info = joined(arguments); mgraphics.redraw(); }
function light(state) { lit = String(state); mgraphics.redraw(); }
function rows(json) {
    try {
        data = JSON.parse(String(json));
    } catch (e) {
        return;
    }
    scroll = Math.min(scroll, maxScroll());
    mgraphics.redraw();
}

function size() { return [box.rect[2] - box.rect[0], box.rect[3] - box.rect[1]]; }

function hex(colour) {
    const n = parseInt(String(colour).replace("#", ""), 16);
    return [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255, 1.0];
}

function font(size) {
    mgraphics.select_font_face(FACE);
    mgraphics.set_font_size(size);
}

function width(str, tracking) {
    let w = 0;
    for (const ch of String(str)) w += mgraphics.text_measure(ch)[0] + tracking;
    return w;
}

//  `str` at `x`, its baseline at `y`, each letter `tracking` apart as the suite's labels are. Returns its width.
function text(str, x, y, colour, tracking) {
    mgraphics.set_source_rgba(colour);
    let at = x;
    for (const ch of String(str)) {
        mgraphics.move_to(at, y);
        mgraphics.show_text(ch);
        at += mgraphics.text_measure(ch)[0] + tracking;
    }
    return at - x;
}

//  Shortened to fit `room`, with an ellipsis.
function fit(str, room, tracking) {
    let s = String(str);
    if (width(s, tracking) <= room) return s;
    while (s.length > 1 && width(s + "…", tracking) > room) s = s.slice(0, -1);
    return s + "…";
}

//  Words wrapped into `room`, at most `most` lines, the last shortened when more would follow.
function wrap(str, room, most, tracking) {
    const lines = [];
    let line = "";
    for (const w of String(str).split(" ")) {
        const next = line === "" ? w : line + " " + w;
        if (line === "" || width(next, tracking) <= room) {
            line = next;
        } else {
            lines.push(line);
            line = w;
        }
    }
    if (line !== "") lines.push(line);
    if (lines.length > most) {
        lines.length = most;
        lines[most - 1] = fit(lines[most - 1] + " …", room, tracking);
    }
    return lines;
}

function fillRect(x, y, w, h, colour) {
    mgraphics.set_source_rgba(colour);
    mgraphics.rectangle(x, y, w, h);
    mgraphics.fill();
}

function frame(x, y, w, h, colour) {
    mgraphics.set_source_rgba(colour);
    mgraphics.set_line_width(1);
    mgraphics.rectangle(x + 0.5, y + 0.5, w - 1, h - 1);
    mgraphics.stroke();
}

function columns() {
    const x0 = SPLIT + 14, room = size()[0] - PAD - x0;
    return {x0: x0, room: room, column: (room - 10) / 2};
}

function maxScroll() { return Math.max(0, Math.ceil(data.rows.length / 2) * ROW - (size()[1] - PAD - TOP)); }

function paint() {
    const [w, h] = size();
    fillRect(0, 0, w, h, PAPER);

    //  the wordmark, as the header draws it
    font(15);
    text("βambi", PAD, PAD + 13, INK, 4.5);

    //  the link: the header's square, filled in the link's colour, and a word, then why
    const sy = PAD + 34;
    fillRect(PAD, sy, 7, 7, lit === "connected" ? INK : lit === "waiting" ? RULE : DIM);
    font(11);
    text(fit(word, LEFT - 14, 0.6), PAD + 14, sy + 7, INK, 0.6);
    font(9);
    wrap(note !== "" ? note : why, LEFT, 4, 0.9).forEach((l, i) => text(l, PAD, sy + 22 + i * 13, LABEL, 0.9));

    //  the foot: the button, over the latency and rate while linked
    const busy = lit === "busy", label = lit === "connected" ? "show host" : "open host";
    font(11);
    const bw = width(label, 0.2) + 18, by = h - PAD - BUTTON_H - 16;
    frame(PAD, by, bw, BUTTON_H, busy ? DIM : INK);
    text(label, PAD + 9, by + 13, busy ? DIM : INK, 0.2);
    buttonAt = busy ? null : [PAD, by, bw, BUTTON_H];
    font(9);
    if (lit === "connected" && info !== "") text(fit(info, LEFT, 1.2), PAD, h - PAD, LABEL, 1.2);

    //  the rule, and the tracks right of it
    fillRect(SPLIT, PAD, 1, h - 2 * PAD, RULE);
    const c = columns();
    text(fit(data.head || "", c.room, 1.2), c.x0, PAD + 7, LABEL, 1.2);
    //  v8ui offers no clipping: a row is drawn only when it lies wholly inside the list's part of the box
    data.rows.forEach((r, i) => {
        const x = c.x0 + (i % 2) * (c.column + 10), y = TOP + Math.floor(i / 2) * ROW - scroll;
        if (y < TOP || y + ROW > h - PAD) return;
        const plays = r.kind === "pair", marked = r.warn !== "" || r.kind === "stray";
        fillRect(x, y + 2, 3, 9, plays ? hex(r.colour) : RULE);
        const nw = text(fit(r.name, c.column - 8 - (marked ? 8 : 0), 0.5), x + 8, y + 10, plays ? INK : DIM, 0.5);
        if (marked) text("!", x + 8 + nw + 3, y + 10, WARN, 0);
    });
}

function onclick(x, y) {
    grabbedAt = y;
    grabbedScroll = scroll;
    if (buttonAt && x >= buttonAt[0] && x <= buttonAt[0] + buttonAt[2] && y >= buttonAt[1] &&
        y <= buttonAt[1] + buttonAt[3]) {
        outlet(0, "show");
        return;
    }
    const c = columns();
    if (x < c.x0 || y < TOP) return;
    const r = data.rows[Math.floor((y - TOP + scroll) / ROW) * 2 + (x >= c.x0 + c.column + 10 ? 1 : 0)];
    if (!r) return;
    const why_ = r.warn !== "" ? r.warn : r.kind === "stray" ? "on bambi's channels, not routed by this device" : "";
    if (why_ !== "") outlet(0, "warning", r.name + ": " + why_);
}

function ondrag(x, y, button) {
    if (!button) return;
    scroll = Math.max(0, Math.min(maxScroll(), grabbedScroll + (grabbedAt - y)));
    mgraphics.redraw();
}

function onresize() {
    mgraphics.redraw();
}
