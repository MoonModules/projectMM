#!/usr/bin/env python3
"""MoonDeck — browser-based developer console for projectMM."""

import http.server
import json
import os
import signal
import subprocess
import sys
import threading
from contextlib import suppress
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
# Re-use the doc generator's perf-table formatter so the MoonDeck step view
# and the generated scenario-tests.md show the same shape per step (single
# source of truth — adding/changing a metric updates both surfaces at once).
import generate_test_docs as test_doc_gen  # noqa: E402


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


def _walk_modules(modules):
    """Yield every module in the tree (depth-first), including nested children."""
    for m in modules or []:
        yield m
        yield from _walk_modules(m.get("children", []))


def _probe_device(ip, port=8080, timeout=0.4):
    """Probe a single IP for /api/state. Returns device info or None.

    Short timeout: on a LAN a live device answers in a few ms and a dead IP
    refuses the connection almost instantly; 0.4s only matters for IPs that
    silently drop packets (firewalled hosts), and a subnet scan should not
    stall seconds on those.

    Returns: { ip, deviceName, firmware, board }
    - `firmware` is the variant flashed (SystemModule.board control value —
      misnamed; renamed in the terminology-cleanup phase). Used to deduce
      `board`.
    - `board` is the physical hardware key — set when the firmware uniquely
      identifies the board (eth/eth-wifi → Olimex). Empty string otherwise;
      the user picks via the UI (the saved value survives refresh).
    """
    import urllib.request
    import urllib.error
    url = f"http://{ip}:{port}/api/state"
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read())
            modules = data.get("modules", [])
            device_name = ""
            firmware = ""
            for m in _walk_modules(modules):
                if m.get("type") == "SystemModule":
                    for c in m.get("controls", []):
                        if c.get("name") == "deviceName":
                            device_name = c.get("value", "") or ""
                        elif c.get("name") == "board":
                            firmware = c.get("value", "") or ""
                    break
            board = _deduce_board(firmware)
            return {
                "ip": f"{ip}:{port}",
                "deviceName": device_name,
                "firmware": firmware,
                "board": board,
            }
    except Exception:
        return None


def _deduce_board(firmware: str) -> str:
    """Map firmware variant → physical board key when the firmware uniquely
    identifies the board (the eth firmware variants hardcode Olimex RMII
    pins). Returns "" when the firmware works on multiple boards — the user
    fills in `board` manually in that case.

    NAMING COLLISION (transitional): "board" means *firmware variant* in
    build_esp32.py and SystemModule.board, and *physical hardware* here in
    MoonDeck device records. The rename to `firmware` everywhere is tracked
    in docs/plan.md § "Board vs firmware separation, runtime board presets"
    — do not propagate this overload to new code. New device-record sites
    should use "board" for hardware only; new build/system sites should
    avoid the word until the rename lands.

    KEEP IN SYNC: scripts/moondeck_ui/app.js boardOptions carries the human
    list of hardware boards the picker offers. When a new board lands with
    a deducible firmware, both sites need an update — collapse into a shared
    catalog at that point (small lift today; growing cost as boards add)."""
    if firmware in ("esp32-eth", "esp32-eth-wifi"):
        return "olimex-esp32-gateway-rev-g"
    return ""


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


_LAST_FLASH_FILE = SCRIPTS_DIR / ".last_flash.json"
_LAST_FLASH_TTL_S = 5 * 60  # ignore markers older than 5 minutes


def _consume_last_flash() -> dict | None:
    """Read the breadcrumb scripts/.last_flash.json that flash_esp32.py drops
    after a successful flash. Returns {port, firmware} when the marker is
    recent (< TTL); deletes the file so the link only happens once. Returns
    None when there's no recent marker."""
    if not _LAST_FLASH_FILE.exists():
        return None
    try:
        data = json.loads(_LAST_FLASH_FILE.read_text())
    except (OSError, json.JSONDecodeError):
        return None
    import time
    if time.time() - float(data.get("ts", 0)) > _LAST_FLASH_TTL_S:
        with suppress(OSError):
            _LAST_FLASH_FILE.unlink()
        return None
    return {"port": data.get("port", ""), "firmware": data.get("firmware", "")}


