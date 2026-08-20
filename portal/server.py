#!/usr/bin/env python3
"""
NEXUS DISPLAY CONTROL — control portal for the ESP32 CYD sci-fi display.

Zero dependencies (Python 3.8+ stdlib only).

Run:    python3 server.py [--port 8484] [--host 0.0.0.0]
Portal: http://<server-ip>:8484
API:    GET  /api/state   -> current state JSON (the ESP32 polls this)
        POST /api/state   -> {"anim": "...", "speed": 1-10, "brightness": 10-100}
        GET  /api/status  -> device online status
"""

import argparse
import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
STATE_FILE = os.path.join(HERE, "state.json")
MEDIA_DIR = os.path.join(HERE, "media")        # saved images / GIF packs
MEDIA_META = os.path.join(HERE, "media.json")
LEGACY_IMAGE = os.path.join(HERE, "image.bin") # pre-library single image

# Device media format ("NX01", 12-byte header, then RGB565-LE frames):
#   magic[4] kind:u8 (0 still, 1 gif) frames:u8 delay_ms:u16 w:u16 h:u16
# still: 1 frame 320x240; gif: up to 20 frames 160x120
MAGIC = b"NX01"
HDR = 12
MAX_MEDIA_BYTES = HDR + 24 * 160 * 120 * 2

ANIMATIONS = [
    {"id": "matrix", "name": "Digital Rain",   "desc": "Katakana code cascade",        "color": "#00ff9c"},
    {"id": "warp",   "name": "Starfield Warp", "desc": "Hyperspace star tunnel",       "color": "#7ab8ff"},
    {"id": "hud",    "name": "Neon HUD",       "desc": "Rotating rings + telemetry",   "color": "#00e5ff"},
    {"id": "plasma", "name": "Plasma Flux",    "desc": "Liquid energy field",          "color": "#c86bff"},
    {"id": "grid",   "name": "Synthwave Grid", "desc": "Retro horizon + neon sun",     "color": "#ff4fd8"},
    {"id": "radar",  "name": "Tactical Radar", "desc": "Sweep scan with contacts",     "color": "#3dff6e"},
    {"id": "core",   "name": "Reactor Core",   "desc": "Pulsing arc reactor",          "color": "#ffb347"},
    {"id": "scope",  "name": "Ghost Signal",   "desc": "Glitch waveform oscilloscope", "color": "#ff5c5c"},
    {"id": "tunnel", "name": "Ion Tunnel",     "desc": "Hex tunnel at lightspeed",     "color": "#00ffd0"},
    {"id": "orbit",  "name": "Low Orbit",      "desc": "Planet dusk from orbit",       "color": "#4aa8ff"},
    {"id": "rain",   "name": "Lofi Rain",      "desc": "Rainy night city + cat",       "color": "#6f9bd1", "group": "chill"},
    {"id": "sakura", "name": "Sakura Night",   "desc": "Petals drifting past the moon","color": "#ffb7d5", "group": "chill"},
    {"id": "fire",   "name": "Firefly Meadow", "desc": "Glowing fireflies at dusk",    "color": "#d7ff7a", "group": "chill"},
    {"id": "koi",    "name": "Koi Pond",       "desc": "Koi gliding under lilies",     "color": "#ff9c54", "group": "chill"},
    {"id": "train",  "name": "Train Window",   "desc": "Dusk fields rolling past",     "color": "#d98c4a", "group": "chill"},
    {"id": "snow",   "name": "First Snow",     "desc": "Snowy night city + cat",       "color": "#cfe4ff", "group": "chill"},
    {"id": "custom", "name": "Custom Image",   "desc": "Your uploaded picture",        "color": "#ffffff", "group": "custom"},
]
ANIM_IDS = {a["id"] for a in ANIMATIONS}

_lock = threading.Lock()
_state = {"anim": "hud", "speed": 5, "brightness": 90, "rev": 1, "img_rev": 0, "img": None}
_device = {"last_seen": 0.0, "ip": None}
_media = {"next_id": 1, "items": []}   # items: [{id, name, kind, frames, w, h}]


