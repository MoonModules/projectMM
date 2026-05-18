#!/usr/bin/env python3
"""Run the desktop executable."""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXECUTABLE = ROOT / "build" / "mmv3"

def main():
    if not EXECUTABLE.exists():
        print(f"Executable not found: {EXECUTABLE}")
        print("Run build_desktop.py first.")
        sys.exit(1)

    print(f"Running {EXECUTABLE}...")
    r = subprocess.run([str(EXECUTABLE)], cwd=ROOT)
    sys.exit(r.returncode)

if __name__ == "__main__":
    main()
