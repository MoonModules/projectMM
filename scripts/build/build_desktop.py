#!/usr/bin/env python3
"""Build the desktop target into a host-named build directory.

``build/macos/`` on macOS arm64, ``build/linux/`` on Linux, ``build/windows/``
on Windows — so a single machine could (in principle) build multiple host
flavours without one wiping the other, and so the layout matches the
ESP32 side (``build/esp32-<board>/``, one dir per target).
"""

import platform
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def host_build_dir() -> str:
    """Map platform.system() to the desktop build-dir name.

    Returns a slash-string (not Path) so the cmake -B argument stays
    portable across shells.
    """
    sys_name = platform.system()
    return {
        "Darwin":  "build/macos",
        "Linux":   "build/linux",
        "Windows": "build/windows",
    }.get(sys_name, f"build/{sys_name.lower()}")


def main():
    bdir = host_build_dir()
    print(f"Building desktop target into {bdir}/ ...")
    r = subprocess.run(
        ["cmake", "-B", bdir, "-DCMAKE_BUILD_TYPE=Release"],
        cwd=ROOT,
    )
    if r.returncode != 0:
        sys.exit(r.returncode)

    r = subprocess.run(
        ["cmake", "--build", bdir],
        cwd=ROOT,
    )
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
