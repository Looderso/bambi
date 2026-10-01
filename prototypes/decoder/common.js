// Shared by the decoder's prototypes: spherical harmonics and rotations, the controls, the OSC tracker, the map.
"use strict";
const D2R = Math.PI / 180;

// ---------- spherical harmonics: real, SN3D, ACN, no Condon-Shortley phase ----------
const FACT = [1]; for (let i = 1; i <= 64; i++) FACT[i] = FACT[i - 1] * i;   // up to order 32
function shN(N, x, y, z) {
  const az = Math.atan2(y, x), s = Math.max(-1, Math.min(1, z)), c = Math.sqrt(Math.max(0, 1 - s * s));
  const out = new Float64Array((N + 1) ** 2);
  for (let m = 0; m <= N; m++) {
    let pmm = 1; for (let i = 1; i <= m; i++) pmm *= (2 * i - 1) * c;      // P_m^m(s)
    let p0 = 0, p1 = pmm;
    for (let n = m; n <= N; n++) {
      let p;
      if (n === m) p = pmm;
      else if (n === m + 1) p = s * (2 * m + 1) * pmm;
      else p = ((2 * n - 1) * s * p1 - (n + m - 1) * p0) / (n - m);
      if (n > m) { p0 = p1; p1 = p; } else { p1 = p; }
      const norm = Math.sqrt((m === 0 ? 1 : 2) * FACT[n - m] / FACT[n + m]) * p;
      if (m === 0) out[n * n + n] = norm;
      else { out[n * n + n + m] = norm * Math.cos(m * az); out[n * n + n - m] = norm * Math.sin(m * az); }
    }
  }
  return out;
}
const dirOf = (az, el) => [Math.cos(el) * Math.cos(az), Math.cos(el) * Math.sin(az), Math.sin(el)];
const angOf = v => [Math.atan2(v[1], v[0]), Math.asin(Math.max(-1, Math.min(1, v[2])))];

// 3x3 rotations, row-major
const mul3 = (a, b) => a.map((_, i) => { const r = Math.floor(i / 3), c = i % 3;
  return a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c]; });
const app3 = (m, v) => [m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
                        m[6] * v[0] + m[7] * v[1] + m[8] * v[2]];
const tr3 = m => [m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]];
const rz = a => [Math.cos(a), -Math.sin(a), 0, Math.sin(a), Math.cos(a), 0, 0, 0, 1];
const ry = a => [Math.cos(a), 0, Math.sin(a), 0, 1, 0, -Math.sin(a), 0, Math.cos(a)];
const rx = a => [1, 0, 0, 0, Math.cos(a), -Math.sin(a), 0, Math.sin(a), Math.cos(a)];
// The head's orientation in the world: yaw left-positive, pitch up-positive, roll right-ear-down positive.
const headRot = h => mul3(mul3(rz(h.yaw * D2R), ry(-h.pitch * D2R)), rx(h.roll * D2R));

// ---------- controls ----------
function $(id) { return document.getElementById(id); }
function seg(el, items, get, set, label = x => x, disabled = () => false) {
  el.innerHTML = "";
  items.forEach(it => { const b = document.createElement("button"); b.textContent = label(it); b.disabled = disabled(it);
    b.onclick = () => { set(it); seg(el, items, get, set, label, disabled); }; if (get() === it) b.classList.add("on"); el.appendChild(b); });
}
// A value: drag sideways or up (the two add), a click on the bar sets it there, shift is fine.
function ctl(parent, label, min, max, get, set, fmt) {
  const d = document.createElement("div"); d.className = "ctl";
  d.innerHTML = `<div class="ctl-top"><span class="lab">${label}</span><span class="val"></span></div><div class="bar"><div class="fill"></div></div>`;
  parent.appendChild(d);
  const bar = d.querySelector(".bar"), fill = d.querySelector(".fill"), val = d.querySelector(".val");
  const draw = () => { const v = get(), z = Math.max(min, Math.min(max, 0)), a = (Math.min(v, z) - min) / (max - min), b = (Math.max(v, z) - min) / (max - min);
    fill.style.left = a * 100 + "%"; fill.style.width = (b - a) * 100 + "%"; val.textContent = fmt(v); };
  bar.onpointerdown = e => {
    const r = bar.getBoundingClientRect(); set(min + (max - min) * Math.max(0, Math.min(1, (e.clientX - r.left) / r.width))); draw();
    let x0 = e.clientX, y0 = e.clientY; bar.setPointerCapture(e.pointerId);
    bar.onpointermove = m => { const k = m.shiftKey ? 0.1 : 1;
      const dv = ((m.clientX - x0) / r.width + (y0 - m.clientY) / 200) * (max - min) * k;
      x0 = m.clientX; y0 = m.clientY; set(Math.max(min, Math.min(max, get() + dv))); draw(); };
    bar.onpointerup = () => { bar.onpointermove = null; };
  };
  draw(); return draw;
}

