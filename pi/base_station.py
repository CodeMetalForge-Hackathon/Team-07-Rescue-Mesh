#!/usr/bin/env python3
"""
Team 7 base station - connects to a LoRa box over USB and serves a web app.

    python3 base_station.py              # auto-finds the Feather over USB
    python3 base_station.py --port /dev/ttyACM0
    python3 base_station.py --sim        # no hardware: simulated 3-box network

Then open  http://<this-computer>:8000  (on the Pi hotspot: http://10.42.0.1:8000)

Only needs Python 3 + pyserial (sudo apt install python3-serial). See PROTOCOL.md
for the serial lines this understands.
"""
import argparse
import json
import math
import os
import queue
import random
import threading
import time
import unicodedata
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
MAX_TEXT = 60              # same limit as the boxes (proto.h)
LINK_MAX_AGE_S = 90        # links older than this are ignored by the map
STALE_S = 30               # node not heard for this long -> stale
LOST_S = 90                # ... -> lost


# =====================================================================  state
class State:
    def __init__(self):
        self.lock = threading.RLock()
        self.version = 0
        self.connected = False
        self.port = ""
        self.my_id = None
        self.nodes = {}          # id -> dict
        self.links = {}          # (a, b) sorted -> {"rssi": {a->b: v}, "t": time}
        self.messages = []       # chat log
        self.alerts = []         # quick-message alerts (HELP etc.)
        self.log = []            # raw lines (last 200)
        self.pending_out = []    # outgoing messages waiting for a SENT line
        self.calib = {"p0": -45.0, "n": 2.7}   # RSSI at 1 m, path-loss exponent
        self.positions = {}      # id -> [x, y] metres (base at 0,0)
        self.next_alert_id = 1
        self.listeners = []      # queues for live web clients
        logdir = os.path.join(HERE, "logs")
        os.makedirs(logdir, exist_ok=True)
        self.logfile = open(os.path.join(logdir, time.strftime("session-%Y%m%d-%H%M%S.jsonl")), "a")

    # ---------------------------------------------------------------- helpers
    def changed(self):
        self.version += 1
        for q in list(self.listeners):
            try:
                q.put_nowait(self.version)
            except queue.Full:
                pass

    def record(self, kind, **data):
        data.update(kind=kind, t=time.time())
        self.logfile.write(json.dumps(data) + "\n")
        self.logfile.flush()

    def node(self, nid):
        n = self.nodes.get(nid)
        if n is None:
            n = self.nodes[nid] = {"id": nid, "role": "?", "chain": None, "temp": None,
                                   "rh": None, "flag": None, "rssi": None, "snr": None,
                                   "via": None, "last": 0, "link": None}
        return n

    def heard(self, nid, rssi=None, snr=None, via=None):
        n = self.node(nid)
        n["last"] = time.time()
        if via is not None:
            n["via"] = via
        if rssi is not None and via == nid:      # only direct packets describe our own link
            n["rssi"], n["snr"] = rssi, snr
        return n

    def add_link(self, frm, to, rssi):
        """frm was heard by `to` at `rssi` dBm."""
        if frm == to or frm is None or to is None:
            return
        key = tuple(sorted((frm, to)))
        L = self.links.setdefault(key, {"rssi": {}, "t": 0})
        d = f"{frm}>{to}"
        old = L["rssi"].get(d)
        L["rssi"][d] = rssi if old is None else 0.6 * old + 0.4 * rssi   # smooth
        L["t"] = time.time()

    # ------------------------------------------------------------------ parser
    def handle_line(self, line):
        line = line.strip()
        if not line:
            return
        with self.lock:
            self.log.append({"t": time.time(), "line": line})
            del self.log[:-200]
            try:
                self._parse(line)
            except (ValueError, IndexError) as e:
                self.log.append({"t": time.time(), "line": f"[parse error: {e}] {line}"})
            self.changed()

    def _parse(self, line):
        tag = line.split(",", 1)[0]
        now = time.time()
        if tag == "ME":
            p = line.split(",")
            nid, role, chain = p[1], p[2], int(p[3])
            self.my_id = nid
            n = self.node(nid)
            n.update(role=role, chain=chain, last=now, me=True)
            if len(p) >= 7:
                n.update(temp=float(p[4]), rh=float(p[5]), flag=p[6])
        elif tag == "DATA":
            p = line.split(",")
            src, seq, t, h, flag, rssi, snr, via, heard_by = p[1:10]
            n = self.heard(src, int(rssi), float(snr), via)
            n.update(temp=float(t), rh=float(h), flag=flag, seq=int(seq))
            self.add_link(via, heard_by, int(rssi))
            self.record("data", src=src, temp=float(t), rh=float(h), rssi=int(rssi), via=via)
        elif tag == "BCN":
            _, src, role, chain, rssi, snr = line.split(",")[:6]
            n = self.heard(src, int(rssi), float(snr), src)
            n.update(role=role, chain=int(chain))
            if self.my_id:
                self.add_link(src, self.my_id, int(rssi))
        elif tag == "LINK":
            _, tail, margin, st = line.split(",")[:4]
            if self.my_id:
                self.node(self.my_id)["link"] = {"tail": tail, "margin": float(margin), "state": st}
        elif tag == "NBR":
            _, src, role, chain, pairs = (line.split(",", 4) + [""])[:5]
            n = self.heard(src)
            n.update(role=role, chain=int(chain))
            for item in filter(None, pairs.split(";")):
                other, rssi = item.split(":")
                self.add_link(other, src, int(rssi))
                self.node(other)
        elif tag == "MSG":
            p = line.split(",", 7)
            src, seq, dst, rssi, snr, via, text = p[1], int(p[2]), p[3], int(p[4]), float(p[5]), p[6], p[7]
            self.heard(src, rssi, snr, via)
            self.add_link(via, self.my_id, rssi)
            self.messages.append({"t": now, "src": src, "dst": dst, "text": text, "dir": "in", "seq": seq})
            del self.messages[:-300]
            self.record("msg_in", src=src, dst=dst, text=text)
        elif tag == "QM":
            _, src, seq, dst, code, rssi, snr, via = line.split(",")[:8]
            self.heard(src, int(rssi), float(snr), via)
            self.add_link(via, self.my_id, int(rssi))
            self.node(src)["quick"] = {"code": code, "t": now}
            self.messages.append({"t": now, "src": src, "dst": dst, "text": f"[{code}]",
                                  "dir": "in", "seq": int(seq), "quick": code})
            if code in ("HELP", "FOUND"):
                self.alerts.append({"id": self.next_alert_id, "t": now, "src": src,
                                    "seq": int(seq), "code": code, "acked": False})
                self.next_alert_id += 1
            self.record("quick", src=src, code=code)
        elif tag == "ACK":
            _, src, acked = line.split(",")[:3]
            for m in self.messages:
                if m["dir"] == "out" and m.get("seq") == int(acked) and m.get("status") != "delivered":
                    m["status"] = "delivered"
                    m["by"] = src
                    break
        elif tag == "SENT":
            p = line.split(",", 4)
            seq = int(p[1])
            if self.pending_out:
                m = self.pending_out.pop(0)
                m["seq"], m["status"] = seq, "sent"

    # ------------------------------------------------------------------ outbound
    def queue_out(self, dst, text, kind="text"):
        with self.lock:
            m = {"t": time.time(), "src": self.my_id or "me", "dst": dst, "text": text,
                 "dir": "out", "status": "queued", "kind": kind}
            self.messages.append(m)
            self.pending_out.append(m)
            self.changed()
            self.record("msg_out", dst=dst, text=text)

    # ------------------------------------------------------------------ minimap
    def distance_m(self, rssi):
        c = self.calib
        return 10 ** ((c["p0"] - rssi) / (10 * c["n"]))

    def solve_map(self):
        """Place nodes so pairwise distances match the RSSI-estimated ones.
        Base (or our own box) is pinned at (0,0). Gradient descent on stress,
        starting from the previous solution so the map doesn't flip around."""
        now = time.time()
        with self.lock:
            ids = sorted(self.nodes)
            dist = {}
            for (a, b), L in self.links.items():
                if now - L["t"] > LINK_MAX_AGE_S or a not in self.nodes or b not in self.nodes:
                    continue
                vals = list(L["rssi"].values())
                dist[(a, b)] = self.distance_m(sum(vals) / len(vals))
            anchor = next((i for i in ids if self.nodes[i]["role"] == "B"), self.my_id or (ids[0] if ids else None))
            pos = {i: list(self.positions.get(i, [random.uniform(-5, 5), random.uniform(-5, 5)])) for i in ids}
        if not ids or anchor is None:
            return
        pos[anchor] = [0.0, 0.0]
        # nodes we have no distance for at all stay where they were
        linked = {x for k in dist for x in k}
        for (a, b), d in dist.items():       # first-time nodes: start near their neighbour
            for u, v in ((a, b), (b, a)):
                if u not in self.positions and v in pos and u != anchor:
                    ang = random.uniform(0, 2 * math.pi)
                    pos[u] = [pos[v][0] + d * math.cos(ang), pos[v][1] + d * math.sin(ang)]
        scale = max(dist.values()) if dist else 1.0

        def descend(pos):
            for it in range(400):
                lr = 0.05 * (1 - it / 400) + 0.005
                grad = {i: [0.0, 0.0] for i in ids}
                for (a, b), d in dist.items():
                    dx, dy = pos[a][0] - pos[b][0], pos[a][1] - pos[b][1]
                    cur = math.hypot(dx, dy) or 1e-6
                    g = (cur - d) / cur / max(d, 1.0)       # relative error: fair to short & long links
                    grad[a][0] += g * dx; grad[a][1] += g * dy
                    grad[b][0] -= g * dx; grad[b][1] -= g * dy
                # unlinked pairs can't hear each other, so they shouldn't sit on top of each other
                for x in range(len(ids)):
                    for y in range(x + 1, len(ids)):
                        a, b = ids[x], ids[y]
                        if (a, b) in dist or a not in linked or b not in linked:
                            continue
                        dx, dy = pos[a][0] - pos[b][0], pos[a][1] - pos[b][1]
                        cur = math.hypot(dx, dy) or 1e-6
                        if cur < scale:
                            g = (cur - scale) / cur / scale * 0.3
                            grad[a][0] += g * dx; grad[a][1] += g * dy
                            grad[b][0] -= g * dx; grad[b][1] -= g * dy
                max_step = 0.1 * scale                  # never move a box more than 10% of the map per step
                for i in ids:
                    if i == anchor or i not in linked:
                        continue
                    sx, sy = lr * scale * grad[i][0], lr * scale * grad[i][1]
                    m = math.hypot(sx, sy)
                    if m > max_step:
                        sx, sy = sx * max_step / m, sy * max_step / m
                    pos[i][0] -= sx
                    pos[i][1] -= sy
            return pos

        def stress(pos):
            return sum(((math.hypot(pos[a][0] - pos[b][0], pos[a][1] - pos[b][1]) - d) / max(d, 1.0)) ** 2
                       for (a, b), d in dist.items())

        # A straight line is a "flat spot" for this kind of solver: once every box is
        # collinear, the steps can only slide them along the line. So start from several
        # positions (previous map nudged sideways + random layouts) and keep the best fit.
        starts = []
        for k in range(4):
            jit = 0.08 * scale if k == 0 else scale
            base = pos if k == 0 else {i: [random.uniform(-1, 1) * scale, random.uniform(-1, 1) * scale] for i in ids}
            starts.append({i: ([0.0, 0.0] if i == anchor else
                               [base[i][0] + random.gauss(0, jit * (0.3 if k == 0 else 0.1)),
                                base[i][1] + random.gauss(0, jit * (0.3 if k == 0 else 0.1))])
                           for i in ids})
        results = [descend(st) for st in starts]
        results = [r for r in results if all(math.isfinite(v) for p in r.values() for v in p)]
        if not results:
            return
        pos = min(results, key=stress)
        # Distances can't tell rotation or mirror-image apart, so fix them:
        # rotate so the first relay (chain 1, else any linked node) points "up" (forward),
        # then pick the mirror image closest to the previous map so it doesn't flip.
        ref = next((i for i in ids if self.nodes[i].get("chain") == 1 and i in linked and i != anchor),
                   next((i for i in ids if i in linked and i != anchor), None))
        if ref is not None and math.hypot(*pos[ref]) > 1e-6:
            ang = math.atan2(pos[ref][0], pos[ref][1])          # angle from +y axis
            c, s = math.cos(ang), math.sin(ang)
            pos = {i: [c * p[0] - s * p[1], s * p[0] + c * p[1]] for i, p in pos.items()}
            mirror = {i: [-p[0], p[1]] for i, p in pos.items()}
            prev = self.positions
            def err(P):
                return sum((P[i][0] - prev[i][0]) ** 2 + (P[i][1] - prev[i][1]) ** 2 for i in P if i in prev)
            if prev and err(mirror) < err(pos):
                pos = mirror
        # how well-determined is each node? (number of distance constraints)
        degree = {i: 0 for i in ids}
        for (a, b) in dist:
            degree[a] += 1; degree[b] += 1
        with self.lock:
            self.positions = {i: [round(p[0], 2), round(p[1], 2)] for i, p in pos.items()}
            self.map_info = {"dist": {f"{a}|{b}": round(d, 1) for (a, b), d in dist.items()},
                             "degree": degree, "anchor": anchor}

    # ------------------------------------------------------------------ snapshot
    def snapshot(self):
        now = time.time()
        with self.lock:
            nodes = []
            for n in self.nodes.values():
                d = dict(n)
                age = now - n["last"] if n["last"] else None
                d["age"] = None if age is None else round(age, 1)
                d["status"] = "lost" if age is None or age > LOST_S else ("stale" if age > STALE_S else "ok")
                nodes.append(d)
            links = []
            for (a, b), L in self.links.items():
                if now - L["t"] > LINK_MAX_AGE_S:
                    continue
                vals = list(L["rssi"].values())
                links.append({"a": a, "b": b, "rssi": round(sum(vals) / len(vals), 1),
                              "age": round(now - L["t"], 1)})
            return {
                "version": self.version, "now": now, "connected": self.connected, "port": self.port,
                "me": self.my_id, "nodes": nodes, "links": links,
                "messages": self.messages[-150:], "alerts": [a for a in self.alerts if not a["acked"]],
                "log": self.log[-60:], "calib": self.calib, "positions": self.positions,
                "map": getattr(self, "map_info", {}),
                "sim_truth": getattr(self, "sim_truth", None),
            }


