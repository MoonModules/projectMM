#!/usr/bin/env python3
"""MoonDeck — browser-based developer console for projectMM."""

import http.server
import json
import os
import signal
import subprocess
import sys
import tempfile
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
        return json.loads((ROOT / "library.json").read_text(encoding="utf-8")).get("version", "?")
    except Exception:
        return "?"


APP_VERSION = _app_version()

# ---------------------------------------------------------------------------
# Boards catalog (single source of truth, shared with the web installer)
# ---------------------------------------------------------------------------

BOARDS_FILE = ROOT / "docs" / "install" / "deviceModels.json"


def _load_boards():
    """Load docs/install/deviceModels.json. Returns [] on missing/malformed file —
    `_deduce_board` then always returns "" (no firmware uniquely identifies
    a board), MoonDeck JS shows only the empty default. The web installer
    Step 2 picker will share this file.
    """
    try:
        return json.loads(BOARDS_FILE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []


BOARDS = _load_boards()

FIRMWARES_FILE = ROOT / "docs" / "install" / "firmwares.json"


def _load_firmwares():
    """Shipping firmware-variant names from docs/install/firmwares.json — the
    generated projection of build_esp32's FIRMWARES dict (the single source of
    truth, shared with the CI release matrix). Returns [] on missing/malformed
    file, so the MoonDeck UI just shows no firmware entries. Filtering on
    `ships` keeps held-out variants (e.g. esp32p4-eth-wifi) out of the picker.
    """
    try:
        doc = json.loads(FIRMWARES_FILE.read_text(encoding="utf-8"))
        return [f["name"] for f in doc["firmwares"] if f.get("ships")]
    except (OSError, json.JSONDecodeError, KeyError):
        return []


# ---------------------------------------------------------------------------
# Script definitions (loaded from scripts.json)
# ---------------------------------------------------------------------------

SCRIPTS_FILE = SCRIPTS_DIR / "moondeck_config.json"

def load_scripts():
    with open(SCRIPTS_FILE) as f:
        return json.load(f)

_scripts_data = load_scripts()
SCRIPTS = _scripts_data["scripts"]
FIRMWARES = _load_firmwares()

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
    - `firmware` is the variant flashed (value of the `firmware` control on
      SystemModule, set from kFirmwareName in build_info.h). Used to deduce
      `board` when the device hasn't been told its board yet. See
      docs/architecture.md § Firmware vs board.
    - `board` is the physical hardware key. Preferred source: the device's
      own `deviceModel` control on SystemModule (the value MoonDeck pushed earlier
      and the device persisted). Fall back to firmware-based deduction
      (catalog lookup) when the device hasn't been told yet — then MoonDeck
      pushes the deduced value on next discover, the device persists it,
      and subsequent probes read it back from the device.
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
            device_board = ""
            for m in _walk_modules(modules):
                # deviceName, firmware AND deviceModel all live on SystemModule now
                # (the deviceModel identity was folded in from the former BoardModule).
                if m.get("type") == "SystemModule":
                    for c in m.get("controls", []):
                        if c.get("name") == "deviceName":
                            device_name = c.get("value", "") or ""
                        elif c.get("name") == "firmware":
                            firmware = c.get("value", "") or ""
                        elif c.get("name") == "deviceModel":
                            device_board = c.get("value", "") or ""
            return {
                "ip": f"{ip}:{port}",
                "deviceName": device_name,
                "firmware": firmware,
                "board": device_board or _deduce_board(firmware),
            }
    except Exception:
        return None


def _deduce_board(firmware: str) -> str:
    """Firmware → board name when exactly one catalog entry claims this
    firmware. Returns "" when zero (unknown firmware) or multiple boards
    claim it (ambiguous — user picks). Catalog lives at
    docs/install/deviceModels.json; see docs/architecture.md § Firmware vs board.
    """
    if not firmware:
        return ""
    matches = [b["name"] for b in BOARDS if firmware in b.get("firmwares", [])]
    return matches[0] if len(matches) == 1 else ""


def _push_board_to_device(ip: str, board: str) -> bool:
    """POST /api/control on the device for every per-board control in deviceModels.json.

    For boards that have a catalog entry in docs/install/deviceModels.json: fans
    out the full `controls.<Module>.<control>` block (matching the web
    installer's and the device-side `?deviceModel=` Inject path — same generic
    iteration, so adding a new field to a board entry Just Works without
    code changes here). For boards without a catalog entry (custom names,
    unknown firmware): still pushes `System.deviceModel` so the bare name lands —
    keeps the legacy single-field behaviour as the fallback.

    Returns True iff EVERY POST returned 200. False on any failure (timeout,
    non-2xx, network error) — partial state may have been applied; the next
    refresh re-attempts. Same best-effort semantics as the prior single-
    field shape.

    `ip` is the "host:port" string from the device record (already includes
    the port discovery picked). `board` is the catalog key MoonDeck wants
    the device to remember (empty string means "clear" — no push).
    """
    if not board:
        return True   # nothing to push; not a failure
    import urllib.request
    import urllib.error
    # Look up the catalog entry. BOARDS is loaded at module init; we don't
    # re-read deviceModels.json per push so a tight discover-refresh cycle
    # doesn't hammer the disk. If the user edits deviceModels.json, restart
    # MoonDeck (same as every other catalog change).
    entry = next((b for b in BOARDS if b.get("name") == board), None)
    if entry is not None:
        modules = entry.get("modules") or []
    else:
        # Custom / unknown deviceModel: push the bare name onto System's `deviceModel`
        # control (the identity lives on SystemModule now — no parent_id, it's the
        # boot-wired top-level module, so _apply just sets the control, no add).
        modules = [{"type": "System", "id": "System",
                    "controls": {"deviceModel": board}}]

    return _apply_modules_to_device(ip, modules)


def _apply_modules_to_device(ip: str, modules: list) -> bool:
    """Add-then-configure a list of module-with-controls units on a device.

    Each unit is `{type, id, parent_id?, controls?}` — the SAME shape deviceModels.json
    catalog entries use and a saved device-profile stores, so both the board push
    (_push_board_to_device) and a profile restore share this one fan-out. Per
    module: add it first when it has a parent_id (a fresh flash has no user-added
    modules like AudioModule, so a control write would 404), then set its controls.
    A module without parent_id is boot-wired/top-level (Board under System,
    Network) that already exists — skip the add, just set controls. The add is
    idempotent (an existing id returns 200). Returns True iff EVERY POST returned
    200; best-effort (partial state may apply, the next refresh re-attempts).
    """
    import urllib.request
    import urllib.error

    def _post(path: str, body_obj: dict) -> bool:
        body = json.dumps(body_obj).encode()
        try:
            req = urllib.request.Request(
                f"http://{ip}{path}",
                data=body, method="POST",
                headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(req, timeout=0.6) as resp:
                return resp.status == 200
        except (urllib.error.URLError, OSError):
            return False

    for m in modules:
        if not isinstance(m, dict):
            continue
        if m.get("parent_id") and m.get("type"):
            if not _post("/api/modules", {
                "type": m.get("type"),
                "id": m.get("id"),
                "parent_id": m.get("parent_id"),
            }):
                return False
        ctrls = m.get("controls") or {}
        for control_name, value in ctrls.items():
            if not _post("/api/control", {
                "module": m.get("id"),
                "control": control_name,
                "value": value,
            }):
                return False
    return True


# Module types whose controls a device-profile captures: the drivers + the
# system/network/audio config a user sets by hand. Effects/layouts/layers are
# animation state, not the device's physical pin wiring, so they're left out —
# a profile is "the GPIO/peripheral setup", re-applied after a reflash wipes it.
# SystemModule carries the device identity (deviceName + deviceModel, the latter
# folded in from the former BoardModule); its read-only telemetry controls are
# dropped by the _NON_REPLAYABLE_CONTROL_TYPES filter, leaving just the Text config.
_PROFILE_MODULE_TYPES = {
    "SystemModule", "NetworkModule", "AudioModule",
    "RmtLedDriver", "LcdLedDriver", "ParlioLedDriver", "NetworkSendDriver",
}

# Control types that must NOT go into a captured profile: password values are
# XOR-obfuscated in /api/state (re-pushing double-encodes them), and display /
# display-int / progress are device-derived read-outs. These mirror the device's
# own isPersistable() exclusions (src/core/Control.cpp); strings are the JSON
# `type` values from controlTypeName().
_NON_REPLAYABLE_CONTROL_TYPES = {"password", "display", "display-int", "progress"}


def _capture_device_profile(ip: str) -> "list | None":
    """Read /api/state and flatten the module tree into profile units.

    Returns a list of `{type, id, parent_id?, controls}` units (the same shape
    _apply_modules_to_device + deviceModels.json use), or None if the device is
    unreachable. Only the config-bearing module types in _PROFILE_MODULE_TYPES are
    captured (the physical pin/peripheral setup), and each module's controls list
    `[{name, value}]` is collapsed to a `{name: value}` dict. parent_id comes from
    the tree position so restore re-creates user-added modules under the right
    container. The catalog `type` is the short id the device reports as the module
    type; we keep it verbatim (e.g. "RmtLedDriver"), matching deviceModels.json.
    """
    import urllib.request
    import urllib.error
    host = ip.split(":")[0]
    port = ip.split(":")[1] if ":" in ip else "8080"
    try:
        with urllib.request.urlopen(f"http://{host}:{port}/api/state", timeout=1.0) as resp:
            state = json.loads(resp.read())
    except (urllib.error.URLError, OSError, json.JSONDecodeError):
        return None

    units: list = []

    def _collect(modules, parent_id) -> None:
        for m in modules or []:
            mtype = m.get("type")
            mid = m.get("name")   # /api/state reports the instance id under "name"
            if mtype in _PROFILE_MODULE_TYPES and mid:
                controls = {}
                for c in m.get("controls", []):
                    cn = c.get("name")
                    # Skip non-replayable control types: password values are
                    # XOR-obfuscated in /api/state (replaying them would double-
                    # encode), and display / display-int / progress are device-
                    # derived read-outs the device overwrites every tick. These are
                    # exactly the types the device's own isPersistable() (Control.cpp)
                    # refuses to save — mirror that here so a profile only carries
                    # writable config. Type strings come from controlTypeName().
                    if c.get("type") in _NON_REPLAYABLE_CONTROL_TYPES:
                        continue
                    if cn is not None and "value" in c:
                        controls[cn] = c["value"]
                unit = {"type": mtype, "id": mid}
                if parent_id:
                    unit["parent_id"] = parent_id
                if controls:
                    unit["controls"] = controls
                units.append(unit)
            # recurse with THIS module's id as the parent for its children
            _collect(m.get("children", []), m.get("name"))

    _collect(state.get("modules", []), None)
    return units


def _push_boards_in_parallel(pushes):
    """Fire _push_board_to_device for each (ip, board) tuple in parallel.

    Discovery + refresh probe a /24, so the push count is bounded by the
    device count (single digits in practice). A small thread pool keeps
    total latency near the slowest single push instead of summing. Result
    is fire-and-forget: callers don't act on the bool returned by each
    push — failures are recoverable on the next refresh cycle.
    """
    if not pushes:
        return
    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        # list() forces all futures to start; the with-block waits for them.
        list(pool.map(lambda p: _push_board_to_device(*p), pushes))


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
        data = json.loads(_LAST_FLASH_FILE.read_text(encoding="utf-8"))
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
        {env, port, devices: [...], tab, firmware, scenario, module, flag_*}
    into the networks-grouped shape:
        {networks: [{name, subnet, wifi, port, devices: [...]}, ...],
         active_network, tab, firmware, scenario, module, flag_*}

    Buckets existing devices by `/24` subnet derived from each device's `ip`.
    Names the largest bucket "Home", subsequent buckets "Network 2", "Network 3",
    ... User can rename via the dropdown. The old top-level `port` migrates
    into the bucket that holds the largest device count (heuristic — usually
    that's where the user was working). Drops the legacy `env` field
    (already migrated to `firmware` in app.js).
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


# `modules` is the full module tree from /api/state — kilobytes per device, no
# UI consumer between probes; strip on save to keep moondeck.json small.
# `deviceName` and `firmware` ARE displayed in the device row label, so keep
# them persisted: stripping made the row show only an IP after every server
# restart until the user clicked Discover. Both fields are correctly
# overwritten by the next probe (no staleness drift problem).
_VOLATILE_DEVICE_FIELDS = ("modules",)


# Serializes the full load → mutate → save transaction across the threaded
# HTTP handlers (ThreadingHTTPServer dispatches each request on its own
# thread). Without this, two concurrent /api/discover requests could both
# load_state(), mutate their own copies, and each save_state() — last write
# wins, half the work is lost. RLock (not Lock) so save_state can also be
# called standalone for the no-mutator path (POST /api/state body merge)
# without deadlocking when nested inside mutate_state.
_state_write_lock = threading.RLock()


def mutate_state(mutator):
    """Run a full load → mutator(state) → save cycle under the state lock.
    Returns the post-mutation state so the handler can echo it to the
    client. `mutator` receives the loaded state dict, mutates in place
    (or returns a new dict, which becomes the value to save), and may
    return None to mean "keep the in-place mutation."

    Slow work (subnet scans, device probes) should happen BEFORE calling
    mutate_state — pass already-gathered data in by closure. Holding the
    lock across network I/O would serialise everything behind the slowest
    scan."""
    with _state_write_lock:
        state = load_state()
        result = mutator(state)
        if result is not None:
            state = result
        save_state(state)
        return state


def save_state(state):
    """Persist MoonDeck state. Strips per-device fields that the device itself
    is the source of truth for (`deviceName`, `firmware`) — caching them
    invites stale values when the device is reflashed/renamed via another
    host. They are re-read from `/api/state` on each refresh and live only
    in the in-memory device lists until the next save. User-set fields
    (`board`, `last_port`, `selected`, `online`) persist. Iterates per network.

    Write is atomic + serialized: a temp file in the same dir → fsync → rename.
    The rename is atomic on POSIX (same filesystem); fsync makes the bytes
    durable before the swap so a crash mid-write never leaves a half-written
    moondeck.json (the previous version stays intact). The lock ensures two
    handler threads don't race on the temp file or the rename."""
    persisted = dict(state)
    networks = persisted.get("networks") or []
    if networks:
        persisted["networks"] = [_strip_network_volatiles(n) for n in networks]
    data = json.dumps(persisted, indent=2)
    with _state_write_lock:
        # NamedTemporaryFile in the same dir so os.replace stays on one
        # filesystem (cross-FS rename is not atomic). delete=False because
        # we hand the path to os.replace ourselves.
        tmp = tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8",
            dir=str(STATE_FILE.parent),
            prefix=STATE_FILE.name + ".",
            suffix=".tmp",
            delete=False,
        )
        try:
            tmp.write(data)
            tmp.flush()
            os.fsync(tmp.fileno())
            tmp.close()
            os.replace(tmp.name, STATE_FILE)
        except Exception:
            # On failure, drop the stray temp file so we don't accumulate
            # .tmp leftovers across crashes. Re-raise so the caller sees it.
            with suppress(OSError):
                os.unlink(tmp.name)
            raise


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
    """List available serial ports.

    POSIX hosts: glob the conventional /dev/tty* device files (no deps).
    Windows: read HKLM\\HARDWARE\\DEVICEMAP\\SERIALCOMM via winreg (stdlib).
    SERIALCOMM is the authoritative table the OS itself maintains for
    present COM ports — what pyserial reads under the hood — so a registry
    walk is both correct and dependency-free. The previous brute-force
    COM0..COM255 open-and-close loop required pyserial, which MoonDeck did
    not declare, so on Windows the list silently came back empty.
    """
    ports: list[str] = []
    import glob
    ports.extend(glob.glob("/dev/tty.usb*"))
    ports.extend(glob.glob("/dev/ttyUSB*"))
    ports.extend(glob.glob("/dev/ttyACM*"))
    if sys.platform == "win32":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                                r"HARDWARE\DEVICEMAP\SERIALCOMM") as key:
                i = 0
                while True:
                    try:
                        _, port, _ = winreg.EnumValue(key, i)
                        ports.append(port)
                        i += 1
                    except OSError:
                        break
        except FileNotFoundError:
            pass  # no SERIALCOMM key — no ports present
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
            self._send_json({"scripts": SCRIPTS, "firmwares": FIRMWARES})

        elif self.path == "/api/ports":
            self._send_json({"ports": list_serial_ports()})

        elif self.path == "/api/scenarios":
            self._send_json({"scenarios": self._list_scenarios()})

        elif self.path.startswith("/api/scenarios/"):
            self._serve_scenario_steps()

        elif self.path == "/api/test-modules":
            self._send_json({"modules": test_meta.list_test_modules()})

        elif self.path == "/api/boards":
            # Serves docs/install/deviceModels.json (loaded at startup). The web
            # installer (Step 2) will fetch the same file directly from
            # Pages; MoonDeck reads it locally and exposes it here so the
            # JS UI shares one source of truth with the Python deduce path.
            self._send_json({"boards": BOARDS})

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
            patch = json.loads(body) if body else {}
            # mutate_state holds the lock across load + merge + save so two
            # concurrent POSTs can't each load the same snapshot, apply
            # different patches, and clobber each other on save.
            def _merge(s):
                s.update(patch)
            result = mutate_state(_merge)
            self._send_json(result)

        elif self.path == "/api/push-board":
            # Push a single (ip, board) to a device. Called by the JS when the
            # user picks a board from the per-device dropdown — saveState
            # alone persists the value in moondeck.json but the device also
            # needs to hear about it (the device persists its `deviceModel` control,
            # now on SystemModule, to /.config/SystemModule.json). The bulk push from discover /
            # refresh covers the multi-device case; this covers the
            # one-device-at-a-time UI mutation.
            body = self._read_body()
            params = json.loads(body) if body else {}
            ip = params.get("ip", "")
            board = params.get("board", "")
            if not ip:
                self._send_json({"error": "ip required"}, 400)
                return
            ok = _push_board_to_device(ip, board)
            self._send_json({"ok": ok})

        elif self.path == "/api/save-profile":
            # Capture a device's current pin/peripheral config (drivers, board,
            # network, audio) and store it as a named profile under the device
            # record in moondeck.json. Lets a user re-apply the GPIO setup after a
            # reflash wipes config, or clone it to a second identical rig.
            body = self._read_body()
            params = json.loads(body) if body else {}
            ip = params.get("ip", "")
            name = (params.get("name") or "").strip()
            if not ip or not name:
                self._send_json({"error": "ip and name required"}, 400)
                return
            modules = _capture_device_profile(ip)
            if modules is None:
                self._send_json({"error": "device unreachable"}, 502)
                return
            # Store under the device whose ip matches, in the ACTIVE network only —
            # scoping to one network avoids a collision when two networks have
            # overlapping private subnets (both a 192.168.1.x device).
            def _store(state) -> None:
                net = _active_network(state)
                if not net:
                    return
                for d in net.get("devices") or []:
                    if d.get("ip") == ip:
                        profiles = [p for p in d.get("profiles", [])
                                    if p.get("name") != name]   # replace same-named
                        profiles.append({"name": name, "modules": modules})
                        d["profiles"] = profiles
            mutate_state(_store)
            self._send_json({"ok": True, "moduleCount": len(modules)})

        elif self.path == "/api/apply-profile":
            # Re-apply a stored profile to a device (same or a second identical
            # board) via the shared add-then-configure fan-out.
            body = self._read_body()
            params = json.loads(body) if body else {}
            ip = params.get("ip", "")
            name = (params.get("name") or "").strip()
            if not ip or not name:
                self._send_json({"error": "ip and name required"}, 400)
                return
            # Scope the profile lookup to the device the UI applied it from, in the
            # ACTIVE network only (its profile dropdown lists that device's own
            # profiles). Matching by name across every device — or across networks
            # with overlapping subnets — would restore the wrong pin map when two
            # devices share a profile name like "default".
            modules = None
            net = _active_network(load_state())
            for d in (net.get("devices") or []) if net else []:
                if d.get("ip") != ip:
                    continue
                for p in d.get("profiles", []):
                    if p.get("name") == name:
                        modules = p.get("modules")
            if modules is None:
                self._send_json({"error": "profile not found"}, 404)
                return
            ok = _apply_modules_to_device(ip, modules)
            self._send_json({"ok": ok})

        elif self.path == "/api/discover":
            body = self._read_body()
            params = json.loads(body) if body else {}
            subnet = params.get("subnet", "")
            # Slow part — subnet scan — happens OUTSIDE the state lock so
            # parallel discovers on different subnets don't serialise behind
            # each other. The merge into the active network record happens
            # under the lock via mutate_state.
            devices, scanned_subnet = discover_devices(subnet)
            target_subnet = _subnet_from_host_subnet(scanned_subnet)
            pushes = []   # (ip, board) tuples populated by _merge_discover

            def _merge_discover(state):
                # Attribute found devices to the network whose subnet matches
                # the scanned one. Creates a new "Network N" if none matches
                # (a future rename in the UI can adjust the name). Returns
                # the full updated state so the JS reloads the authoritative
                # shape.
                net = next((n for n in (state.get("networks") or [])
                            if n.get("subnet") == target_subnet), None)
                if net is None and devices:
                    existing = state.setdefault("networks", [])
                    name = "Home" if not existing else f"Network {len(existing) + 1}"
                    net = {"name": name, "subnet": target_subnet,
                           "wifi": {"ssid": "", "password": ""},
                           "port": "", "devices": []}
                    existing.append(net)
                if net is None:
                    return  # nothing to merge — state unchanged
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
                    # The device's `board` (from probe) is what's persisted
                    # device-side. If the merged value differs — typically
                    # because MoonDeck just deduced one from firmware on a
                    # device that hadn't been told yet — schedule a push so
                    # the next probe reads the value back from the device.
                    device_board = (fresh.get("board") or "")
                    merged_board = (out.get("board") or "")
                    if merged_board and merged_board != device_board:
                        pushes.append((ip, merged_board))
                # Devices that existed previously but weren't found in the
                # scan stay in the network as offline (the user may want to
                # keep them around for when the device comes back). Discover
                # is additive — refresh is the verb that prunes.
                found_ips = {d.get("ip") for d in devices}
                for ip, dev in by_ip.items():
                    if ip not in found_ips:
                        merged.append({**dev, "online": False})
                net["devices"] = merged

            result = mutate_state(_merge_discover)
            # Fire pushes outside the lock — the state write has already
            # landed; pushes are best-effort device-side mirroring.
            _push_boards_in_parallel(pushes)
            self._send_json(result)

        elif self.path == "/api/refresh":
            body = self._read_body()
            params = json.loads(body) if body else {}
            network_name = params.get("network", "")
            # Read the device list snapshot under the lock, release, do the
            # slow probes outside, then re-enter mutate_state for the merge.
            # Holding the lock across the probes would serialise every refresh.
            with _state_write_lock:
                state = load_state()
                net = next((n for n in (state.get("networks") or [])
                            if n.get("name") == network_name), None)
                snapshot = list(net.get("devices") or []) if net else None
            if snapshot is None:
                # No-op when the named network doesn't exist (e.g. it was
                # renamed mid-flight). Return state so the JS can re-sync.
                self._send_json(state)
                return
            refreshed = refresh_devices(snapshot)
            pushes = []   # (ip, board) tuples populated by _merge_refresh

            def _merge_refresh(state):
                # Re-resolve `net` under the second lock — the network may have
                # been renamed / re-added by another handler while the probes
                # ran. If it's gone, drop the refresh result (the user will
                # see the empty list and re-discover).
                target = next((n for n in (state.get("networks") or [])
                               if n.get("name") == network_name), None)
                if target is None:
                    return
                # refresh_devices returns only devices that responded — devices
                # marked offline (didn't respond) are dropped from the list it
                # returns. Carry them forward as offline so the UI doesn't lose
                # known-but-unreachable entries.
                refreshed_ips = {d.get("ip") for d in refreshed}
                merged = list(refreshed)
                for prior in (target.get("devices") or []):
                    if prior.get("ip") not in refreshed_ips:
                        merged.append({**prior, "online": False})
                target["devices"] = merged
                # Schedule a board push for every online device with a
                # non-empty board. Redundant writes are cheap on the device
                # (Text-control write hits a 2s debounce — repeated identical
                # writes coalesce into one disk write). Catches the case
                # where the device lost its persisted value but MoonDeck
                # still has the user-set / deduced one.
                for dev in merged:
                    if dev.get("online") and dev.get("board") and dev.get("ip"):
                        pushes.append((dev["ip"], dev["board"]))

            result = mutate_state(_merge_refresh)
            _push_boards_in_parallel(pushes)
            self._send_json(result)

        else:
            self.send_error(404)

    def _handle_run(self, script_id: str, params: dict):
        """Start a script and return immediately. Client uses SSE to stream."""
        script_def = next((s for s in SCRIPTS if s["id"] == script_id), None)
        if not script_def:
            self._send_json({"error": "unknown script"}, 404)
            return

        # Refuse to launch with a required selector unset: the underlying
        # script declares the arg `required=True`, so running it bare just
        # leaks argparse's "the following arguments are required" usage error
        # into the log. Surface a clear message naming what to pick instead.
        REQUIRED = [("needs_port", "port", "a serial port"),
                    ("needs_firmware", "firmware", "a firmware variant"),
                    ("needs_scenario", "scenario", "a scenario"),
                    ("needs_module", "module", "a module")]
        missing = [label for flag, key, label in REQUIRED
                   if script_def.get(flag) and not params.get(key)]
        if missing:
            self._send_json({"error": f"Select {' and '.join(missing)} first."}, 400)
            return

        kill_script(script_id)  # Kill previous if still running

        script_path = SCRIPTS_DIR / script_def["script"]
        cmd = ["uv", "run", str(script_path)]

        # Forward selector state (firmware / port / host) when the script
        # declares it needs them. The UI maintains a single Firmware dropdown
        # on the ESP32 tab driving every needs_firmware script; the older
        # per-firmware buttons + extra_args plumbing was collapsed into this.
        if script_def.get("needs_firmware") and params.get("firmware"):
            cmd.extend(["--firmware", params["firmware"]])
        if script_def.get("needs_port") and params.get("port"):
            cmd.extend(["--port", params["port"]])
        if script_def.get("needs_scenario") and params.get("scenario"):
            cmd.extend(["--name", params["scenario"]])
        if script_def.get("needs_module") and params.get("module"):
            cmd.extend(["--module", params["module"]])
        # pass_board: forward the board picked in the UI's provisioning
        # dropdown (state.provisionBoard) so improv_provision.py injects that
        # board's TX-power cap BEFORE provisioning (the weak-power brown-out fix).
        # No firmware-deduce fallback: the only pass_board script
        # (improv_provision) doesn't declare needs_firmware, so params never
        # carries a firmware to deduce from — the dropdown is the sole source.
        if script_def.get("pass_board"):
            board = params.get("board")
            if board:
                cmd.extend(["--board", board])
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
        from urllib.parse import unquote
        # URL-decode the path: a doc image with a space in its name is written `Hue%20driver.png` in
        # the markdown, so the request path carries `%20`; without decoding, the file lookup would seek
        # a literal "%20" in the name and 404.
        rel = unquote(self.path[len("/api/doc-asset/"):])
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

        def _render_cell(c: str) -> str:
            """Render one table cell. The compact module pages (effects/modifiers/layouts)
            use raw <img src=… width=…> previews and <a id=…></a> row anchors inside cells —
            two tags GitHub/VS Code honor but the default escape path would turn to literal
            text. Pass those two through (resolving an <img> src to /api/doc-asset/ like the
            markdown-image path does); escape + inline-render the rest."""
            def _img_attrs(tag: str) -> dict:
                """Pull src/width/alt/style from an <img …> tag regardless of attribute ORDER — each
                is matched by its own name="value" search, so an author may write them in any sequence
                (the previous fixed src→width→alt→style regex silently dropped out-of-order attrs)."""
                def attr(name):
                    m = re.search(rf'\b{name}="([^"]*)"', tag)
                    return m.group(1) if m else None
                return {k: attr(k) for k in ("src", "width", "alt", "style")}
            def _img(a):
                src_ = a.get("src") or ""
                width = a.get("width")
                style = a.get("style")
                alt_ = a.get("alt")
                if not src_.startswith(("http://", "https://", "/")):
                    abs_src = (md_path.parent / src_).resolve()
                    try:
                        src_ = str(abs_src.relative_to(ROOT.resolve()))
                    except ValueError:
                        pass
                    src_ = f"/api/doc-asset/{src_}"
                # Escape every attribute value that reaches the HTML — src/width/style as well as alt —
                # so a doc-page value with a quote or angle bracket can't break the tag or inject markup.
                wattr = f' width="{html_mod.escape(width)}"' if width else ""
                altattr = f' alt="{html_mod.escape(alt_)}"' if alt_ else ""
                # Preserve an author-set width style (the cross-renderer size lever) and append our
                # margin so the preview isn't flush against the cell edges.
                style = (style + ";" if style else "") + "margin:4px 0"
                return f'<img src="{html_mod.escape(src_)}"{wattr}{altattr} style="{html_mod.escape(style)}">'
            # No raw HTML → ordinary escaped+inline cell (the common case).
            if "<img" not in c and "<a id=" not in c and "<br" not in c:
                return render_inline(html_mod.escape(c))
            # Protect the few tags the module-doc cells use (img preview, row anchor, <br> line
            # breaks in a "card" cell), escape the rest, render markdown, then restore them.
            tokens = []
            def _stash(html: str) -> str:
                tokens.append(html)
                return f"\x00{len(tokens)-1}\x00"
            # Match the <img …> envelope, then extract attributes by name (order-independent).
            c = re.sub(r'<img\b[^>]*>', lambda m: _stash(_img(_img_attrs(m.group(0)))), c)
            c = re.sub(r'<a id="([a-z0-9-]+)"></a>', lambda m: _stash(f'<a id="{m.group(1)}"></a>'), c)
            c = re.sub(r'<br\s*/?>', lambda _: _stash("<br>"), c)
            out = render_inline(html_mod.escape(c))
            for i, html in enumerate(tokens):
                out = out.replace(f"\x00{i}\x00", html)
            return out

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
                    f"<{cell_tag}>{_render_cell(c)}</{cell_tag}>"
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

            stripped_check = raw_line.strip()

            # A standalone <img …> line (the per-module doc pages put the preview gif on its own
            # line above the description). Resolve a relative src to /api/doc-asset/ and keep the
            # width, like the table-cell path does — the allowlist below doesn't cover <img>.
            if re.fullmatch(r'<img\b[^>]*>(?:\s*<!--.*-->)?', stripped_check):
                # Extract each attribute by name, order-independent (an author may write src/alt/width
                # in any sequence — a fixed-order regex would silently drop the out-of-order ones).
                def _attr(name):
                    m = re.search(rf'\b{name}="([^"]*)"', stripped_check)
                    return m.group(1) if m else None
                src_ = _attr("src") or ""
                if not src_.startswith(("http://", "https://", "/")):
                    abs_src = (md_path.parent / src_).resolve()
                    try:
                        src_ = f"/api/doc-asset/{abs_src.relative_to(ROOT.resolve())}"
                    except ValueError:
                        pass
                w_ = _attr("width")
                # Escape every attribute value before it reaches the HTML (like _render_cell._img does)
                # so a doc-page src/width/alt with a quote or bracket can't break the tag or inject markup.
                wattr = f' width="{html_mod.escape(w_)}"' if w_ else ""
                alt_ = _attr("alt")
                aattr = f' alt="{html_mod.escape(alt_)}"' if alt_ is not None else ""
                lines.append(f'<img src="{html_mod.escape(src_)}"{wattr}{aattr} style="margin:4px 0">')
                continue

            # Pass-through for a fixed allowlist of structural HTML tags used
            # by history_report.py's combined graph+commits output. Narrowed
            # to known safe tags so arbitrary doc content can't inject scripts.
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