// ---------- the tracker: OSC through osc-bridge.py ----------
// Orientation arrives as a quaternion or as yaw-pitch-roll; either becomes a 3x3 rotation of the device in a
// gravity-aligned world (z up), is mapped to the head's axes (x front, y left, z up), has the yaw at recentre taken
// off — pitch and roll stay against gravity — and is smoothed. The result replaces the sliders while OSC is on.
const T = { ws: null, on: false, read: "auto", addr: null, interp: "quat-wxyz", flip: [false, false, false],
  axes: "head", smooth: 0.05, raw: null, yaw0: null, R: null, q: [1, 0, 0, 0], target: null,
  seen: {}, parts: {}, count: 0, rate: 0, last: "", tick: performance.now(),
  hooks: { onHead: () => {}, onToggle: () => {} } };
// ADM-OSC's pitch is up-positive, as the suite's; IEM's and SPARTA's is a rotation about +y (left), so down-positive.
const KNOWN = { "/adm/lis/ypr": "ypr-deg", "/ypr": "ypr-deg-down", "/SceneRotator/ypr": "ypr-deg-down",
  "/quaternion": "quat-wxyz", "/SceneRotator/quaternions": "quat-wxyz", "/rotationvector": "quat-xyzw" };
const INTERP = { "quat-wxyz": "quat w x y z", "quat-xyzw": "quat x y z w", "ypr-deg": "ypr °, pitch up +",
  "ypr-deg-down": "ypr °, pitch down +", "ypr-rad": "ypr rad, pitch up +" };
// Rows: the head's front, left and up, in the device's coordinates.
const AXES = {
  head: { label: "as sent", M: [1, 0, 0, 0, 1, 0, 0, 0, 1] },
  phoneTop: { label: "phone, top edge", M: [0, 1, 0, -1, 0, 0, 0, 0, 1] },
  phoneBack: { label: "phone, back", M: [0, 0, -1, -1, 0, 0, 0, 1, 0] },
};
const qToM = ([w, x, y, z]) => [1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
  2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
  2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)];
