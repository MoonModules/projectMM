#!/usr/bin/env python3
"""Live ArtNet matrix test across every online board in moondeck.json.

Proves the ArtNet receive path end-to-end on real firmware. Devices come from
scripts/moondeck.json (the MoonDeck device list, active network, online only).
Each round one device is the SENDER and every other device LISTENS:

  1. The PC seeds the sender: builds OpDmx packets with a round-distinct solid
     colour and sends them to the sender's UDP 6454; an ArtNetReceiveEffect
     (added to each device's Layer for the duration of the run) writes them
     into the layer buffer. The sender's /ws preview stream must show the
     colour — proves PC → device receive.
  2. The sender's own ArtNetSendDriver is pointed at each listener in turn;
     the listener's preview must show the sender's CORRECTED colour (the send
     driver applies brightness + channel order) — proves device → device over
     real firmware send + receive.

With one online device only step 1 runs (the matrix needs ≥2 boards). All
mutated state (grid size, ArtNetSend ip, the added effects) is restored in a
finally block. Exit codes follow improv_smoke_test.py: 0 = all legs passed,
1 = a leg failed, 2 = environment problem (no devices, moondeck.json missing).

Run:  uv run scripts/scenario/run_artnet_live.py [--device NAME] [--host IP]
"""

import argparse
import json
import sys
import time
import socket
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_live_scenario import Client, _control_value  # shared HTTP wrapper  # noqa: E402
import _preview_ws  # noqa: E402

MOONDECK_STATE = ROOT / "scripts" / "moondeck.json"
ARTNET_PORT = 6454
CHANNELS_PER_UNIVERSE = 510

# Round colours: channel values far apart so they stay distinct after the
# sender's brightness scale (default 20/255 → e.g. (255,128,0) → (20,10,0)),
# and distinct per round so a stale frame from an earlier round can't pass.
ROUND_COLOURS = [(255, 128, 0), (0, 255, 128), (128, 0, 255),
                 (255, 0, 128), (128, 255, 0), (0, 128, 255)]

# Mirrors src/light/drivers/Correction.h (briLut scale + order[] reorder) — a
# listener sees the sender's corrected bytes, so the expected colour replicates
# that transform. 3-channel presets only; RGBW senders emit 4 bytes/light which
# misaligns a 3-channel listener buffer, so those legs are skipped. Keep in sync.
PRESET_ORDER = {"RGB": (0, 1, 2), "RBG": (0, 2, 1), "GRB": (1, 0, 2),
                "GBR": (1, 2, 0), "BRG": (2, 0, 1), "BGR": (2, 1, 0)}
PRESET_NAMES = ["RGB", "RBG", "GRB", "GBR", "BRG", "BGR", "RGBW", "GRBW"]


