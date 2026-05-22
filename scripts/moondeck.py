#!/usr/bin/env python3
"""MoonDeck — browser-based developer console for projectMM."""

import http.server
import json
import os
import signal
import subprocess
import sys
import threading
from pathlib import Path

PORT = 8420
ROOT = Path(__file__).resolve().parent.parent
SCRIPTS_DIR = Path(__file__).resolve().parent
UI_DIR = SCRIPTS_DIR / "moondeck_ui"
ASSETS_DIR = ROOT / "docs" / "assets"
STATE_FILE = SCRIPTS_DIR / "moondeck.json"

# ---------------------------------------------------------------------------
# Script definitions (loaded from scripts.json)
# ---------------------------------------------------------------------------

SCRIPTS_FILE = SCRIPTS_DIR / "moondeck_config.json"

def load_scripts():
    with open(SCRIPTS_FILE) as f:
        return json.load(f)

_scripts_data = load_scripts()
SCRIPTS = _scripts_data["scripts"]
ESP32_ENVS = _scripts_data["envs"]

# ---------------------------------------------------------------------------
# Device discovery
# ---------------------------------------------------------------------------

def _get_local_subnet():
    """Try to detect the local subnet (e.g. '192.168.1')."""
    import socket
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ".".join(ip.split(".")[:3])
    except Exception:
        return "192.168.1"


def _probe_device(ip, port=8080, timeout=0.4):
    """Probe a single IP for /api/state. Returns device info or None.

    Short timeout: on a LAN a live device answers in a few ms and a dead IP
    refuses the connection almost instantly; 0.4s only matters for IPs that
    silently drop packets (firewalled hosts), and a subnet scan should not
    stall seconds on those.
    """
    import urllib.request
    import urllib.error
    url = f"http://{ip}:{port}/api/state"
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read())
            module_count = len(data.get("modules", []))
            return {"ip": f"{ip}:{port}", "modules": module_count}
    except Exception:
        return None


def discover_devices(subnet=""):
    """Scan subnet for devices responding to /api/state."""
    if not subnet:
        subnet = _get_local_subnet()

    # .1-.254 on port 80 (ESP32) and 8080 (desktop), plus localhost.
    targets = [(f"{subnet}.{i}", port)
               for i in range(1, 255) for port in (80, 8080)]
    targets.append(("localhost", 8080))

    # Wide thread pool — the probes are I/O-bound (almost always blocked on the
    # socket, not the CPU), so running all ~509 in one wave means the whole /24
    # scan finishes in about one probe-timeout window (~0.4s) instead of
    # batch-serializing. The pool still caps thread churn vs. raw thread spawns.
    from concurrent.futures import ThreadPoolExecutor
    devices = []
    with ThreadPoolExecutor(max_workers=len(targets)) as pool:
        for result in pool.map(lambda t: _probe_device(*t), targets):
            if result:
                devices.append(result)

    # Deduplicate: if localhost found, remove the Mac's own IP:port (same process)
    hasLocalhost = any(d["ip"].startswith("localhost") for d in devices)
    if hasLocalhost:
        import socket
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            localIp = s.getsockname()[0]
            s.close()
            devices = [d for d in devices if not d["ip"].startswith(localIp + ":")]
        except Exception:
            pass

    # Sort by IP
    devices.sort(key=lambda d: d["ip"])
    return devices, subnet


def refresh_devices(known_devices):
    """Probe known devices to check online/offline status."""
    def probe(device):
        ip = device.get("ip", "")
        if ":" in ip:
            host, port = ip.rsplit(":", 1)
            return _probe_device(host, int(port))
        return _probe_device(ip)

    if not known_devices:
        return []
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=16) as pool:
        return [r for r in pool.map(probe, known_devices) if r]


# ---------------------------------------------------------------------------
# State management
# ---------------------------------------------------------------------------

def load_state():
    if STATE_FILE.exists():
        with open(STATE_FILE) as f:
            return json.load(f)
    return {"env": "esp32", "port": "", "devices": [], "tab": "pc"}


def save_state(state):
    with open(STATE_FILE, "w") as f:
        json.dump(state, f, indent=2)


# ---------------------------------------------------------------------------
# Process management
# ---------------------------------------------------------------------------

_running: dict[str, subprocess.Popen] = {}
_lock = threading.Lock()
_IS_WIN = sys.platform == "win32"


