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

# Shared test-metadata parsers live next to the doc generator. Both this server
# and scripts/docs/generate_test_docs.py import from there so the two views of
# the same source files (HTML in MoonDeck, markdown in docs/tests/) can't drift.
sys.path.insert(0, str(SCRIPTS_DIR / "docs"))
import _test_metadata as test_meta  # noqa: E402


def _app_version():
    """Read the project version from library.json. '?' if unavailable."""
    try:
        return json.loads((ROOT / "library.json").read_text()).get("version", "?")
    except Exception:
        return "?"


APP_VERSION = _app_version()

# ---------------------------------------------------------------------------
# Script definitions (loaded from scripts.json)
# ---------------------------------------------------------------------------

SCRIPTS_FILE = SCRIPTS_DIR / "moondeck_config.json"

def load_scripts():
    with open(SCRIPTS_FILE) as f:
        return json.load(f)

_scripts_data = load_scripts()
SCRIPTS = _scripts_data["scripts"]
BOARDS = _scripts_data["boards"]

# ---------------------------------------------------------------------------
# Device discovery
# ---------------------------------------------------------------------------

def _lan_ip():
    """This machine's LAN IP. '' if it can't be determined (offline).

    connect() on a UDP socket sends no packet — it just picks the outbound
    interface, whose address is the LAN IP.
    """
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return ""
    finally:
        s.close()