STATE = State()


def clean_text(text):
    """Phones send emoji/accents - LoRa packets are plain ASCII."""
    text = unicodedata.normalize("NFKD", str(text)).encode("ascii", "ignore").decode()
    text = "".join(ch for ch in text if 32 <= ord(ch) < 127)
    return text.strip()[:MAX_TEXT]


def clean_id(x):
    x = "".join(ch for ch in str(x) if ch.isalnum() or ch in "_-*")
    return x[:8] or "*"


# =====================================================================  serial link
class SerialLink:
    """Talks to the real box; reconnects automatically if USB drops."""

    def __init__(self, port=None):
        self.port_arg = port
        self.ser = None
        self.wlock = threading.Lock()

    def find_port(self):
        from serial.tools import list_ports
        if self.port_arg:
            return self.port_arg
        for p in list_ports.comports():
            if p.vid == 0x239A:           # Adafruit
                return p.device
        return None

    def run(self):
        import serial
        while True:
            port = self.find_port()
            if not port:
                time.sleep(2)
                continue
            try:
                self.ser = serial.Serial(port, 115200, timeout=1)
                with STATE.lock:
                    STATE.connected, STATE.port = True, port
                    STATE.changed()
                print("Connected to", port)
                while True:
                    raw = self.ser.readline()
                    if raw:
                        STATE.handle_line(raw.decode("utf-8", "replace"))
            except Exception as e:                       # unplugged, reset, ...
                print("Serial problem:", e, "- retrying")
                with STATE.lock:
                    STATE.connected = False
                    STATE.changed()
                try:
                    self.ser and self.ser.close()
                except Exception:
                    pass
                self.ser = None
                time.sleep(2)

    def send(self, line):
        with self.wlock:
            if self.ser is None:
                return False
            try:
                self.ser.write((line + "\n").encode("ascii"))
                return True
            except Exception:
                return False