def load_state():
    global _state
    try:
        with open(STATE_FILE) as f:
            saved = json.load(f)
        if saved.get("anim") in ANIM_IDS:
            _state.update({k: saved[k] for k in
                           ("anim", "speed", "brightness", "rev", "img_rev", "img") if k in saved})
    except (OSError, ValueError):
        pass
    load_media()


def save_state():
    try:
        with open(STATE_FILE, "w") as f:
            json.dump(_state, f)
    except OSError:
        pass


def media_path(mid):
    return os.path.join(MEDIA_DIR, f"{int(mid)}.bin")


def save_media():
    with open(MEDIA_META, "w") as f:
        json.dump(_media, f)


def load_media():
    global _media
    os.makedirs(MEDIA_DIR, exist_ok=True)
    try:
        with open(MEDIA_META) as f:
            _media = json.load(f)
    except (OSError, ValueError):
        pass
    # import the pre-library single image, if one was uploaded
    if not _media["items"] and os.path.exists(LEGACY_IMAGE):
        try:
            with open(LEGACY_IMAGE, "rb") as f:
                pixels = f.read()
            if len(pixels) == 320 * 240 * 2:
                hdr = MAGIC + bytes([0, 1]) + (0).to_bytes(2, "little") \
                    + (320).to_bytes(2, "little") + (240).to_bytes(2, "little")
                mid = _media["next_id"]
                with open(media_path(mid), "wb") as f:
                    f.write(hdr + pixels)
                _media["items"].append({"id": mid, "name": "imported", "kind": 0,
                                        "frames": 1, "w": 320, "h": 240})
                _media["next_id"] = mid + 1
                _state["img"] = mid
                save_media()
            os.remove(LEGACY_IMAGE)
        except OSError:
            pass


