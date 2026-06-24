#!/usr/bin/env -S uv run python
"""Browser-faithful preview-stream health probe.

Measures the device's 3D-preview WebSocket stream the way a real browser tab experiences it, so the
numbers here match what a person watching the preview sees. This is the measurement foundation for
tuning the preview transport: without a faithful probe, a plain one-shot reader stalls where a
browser does not (it gives up on a close instead of reconnecting), so it reports problems the user
never sees AND masks problems the user does see. Both failure modes burned real debugging time, hence
this tool.

What it replicates from the real client (src/ui/app.js connectWs):
  - reads binary frames (0x03 coord table, 0x02 RGB colour) — see src/light/drivers/PreviewDriver.h;
  - sends a "ping" TEXT frame every 25 s (app.js heartbeat — Safari reaps idle sockets otherwise);
  - AUTO-RECONNECTS on close with 500ms→5s backoff (app.js ws.onclose → connectWs). The browser never
    gives up, so a momentary device-side close shows as a brief visual blip, not a frozen preview. A
    probe that quits on close measures something no user experiences.

What it reports (the honest "what does the user see" metrics):
  - colour frames + sustained fps over the window;
  - reconnects: each one is a blip the user might notice (a ~0.5 s gap as the browser re-handshakes);
  - maxgap: the longest stretch with NO colour frame — the real "did it freeze?" number. A browser
    that reconnects in 500 ms has a ~0.5 s gap (looks continuous); a multi-second maxgap is a visible
    freeze the reconnect could not hide.
  - verdict: SMOOTH (no long gaps, frames flowing) / CHOPPY (frames but long gaps or reconnects) /
    DEAD (no colour frames at all).

Stdlib-only (socket/base64/os/struct/time/json/urllib), like the scenario helpers — runs anywhere uv
runs, no third-party deps. Devices come from an explicit ip[:port] argument, or (no arg) every online
device on moondeck.json's active network.

Usage:
  uv run scripts/diag/preview_health.py <ip[:port]> [--seconds N] [--grid SIZE]
  uv run scripts/diag/preview_health.py            # all online devices on the active network
    --seconds N   measurement window per device (default 30)
    --grid SIZE   POST Grid width/height=SIZE, depth=1 before measuring (default: leave grid as-is)
"""

import argparse
import base64
import json
import os
import socket
import struct
import sys
import time
import urllib.request

# Repo root: scripts/diag/preview_health.py → ../../
_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


# --- minimal RFC 6455 client (one connection), mirroring scripts/scenario/_preview_ws.py -----------

