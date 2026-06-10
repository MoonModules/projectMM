#!/usr/bin/env python3
"""Flash a built ESP32 firmware to a device.

Reads ``build/esp32-<firmware>/projectMM.bin``. The per-firmware build dir
(written by ``build_esp32.py``) makes "which firmware am I flashing" an
on-disk fact rather than an in-memory marker — switching firmwares is a
``--firmware`` change, not a clean-rebuild.

Prints the artifact size + age before flashing so a stale build (one
from yesterday vs an edit five minutes ago) is visible in the log.
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ESP32_DIR = ROOT / "esp32"

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_esp32 import find_idf, idf_env, idf_cmd, FIRMWARES, build_dir_for


def _fmt_age(seconds: float) -> str:
    """Compact human-readable age (5s, 12m, 3h, 2d)."""
    s = int(seconds)
    if s < 60:    return f"{s}s"
    if s < 3600:  return f"{s // 60}m"
    if s < 86400: return f"{s // 3600}h"
    return f"{s // 86400}d"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", required=True, help="Serial port")
    parser.add_argument("--firmware", required=True, choices=sorted(FIRMWARES),
                        help="Firmware variant to flash. The build for this "
                             "firmware must exist at build/esp32-<firmware>/ — "
                             "i.e. you must have run Build with the same "
                             "--firmware first.")
    parser.add_argument("--baud", type=int, default=460800,
                        help="esptool flash baud rate (default: 460800 — reliable on "
                             "every board). The web installer uses 921600 (~2x faster); "
                             "pass --baud 921600 to match it, but some USB bridges "
                             "(CP210x/CH340) drop to 'chip stopped responding' mid-flash "
                             "at that rate, so it's opt-in, not the default.")
    args = parser.parse_args()

    if not ESP32_DIR.exists():
        print(f"ESP32 project directory not found: {ESP32_DIR}")
        sys.exit(1)

    build_dir = build_dir_for(args.firmware)
    image = build_dir / "projectMM.bin"

    if not image.exists():
        print(f"ERROR: no build for {args.firmware!r} at "
              f"{build_dir.relative_to(ROOT)}/.")
        print(f"       Run Build with --firmware {args.firmware} first, then "
              f"Flash again.")
        sys.exit(2)

    size_kb = image.stat().st_size // 1024
    age = _fmt_age(time.time() - image.stat().st_mtime)
    print(f"==> flashing {args.firmware} build ({size_kb} KB, built {age} ago) "
          f"to {args.port}")

    idf_path = find_idf()
    if not idf_path:
        print("ESP-IDF not found. Install it or set IDF_PATH.")
        sys.exit(1)

    env = idf_env(idf_path)
    cmd = idf_cmd(idf_path)
    # -B + -DSDKCONFIG mirror build_esp32.py so idf.py flash reads the
    # per-firmware sdkconfig (the chip target lives in there). Without
    # -DSDKCONFIG, idf.py reads esp32/sdkconfig at the project root,
    # which may belong to a different firmware.
    b_arg = [
        "-B", str(build_dir),
        "-DSDKCONFIG=" + str(build_dir / "sdkconfig"),
    ]

    # -b sets the esptool flash baud (idf.py's own default is also 460800).
    # --baud 921600 matches the web installer for ~2x speed, but isn't the
    # default because some USB bridges can't sustain it (see --baud help).
    r = subprocess.run(cmd + b_arg + ["flash", "-p", args.port, "-b", str(args.baud)],
                       cwd=ESP32_DIR, env=env)
    if r.returncode == 0:
        _record_flash_event(args.port, args.firmware)
    sys.exit(r.returncode)


def _record_flash_event(port: str, firmware: str) -> None:
    """Drop a `scripts/.last_flash.json` breadcrumb so MoonDeck can link the
    just-flashed serial port to whichever device appears online next.
    MoonDeck's _probe_device consumes it on the next refresh and clears it.
    Stored as JSON in the same directory as moondeck.json so the entire
    "MoonDeck state" lives in one place."""
    import json, time
    marker = ROOT / "scripts" / ".last_flash.json"
    marker.write_text(json.dumps({
        "port": port,
        "firmware": firmware,
        "ts": time.time(),
    }))


if __name__ == "__main__":
    main()
