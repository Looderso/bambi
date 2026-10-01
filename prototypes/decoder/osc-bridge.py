"""OSC over UDP in, WebSocket out — so a browser prototype can read a head tracker or a phone.

Every OSC message (bundles unpacked) goes to every connected page as JSON: {"address": "/ypr", "args": [..], "t": ..},
t its arrival in milliseconds of system uptime modulo 100 000 — so a sender that stamps its messages can be timed.
Python's standard library only.

  python osc-bridge.py                   OSC on 127.0.0.1:9000, WebSocket on 127.0.0.1:8766
  python osc-bridge.py --network         OSC on every interface, for a phone on the same network
  python osc-bridge.py --demo FORMAT     also send a slow synthetic head turn to the OSC port, in one of:
                                         adm, sparta-ypr, sparta-quat, iem-ypr, iem-quat, android

Port 9000 is SPARTA's default. Point Headitude, a tracker's bridge app or a phone app at this computer and port.
"""
import argparse, asyncio, base64, hashlib, json, math, socket, struct, time

clients = set()
stats = {"count": 0, "addresses": {}}


# ---------- OSC ----------
def _string(b, i):
    end = b.index(b"\0", i)
    return b[i:end].decode("utf-8", "replace"), (end + 4) & ~3


def parse_osc(b):
    """Yield (address, args) for a packet; bundles are unpacked."""
    if b.startswith(b"#bundle\0"):
        i = 16
        while i + 4 <= len(b):
            (size,) = struct.unpack(">i", b[i:i + 4])
            yield from parse_osc(b[i + 4:i + 4 + size])
            i += 4 + size
        return
    address, i = _string(b, 0)
    if i >= len(b):
        yield address, []
        return
    tags, i = _string(b, i)
    args = []
    for t in tags[1:]:
        if t == "f": args.append(struct.unpack(">f", b[i:i + 4])[0]); i += 4
        elif t == "i": args.append(struct.unpack(">i", b[i:i + 4])[0]); i += 4
        elif t == "d": args.append(struct.unpack(">d", b[i:i + 8])[0]); i += 8
        elif t == "h": args.append(struct.unpack(">q", b[i:i + 8])[0]); i += 8
        elif t == "s": s, i = _string(b, i); args.append(s)
        elif t == "b": (n,) = struct.unpack(">i", b[i:i + 4]); i = (i + 4 + n + 3) & ~3; args.append(None)
        elif t == "T": args.append(True)
        elif t == "F": args.append(False)
        elif t == "N": args.append(None)
    yield address, args


def build_osc(address, floats):
    def pad(s):
        s = s.encode() + b"\0"
        return s + b"\0" * (-len(s) % 4)
    return pad(address) + pad("," + "f" * len(floats)) + b"".join(struct.pack(">f", f) for f in floats)


class OscIn(asyncio.DatagramProtocol):
    def datagram_received(self, data, addr):
        arrived = (time.monotonic() * 1000) % 100_000   # ms, the same uptime clock a sender's timestamps use on macOS
        try:
            for address, args in parse_osc(data):
                stats["count"] += 1
                stats["addresses"][address] = stats["addresses"].get(address, 0) + 1
                msg = json.dumps({"address": address, "args": args, "from": addr[0], "t": arrived})
                for w in list(clients):
                    send_text(w, msg)
        except Exception as e:
            print("unreadable packet from", addr, e)


# ---------- WebSocket, server side: text frames out, the client's frames read and dropped ----------
def send_text(writer, text):
    data = text.encode()
    n = len(data)
    head = bytes([0x81, n]) if n < 126 else bytes([0x81, 126]) + struct.pack(">H", n) if n < 65536 \
        else bytes([0x81, 127]) + struct.pack(">Q", n)
    try:
        writer.write(head + data)
    except Exception:
        clients.discard(writer)


