#!/usr/bin/env python3
"""Run desktop tests from the per-host build dir."""

import platform
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def _host_build_dir() -> Path:
    """Match build_desktop.py / run_desktop.py — one dir per host."""
    return ROOT / "build" / {
        "Darwin":  "macos",
        "Linux":   "linux",
        "Windows": "windows",
    }.get(platform.system(), platform.system().lower())


def main():
    build_dir = _host_build_dir()
    if not build_dir.exists():
        print(f"Build directory not found: {build_dir.relative_to(ROOT)}. "
              f"Run build_desktop.py first.")
        sys.exit(1)

    # MSVC multi-config drops mm_tests under test/Release/; everyone else
    # puts it at test/. Pick whichever exists.
    candidates = [
        build_dir / "test" / "mm_tests.exe",
        build_dir / "test" / "Release" / "mm_tests.exe",
        build_dir / "test" / "mm_tests",
    ]
    test_exe = next((c for c in candidates if c.exists()), None)
    if test_exe is None:
        print(f"Test executable not found under "
              f"{build_dir.relative_to(ROOT)}/test/.")
        print("Run build_desktop.py first.")
        sys.exit(1)

    print(f"Running tests from {build_dir.relative_to(ROOT)}/ ...")
    r = subprocess.run([str(test_exe), "-s"], cwd=build_dir)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