class Handler(BaseHTTPRequestHandler):
    server_version = "NexusDisplay/1.0"

    def log_message(self, fmt, *args):
        # Keep the console quiet for the device's 2-second polling.
        if "/api/state" not in (args[0] if args else ""):
            super().log_message(fmt, *args)

    def _send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype + "; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/" or path == "/index.html":
            self._send(200, PAGE, "text/html")
        elif path == "/api/state":
            if "device=" in self.path:
                with _lock:
                    _device["last_seen"] = time.time()
                    _device["ip"] = self.client_address[0]
            with _lock:
                self._send(200, json.dumps(_state))
        elif path == "/api/image.bin":       # device: currently selected media
            with _lock:
                mid = _state["img"]
            try:
                with open(media_path(mid), "rb") as f:
                    self._send(200, f.read(), "application/octet-stream")
            except (OSError, TypeError):
                self._send(404, '{"error":"no media selected"}')
        elif path == "/api/media":
            with _lock:
                self._send(200, json.dumps({"items": _media["items"], "current": _state["img"]}))
        elif path.startswith("/api/media/") and path.endswith(".bin"):
            try:
                mid = int(path[len("/api/media/"):-4])
                with open(media_path(mid), "rb") as f:
                    self._send(200, f.read(), "application/octet-stream")
            except (ValueError, OSError):
                self._send(404, '{"error":"not found"}')
        elif path == "/api/status":
            with _lock:
                age = time.time() - _device["last_seen"]
                online = _device["last_seen"] > 0 and age < 10
                self._send(200, json.dumps({
                    "online": online,
                    "ip": _device["ip"],
                    "last_seen_secs": None if _device["last_seen"] == 0 else round(age, 1),
                    "state": _state,
                    "animations": ANIMATIONS,
                }))
        else:
            self._send(404, '{"error":"not found"}')

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        if path == "/api/media":
            self._recv_media()
            return
        if path in ("/api/media/select", "/api/media/delete"):
            self._media_op(path.endswith("delete"))
            return
        if path != "/api/state":
            self._send(404, '{"error":"not found"}')
            return
        try:
            length = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            self._send(400, '{"error":"bad json"}')
            return
        with _lock:
            changed = False
            if "anim" in body and body["anim"] in ANIM_IDS and body["anim"] != _state["anim"]:
                _state["anim"] = body["anim"]
                changed = True
            if "speed" in body:
                v = max(1, min(10, int(body["speed"])))
                changed |= v != _state["speed"]
                _state["speed"] = v
            if "brightness" in body:
                v = max(10, min(100, int(body["brightness"])))
                changed |= v != _state["brightness"]
                _state["brightness"] = v
            if changed:
                _state["rev"] += 1
                save_state()
            self._send(200, json.dumps(_state))

    def _recv_media(self):
        # body: complete device-format binary, built by the browser (see MAGIC)
        from urllib.parse import parse_qs, urlparse, unquote
        length = int(self.headers.get("Content-Length", 0))
        if not HDR < length <= MAX_MEDIA_BYTES:
            self._send(400, json.dumps({"error": f"bad size {length}"}))
            return
        raw = b""
        while len(raw) < length:
            chunk = self.rfile.read(length - len(raw))
            if not chunk:
                self._send(400, '{"error":"truncated body"}')
                return
            raw += chunk
        if raw[:4] != MAGIC:
            self._send(400, '{"error":"bad magic"}')
            return
        kind, frames = raw[4], raw[5]
        w = int.from_bytes(raw[8:10], "little")
        h = int.from_bytes(raw[10:12], "little")
        if frames < 1 or len(raw) != HDR + frames * w * h * 2 \
           or (kind == 0 and (w, h, frames) != (320, 240, 1)) \
           or (kind == 1 and (w, h) != (160, 120)):
            self._send(400, '{"error":"header/body mismatch"}')
            return
        q = parse_qs(urlparse(self.path).query)
        name = unquote(q.get("name", ["untitled"])[0])[:40]
        name = "".join(c for c in name if c.isprintable() and c not in "<>&\"'") or "untitled"
        with _lock:
            mid = _media["next_id"]
            _media["next_id"] = mid + 1
            with open(media_path(mid), "wb") as f:
                f.write(raw)
            _media["items"].append({"id": mid, "name": name, "kind": kind,
                                    "frames": frames, "w": w, "h": h})
            save_media()
            _state["img"] = mid
            _state["anim"] = "custom"
            _state["img_rev"] += 1
            _state["rev"] += 1
            save_state()
            self._send(200, json.dumps(_state))

    def _media_op(self, is_delete):
        try:
            length = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(length) or b"{}")
            mid = int(body["id"])
        except (ValueError, KeyError):
            self._send(400, '{"error":"bad json"}')
            return
        with _lock:
            if not any(it["id"] == mid for it in _media["items"]):
                self._send(404, '{"error":"no such media"}')
                return
            if is_delete:
                _media["items"] = [it for it in _media["items"] if it["id"] != mid]
                save_media()
                try:
                    os.remove(media_path(mid))
                except OSError:
                    pass
                if _state["img"] == mid:
                    _state["img"] = _media["items"][-1]["id"] if _media["items"] else None
                    _state["img_rev"] += 1
                    _state["rev"] += 1
            else:  # select
                _state["img"] = mid
                _state["anim"] = "custom"
                _state["img_rev"] += 1
                _state["rev"] += 1
            save_state()
            self._send(200, json.dumps(_state))


