#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""Send WiFi credentials to a projectMM device over USB-serial using the
Improv-WiFi protocol — the same protocol the browser flow at
https://www.improv-wifi.com/ uses, just driven from Python.

The script:
  1. Opens the serial port at 115200-8N1.
  2. Sends an Improv WIFI_SETTINGS RPC frame with the SSID + password.
  3. Reads response frames until "provisioning success" (containing the
     device's new URL), "provisioning fail", or `--timeout` expires.
  4. Prints the device URL on success; non-zero exit on failure.

The device must be running a projectMM firmware that includes the Improv
listener (Track 3 of plan-18 onwards). On the ESP32-S3-DevKitC-1, connect
via the silkscreen-labelled UART USB port — the native USB-Serial-JTAG
port is unsupported by the Improv path.

This replaces v1's `deploy/wifi.py` + `deploy/flashfs.py --wifi` rack flow
(which baked credentials into a LittleFS partition and required halting +
re-flashing each device). Single-port mode today; a future
`--from-list <devicelist.json>` mode for racks is its own plan.

Improv protocol spec: https://www.improv-wifi.com/serial/
"""

import argparse
import sys
import time
from pathlib import Path

# Reuse the host-WiFi reader sitting next to this script. Adding the script
# directory to sys.path keeps host_wifi.py importable when this script is
# invoked from outside its directory (MoonDeck launches with cwd=ROOT).
sys.path.insert(0, str(Path(__file__).resolve().parent))
from host_wifi import get_host_wifi   # noqa: E402

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed. Run via `uv run` so the inline "
          "dependency block at the top of this file is honoured, or "
          "`pip install pyserial`.", file=sys.stderr)
    sys.exit(2)

# Improv-serial framing constants — copied from the wire spec, not from any
# host library, so the script stays standalone (no improv-wifi/sdk-cpp dep).
HEADER = b"IMPROV"
VERSION = 1
TYPE_CURRENT_STATE = 0x01
TYPE_ERROR_STATE = 0x02
TYPE_RPC = 0x03
TYPE_RPC_RESPONSE = 0x04
CMD_WIFI_SETTINGS = 0x01


def checksum(buf: bytes) -> int:
    """XOR-style sum-mod-256 checksum the Improv spec requires."""
    return sum(buf) & 0xFF


def build_frame(msg_type: int, payload: bytes) -> bytes:
    """Wrap a payload in the Improv framing (magic + version + type + len + checksum)."""
    body = HEADER + bytes([VERSION, msg_type, len(payload)]) + payload
    return body + bytes([checksum(body)])


def build_wifi_settings_payload(ssid: str, password: str) -> bytes:
    """RPC payload for WIFI_SETTINGS:
    [cmd][total_len][ssid_len][ssid_bytes][pw_len][pw_bytes].
    """
    s = ssid.encode("utf-8")
    p = password.encode("utf-8")
    rpc_body = bytes([len(s)]) + s + bytes([len(p)]) + p
    return bytes([CMD_WIFI_SETTINGS, len(rpc_body)]) + rpc_body


def parse_frame(ser: "serial.Serial", deadline: float) -> "tuple[int, bytes] | None":
    """Read one full Improv frame. Returns (type, payload) or None on timeout.

    Re-syncs on noise: any byte that breaks the magic restarts the search.
    """
    state = 0  # 0..5 = magic, 6 = version, 7 = type, 8 = length, 9 = payload, 10 = checksum
    header = bytearray()
    msg_type = 0
    expected_len = 0
    payload = bytearray()

    while time.monotonic() < deadline:
        try:
            b = ser.read(1)
        except serial.SerialException as e:
            # macOS USB CDC driver occasionally throws "Device not configured"
            # (Errno 6) mid-read when the device transitions WiFi modes (AP→STA,
            # WiFi power-saving wakes, etc.). The device itself is fine; the
            # host-side driver just stalled. Drain and retry without giving up
            # on the whole conversation. Real port loss (cable unplug) keeps
            # failing forever and the outer deadline catches that.
            if "Device not configured" in str(e) or "Errno 6" in str(e):
                time.sleep(0.1)
                try:
                    ser.reset_input_buffer()
                except Exception:
                    pass
                continue
            raise
        if not b:
            continue
        byte = b[0]

        if state < 6:
            if byte == HEADER[state]:
                header.append(byte)
                state += 1
            else:
                # Resync: if this byte happens to be the start of the magic, take it.
                header = bytearray()
                state = 0
                if byte == HEADER[0]:
                    header.append(byte)
                    state = 1
        elif state == 6:  # version
            if byte == VERSION:
                header.append(byte)
                state = 7
            else:
                state = 0
                header = bytearray()
        elif state == 7:  # type
            msg_type = byte
            header.append(byte)
            state = 8
        elif state == 8:  # length
            expected_len = byte
            header.append(byte)
            payload = bytearray()
            state = 9 if expected_len > 0 else 10
        elif state == 9:  # payload
            payload.append(byte)
            if len(payload) >= expected_len:
                state = 10
        elif state == 10:  # checksum
            if checksum(bytes(header) + bytes(payload)) == byte:
                return msg_type, bytes(payload)
            # Bad checksum — drop and resync. Don't return; keep looking.
            state = 0
            header = bytearray()

    return None


def describe_error(code: int) -> str:
    return {
        0x00: "no error",
        0x01: "invalid RPC",
        0x02: "unknown RPC",
        0x03: "unable to connect (bad credentials, no network, or timeout)",
        0x04: "not authorized",
        0xFF: "unknown",
    }.get(code, f"unknown code 0x{code:02x}")


def self_test() -> int:
    """Round-trip the framing + payload helpers without a serial cable.

    The script has no other test harness — the C++ parser at
    src/core/ImprovFrame.h + test/test_improv_frame.cpp covers the device
    side; this covers the host-CLI side. Re-runnable: `uv run
    scripts/build/improv_provision.py --self-test`.
    """
    # Frame builder produces magic + version + type + len + payload + checksum.
    frame = build_frame(TYPE_RPC, b"\x42")
    assert frame.startswith(HEADER), "frame must start with IMPROV"
    assert frame[6] == VERSION, "version byte"
    assert frame[7] == TYPE_RPC, "type byte"
    assert frame[8] == 1, "length byte"
    assert frame[9] == 0x42, "payload byte"
    assert frame[10] == checksum(frame[:10]), "checksum byte"

    # Zero-length payload.
    f0 = build_frame(TYPE_CURRENT_STATE, b"")
    assert len(f0) == 6 + 1 + 1 + 1 + 1, "zero-payload frame length"
    assert f0[8] == 0, "zero-length byte"

    # WIFI_SETTINGS payload shape.
    p = build_wifi_settings_payload("MyAP", "hunter2")
    assert p[0] == CMD_WIFI_SETTINGS
    assert p[1] == len(p) - 2, "total RPC body length encoded at byte 1"
    assert p[2] == 4 and p[3:7] == b"MyAP"
    assert p[7] == 7 and p[8:15] == b"hunter2"

    # Long-but-valid inputs (32-byte SSID + 63-byte password — the protocol's max).
    p2 = build_wifi_settings_payload("A" * 32, "B" * 63)
    assert len(p2) == 2 + 1 + 32 + 1 + 63

    # Error-code descriptions cover the codes the device sends.
    for code in (0x00, 0x01, 0x02, 0x03, 0x04, 0xFF):
        assert describe_error(code) != ""

    print("self-test: OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--self-test", action="store_true",
                    help="Run the framing + payload round-trip checks and exit. "
                         "No serial port needed; useful in CI / pre-commit.")
    ap.add_argument("--port",
                    help="Serial device path, e.g. /dev/tty.usbserial-X or COM3")
    ap.add_argument("--ssid",
                    help="WiFi SSID to provision (default: read from host's "
                         "currently-joined WiFi via host_wifi.py)")
    ap.add_argument("--password", default=None,
                    help="WiFi password (default: read from host's WiFi via "
                         "Keychain on macOS, NetworkManager on Linux, netsh "
                         "on Windows; empty for open networks)")
    ap.add_argument("--timeout", type=float, default=45.0,
                    help="Max seconds to wait for a final response (default: 45)")
    ap.add_argument("--board", default=None, metavar="NAME",
                    help="Board name from docs/install/boards.json (e.g. "
                         "'ESP32-S3 N16R8 Dev'). Resolves the board's TX-power cap "
                         "(controls.Network.txPowerSetting) automatically and "
                         "pushes the board name via SET_BOARD after "
                         "provisioning — the same injection the web installer "
                         "does. An explicit --tx-power overrides the lookup.")
    ap.add_argument("--tx-power", type=int, default=None, metavar="DBM",
                    help="Send the SET_TX_POWER vendor RPC (0..21 whole dBm) "
                         "BEFORE the credentials. Required for boards whose LDO "
                         "browns out at full TX power (weak-powered boards → 8, see "
                         "docs/install/boards.json) — without it the very first "
                         "association fails and the cap can never arrive over HTTP.")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    if not args.port:
        ap.error("--port is required (or pass --self-test)")

    # Autofill from host WiFi when SSID / password aren't given. Keeps the
    # MoonDeck button to one click — pick port, hit Improv. Host detection
    # is silent on failure (returns None); we surface a clear error here so
    # the user knows whether to retype or to type from scratch.
    if args.ssid is None or args.password is None:
        host_ssid, host_password = get_host_wifi()
        if args.ssid is None:
            if host_ssid is None:
                ap.error("--ssid not given and host WiFi not detected. "
                         "Pass --ssid explicitly, or join a network on this "
                         "machine first (run `python3 scripts/build/host_wifi.py` "
                         "to verify detection).")
            args.ssid = host_ssid
            print(f"==> using host SSID: {args.ssid}")
        if args.password is None:
            if host_password is None:
                # SSID came from host; password didn't. Most commonly: macOS
                # Keychain denied access, or Linux nmcli without sudo. The
                # user can still proceed (open network) or pass --password.
                print("==> warning: host WiFi password not readable "
                      "(Keychain denied / no sudo / open network?). "
                      "Sending empty password — pass --password to override.",
                      file=sys.stderr)
                args.password = ""
            else:
                args.password = host_password

    if len(args.ssid) > 32:
        print(f"ERROR: SSID too long ({len(args.ssid)} > 32 bytes)", file=sys.stderr)
        return 2
    if len(args.password) > 63:
        print(f"ERROR: password too long ({len(args.password)} > 63 bytes)", file=sys.stderr)
        return 2

    if args.board:
        # boards.json is the single source of truth for per-board injection —
        # same file the web installer and MoonDeck read.
        import json
        from pathlib import Path
        boards_file = Path(__file__).resolve().parents[2] / "docs" / "install" / "boards.json"
        try:
            catalog = json.loads(boards_file.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            print(f"ERROR: cannot read {boards_file}: {e}", file=sys.stderr)
            return 2
        entry = next((b for b in catalog if b.get("name") == args.board), None)
        if entry is None:
            names = ", ".join(b.get("name", "?") for b in catalog)
            print(f"ERROR: board {args.board!r} not in boards.json ({names})",
                  file=sys.stderr)
            return 2
        cap = entry.get("controls", {}).get("Network", {}).get("txPowerSetting")
        if args.tx_power is None and isinstance(cap, int):
            args.tx_power = cap
            print(f"==> board {args.board!r}: TX-power cap {cap} dBm from boards.json")

    try:
        ser = serial.Serial(args.port, baudrate=115200, timeout=0.1)
    except serial.SerialException as e:
        print(f"ERROR: could not open {args.port}: {e}", file=sys.stderr)
        return 2

    if args.tx_power is not None:
        if not 0 <= args.tx_power <= 21:
            print(f"ERROR: --tx-power {args.tx_power} out of range 0..21", file=sys.stderr)
            return 2
        # SET_TX_POWER vendor RPC (0xFD): [cmd][data_len=1][dBm]. Mirrors
        # SET_BOARD's framing; the firmware persists + applies it before the
        # association the credentials below will trigger. The 2.5 s pause lets
        # the module's 1 Hz consumer pick the cap up first.
        print(f"==> sending SET_TX_POWER {args.tx_power} dBm to {args.port}")
        ser.write(build_frame(TYPE_RPC, bytes([0xFD, 1, args.tx_power])))
        ser.flush()
        deadline = time.monotonic() + 5.0
        ack = parse_frame(ser, deadline)
        if not (ack and ack[0] == TYPE_RPC_RESPONSE):
            print("==> warning: no SET_TX_POWER ack (old firmware?) — continuing",
                  file=sys.stderr)
        time.sleep(2.5)

    print(f"==> sending WIFI_SETTINGS to {args.port} (SSID: {args.ssid!r})")
    payload = build_wifi_settings_payload(args.ssid, args.password)
    frame = build_frame(TYPE_RPC, payload)
    ser.write(frame)
    ser.flush()

    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        result = parse_frame(ser, deadline)
        if result is None:
            break

        msg_type, body = result
        if msg_type == TYPE_CURRENT_STATE:
            state = body[0] if body else 0xFF
            name = {0: "stopped", 1: "awaiting auth", 2: "authorized",
                    3: "provisioning", 4: "provisioned"}.get(state, f"0x{state:02x}")
            print(f"    state: {name}")
        elif msg_type == TYPE_ERROR_STATE:
            code = body[0] if body else 0xFF
            if code == 0x00:
                # ERROR_NONE acks the RPC; not a real error. Keep waiting.
                continue
            print(f"==> ERROR: {describe_error(code)}", file=sys.stderr)
            ser.close()
            return 1
        elif msg_type == TYPE_RPC_RESPONSE:
            # Response: [cmd][len][n strings (each [len][bytes])]
            if len(body) < 2 or body[0] != CMD_WIFI_SETTINGS:
                continue   # not the response we're waiting for (e.g. device info)
            # Walk the strings; the URL is the first one.
            idx = 2
            urls: list[str] = []
            while idx < len(body):
                slen = body[idx]
                idx += 1
                urls.append(body[idx:idx + slen].decode("utf-8", errors="replace"))
                idx += slen
            url = urls[0] if urls else "(no URL reported)"
            print(f"==> provisioned: {url}")
            if args.board:
                # SET_BOARD vendor RPC (0xFE): [cmd][1+len][len][name] —
                # the same post-provision push the web installer does, so
                # the device persists its physical-board identity.
                name = args.board.encode("utf-8")
                ser.write(build_frame(TYPE_RPC,
                                      bytes([0xFE, 1 + len(name), len(name)]) + name))
                ser.flush()
                time.sleep(0.5)   # let the device's serial task consume it
                print(f"==> pushed SET_BOARD {args.board!r}")
            ser.close()
            return 0

    print(f"==> TIMEOUT after {args.timeout:.0f}s — no final response from device", file=sys.stderr)
    ser.close()
    return 1


if __name__ == "__main__":
    sys.exit(main())