async def ws_client(reader, writer):
    try:
        request = await reader.readuntil(b"\r\n\r\n")
    except (asyncio.IncompleteReadError, ConnectionError):
        return                                   # a port probe, not a page
    key = [l.split(b":", 1)[1].strip() for l in request.split(b"\r\n") if l.lower().startswith(b"sec-websocket-key")]
    if not key:
        writer.close()
        return
    accept = base64.b64encode(hashlib.sha1(key[0] + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11").digest())
    writer.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")
    clients.add(writer)
    print("page connected,", len(clients), "open", flush=True)
    try:
        while True:
            b = await reader.readexactly(2)
            if b[0] & 0x0F == 0x8:
                break
            n = b[1] & 0x7F
            if n == 126: n = struct.unpack(">H", await reader.readexactly(2))[0]
            elif n == 127: n = struct.unpack(">Q", await reader.readexactly(8))[0]
            await reader.readexactly(n + (4 if b[1] & 0x80 else 0))
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    clients.discard(writer)
    writer.close()
    print("page left,", len(clients), "open", flush=True)


# ---------- a synthetic head, for trying the path without a device ----------
def quat_from_ypr(yaw, pitch, roll):
    """Degrees, the suite's convention (yaw left positive, pitch up positive, roll right ear down positive):
    the rotation Rz(yaw)·Ry(−pitch)·Rx(roll) as w, x, y, z."""
    cy, sy = math.cos(math.radians(yaw) / 2), math.sin(math.radians(yaw) / 2)
    cp, sp = math.cos(-math.radians(pitch) / 2), math.sin(-math.radians(pitch) / 2)
    cr, sr = math.cos(math.radians(roll) / 2), math.sin(math.radians(roll) / 2)
    return (cy * cp * cr + sy * sp * sr, cy * cp * sr - sy * sp * cr,
            cy * sp * cr + sy * cp * sr, sy * cp * cr - cy * sp * sr)


async def demo(fmt, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    t0 = time.time()
    while True:
        t = time.time() - t0
        yaw, pitch, roll = 60 * math.sin(2 * math.pi * t / 8), 20 * math.sin(2 * math.pi * t / 11), 0.0
        w, x, y, z = quat_from_ypr(yaw, pitch, roll)
        packets = {
            "adm": [build_osc("/adm/lis/ypr", [yaw, pitch, roll])],
            "sparta-ypr": [build_osc("/ypr", [yaw, -pitch, roll])],          # their pitch is down-positive
            "sparta-quat": [build_osc("/quaternion", [w, x, y, z])],
            "iem-ypr": [build_osc("/SceneRotator/ypr", [yaw, -pitch, roll])],
            "iem-quat": [build_osc("/SceneRotator/quaternions", [w, x, y, z])],
            "android": [build_osc("/rotationvector", [x, y, z, w])],
        }[fmt]
        for p in packets:
            sock.sendto(p, ("127.0.0.1", port))
        await asyncio.sleep(1 / 60)


async def report():
    while True:
        await asyncio.sleep(5)
        if stats["count"]:
            print(f"{stats['count']} messages in 5 s:", ", ".join(sorted(stats["addresses"])), flush=True)
        stats["count"], stats["addresses"] = 0, {}


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--osc-port", type=int, default=9000)
    ap.add_argument("--ws-port", type=int, default=8766)
    ap.add_argument("--network", action="store_true", help="listen for OSC on every interface, not only this computer")
    ap.add_argument("--demo", choices=["adm", "sparta-ypr", "sparta-quat", "iem-ypr", "iem-quat", "android"])
    a = ap.parse_args()
    host = "0.0.0.0" if a.network else "127.0.0.1"
    loop = asyncio.get_running_loop()
    try:
        await loop.create_datagram_endpoint(OscIn, local_addr=(host, a.osc_port))
    except OSError:
        raise SystemExit(f"Port {a.osc_port} is taken — another program (REAPER, a tracker app, a plug-in) is "
                         f"listening on it. Choose another with --osc-port and send there.")
    server = await asyncio.start_server(ws_client, "127.0.0.1", a.ws_port)
    if a.network:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("10.255.255.255", 1)); ip = s.getsockname()[0]
        except OSError:
            ip = "this computer's address"
        print(f"OSC on every interface, port {a.osc_port} — send from a phone to {ip}:{a.osc_port}")
    else:
        print(f"OSC on 127.0.0.1:{a.osc_port} (this computer only; --network for a phone)")
    print(f"WebSocket on ws://127.0.0.1:{a.ws_port}", flush=True)
    tasks = [report()] + ([demo(a.demo, a.osc_port)] if a.demo else [])
    async with server:
        await asyncio.gather(server.serve_forever(), *tasks)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