class SimLink:
    """Pretends to be the base box in a 3-box chain, for testing without hardware.
    BASE at (0,0), relay R1 ~25 m away, carried C1 walking out past R1 and back."""

    def __init__(self):
        self.t0 = time.time()
        self.seq = 100
        self.out = queue.Queue()
        self.p0, self.n = -45.0, 2.7
        self.true = {"BASE": (0.0, 0.0), "R1": (24.0, 6.0), "C1": (24.0, 6.0)}
        self.temps = {"BASE": 22.0, "R1": 19.5, "C1": 17.0}

    def rssi(self, a, b):
        (x1, y1), (x2, y2) = self.true[a], self.true[b]
        d = max(0.5, math.hypot(x1 - x2, y1 - y2))
        return int(round(self.p0 - 10 * self.n * math.log10(d) + random.gauss(0, 2.0)))

    def hears(self, a, b):
        return self.rssi(a, b) > -95

    def run(self):
        STATE.connected, STATE.port = True, "SIMULATION"
        STATE.handle_line(f"ME,BASE,B,0,{self.temps['BASE']:.2f},45.00,R")
        STATE.handle_line("SHT41 found, serial 0x51A51A51 (simulated)")
        tick = 0
        while True:
            time.sleep(1.0)
            tick += 1
            t = time.time() - self.t0
            # C1 walks from R1 down a "side corridor" (a 90-degree turn) and back, period 120 s
            phase = (math.sin(2 * math.pi * t / 120 - math.pi / 2) + 1) / 2
            self.true["C1"] = (24 - 10 * phase, 6 + 40 * phase)
            STATE.sim_truth = {k: list(v) for k, v in self.true.items()}
            for k in self.temps:
                self.temps[k] += random.uniform(-0.05, 0.05)
            # outbound commands from the web app
            while not self.out.empty():
                line = self.out.get()
                self.seq += 1
                parts = line.split(",", 2)
                STATE.handle_line(f"SENT,{self.seq},{parts[1]},{parts[0]},{parts[2] if len(parts) > 2 else ''}")
                if parts[0] == "TX":
                    s = self.seq
                    threading.Timer(2.5, lambda s=s: STATE.handle_line(f"ACK,C1,{s}")).start()
                    threading.Timer(4.0, lambda: self.inbound_msg("C1", "Copy that, base.")).start()
            if tick % 2 == 0:   # beacons from the deployed relay
                if self.hears("R1", "BASE"):
                    STATE.handle_line(f"BCN,R1,D,1,{self.rssi('R1', 'BASE')},{random.uniform(5, 10):.2f}")
            if tick % 5 == 0:
                STATE.handle_line(f"ME,BASE,B,0,{self.temps['BASE']:.2f},45.00,R")
            if tick % 5 == 1:   # sensor readings
                self.seq += 1
                STATE.handle_line(f"DATA,R1,{self.seq},{self.temps['R1']:.2f},48.20,R,{self.rssi('R1','BASE')},8.50,R1,BASE")
                self.seq += 1
                if self.hears("C1", "BASE"):
                    STATE.handle_line(f"DATA,C1,{self.seq},{self.temps['C1']:.2f},61.00,R,{self.rssi('C1','BASE')},6.00,C1,BASE")
                elif self.hears("C1", "R1"):
                    STATE.handle_line(f"DATA,C1,{self.seq},{self.temps['C1']:.2f},61.00,R,{self.rssi('R1','BASE')},8.00,R1,BASE")
            if tick % 10 == 3:  # neighbour reports
                roles = {"R1": "D,1", "C1": "C,-1"}
                for me in ("R1", "C1"):
                    pairs = [f"{o}:{self.rssi(o, me)}" for o in self.true if o != me and self.hears(o, me)]
                    if pairs and (self.hears(me, "BASE") or self.hears("R1", "BASE")):
                        STATE.handle_line(f"NBR,{me},{roles[me]},{';'.join(pairs)}")
                pairs = [f"{o}:{self.rssi(o, 'BASE')}" for o in ("R1", "C1") if self.hears(o, "BASE")]
                STATE.handle_line(f"NBR,BASE,B,0,{';'.join(pairs)}")
            if tick == 25:
                self.seq += 1
                STATE.handle_line(f"QM,C1,{self.seq},*,HELP,{self.rssi('R1','BASE')},7.25,R1")
            if tick % 30 == 12:
                self.inbound_msg("C1", random.choice(["Reached the stairwell, going down.",
                                                      "Smoke on level 2, humidity dropping",
                                                      "Dropping relay box now"]))

    def inbound_msg(self, src, text):
        self.seq += 1
        STATE.handle_line(f"MSG,{src},{self.seq},*,{self.rssi('R1','BASE')},7.00,R1,{text}")

    def send(self, line):
        self.out.put(line)
        return True