def _kill_process_by_name(name: str):
    """Kill processes matching name. Cross-platform."""
    if _IS_WIN:
        subprocess.run(["taskkill", "/F", "/IM", name + ".exe"],
                       capture_output=True)
    else:
        subprocess.run(["pkill", "-f", name], capture_output=True)


def kill_script(script_id: str):
    with _lock:
        proc = _running.pop(script_id, None)
    if proc and proc.poll() is None:
        try:
            if _IS_WIN:
                proc.terminate()
            else:
                os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        except (OSError, ProcessLookupError):
            pass

    # Clean up any orphaned processes (e.g. projectMM after os.execv)
    script_def = next((s for s in SCRIPTS if s["id"] == script_id), None)
    pname = script_def.get("process_name") if script_def else None
    if pname:
        _kill_process_by_name(pname)


def is_process_running(name: str) -> bool:
    """Check if a process matching name is running. Cross-platform."""
    if _IS_WIN:
        r = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}.exe"],
                           capture_output=True, text=True)
        return name in r.stdout
    else:
        r = subprocess.run(["pgrep", "-f", name], capture_output=True)
        return r.returncode == 0


# ---------------------------------------------------------------------------
# Serial port discovery
# ---------------------------------------------------------------------------

def list_serial_ports() -> list[str]:
    """List available serial ports."""
    ports = []
    # macOS
    import glob
    ports.extend(glob.glob("/dev/tty.usb*"))
    ports.extend(glob.glob("/dev/ttyUSB*"))
    ports.extend(glob.glob("/dev/ttyACM*"))
    # Windows COM ports
    if sys.platform == "win32":
        for i in range(256):
            port = f"COM{i}"
            try:
                import serial
                s = serial.Serial(port)
                s.close()
                ports.append(port)
            except Exception:
                pass
    return sorted(ports)


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class MoonDeckHandler(http.server.BaseHTTPRequestHandler):

    def log_message(self, format, *args):
        # Suppress default request logging
        pass

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError):
            pass  # Browser closed connection — harmless

    def _send_json(self, data, status=200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length) if length else b""

    def do_GET(self):
        if self.path == "/api/scripts":
            self._send_json({"scripts": SCRIPTS, "envs": ESP32_ENVS})

        elif self.path == "/api/ports":
            self._send_json({"ports": list_serial_ports()})

        elif self.path == "/api/state":
            self._send_json(load_state())

        elif self.path == "/api/running":
            running = {}
            for s in SCRIPTS:
                pname = s.get("process_name")
                if pname:
                    running[s["id"]] = is_process_running(pname)
            self._send_json(running)

        elif self.path.startswith("/api/stream/"):
            script_id = self.path.split("/")[-1]
            self._handle_stream(script_id)

        elif self.path.startswith("/api/help"):
            self._serve_help()

        else:
            self._serve_static()

    def do_POST(self):
        if self.path.startswith("/api/run/"):
            script_id = self.path.split("/")[-1]
            body = self._read_body()
            params = json.loads(body) if body else {}
            self._handle_run(script_id, params)

        elif self.path.startswith("/api/kill/"):
            script_id = self.path.split("/")[-1]
            kill_script(script_id)
            self._send_json({"status": "killed"})

        elif self.path == "/api/state":
            body = self._read_body()
            state = json.loads(body) if body else {}
            current = load_state()
            current.update(state)
            save_state(current)
            self._send_json(current)

        elif self.path == "/api/discover":
            body = self._read_body()
            params = json.loads(body) if body else {}
            subnet = params.get("subnet", "")
            devices, scanned_subnet = discover_devices(subnet)
            self._send_json({"devices": devices, "subnet": scanned_subnet})

        elif self.path == "/api/refresh":
            body = self._read_body()
            params = json.loads(body) if body else {}
            known = params.get("devices", [])
            online = refresh_devices(known)
            self._send_json({"online": online})

        else:
            self.send_error(404)

    def _handle_run(self, script_id: str, params: dict):
        """Start a script and return immediately. Client uses SSE to stream."""
        script_def = next((s for s in SCRIPTS if s["id"] == script_id), None)
        if not script_def:
            self._send_json({"error": "unknown script"}, 404)
            return

        kill_script(script_id)  # Kill previous if still running

        script_path = SCRIPTS_DIR / script_def["script"]
        cmd = ["uv", "run", str(script_path)]

        # Add environment/port/host args
        if script_def.get("needs_env") and params.get("env"):
            cmd.extend(["--env", params["env"]])
        if script_def.get("needs_port") and params.get("port"):
            cmd.extend(["--port", params["port"]])
        if params.get("host"):
            cmd.extend(["--host", params["host"]])

        try:
            popen_kwargs = dict(
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                cwd=str(ROOT),
            )
            if not _IS_WIN:
                popen_kwargs["start_new_session"] = True
            else:
                popen_kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
            proc = subprocess.Popen(cmd, **popen_kwargs)
            with _lock:
                _running[script_id] = proc
            self._send_json({"status": "started", "pid": proc.pid})
        except Exception as e:
            self._send_json({"error": str(e)}, 500)

    def _handle_stream(self, script_id: str):
        """SSE endpoint: stream stdout of a running script."""
        with _lock:
            proc = _running.get(script_id)

        if not proc:
            self.send_error(404, "No running process")
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.end_headers()

        try:
            for line in iter(proc.stdout.readline, b""):
                text = line.decode("utf-8", errors="replace").rstrip("\n")
                self.wfile.write(f"data: {json.dumps(text)}\n\n".encode())
                self.wfile.flush()

            proc.wait()
            exit_msg = f"[exit code: {proc.returncode}]"
            self.wfile.write(f"data: {json.dumps(exit_msg)}\n\n".encode())
            done_data = json.dumps({"exitCode": proc.returncode})
            self.wfile.write(f"event: done\ndata: {done_data}\n\n".encode())
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with _lock:
                _running.pop(script_id, None)

    def _serve_help(self):
        """Serve MoonDeck.md as simple HTML with anchor support."""
        import html as html_mod
        md_path = SCRIPTS_DIR / "MoonDeck.md"
        if not md_path.exists():
            self.send_error(404, "MoonDeck.md not found")
            return

        # Parse anchor from query string
        anchor = ""
        if "?" in self.path:
            anchor = self.path.split("?", 1)[1]

        text = md_path.read_text(encoding="utf-8")
        # Simple markdown to HTML: headings and code blocks
        lines = []
        in_code = False
        for line in text.splitlines():
            if line.startswith("```"):
                if in_code:
                    lines.append("</code></pre>")
                else:
                    lines.append("<pre><code>")
                in_code = not in_code
            elif in_code:
                lines.append(html_mod.escape(line))
            elif line.startswith("## "):
                slug = line[3:].strip().lower().replace(" ", "_")
                lines.append(f'<h2 id="{slug}">{html_mod.escape(line[3:])}</h2>')
            elif line.startswith("# "):
                lines.append(f"<h1>{html_mod.escape(line[2:])}</h1>")
            elif line.strip() == "":
                lines.append("<br>")
            else:
                lines.append(f"<p>{html_mod.escape(line)}</p>")

        body_html = "\n".join(lines)
        page = f"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