function mToQ(m) {
  const tr = m[0] + m[4] + m[8]; let w, x, y, z;
  if (tr > 0) { const s = Math.sqrt(tr + 1) * 2; w = s / 4; x = (m[7] - m[5]) / s; y = (m[2] - m[6]) / s; z = (m[3] - m[1]) / s; }
  else if (m[0] > m[4] && m[0] > m[8]) { const s = Math.sqrt(1 + m[0] - m[4] - m[8]) * 2; w = (m[7] - m[5]) / s; x = s / 4; y = (m[1] + m[3]) / s; z = (m[2] + m[6]) / s; }
  else if (m[4] > m[8]) { const s = Math.sqrt(1 + m[4] - m[0] - m[8]) * 2; w = (m[2] - m[6]) / s; x = (m[1] + m[3]) / s; y = s / 4; z = (m[5] + m[7]) / s; }
  else { const s = Math.sqrt(1 + m[8] - m[0] - m[4]) * 2; w = (m[3] - m[1]) / s; x = (m[2] + m[6]) / s; y = (m[5] + m[7]) / s; z = s / 4; }
  return [w, x, y, z];
}
function slerp(a, b, k) {
  let d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (d < 0) { b = b.map(v => -v); d = -d; }
  const r = d > 0.9995 ? a.map((v, i) => v + k * (b[i] - v)) : (() => { const th = Math.acos(d), s = Math.sin(th);
    return a.map((v, i) => (Math.sin((1 - k) * th) * v + Math.sin(k * th) * b[i]) / s); })();
  const n = Math.hypot(...r); return r.map(v => v / n);
}
function toMatrix(interp, v) {
  const f = T.flip;
  if (interp.startsWith("quat")) {
    let [w, x, y, z] = interp === "quat-wxyz" ? v : [v[3], v[0], v[1], v[2]];
    const n = Math.hypot(w, x, y, z) || 1; [w, x, y, z] = [w / n, x / n, y / n, z / n];
    return qToM([w, f[0] ? -x : x, f[1] ? -y : y, f[2] ? -z : z]);
  }
  const k = interp === "ypr-rad" ? 180 / Math.PI : 1, down = interp === "ypr-deg-down" ? -1 : 1;
  return headRot({ yaw: (f[0] ? -1 : 1) * v[0] * k, pitch: (f[1] ? -1 : 1) * down * v[1] * k, roll: (f[2] ? -1 : 1) * v[2] * k });
}
const yawOf = m => Math.atan2(m[3], m[0]);
function onOsc(m) {
  T.count++; T.seen[m.address] = m.args; T.last = m.address;
  let interp = null, v = m.args;
  // Some apps send each value under its own address; gather them.
  const part = m.address.match(/^(\/rotationvector|\/SceneRotator)\/(X|Y|Z|cos|yaw|pitch|roll)$/i);
  if (part) { const g = T.parts[part[1]] ||= {}; g[part[2].toLowerCase()] = v[0];
    if (part[1] === "/rotationvector" && part[2] === "cos" && "x" in g) { interp = "quat-xyzw"; v = [g.x, g.y, g.z, g.cos]; }
    else if (part[1] === "/SceneRotator" && "yaw" in g) { interp = "ypr-deg-down"; v = [g.yaw, g.pitch ?? 0, g.roll ?? 0]; } }
  if (T.read === "auto" && KNOWN[m.address]) interp = KNOWN[m.address];
  if (T.read === "learn") { if (m.address !== T.addr) return; interp = T.interp; }
  if (!interp || v.length < (interp.startsWith("quat") ? 4 : 3) || v.some(x => typeof x !== "number")) return;
  T.raw = mul3(toMatrix(interp, v), tr3(AXES[T.axes].M));    // the head's axes in the world
  if (T.yaw0 === null) T.yaw0 = yawOf(T.raw);
  T.target = mToQ(mul3(rz(-T.yaw0), T.raw));
  trackerFrame();
}
// Run on every message, not on the animation frame: a hidden tab stops its animation frames, and the head must
// keep turning while another window is in front.
function trackerFrame() {
  const now = performance.now(), dt = (now - T.tick) / 1000, step = Math.min(0.1, (now - (T.stepAt ?? now)) / 1000);
  T.stepAt = now;
  if (dt > 1) { T.rate = T.count / dt; T.count = 0; T.tick = now; drawTrackerNote(); if (T.read === "learn") drawLearn(); }
  if (!T.on || !T.target) return;
  T.q = slerp(T.q, T.target, T.smooth > 0 ? 1 - Math.exp(-step / T.smooth) : 1);
  T.R = qToM(T.q);
  const f = app3(T.R, [1, 0, 0]);
  T.ypr = { yaw: Math.atan2(f[1], f[0]) / D2R, pitch: Math.asin(Math.max(-1, Math.min(1, f[2]))) / D2R,
             roll: Math.atan2(T.R[7], T.R[8]) / D2R };
  T.hooks.onHead(T.R, T.ypr);
}
function drawTrackerNote() {
  if (!T.ws) return;
  const known = KNOWN[T.last] ? ` — ${INTERP[KNOWN[T.last]]}` : (T.last ? " — unknown, use learn" : "");
  $("wsNote").textContent = T.ws.readyState === 1
    ? (T.rate ? `${T.rate.toFixed(0)} messages/s, last ${T.last}${known}` : "Connected. Nothing arriving yet — check the sender's address and port.")
    : "Not connected — is osc-bridge.py running?";
}
function drawLearn() {
  $("learnBox").style.display = T.read === "learn" ? "" : "none";
  const addrs = Object.keys(T.seen);
  seg($("segAddr"), addrs.length ? addrs : ["(nothing yet)"], () => T.addr, v => { T.addr = v; T.yaw0 = null; });
  seg($("segInterp"), Object.keys(INTERP), () => T.interp, v => { T.interp = v; T.yaw0 = null; }, v => INTERP[v]);
}
// The tracker's section of a page: its controls go into el; onHead(R, ypr) is called with each new orientation
// while OSC drives the head, onToggle(on) when that is switched.
function mountTracker(el, hooks) {
  Object.assign(T.hooks, hooks);
  el.innerHTML = `
    <h2>TRACKER · OSC</h2>
    <div class="row"><button id="bWs">Connect bridge</button><button id="bOsc" disabled>OSC drives the head</button></div>
    <p class="note" id="wsNote">Run <b>python3 prototypes/decoder/osc-bridge.py</b> (add <b>--network</b> for a phone),
      then connect. OSC port 9000; if REAPER or another app holds it, <b>--osc-port 9001</b>.</p>
    <div class="sub note">read as</div>
    <div class="seg" id="segRead"></div>
    <div id="learnBox" style="display:none">
      <div class="sub note">address</div><div class="seg" id="segAddr"></div>
      <div class="sub note">its values are</div><div class="seg" id="segInterp"></div>
      <div class="sub note">flip</div><div class="row" id="rowFlip"></div>
    </div>
    <div class="sub note">the device's axes</div>
    <div class="seg" id="segAxes"></div>
    <div id="smoothCtl"></div>
    <p class="note">On, OSC overrides the sliders, the keys and the drag. <kbd>R</kbd> recentres on where the device
      points now. A phone points with its top edge (held flat) or its back (held upright).</p>
`;
  seg($("segRead"), ["auto", "learn"], () => T.read, v => { T.read = v; T.yaw0 = null; drawLearn(); },
      v => v === "auto" ? "Known formats" : "Learn");
  seg($("segAxes"), Object.keys(AXES), () => T.axes, v => { T.axes = v; T.yaw0 = null; }, v => AXES[v].label);
  ["x / yaw", "y / pitch", "z / roll"].forEach((lab, i) => { const b = document.createElement("button"); b.textContent = lab;
    b.onclick = () => { T.flip[i] = !T.flip[i]; b.classList.toggle("on", T.flip[i]); T.yaw0 = null; }; $("rowFlip").appendChild(b); });
  ctl($("smoothCtl"), "smoothing", 0, 0.3, () => T.smooth, v => { T.smooth = v; }, v => (v * 1000).toFixed(0) + " ms");
  $("bWs").onclick = () => {
    if (T.ws) { T.ws.close(); return; }
    const ws = new WebSocket("ws://127.0.0.1:8766"); T.ws = ws;
    ws.onopen = () => { $("bWs").textContent = "Disconnect"; $("bOsc").disabled = false; drawTrackerNote(); };
    ws.onmessage = e => onOsc(JSON.parse(e.data));
    ws.onclose = () => { T.ws = null; T.on = false; $("bWs").textContent = "Connect bridge"; $("bOsc").disabled = true;
      $("bOsc").classList.remove("on"); $("wsNote").textContent = "Disconnected — is osc-bridge.py running?"; };
  };
  $("bOsc").onclick = () => { T.on = !T.on; $("bOsc").classList.toggle("on", T.on); T.yaw0 = null; T.q = [1, 0, 0, 0];
    T.hooks.onToggle(T.on); };
}