def refresh_devices(known_devices):
    """Probe known devices to check online/offline status.

    Preserves user-set fields (`board`, `last_port`) across refreshes: the
    probe result carries fresh `firmware`/`deviceName` and a deduced `board`
    (set only when firmware unambiguously identifies hardware), but a user-set
    `board` for a firmware that can run on multiple boards (e.g. `esp32` on
    LOLIN D32 vs generic DevKit) must survive a refresh. Same for `last_port`,
    which is set by the flash-event breadcrumb (see _consume_last_flash).
    """
    def probe(device):
        ip = device.get("ip", "")
        if ":" in ip:
            host, port = ip.rsplit(":", 1)
            fresh = _probe_device(host, int(port))
        else:
            fresh = _probe_device(ip)
        if not fresh:
            return None
        # Merge: probe wins for live-readable fields; user-set / flash-tracked
        # fields must survive. Without this, `online` (the device responded
        # → True), `selected` (user checkbox), `board` (user-set when not
        # deducible), and `last_port` (set by the flash breadcrumb) would
        # disappear from the persisted record after every refresh.
        fresh["online"] = True
        if "selected" in device:
            fresh["selected"] = device["selected"]
        if not fresh.get("board") and device.get("board"):
            fresh["board"] = device["board"]
        if device.get("last_port"):
            fresh["last_port"] = device["last_port"]
        return fresh

    if not known_devices:
        return []
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=16) as pool:
        refreshed = [r for r in pool.map(probe, known_devices) if r]

    # Link a recent flash event to the device whose firmware matches. The
    # flash flow writes scripts/.last_flash.json after a successful flash;
    # here we attribute it to a refreshed device with the same firmware
    # variant (newest "online with matching firmware" wins — usually the
    # only candidate). After linking we consume the marker so the same
    # event doesn't keep applying on every refresh.
    last_flash = _consume_last_flash()
    if last_flash and last_flash["firmware"]:
        matches = [d for d in refreshed if d.get("firmware") == last_flash["firmware"]]
        if len(matches) == 1:
            matches[0]["last_port"] = last_flash["port"]
            with suppress(OSError):
                _LAST_FLASH_FILE.unlink()
        # If 0 matches (device hasn't booted yet) or 2+ matches (ambiguous),
        # leave the marker for the next refresh to retry / re-evaluate.
    return refreshed


# ---------------------------------------------------------------------------
# State management
# ---------------------------------------------------------------------------

def load_state():
    """Load MoonDeck state. Migrates the old flat-list shape (top-level
    `devices` + `port`) to the new networks-grouped shape on first load
    after this commit ships; new-shape files load as-is. See _migrate_to_networks."""
    if STATE_FILE.exists():
        with open(STATE_FILE) as f:
            state = json.load(f)
        if "networks" not in state and ("devices" in state or "port" in state):
            state = _migrate_to_networks(state)
        return state
    return {"networks": [], "active_network": "", "tab": "pc"}


def _migrate_to_networks(old_state: dict) -> dict:
    """One-shot migration of the pre-networks moondeck.json shape:
        {env, port, devices: [...], tab, board, scenario, module, flag_*}
    into the networks-grouped shape:
        {networks: [{name, subnet, wifi, port, devices: [...]}, ...],
         active_network, tab, board, scenario, module, flag_*}

    Buckets existing devices by `/24` subnet derived from each device's `ip`.
    Names the largest bucket "Home", subsequent buckets "Network 2", "Network 3",
    ... User can rename via the dropdown. The old top-level `port` migrates
    into the bucket that holds the largest device count (heuristic — usually
    that's where the user was working). Drops the legacy `env` field
    (already migrated to `board` in app.js).
    """
    import sys
    devices = old_state.get("devices") or []
    by_subnet: dict[str, list] = {}
    for d in devices:
        ip_port = d.get("ip", "")
        host = ip_port.split(":", 1)[0]
        parts = host.split(".")
        subnet = ".".join(parts[:3]) + ".0/24" if len(parts) == 4 else "unknown"
        by_subnet.setdefault(subnet, []).append(d)

    # Largest bucket first → named "Home"; rest "Network 2", "Network 3", ...
    ordered = sorted(by_subnet.items(), key=lambda kv: -len(kv[1]))
    networks = []
    for i, (subnet, bucket) in enumerate(ordered):
        name = "Home" if i == 0 else f"Network {i + 1}"
        networks.append({
            "name": name,
            "subnet": subnet,
            "wifi": {"ssid": "", "password": ""},
            "port": "",
            "devices": bucket,
        })
    # Old top-level port → largest bucket (which is networks[0] if any).
    if networks and old_state.get("port"):
        networks[0]["port"] = old_state["port"]

    new_state = {k: v for k, v in old_state.items()
                 if k not in ("devices", "port", "env")}
    new_state["networks"] = networks
    new_state["active_network"] = networks[0]["name"] if networks else ""
    new_state.setdefault("tab", "pc")
    print(f"moondeck: migrated {len(devices)} device(s) into {len(networks)} "
          f"network(s): {', '.join(n['name'] for n in networks)}", file=sys.stderr)
    return new_state


