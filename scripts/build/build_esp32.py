#!/usr/bin/env python3
"""Build the ESP32 target."""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ESP32_DIR = ROOT / "esp32"

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--env", default="esp32s3", help="ESP32 chip type")
    args = parser.parse_args()

    if not ESP32_DIR.exists():
        print(f"ESP32 project directory not found: {ESP32_DIR}")
        print("Create the esp32/ wrapper first.")
        sys.exit(1)

    print(f"Building for {args.env}...")
    r = subprocess.run(
        ["idf.py", "set-target", args.env],
        cwd=ESP32_DIR,
    )
    if r.returncode != 0:
        sys.exit(r.returncode)

    r = subprocess.run(
        ["idf.py", "build"],
        cwd=ESP32_DIR,
    )
    sys.exit(r.returncode)

if __name__ == "__main__":
    main()
