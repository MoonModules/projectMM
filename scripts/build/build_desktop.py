#!/usr/bin/env python3
"""Build the desktop target."""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

def main():
    print("Building desktop target...")
    r = subprocess.run(
        ["cmake", "-B", "build", "-DCMAKE_BUILD_TYPE=Release"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        sys.exit(r.returncode)

    r = subprocess.run(
        ["cmake", "--build", "build"],
        cwd=ROOT,
    )
    sys.exit(r.returncode)

if __name__ == "__main__":
    main()