_VOLATILE_DEVICE_FIELDS = ("deviceName", "firmware", "modules")


def save_state(state):
    """Persist MoonDeck state. Strips per-device fields that the device itself
    is the source of truth for (`deviceName`, `firmware`) — caching them
    invites stale values when the device is reflashed/renamed via another
    host. They are re-read from `/api/state` on each refresh and live only
    in the in-memory device lists until the next save. User-set fields
    (`board`, `last_port`, `selected`, `online`) persist. Iterates per network."""
    persisted = dict(state)
    networks = persisted.get("networks") or []
    if networks:
        persisted["networks"] = [_strip_network_volatiles(n) for n in networks]
    with open(STATE_FILE, "w") as f:
        json.dump(persisted, f, indent=2)


def _strip_network_volatiles(network: dict) -> dict:
    """Return a copy of a network with volatile per-device fields stripped.

    `board` is conditionally volatile: when it equals the value `_deduce_board`
    produces from the device's current firmware, the next probe will re-derive
    it for free — no need to persist. When the user picked a board manually
    (firmware doesn't deduce to anything, e.g. `esp32` could be LOLIN D32 or
    generic), the picker's choice is the only source so we must keep it.
    """
    out = dict(network)
    devs = out.get("devices") or []
    cleaned = []
    for d in devs:
        c = {k: v for k, v in d.items() if k not in _VOLATILE_DEVICE_FIELDS}
        # `board` strip: drop it when empty (noise) or when it matches the
        # firmware-deduced value (recomputable). Keep when the user picked
        # it (firmware deduces "" so the picker's value is the only source).
        firmware = d.get("firmware") or ""
        if "board" in c and (not c["board"] or c["board"] == _deduce_board(firmware)):
            del c["board"]
        cleaned.append(c)
    out["devices"] = cleaned
    return out


def _active_network(state: dict) -> dict | None:
    """Return the dict for state['active_network'] from state['networks'],
    or None when the name doesn't match or there's no active selection.
    Every consumer that previously read state['devices'] or state['port']
    routes through this helper."""
    name = state.get("active_network") or ""
    for n in state.get("networks") or []:
        if n.get("name") == name:
            return n
    return None


def _subnet_from_host_subnet(host_subnet: str) -> str:
    """Normalise `_get_local_subnet()` output (e.g. "192.168.1") to the
    network record's `subnet` field shape ("192.168.1.0/24")."""
    if not host_subnet:
        return ""
    return f"{host_subnet}.0/24"


def _auto_select_network(state: dict, host_subnet: str) -> None:
    """In-place: set state['active_network'] to whichever known network's
    subnet matches the host's current subnet — but only if the user hasn't
    pinned a different network. Pinning happens when the user changes the
    dropdown; cleared when the pinned network's subnet stops matching the
    host (next time we land on its LAN, auto-select takes over again)."""
    if not state.get("networks"):
        return
    target_subnet = _subnet_from_host_subnet(host_subnet)
    if not target_subnet:
        return
    pinned = state.get("active_network_user_pinned")
    if pinned:
        active = _active_network(state)
        if active and active.get("subnet") == target_subnet:
            return  # pinned network still matches host — leave as is
        # Pinned network no longer matches host — release the pin so the
        # next auto-select picks the right network for where we are now.
        state["active_network_user_pinned"] = False
    for n in state["networks"]:
        if n.get("subnet") == target_subnet:
            state["active_network"] = n["name"]
            return


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
# Perf-table HTML (shared shape with docs/tests/scenario-tests.md)
# ---------------------------------------------------------------------------

