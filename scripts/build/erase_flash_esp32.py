#!/usr/bin/env python3
"""Erase the entire ESP32 flash. Useful when on-device state (e.g. /.config persistence)
is wedged and needs a fresh start. Triggers `idf.py erase-flash` on the selected port."""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ESP32_DIR = ROOT / "esp32"

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_esp32 import find_idf, idf_env, idf_cmd

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="Serial port")
    args = parser.parse_args()

    if not ESP32_DIR.exists():
        print(f"ESP32 project directory not found: {ESP32_DIR}")
        sys.exit(1)

    idf_path = find_idf()
    if not idf_path:
        print("ESP-IDF not found. Install it or set IDF_PATH.")
        sys.exit(1)

    env = idf_env(idf_path)
    cmd = idf_cmd(idf_path)

    print(f"Erasing flash on {args.port}...")
    r = subprocess.run(cmd + ["erase-flash", "-p", args.port],
                       cwd=ESP32_DIR, env=env)
    sys.exit(r.returncode)

if __name__ == "__main__":
    main()
