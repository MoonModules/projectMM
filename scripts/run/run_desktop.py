#!/usr/bin/env python3
"""Run the desktop executable."""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
EXECUTABLE = ROOT / "build" / "mmv3"
if sys.platform == "win32":
    EXECUTABLE = EXECUTABLE.with_suffix(".exe")

def main():
    if not EXECUTABLE.exists():
        print(f"Executable not found: {EXECUTABLE}")
        print("Run build_desktop.py first.")
        sys.exit(1)

    # Kill any already-running instance
    if sys.platform == "win32":
        subprocess.run(["taskkill", "/F", "/IM", EXECUTABLE.name],
                       capture_output=True)
    else:
        subprocess.run(["pkill", "-f", EXECUTABLE.name], capture_output=True)

    print(f"Running {EXECUTABLE}...")
    sys.stdout.flush()

    # Replace this process so MoonDeck can stream its output and kill it
    if sys.platform == "win32":
        r = subprocess.run([str(EXECUTABLE)])
        sys.exit(r.returncode)
    else:
        os.execv(str(EXECUTABLE), [str(EXECUTABLE)])

if __name__ == "__main__":
    main()