def _get_local_subnet():
    """The local /24 subnet prefix (e.g. '192.168.1'). Falls back to a default."""
    ip = _lan_ip()
    return ".".join(ip.split(".")[:3]) if ip else "192.168.1"


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

    # The local app answers on both localhost and this machine's LAN IP — the
    # subnet scan finds the LAN-IP entry, the explicit localhost probe finds the
    # other. Keep the LAN IP (usable from any device) and drop the redundant
    # localhost entry so the discovered list shows real network addresses.
    localIp = _lan_ip()
    hasLanEntry = localIp and any(d["ip"].startswith(localIp + ":") for d in devices)
    if hasLanEntry:
        devices = [d for d in devices if not d["ip"].startswith("localhost:")]

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
            self._send_json({"scripts": SCRIPTS, "boards": BOARDS})

        elif self.path == "/api/ports":
            self._send_json({"ports": list_serial_ports()})

        elif self.path == "/api/scenarios":
            self._send_json({"scenarios": self._list_scenarios()})

        elif self.path.startswith("/api/scenarios/"):
            self._serve_scenario_steps()

        elif self.path == "/api/test-modules":
            self._send_json({"modules": test_meta.list_test_modules()})

        elif self.path.startswith("/api/unit-tests/"):
            self._serve_unit_tests_for_module()

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

        elif self.path.startswith("/api/docs/"):
            self._serve_doc()

        elif self.path == "/api/history-report":
            self._serve_history_report()

        elif self.path.startswith("/api/doc-asset/"):
            self._serve_doc_asset()

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

        # Forward selector state (board / port / host) when the script
        # declares it needs them. The UI maintains a single Firmware dropdown
        # on the ESP32 tab driving every needs_board script; the older
        # per-board buttons + extra_args plumbing was collapsed into this.
        if script_def.get("needs_board") and params.get("board"):
            cmd.extend(["--board", params["board"]])
        if script_def.get("needs_port") and params.get("port"):
            cmd.extend(["--port", params["port"]])
        if script_def.get("needs_scenario") and params.get("scenario"):
            cmd.extend(["--name", params["scenario"]])
        if script_def.get("needs_module") and params.get("module"):
            cmd.extend(["--module", params["module"]])
        if params.get("host"):
            cmd.extend(["--host", params["host"]])
        for flag in script_def.get("flags", []):
            if params.get("flag_" + flag["id"]):
                cmd.append(flag["arg"])

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

    def _serve_doc(self):
        """Serve any docs/**/*.md file as styled HTML with deep-link anchor support.
        URL: /api/docs/<path>[?#anchor] — e.g. /api/docs/testing.md, /api/docs/tests/unit-tests.md"""
        import re as _re
        raw_path = self.path[len("/api/docs/"):]
        parts = raw_path.split("?", 1)
        filename = parts[0].strip("/")
        raw_anchor = parts[1] if len(parts) > 1 else ""
        anchor = raw_anchor if _re.fullmatch(r"[A-Za-z0-9._-]+", raw_anchor) else ""
        # Restrict to .md files and resolve under docs/ with a traversal guard:
        # build the candidate path, resolve symlinks, then verify it sits inside docs/.
        # Allows subpaths like tests/unit-tests.md while still rejecting ../escape attempts.
        if not filename.endswith(".md") or ".." in filename.split("/"):
            self.send_error(400, "Only .md files under docs/ are served here")
            return
        docs_root = (ROOT / "docs").resolve()
        md_path = (docs_root / filename).resolve()
        try:
            md_path.relative_to(docs_root)
        except ValueError:
            self.send_error(400, "Path escapes docs/")
            return
        if not md_path.exists():
            self.send_error(404, f"{filename} not found")
            return
        self._serve_markdown_as_html(md_path, anchor)

    def _list_scenarios(self):
        """Return [{name, module, also}] for every scenario JSON.

        The list endpoint surfaces module so MoonDeck's dropdown can filter
        without an extra round-trip per scenario."""
        return [
            {"name": s["path"].stem, "module": s["module"] or "", "also": s["also"]}
            for s in test_meta.collect_scenario_files()
        ]

    def _serve_unit_tests_for_module(self):
        """Render a per-module list of unit-test cases as an HTML view.
        URL: /api/unit-tests/<Module> — `Module` is the CamelCase @module name."""
        import html as html_mod

        raw = self.path[len("/api/unit-tests/"):].split("?", 1)[0].strip("/")
        if not raw or not all(c.isalnum() or c in "-_" for c in raw):
            self.send_error(400, "Bad module name")
            return

        cases = test_meta.cases_for_module(raw)
        if not cases:
            self.send_error(404, f"No unit tests found for module {raw}")
            return

        rows = []
        for i, c in enumerate(cases):
            desc_html = html_mod.escape(c["desc"]) if c["desc"] else f'<em>{html_mod.escape(c["name"])}</em>'
            tag = '' if c["primary"] else ' <span class="also">(also)</span>'
            rows.append(
                f'<div class="case"><div class="case-head">'
                f'<span class="case-num">{i + 1}.</span> '
                f'<span class="case-name">{html_mod.escape(c["name"])}</span>{tag}'
                f'</div><div class="case-desc">{desc_html}</div>'
                f'<div class="case-file"><code>{html_mod.escape(c["file"])}</code></div></div>'
            )

        body_html = "\n".join(rows)
        page = f"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
body {{ font-family: -apple-system, monospace; background: #0d1117; color: #c0c0c0;
       padding: 20px; line-height: 1.6; font-size: 13px; }}
