#!/usr/bin/env python3
"""Run desktop tests."""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

def main():
    build_dir = ROOT / "build"
    if not build_dir.exists():
        print("Build directory not found. Run build_desktop.py first.")
        sys.exit(1)

    test_exe = build_dir / "test" / "mm_tests"
    if not test_exe.exists():
        print(f"Test executable not found: {test_exe}")
        print("Run build_desktop.py first.")
        sys.exit(1)

    print("Running tests...")
    r = subprocess.run([str(test_exe), "-s"], cwd=build_dir)
    sys.exit(r.returncode)

if __name__ == "__main__":
    main()