body {{ font-family: -apple-system, monospace; background: #0d1117; color: #c0c0c0;
       padding: 20px; line-height: 1.6; font-size: 13px; }}
h1 {{ color: #e94560; font-size: 18px; }}
h2 {{ color: #e94560; font-size: 15px; border-bottom: 1px solid #0f3460; padding-bottom: 4px; }}
pre {{ background: #161b22; padding: 10px; border-radius: 4px; overflow-x: auto; }}
code {{ font-size: 12px; }}
p {{ margin: 2px 0; }}
</style></head><body>
{body_html}
{f'<script>document.getElementById("{anchor}")?.scrollIntoView();</script>' if anchor else ''}
</body></html>"""

        data = page.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _serve_static(self):
        """Serve files from moondeck_ui/ and docs/assets/."""
        path = self.path.lstrip("/")
        if path == "" or path == "/":
            path = "index.html"

        # Serve /assets/* from docs/assets/
        if path.startswith("assets/"):
            file_path = ASSETS_DIR / path.removeprefix("assets/")
        else:
            file_path = UI_DIR / path

        if not file_path.exists() or not file_path.is_file():
            self.send_error(404)
            return

        content_types = {
            ".html": "text/html",
            ".css": "text/css",
            ".js": "application/javascript",
            ".json": "application/json",
            ".png": "image/png",
            ".svg": "image/svg+xml",
        }
        ext = file_path.suffix.lower()
        content_type = content_types.get(ext, "application/octet-stream")

        data = file_path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    server = http.server.ThreadingHTTPServer(("", PORT), MoonDeckHandler)
    print(f"MoonDeck running at http://localhost:{PORT}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down...")
        # Kill any running scripts
        for sid in list(_running.keys()):
            kill_script(sid)
        server.server_close()


if __name__ == "__main__":
    main()
