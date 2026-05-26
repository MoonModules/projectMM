#!/usr/bin/env python3
"""Flash a built ESP32 firmware to a device.

Reads ``build/esp32-<board>/projectMM.bin``. The per-board build dir
(written by ``build_esp32.py``) makes "which board am I flashing" an
on-disk fact rather than an in-memory marker — switching boards is a
``--board`` change, not a clean-rebuild.

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
from build_esp32 import find_idf, idf_env, idf_cmd, BOARDS, build_dir_for


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
    parser.add_argument("--board", required=True, choices=sorted(BOARDS),
                        help="Firmware variant to flash. The build for this "
                             "board must exist at build/esp32-<board>/ — "
                             "i.e. you must have run Build with the same "
                             "--board first.")
    args = parser.parse_args()

    if not ESP32_DIR.exists():
        print(f"ESP32 project directory not found: {ESP32_DIR}")
        sys.exit(1)

    build_dir = build_dir_for(args.board)
    image = build_dir / "projectMM.bin"

    if not image.exists():
        print(f"ERROR: no build for {args.board!r} at "
              f"{build_dir.relative_to(ROOT)}/.")
        print(f"       Run Build with --board {args.board} first, then "
              f"Flash again.")
        sys.exit(2)

    size_kb = image.stat().st_size // 1024
    age = _fmt_age(time.time() - image.stat().st_mtime)
    print(f"==> flashing {args.board} build ({size_kb} KB, built {age} ago) "
          f"to {args.port}")

    idf_path = find_idf()
    if not idf_path:
        print("ESP-IDF not found. Install it or set IDF_PATH.")
        sys.exit(1)

    env = idf_env(idf_path)
    cmd = idf_cmd(idf_path)
    # -B + -DSDKCONFIG mirror build_esp32.py so idf.py flash reads the
    # per-board sdkconfig (the chip target lives in there). Without
    # -DSDKCONFIG, idf.py reads esp32/sdkconfig at the project root,
    # which may belong to a different board.
    b_arg = [
        "-B", str(build_dir),
        "-DSDKCONFIG=" + str(build_dir / "sdkconfig"),
    ]

    r = subprocess.run(cmd + b_arg + ["flash", "-p", args.port],
                       cwd=ESP32_DIR, env=env)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
