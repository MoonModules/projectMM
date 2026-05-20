#!/usr/bin/env python3
"""Flash the ESP32 target."""

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

    print(f"Flashing to {args.port}...")
    r = subprocess.run(cmd + ["flash", "-p", args.port],
                       cwd=ESP32_DIR, env=env)
    sys.exit(r.returncode)

if __name__ == "__main__":
    main()