def _render_perf_table_html(step: dict) -> str:
    """Render a scenario step's contract+observed data as an HTML table that
    matches the markdown table emitted by test_doc_gen._format_perf_table.
    The doc generator owns the cell formatters; we just translate its pipe-
    delimited markdown to <table><tr><td>. Returns "" when the step has no
    contract/observed data."""
    import html as html_mod
    md_lines = test_doc_gen._format_perf_table(step)
    if not md_lines:
        return ""
    # Lines come in groups:
    #   header text (`**Performance** ...`)
    #   blank
    #   table header row (`| Board | FPS | ... |`)
    #   separator row (`|---|---|...`)
    #   N body rows (`| `target` | ... |`)
    #   blank
    #   optional footer lines (`- \`target\`: contract set ... · observed ...`)
    out: list[str] = []
    in_table = False
    rendered_header = False
    for line in md_lines:
        if not line.strip():
            if in_table:
                out.append("</tbody></table>")
                in_table = False
            continue
        if line.startswith("**"):
            # `**Performance** (contract / observed) — tick stored, FPS shown:`
            # → strip both the leading `**...**` bold marker (just the markup,
            # keep the bolded text inside) and the trailing `:` colon.
            import re as _re
            txt = _re.sub(r"\*\*(.+?)\*\*", r"\1", line.strip()).rstrip(":").strip()
            out.append(f'<div class="perf-head"><strong>{html_mod.escape(txt)}</strong></div>')
            continue
        if line.startswith("|") and "---" in line:
            continue  # markdown separator row
        if line.startswith("|"):
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if not in_table:
                out.append('<table class="perf-table"><tbody>')
                in_table = True
            tag = "th" if not rendered_header else "td"
            rendered_header = True
            row = "".join(
                f"<{tag}>{_inline_code_html(html_mod.escape(c))}</{tag}>"
                for c in cells
            )
            out.append(f"<tr>{row}</tr>")
            continue
        if line.lstrip().startswith("-"):
            # Audit footer line.
            txt = line.lstrip().lstrip("-").strip()
            out.append(f'<div class="perf-audit">{_inline_code_html(html_mod.escape(txt))}</div>')
            continue
    if in_table:
        out.append("</tbody></table>")
    return '<div class="perf">' + "".join(out) + '</div>'


def _inline_code_html(s: str) -> str:
    """Tiny markdown-inline-code → <code> translator. The perf table cells
    contain `target` and `field` names wrapped in backticks; render as <code>.
    Doesn't try to be a full markdown parser — just the patterns the perf
    formatter produces."""
    import re as _re
    return _re.sub(r"`([^`]+)`", r"<code>\1</code>", s)


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------

