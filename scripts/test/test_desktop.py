#!/usr/bin/env python3
"""Run desktop tests."""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

def main():
    print("Running tests...")
    r = subprocess.run(
        ["cmake", "--build", "build", "--target", "test"],
        cwd=ROOT,
    )
    sys.exit(r.returncode)

if __name__ == "__main__":
    main()