def corrected(rgb, brightness, preset):
    scaled = [(v * int(brightness)) // 255 for v in rgb]
    order = PRESET_ORDER[preset]
    return tuple(scaled[order[i]] for i in range(3))


# Mirrors src/light/ArtNetPacket.h::buildArtDmxPacket — cross-language
# duplication is unavoidable here; keep the two in sync.
def build_artdmx(universe: int, sequence: int, data: bytes) -> bytes:
    pkt = bytearray(b"Art-Net\0")
    pkt += bytes([0x00, 0x50])                        # OpDmx, little-endian
    pkt += bytes([0x00, 0x0E])                        # protocol 14, big-endian
    pkt += bytes([sequence & 0xFF, 0])                # sequence, physical
    pkt += bytes([universe & 0xFF, (universe >> 8) & 0xFF])   # universe LE
    pkt += bytes([(len(data) >> 8) & 0xFF, len(data) & 0xFF])  # length BE
    pkt += data
    return bytes(pkt)


def send_solid(host: str, rgb, universes: int = 2, repeats: int = 10,
               pace_ms: int = 50):
    """Send `repeats` full frames of `universes`×510-channel solid colour to the
    device. Repeats absorb WiFi power-save first-packet loss; the receiver's
    hold-last-frame staging means one arrival suffices."""
    ip = host.partition(":")[0]
    payload = bytes(rgb) * (CHANNELS_PER_UNIVERSE // 3)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        seq = 0
        for _ in range(repeats):
            for u in range(universes):
                sock.sendto(build_artdmx(u, seq, payload), (ip, ARTNET_PORT))
            seq = (seq + 1) & 0xFF
            time.sleep(pace_ms / 1000.0)
    finally:
        sock.close()


# --- device list -------------------------------------------------------------

def load_online_devices():
    """Devices of moondeck.json's active network, re-probed (a stale `online`
    flag must not hang the run on a 15 s mutating-call timeout)."""
    if not MOONDECK_STATE.exists():
        print(f"FAIL  {MOONDECK_STATE} not found — run MoonDeck once to discover devices")
        sys.exit(2)
    state = json.loads(MOONDECK_STATE.read_text())
    nets = state.get("networks", [])
    net = next((n for n in nets if n.get("name") == state.get("active_network")),
               nets[0] if nets else None)
    if not net:
        print("FAIL  moondeck.json has no networks")
        sys.exit(2)
    devices = []
    for dev in net.get("devices", []):
        if not dev.get("online"):
            continue
        try:  # quick reachability probe, far shorter than Client's mutate timeout
            with urllib.request.urlopen(f"http://{dev['ip']}/api/state", timeout=3):
                devices.append(dev)
        except OSError:
            print(f"  WARN  {dev.get('deviceName', dev['ip'])} marked online but unreachable — skipped")
    return devices


def find_module(state: dict, name: str):
    def walk(mod):
        if mod.get("name") == name:
            return mod
        for child in mod.get("children", []):
            found = walk(child)
            if found:
                return found
        return None
    for mod in state.get("modules", []):
        found = walk(mod)
        if found:
            return found
    return None


# --- per-device setup / restore ----------------------------------------------

class Board:
    """One device under test: HTTP client + the original state we mutate."""

    def __init__(self, dev: dict):
        self.name = dev.get("deviceName") or dev["ip"]
        self.host = dev["ip"]
        self.client = Client(self.host)
        state = self.client.get("/api/state")
        grid = find_module(state, "Grid") or {}
        drivers = find_module(state, "Drivers") or {}
        artnet = find_module(state, "ArtNetSend") or {}
        self.orig_w = _control_value(grid, "width")
        self.orig_h = _control_value(grid, "height")
        self.orig_ip = _control_value(artnet, "ip")
        # The user may have ArtNetSend disabled (e.g. while testing LED output);
        # relay legs need it on, so remember the original state to restore.
        self.artnet_enabled = bool(artnet.get("enabled", False))
        self.brightness = _control_value(drivers, "brightness") or 0
        preset_idx = _control_value(drivers, "lightPreset") or 0
        self.preset = PRESET_NAMES[int(preset_idx)] if int(preset_idx) < len(PRESET_NAMES) else "RGB"
        self.added_receiver = False
        self.ip_changed = False
        self.enable_changed = False

    def set_control(self, module: str, control: str, value):
        self.client.post("/api/control",
                         {"module": module, "control": control, "value": value})

    def setup(self):
        # Same grid everywhere: a solid frame must fill every listener's buffer
        # (16×16 → 256 lights → 2 universes, preview stride 1).
        self.set_control("Grid", "width", 16)
        self.set_control("Grid", "height", 16)
        self.client.post("/api/modules", {"type": "ArtNetReceiveEffect",
                                          "id": "ArtNetReceive", "parent_id": "Layer"})
        self.added_receiver = True

    def restore(self):
        if self.added_receiver:
            try:
                self.client.delete("/api/modules/ArtNetReceive")
            except Exception as e:
                print(f"  WARN  {self.name}: could not remove ArtNetReceive: {e}")
        if self.ip_changed and self.orig_ip is not None:
            try:
                self.set_control("ArtNetSend", "ip", self.orig_ip)
            except Exception as e:
                print(f"  WARN  {self.name}: could not restore ArtNetSend.ip: {e}")
        if self.enable_changed:
            try:
                self.set_control("ArtNetSend", "enabled", self.artnet_enabled)
            except Exception as e:
                print(f"  WARN  {self.name}: could not restore ArtNetSend.enabled: {e}")
        for key, val in (("width", self.orig_w), ("height", self.orig_h)):
            if val is not None:
                try:
                    self.set_control("Grid", key, val)
                except Exception as e:
                    print(f"  WARN  {self.name}: could not restore Grid.{key}: {e}")


# --- the matrix ----------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", help="only run rounds where this deviceName is the sender")
    ap.add_argument("--host", help="only run rounds where this host is the sender "
                                   "(MoonDeck forwards the selected device here)")
    ap.add_argument("--tolerance", type=int, default=0,
                    help="per-channel colour tolerance (default 0 — preview is byte-exact)")
    ap.add_argument("--timeout", type=float, default=10.0,
                    help="seconds to wait for a matching preview frame per leg")
    ap.add_argument("--packets", type=int, default=10, help="PC seed frame repeats")
    ap.add_argument("--pace-ms", type=int, default=50, help="pause between seed frames")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="seconds after a mutating call before asserting")
    args = ap.parse_args()

    devices = load_online_devices()
    if not devices:
        print("FAIL  no online devices in moondeck.json's active network")
        return 2
    print(f"devices: {len(devices)} online — "
          + ", ".join(f"{d.get('deviceName', '?')} ({d['ip']})" for d in devices), flush=True)
    if len(devices) == 1:
        print("note: only one device online — running the PC→device leg; "
              "the device↔device matrix needs ≥2 boards", flush=True)

    boards = [Board(d) for d in devices]
    passed, failed, skipped = 0, 0, 0
    try:
        for b in boards:
            b.setup()
        time.sleep(args.settle)  # one settle for the buildStates above

        for k, sender in enumerate(boards):
            if args.device and sender.name != args.device:
                continue
            if args.host and sender.host != args.host:
                continue
            colour = ROUND_COLOURS[k % len(ROUND_COLOURS)]
            print(f"== round {k + 1}/{len(boards)}: sender {sender.name}, "
                  f"colour {colour}", flush=True)

            # Leg 1 — PC seeds the sender; its preview shows the RAW colour
            # (preview streams the uncorrected buffer).
            send_solid(sender.host, colour, repeats=args.packets, pace_ms=args.pace_ms)
            ok, pct, pts, detail = _preview_ws.wait_for_solid(
                sender.host, colour, args.tolerance, 100.0, args.timeout)
            if ok:
                print(f"PASS  pc → {sender.name} (preview solid, {pts} points)", flush=True)
                passed += 1
            else:
                print(f"FAIL  pc → {sender.name} (best {pct:.0f}% of {pts} points"
                      f"{', ' + detail if detail else ''})"
                      " — desktop listeners: check the OS firewall allows UDP 6454", flush=True)
                failed += 1
                continue  # without a seeded sender the relay legs can't mean anything

            # Legs 2..N — sender relays to each listener via its own
            # ArtNetSendDriver; listeners see the sender's CORRECTED colour.
            for listener in boards:
                if listener is sender:
                    continue
                if self_skip := _relay_skip_reason(sender):
                    print(f"SKIP  {sender.name} → {listener.name} ({self_skip})", flush=True)
                    skipped += 1
                    continue
                expected = corrected(colour, sender.brightness, sender.preset)
                if not sender.artnet_enabled and not sender.enable_changed:
                    sender.set_control("ArtNetSend", "enabled", True)
                    sender.enable_changed = True
                sender.set_control("ArtNetSend", "ip", listener.host.partition(":")[0])
                sender.ip_changed = True
                ok, pct, pts, detail = _preview_ws.wait_for_solid(
                    listener.host, expected, args.tolerance, 100.0, args.timeout)
                if ok:
                    print(f"PASS  {sender.name} → {listener.name} "
                          f"(expected {expected} after sender correction, {pts} points)", flush=True)
                    passed += 1
                else:
                    print(f"FAIL  {sender.name} → {listener.name} (expected {expected}, "
                          f"best {pct:.0f}% of {pts} points"
                          f"{', ' + detail if detail else ''})", flush=True)
                    failed += 1
            if sender.ip_changed and sender.orig_ip is not None:
                sender.set_control("ArtNetSend", "ip", sender.orig_ip)
                sender.ip_changed = False
    finally:
        for b in boards:
            b.restore()

    print(f"SUMMARY: {passed} passed, {failed} failed, {skipped} skipped", flush=True)
    return 1 if failed else 0


def _relay_skip_reason(sender: "Board"):
    """A relay leg is meaningless when the sender's correction destroys the
    signal: RGBW presets emit 4 bytes/light (misaligns a 3-channel listener),
    and brightness 0 corrects every colour to black — black also matches a
    listener that received NOTHING (staging zero-fill), a guaranteed false pass."""
    if sender.preset not in PRESET_ORDER:
        return f"sender preset {sender.preset} is 4-channel — relay assert supports 3-channel presets"
    if all(c == 0 for c in corrected((255, 255, 255), sender.brightness, "RGB")):
        return "sender Drivers.brightness too low — corrected colour is black (raise brightness)"
    return None


if __name__ == "__main__":
    sys.exit(main())
