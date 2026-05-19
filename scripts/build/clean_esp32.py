#!/usr/bin/env python3
"""Clean the ESP32 build directory."""

import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ESP32_DIR = ROOT / "esp32"

def main():
    build_dir = ESP32_DIR / "build"
    if build_dir.exists():
        shutil.rmtree(build_dir)
        print("ESP32 build directory removed.")
    else:
        print("Nothing to clean.")

    # Also remove sdkconfig so set-target runs fresh
    sdkconfig = ESP32_DIR / "sdkconfig"
    if sdkconfig.exists():
        sdkconfig.unlink()
        print("sdkconfig removed.")

if __name__ == "__main__":
    main()
