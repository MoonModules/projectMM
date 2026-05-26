#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""Non-destructive Improv health check over USB-serial.

Sends GET_DEVICE_INFO + GET_CURRENT_STATE Improv RPCs and prints what
the device reports. Useful when:

  - A web installer / ESP Web Tools dialog shows the minimal popup
    instead of the rich panel; this probe says whether Improv is
    actually answering on the wire.
  - You want to confirm post-flash that the Improv listener task is
    up before driving the longer improv_provision.py provisioning
    flow.
  - You've just changed firmware and want to verify the device name,
    chip family, or current state without re-flashing or opening
    the browser dialog.

No credentials are exchanged. The device's WiFi state isn't touched.

Usage:
  uv run scripts/build/improv_probe.py --port /dev/tty.usbserial-X

Exit codes: 0 = both RPCs answered, 1 = partial / no response,
2 = couldn't open the port.

Sibling of improv_provision.py — same framing helpers (imported), same
UART connection assumptions (ESP32-S3 DevKitC-1: silkscreen UART port,
not the native USB-Serial-JTAG port).
"""

import argparse
import sys
import time
from pathlib import Path

# Reuse the framing helpers from improv_provision.py living next to this script.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from improv_provision import (  # noqa: E402
    HEADER, VERSION,
    TYPE_RPC, TYPE_CURRENT_STATE, TYPE_ERROR_STATE, TYPE_RPC_RESPONSE,
    build_frame, parse_frame, describe_error,
)

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed. Run via `uv run` so the inline "
          "dependency block at the top of this file is honoured, or "
          "`pip install pyserial`.", file=sys.stderr)
    sys.exit(2)

# Improv RPC command IDs (host → device). Per the Improv-serial spec.
# These three are read-only; nothing here changes device state.
RPC_WIFI_SETTINGS     = 0x01
RPC_GET_CURRENT_STATE = 0x02
RPC_GET_DEVICE_INFO   = 0x03


def build_simple_rpc(cmd: int) -> bytes:
    """A no-payload RPC: [cmd][0=rpc_body_len]."""
    return bytes([cmd, 0])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", required=True,
                    help="Serial device path, e.g. /dev/tty.usbserial-X or COM3")
    ap.add_argument("--timeout", type=float, default=2.0,
                    help="Worst-case seconds to wait for a missing response. "
                         "Healthy devices respond in <20 ms each; this is the "
                         "ceiling for an unhealthy / disconnected port (default: 2)")
    ap.add_argument("--url-grace", type=float, default=0.25,
                    help="After STATE_PROVISIONED, wait this long for the "
                         "optional WIFI_SETTINGS URL follow-up before exiting "
                         "(default: 0.25)")
    args = ap.parse_args()

    print(f"==> probing {args.port}")
    try:
        ser = serial.Serial(args.port, baudrate=115200, timeout=0.1)
    except serial.SerialException as e:
        print(f"ERROR: could not open {args.port}: {e}", file=sys.stderr)
        return 2

    # Step 1: GET_DEVICE_INFO. The device responds with an RPC_RESPONSE frame
    # carrying 4 strings: firmware name, version, chip family, device name.
    frame = build_frame(TYPE_RPC, build_simple_rpc(RPC_GET_DEVICE_INFO))
    print(f"    → GET_DEVICE_INFO")
    ser.write(frame)
    ser.flush()

    # `deadline` is the worst-case ceiling — used while we're still waiting
    # for a known-pending response (device info, then current state). Once
    # we've heard everything we expect, we either exit immediately or wait
    # the `url-grace` for the optional URL follow-up. ESP-IDF answers each
    # RPC in <20 ms typically; the 2-s ceiling is for unhealthy ports.
    deadline = time.monotonic() + args.timeout
    got_device_info = False
    got_current_state = False
    state_was_provisioned = False
    sent_state = False
    saw_url = False

    while time.monotonic() < deadline:
        # Once we have both expected answers, tighten the deadline to the
        # url-grace window. Provisioned devices send the WIFI_SETTINGS URL
        # right after the state frame; unprovisioned devices don't, so the
        # short wait then exit cleanly.
        if got_device_info and got_current_state and not saw_url:
            grace_deadline = time.monotonic() + args.url_grace
            if grace_deadline < deadline:
                deadline = grace_deadline

        result = parse_frame(ser, deadline)
        if result is None:
            break
        msg_type, body = result

        if msg_type == TYPE_RPC_RESPONSE:
            if not body:
                continue
            cmd = body[0]
            if cmd == RPC_GET_DEVICE_INFO and not got_device_info:
                got_device_info = True
                # Walk the [len][bytes] string list.
                idx = 2
                strings = []
                while idx < len(body):
                    slen = body[idx]
                    idx += 1
                    strings.append(body[idx:idx + slen].decode("utf-8", errors="replace"))
                    idx += slen
                # Spec order: firmware, version, chip family, device name; the
                # trailing empty string is the protocol's end-of-list sentinel.
                labels = ["firmware", "version", "chip", "name"]
                for label, value in zip(labels, strings):
                    print(f"      {label}: {value!r}")
                # Step 2: GET_CURRENT_STATE. Some devices (ours included) also
                # follow up with a WIFI_SETTINGS RPC response carrying the
                # device URL when the state is `provisioned`.
                if not sent_state:
                    frame2 = build_frame(TYPE_RPC, build_simple_rpc(RPC_GET_CURRENT_STATE))
                    print(f"    → GET_CURRENT_STATE")
                    ser.write(frame2)
                    ser.flush()
                    sent_state = True
            elif cmd == RPC_WIFI_SETTINGS:
                # Optional URL follow-up — only sent by some firmwares.
                idx = 2
                urls = []
                while idx < len(body):
                    slen = body[idx]
                    idx += 1
                    urls.append(body[idx:idx + slen].decode("utf-8", errors="replace"))
                    idx += slen
                url = urls[0] if urls else "(empty)"
                print(f"      url: {url}")
                saw_url = True
        elif msg_type == TYPE_CURRENT_STATE:
            state = body[0] if body else 0xFF
            name = {0: "stopped", 1: "awaiting auth", 2: "authorized",
                    3: "provisioning", 4: "provisioned"}.get(state, f"0x{state:02x}")
            print(f"      state: {name}")
            got_current_state = True
            state_was_provisioned = (state == 4)
        elif msg_type == TYPE_ERROR_STATE:
            code = body[0] if body else 0xFF
            print(f"==> ERROR_STATE: {describe_error(code)}", file=sys.stderr)
            # Spec: ERROR_NONE (0x00) is an idle ack, not a real error.
            if code != 0x00:
                ser.close()
                return 1

    ser.close()
    if not got_device_info:
        print("==> no device-info response — Improv not answering on this port",
              file=sys.stderr)
        return 1
    if not got_current_state:
        print("==> device-info ok, but no current-state — partial response",
              file=sys.stderr)
        return 1
    if saw_url:
        print("==> Improv healthy (device info + state + URL follow-up)")
    elif not state_was_provisioned:
        print("==> Improv healthy (device info + state — no URL follow-up, "
              "expected for unprovisioned device)")
    else:
        # Provisioned but no URL frame within the grace window. The device
        # firmware likely predates the GET_CURRENT_STATE URL follow-up;
        # everything still works, just less self-describing.
        print("==> Improv healthy (device info + state; provisioned device "
              "didn't send the URL follow-up — older firmware build)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
