#!/usr/bin/env python3
"""Run the desktop executable."""

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
EXECUTABLE = ROOT / "build" / "mmv3"

def main():
    if not EXECUTABLE.exists():
        print(f"Executable not found: {EXECUTABLE}")
        print("Run build_desktop.py first.")
        sys.exit(1)

    # Kill any already-running instance
    subprocess.run(["pkill", "-f", EXECUTABLE.name], capture_output=True)

    print(f"Running {EXECUTABLE}...")
    sys.stdout.flush()

    # Replace this process with mmv3 so MoonDeck can stream its output and kill it
    os.execv(str(EXECUTABLE), [str(EXECUTABLE)])

if __name__ == "__main__":
    main()