# =====================================================================  web server
class Handler(BaseHTTPRequestHandler):
    link = None
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):      # keep the console quiet
        pass

    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            with open(os.path.join(HERE, "static", "index.html"), "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/api/state":
            self.send_json(STATE.snapshot())
        elif self.path == "/events":
            self.stream_events()
        else:
            self.send_json({"error": "not found"}, 404)

    def stream_events(self):
        """Server-Sent Events: push a fresh snapshot whenever something changes."""
        q = queue.Queue(maxsize=5)
        STATE.listeners.append(q)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        try:
            last = -1
            while True:
                try:
                    q.get(timeout=2)
                    time.sleep(0.15)             # batch bursts of lines
                    while not q.empty():
                        q.get_nowait()
                except queue.Empty:
                    pass                         # periodic refresh keeps ages ticking
                snap = STATE.snapshot()
                if snap["version"] == last and int(time.time()) % 2:
                    self.wfile.write(b": keepalive\n\n")
                else:
                    self.wfile.write(b"data: " + json.dumps(snap).encode() + b"\n\n")
                    last = snap["version"]
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            STATE.listeners.remove(q)

    def do_POST(self):
        try:
            n = int(self.headers.get("Content-Length", 0))
            data = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return self.send_json({"error": "bad json"}, 400)
        if self.path == "/api/send":
            text, dst = clean_text(data.get("text", "")), clean_id(data.get("dst", "*"))
            if not text:
                return self.send_json({"error": "empty message"}, 400)
            STATE.queue_out(dst, text)
            ok = self.link.send(f"TX,{dst},{text}")
            return self.send_json({"ok": ok, "text": text})
        if self.path == "/api/quick":
            code = clean_id(data.get("code", "")).upper()
            dst = clean_id(data.get("dst", "*"))
            if code not in ("OK", "HELP", "FOUND", "RETURN"):
                return self.send_json({"error": "unknown code"}, 400)
            STATE.queue_out(dst, f"[{code}]", kind="quick")
            return self.send_json({"ok": self.link.send(f"QM,{dst},{code}")})
        if self.path == "/api/ack":
            with STATE.lock:
                for a in STATE.alerts:
                    if a["id"] == data.get("id"):
                        a["acked"] = True
                        self.link.send(f"AK,{a['src']},{a['seq']}")
                STATE.changed()
            return self.send_json({"ok": True})
        if self.path == "/api/calibrate":
            # "boxes a and b are `meters` apart right now" -> fit P0 from the current link
            a, b, meters = clean_id(data.get("a")), clean_id(data.get("b")), float(data.get("meters", 1))
            with STATE.lock:
                L = STATE.links.get(tuple(sorted((a, b))))
                if not L or not L["rssi"]:
                    return self.send_json({"error": f"no link between {a} and {b} yet"}, 400)
                vals = list(L["rssi"].values())
                rssi = sum(vals) / len(vals)
                STATE.calib["p0"] = round(rssi + 10 * STATE.calib["n"] * math.log10(max(meters, 0.1)), 1)
                STATE.changed()
            return self.send_json({"ok": True, "calib": STATE.calib})
        return self.send_json({"error": "not found"}, 404)


def map_loop():
    while True:
        try:
            STATE.solve_map()
            with STATE.lock:
                STATE.changed()
        except Exception as e:
            print("map error:", e)
        time.sleep(2)


def main():
    ap = argparse.ArgumentParser(description="Team 7 LoRa base station")
    ap.add_argument("--port", help="serial port (default: auto-detect Adafruit board)")
    ap.add_argument("--sim", action="store_true", help="simulate a 3-box network, no hardware")
    ap.add_argument("--http", type=int, default=8000, help="web port (default 8000)")
    args = ap.parse_args()

    link = SimLink() if args.sim else SerialLink(args.port)
    Handler.link = link
    threading.Thread(target=link.run, daemon=True).start()
    threading.Thread(target=map_loop, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", args.http), Handler)
    srv.daemon_threads = True
    print(f"Team 7 base station on http://0.0.0.0:{args.http}  ({'SIMULATION' if args.sim else 'serial'})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