PAGE = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>NEXUS // Display Control</title>
<style>
  :root {
    --bg: #06080f; --panel: #0b101c; --edge: #16223a;
    --cyan: #00e5ff; --mag: #ff4fd8; --txt: #c9d8e8; --dim: #5a6b84;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg); color: var(--txt); min-height: 100vh;
    font-family: "SF Mono", ui-monospace, Menlo, Consolas, monospace;
    background-image:
      radial-gradient(ellipse 80% 50% at 50% -10%, rgba(0,229,255,.10), transparent),
      repeating-linear-gradient(0deg, transparent 0 2px, rgba(255,255,255,.012) 2px 4px);
  }
  .wrap { max-width: 900px; margin: 0 auto; padding: 28px 20px 60px; }
  header { display: flex; align-items: baseline; justify-content: space-between;
           flex-wrap: wrap; gap: 10px; border-bottom: 1px solid var(--edge);
           padding-bottom: 14px; margin-bottom: 24px; }
  h1 { font-size: 20px; letter-spacing: .35em; color: #fff; font-weight: 600; }
  h1 span { color: var(--cyan); text-shadow: 0 0 12px var(--cyan); }
  .status { font-size: 12px; letter-spacing: .15em; display: flex; align-items: center; gap: 8px; }
  .dot { width: 9px; height: 9px; border-radius: 50%; background: #555; transition: all .3s; }
  .dot.on  { background: #3dff6e; box-shadow: 0 0 10px #3dff6e; }
  .dot.off { background: #ff5c5c; box-shadow: 0 0 10px #ff5c5c66; }
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(190px, 1fr)); gap: 14px; }
  .card {
    position: relative; background: var(--panel); border: 1px solid var(--edge);
    border-radius: 10px; padding: 14px; cursor: pointer; overflow: hidden;
    transition: transform .15s, border-color .2s, box-shadow .2s; text-align: left;
    color: inherit; font: inherit; width: 100%;
  }
  .card:hover { transform: translateY(-2px); border-color: var(--c); }
  .card.active { border-color: var(--c); box-shadow: 0 0 18px color-mix(in srgb, var(--c) 35%, transparent),
                 inset 0 0 24px color-mix(in srgb, var(--c) 8%, transparent); }
  .card.active::after {
    content: "▶ RUNNING"; position: absolute; top: 10px; right: 10px; font-size: 9px;
    letter-spacing: .15em; color: var(--c); animation: blink 1.2s steps(2) infinite;
  }
  @keyframes blink { 50% { opacity: .25; } }
  .prev { height: 64px; border-radius: 6px; margin-bottom: 12px; position: relative; overflow: hidden;
          background: #04060b; border: 1px solid #101828; }
  .prev::before { content: ""; position: absolute; inset: 0; }
  .card .nm { font-size: 13px; color: #fff; letter-spacing: .12em; margin-bottom: 4px; }
  .card .ds { font-size: 11px; color: var(--dim); letter-spacing: .05em; }
  /* per-animation CSS previews */
  .p-matrix::before { background: repeating-linear-gradient(90deg, transparent 0 14px, rgba(0,255,156,.16) 14px 16px);
    animation: rain 1.1s linear infinite; }
  @keyframes rain { to { background-position-y: 64px; } }
  .p-warp::before { background:
    radial-gradient(1.5px 1.5px at 20% 30%, #fff, transparent),
    radial-gradient(2px 2px at 70% 60%, #9cf, transparent),
    radial-gradient(1.5px 1.5px at 45% 80%, #fff, transparent),
    radial-gradient(2px 2px at 85% 20%, #bdf, transparent);
    animation: zoom 1.4s ease-in infinite; }
  @keyframes zoom { to { transform: scale(2.4); opacity: 0; } }
  .p-hud::before { border: 2px solid rgba(0,229,255,.7); border-top-color: transparent; border-radius: 50%;
    inset: 10px 35%; animation: spin 1.6s linear infinite; }
  @keyframes spin { to { transform: rotate(360deg); } }
  .p-plasma::before { background: linear-gradient(115deg, #7b2ff7, #c86bff, #f107a3, #7b2ff7);
    background-size: 300% 300%; animation: flux 3s ease infinite; opacity: .7; filter: blur(6px); }
  @keyframes flux { 50% { background-position: 100% 100%; } }
  .p-grid::before { background:
    linear-gradient(transparent 55%, rgba(255,79,216,.5) 55.5%, transparent 56%),
    repeating-linear-gradient(90deg, rgba(255,79,216,.35) 0 1px, transparent 1px 18px),
    repeating-linear-gradient(0deg, rgba(255,79,216,.35) 0 1px, transparent 1px 9px);
    transform: perspective(60px) rotateX(50deg) translateY(12px); animation: scroll .8s linear infinite; }
  @keyframes scroll { to { background-position-y: 9px; } }
  .p-radar::before { background: conic-gradient(from 0deg, rgba(61,255,110,.8), transparent 70deg);
    border-radius: 50%; inset: 4px 32%; animation: spin 2s linear infinite; }
  .p-core::before { background: radial-gradient(circle, #fff 0 8%, #ffb347 20%, rgba(255,120,40,.25) 45%, transparent 70%);
    animation: pulse 1.3s ease-in-out infinite; }
  @keyframes pulse { 50% { transform: scale(1.35); opacity: .65; } }
  .p-scope::before { background:
    repeating-linear-gradient(90deg, transparent 0 6px, rgba(255,92,92,.8) 6px 7px, transparent 7px 30px);
    mask: linear-gradient(transparent 40%, #000 48% 52%, transparent 60%);
    animation: jitter .28s steps(3) infinite; }
  @keyframes jitter { 33% { transform: translate(-4px, 2px); } 66% { transform: translate(3px, -2px); } }
  .p-rain { background: linear-gradient(#0a1630 55%, #1a2033 55%) !important; }
  .p-rain::before { background:
    radial-gradient(2px 2px at 22% 68%, #ffd98a, transparent),
    radial-gradient(2px 2px at 38% 72%, #ffca6a, transparent),
    radial-gradient(2px 2px at 61% 65%, #ffd98a, transparent),
    radial-gradient(2px 2px at 79% 70%, #ffca6a, transparent),
    repeating-linear-gradient(100deg, transparent 0 11px, rgba(160,200,255,.28) 11px 12px);
    animation: rainfall .5s linear infinite; }
  @keyframes rainfall { to { background-position: 0 0, 0 0, 0 0, 0 0, -12px 64px; } }
  .p-sakura { background: linear-gradient(#141028, #241436) !important; }
  .p-sakura::before { background:
    radial-gradient(circle at 78% 28%, #fff3d6 0 9px, rgba(255,243,214,.25) 11px, transparent 16px),
    radial-gradient(3px 3px at 20% 20%, #ffb7d5, transparent),
    radial-gradient(3px 3px at 45% 55%, #ff9cc7, transparent),
    radial-gradient(3px 3px at 65% 35%, #ffb7d5, transparent);
    animation: drift 3s ease-in-out infinite; }
  @keyframes drift { 50% { background-position: 0 0, 8px 14px, -6px 10px, 5px 16px; } }
  .p-fire { background: linear-gradient(#0a1418 60%, #101c10 60%) !important; }
  .p-fire::before { background:
    radial-gradient(2.5px 2.5px at 25% 45%, #e4ff9c, transparent),
    radial-gradient(2px 2px at 55% 65%, #d7ff7a, transparent),
    radial-gradient(2.5px 2.5px at 75% 35%, #e4ff9c, transparent),
    radial-gradient(2px 2px at 40% 25%, #d7ff7a, transparent);
    animation: hover 2.4s ease-in-out infinite; }
  @keyframes hover { 33% { background-position: 4px -5px, -5px 4px, 3px 5px, -4px -3px; opacity: .55; }
                     66% { background-position: -3px 4px, 4px -6px, -5px -3px, 5px 4px; } }
  .p-tunnel::before { background:
    repeating-radial-gradient(circle at 50% 50%, rgba(0,255,208,.55) 0 1px, transparent 1px 11px);
    animation: rush 1.1s linear infinite; }
  @keyframes rush { to { transform: scale(1.55); opacity: .4; } }
  .p-orbit::before { background:
    radial-gradient(1.5px 1.5px at 20% 25%, #fff, transparent),
    radial-gradient(1.5px 1.5px at 60% 15%, #cde, transparent),
    radial-gradient(circle at 72% 150%, #1a4a7a 0 52%, #6ad0ff 53% 54%, transparent 56%);
    animation: rise 4s ease-in-out infinite; }
  @keyframes rise { 50% { background-position: 0 0, 0 0, 0 -4px; } }
  .p-koi { background: linear-gradient(#0e3a40, #17545a) !important; }
  .p-koi::before { background:
    radial-gradient(6px 3px at 30% 55%, #ff8c3a 0 60%, transparent),
    radial-gradient(5px 3px at 70% 30%, #f0ebe0 0 60%, transparent),
    radial-gradient(circle at 85% 75%, #14432a 0 9px, transparent 10px);
    animation: swim 3.2s ease-in-out infinite; }
  @keyframes swim { 50% { background-position: 10px 4px, -8px 5px, 0 0; } }
  .p-train { background: linear-gradient(#241436 30%, #ff8a4a 62%, #101a10 63%) !important; }
  .p-train::before { background:
    repeating-linear-gradient(90deg, rgba(10,10,12,.9) 0 2px, transparent 2px 34px);
    animation: pass .5s linear infinite; }
  @keyframes pass { to { background-position-x: -34px; } }
  .p-snow { background: linear-gradient(#0d1424 55%, #1a2438 55%) !important; }
  .p-snow::before { background:
    radial-gradient(1.5px 1.5px at 20% 15%, #f2f6ff, transparent),
    radial-gradient(1.5px 1.5px at 55% 40%, #d8e0f0, transparent),
    radial-gradient(1.5px 1.5px at 80% 20%, #f2f6ff, transparent),
    radial-gradient(2px 2px at 38% 65%, #ffd98a, transparent),
    radial-gradient(2px 2px at 68% 70%, #ffca6a, transparent);
    animation: fall 2.6s linear infinite; }
  @keyframes fall { to { background-position: 6px 64px, -5px 64px, 4px 64px, 0 0, 0 0; } }
  .sect { font-size: 11px; letter-spacing: .3em; color: var(--dim); margin: 22px 0 12px; }
  .sect:first-of-type { margin-top: 0; }
  .panel { background: var(--panel); border: 1px solid var(--edge); border-radius: 10px;
           padding: 18px; margin-top: 24px; }
  .panel h2 { font-size: 11px; letter-spacing: .3em; color: var(--dim); margin-bottom: 16px; }
  .slider-row { display: flex; align-items: center; gap: 14px; margin-bottom: 14px; font-size: 12px; }
  .slider-row label { width: 110px; letter-spacing: .12em; color: var(--txt); }
  .slider-row output { width: 42px; text-align: right; color: var(--cyan); }
  input[type=range] { flex: 1; appearance: none; height: 3px; border-radius: 2px;
    background: linear-gradient(90deg, var(--cyan), var(--mag)); outline: none; }
  input[type=range]::-webkit-slider-thumb { appearance: none; width: 16px; height: 16px; border-radius: 50%;
    background: #fff; border: 2px solid var(--cyan); box-shadow: 0 0 10px var(--cyan); cursor: pointer; }
  footer { margin-top: 26px; font-size: 10px; color: var(--dim); letter-spacing: .15em; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>NEXUS<span>_</span>DISPLAY CONTROL</h1>
    <div class="status"><div class="dot" id="dot"></div><span id="stx">LINKING…</span></div>
  </header>
  <div class="sect">// SCI-FI PROTOCOLS</div>
  <div class="grid" id="grid"></div>
  <div class="sect">// CHILL ZONE</div>
  <div class="grid" id="grid2"></div>
  <div class="sect">// CUSTOM MEDIA</div>
  <div class="grid" id="mediaGrid">
    <button class="card" id="uploadCard" style="--c:#00e5ff">
      <div class="prev" style="display:flex;align-items:center;justify-content:center;
           font-size:26px;color:var(--cyan)">&#8686;</div>
      <div class="nm">Upload</div><div class="ds" id="upStatus">PNG / JPG / GIF</div>
    </button>
  </div>
  <input type="file" id="file" accept="image/*,.gif" hidden>
  <div class="panel">
    <h2>PARAMETERS</h2>
    <div class="slider-row">
      <label>SPEED</label>
      <input type="range" id="speed" min="1" max="10" value="5">
      <output id="speedv">5</output>
    </div>
    <div class="slider-row" style="margin-bottom:0">
      <label>BRIGHTNESS</label>
      <input type="range" id="bright" min="10" max="100" value="90">
      <output id="brightv">90%</output>
    </div>
  </div>
  <footer id="foot">AWAITING UPLINK</footer>
</div>
<script>
const grid = document.getElementById('grid');
let anims = [], state = {};

function render() {
  const grid2 = document.getElementById('grid2');
  grid.innerHTML = ''; grid2.innerHTML = '';
  for (const a of anims) {
    if (a.group === 'custom') continue;   // rendered statically with the thumbnail
    const b = document.createElement('button');
    b.className = 'card' + (state.anim === a.id ? ' active' : '');
    b.style.setProperty('--c', a.color);
    b.innerHTML = `<div class="prev p-${a.id}"></div><div class="nm">${a.name}</div><div class="ds">${a.desc}</div>`;
    b.onclick = () => post({anim: a.id});
    (a.group === 'chill' ? grid2 : grid).appendChild(b);
  }
  for (const c of document.querySelectorAll('#mediaGrid .card[data-id]'))
    c.classList.toggle('active', state.anim === 'custom' && state.img === +c.dataset.id);
  document.getElementById('speed').value = state.speed;
  document.getElementById('speedv').textContent = state.speed;
  document.getElementById('bright').value = state.brightness;
  document.getElementById('brightv').textContent = state.brightness + '%';
}

async function post(patch) {
  const r = await fetch('/api/state', {method: 'POST', body: JSON.stringify(patch)});
  state = await r.json();
  render();
}

async function poll() {
  try {
    const r = await fetch('/api/status');
    const s = await r.json();
    anims = s.animations; state = s.state;
    const dot = document.getElementById('dot'), stx = document.getElementById('stx');
    dot.className = 'dot ' + (s.online ? 'on' : 'off');
    stx.textContent = s.online ? 'DISPLAY ONLINE' : 'DISPLAY OFFLINE';
    document.getElementById('foot').textContent = s.ip
      ? `UNIT ${s.ip} // LAST CONTACT ${s.last_seen_secs}s AGO // REV ${s.state.rev}`
      : 'NO UNIT HAS CONNECTED YET';
    if (!document.activeElement || document.activeElement.tagName !== 'INPUT') render();
  } catch (e) { document.getElementById('stx').textContent = 'SERVER UNREACHABLE'; }
}

let t;
function debouncedPost(patch) { clearTimeout(t); t = setTimeout(() => post(patch), 250); }
document.getElementById('speed').oninput = e => {
  document.getElementById('speedv').textContent = e.target.value;
  debouncedPost({speed: +e.target.value});
};
document.getElementById('bright').oninput = e => {
  document.getElementById('brightv').textContent = e.target.value + '%';
  debouncedPost({brightness: +e.target.value});
};

// ---- custom media library (stills + GIFs) ----
document.getElementById('uploadCard').onclick = () => document.getElementById('file').click();

function coverDraw(ctx, src, W, H) {
  const s = Math.max(W / src.width, H / src.height);
  ctx.drawImage(src, (W - src.width*s)/2, (H - src.height*s)/2, src.width*s, src.height*s);
}

function pack565(data, out, off) {   // RGBA -> RGB565 little-endian
  for (let i = 0, j = off; i < data.length; i += 4) {
    const v = ((data[i] & 0xF8) << 8) | ((data[i+1] & 0xFC) << 3) | (data[i+2] >> 3);
    out[j++] = v & 0xFF; out[j++] = v >> 8;
  }
}

function unpack565(dv, off, W, H, ctx) {
  const id = ctx.createImageData(W, H);
  for (let p = 0; p < W * H; p++) {
    const v = dv.getUint16(off + p * 2, true);
    id.data[p*4] = (v >> 8) & 0xF8; id.data[p*4+1] = (v >> 3) & 0xFC;
    id.data[p*4+2] = (v << 3) & 0xF8; id.data[p*4+3] = 255;
  }
  ctx.putImageData(id, 0, 0);
}

async function refreshMedia() {
  const r = await fetch('/api/media');
  const m = await r.json();
  const g = document.getElementById('mediaGrid');
  for (const c of g.querySelectorAll('.card[data-id]')) c.remove();
  for (const it of m.items) {
    const b = document.createElement('button');
    b.className = 'card';
    b.dataset.id = it.id;
    b.style.setProperty('--c', it.kind ? '#ff4fd8' : '#ffffff');
    b.innerHTML = `<div class="prev" style="display:flex;align-items:center;justify-content:center">
        <canvas width="${it.w}" height="${it.h}" style="max-width:100%;max-height:100%"></canvas></div>
      <div class="nm"></div>
      <div class="ds">${it.kind ? it.frames + ' frames // GIF' : 'still image'}
        <span style="color:#ff5c5c;float:right;cursor:pointer" data-del="${it.id}">&#10005;</span></div>`;
    b.querySelector('.nm').textContent = it.name;
    b.onclick = async ev => {
      if (ev.target.dataset.del) {
        await fetch('/api/media/delete', {method: 'POST', body: JSON.stringify({id: it.id})});
        refreshMedia();
      } else {
        state = await (await fetch('/api/media/select',
          {method: 'POST', body: JSON.stringify({id: it.id})})).json();
        render();
      }
    };
    g.appendChild(b);
    fetch(`/api/media/${it.id}.bin`).then(async res => {   // thumbnail = first frame
      if (!res.ok) return;
      const dv = new DataView(await res.arrayBuffer());
      unpack565(dv, 12, it.w, it.h, b.querySelector('canvas').getContext('2d'));
    });
  }
  render();
}

async function buildStill(f) {
  let img;
  try { img = await createImageBitmap(f); }
  catch (_) { throw new Error('browser cannot decode this format (HEIC?) - use PNG/JPG/GIF'); }
  const c = new OffscreenCanvas(320, 240), x = c.getContext('2d');
  coverDraw(x, img, 320, 240);
  const out = new Uint8Array(12 + 320 * 240 * 2);
  out.set([0x4E, 0x58, 0x30, 0x31, 0, 1, 0, 0, 320 & 255, 320 >> 8, 240, 0]);
  pack565(x.getImageData(0, 0, 320, 240).data, out, 12);
  return out;
}

async function buildGif(f) {
  if (!('ImageDecoder' in window)) throw new Error('GIF decoding needs Chrome/Edge');
  const dec = new ImageDecoder({data: await f.arrayBuffer(), type: 'image/gif'});
  await dec.tracks.ready; await dec.completed;
  const total = dec.tracks.selectedTrack.frameCount;
  const MAXF = 20, n = Math.min(total, MAXF);
  const c = new OffscreenCanvas(160, 120), x = c.getContext('2d');
  const out = new Uint8Array(12 + n * 160 * 120 * 2);
  let delay = 100;
  for (let k = 0; k < n; k++) {
    const idx = total <= MAXF ? k : Math.floor(k * total / n);   // sample long GIFs
    const {image} = await dec.decode({frameIndex: idx});
    if (k === 0 && image.duration) delay = Math.min(500, Math.max(30, image.duration / 1000));
    coverDraw(x, image, 160, 120);
    image.close();
    pack565(x.getImageData(0, 0, 160, 120).data, out, 12 + k * 160 * 120 * 2);
  }
  out.set([0x4E, 0x58, 0x30, 0x31, 1, n, delay & 255, delay >> 8, 160, 0, 120, 0]);
  return out;
}

document.getElementById('file').onchange = async e => {
  const f = e.target.files[0];
  if (!f) return;
  const st = document.getElementById('upStatus');
  st.textContent = 'PROCESSING...';
  try {
    const bin = f.type === 'image/gif' ? await buildGif(f) : await buildStill(f);
    st.textContent = 'UPLOADING...';
    const name = encodeURIComponent(f.name.replace(/\.[^.]+$/, ''));
    const r = await fetch('/api/media?name=' + name, {method: 'POST', body: bin});
    if (!r.ok) throw new Error(await r.text());
    state = await r.json();
    st.textContent = 'SENT // DISPLAY UPDATING';
    refreshMedia();
  } catch (err) {
    st.textContent = 'FAILED: ' + err.message;
  }
  e.target.value = '';
};

poll(); setInterval(poll, 3000); refreshMedia();
</script>
</body>
</html>
"""


def main():
    ap = argparse.ArgumentParser(description="NEXUS display control portal")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8484)
    args = ap.parse_args()
    load_state()
    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"NEXUS DISPLAY CONTROL online -> http://{args.host}:{args.port}")
    print(f"ESP32 poll endpoint         -> GET /api/state?device=1")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