class _Ws:
    """One /ws connection to host "ip[:port]". Reads server frames; can send a text frame (the
    keepalive ping). The device never fragments and stays under 64 KiB (HttpServerModule), so the
    reader handles only FIN frames with the 7/16-bit length forms."""

    def __init__(self, host, timeout_s=5.0):
        h, _, p = host.partition(":")
        self.sock = socket.create_connection((h, int(p or 80)), timeout=timeout_s)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(
            (f"GET /ws HTTP/1.1\r\nHost: {host}\r\n"
             f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
             f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        raw = b""
        while b"\r\n\r\n" not in raw:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise ConnectionError("WS handshake: connection closed")
            raw += chunk
        head, _, rest = raw.partition(b"\r\n\r\n")
        if b" 101" not in head.split(b"\r\n", 1)[0]:
            self.sock.close()
            raise ConnectionError("WS handshake refused (max 4 clients — close preview tabs?)")
        self._buf = bytearray(rest)

    def _need(self, n, deadline):
        while len(self._buf) < n:
            self.sock.settimeout(max(0.05, deadline - time.monotonic()))
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed")
            self._buf += chunk

    def recv_frame(self, deadline):
        """Return (opcode, payload) for the next frame, or raise on close/timeout."""
        self._need(2, deadline)
        b0, b1 = self._buf[0], self._buf[1]
        opcode = b0 & 0x0F
        ln = b1 & 0x7F
        off = 2
        if ln == 126:
            self._need(4, deadline)
            ln = struct.unpack(">H", self._buf[2:4])[0]
            off = 4
        elif ln == 127:
            self._need(10, deadline)
            ln = struct.unpack(">Q", self._buf[2:10])[0]
            off = 10
        self._need(off + ln, deadline)
        payload = bytes(self._buf[off:off + ln])
        del self._buf[:off + ln]
        return opcode, payload

    def send_text(self, s):
        # Client→server frames MUST be masked (RFC 6455). The browser sends "ping" as a text frame.
        data = s.encode()
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        hdr = bytes([0x81, 0x80 | len(data)]) + mask
        self.sock.sendall(hdr + masked)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# --- measurement (browser-faithful: reconnect + keepalive) -----------------------------------------

def measure(host, seconds, grid):
    # --grid resizes the device's Grid for the run; snapshot the original so we restore it afterwards
    # (the probe stays non-destructive, like the live scenarios — decisions.md). The restore is in the
    # finally at the end of the function.
    saved_grid = _read_grid(host) if grid else {}
    if grid:
        for ctrl in (("width", grid), ("height", grid), ("depth", 1)):
            _set_control(host, "Grid", ctrl[0], ctrl[1])
        time.sleep(3)  # let the geometry rebuild settle

    try:
        return _measure_loop(host, seconds, grid)
    finally:
        for name, val in saved_grid.items():
            if val is not None:
                _set_control(host, "Grid", name, val)


def _measure_loop(host, seconds, grid):
    t0 = time.monotonic()
    deadline = t0 + seconds
    colour = coord = reconnects = 0
    pts = None
    last_colour = 0.0
    maxgap = 0.0
    backoff = 0.5

    while time.monotonic() < deadline:
        reconnects += 1
        try:
            ws = _Ws(host)
        except Exception:
            time.sleep(min(backoff, max(0.0, deadline - time.monotonic())))
            backoff = min(backoff * 2, 5.0)
            continue
        last_ping = time.monotonic()
        try:
            while time.monotonic() < deadline:
                now = time.monotonic()
                if now - last_ping >= 25:
                    ws.send_text("ping")
                    last_ping = now
                opcode, payload = ws.recv_frame(deadline)
                if opcode == 0x08:           # server close frame
                    raise ConnectionError("server close")
                if opcode != 0x02 or not payload:
                    continue                 # text (state JSON) / ping / pong — skip
                t = time.monotonic()
                if payload[0] == 0x02:
                    if last_colour and t - last_colour > maxgap:
                        maxgap = t - last_colour
                    last_colour = t
                    colour += 1
                elif payload[0] == 0x03:
                    coord += 1
                    if len(payload) >= 5:
                        pts = struct.unpack("<I", payload[1:5])[0]
            backoff = 0.5                    # a session that ran to the deadline resets the backoff
        except Exception:
            # Closed/timeout → reconnect like the browser, after the same 500ms→5s backoff it applies
            # on ANY close (app.js ws.onclose), not only on a failed handshake.
            time.sleep(min(backoff, max(0.0, deadline - time.monotonic())))
            backoff = min(backoff * 2, 5.0)
        finally:
            ws.close()

    elapsed = time.monotonic() - t0
    reconnects -= 1                          # the first connect is not a "re"-connect
    if last_colour:                          # a trailing no-colour stretch is a freeze at the end
        tail = time.monotonic() - last_colour
        maxgap = max(maxgap, tail)
    fps = colour / elapsed if elapsed else 0
    verdict = "SMOOTH" if (maxgap < 1.5 and colour > elapsed) else ("CHOPPY" if colour else "DEAD")
    print(f"  [{host} grid={grid or '?'}] {colour}f over {elapsed:.0f}s ({fps:.1f}fps), "
          f"{coord} coord (pts={pts}), {reconnects} reconnect(s), maxgap={maxgap:.1f}s -> {verdict}")
    return verdict


def _set_control(host, module, control, value):
    body = json.dumps({"module": module, "control": control, "value": value}).encode()
    h, _, p = host.partition(":")
    url = f"http://{h}:{p or 80}/api/control"
    req = urllib.request.Request(url, data=body,
                                 headers={"Content-Type": "application/json"}, method="POST")
    try:
        urllib.request.urlopen(req, timeout=5).read()
    except Exception as e:
        print(f"  (warn) set {module}.{control}={value} failed: {e}", file=sys.stderr)


def _read_grid(host):
    """Current Grid {width,height,depth} from /api/state, or {} if unavailable — used to snapshot
    the grid before --grid changes it, so the probe restores it (non-destructive, like the live
    scenarios). Children live under the `children` key."""
    h, _, p = host.partition(":")
    try:
        with urllib.request.urlopen(f"http://{h}:{p or 80}/api/state", timeout=5) as r:
            state = json.load(r)
    except Exception:
        return {}
    out = {}
    def walk(m):
        for c in m.get("controls", []):
            if c.get("name") in ("width", "height", "depth"):
                out[c["name"]] = c.get("value")
        for ch in m.get("children", []):
            walk(ch)
    for m in state.get("modules", []):
        walk(m)
    return out


def _online_devices():
    """ip[:port] for every online device on moondeck.json's active network."""
    cfg = json.load(open(os.path.join(_ROOT, "scripts", "moondeck.json")))
    nets = cfg.get("networks", [])
    active = cfg.get("active_network")
    net = next((n for n in nets if n.get("name") == active), nets[0] if nets else None)
    if not net:
        return []
    return [d["ip"] for d in net.get("devices", []) if d.get("online") and d.get("ip")]


def main():
    ap = argparse.ArgumentParser(description="Browser-faithful preview-stream health probe.")
    # Positional for CLI convenience; --host is the MoonDeck convention (the UI forwards the selected
    # device as --host, matching run_network_live.py). Either works; --host wins if both are given.
    ap.add_argument("host_pos", nargs="?", metavar="host",
                    help="device ip[:port]; omit to probe all online devices on the active network")
    ap.add_argument("--host", dest="host_opt", help="device ip[:port] (MoonDeck forwards this)")
    ap.add_argument("--seconds", type=int, default=30, help="measurement window per device")
    ap.add_argument("--grid", type=int, default=0, help="set Grid to SIZE×SIZE before measuring")
    args = ap.parse_args()

    host = args.host_opt or args.host_pos
    hosts = [host] if host else _online_devices()
    if not hosts:
        print("No devices to probe (give an ip[:port] or mark devices online in moondeck.json).")
        return 1
    print(f"Preview health — {args.seconds}s per device"
          f"{f', grid {args.grid}²' if args.grid else ''}:")
    all_smooth = True
    for h in hosts:
        v = measure(h, args.seconds, args.grid)
        all_smooth = all_smooth and v == "SMOOTH"
    # Exit non-zero if any device was not SMOOTH, so the exit code is honest — MoonDeck's Live tab
    # reads it (a CHOPPY/DEAD run shows as failed) and a CLI/CI caller can branch on it. The per-device
    # verdict is always printed regardless, so the line above is the human-readable detail.
    return 0 if all_smooth else 1


if __name__ == "__main__":
    sys.exit(main())
