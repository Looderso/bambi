// SPDX-License-Identifier: GPL-3.0-or-later
//
//  The bambi Live device's track list, drawn in a v8ui: a headline ("4 tracks on bambi"), then every track
//  with its colour and what bambi does with it -- a pair of the loop device, into its group, in Live -- a
//  child indented under its group, a warning marked "!". A list longer than the box scrolls when dragged.
//
//  It is given the whole list as one JSON text, `rows <json>`: {head, sub, rows: [{name, colour, child, to,
//  kind, warn}]}, `kind` one of "pair", "stray" or "" for a plain destination.
//
//  A press on a row with a warning sends `warning <text>` out, for the device to show in full.
//
//  Drawn in dark text on the device's own background, which suits Live's light themes; a dark theme is not
//  yet asked for its colours.

outlets = 1;
mgraphics.init();
mgraphics.relative_coords = 0;
mgraphics.autofill = 0;

const ROW = 15, HEAD = 18;
const INK = [0.106, 0.102, 0.090, 1.0];       // bambi's black
const DIM = [0.35, 0.35, 0.35, 1.0];
const WARN = [0.62, 0.40, 0.05, 1.0];
const PAIR = [0.369, 0.545, 0.682, 1.0];      // the encoder's blue: what bambi owns
const RULE = [0.0, 0.0, 0.0, 0.18];

let data = {head: "", sub: "", rows: []};
let scroll = 0;   // pixels the rows are scrolled up
let grabbedAt = 0, grabbedScroll = 0;

function rows(json) {
    try {
        data = JSON.parse(String(json));
    } catch (e) {
        return;
    }
    scroll = Math.min(scroll, maxScroll());
    mgraphics.redraw();
}

const PAD = 4;  // air on the right, where text would touch the box's edge

function size() {
    return [box.rect[2] - box.rect[0] - PAD, box.rect[3] - box.rect[1]];
}

function maxScroll() {
    return Math.max(0, data.rows.length * ROW - (size()[1] - HEAD));
}

function hex(colour) {
    const n = parseInt(String(colour).replace("#", ""), 16);
    return [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255, 1.0];
}

function text(str, x, y, colour) {
    mgraphics.set_source_rgba(colour);
    mgraphics.move_to(x, y);
    mgraphics.show_text(String(str));
}

//  Shortened to fit `width`, with an ellipsis.
function fit(str, width) {
    let s = String(str);
    if (mgraphics.text_measure(s)[0] <= width) return s;
    while (s.length > 1 && mgraphics.text_measure(s + "…")[0] > width) s = s.slice(0, -1);
    return s + "…";
}

function paint() {
    const [w, h] = size();
    mgraphics.select_font_face("Arial");
    mgraphics.set_font_size(10);

    text(data.head, 0, 11, INK);
    const subWidth = mgraphics.text_measure(String(data.sub))[0];
    text(data.sub, w - subWidth, 11, DIM);
    mgraphics.set_source_rgba(RULE);
    mgraphics.rectangle(0, HEAD - 2, w, 1);
    mgraphics.fill();

    //  v8ui offers no clipping: a row is drawn only when it lies wholly below the headline and inside the box
    data.rows.forEach((r, i) => {
        const y = HEAD + i * ROW - scroll;
        if (y < HEAD || y + ROW > h) return;
        const indent = r.child ? 10 : 0;
        mgraphics.set_source_rgba(hex(r.colour));
        mgraphics.rectangle(indent, y + 4, 8, 8);
        mgraphics.fill();

        mgraphics.set_font_size(9);
        const label = String(r.to);
        const labelWidth = mgraphics.text_measure(label)[0] + (r.kind === "pair" ? 8 : 0);
        if (r.kind === "pair") {
            mgraphics.set_source_rgba(PAIR);
            mgraphics.rectangle(w - labelWidth, y + 2, labelWidth, 12);
            mgraphics.fill();
            text(label, w - labelWidth + 4, y + 11, [1, 1, 1, 1]);
        } else {
            text(label, w - labelWidth, y + 11, r.kind === "stray" ? WARN : DIM);
        }

        mgraphics.set_font_size(10);
        const nameX = indent + 13, room = w - labelWidth - nameX - 6;
        const name = fit(r.name, room - (r.warn ? 10 : 0));
        text(name, nameX, y + 11, r.child ? DIM : INK);
        if (r.warn) text("!", nameX + mgraphics.text_measure(name)[0] + 4, y + 11, WARN);
    });
}

function onclick(x, y) {
    grabbedAt = y;
    grabbedScroll = scroll;
    const i = Math.floor((y - HEAD + scroll) / ROW);
    if (y >= HEAD && i >= 0 && i < data.rows.length)
        outlet(0, "warning", data.rows[i].warn ? data.rows[i].name + ": " + data.rows[i].warn : "");
}

function ondrag(x, y, button) {
    if (!button) return;
    scroll = Math.max(0, Math.min(maxScroll(), grabbedScroll + (grabbedAt - y)));
    mgraphics.redraw();
}

function onresize() {
    mgraphics.redraw();
}