// ---------- the map ----------
// Equirectangular, front in the middle, left on the left: x from azimuth +180 (left edge) to -180.
function mapGeom(cv) { const r = cv.getBoundingClientRect(), dpr = devicePixelRatio || 1;
  if (cv.width !== Math.round(r.width * dpr)) { cv.width = Math.round(r.width * dpr); cv.height = Math.round(r.height * dpr); }
  const pad = 24 * dpr, w = cv.width - 2 * pad, h = Math.min(cv.height - 2 * pad - 10 * dpr, w / 2);
  return { dpr, x0: pad + (w - 2 * h) / 2 * (w > 2 * h), y0: (cv.height - h) / 2 + 6 * dpr, w: Math.min(w, 2 * h), h }; }
const toXY = (g, az, el) => [g.x0 + (180 - az) / 360 * g.w, g.y0 + (90 - el) / 180 * g.h];
const fromXY = (g, x, y) => [180 - (x - g.x0) / g.w * 360, 90 - (y - g.y0) / g.h * 180];

function drawGrid(c, g) {
  c.fillStyle = "#efede7"; c.fillRect(g.x0, g.y0, g.w, g.h);
  c.strokeStyle = "#c8c4ba"; c.lineWidth = g.dpr;
  for (let az = -180; az <= 180; az += 45) { const [x] = toXY(g, az, 0); c.beginPath(); c.moveTo(x, g.y0); c.lineTo(x, g.y0 + g.h); c.stroke(); }
  for (let el = -90; el <= 90; el += 45) { const [, y] = toXY(g, 0, el); c.beginPath(); c.moveTo(g.x0, y); c.lineTo(g.x0 + g.w, y); c.stroke(); }
  c.fillStyle = "#6f6b62"; c.font = `${9 * g.dpr}px Helvetica Neue, sans-serif`; c.textAlign = "center";
  [[180, "back"], [90, "left"], [0, "front"], [-90, "right"], [-180, "back"]].forEach(([az, t]) => { const [x] = toXY(g, az, 0); c.fillText(t, x, g.y0 + g.h + 12 * g.dpr); });
}
