#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial"]
# ///
"""Monitor the ESP32 serial output. Saves to esp32/monitor.log."""

import argparse
import serial
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
LOG_FILE = ROOT / "esp32" / "monitor.log"

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True, help="Serial port")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    args = parser.parse_args()

    print(f"Monitoring {args.port} at {args.baud} baud...")
    print(f"Log saved to {LOG_FILE}")
    print("Press Ctrl+C (or Stop in MoonDeck) to stop.\n")
    sys.stdout.flush()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"Cannot open {args.port}: {e}")
        sys.exit(1)

    with open(LOG_FILE, "w") as log:
        try:
            while True:
                line = ser.readline().decode("utf-8", errors="replace").rstrip("\r\n")
                if line:
                    print(line)
                    sys.stdout.flush()
                    log.write(line + "\n")
                    log.flush()
        except KeyboardInterrupt:
            pass
        finally:
            ser.close()
            print(f"\nStopped. Full log: {LOG_FILE}")

if __name__ == "__main__":
    main()
