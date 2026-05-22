#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""Build the ESP32 target with the Ethernet-only profile (WiFi compiled out).

Thin wrapper over build_esp32.py — MoonDeck's runner only forwards --env/--port,
so a fixed --profile is injected here. Standalone use: prefer
`build_esp32.py --profile eth-only` directly.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_esp32  # noqa: E402

if __name__ == "__main__":
    if "--profile" not in sys.argv:
        sys.argv += ["--profile", "eth-only"]
    build_esp32.main()