h1 {{ color: #e94560; font-size: 18px; margin: 0 0 4px 0; }}
.sub {{ color: #9aa6ba; margin: 0 0 18px 0; font-size: 12px; }}
.case {{ margin: 6px 0 10px 0; padding: 6px 10px; background: #161b22;
         border-radius: 4px; border-left: 3px solid #0f3460; }}
.case-head {{ font-size: 13px; }}
.case-num {{ color: #6a7a99; }}
.case-name {{ color: #e94560; font-weight: 600; }}
.case-desc {{ margin-top: 2px; color: #c0c0c0; }}
.case-file {{ margin-top: 2px; color: #6a7a99; font-size: 11px; }}
.also {{ color: #6a7a99; font-size: 11px; margin-left: 4px; }}
code {{ background: transparent; color: #8aa6ba; padding: 0; }}
</style></head><body>
<h1>{html_mod.escape(raw)} unit tests</h1>
<div class="sub">{len(cases)} test case(s). "(also)" marks cases from files whose primary @module is a different module.</div>
{body_html}
</body></html>"""

        data_bytes = page.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data_bytes)))
        self.end_headers()
        self.wfile.write(data_bytes)

    def _serve_scenario_steps(self):
        """Render a single test/scenarios/<name>.json as an HTML view of its steps.
        URL: /api/scenarios/<name> — `name` is the file stem (no .json suffix), same
        names /api/scenarios returns. The view pane gets one card per step showing
        op, name, and the rest of the step's keys/values verbatim. Lightweight on
        purpose — the test runner is the source of truth for what each op means."""
        import html as html_mod

        raw = self.path[len("/api/scenarios/"):].split("?", 1)[0].strip("/")
        # Restrict to file-stem characters (no path traversal, no .json suffix expected)
        if not raw or not all(c.isalnum() or c in "-_" for c in raw):
            self.send_error(400, "Bad scenario name")
            return
        # Scenarios live in subfolders (core/, light/, …) — find by stem.
        path = test_meta.find_scenario_path(raw)
        if not path:
            self.send_error(404, f"{raw} not found")
            return
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            self.send_error(500, f"Invalid JSON in {raw}.json: {e}")
            return

        # Build a compact per-step list. Each step has at minimum an `op`; we render
        # whatever other keys it carries (id, type, parent_id, key, value, props, bounds, …)
        # as a small definition list so the schema can evolve without code change.
        scen_name = html_mod.escape(str(data.get("name", raw)))
        scen_desc = html_mod.escape(str(data.get("description", "")))
        scen_module = html_mod.escape(str(data.get("module", "")))
        scen_also = data.get("also") or []
        scen_also_html = (
            f'<div class="also">Also touches: {html_mod.escape(", ".join(scen_also))}</div>'
            if scen_also else ""
        )
        scen_module_html = (
            f'<div class="module">Module: <strong>{scen_module}</strong></div>'
            if scen_module else ""
        )
        steps = data.get("steps", []) or []

        rows = []
        for i, step in enumerate(steps):
            op = html_mod.escape(str(step.get("op", "?")))
            step_name = html_mod.escape(str(step.get("name", "")))
            step_desc = html_mod.escape(str(step.get("description", "")))
            # Render every key except op/name/description as <code>key</code> = <code>json-value</code>
            other = {k: v for k, v in step.items() if k not in ("op", "name", "description")}
            kv_html = ""
            if other:
                parts = []
                for k, v in other.items():
                    v_str = json.dumps(v) if not isinstance(v, str) else v
                    parts.append(
                        f'<div><code>{html_mod.escape(k)}</code> = '
                        f'<code>{html_mod.escape(v_str)}</code></div>'
                    )
                kv_html = '<div class="step-kv">' + "".join(parts) + "</div>"
            desc_html = f'<div class="step-desc">{step_desc}</div>' if step_desc else ""
            rows.append(
                f'<div class="step"><div class="step-head">'
                f'<span class="step-num">{i + 1}.</span> '
                f'<span class="step-op">{op}</span>'
                f'{f" <span class=\"step-name\">{step_name}</span>" if step_name else ""}'
                f'</div>{desc_html}{kv_html}</div>'
            )
        body_html = "\n".join(rows) if rows else "<p><em>(no steps)</em></p>"

        page = f"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
body {{ font-family: -apple-system, monospace; background: #0d1117; color: #c0c0c0;
       padding: 20px; line-height: 1.6; font-size: 13px; }}
h1 {{ color: #e94560; font-size: 18px; margin: 0 0 4px 0; }}
.desc {{ color: #9aa6ba; margin: 0 0 18px 0; font-size: 12px; }}
.step {{ margin: 6px 0 10px 0; padding: 6px 10px; background: #161b22;
         border-radius: 4px; border-left: 3px solid #0f3460; }}
.step-head {{ font-size: 13px; }}
.step-num {{ color: #6a7a99; }}
.step-op {{ color: #e94560; font-weight: 600; }}
.step-name {{ color: #9aa6ba; margin-left: 6px; }}
.step-desc {{ margin-top: 2px; color: #c0c0c0; }}
.step-kv {{ margin-top: 4px; padding-left: 14px; font-size: 12px; }}
.step-kv > div {{ margin: 2px 0; }}
.module {{ color: #9aa6ba; font-size: 12px; margin: 0 0 2px 0; }}
.module strong {{ color: #e94560; }}
.also {{ color: #6a7a99; font-size: 11px; margin: 0 0 12px 0; }}
code {{ background: transparent; color: #c0c0c0; padding: 0; }}
.step-kv code:first-child {{ color: #8aa6ba; }}
</style></head><body>
<h1>{scen_name}</h1>
{scen_module_html}
{f'<div class="desc">{scen_desc}</div>' if scen_desc else ''}
{scen_also_html}
{body_html}
</body></html>"""

        data_bytes = page.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data_bytes)))
        self.end_headers()
        self.wfile.write(data_bytes)

    def _serve_help(self):
        """Serve MoonDeck.md as styled HTML with deep-link anchor support."""
        md_path = SCRIPTS_DIR / "MoonDeck.md"
        if not md_path.exists():
            self.send_error(404, "MoonDeck.md not found")
            return
        raw = self.path.split("?", 1)[1] if "?" in self.path else ""
        import re as _re
        anchor = raw if _re.fullmatch(r"[A-Za-z0-9._-]+", raw) else ""
        self._serve_markdown_as_html(md_path, anchor)

    def _serve_history_report(self):
        """Serve build/history.md (generated by history_report.py) as HTML
        through the same renderer the help pages use. Iframes can't load
        file:// from an http:// parent, so we serve the file through the
        MoonDeck origin instead — same trick /api/help already uses."""
        md_path = ROOT / "build" / "history.md"
        if not md_path.exists():
            self.send_error(
                404,
                "build/history.md not found — run the History Report button first.",
            )
            return
        self._serve_markdown_as_html(md_path, "")

    def _serve_doc_asset(self):
        """Serve a static asset (image, etc.) referenced from a rendered doc.

        Path: /api/doc-asset/<ROOT-relative-path>
        The renderer resolves relative image src values to ROOT-relative paths
        before building the URL, so this handler only needs a simple join."""
        import mimetypes
        rel = self.path[len("/api/doc-asset/"):]
        # Resolve against ROOT and ensure no escape.
        try:
            asset_path = (ROOT / rel).resolve()
            ROOT.resolve()  # ensure ROOT itself is resolved
            asset_path.relative_to(ROOT.resolve())  # raises ValueError if escape
        except (ValueError, OSError):
            self.send_error(403, "Forbidden")
            return
        if not asset_path.exists() or not asset_path.is_file():
            self.send_error(404, f"Asset not found: {rel}")
            return
        mime, _ = mimetypes.guess_type(str(asset_path))
        data = asset_path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", mime or "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _serve_markdown_as_html(self, md_path, anchor):
        """Render a markdown file to HTML for the View pane. Handles
        headings (with id slugs for deep-linking), fenced code blocks,
        tables, list items, blockquotes, and the inline forms
        (**bold**, `code`, [text](url), italic-via-_underscore_).
        Deliberately not a full CommonMark renderer — just the subset the
        repo's markdown files actually use."""
        import html as html_mod
        import re

        text = md_path.read_text(encoding="utf-8")
        lines: list[str] = []
        in_code = False
        in_list = False           # top-level <ul> open?
        in_quote = False          # <blockquote> open?
        in_quote_list = False     # nested <ul> inside a blockquote open?
        in_table = False

        def render_inline(s: str) -> str:
            """Apply inline markdown to one already-HTML-escaped string.

            Order matters: code first (fenced text inside backticks must
            not get bold/italic-rendered); then links; then bold; then
            italics. Each pass uses placeholder-free regexes that are
            safe to chain because we only ever replace md tokens with
            HTML tags (no nested-markup confusion in the inputs we see)."""
            # `code` — exclude backticks themselves
            s = re.sub(r'`([^`]+)`', r'<code>\1</code>', s)
            # ![alt](url) — images (must come before link regex to avoid partial match)
            def _img_tag(m):
                alt_, src_ = m.group(1), m.group(2)
                # Resolve relative path from md file's directory to a
                # ROOT-relative path, then serve via /api/doc-asset/.
                if not src_.startswith(("http://", "https://", "/")):
                    abs_src = (md_path.parent / src_).resolve()
                    try:
                        root_rel = abs_src.relative_to(ROOT.resolve())
                        src_ = str(root_rel)
                    except ValueError:
                        pass  # outside ROOT — keep original path
                return f'<img src="/api/doc-asset/{src_}" alt="{html_mod.escape(alt_)}" style="max-width:100%;margin:4px 0;">'
            s = re.sub(r'!\[([^\]]*)\]\(([^)]+)\)', _img_tag, s)
            # [text](url) — same-origin /api/ links post a message to the
            # parent frame (iframe nav is sandboxed); external links open in
            # a new tab.
            def _link_tag(m):
                import urllib.parse as _up
                text_, url_ = m.group(1), m.group(2)
                if url_.startswith("/api/"):
                    return f'<a href="{url_}" data-moondeck-nav="1">{text_}</a>'
                scheme = _up.urlparse(url_).scheme
                if scheme not in ("", "http", "https", "mailto"):
                    return html_mod.escape(text_)  # strip unsafe schemes (e.g. javascript:)
                return f'<a href="{url_}" target="_blank" rel="noopener">{text_}</a>'
            s = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', _link_tag, s)
            # **bold**
            s = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', s)
            # _italic_ (underscore form only — asterisk-italic is ambiguous
            # next to **bold** and the repo doesn't use it)
            s = re.sub(r'(?<!\w)_([^_]+)_(?!\w)', r'<em>\1</em>', s)
            return s

        def close_list_if_open():
            nonlocal in_list
            if in_list:
                lines.append("</ul>")
                in_list = False

        def close_quote_if_open():
            nonlocal in_quote, in_quote_list
            if in_quote_list:
                lines.append("</ul>")
                in_quote_list = False
            if in_quote:
                lines.append("</blockquote>")
                in_quote = False

        def close_table_if_open():
            nonlocal in_table
            if in_table:
                lines.append("</tbody></table>")
                in_table = False

        def close_blocks():
            close_list_if_open()
            close_quote_if_open()
            close_table_if_open()

        _explicit_id_re = re.compile(r'\{#([A-Za-z0-9._-]+)\}\s*$')
        _allowed_html_re = re.compile(
            r'^</?(?:div|p|span|table|thead|tbody|tr|td|th|ul|ol|li|br|hr'
            r'|strong|em|code|pre|a|h[1-6])[\s>"/]'
        )

        def _heading_slug(text: str) -> tuple[str, str]:
            m_id = _explicit_id_re.search(text)
            if m_id:
                return m_id.group(1), text[:m_id.start()].strip()
            return text.lower().replace(" ", "_"), text

        for raw_line in text.splitlines():
            # Fenced code block toggle. Strip the optional language tag.
            stripped = raw_line.strip()
            if stripped.startswith("```"):
                close_blocks()
                if in_code:
                    lines.append("</code></pre>")
                else:
                    lines.append("<pre><code>")
                in_code = not in_code
                continue
            if in_code:
                lines.append(html_mod.escape(raw_line))
                continue

            # Tables — `| col | col |` lines + a separator row `|---|---|`.
            # We detect the table by the leading `|`; the separator row is
            # skipped (it's just markdown formatting, not data).
            if raw_line.startswith("|") and raw_line.rstrip().endswith("|"):
                close_list_if_open()
                close_quote_if_open()
                # Separator row: |---|---| (all cells are dashes / colons)
                inner = raw_line.strip().strip("|")
                cells = [c.strip() for c in inner.split("|")]
                if all(re.fullmatch(r":?-+:?", c) for c in cells):
                    continue  # skip the alignment row
                if not in_table:
                    lines.append('<table><tbody>')
                    in_table = True
                # First content row → header row (the row above the
                # separator); subsequent rows are body. We don't track
                # which is which precisely — render all as <td> and let
                # CSS handle the first-row styling.
                cell_tag = "td"
                cell_html = "".join(
                    f"<{cell_tag}>{render_inline(html_mod.escape(c))}</{cell_tag}>"
                    for c in cells
                )
                lines.append(f"<tr>{cell_html}</tr>")
                continue
            close_table_if_open()

            # Blockquote — `> text` (with optional leading whitespace from
            # nested list-item quotes like the history report's two-space
            # indent before `>`). Inside a blockquote we recognize `- foo`
            # rows as a nested `<ul>` so commit bodies with dashed-list
            # paragraphs render as real bullet lists. Non-list lines get
            # a trailing `<br>` so source-level newlines survive (the
            # browser otherwise collapses all-but-paragraph whitespace
            # and the commit body becomes one long flowing string).
            quote_match = re.match(r"^(\s*)> ?(.*)$", raw_line)
            if quote_match:
                close_list_if_open()
                if not in_quote:
                    lines.append("<blockquote>")
                    in_quote = True
                quote_content = quote_match.group(2)
                if quote_content.startswith("- "):
                    # Nested list item. Open the <ul> the first time.
                    if not in_quote_list:
                        lines.append("<ul>")
                        in_quote_list = True
                    item = quote_content[2:]
                    lines.append(f"<li>{render_inline(html_mod.escape(item))}</li>")
                    continue
                # Not a list item — close any open nested list before
                # rendering the line as flowing text.
                if in_quote_list:
                    lines.append("</ul>")
                    in_quote_list = False
                if quote_content == "":
                    lines.append("<br>")
                else:
                    lines.append(render_inline(html_mod.escape(quote_content)) + "<br>")
                continue
            # If a blockquote just ended, close any nested <ul> too.
            if in_quote and in_quote_list:
                lines.append("</ul>")
                in_quote_list = False
            close_quote_if_open()

            # Unordered list — `- text` at column 0.
            if raw_line.startswith("- "):
                if not in_list:
                    lines.append("<ul>")
                    in_list = True
                item = raw_line[2:]
                lines.append(f"<li>{render_inline(html_mod.escape(item))}</li>")
                continue
            close_list_if_open()

            # Pass-through for a fixed allowlist of structural HTML tags used
            # by history_report.py's combined graph+commits output. Narrowed
            # to known safe tags so arbitrary doc content can't inject scripts.
            stripped_check = raw_line.strip()
            if (stripped_check.startswith("<")
                    and stripped_check.endswith(">")
                    and _allowed_html_re.match(stripped_check)):
                lines.append(raw_line)
                continue

            # Headings, then blank → spacer, then plain paragraph.
            # {#explicit-id} suffix overrides the auto-slug.
            if raw_line.startswith("### "):
                slug, heading_text = _heading_slug(raw_line[4:].strip())
                lines.append(f'<h3 id="{slug}">{render_inline(html_mod.escape(heading_text))}</h3>')
            elif raw_line.startswith("## "):
                slug, heading_text = _heading_slug(raw_line[3:].strip())
                lines.append(f'<h2 id="{slug}">{render_inline(html_mod.escape(heading_text))}</h2>')
            elif raw_line.startswith("# "):
                lines.append(f'<h1>{render_inline(html_mod.escape(raw_line[2:]))}</h1>')
            elif raw_line.strip() == "":
                lines.append("<br>")
            else:
                lines.append(f"<p>{render_inline(html_mod.escape(raw_line))}</p>")

        close_blocks()
        if in_code:
            # Defensive — unbalanced fence in source shouldn't crash.
            lines.append("</code></pre>")

        body_html = "\n".join(lines)
        page = f"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
body {{ font-family: -apple-system, monospace; background: #0d1117; color: #c0c0c0;
       padding: 20px; line-height: 1.6; font-size: 13px; }}
h1 {{ color: #e94560; font-size: 18px; }}
h2 {{ color: #e94560; font-size: 15px; border-bottom: 1px solid #0f3460; padding-bottom: 4px; margin-top: 22px; }}
h3 {{ color: #e94560; font-size: 13px; margin-top: 18px; }}
pre {{ background: #161b22; padding: 10px; border-radius: 4px; overflow-x: auto;
       font-size: 11px; line-height: 1.35; }}
code {{ font-size: 12px; background: #161b22; padding: 0 4px; border-radius: 3px; }}
pre code {{ background: transparent; padding: 0; }}
p {{ margin: 2px 0; }}
ul {{ margin: 4px 0 8px 0; padding-left: 22px; }}
li {{ margin: 4px 0; }}
blockquote {{ margin: 4px 0 8px 22px; padding-left: 10px;
              border-left: 2px solid #0f3460; color: #9aa6ba; }}
table {{ border-collapse: collapse; margin: 8px 0; }}
td {{ padding: 4px 10px; border: 1px solid #0f3460; }}
tr:first-child td {{ background: #0f3460; color: #e94560; font-weight: 600; }}
a {{ color: #e94560; text-decoration: none; }}
a:hover {{ text-decoration: underline; }}
strong {{ color: #fff; }}

/* History report: combined graph + commits section. The rail is monospace
 * (matches git log --graph's ASCII characters); each commit's body
 * blockquote already has a left border that visually extends the rail's
 * vertical lines into the description. */
.hr-line {{ font-family: ui-monospace, monospace; color: #6a7a99;
            font-size: 11px; line-height: 1.4; margin: 0; }}
.hr-commit {{ margin: 6px 0; }}
.hr-head {{ font-family: ui-monospace, monospace; font-size: 12px;
            line-height: 1.4; }}
.hr-rail {{ color: #6a7a99; white-space: pre; }}
.hr-merge {{ color: #e94560; }}
.hr-date {{ color: #6a7a99; font-size: 11px; }}
.hr-commit blockquote {{ margin-left: 30px; }}
</style></head><body>
{body_html}
{f'''<script>
// Wait for images so the anchor lands at the right position. scrollIntoView
// fired before image load left the viewport on an earlier section once the
// images finished loading and pushed content down.
(function() {{
  var anchor = "{anchor}";
  function jump() {{
    var el = document.getElementById(anchor);
    if (el) el.scrollIntoView();
  }}
  var imgs = Array.from(document.images || []);
  var pending = imgs.filter(function(i) {{ return !i.complete; }});
  if (pending.length === 0) {{ jump(); return; }}
  var left = pending.length;
  pending.forEach(function(img) {{
    img.addEventListener("load",  function() {{ if (--left === 0) jump(); }});
    img.addEventListener("error", function() {{ if (--left === 0) jump(); }});
  }});
  // Safety net: never wait more than 1.5s for images.
  setTimeout(jump, 1500);
}})();
</script>''' if anchor else ''}
<script>
document.addEventListener("click", function(e) {{
    var a = e.target.closest("a[data-moondeck-nav]");
    if (!a) return;
    e.preventDefault();
    window.parent.postMessage({{type:"moondeck-nav", url: a.getAttribute("href")}}, "*");
}});
</script>
</body></html>"""

        data = page.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _serve_static(self):
        """Serve files from moondeck_ui/ and docs/assets/."""
        # Strip query string before resolving path (e.g. /?tab=pc → /)
        raw = self.path.split("?", 1)[0].lstrip("/")
        path = raw if raw else "index.html"

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
        # index.html carries a {{VERSION}} placeholder filled at serve time.
        if path == "index.html":
            data = data.replace(b"{{VERSION}}", APP_VERSION.encode())
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
    # ThreadingHTTPServer binds to "" → all interfaces, so MoonDeck is reachable
    # from other devices on the LAN, not just this machine.
    server = http.server.ThreadingHTTPServer(("", PORT), MoonDeckHandler)
    print(f"MoonDeck running at http://localhost:{PORT}")
    ip = _lan_ip()
    if ip:
        print(f"  on the network:   http://{ip}:{PORT}")
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
