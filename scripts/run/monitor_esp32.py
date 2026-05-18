#!/usr/bin/env python3
"""Monitor the ESP32 serial output."""

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ESP32_DIR = ROOT / "esp32"

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="Serial port")
    args = parser.parse_args()

    if not ESP32_DIR.exists():
        print(f"ESP32 project directory not found: {ESP32_DIR}")
        sys.exit(1)

    print(f"Monitoring {args.port}... (Ctrl+C to stop)")
    r = subprocess.run(
        ["idf.py", "monitor", "-p", args.port],
        cwd=ESP32_DIR,
    )
    sys.exit(r.returncode)

if __name__ == "__main__":
    main()