class MoonDeckHandler(http.server.BaseHTTPRequestHandler):

    def log_message(self, format, *args):
        # Suppress default request logging
        pass

    def handle(self):
        # Browser closing the connection is harmless; suppress the noise.
        with suppress(ConnectionResetError, BrokenPipeError):
            super().handle()

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
            # Auto-select the network matching the host's current subnet
            # (unless the user has pinned a different one — see
            # _auto_select_network). Persist the selection back so the next
            # load is stable when the host's subnet hasn't changed.
            state = load_state()
            before = state.get("active_network")
            _auto_select_network(state, _get_local_subnet())
            if state.get("active_network") != before:
                save_state(state)
            self._send_json(state)

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
            # Attribute found devices to the network whose subnet matches the
            # scanned one. Creates a new "Network N" if none matches (a future
            # rename in the UI can adjust the name). Returns the full updated
            # state so the JS reloads the authoritative shape.
            target_subnet = _subnet_from_host_subnet(scanned_subnet)
            state = load_state()
            net = next((n for n in (state.get("networks") or [])
                        if n.get("subnet") == target_subnet), None)
            if net is None and devices:
                existing = state.setdefault("networks", [])
                name = "Home" if not existing else f"Network {len(existing) + 1}"
                net = {"name": name, "subnet": target_subnet,
                       "wifi": {"ssid": "", "password": ""},
                       "port": "", "devices": []}
                existing.append(net)
            if net is not None:
                # Merge found devices into the network: keep existing user
                # fields (board, last_port, selected), update online + probe
                # fields. Drop devices no longer reachable — they stay
                # offline elsewhere if previously known, here we trust the
                # fresh scan as authoritative for what's on this subnet.
                by_ip = {d["ip"]: d for d in net.get("devices", [])}
                merged = []
                for fresh in devices:
                    ip = fresh.get("ip", "")
                    keep = by_ip.get(ip, {})
                    out = {**fresh, "online": True,
                           "selected": keep.get("selected", False)}
                    if keep.get("board") and not out.get("board"):
                        out["board"] = keep["board"]
                    if keep.get("last_port"):
                        out["last_port"] = keep["last_port"]
                    merged.append(out)
                # Devices that existed previously but weren't found in the
                # scan stay in the network as offline (the user may want to
                # keep them around for when the device comes back). Discover
                # is additive — refresh is the verb that prunes.
                found_ips = {d.get("ip") for d in devices}
                for ip, dev in by_ip.items():
                    if ip not in found_ips:
                        merged.append({**dev, "online": False})
                net["devices"] = merged
                save_state(state)
            self._send_json(state)

        elif self.path == "/api/refresh":
            body = self._read_body()
            params = json.loads(body) if body else {}
            network_name = params.get("network", "")
            state = load_state()
            net = next((n for n in (state.get("networks") or [])
                        if n.get("name") == network_name), None)
            if net is None:
                # No-op when the named network doesn't exist (e.g. it was
                # renamed mid-flight). Return state so the JS can re-sync.
                self._send_json(state)
                return
            refreshed = refresh_devices(net.get("devices") or [])
            # refresh_devices returns only devices that responded — devices
            # marked offline (didn't respond) are dropped from the list it
            # returns. Carry them forward as offline so the UI doesn't lose
            # known-but-unreachable entries.
            refreshed_ips = {d.get("ip") for d in refreshed}
            for prior in (net.get("devices") or []):
                if prior.get("ip") not in refreshed_ips:
                    refreshed.append({**prior, "online": False})
            net["devices"] = refreshed
            save_state(state)
            self._send_json(state)

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
            # `contract` and `observed` are the per-target performance data
            # and render as a single shared table (same shape as
            # docs/tests/scenario-tests.md — see test_doc_gen._format_perf_table).
            # Everything else stays in the JSON-dump key/value list below.
            perf_html = _render_perf_table_html(step)
            other = {k: v for k, v in step.items()
                     if k not in ("op", "name", "description", "contract", "observed")}
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
                f'</div>{desc_html}{kv_html}{perf_html}</div>'
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
/* Perf table — same shape as docs/tests/scenario-tests.md per-step table */
.perf {{ margin-top: 6px; }}
.perf-head {{ font-size: 12px; color: #9aa6ba; margin: 4px 0 2px 0; }}
.perf-table {{ border-collapse: collapse; font-size: 12px; margin: 2px 0; }}
.perf-table th, .perf-table td {{ padding: 2px 8px; text-align: left;
                                   border-bottom: 1px solid #1c2535; }}
.perf-table th {{ color: #8aa6ba; font-weight: 500; }}
.perf-audit {{ font-size: 11px; color: #6a7a99; margin: 2px 0 0 8px; }}
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
            # a new tab. Relative `.md` links are rewritten to /api/docs/<path>
            # so the rendered page stays navigable when served through MoonDeck
            # (the docs are also valid when read straight from the repo: same
            # paths, different host).
            def _link_tag(m):
                import urllib.parse as _up
                text_, url_ = m.group(1), m.group(2)
                if url_.startswith("/api/"):
                    return f'<a href="{url_}" data-moondeck-nav="1">{text_}</a>'
                # Relative .md link (with optional #anchor) → resolve against the
                # current file's directory, re-anchor under docs/, serve via /api/docs/.
                parsed = _up.urlparse(url_)
                if (not parsed.scheme and not url_.startswith("/")
                        and parsed.path.endswith(".md")):
                    try:
                        abs_md = (md_path.parent / parsed.path).resolve()
                        rel = abs_md.relative_to((ROOT / "docs").resolve())
                        api_url = "/api/docs/" + str(rel)
                        if parsed.fragment:
                            api_url += "?" + parsed.fragment
                        return f'<a href="{api_url}" data-moondeck-nav="1">{text_}</a>'
                    except ValueError:
                        pass  # outside docs/ — fall through to default handling
                scheme = parsed.scheme
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
