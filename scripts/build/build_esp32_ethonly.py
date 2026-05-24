#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""Build the ESP32 target for Ethernet only (WiFi compiled out).

Thin wrapper over build_esp32.py — MoonDeck's runner historically only
forwarded --env/--port, so a fixed --board is injected here. Kept for any
external scripting that already calls this filename. Standalone use: prefer
`build_esp32.py --board esp32-eth` directly.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_esp32  # noqa: E402

if __name__ == "__main__":
    if "--board" not in sys.argv and "--profile" not in sys.argv:
        sys.argv += ["--board", "esp32-eth"]
    build_esp32.main()
